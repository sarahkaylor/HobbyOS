/*
 * wsurf.c - the window-surface render-confinement layer.
 *
 * Every pixel-mode app pixel draw goes through this layer; every primitive
 * clamps to the WM-assigned window content rect (graphics/wsurf.h), so an app
 * cannot paint outside its window by construction even though the framebuffer
 * is shared.  See render-pipeline.md (Design 1) for the enforcement envelope.
 *
 * Coordinates are window-local: (0,0) is the content rect's top-left, and a
 * draw is clamped to [0,cw) x [0,ch).  Damage is a screen-coordinates
 * bounding-box union of everything written since the last take; it lives in
 * file scope because the frozen v1 interface carries no state slot (static-
 * free re-entrancy is not required).  No dynamic allocation.
 */

#include "wsurf.h"
#include "font.h"

/* ---- damage bookkeeping (screen coords, half-open) ---- */
static int dmg_valid;
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;

static void dmg_union(int sx, int sy, int sw, int sh) {
  if (sw <= 0 || sh <= 0)
    return;
  if (!dmg_valid) {
    dmg_x0 = sx;
    dmg_y0 = sy;
    dmg_x1 = sx + sw;
    dmg_y1 = sy + sh;
    dmg_valid = 1;
  } else {
    if (sx < dmg_x0) dmg_x0 = sx;
    if (sy < dmg_y0) dmg_y0 = sy;
    if (sx + sw > dmg_x1) dmg_x1 = sx + sw;
    if (sy + sh > dmg_y1) dmg_y1 = sy + sh;
  }
}

/* Intersect the window-local rect [x,x+w) x [y,y+h) with the content rect
 * [0,cw) x [0,ch).  Returns 0 when the intersection is empty (or the draw is
 * non-positive); otherwise fills the clamped intersection in WINDOW-LOCAL
 * coords and returns 1.  int64 arithmetic keeps huge w/h draws overflow-safe. */
static int wsurf_clip(const struct wsurf *s, int x, int y, int w, int h,
                      int *ox, int *oy, int *ow, int *oh) {
  if (w <= 0 || h <= 0)
    return 0;
  long long x0 = x < 0 ? 0 : (long long)x;
  long long y0 = y < 0 ? 0 : (long long)y;
  long long x1 = (long long)x + w;
  long long y1 = (long long)y + h;
  if (x1 > s->cw) x1 = s->cw;
  if (y1 > s->ch) y1 = s->ch;
  if (x0 >= x1 || y0 >= y1)
    return 0;
  *ox = (int)x0;
  *oy = (int)y0;
  *ow = (int)(x1 - x0);
  *oh = (int)(y1 - y0);
  return 1;
}

void wsurf_init(struct wsurf *s, uint32_t *fb, int fb_w, int fb_h) {
  s->fb = fb;
  s->fb_w = fb_w;
  s->fb_h = fb_h;
  s->cx = 0;
  s->cy = 0;
  s->cw = 0;
  s->ch = 0;
  dmg_valid = 0;
}

int wsurf_set_rect(struct wsurf *s, int x, int y, int w, int h) {
  int nx = x, ny = y, nw = w, nh = h;
  if (nw < 0) nw = 0;
  if (nh < 0) nh = 0;
  if (nx < 0) nx = 0;
  if (ny < 0) ny = 0;
  if (nx > s->fb_w) nx = s->fb_w;
  if (ny > s->fb_h) ny = s->fb_h;
  if (nw > s->fb_w - nx) nw = s->fb_w - nx;
  if (nh > s->fb_h - ny) nh = s->fb_h - ny;
  if (nw < 0) nw = 0;
  if (nh < 0) nh = 0;
  int changed = nx != s->cx || ny != s->cy || nw != s->cw || nh != s->ch;
  s->cx = nx;
  s->cy = ny;
  s->cw = nw;
  s->ch = nh;
  return changed;
}

void wsurf_fill(struct wsurf *s, int x, int y, int w, int h, uint32_t color) {
  if (!s->fb || !s->cw || !s->ch)
    return;
  int gx, gy, nw, nh;
  if (!wsurf_clip(s, x, y, w, h, &gx, &gy, &nw, &nh))
    return;
  int sx0 = s->cx + gx, sy0 = s->cy + gy;
  dmg_union(sx0, sy0, nw, nh);
  for (int row = 0; row < nh; row++) {
    uint32_t *p = s->fb + (unsigned long)(sy0 + row) * s->fb_w + sx0;
    for (int col = 0; col < nw; col++)
      *p++ = color;
  }
}

void wsurf_frame(struct wsurf *s, int x, int y, int w, int h, uint32_t color) {
  if (w <= 0 || h <= 0)
    return;
  /* Four 1px runs; each is clamped independently, so a degenerate outline
   * stays confined to the content rect. */
  wsurf_fill(s, x, y, w, 1, color);
  wsurf_fill(s, x, y + h - 1, w, 1, color);
  wsurf_fill(s, x, y, 1, h, color);
  wsurf_fill(s, x + w - 1, y, 1, h, color);
}

