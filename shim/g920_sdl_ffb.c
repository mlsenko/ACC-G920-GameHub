/* x86_64 SDL2 stand-in for GameHub's Wine.
 * Axes and buttons stay in the real SDL. Force feedback for the Logitech G920
 * (046d:c262) is translated to HID++ feature 0x8123, which is what this wheel
 * speaks. The classic 7-byte lg4ff reports used by G29/G923 PS do nothing here.
 *
 * Wine looks these SDL_Haptic symbols up with dlsym, so they have to live in
 * this image. Everything else is reexported from libSDL2-2.0.0.real.dylib.
 */

#include <SDL2/SDL.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>

#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VID_LOGITECH 0x046d
#define PID_G920 0xc262
#define HIDPP_FF 0x8123
#define MAX_EFFECTS 16

enum {
    FF_GET_INFO = 0,
    FF_RESET_ALL = 1,
    FF_DOWNLOAD = 2,
    FF_SET_STATE = 3,
    FF_DESTROY = 4,
    FF_SET_APERTURE = 6,
    FF_SET_GAIN = 8
};

enum { STATE_STOP = 1, STATE_PLAY = 2, STATE_PAUSE = 3 };

typedef struct {
    int used;
    int slot;
    int playing;
    int kind;
} Effect;

static pthread_mutex_t hid_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hid_cv = PTHREAD_COND_INITIALIZER;
static pthread_t hid_thread;
static int hid_thread_known;
static int hid_started;
static int hid_ready;
static int hid_failed;
static IOHIDDeviceRef wheel;
static uint8_t report_buf[64];
static uint8_t ff_index;
static int slot_count;
static int autocenter_slot;
static int paused;
static Effect effects[MAX_EFFECTS];
static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER;
static CFRunLoopRef hid_loop;
static int upd_dirty[MAX_EFFECTS];
static int upd_size[MAX_EFFECTS];
static uint8_t upd_params[MAX_EFFECTS][20];
static int sync_busy;
static int sync_done;
static int sync_op;
static int sync_slot;
static int sync_size;
static int sync_extra;
static int sync_result;
static uint8_t sync_params[20];

enum { OP_DOWNLOAD = 1, OP_STATE, OP_DESTROY, OP_GAIN, OP_RESET };

static void ffb_service_loop(void);

static uint8_t wait_feat;
static uint8_t wait_func;
static uint8_t resp[64];
static int resp_len;
static int resp_ready;
static int resp_error;

static void *real_sdl;

static void log_line(const char *fmt, ...) {
    static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
    FILE *f;
    va_list ap;
    pthread_mutex_lock(&log_mu);
    f = fopen("/tmp/g920-ffb.log", "a");
    if (f) {
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    pthread_mutex_unlock(&log_mu);
}

static void *load_real(void) {
    Dl_info info;
    char path[4096];
    const char *slash;
    if (real_sdl)
        return real_sdl;
    if (!dladdr((void *)&SDL_JoystickIsHaptic, &info) || !info.dli_fname) {
        log_line("dladdr failed");
        return NULL;
    }
    slash = strrchr(info.dli_fname, '/');
    if (!slash || (size_t)(slash - info.dli_fname) > sizeof(path) - 40)
        return NULL;
    snprintf(path, sizeof(path), "%.*s/libSDL2-2.0.0.real.dylib", (int)(slash - info.dli_fname), info.dli_fname);
    real_sdl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!real_sdl)
        log_line("dlopen %s failed: %s", path, dlerror());
    else
        log_line("real SDL %s", path);
    return real_sdl;
}

static void *real_sym(const char *name) {
    void *lib = load_real();
    void *sym = lib ? dlsym(lib, name) : NULL;
    if (!sym)
        log_line("missing real %s", name);
    return sym;
}

static void on_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex length) {
    uint8_t local[64];
    int n = 0;
    (void)ctx;
    (void)result;
    (void)sender;
    (void)type;
    if (length < 0)
        return;
    if (length > 0 && report && report[0] != (uint8_t)reportID && reportID <= 0xff && n < 64) {
        local[n++] = (uint8_t)reportID;
    }
    if (length > 64 - n)
        length = 64 - n;
    if (report && length > 0)
        memcpy(local + n, report, (size_t)length);
    n += (int)length;
    if (n < 4 || (local[0] != 0x10 && local[0] != 0x11 && local[0] != 0x12))
        return;
    pthread_mutex_lock(&hid_mu);
    if (local[3] == wait_func && (local[2] == wait_feat || local[2] == 0xff)) {
        memcpy(resp, local, (size_t)n);
        resp_len = n;
        resp_error = local[2] == 0xff;
        resp_ready = 1;
        pthread_cond_signal(&hid_cv);
    }
    pthread_mutex_unlock(&hid_mu);
}

