/* src/user/cp/vkcube_math.h — the spinning-cube MVP shared by vkcube.c
 * (offscreen -> /dev/fb0) and vkcubex.c (X11 swapchain); docs/cp-vk.md.
 * The angle is a function of the frame index only, so runs are comparable. */
#ifndef CP_VKCUBE_MATH_H
#define CP_VKCUBE_MATH_H
#include <math.h>
#include <string.h>

/* ---- maths (column-major, Vulkan clip space: y down, z in [0,1]) ------------- */
static void mat_mul(float *o, const float *a, const float *b) {
  float t[16];
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      t[c * 4 + r] = s;
    }
  memcpy(o, t, sizeof(t));
}

static void cube_mvp(float *mvp, float aspect, int frame) {
  float a = (float)frame * (6.2831853f / 90.0f);
  float f = 1.0f / tanf(0.5f * 0.9f), n = 0.1f, fa = 50.0f;
  float P[16] = {f / aspect, 0, 0, 0,  0, -f, 0, 0,  0, 0, fa / (n - fa), -1,
                 0, 0, n * fa / (n - fa), 0};
  float V[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -5.0f, 1};
  float cy = cosf(a), sy = sinf(a), cx = cosf(a * 0.7f), sx = sinf(a * 0.7f);
  float RY[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
  float RX[16] = {1, 0, 0, 0, 0, cx, sx, 0, 0, -sx, cx, 0, 0, 0, 0, 1};
  float M[16];
  mat_mul(M, RY, RX);
  mat_mul(mvp, V, M);
  mat_mul(mvp, P, mvp);
}

#endif
