/*
 * gl_bench.c -- the M7 performance benchmark: the same reference scene rendered
 * through the SAME DSO, in three places, so "playable" becomes a number.
 *
 * The scene is shaped like RimWorld's main menu, which the host profile
 * measured at **110 draw calls/frame** (docs/rimworld-evidence.md §6): 110
 * textured, alpha-blended screen-space quads totalling ~2 screens of overdraw
 * (UI is fill-bound, and a menu is a stack of translucent panels), drawn one
 * DrawArrays each, cleared and glFinish()ed per frame.  It is deliberately
 * resolution-independent in coverage, so a smaller viewport costs
 * proportionally fewer pixels -- the same scaling the game has.
 *
 * Modes, per viewport size (640x480, 960x540, 1280x720, 1916x1018):
 *
 *   fbo      render the scene into an off-screen FBO of that size, glFinish
 *            per frame, NEVER present.  This is Task 2: rasterisation + the
 *            compat layer's CPU cost, with the swap/XPutImage path excluded.
 *   present  render into the same FBO, glFinish, then blit the FBO to the
 *            window's back buffer and glXSwapBuffers + XSync.  Difference
 *            against `fbo` is the present cost.
 *   direct   render straight into the window's default framebuffer and swap --
 *            exactly the path Unity uses -- at the drawable's real size.
 *   sprites  110 draws of 16x16-pixel quads: near-zero fill, so frame time is
 *            per-draw CPU cost (validated by the `sweep` cells).
 *   sweep    tiny quads at 1 / 110 / 500 draws at 1280x720, so the per-draw
 *            cost is a measured slope (us/draw), not a division.
 *
 * Why the sizes: 640x480/960x540/1280x720 are the plausible render targets;
 * 1916x1018 is the desktop's MAXIMISED content rectangle in the guest, which is
 * the only window size the WM will give a client (it ignores the requested
 * size).  So "present at 640x480" means "render 640x480 and present it into the
 * real 1916x1018 window" -- which is the only thing the game can do too.
 *
 * One source, two builds:
 *   guest: glibc 2.31 PIE, -lX11 against the compat shim, libGL.so.1 dlopen()ed
 *          from /LIB64 (see tools/gate/m7_perf.py for the exact link line).
 *   host:  ordinary gcc -lX11 -ldl.  With no LD_LIBRARY_PATH it loads the
 *          system Mesa (llvmpipe); with LD_LIBRARY_PATH=obj/mesa/lib it loads
 *          the *cross-built guest DSO* -- same bytes as the guest -- so the
 *          host/guest difference is the compat layer, and the
 *          softpipe/llvmpipe difference is the rasteriser.
 *
 * Timing: each frame is timed as
 *     draw ... glFinish   -> ms_raster
 *     [swap + XSync]      -> ms_present
 *     total               -> ms_total
 * so present cost is separable even inside the `present` cells.
 *
 * Rows are machine-readable, one per cell, on stderr (fd 2 is the serial
 * console for a desktop-launched compat program):
 *
 *   [PB] RESULT mode=present w=1280 h=720 draws=110 frames=60 fps_mean=..
 *        fps_median=.. fps_p90=.. ms_p50=.. ms_p90=.. ms_max=..
 *        ms_raster=.. ms_present=.. us_per_draw=..
 *
 * Exit 0 only if every cell produced frames and at least one readback proved
 * the scene actually rasterised something.
 */
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- reporting ---------------------------------------------------------- */

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

static int g_fail;

/* ---- time --------------------------------------------------------------- */

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

/* p in [0,1] over an already-sorted array */
static double pct_sorted(const double *v, int n, double p) {
  if (n <= 0) return 0.0;
  int i = (int)(p * (double)(n - 1) + 0.5);
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return v[i];
}

static uint64_t fnv1a64(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* ---- GL entry points, all through glXGetProcAddress --------------------- */

typedef void (*PFN_glClear)(GLbitfield);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_glFinish)(void);
typedef void (*PFN_glFlush)(void);
typedef GLenum (*PFN_glGetError)(void);
typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                          GLsizei, const void *);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *,
                                   const GLint *);