static int hid_send(uint8_t feature, uint8_t function, const uint8_t *params, int param_count, uint8_t *out, int *out_len) {
    uint8_t pkt[64];
    uint8_t func = (uint8_t)((function << 4) | 0x01);
    int very = param_count > 16;
    int len = very ? 64 : 20;
    struct timespec ts;
    int rc = -1;
    IOReturn kr;
    if (!wheel)
        return -1;
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = very ? 0x12 : 0x11;
    pkt[1] = 0xff;
    pkt[2] = feature;
    pkt[3] = func;
    if (params && param_count > 0) {
        if (param_count > len - 4)
            param_count = len - 4;
        memcpy(pkt + 4, params, (size_t)param_count);
    }
    pthread_mutex_lock(&hid_mu);
    wait_feat = feature;
    wait_func = func;
    resp_ready = 0;
    resp_error = 0;
    pthread_mutex_unlock(&hid_mu);
    kr = IOHIDDeviceSetReport(wheel, kIOHIDReportTypeOutput, pkt[0], pkt, len);
    if (kr != kIOReturnSuccess) {
        log_line("setReport failed %d", (int)kr);
        return -1;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 400000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&hid_mu);
    while (!resp_ready) {
        if (hid_thread_known && pthread_equal(pthread_self(), hid_thread)) {
            struct timespec now;
            pthread_mutex_unlock(&hid_mu);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            pthread_mutex_lock(&hid_mu);
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                break;
        } else if (pthread_cond_timedwait(&hid_cv, &hid_mu, &ts) == ETIMEDOUT) {
            break;
        }
    }
    if (resp_ready && !resp_error) {
        if (out && out_len) {
            memcpy(out, resp, (size_t)resp_len);
            *out_len = resp_len;
        }
        rc = 0;
    } else if (resp_error) {
        log_line("hid++ error feat %02x func %02x code %02x", feature, func, resp_len > 5 ? resp[5] : 0);
    } else {
        log_line("hid++ timeout feat %02x func %02x", feature, func);
    }
    pthread_mutex_unlock(&hid_mu);
    return rc;
}

static void *hid_main(void *arg) {
    int vid = VID_LOGITECH, pid = PID_G920, page = 1, usage = 4;
    uint8_t ping[3] = {0x00, 0x00, 0x5a};
    CFNumberRef nvid, npid, npage, nusage;
    CFMutableDictionaryRef match;
    CFSetRef devs;
    const void *vals[4];
    IOReturn kr;
    uint8_t answer[64];
    int answer_len = 0;
    uint8_t lookup[2] = {0x81, 0x23};
    uint8_t gain[4] = {0xff, 0xff, 0x00, 0x00};
    uint8_t aperture[2] = {0x03, 0x84};
    (void)arg;
    hid_thread = pthread_self();
    hid_thread_known = 1;

    nvid = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    npid = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    npage = CFNumberCreate(NULL, kCFNumberIntType, &page);
    nusage = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), nvid);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), npid);
    CFDictionarySetValue(match, CFSTR(kIOHIDPrimaryUsagePageKey), npage);
    CFDictionarySetValue(match, CFSTR(kIOHIDPrimaryUsageKey), nusage);

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(manager, match);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    kr = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    devs = kr == kIOReturnSuccess ? IOHIDManagerCopyDevices(manager) : NULL;
    if (!devs || CFSetGetCount(devs) < 1) {
        log_line("G920 HID device not found");
        pthread_mutex_lock(&hid_mu);
        hid_failed = 1;
        hid_ready = 1;
        pthread_cond_broadcast(&hid_cv);
        pthread_mutex_unlock(&hid_mu);
        CFRunLoopRun();
        return NULL;
    }
    CFSetGetValues(devs, vals);
    wheel = (IOHIDDeviceRef)vals[0];
    CFRetain(wheel);
    kr = IOHIDDeviceOpen(wheel, kIOHIDOptionsTypeNone);
    if (kr != kIOReturnSuccess) {
        log_line("IOHIDDeviceOpen %d", (int)kr);
        wheel = NULL;
        pthread_mutex_lock(&hid_mu);
        hid_failed = 1;
        hid_ready = 1;
        pthread_cond_broadcast(&hid_cv);
        pthread_mutex_unlock(&hid_mu);
        CFRunLoopRun();
        return NULL;
    }
    IOHIDDeviceScheduleWithRunLoop(wheel, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(wheel, report_buf, sizeof(report_buf), on_report, NULL);
    hid_send(0x00, 0x01, ping, 3, NULL, NULL);

    if (hid_send(0x00, 0x00, lookup, 2, answer, &answer_len) != 0 || answer_len < 5 || answer[4] == 0) {
        log_line("feature 0x8123 missing");
        pthread_mutex_lock(&hid_mu);
        hid_failed = 1;
        hid_ready = 1;
        pthread_cond_broadcast(&hid_cv);
        pthread_mutex_unlock(&hid_mu);
        CFRunLoopRun();
        return NULL;
    }
    ff_index = answer[4];
    if (hid_send(ff_index, FF_GET_INFO, NULL, 0, answer, &answer_len) == 0 && answer_len >= 5)
        slot_count = answer[4];
    hid_send(ff_index, FF_RESET_ALL, NULL, 0, NULL, NULL);
    hid_send(ff_index, FF_SET_GAIN, gain, 4, NULL, NULL);
    hid_send(ff_index, FF_SET_APERTURE, aperture, 2, NULL, NULL);
    log_line("G920 ff index %u slots %d", ff_index, slot_count);

    hid_loop = CFRunLoopGetCurrent();
    pthread_mutex_lock(&hid_mu);
    hid_ready = 1;
    pthread_cond_broadcast(&hid_cv);
    pthread_mutex_unlock(&hid_mu);
    ffb_service_loop();
    return NULL;
}

