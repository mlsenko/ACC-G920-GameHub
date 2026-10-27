#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    void *lib;
    SDL_Haptic *haptic;
    SDL_HapticEffect effect;
    int i, id, axes, buttons;
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    lib = dlopen("libSDL2-2.0.0.dylib", RTLD_NOLOAD);
    printf("dlsym SDL_Init %p SDL_HapticOpenFromJoystick %p SDL_PollEvent %p\n",
           dlsym(lib, "SDL_Init"), dlsym(lib, "SDL_HapticOpenFromJoystick"), dlsym(lib, "SDL_PollEvent"));
    printf("joysticks %d\n", SDL_NumJoysticks());
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *joy = SDL_JoystickOpen(i);
        axes = joy ? SDL_JoystickNumAxes(joy) : -1;
        buttons = joy ? SDL_JoystickNumButtons(joy) : -1;
        printf("  %d %s vendor %04x product %04x axes %d buttons %d haptic %d\n", i,
               SDL_JoystickNameForIndex(i), joy ? SDL_JoystickGetVendor(joy) : 0,
               joy ? SDL_JoystickGetProduct(joy) : 0, axes, buttons, joy ? SDL_JoystickIsHaptic(joy) : 0);
    }
    haptic = SDL_HapticOpen(0);
    if (!haptic) {
        printf("no haptic: %s\n", SDL_GetError());
        return 2;
    }
    printf("query 0x%x axes %d\n", SDL_HapticQuery(haptic), SDL_HapticNumAxes(haptic));
    memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_CONSTANT;
    effect.constant.direction.type = SDL_HAPTIC_SPHERICAL;
    effect.constant.direction.dir[0] = 27000;
    effect.constant.length = 700;
    effect.constant.level = 9000;
    id = SDL_HapticNewEffect(haptic, &effect);
    printf("effect %d\n", id);
    if (id < 0)
        return 3;
    SDL_HapticRunEffect(haptic, id, 1);
    usleep(700000);
    effect.constant.level = -9000;
    SDL_HapticUpdateEffect(haptic, id, &effect);
    SDL_HapticRunEffect(haptic, id, 1);
    usleep(700000);
    SDL_HapticStopEffect(haptic, id);
    SDL_HapticDestroyEffect(haptic, id);
    SDL_HapticClose(haptic);
    printf("done\n");
    return 0;
}