typedef void (*PFN_glCompileShader)(GLuint);
typedef void (*PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(GLuint, GLuint);
typedef void (*PFN_glLinkProgram)(GLuint);
typedef void (*PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void (*PFN_glUseProgram)(GLuint);
typedef GLint (*PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (*PFN_glUniform1i)(GLint, GLint);
typedef void (*PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void (*PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (*PFN_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_glActiveTexture)(GLenum);
typedef void (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei,
                                 GLint, GLenum, GLenum, const void *);
typedef void (*PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void (*PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum);
typedef void (*PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                      GLint, GLint, GLbitfield, GLenum);
typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                 GLenum, void *);
typedef void (*PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (*PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);

static struct {
  PFN_glClear Clear;
  PFN_glClearColor ClearColor;
  PFN_glViewport Viewport;
  PFN_glDrawArrays DrawArrays;
  PFN_glFinish Finish;
  PFN_glFlush Flush;
  PFN_glGetError GetError;
  PFN_glGetString GetString;
  PFN_glGetIntegerv GetIntegerv;
  PFN_glEnable Enable;
  PFN_glDisable Disable;
  PFN_glBlendFunc BlendFunc;
  PFN_glGenVertexArrays GenVertexArrays;
  PFN_glBindVertexArray BindVertexArray;
  PFN_glGenBuffers GenBuffers;
  PFN_glBindBuffer BindBuffer;
  PFN_glBufferData BufferData;
  PFN_glVertexAttribPointer VertexAttribPointer;
  PFN_glEnableVertexAttribArray EnableVertexAttribArray;
  PFN_glCreateShader CreateShader;
  PFN_glShaderSource ShaderSource;
  PFN_glCompileShader CompileShader;
  PFN_glGetShaderiv GetShaderiv;
  PFN_glGetShaderInfoLog GetShaderInfoLog;
  PFN_glCreateProgram CreateProgram;
  PFN_glAttachShader AttachShader;
  PFN_glLinkProgram LinkProgram;
  PFN_glGetProgramiv GetProgramiv;
  PFN_glGetProgramInfoLog GetProgramInfoLog;
  PFN_glUseProgram UseProgram;
  PFN_glGetUniformLocation GetUniformLocation;
  PFN_glUniform1i Uniform1i;
  PFN_glUniform2f Uniform2f;
  PFN_glGenTextures GenTextures;
  PFN_glBindTexture BindTexture;
  PFN_glActiveTexture ActiveTexture;
  PFN_glTexImage2D TexImage2D;
  PFN_glTexParameteri TexParameteri;
  PFN_glGenFramebuffers GenFramebuffers;
  PFN_glBindFramebuffer BindFramebuffer;
  PFN_glGenRenderbuffers GenRenderbuffers;
  PFN_glBindRenderbuffer BindRenderbuffer;
  PFN_glRenderbufferStorage RenderbufferStorage;
  PFN_glFramebufferRenderbuffer FramebufferRenderbuffer;
  PFN_glCheckFramebufferStatus CheckFramebufferStatus;
  PFN_glBlitFramebuffer BlitFramebuffer;
  PFN_glReadPixels ReadPixels;
  PFN_glDeleteFramebuffers DeleteFramebuffers;
  PFN_glDeleteRenderbuffers DeleteRenderbuffers;
} gl;

typedef __GLXextFuncPtr (*PFN_glXGetProcAddress)(const GLubyte *);
typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig,
                                                     GLXContext, Bool,
                                                     const int *);
typedef GLXFBConfig *(*PFN_glXChooseFBConfig)(Display *, int, const int *,
                                              int *);
typedef XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(Display *, GLXFBConfig);
typedef Bool (*PFN_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
typedef void (*PFN_glXSwapBuffers)(Display *, GLXDrawable);
typedef void (*PFN_glXDestroyContext)(Display *, GLXContext);

static PFN_glXGetProcAddress p_getproc;
static PFN_glXChooseFBConfig x_ChooseFBConfig;
static PFN_glXGetVisualFromFBConfig x_GetVisualFromFBConfig;
static PFN_glXMakeCurrent x_MakeCurrent;
static PFN_glXSwapBuffers x_SwapBuffers;
static PFN_glXDestroyContext x_DestroyContext;
static PFN_glXCreateContextAttribsARB p_create_ctx;

static void *xsym(void *h, const char *name) {
  void *p = dlsym(h, name);
  if (!p) say("[PB] FAIL missing libGL symbol %s\n", name);
  return p;
}

static void *gpa(const char *name) {
  void *p = (void *)p_getproc((const GLubyte *)name);
  if (!p) say("[PB] FAIL glXGetProcAddress(%s) is NULL\n", name);
  return p;
}

#define RESOLVE(field, name)                      \
  do {                                            \
    gl.field = (PFN_##name)gpa(#name);             \
    if (!gl.field && strcmp(#name, "glFlush") != 0) g_fail = 1; \
  } while (0)

static int resolve_gl(void) {
  RESOLVE(Clear, glClear);
  RESOLVE(ClearColor, glClearColor);
  RESOLVE(Viewport, glViewport);
  RESOLVE(DrawArrays, glDrawArrays);
  RESOLVE(Finish, glFinish);
  RESOLVE(Flush, glFlush);
  RESOLVE(GetError, glGetError);
  RESOLVE(GetString, glGetString);
  RESOLVE(GetIntegerv, glGetIntegerv);
  RESOLVE(Enable, glEnable);
  RESOLVE(Disable, glDisable);
  RESOLVE(BlendFunc, glBlendFunc);
  RESOLVE(GenVertexArrays, glGenVertexArrays);
  RESOLVE(BindVertexArray, glBindVertexArray);
  RESOLVE(GenBuffers, glGenBuffers);
  RESOLVE(BindBuffer, glBindBuffer);
  RESOLVE(BufferData, glBufferData);
  RESOLVE(VertexAttribPointer, glVertexAttribPointer);
  RESOLVE(EnableVertexAttribArray, glEnableVertexAttribArray);
  RESOLVE(CreateShader, glCreateShader);
  RESOLVE(ShaderSource, glShaderSource);
  RESOLVE(CompileShader, glCompileShader);
  RESOLVE(GetShaderiv, glGetShaderiv);
  RESOLVE(GetShaderInfoLog, glGetShaderInfoLog);
  RESOLVE(CreateProgram, glCreateProgram);
  RESOLVE(AttachShader, glAttachShader);
  RESOLVE(LinkProgram, glLinkProgram);
  RESOLVE(GetProgramiv, glGetProgramiv);
  RESOLVE(GetProgramInfoLog, glGetProgramInfoLog);
  RESOLVE(UseProgram, glUseProgram);
  RESOLVE(GetUniformLocation, glGetUniformLocation);
  RESOLVE(Uniform1i, glUniform1i);
  RESOLVE(Uniform2f, glUniform2f);
  RESOLVE(GenTextures, glGenTextures);
  RESOLVE(BindTexture, glBindTexture);
  RESOLVE(ActiveTexture, glActiveTexture);
  RESOLVE(TexImage2D, glTexImage2D);
  RESOLVE(TexParameteri, glTexParameteri);
  RESOLVE(GenFramebuffers, glGenFramebuffers);
  RESOLVE(BindFramebuffer, glBindFramebuffer);
  RESOLVE(GenRenderbuffers, glGenRenderbuffers);
  RESOLVE(BindRenderbuffer, glBindRenderbuffer);
  RESOLVE(RenderbufferStorage, glRenderbufferStorage);
  RESOLVE(FramebufferRenderbuffer, glFramebufferRenderbuffer);
  RESOLVE(CheckFramebufferStatus, glCheckFramebufferStatus);
  RESOLVE(BlitFramebuffer, glBlitFramebuffer);
  RESOLVE(ReadPixels, glReadPixels);
  RESOLVE(DeleteFramebuffers, glDeleteFramebuffers);
  RESOLVE(DeleteRenderbuffers, glDeleteRenderbuffers);
  return !g_fail;
}
#undef RESOLVE

/* ---- the reference scene ------------------------------------------------ */

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform vec2 u_off;\n"
    "uniform vec2 u_scale;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos * u_scale + u_off, 0.0, 1.0);\n"
    "}\n";

/* One texture fetch and one multiply: the cheapest realistic UI fragment,
 * because at 110 blended quads the cost is fill rate and state changes, not
 * shader ALU.  alpha 0.7 makes the overdraw additive-ish, like stacked panels. */
static const char *FS_SRC =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  vec4 t = texture(u_tex, v_uv);\n"
    "  o = vec4(t.rgb, t.a * 0.7);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src, const char *label) {
  GLuint s = gl.CreateShader(type);
  gl.ShaderSource(s, 1, (const GLchar *const *)&src, NULL);
  gl.CompileShader(s);
  GLint ok = 0;
  gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    GLsizei n = 0;
    log[0] = 0;
    gl.GetShaderInfoLog(s, sizeof log, &n, log);
    say("[PB] FAIL %s compile: %s\n", label, log);
    g_fail = 1;
    return 0;
  }
  return s;
}

/* Deterministic 32-bit LCG: the same quad layout on host and guest, so the two
 * runs render the same work (a random layout would make the comparison noisy). */
static uint32_t g_rng;
static double rnd(void) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return (double)((g_rng >> 8) & 0xFFFFFF) / (double)0x1000000;
}

#define TEX_N 256
static GLuint make_texture(void) {
  unsigned char *px = (unsigned char *)malloc(TEX_N * TEX_N * 4);
  if (!px) return 0;
  for (int y = 0; y < TEX_N; y++)
    for (int x = 0; x < TEX_N; x++) {
      unsigned char *p = px + ((size_t)y * TEX_N + x) * 4;
      int c = ((x >> 4) + (y >> 4)) & 1;
      p[0] = (unsigned char)(c ? 200 : 48);
      p[1] = (unsigned char)((x * 255) / (TEX_N - 1));
      p[2] = (unsigned char)((y * 255) / (TEX_N - 1));
      p[3] = (unsigned char)(c ? 255 : 160);
    }
  GLuint t = 0;
  gl.GenTextures(1, &t);
  gl.BindTexture(GL_TEXTURE_2D, t);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_N, TEX_N, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, px);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  free(px);
  return t;
}

/* A unit quad: two triangles, 6 vertices, position + uv. */
static GLuint g_vao, g_vbo, g_prog, g_tex;
static GLint g_u_off, g_u_scale, g_u_tex;

static int make_scene(void) {
  static const GLfloat verts[] = {
      /*  x     y     u     v  */
      -1.0f, -1.0f, 0.0f, 0.0f,
       1.0f, -1.0f, 1.0f, 0.0f,
       1.0f,  1.0f, 1.0f, 1.0f,
      -1.0f, -1.0f, 0.0f, 0.0f,
       1.0f,  1.0f, 1.0f, 1.0f,
      -1.0f,  1.0f, 0.0f, 1.0f,
  };
  gl.GenVertexArrays(1, &g_vao);
  gl.BindVertexArray(g_vao);
  gl.GenBuffers(1, &g_vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, g_vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
  gl.EnableVertexAttribArray(0);
  gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         (const void *)0);
  gl.EnableVertexAttribArray(1);
  gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         (const void *)(2 * sizeof(GLfloat)));

  GLuint vs = compile_shader(GL_VERTEX_SHADER, VS_SRC, "vertex");
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC, "fragment");
  if (!vs || !fs) return 0;
  g_prog = gl.CreateProgram();
  gl.AttachShader(g_prog, vs);
  gl.AttachShader(g_prog, fs);
  gl.LinkProgram(g_prog);
  GLint linked = 0;
  gl.GetProgramiv(g_prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[2048];
    GLsizei n = 0;
    log[0] = 0;
    gl.GetProgramInfoLog(g_prog, sizeof log, &n, log);
    say("[PB] FAIL program link: %s\n", log);
    return 0;
  }
  g_u_off = gl.GetUniformLocation(g_prog, "u_off");
  g_u_scale = gl.GetUniformLocation(g_prog, "u_scale");
  g_u_tex = gl.GetUniformLocation(g_prog, "u_tex");
  if (g_u_off < 0 || g_u_scale < 0 || g_u_tex < 0) {
    say("[PB] FAIL uniforms optimised away (off=%d scale=%d tex=%d)\n", g_u_off,
        g_u_scale, g_u_tex);
    return 0;
  }
  g_tex = make_texture();
  if (!g_tex) {
    say("[PB] FAIL texture allocation\n");
    return 0;
  }
  gl.UseProgram(g_prog);
  gl.Uniform1i(g_u_tex, 0);
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, g_tex);
  gl.Enable(GL_BLEND);
  gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl.Disable(GL_DEPTH_TEST);
  return 1;
}

/* Draw `draws` blended quads whose TOTAL area is `overdraw` screens.  Seed the
 * RNG identically every frame so the same pixels are touched each time. */
static void draw_scene(int draws, double overdraw) {
  g_rng = 0x5eed1234u;
  double side = 0.0;
  if (draws > 0 && overdraw > 0.0) {
    /* NDC area of one quad is (2*side)^2/4 = side^2, so draws*side^2 is the
     * total in screens; solve for `overdraw` screens. */
    side = __builtin_sqrt(overdraw / (double)draws);
  }
  for (int i = 0; i < draws; i++) {
    double sx = side, sy = side;
    double ox = (rnd() * 2.0 - 1.0) * (1.0 - sx);
    double oy = (rnd() * 2.0 - 1.0) * (1.0 - sy);
    gl.Uniform2f(g_u_scale, (GLfloat)sx, (GLfloat)sy);
    gl.Uniform2f(g_u_off, (GLfloat)ox, (GLfloat)oy);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
  }
}

/* Fill-free work: `draws` quads of 32x32 NDC units = 16x16 LOOKED-AT pixels at
 * w x h (NDC 2.0 == the whole width, so 32/w NDC == 16 px).  Per-draw CPU cost
 * with essentially no rasterisation, which is what the `sweep` slope measures. */
static void draw_tiny(int draws, int w, int h) {
  g_rng = 0x5eed1234u;
  double sx = 32.0 / (double)w, sy = 32.0 / (double)h;
  for (int i = 0; i < draws; i++) {
    gl.Uniform2f(g_u_scale, (GLfloat)sx, (GLfloat)sy);
    gl.Uniform2f(g_u_off, (GLfloat)((rnd() * 2.0 - 1.0) * (1.0 - sx)),
                 (GLfloat)((rnd() * 2.0 - 1.0) * (1.0 - sy)));
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
  }
}

/* ---- FBO ---------------------------------------------------------------- */

struct target {
  GLuint fbo, color, depth;
  int w, h;
};

static int target_create(struct target *t, int w, int h) {
  memset(t, 0, sizeof *t);
  t->w = w;
  t->h = h;
  gl.GenFramebuffers(1, &t->fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  gl.GenRenderbuffers(1, &t->color);
  gl.BindRenderbuffer(GL_RENDERBUFFER, t->color);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_RENDERBUFFER, t->color);
  gl.GenRenderbuffers(1, &t->depth);
  gl.BindRenderbuffer(GL_RENDERBUFFER, t->depth);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             GL_RENDERBUFFER, t->depth);
  GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    say("[PB] FAIL FBO %dx%d status=0x%x\n", w, h, st);
    return 0;
  }
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  return 1;
}