static int ensure_hid(void) {
    pthread_mutex_lock(&hid_mu);
    if (!hid_started) {
        hid_started = 1;
        if (pthread_create(&hid_thread, NULL, hid_main, NULL) != 0) {
            hid_failed = 1;
            hid_ready = 1;
        }
    }
    while (!hid_ready)
        pthread_cond_wait(&hid_cv, &hid_mu);
    pthread_mutex_unlock(&hid_mu);
    return hid_failed || !wheel || !ff_index ? -1 : 0;
}

static int is_g920(SDL_Joystick *joy) {
    Uint16 vendor;
    Uint16 product;
    const char *name;
    SDL_JoystickGUID guid;
    if (!joy)
        return 0;
    vendor = SDL_JoystickGetVendor(joy);
    product = SDL_JoystickGetProduct(joy);
    name = SDL_JoystickName(joy);
    if (vendor == VID_LOGITECH && product == PID_G920)
        return 1;
    if (name && strstr(name, "G920"))
        return 1;
    guid = SDL_JoystickGetGUID(joy);
    if ((guid.data[4] | (guid.data[5] << 8)) == VID_LOGITECH && (guid.data[8] | (guid.data[9] << 8)) == PID_G920)
        return 1;
    if ((vendor == 0 || vendor == VID_LOGITECH) && SDL_JoystickNumAxes(joy) == 4 &&
        SDL_JoystickNumButtons(joy) >= 18 && SDL_JoystickNumHats(joy) >= 1) {
        log_line("matched joystick by shape vendor=%04x product=%04x name=%s", vendor, product, name ? name : "");
        return 1;
    }
    return 0;
}

static int tune_strength = 100;
static int tune_ceiling = 100;
static int tune_invert = 0;
static int tune_env_invert = 0;
static struct timespec tune_checked;
static time_t tune_mtime_sec;
static long tune_mtime_nsec;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Read by the menu-bar app. Checked a few times a second so a slider moves
 * the wheel on the next physics update without reopening the device. */
static void load_tune(void) {
    struct timespec now;
    struct stat st;
    char path[1024];
    const char *home;
    const char *env;
    FILE *f;
    int strength, ceiling, invert, changed;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (tune_checked.tv_sec != 0) {
        long ms = (now.tv_sec - tune_checked.tv_sec) * 1000L + (now.tv_nsec - tune_checked.tv_nsec) / 1000000L;
        if (ms >= 0 && ms < 200)
            return;
    }
    tune_checked = now;
    env = getenv("G920_FFB_INVERT");
    tune_env_invert = env && env[0] && env[0] != '0';
    home = getenv("HOME");
    if (!home)
        return;
    snprintf(path, sizeof(path), "%s/Library/Application Support/G920/ffb.conf", home);
    if (stat(path, &st) != 0) {
        tune_strength = 100;
        tune_ceiling = 100;
        tune_invert = 0;
        tune_mtime_sec = 0;
        tune_mtime_nsec = 0;
        return;
    }
    if (st.st_mtimespec.tv_sec == tune_mtime_sec && st.st_mtimespec.tv_nsec == tune_mtime_nsec)
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    strength = tune_strength;
    ceiling = tune_ceiling;
    invert = tune_invert;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "strength=%d", &v) == 1)
            strength = clamp_int(v, 40, 120);
        else if (sscanf(line, "ceiling=%d", &v) == 1)
            ceiling = clamp_int(v, 50, 100);
        else if (sscanf(line, "invert=%d", &v) == 1)
            invert = v != 0;
    }
    fclose(f);
    tune_mtime_sec = st.st_mtimespec.tv_sec;
    tune_mtime_nsec = st.st_mtimespec.tv_nsec;
    changed = strength != tune_strength || ceiling != tune_ceiling || invert != tune_invert;
    tune_strength = strength;
    tune_ceiling = ceiling;
    tune_invert = invert;
    if (changed)
        log_line("g920 tune strength %d ceiling %d invert %d", strength, ceiling, invert);
}

