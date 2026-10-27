#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
    SDL_Haptic *haptic;
    SDL_HapticEffect effect;
    int id, i;
    double t0, t1;
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0)
        return 1;
    haptic = SDL_HapticOpen(0);
    if (!haptic) {
        printf("no haptic\n");
        return 2;
    }
    memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_SPHERICAL;
    effect.constant.direction.dir[0] = 0;
    effect.constant.length = SDL_HAPTIC_INFINITY;
    effect.constant.level = 0;
    id = SDL_HapticNewEffect(haptic, &effect);
    if (id < 0 || SDL_HapticRunEffect(haptic, id, SDL_HAPTIC_INFINITY) != 0) {
        printf("effect failed %d\n", id);
        return 3;
    }
    t0 = now_ms();
    for (i = 0; i < 3000; i++) {
        effect.constant.level = (Sint16)((i % 200) - 100);
        if (SDL_HapticUpdateEffect(haptic, id, &effect) != 0) {
            printf("update failed at %d\n", i);
            return 4;
        }
    }
    t1 = now_ms();
    printf("3000 updates in %.1f ms\n", t1 - t0);
    effect.constant.level = 8000;
    SDL_HapticUpdateEffect(haptic, id, &effect);
    usleep(500000);
    effect.constant.level = -8000;
    SDL_HapticUpdateEffect(haptic, id, &effect);
    usleep(500000);
    SDL_HapticStopEffect(haptic, id);
    SDL_HapticDestroyEffect(haptic, id);
    SDL_HapticClose(haptic);
    printf("done\n");
    return t1 - t0 > 200.0 ? 5 : 0;
}