void wsurf_put(struct wsurf *s, int x, int y, uint32_t color) {
  if (!s->fb || x < 0 || y < 0 || x >= s->cw || y >= s->ch)
    return;
  int gx = s->cx + x, gy = s->cy + y;
  dmg_union(gx, gy, 1, 1);
  s->fb[(unsigned long)gy * s->fb_w + gx] = color;
}

uint32_t wsurf_get(struct wsurf *s, int x, int y) {
  if (!s->fb || x < 0 || y < 0 || x >= s->cw || y >= s->ch)
    return 0;
  return s->fb[(unsigned long)(s->cy + y) * s->fb_w + (s->cx + x)];
}

void wsurf_blit(struct wsurf *s, int x, int y, int w, int h,
                const uint32_t *src, int src_stride) {
  if (!s->fb || !src || !s->cw || !s->ch)
    return;
  int gx, gy, nw, nh;
  if (!wsurf_clip(s, x, y, w, h, &gx, &gy, &nw, &nh))
    return;
  if (src_stride < w) src_stride = w; /* never read past the row */
  int lx0 = gx, ly0 = gy; /* window-local top-left of the clipped region */
  int sx0 = s->cx + gx, sy0 = s->cy + gy;
  dmg_union(sx0, sy0, nw, nh);
  for (int row = 0; row < nh; row++) {
    uint32_t *dst = s->fb + (unsigned long)(sy0 + row) * s->fb_w + sx0;
    const uint32_t *sp = src + (unsigned long)(ly0 + row - y) * src_stride + (lx0 - x);
    for (int col = 0; col < nw; col++)
      dst[col] = sp[col];
  }
}

void wsurf_blit_scaled(struct wsurf *s, int x, int y, int w, int h,
                       const uint32_t *src, int sw, int sh) {
  if (!s->fb || !src || !s->cw || !s->ch)
    return;
  if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
    return;
  int gx, gy, nw, nh;
  if (!wsurf_clip(s, x, y, w, h, &gx, &gy, &nw, &nh))
    return;
  int sx0 = s->cx + gx, sy0 = s->cy + gy;
  dmg_union(sx0, sy0, nw, nh);
  /* Mirror the fork driver's blitCachedFrame() exactly: each output pixel's
   * source index is the DEST-RELATIVE coordinate scaled (sx=(dx-x)*sw/w,
   * sy=(dy-y)*sh/h); the driver always blits from the content top-left, so
   * its dest-relative == window-local.  Identical rounding, identical clamps. */
  for (int row = 0; row < nh; row++) {
    int dy = gy + row - y; /* dest-relative row; >= 0 after clipping */
    int sy = dy * sh / h;
    if (sy >= sh) sy = sh - 1;
    uint32_t *dst = s->fb + (unsigned long)(sy0 + row) * s->fb_w + sx0;
    for (int col = 0; col < nw; col++) {
      int dx = gx + col - x; /* dest-relative col; >= 0 after clipping */
      int sx = dx * sw / w;
      if (sx >= sw) sx = sw - 1;
      dst[col] = src[(unsigned long)sy * sw + sx];
    }
  }
}

int wsurf_text8x8(struct wsurf *s, int x, int y, const char *text, int max_x,
                  uint32_t fg, uint32_t bg) {
  int pen = x;
  int row = y;
  while (*text) {
    if (*text == '\n') {
      row += 8;
      pen = x;
    } else {
      if (pen + 8 > max_x)
        break; /* no room for this glyph; stop the run */
      unsigned char c = (unsigned char)*text;
      if (c < 32 || c > 127) c = '?';
      const uint8_t *gp = font8x8[c - 32];
      for (int gy = 0; gy < 8; gy++) {
        uint8_t bits = gp[gy];
        for (int gx = 0; gx < 8; gx++)
          wsurf_put(s, pen + gx, row + gy,
                    (bits & (1 << (7 - gx))) ? fg : bg);
      }
      pen += 8;
    }
    text++;
  }
  return pen;
}

int wsurf_take_damage(struct wsurf *s, int out[4]) {
  if (!dmg_valid)
    return 0;
  int x0 = dmg_x0, y0 = dmg_y0, x1 = dmg_x1, y1 = dmg_y1;
  int cx1 = s->cx + s->cw, cy1 = s->cy + s->ch;
  if (x0 < s->cx) x0 = s->cx;
  if (y0 < s->cy) y0 = s->cy;
  if (x1 > cx1) x1 = cx1;
  if (y1 > cy1) y1 = cy1;
  dmg_valid = 0; /* always clear, even if the clamp emptied the box */
  if (x0 >= x1 || y0 >= y1)
    return 0;
  out[0] = x0;
  out[1] = y0;
  out[2] = x1 - x0;
  out[3] = y1 - y0;
  return 1;
}