static int apply_tune(int force) {
    int cap;
    int mag;
    load_tune();
    if (tune_invert || tune_env_invert)
        force = -force;
    force = (int)((long)force * tune_strength / 100);
    cap = (int)((long)32767 * tune_ceiling / 100);
    mag = force < 0 ? -force : force;
    if (mag > cap)
        mag = cap;
    if (mag > 32767)
        mag = 32767;
    return force < 0 ? -mag : mag;
}

/* Wine turns a DirectInput polar direction into SDL spherical by adding 270°.
 * A constant force aimed due north then has no steering-axis component, while
 * ACC puts the signed torque in the level. When the axis projection is ~0,
 * the signed level itself is the wheel torque. */
static int steer_force(const SDL_HapticDirection *dir, int level) {
    double x = 1.0;
    double rad;
    if (!dir)
        return apply_tune(level);
    rad = (dir->dir[0] / 100.0) * (3.14159265358979323846 / 180.0);
    switch (dir->type) {
    case SDL_HAPTIC_CARTESIAN: {
        double dx = dir->dir[0];
        double dy = dir->dir[1];
        double len = hypot(dx, dy);
        if (len < 1.0)
            return apply_tune(level);
        x = dx / len;
        break;
    }
    case SDL_HAPTIC_POLAR:
        x = sin(rad);
        break;
    case SDL_HAPTIC_STEERING_AXIS:
        return apply_tune(level);
    case SDL_HAPTIC_SPHERICAL:
    default:
        x = cos(rad);
        break;
    }
    if (fabs(x) < 0.25)
        return apply_tune(level);
    return apply_tune((int)lround(level * x));
}