static void target_free(struct target *t) {
  if (t->fbo) gl.DeleteFramebuffers(1, &t->fbo);
  if (t->color) gl.DeleteRenderbuffers(1, &t->color);
  if (t->depth) gl.DeleteRenderbuffers(1, &t->depth);
  memset(t, 0, sizeof *t);
}

/* ---- one measurement cell ----------------------------------------------- */

/* Filter used by the FBO->window blit.  LINEAR is what a scaling present needs;
 * NEAREST is the 1:1 case a game uses for a same-size blit, and on softpipe the
 * two are not remotely the same cost -- measured on the host: 101-132 ms vs a
 * copy (see docs/rimworld-m7-perf.md §3).  PB_BLIT_LINEAR=0 selects NEAREST so
 * the filter's share can be separated from the copy's. */
static GLenum g_blit_filter = GL_LINEAR;

#define MAX_FRAMES 1000

struct cell {
  int mode_fbo;    /* render into an FBO (1) or the window (0) */
  int present;     /* swap after the draws */
  int tiny;        /* 16x16-pixel quads instead of the coverage quads */
  int w, h;
  int draws;
  const char *name;
};

/* Returns 1 if the cell produced usable numbers. */
static int run_cell(Display *dpy, Window win, const struct cell *c,
                    int win_w, int win_h, long budget_ms, int max_frames,
                    int min_frames) {
  struct target t;
  struct target *tp = NULL;
  if (c->mode_fbo) {
    if (!target_create(&t, c->w, c->h)) return 0;
    tp = &t;
  }
  if (tp) gl.BindFramebuffer(GL_FRAMEBUFFER, tp->fbo);
  else gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl.Viewport(0, 0, c->w, c->h);

  double overdraw = 2.0;
  if (c->tiny) {
    draw_tiny(c->draws, c->w, c->h);
    gl.Finish();
  } else {
    /* warm-up: 2 frames, so shader/pipeline state and the FBO's first-touch
     * allocation are not in the sample. */
    for (int i = 0; i < 2; i++) {
      gl.ClearColor(0.02f, 0.03f, 0.05f, 1.0f);
      gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      draw_scene(c->draws, overdraw);
      gl.Finish();
    }
  }

  static double ms_total[MAX_FRAMES], ms_raster[MAX_FRAMES];
  static double ms_blit[MAX_FRAMES], ms_present[MAX_FRAMES];
  static double fps[MAX_FRAMES];
  int n = 0;
  double t0 = now_s();
  while (n < max_frames &&
         (n < min_frames || (now_s() - t0) * 1000.0 < (double)budget_ms)) {
    double a = now_s();
    if (!c->tiny) {
      gl.ClearColor(0.02f, 0.03f, 0.05f, 1.0f);
      gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      draw_scene(c->draws, overdraw);
    } else {
      draw_tiny(c->draws, c->w, c->h);
    }
    /* Everything above (and the FBO->window blit below) is rasterisation and
     * is flushed before the swap, so the swap is measured on its own.  Keeping
     * the blit in its own bucket matters: on softpipe a 1:1 1916x1018 blit is
     * ~100 ms, and counting it as "present" would blame XPutImage for it. */
    gl.Finish();
    double b = now_s();
    if (c->present && tp) {
      gl.BindFramebuffer(GL_READ_FRAMEBUFFER, tp->fbo);
      gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      gl.BlitFramebuffer(0, 0, c->w, c->h, 0, 0, win_w, win_h,
                         GL_COLOR_BUFFER_BIT, g_blit_filter);
      gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl.Finish();
    }
    double bb = now_s();
    if (c->present) {
      x_SwapBuffers(dpy, win);
      /* XSync makes the number "the frame is on the desktop", not "the blit
       * was queued" -- the desktop services presents on its own thread, and a
       * game that outruns it does not get those frames. */
      XSync(dpy, False);
    }
    double cc = now_s();
    ms_raster[n] = (b - a) * 1000.0;
    ms_blit[n] = (bb - b) * 1000.0;
    ms_present[n] = (cc - bb) * 1000.0;
    ms_total[n] = (cc - a) * 1000.0;
    fps[n] = ms_total[n] > 0.0 ? 1000.0 / ms_total[n] : 0.0;
    n++;
  }

  if (n < 3) {
    say("[PB] FAIL cell %s %dx%d produced %d frames\n", c->name, c->w, c->h, n);
    if (tp) target_free(&t);
    return 0;
  }

  /* header copies for the percentiles (keep the fps array intact) */
  static double st[MAX_FRAMES];
  memcpy(st, ms_total, sizeof(double) * (size_t)n);
  qsort(st, (size_t)n, sizeof(double), cmp_d);
  double med = pct_sorted(st, n, 0.5);
  double p90 = pct_sorted(st, n, 0.9);
  double max = st[n - 1];

  double sum = 0, rsum = 0, bsum = 0, psum = 0;
  for (int i = 0; i < n; i++) {
    sum += ms_total[i];
    rsum += ms_raster[i];
    bsum += ms_blit[i];
    psum += ms_present[i];
  }
  double mean = sum / n;

  static double sf[MAX_FRAMES];
  memcpy(sf, fps, sizeof(double) * (size_t)n);
  qsort(sf, (size_t)n, sizeof(double), cmp_d);
  double fps_p90 = pct_sorted(sf, n, 0.9);

  say("[PB] RESULT mode=%s w=%d h=%d draws=%d frames=%d "
      "fps_mean=%.2f fps_median=%.2f fps_p90=%.2f "
      "ms_p50=%.2f ms_p90=%.2f ms_max=%.2f "
      "ms_raster=%.2f ms_blit=%.2f ms_present=%.2f us_per_draw=%.1f\n",
      c->name, c->w, c->h, c->draws, n, n > 0 ? 1000.0 / mean : 0.0,
      med > 0 ? 1000.0 / med : 0.0, fps_p90, med, p90, max, rsum / n,
      bsum / n, psum / n,
      c->draws > 0 ? (med * 1000.0) / (double)c->draws : 0.0);
  if (tp) target_free(&t);
  return 1;
}

