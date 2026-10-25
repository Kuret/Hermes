// SPDX-License-Identifier: GPL-3.0-only
// Observe a controller added after SDL initialization under a session UID.
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 3) {
    return 2;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  if (SDL_Init(SDL_INIT_JOYSTICK) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  for (int index = 0; index < SDL_NumJoysticks(); ++index) {
    const char *name = SDL_JoystickNameForIndex(index);
    if (name && (strcmp(name, argv[1]) == 0 || strcmp(name, argv[2]) == 0)) {
      fprintf(stderr, "test controllers must be created after SDL initialization\n");
      SDL_Quit();
      return 1;
    }
  }
  printf("ready\n");
  fflush(stdout);
  const Uint32 started = SDL_GetTicks();
  int result = 1;
  while (SDL_GetTicks() - started < 15000) {
    SDL_bool own_attached = SDL_FALSE;
    SDL_JoystickUpdate();
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
      const char *name = SDL_JoystickNameForIndex(index);
      if (!name) {
        continue;
      }
      if (strcmp(name, argv[2]) == 0) {
        fprintf(stderr, "SDL enumerated the foreign seat controller\n");
        goto done;
      }
      if (strcmp(name, argv[1]) == 0) {
        SDL_Joystick *device = SDL_JoystickOpen(index);
        if (!device) {
          continue;
        }
        const SDL_bool attached = SDL_JoystickGetAttached(device);
        SDL_JoystickClose(device);
        if (attached) {
          own_attached = SDL_TRUE;
        }
      }
    }
    if (own_attached) {
      printf("PASS: controller discovered after SDL initialization\n");
      result = 0;
      goto done;
    }
    SDL_Delay(20);
  }
  fprintf(stderr, "SDL did not discover its readable hotplugged controller\n");
done:
  SDL_Quit();
  return result;
}