static void put_u16(uint8_t *p, unsigned v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

static int build_effect(const SDL_HapticEffect *effect, uint8_t *params, int *size) {
    unsigned length;
    unsigned delay;
    int force;
    memset(params, 0, 20);
    length = effect->constant.length;
    delay = effect->constant.delay;
    if (length == 0 || length == SDL_HAPTIC_INFINITY) {
        params[2] = params[3] = 0;
    } else {
        if (length > 65535)
            length = 65535;
        put_u16(params + 2, length);
    }
    put_u16(params + 4, delay > 65535 ? 65535 : delay);
    switch (effect->type) {
    case SDL_HAPTIC_CONSTANT:
        force = steer_force(&effect->constant.direction, effect->constant.level);
        params[1] = 0x00;
        put_u16(params + 6, (unsigned)(uint16_t)(int16_t)force);
        params[8] = (uint8_t)(effect->constant.attack_level >> 7);
        put_u16(params + 9, effect->constant.attack_length);
        params[11] = (uint8_t)(effect->constant.fade_level >> 7);
        put_u16(params + 12, effect->constant.fade_length);
        *size = 14;
        return 0;
    case SDL_HAPTIC_SINE:
    case SDL_HAPTIC_TRIANGLE:
    case SDL_HAPTIC_SAWTOOTHUP:
    case SDL_HAPTIC_SAWTOOTHDOWN:
    case 1u << 2: /* square, SDL no longer exports the constant */
        if (effect->type == SDL_HAPTIC_SINE)
            params[1] = 0x01;
        else if (effect->type == (1u << 2))
            params[1] = 0x02;
        else if (effect->type == SDL_HAPTIC_TRIANGLE)
            params[1] = 0x03;
        else if (effect->type == SDL_HAPTIC_SAWTOOTHUP)
            params[1] = 0x04;
        else
            params[1] = 0x05;
        force = steer_force(&effect->periodic.direction, effect->periodic.magnitude);
        put_u16(params + 6, (unsigned)(uint16_t)(int16_t)force);
        put_u16(params + 8, (unsigned)(uint16_t)effect->periodic.offset);
        put_u16(params + 10, effect->periodic.period);
        put_u16(params + 12, effect->periodic.phase);
        params[14] = (uint8_t)(effect->periodic.attack_level >> 7);
        put_u16(params + 15, effect->periodic.attack_length);
        params[17] = (uint8_t)(effect->periodic.fade_level >> 7);
        put_u16(params + 18, effect->periodic.fade_length);
        *size = 20;
        return 0;
    case SDL_HAPTIC_RAMP:
        params[1] = 0x0a;
        force = steer_force(&effect->ramp.direction, effect->ramp.start);
        put_u16(params + 6, (unsigned)(uint16_t)(int16_t)force);
        force = steer_force(&effect->ramp.direction, effect->ramp.end);
        put_u16(params + 8, (unsigned)(uint16_t)(int16_t)force);
        params[10] = (uint8_t)(effect->ramp.attack_level >> 7);
        put_u16(params + 11, effect->ramp.attack_length);
        params[13] = (uint8_t)(effect->ramp.fade_level >> 7);
        put_u16(params + 14, effect->ramp.fade_length);
        *size = 16;
        return 0;
    case SDL_HAPTIC_SPRING:
    case SDL_HAPTIC_DAMPER:
    case SDL_HAPTIC_FRICTION:
    case SDL_HAPTIC_INERTIA:
        if (effect->type == SDL_HAPTIC_SPRING)
            params[1] = 0x06;
        else if (effect->type == SDL_HAPTIC_DAMPER)
            params[1] = 0x07;
        else if (effect->type == SDL_HAPTIC_FRICTION)
            params[1] = 0x08;
        else
            params[1] = 0x09;
        params[6] = (uint8_t)(effect->condition.left_sat[0] >> 9);
        params[7] = (uint8_t)((effect->condition.left_sat[0] >> 1) & 255);
        put_u16(params + 8, (unsigned)(uint16_t)effect->condition.left_coeff[0]);
        params[10] = (uint8_t)(effect->condition.deadband[0] >> 9);
        params[11] = (uint8_t)((effect->condition.deadband[0] >> 1) & 255);
        put_u16(params + 12, (unsigned)(uint16_t)effect->condition.center[0]);
        put_u16(params + 14, (unsigned)(uint16_t)effect->condition.right_coeff[0]);
        params[16] = (uint8_t)(effect->condition.right_sat[0] >> 9);
        params[17] = (uint8_t)((effect->condition.right_sat[0] >> 1) & 255);
        *size = 18;
        return 0;
    default:
        return -1;
    }
}

static int alloc_effect(void) {
    int i;
    for (i = 0; i < MAX_EFFECTS; i++) {
        if (!effects[i].used) {
            memset(&effects[i], 0, sizeof(effects[i]));
            effects[i].used = 1;
            return i;
        }
    }
    return -1;
}

static int download(int slot, const uint8_t *params, int size, int autostart) {
    uint8_t buf[20];
    uint8_t answer[64];
    int answer_len = 0;
    memcpy(buf, params, (size_t)size);
    buf[0] = (uint8_t)slot;
    if (autostart)
        buf[1] |= 0x80;
    if (hid_send(ff_index, FF_DOWNLOAD, buf, size, answer, &answer_len) != 0 || answer_len < 5)
        return -1;
    return answer[4];
}

static int set_state(int slot, int state) {
    uint8_t params[2];
    params[0] = (uint8_t)slot;
    params[1] = (uint8_t)state;
    return hid_send(ff_index, FF_SET_STATE, params, 2, NULL, NULL);
}

static void wake_hid(void) {
    if (hid_loop)
        CFRunLoopWakeUp(hid_loop);
}

/* Runs on the HID thread. The game thread must not call hid_send. */
static void perform_sync(void) {
    int op, slot, size, extra, result;
    uint8_t params[20];
    pthread_mutex_lock(&q_mu);
    op = sync_op;
    slot = sync_slot;
    size = sync_size;
    extra = sync_extra;
    memcpy(params, sync_params, sizeof(params));
    pthread_mutex_unlock(&q_mu);
    result = -1;
    switch (op) {
    case OP_DOWNLOAD:
        result = download(slot, params, size, extra);
        break;
    case OP_STATE:
        result = set_state(slot, extra);
        break;
    case OP_DESTROY: {
        uint8_t slotb = (uint8_t)slot;
        result = hid_send(ff_index, FF_DESTROY, &slotb, 1, NULL, NULL);
        break;
    }
    case OP_GAIN:
        result = hid_send(ff_index, FF_SET_GAIN, params, size, NULL, NULL);
        break;
    case OP_RESET:
        result = hid_send(ff_index, FF_RESET_ALL, NULL, 0, NULL, NULL);
        break;
    default:
        break;
    }
    pthread_mutex_lock(&q_mu);
    sync_result = result;
    sync_done = 1;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mu);
}

static void ffb_service_loop(void) {
    log_line("ffb updates are asynchronous");
    for (;;) {
        int effect = -1;
        int size = 0;
        int slot;
        int do_sync;
        uint8_t params[20];
        pthread_mutex_lock(&q_mu);
        do_sync = sync_busy && !sync_done;
        if (!do_sync) {
            int i;
            for (i = 0; i < MAX_EFFECTS; i++) {
                if (!upd_dirty[i])
                    continue;
                effect = i;
                size = upd_size[i];
                memcpy(params, upd_params[i], (size_t)size);
                upd_dirty[i] = 0;
                break;
            }
        }
        pthread_mutex_unlock(&q_mu);
        if (do_sync) {
            perform_sync();
            continue;
        }
        if (effect >= 0) {
            slot = effects[effect].slot;
            if (slot > 0) {
                int got = download(slot, params, size, 0);
                if (got > 0)
                    effects[effect].slot = got;
            }
            continue;
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.004, true);
    }
}