/* One readback, to prove the scene rasterised something rather than being
 * optimised away: a checksum + a count of pixels that differ from the clear
 * colour.  Called once, at 640x480, right after that size's `fbo` cell. */
static void prove_pixels(int w, int h) {
  unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
  if (!px) return;
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  /* re-render into a fresh FBO of that size and read it back */
  struct target t;
  if (!target_create(&t, w, h)) {
    free(px);
    return;
  }
  gl.BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  gl.Viewport(0, 0, w, h);
  gl.ClearColor(0.02f, 0.03f, 0.05f, 1.0f);
  gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  draw_scene(110, 2.0);
  gl.Finish();
  gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gl.Finish();
  const unsigned char bg[4] = {px[0], px[1], px[2], px[3]};
  int nonbg = 0;
  for (int i = 0; i < w * h; i++)
    if (memcmp(px + (size_t)i * 4, bg, 4) != 0) nonbg++;
  say("[PB] CHECK scene %dx%d fnv1a64=0x%016llx nonbg=%d/%d (%.1f%%) "
      "glGetError=0x%x\n",
      w, h, (unsigned long long)fnv1a64(px, (size_t)w * h * 4), nonbg, w * h,
      nonbg * 100.0 / (double)(w * h), gl.GetError());
  if (nonbg * 100 / (w * h) < 20) {
    say("[PB] FAIL the reference scene covered only %d%% of the frame\n",
        nonbg * 100 / (w * h));
    g_fail = 1;
  }
  target_free(&t);
  free(px);
}

