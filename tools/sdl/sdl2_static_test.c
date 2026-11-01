/* sdl2_static_test.c — the GB-031 vehicle.  Links against the profile's
 * static libSDL2.a and actually exercises the video path: init, create a
 * window, get its surface, fill it and update it.  The first version only
 * called SDL_Init(0), so "dispatches and draws" could not be observed even on
 * a platform where SDL worked (GB-031-V clause 3).
 *
 * Linked as static-PIE (the v2 loader's contract), not static non-PIE. */
#include <stdio.h>
#include <SDL.h>

int main(void) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    printf("sdl2_static_test: init failed: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window *w = SDL_CreateWindow("gb031", 0, 0, 64, 48, 0);
  if (!w) {
    printf("sdl2_static_test: window failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 3;
  }
  SDL_Surface *s = SDL_GetWindowSurface(w);
  if (!s) {
    printf("sdl2_static_test: surface failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(w);
    SDL_Quit();
    return 4;
  }
  SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 200, 0));
  int rc = SDL_UpdateWindowSurface(w);
  printf("sdl2_static_test: driver=%s w=%d h=%d fill=rc%d\n",
         SDL_GetCurrentVideoDriver(), s->w, s->h, rc);
  SDL_DestroyWindow(w);
  SDL_Quit();
  return rc == 0 ? 0 : 5;
}