static int call_hid(int op, int slot, const uint8_t *params, int size, int extra) {
    int result;
    pthread_mutex_lock(&q_mu);
    while (sync_busy)
        pthread_cond_wait(&q_cv, &q_mu);
    sync_op = op;
    sync_slot = slot;
    sync_size = size > 20 ? 20 : size;
    sync_extra = extra;
    memset(sync_params, 0, sizeof(sync_params));
    if (params && sync_size > 0)
        memcpy(sync_params, params, (size_t)sync_size);
    sync_result = -1;
    sync_done = 0;
    sync_busy = 1;
    pthread_mutex_unlock(&q_mu);
    wake_hid();
    pthread_mutex_lock(&q_mu);
    while (!sync_done)
        pthread_cond_wait(&q_cv, &q_mu);
    result = sync_result;
    sync_busy = 0;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mu);
    return result;
}

static void queue_update(int effect, const uint8_t *params, int size) {
    if (size > 20)
        size = 20;
    pthread_mutex_lock(&q_mu);
    memcpy(upd_params[effect], params, (size_t)size);
    upd_size[effect] = size;
    upd_dirty[effect] = 1;
    pthread_mutex_unlock(&q_mu);
    wake_hid();
}

static void upload_autocenter(int magnitude) {
    uint8_t params[20];
    int coeff;
    int slot;
    memset(params, 0, sizeof(params));
    params[1] = 0x06;
    coeff = magnitude >> 3;
    params[6] = (uint8_t)(magnitude >> 9);
    params[7] = (uint8_t)((magnitude >> 1) & 255);
    params[8] = (uint8_t)(coeff >> 8);
    params[9] = (uint8_t)(coeff & 255);
    params[14] = params[8];
    params[15] = params[9];
    params[16] = params[6];
    params[17] = params[7];
    slot = call_hid(OP_DOWNLOAD, autocenter_slot, params, 18, 1);
    if (slot > 0)
        autocenter_slot = slot;
}

static int our_haptic(SDL_Haptic *haptic) {
    return haptic == (SDL_Haptic *)&effects;
}

int SDL_JoystickIsHaptic(SDL_Joystick *joystick) {
    static int (*real_fn)(SDL_Joystick *) = NULL;
    if (is_g920(joystick)) {
        if (ensure_hid() == 0)
            return SDL_TRUE;
        log_line("G920 seen but HID++ init failed");
        return SDL_FALSE;
    }
    if (!real_fn)
        real_fn = real_sym("SDL_JoystickIsHaptic");
    return real_fn ? real_fn(joystick) : SDL_FALSE;
}

SDL_Haptic *SDL_HapticOpenFromJoystick(SDL_Joystick *joystick) {
    static SDL_Haptic *(*real_fn)(SDL_Joystick *) = NULL;
    if (is_g920(joystick)) {
        if (ensure_hid() != 0)
            return NULL;
        if (!autocenter_slot)
            upload_autocenter(0);
        log_line("haptic open %s", SDL_JoystickName(joystick) ? SDL_JoystickName(joystick) : "G920");
        return (SDL_Haptic *)&effects;
    }
    if (!real_fn)
        real_fn = real_sym("SDL_HapticOpenFromJoystick");
    return real_fn ? real_fn(joystick) : NULL;
}

SDL_Haptic *SDL_HapticOpen(int device_index) {
    SDL_Joystick *joy;
    SDL_Haptic *haptic;
    static SDL_Haptic *(*real_fn)(int) = NULL;
    joy = SDL_JoystickOpen(device_index);
    if (joy && is_g920(joy)) {
        haptic = SDL_HapticOpenFromJoystick(joy);
        return haptic;
    }
    if (joy)
        SDL_JoystickClose(joy);
    if (!real_fn)
        real_fn = real_sym("SDL_HapticOpen");
    return real_fn ? real_fn(device_index) : NULL;
}

void SDL_HapticClose(SDL_Haptic *haptic) {
    static void (*real_fn)(SDL_Haptic *) = NULL;
    int i;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticClose");
        if (real_fn)
            real_fn(haptic);
        return;
    }
    pthread_mutex_lock(&q_mu);
    memset(upd_dirty, 0, sizeof(upd_dirty));
    pthread_mutex_unlock(&q_mu);
    for (i = 0; i < MAX_EFFECTS; i++) {
        effects[i].used = 0;
        effects[i].slot = 0;
        effects[i].playing = 0;
    }
    autocenter_slot = 0;
    call_hid(OP_RESET, 0, NULL, 0, 0);
    log_line("haptic close, centering spring restored");
}

int SDL_HapticNumAxes(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return 1;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticNumAxes");
    return real_fn ? real_fn(haptic) : -1;
}