/* ---- main --------------------------------------------------------------- */

/* Hand-rolled decimal parse, not strtol/atoi: a glibc-2.39+ <stdlib.h>
 * redirects strtol to __isoc23_strtol (and defines atoi in terms of it), which
 * the pinned 2.31 libc does not export -- the same trap
 * tools/mesa/build-hobbyos.sh documents.  This keeps one source buildable
 * against both header sets. */
static long parse_long(const char *v, long dflt) {
  if (!v || !*v) return dflt;
  long sign = 1, x = 0;
  while (*v == ' ') v++;
  if (*v == '-') { sign = -1; v++; }
  else if (*v == '+') v++;
  if (*v < '0' || *v > '9') return dflt;
  while (*v >= '0' && *v <= '9') x = x * 10 + (*v++ - '0');
  return sign * x;
}

static long env_long(const char *k, long dflt) {
  return parse_long(getenv(k), dflt);
}

/* PB_WINDOW=1916x1018 requests a window size.  The guest WM maximises
 * regardless, so there this only affects the pre-map request; on the host it
 * makes the drawable match the guest's for a like-for-like comparison. */
static void env_window(int *w, int *h) {
  const char *v = getenv("PB_WINDOW");
  int vals[2] = {0, 0}, nv = 0;
  if (!v) return;
  const char *p = v;
  while (*p && nv < 2) {
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) break;
    int x = 0;
    while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
    vals[nv++] = x;
  }
  if (nv == 2 && vals[0] > 0 && vals[1] > 0) {
    *w = vals[0];
    *h = vals[1];
  }
}

