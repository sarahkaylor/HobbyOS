#ifndef WSURF_H
#define WSURF_H

#include <stdint.h>

/* C-ABI window-surface render-confinement layer. */
#ifdef __cplusplus
extern "C" {
#endif

  /* ---- wsurf: the window-surface render sandbox (render-pipeline.md §Design 1)
   *
   * Every pixel-mode app pixel draw goes through this layer.  An app that owns
   * a pixel window (the WM's ESC ] X / ESC ] G protocol) must NOT raw-write the
   * shared framebuffer: all drawing goes through wsurf, and the layer clamps
   * every primitive to the WM-assigned window content rect, so the app cannot
   * paint outside its designated window area *by construction*.
   *
   * Drawing coordinates are window-local: (0,0) is the content rect's top-left
   * (which the WM reported in screen coordinates via ESC ] G; wsurf_set_rect
   * is how that geometry is bound to the surface).  Every draw is clamped to
   * [0,cw) x [0,ch); a draw fully outside is a no-op; partial draws clip.
   * Damage is tracked as a screen-coordinates bounding-box union for
   * rect-based presents (wsurf_take_damage).
   *
   * This is platform-layer (user-mode) confinement, exactly as requested --
   * not literal sandboxing.  Kernel/MMU-enforced mapping isolation is a
   * documented future option and out of scope here; the WM's existing
   * repair/expose machinery remains the recovery net.
   *
   * Pixel format is the framebuffer format: 0xRRGGBBAA stored as uint32_t
   * (r<<16|g<<8|b|0xFF<<24).  No dynamic allocation anywhere in this layer. */
  struct wsurf {
    uint32_t *fb;       /* mapped framebuffer base */
    int fb_w, fb_h;     /* screen size (stride == fb_w) */
    int cx, cy, cw, ch; /* content rect in SCREEN coords -- the whole sandbox */
  };

  /* Bind an fb and start with an empty (0,0,0,0) content rect. */
  void wsurf_init(struct wsurf *s, uint32_t *fb, int fb_w, int fb_h);

  /* Set the content rect in screen coordinates, clamped to the screen;
   * returns 1 if it changed. */
  int wsurf_set_rect(struct wsurf *s, int x, int y, int w, int h);

  /* Filled rectangle / 1px outline.  Window-local; clamped. */
  void wsurf_fill(struct wsurf *s, int x, int y, int w, int h, uint32_t color);
  void wsurf_frame(struct wsurf *s, int x, int y, int w, int h, uint32_t color);

  /* Single pixel.  Window-local; outside the content rect is a no-op. */
  void wsurf_put(struct wsurf *s, int x, int y, uint32_t color);

  /* Read a pixel; window-local; 0 outside the content rect. */
  uint32_t wsurf_get(struct wsurf *s, int x, int y);

  /* Copy a w x h block from `src` (row stride src_stride, uint32_t units)
   * to the window-local dest rect (x,y,w,h).  Clamped like everything else;
   * a source wider/taller than the dest rect is fine (only w x h pixels are
   * read). */
  void wsurf_blit(struct wsurf *s, int x, int y, int w, int h,
                  const uint32_t *src, int src_stride);

  /* Nearest-neighbour scale of a sw x sh source into the dest rect.
   * Pixel-for-pixel identical to the fork driver's blitCachedFrame():
   * for each dest pixel, sx = (dx-x)*sw/w and sy = (dy-y)*sh/h with the same
   * clamps, so it matches at 1:1 AND under scale. */
  void wsurf_blit_scaled(struct wsurf *s, int x, int y, int w, int h,
                         const uint32_t *src, int sw, int sh);

  /* 8x8-bitmap text: each glyph is drawn in a 8x8 bg-filled cell; the pen
   * advances 8 px per glyph and the run stops before a glyph would cross
   * max_x (a window-local x limit).  Returns the pen-x (window-local). */
  int wsurf_text8x8(struct wsurf *s, int x, int y, const char *text, int max_x,
                    uint32_t fg, uint32_t bg);

  /* Screen-coords bounding-box union of everything written since the last
   * take, clamped to the current content rect.  Returns 1 (and fills
   * out[4] = {x, y, w, h}) when nonempty; 0 when empty.  Clears. */
  int wsurf_take_damage(struct wsurf *s, int out[4]);

#ifdef __cplusplus
}
#endif

#endif