unsigned int SDL_HapticQuery(SDL_Haptic *haptic) {
    static unsigned int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic)) {
        return SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SINE | SDL_HAPTIC_TRIANGLE | SDL_HAPTIC_SAWTOOTHUP |
               SDL_HAPTIC_SAWTOOTHDOWN | SDL_HAPTIC_RAMP | SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER |
               SDL_HAPTIC_INERTIA | SDL_HAPTIC_FRICTION | SDL_HAPTIC_GAIN | SDL_HAPTIC_AUTOCENTER |
               SDL_HAPTIC_STATUS | SDL_HAPTIC_PAUSE;
    }
    if (!real_fn)
        real_fn = real_sym("SDL_HapticQuery");
    return real_fn ? real_fn(haptic) : 0;
}

int SDL_HapticNumEffects(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return MAX_EFFECTS;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticNumEffects");
    return real_fn ? real_fn(haptic) : -1;
}

int SDL_HapticNumEffectsPlaying(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return MAX_EFFECTS;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticNumEffectsPlaying");
    return real_fn ? real_fn(haptic) : -1;
}

int SDL_HapticEffectSupported(SDL_Haptic *haptic, SDL_HapticEffect *effect) {
    static int (*real_fn)(SDL_Haptic *, SDL_HapticEffect *) = NULL;
    if (our_haptic(haptic))
        return effect && (SDL_HapticQuery(haptic) & effect->type) ? SDL_TRUE : SDL_FALSE;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticEffectSupported");
    return real_fn ? real_fn(haptic, effect) : SDL_FALSE;
}

int SDL_HapticNewEffect(SDL_Haptic *haptic, SDL_HapticEffect *effect) {
    static int (*real_fn)(SDL_Haptic *, SDL_HapticEffect *) = NULL;
    uint8_t params[20];
    int size = 0;
    int id;
    int slot;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticNewEffect");
        return real_fn ? real_fn(haptic, effect) : -1;
    }
    if (!effect || build_effect(effect, params, &size) != 0)
        return -1;
    id = alloc_effect();
    if (id < 0)
        return -1;
    slot = call_hid(OP_DOWNLOAD, 0, params, size, 0);
    if (slot <= 0) {
        effects[id].used = 0;
        log_line("new effect type %u rejected", effect->type);
        return -1;
    }
    effects[id].slot = slot;
    effects[id].kind = effect->type;
    log_line("new effect id %d slot %d type %u level %d dir %u/%d", id, slot, effect->type,
             effect->type == SDL_HAPTIC_CONSTANT ? effect->constant.level : 0, effect->constant.direction.type,
             effect->constant.direction.dir[0]);
    return id;
}

int SDL_HapticUpdateEffect(SDL_Haptic *haptic, int effect, SDL_HapticEffect *data) {
    static int (*real_fn)(SDL_Haptic *, int, SDL_HapticEffect *) = NULL;
    uint8_t params[20];
    int size = 0;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticUpdateEffect");
        return real_fn ? real_fn(haptic, effect, data) : -1;
    }
    if (effect < 0 || effect >= MAX_EFFECTS || !effects[effect].used || effects[effect].slot <= 0 || !data)
        return -1;
    if (build_effect(data, params, &size) != 0)
        return -1;
    queue_update(effect, params, size);
    return 0;
}

int SDL_HapticRunEffect(SDL_Haptic *haptic, int effect, Uint32 iterations) {
    static int (*real_fn)(SDL_Haptic *, int, Uint32) = NULL;
    (void)iterations;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticRunEffect");
        return real_fn ? real_fn(haptic, effect, iterations) : -1;
    }
    if (effect < 0 || effect >= MAX_EFFECTS || !effects[effect].used || effects[effect].slot <= 0)
        return -1;
    if (paused)
        return 0;
    if (call_hid(OP_STATE, effects[effect].slot, NULL, 0, STATE_PLAY) != 0)
        return -1;
    effects[effect].playing = 1;
    return 0;
}

int SDL_HapticStopEffect(SDL_Haptic *haptic, int effect) {
    static int (*real_fn)(SDL_Haptic *, int) = NULL;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticStopEffect");
        return real_fn ? real_fn(haptic, effect) : -1;
    }
    if (effect < 0 || effect >= MAX_EFFECTS || !effects[effect].used || effects[effect].slot <= 0)
        return -1;
    effects[effect].playing = 0;
    return call_hid(OP_STATE, effects[effect].slot, NULL, 0, STATE_STOP);
}

