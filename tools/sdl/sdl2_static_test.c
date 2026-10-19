/* sdl2_static_test.c — minimal SDL2 app that the GB-031 static-musl profile
 * links into a static, non-PIE ELF (build-sdl2-static-musl.sh).  Initialises
 * and tears down SDL so the static libSDL2.a is genuinely linked and the
 * artifact's reproducibility can be hashed; the HobbyOS fbcon video driver
 * and the loader-acceptance boot test are the remaining GB-031 half. */
#include <SDL.h>

int main(void) {
  if (SDL_Init(0) != 0)
    return 1;
  SDL_Quit();
  return 0;
}