int main(void) {
  static const int sizes[][2] = {{640, 480}, {960, 540}, {1280, 720}, {1916, 1018}};
  const int nsizes = (int)(sizeof sizes / sizeof sizes[0]);
  const int draws = (int)env_long("PB_DRAWS", 110);
  const long budget = env_long("PB_BUDGET_MS", 4000);
  const int max_frames = (int)env_long("PB_MAX_FRAMES", 240);
  /* 15 frames is the floor so p90 is not a single sample even when a cell runs
   * at 3 fps (12 frames would make ms_p90 the max). */
  const int min_frames = (int)env_long("PB_MIN_FRAMES", 15);

  say("[PB] bench start draws=%d budget_ms=%ld max_frames=%d min_frames=%d\n",
      draws, budget, max_frames, min_frames);
  if (parse_long(getenv("PB_BLIT_LINEAR"), 1) == 0) {
    g_blit_filter = GL_NEAREST;
    say("[PB] blit filter=nearest (PB_BLIT_LINEAR=0)\n");
  }

  /* libGL.so.1 is dlopen()ed, never linked: that is how SDL/Unity get it, and
   * it keeps this one source buildable for the guest and the host. */
  const char *lib = getenv("PB_LIBGL");
  if (!lib || !*lib) lib = "libGL.so.1";
  void *h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    say("[PB] FAIL dlopen(%s): %s\n", lib, dlerror());
    return 2;
  }
  p_getproc = (PFN_glXGetProcAddress)xsym(h, "glXGetProcAddressARB");
  if (!p_getproc) p_getproc = (PFN_glXGetProcAddress)xsym(h, "glXGetProcAddress");
  x_ChooseFBConfig = (PFN_glXChooseFBConfig)xsym(h, "glXChooseFBConfig");
  x_GetVisualFromFBConfig =
      (PFN_glXGetVisualFromFBConfig)xsym(h, "glXGetVisualFromFBConfig");
  x_MakeCurrent = (PFN_glXMakeCurrent)xsym(h, "glXMakeCurrent");
  x_SwapBuffers = (PFN_glXSwapBuffers)xsym(h, "glXSwapBuffers");
  x_DestroyContext = (PFN_glXDestroyContext)xsym(h, "glXDestroyContext");
  if (!p_getproc || !x_ChooseFBConfig || !x_GetVisualFromFBConfig ||
      !x_MakeCurrent || !x_SwapBuffers) {
    say("[PB] FAIL libGL is missing the GLX entry points\n");
    return 2;
  }
  p_create_ctx = (PFN_glXCreateContextAttribsARB)gpa("glXCreateContextAttribsARB");
  if (!p_create_ctx) return 2;

  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    say("[PB] FAIL XOpenDisplay(NULL): DISPLAY=%s\n",
        getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    return 3;
  }

  int attribs[] = {GLX_X_RENDERABLE, True,
                   GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                   GLX_RENDER_TYPE,   GLX_RGBA_BIT,
                   GLX_RED_SIZE,      8,
                   GLX_GREEN_SIZE,    8,
                   GLX_BLUE_SIZE,     8,
                   GLX_ALPHA_SIZE,    8,
                   GLX_DEPTH_SIZE,    24,
                   GLX_DOUBLEBUFFER,  True,
                   None};
  int ncfg = 0;
  GLXFBConfig *cfgs = x_ChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &ncfg);
  if (!cfgs || ncfg < 1) {
    say("[PB] FAIL glXChooseFBConfig: %d configs\n", ncfg);
    return 3;
  }
  GLXFBConfig fbc = cfgs[0];
  XVisualInfo *vi = x_GetVisualFromFBConfig(dpy, fbc);
  if (!vi) {
    say("[PB] FAIL glXGetVisualFromFBConfig\n");
    return 3;
  }

  Window root = RootWindow(dpy, vi->screen);
  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof swa);
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.border_pixel = 0;
  swa.event_mask = ExposureMask | StructureNotifyMask;
  int req_w = 320, req_h = 240;
  env_window(&req_w, &req_h);
  Window win = XCreateWindow(dpy, root, 0, 0, (unsigned)req_w, (unsigned)req_h,
                             0, vi->depth, InputOutput, vi->visual,
                             CWBorderPixel | CWColormap | CWEventMask, &swa);
  if (!win) {
    say("[PB] FAIL XCreateWindow\n");
    return 3;
  }
  XStoreName(dpy, win, "GLBENCH");
  XMapWindow(dpy, win);
  XSync(dpy, False);

  /* Rule 23: the shim's XPutImage refuses to blit a window the desktop has not
   * laid out, so wait for the content rect before drawing anything. */
  {
    double deadline = now_s() + 10.0;
    int got = 0;
    while (now_s() < deadline && !got) {
      while (XPending(dpy)) {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        XNextEvent(dpy, &ev);
        if (ev.type == Expose || ev.type == ConfigureNotify) got = 1;
      }
      if (!got) usleep(10000);
    }
    if (!got) {
      say("[PB] FAIL the desktop never reported the window's content rect\n");
      return 3;
    }
  }
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, win, &wa)) {
    say("[PB] FAIL XGetWindowAttributes\n");
    return 3;
  }
  const int WIN_W = wa.width, WIN_H = wa.height;
  say("[PB] window content rect %dx%d (requested %dx%d) at %d,%d\n", WIN_W,
      WIN_H, req_w, req_h, wa.x, wa.y);

  int ctx_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                       GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                       GLX_CONTEXT_PROFILE_MASK_ARB,
                       GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                       None};
  GLXContext ctx = p_create_ctx(dpy, fbc, NULL, True, ctx_attribs);
  if (!ctx) {
    say("[PB] FAIL glXCreateContextAttribsARB(3.3 core)\n");
    return 4;
  }
  if (!x_MakeCurrent(dpy, win, ctx)) {
    say("[PB] FAIL glXMakeCurrent\n");
    return 4;
  }
  if (!resolve_gl()) {
    say("[PB] FAIL resolving the GL 3.3 entry points\n");
    return 4;
  }
  say("[PB] renderer=%s\n", (const char *)gl.GetString(GL_RENDERER));
  say("[PB] version=%s\n", (const char *)gl.GetString(GL_VERSION));
  say("[PB] glsl=%s\n",
      (const char *)gl.GetString(GL_SHADING_LANGUAGE_VERSION));

  if (!make_scene()) {
    say("[PB] FAIL building the reference scene\n");
    return 5;
  }
  say("[PB] scene draws=%d overdraw=2.00 units=blended textured quads\n", draws);

  /* The two modes Task 1 and Task 2 ask for, at every size the game will use. */
  for (int i = 0; i < nsizes; i++) {
    struct cell f = {1, 0, 0, sizes[i][0], sizes[i][1], draws, "fbo"};
    if (!run_cell(dpy, win, &f, WIN_W, WIN_H, budget, max_frames, min_frames))
      g_fail = 1;
    if (sizes[i][0] == 640 && sizes[i][1] == 480) {
      /* prove the scene really rasterised something, once, cheaply */
      prove_pixels(640, 480);
    }
    struct cell p = {1, 1, 0, sizes[i][0], sizes[i][1], draws, "present"};
    if (!run_cell(dpy, win, &p, WIN_W, WIN_H, budget, max_frames, min_frames))
      g_fail = 1;
  }

  /* The game's own path: default framebuffer at the drawable size, then swap. */
  {
    struct cell d = {0, 1, 0, WIN_W, WIN_H, draws, "direct"};
    if (!run_cell(dpy, win, &d, WIN_W, WIN_H, budget, max_frames, min_frames))
      g_fail = 1;
  }

  /* Per-draw CPU cost: fill-free cells, then the same at 1/110/500 draws. */
  {
    struct cell s = {1, 0, 1, 1280, 720, draws, "sprites"};
    if (!run_cell(dpy, win, &s, WIN_W, WIN_H, budget, max_frames, min_frames))
      g_fail = 1;
  }
  {
    static const int counts[] = {1, 110, 500};
    for (int i = 0; i < 3; i++) {
      struct cell s = {1, 0, 1, 1280, 720, counts[i], "sweep"};
      char name[32];
      snprintf(name, sizeof name, "sweep%d", counts[i]);
      s.name = name;
      if (!run_cell(dpy, win, &s, WIN_W, WIN_H, budget, max_frames,
                    min_frames))
        g_fail = 1;
    }
  }

  gl.Finish();
  x_MakeCurrent(dpy, None, NULL);
  x_DestroyContext(dpy, ctx);
  say("[PB] DONE fail=%d\n", g_fail);
  return g_fail ? 1 : 0;
}