void SDL_HapticDestroyEffect(SDL_Haptic *haptic, int effect) {
    static void (*real_fn)(SDL_Haptic *, int) = NULL;
    uint8_t slot;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticDestroyEffect");
        if (real_fn)
            real_fn(haptic, effect);
        return;
    }
    if (effect < 0 || effect >= MAX_EFFECTS || !effects[effect].used)
        return;
    pthread_mutex_lock(&q_mu);
    upd_dirty[effect] = 0;
    pthread_mutex_unlock(&q_mu);
    slot = (uint8_t)effects[effect].slot;
    effects[effect].used = 0;
    effects[effect].slot = 0;
    effects[effect].playing = 0;
    if (slot > 0 && slot != (uint8_t)autocenter_slot)
        call_hid(OP_DESTROY, slot, NULL, 0, 0);
}

int SDL_HapticGetEffectStatus(SDL_Haptic *haptic, int effect) {
    static int (*real_fn)(SDL_Haptic *, int) = NULL;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticGetEffectStatus");
        return real_fn ? real_fn(haptic, effect) : -1;
    }
    if (effect < 0 || effect >= MAX_EFFECTS || !effects[effect].used)
        return -1;
    return effects[effect].playing ? 1 : 0;
}

int SDL_HapticSetGain(SDL_Haptic *haptic, int gain) {
    static int (*real_fn)(SDL_Haptic *, int) = NULL;
    uint8_t params[4];
    unsigned scaled;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticSetGain");
        return real_fn ? real_fn(haptic, gain) : -1;
    }
    if (gain < 0)
        gain = 0;
    if (gain > 100)
        gain = 100;
    scaled = (unsigned)(gain * 65535 / 100);
    params[0] = (uint8_t)(scaled >> 8);
    params[1] = (uint8_t)(scaled & 255);
    params[2] = params[3] = 0;
    log_line("gain %d", gain);
    return call_hid(OP_GAIN, 0, params, 4, 0);
}

int SDL_HapticSetAutocenter(SDL_Haptic *haptic, int autocenter) {
    static int (*real_fn)(SDL_Haptic *, int) = NULL;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticSetAutocenter");
        return real_fn ? real_fn(haptic, autocenter) : -1;
    }
    if (autocenter < 0)
        autocenter = 0;
    if (autocenter > 100)
        autocenter = 100;
    upload_autocenter(autocenter * 65535 / 100);
    log_line("autocenter %d slot %d", autocenter, autocenter_slot);
    return autocenter_slot > 0 || autocenter == 0 ? 0 : -1;
}

int SDL_HapticStopAll(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    int i;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticStopAll");
        return real_fn ? real_fn(haptic) : -1;
    }
    for (i = 0; i < MAX_EFFECTS; i++) {
        if (effects[i].used && effects[i].slot > 0) {
            call_hid(OP_STATE, effects[i].slot, NULL, 0, STATE_STOP);
            effects[i].playing = 0;
        }
    }
    return 0;
}

int SDL_HapticPause(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    int i;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticPause");
        return real_fn ? real_fn(haptic) : -1;
    }
    paused = 1;
    for (i = 0; i < MAX_EFFECTS; i++) {
        if (effects[i].playing && effects[i].slot > 0)
            call_hid(OP_STATE, effects[i].slot, NULL, 0, STATE_PAUSE);
    }
    return 0;
}

int SDL_HapticUnpause(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    int i;
    if (!our_haptic(haptic)) {
        if (!real_fn)
            real_fn = real_sym("SDL_HapticUnpause");
        return real_fn ? real_fn(haptic) : -1;
    }
    paused = 0;
    for (i = 0; i < MAX_EFFECTS; i++) {
        if (effects[i].playing && effects[i].slot > 0)
            call_hid(OP_STATE, effects[i].slot, NULL, 0, STATE_PLAY);
    }
    return 0;
}

int SDL_HapticRumbleSupported(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return SDL_FALSE;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticRumbleSupported");
    return real_fn ? real_fn(haptic) : SDL_FALSE;
}

int SDL_HapticRumbleInit(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return -1;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticRumbleInit");
    return real_fn ? real_fn(haptic) : -1;
}

int SDL_HapticRumblePlay(SDL_Haptic *haptic, float strength, Uint32 length) {
    static int (*real_fn)(SDL_Haptic *, float, Uint32) = NULL;
    if (our_haptic(haptic))
        return -1;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticRumblePlay");
    return real_fn ? real_fn(haptic, strength, length) : -1;
}

int SDL_HapticRumbleStop(SDL_Haptic *haptic) {
    static int (*real_fn)(SDL_Haptic *) = NULL;
    if (our_haptic(haptic))
        return -1;
    if (!real_fn)
        real_fn = real_sym("SDL_HapticRumbleStop");
    return real_fn ? real_fn(haptic) : -1;
}

__attribute__((constructor)) static void shim_loaded(void) {
    log_line("g920 sdl ffb shim loaded");
    log_line("g920 tune build 1");
}

__attribute__((destructor)) static void shim_unload(void) {
    if (wheel && ff_index)
        hid_send(ff_index, FF_RESET_ALL, NULL, 0, NULL, NULL);
}
