/*
 * gl_m3bench.c -- the M3 present benchmark: the M2 present path (full-surface
 * readback) against the M3 damage-rect path, on the same DSO, in one boot.
 *
 * Why this exists: M2 measured F = 8.95 ms/frame for a clear-and-swap and
 * showed it was the 7.8 MB TRANSFER_FROM_HOST_3D of a 1916x1018 surface, not
 * the submit.  M3 changes the present to read back only what changed, so the
 * two things that must be *measured* rather than asserted are
 *
 *   1. F for a scene whose damage is small (a loading screen: a static
 *      background plus a moving progress bar), with the damage path on and
 *      off, from identical frames; and
 *   2. that the damage path presents the *same pixels* as the full path --
 *      a faster path that drops a region is not a faster path, it is a bug.
 *
 * Scenes (one source, four builds -- M3_SCENE/M3_DMG baked in, the same trick
 * M2 used for its two drivers, because a desktop-menu launch cannot pass an
 * environment):
 *
 *   ui      110 blended quads, identical every frame (the menu's shape),
 *           plus a progress bar that advances and a spinner that moves.
 *           Only those two rects change between frames: the loading screen.
 *   frames  the same 110 quads re-scattered every frame: the whole surface
 *           changes, i.e. the worst case for a damage path.
 *
 * Every frame is timed as
 *     draw + glFinish   -> ms_raster
 *     glXSwapBuffers + XSync (the present) -> ms_present
 * and the damage path's own counters come from the winsys' [VGL] row.
 *
 * Verification (per run, at the end):
 *   * glReadPixels of the frame, crc64 -- two runs of the same scene with the
 *     same frame count must produce the same crc, which is what "the damage
 *     path presents the same pixels" means;
 *   * the same pixels against /dev/fb0 (the surface the desktop scans out),
 *     with M2's mouse-pointer budget: a stale band that the damage path never
 *     re-read would show up here and nowhere else;
 *   * the shim's own [XSW] crc row is the second, independent receipt.
 *
 * Rows are machine-readable on stderr, one per fact:
 *   [M3B] start scene=ui damage=1
 *   [M3B] GL_RENDERER: virgl
 *   [M3B] window 1916x1018 at 0,0 (requested 1916x1018)
 *   [M3B] run frames=120 fps=18.42 ms_mean=54.29 ms_p50=.. ms_p90=..
 *         ms_raster=.. ms_present=..
 *   [M3B] verify glReadPixels: crc=0123456789abcdef nz=..
 *   [M3B] verify /dev/fb0: rect 1916x1018 at 0,0 diff=96/1950488
 *   [M3B] done rc=0
 */
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef float GLfloat;
typedef unsigned char GLubyte;

#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLE_STRIP 0x0005
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#define GLX_USE_GL 1
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_X_RENDERABLE 0x8012
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_WINDOW_BIT 0x00000001
#define GLX_RENDER_TYPE 0x8011
#define GLX_RGBA_BIT 0x00000001
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef XID GLXDrawable;
typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;

#ifndef M3_SCENE
#define M3_SCENE "ui"
#endif
#ifndef M3_DMG
#define M3_DMG "1"
#endif
/* The window to ask for.  1916x1018 is what the WM hands out for a maximised
 * client (and what M2 measured); 1280x720 needs /WMSIZE staged so the WM
 * honours the request. */
#ifndef M3_WIN_W
#define M3_WIN_W 1916
#endif
#ifndef M3_WIN_H
#define M3_WIN_H 1018
#endif

#define FB_W 1920
#define FB_H 1080

/* libm-2.31.so is on the link line (the M2 probe's link recipe), so the
 * spinner can use the real sin/cos instead of a table. */
extern double cos(double);
extern double sin(double);

static int g_fail;

static void
say(const char *fmt, ...)
{
   char b[512];
   int n;
   va_list ap;
   va_start(ap, fmt);
   n = vsnprintf(b, sizeof b, fmt, ap);
   va_end(ap);
   if (n > 0)
      (void)!write(2, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}

static uint64_t
now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void
on_segv(int sig, siginfo_t *si, void *uc)
{
   (void)uc;
   say("[M3B] SIGNAL %d at %p (m3bench)\n", sig, si ? si->si_addr : NULL);
   _exit(66);
}

/* ---- GL entry points ----------------------------------------------------- */
typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef GLenum (*PFN_glGetError)(void);
typedef void (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glClear)(GLenum);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, long, const void *, GLenum);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                          GLsizei, const void *);
typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void (*PFN_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
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
typedef void (*PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_glFinish)(void);
typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                 void *);

typedef void *(*PFN_glXGetProcAddress)(const GLubyte *);
typedef GLXFBConfig *(*PFN_glXChooseFBConfig)(Display *, int, const int *, int *);
typedef XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(Display *, GLXFBConfig);
typedef Bool (*PFN_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
typedef void (*PFN_glXSwapBuffers)(Display *, GLXDrawable);
typedef void (*PFN_glXDestroyContext)(Display *, GLXContext);
typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig,
                                                     GLXContext, Bool,
                                                     const int *);

static PFN_glGetString p_glGetString;
static PFN_glGetError p_glGetError;
static PFN_glViewport p_glViewport;
static PFN_glClearColor p_glClearColor;
static PFN_glClear p_glClear;
static PFN_glEnable p_glEnable;
static PFN_glDisable p_glDisable;
static PFN_glBlendFunc p_glBlendFunc;
static PFN_glGenVertexArrays p_glGenVertexArrays;
static PFN_glBindVertexArray p_glBindVertexArray;
static PFN_glGenBuffers p_glGenBuffers;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glBufferData p_glBufferData;
static PFN_glEnableVertexAttribArray p_glEnableVertexAttribArray;
static PFN_glVertexAttribPointer p_glVertexAttribPointer;
static PFN_glCreateShader p_glCreateShader;
static PFN_glShaderSource p_glShaderSource;
static PFN_glCompileShader p_glCompileShader;
static PFN_glGetShaderiv p_glGetShaderiv;
static PFN_glGetShaderInfoLog p_glGetShaderInfoLog;
static PFN_glCreateProgram p_glCreateProgram;
static PFN_glAttachShader p_glAttachShader;
static PFN_glLinkProgram p_glLinkProgram;
static PFN_glGetProgramiv p_glGetProgramiv;
static PFN_glGetProgramInfoLog p_glGetProgramInfoLog;
static PFN_glUseProgram p_glUseProgram;
static PFN_glGetUniformLocation p_glGetUniformLocation;
static PFN_glUniform4f p_glUniform4f;
static PFN_glDrawArrays p_glDrawArrays;
static PFN_glFinish p_glFinish;
static PFN_glReadPixels p_glReadPixels;

static PFN_glXGetProcAddress p_getproc;
static PFN_glXChooseFBConfig x_glXChooseFBConfig;
static PFN_glXGetVisualFromFBConfig x_glXGetVisualFromFBConfig;
static PFN_glXMakeCurrent x_glXMakeCurrent;
static PFN_glXSwapBuffers x_glXSwapBuffers;
static PFN_glXDestroyContext x_glXDestroyContext;
static PFN_glXCreateContextAttribsARB p_create_ctx;

static void *
gpa(const char *name)
{
   void *p = p_getproc ? p_getproc((const GLubyte *)name) : NULL;
   if (!p) {
      say("[M3B] FAIL glXGetProcAddress(%s) == NULL\n", name);
      g_fail = 1;
   }
   return p;
}

#define GPA(fn) do { p_##fn = (PFN_##fn)gpa(#fn); } while (0)

static void
resolve_gl(void)
{
   GPA(glGetString);
   GPA(glGetError);
   GPA(glViewport);
   GPA(glClearColor);
   GPA(glClear);
   GPA(glEnable);
   GPA(glDisable);
   GPA(glBlendFunc);
   GPA(glGenVertexArrays);
   GPA(glBindVertexArray);
   GPA(glGenBuffers);
   GPA(glBindBuffer);
   GPA(glBufferData);
   GPA(glEnableVertexAttribArray);
   GPA(glVertexAttribPointer);
   GPA(glCreateShader);
   GPA(glShaderSource);
   GPA(glCompileShader);
   GPA(glGetShaderiv);
   GPA(glGetShaderInfoLog);
   GPA(glCreateProgram);
   GPA(glAttachShader);
   GPA(glLinkProgram);
   GPA(glGetProgramiv);
   GPA(glGetProgramInfoLog);
   GPA(glUseProgram);
   GPA(glGetUniformLocation);
   GPA(glUniform4f);
   GPA(glDrawArrays);
   GPA(glFinish);
   GPA(glReadPixels);
}

/* ---- shaders ------------------------------------------------------------- */
static const char *VS_SRC =
   "#version 330 core\n"
   "layout(location = 0) in vec2 a_pos;\n"
   "uniform vec4 u_rect;\n"
   "uniform vec4 u_color;\n"
   "out vec4 v_color;\n"
   "void main() {\n"
   "  vec2 p = u_rect.xy + a_pos * u_rect.zw;\n"
   "  gl_Position = vec4(p, 0.0, 1.0);\n"
   "  v_color = u_color;\n"
   "}\n";

static const char *FS_SRC =
   "#version 330 core\n"
   "in vec4 v_color;\n"
   "out vec4 o_color;\n"
   "void main() { o_color = v_color; }\n";

static GLuint
compile(GLenum type, const char *src, const char *label)
{
   GLuint s = p_glCreateShader(type);
   GLint ok = 0;
   char log[1024];
   p_glShaderSource(s, 1, &src, NULL);
   p_glCompileShader(s);
   p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      p_glGetShaderInfoLog(s, sizeof log, NULL, log);
      say("[M3B] FAIL %s compile: %s\n", label, log);
      g_fail = 1;
   }
   return s;
}

/* ---- scene --------------------------------------------------------------- */
static GLuint g_prog, g_vao, g_vbo;
static GLint g_u_rect, g_u_color;
static int g_win_w, g_win_h;

static void
quad(float x, float y, float w, float h, float r, float g, float b, float a)
{
   p_glUniform4f(g_u_rect, x, y, w, h);
   p_glUniform4f(g_u_color, r, g, b, a);
   p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The 110-quad UI: the menu's measured draw count and overdraw, deterministic
 * for a given `s` (so two builds of this probe draw byte-identical frames). */
static void
ui_background(uint32_t s)
{
   for (int i = 0; i < 110; i++) {
      float x, y, w, h, r, g, b;
      s = s * 1664525u + 1013904223u;
      x = -1.0f + (float)((s >> 8) & 0x3ff) / 512.0f;
      y = -1.0f + (float)((s >> 18) & 0x3ff) / 512.0f;
      w = 0.08f + (float)((s >> 4) & 0xff) / 1275.0f;
      h = 0.06f + (float)((s >> 12) & 0xff) / 1700.0f;
      r = (float)((s >> 2) & 0xff) / 255.0f;
      g = (float)((s >> 10) & 0xff) / 255.0f;
      b = (float)((s >> 20) & 0xff) / 255.0f;
      quad(x, y, w, h, r, g, b, 0.55f);
   }
}

/* One frame of the `ui` scene: the same background, then the two things a
 * loading screen actually animates.  Only those rects differ from frame N-1. */
static void
scene_ui(unsigned f)
{
   ui_background(12345u);
   /* progress bar: bottom-left, advancing by 1/120 of its track per frame */
   float p = (float)(f % 120u) / 120.0f;
   quad(-0.85f, -0.92f, 1.70f * p, 0.05f, 0.20f, 0.65f, 0.95f, 1.0f);
   /* spinner: a small square rotating around a fixed centre */
   {
      double a = (double)(f % 60u) * 0.104719755;
      quad(0.55f + 0.12f * (float)cos(a), 0.80f + 0.12f * (float)sin(a),
           0.05f, 0.05f, 1.0f, 0.85f, 0.20f, 1.0f);
   }
}

/* One frame of the `frames` scene: 110 draws, all of them somewhere new. */
static void
scene_frames(unsigned f)
{
   uint32_t s = 12345u + f * 2654435761u;
   for (int i = 0; i < 110; i++) {
      float x, y, w, h, r, g, b;
      s = s * 1664525u + 1013904223u;
      x = -1.0f + (float)((s >> 8) & 0x3ff) / 512.0f;
      y = -1.0f + (float)((s >> 18) & 0x3ff) / 512.0f;
      w = 0.10f + (float)((s >> 4) & 0xff) / 900.0f;
      h = 0.08f + (float)((s >> 12) & 0xff) / 1200.0f;
      r = (float)((s >> 2) & 0xff) / 255.0f;
      g = (float)((s >> 10) & 0xff) / 255.0f;
      b = (float)((s >> 20) & 0xff) / 255.0f;
      quad(x, y, w, h, r, g, b, 0.6f);
   }
}

/* ---- /dev/fb0 ------------------------------------------------------------ */
static unsigned int *g_fb;

static int
fb_map(void)
{
   int fd = open("/dev/fb0", O_RDWR);
   void *m;
   if (fd < 0)
      return -1;
   m = mmap(0, (size_t)FB_W * FB_H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   close(fd);
   if (m == MAP_FAILED)
      return -1;
   g_fb = (unsigned int *)m;
   return 0;
}

static long
fb_diff_rect(int x, int y, int w, int h, const unsigned char *gl)
{
   long diff = 0;
   for (int gy = 0; gy < h; gy++) {
      int dy = h - 1 - gy; /* GL rows are bottom-up, the fb is top-down */
      const unsigned char *g = gl + (size_t)gy * w * 4;
      const unsigned int *p = g_fb + (size_t)(y + dy) * FB_W + x;
      for (int gx = 0; gx < w; gx++) {
         unsigned int v = p[gx];
         unsigned char b[4] = {(unsigned char)((v >> 16) & 0xff),
                               (unsigned char)((v >> 8) & 0xff),
                               (unsigned char)(v & 0xff),
                               (unsigned char)((v >> 24) & 0xff)};
         if (b[0] != g[gx * 4] || b[1] != g[gx * 4 + 1] ||
             b[2] != g[gx * 4 + 2] || b[3] != g[gx * 4 + 3])
            diff++;
      }
   }
   return diff;
}

static uint64_t
crc64(const unsigned char *p, size_t n)
{
   uint64_t h = 0xcbf29ce484222325ull;
   for (size_t i = 0; i < n; i++) {
      h ^= p[i];
      h *= 0x100000001b3ull;
   }
   return h;
}

/* ---- main ---------------------------------------------------------------- */
/* Hand-rolled, not strtol: a glibc-2.39+ <stdlib.h> redirects strtol to
 * __isoc23_strtol, which the pinned 2.31 libc does not export (gl_bench.c
 * documents the same trap). */
static long
env_long(const char *k, long dflt)
{
   const char *v = getenv(k);
   long x = 0;
   if (!v || !*v)
      return dflt;
   while (*v == ' ')
      v++;
   if (*v < '0' || *v > '9')
      return dflt;
   while (*v >= '0' && *v <= '9')
      x = x * 10 + (*v++ - '0');
   return x;
}

int
main(void)
{
   const char *scene = M3_SCENE;
   const char *dmg = M3_DMG;
   unsigned frames = (unsigned)env_long("M3B_FRAMES", 120);
   int win_x = 0, win_y = 0;

   say("[M3B] start scene=%s damage=%s frames=%u requested=%dx%d\n", scene, dmg,
       frames, M3_WIN_W, M3_WIN_H);
   {
      struct sigaction sa;
      memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = on_segv;
      sa.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &sa, NULL);
      sigaction(SIGBUS, &sa, NULL);
   }

   /* Both switches go to files, written before libGL is loaded: /GLMODE picks
    * the driver, /GLDMG the present path (0 = M2's full-surface readback). */
   {
      FILE *f = fopen("/GLMODE", "w");
      if (!f) {
         say("[M3B] FAIL cannot write /GLMODE\n");
         return 2;
      }
      fprintf(f, "virgl\n");
      fclose(f);
      f = fopen("/GLDMG", "w");
      if (!f) {
         say("[M3B] FAIL cannot write /GLDMG\n");
         return 2;
      }
      fprintf(f, "%s\n", dmg);
      fclose(f);
   }

   Display *dpy = XOpenDisplay(NULL);
   if (!dpy) {
      say("[M3B] FAIL XOpenDisplay(NULL)\n");
      return 2;
   }
   void *h = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
   if (!h) {
      say("[M3B] FAIL dlopen(libGL.so.1): %s\n", dlerror());
      return 2;
   }
   p_getproc = (PFN_glXGetProcAddress)dlsym(h, "glXGetProcAddress");
   if (!p_getproc)
      p_getproc = (PFN_glXGetProcAddress)dlsym(h, "glXGetProcAddressARB");
   x_glXChooseFBConfig = (PFN_glXChooseFBConfig)dlsym(h, "glXChooseFBConfig");
   x_glXGetVisualFromFBConfig =
      (PFN_glXGetVisualFromFBConfig)dlsym(h, "glXGetVisualFromFBConfig");
   x_glXMakeCurrent = (PFN_glXMakeCurrent)dlsym(h, "glXMakeCurrent");
   x_glXSwapBuffers = (PFN_glXSwapBuffers)dlsym(h, "glXSwapBuffers");
   x_glXDestroyContext = (PFN_glXDestroyContext)dlsym(h, "glXDestroyContext");
   if (!p_getproc || !x_glXChooseFBConfig || !x_glXGetVisualFromFBConfig ||
       !x_glXMakeCurrent || !x_glXSwapBuffers) {
      say("[M3B] FAIL GLX export set incomplete\n");
      return 2;
   }
   p_create_ctx = (PFN_glXCreateContextAttribsARB)gpa("glXCreateContextAttribsARB");
   resolve_gl();
   if (g_fail)
      return 2;

   int attribs[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                    GLX_RENDER_TYPE,   GLX_RGBA_BIT, GLX_RED_SIZE, 8,
                    GLX_GREEN_SIZE,    8, GLX_BLUE_SIZE, 8,
                    GLX_ALPHA_SIZE,    8, GLX_DEPTH_SIZE, 24,
                    GLX_DOUBLEBUFFER,  True, None};
   int ncfg = 0;
   GLXFBConfig *cfgs = x_glXChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &ncfg);
   if (!cfgs || ncfg < 1) {
      say("[M3B] FAIL glXChooseFBConfig n=%d\n", ncfg);
      return 3;
   }
   XVisualInfo *vi = x_glXGetVisualFromFBConfig(dpy, cfgs[0]);
   if (!vi) {
      say("[M3B] FAIL glXGetVisualFromFBConfig\n");
      return 3;
   }
   Window root = RootWindow(dpy, vi->screen);
   XSetWindowAttributes swa;
   memset(&swa, 0, sizeof swa);
   swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
   swa.event_mask = ExposureMask | StructureNotifyMask;
   Window win = XCreateWindow(dpy, root, 0, 0, M3_WIN_W, M3_WIN_H, 0, vi->depth,
                              InputOutput, vi->visual,
                              CWBorderPixel | CWColormap | CWEventMask, &swa);
   if (!win) {
      say("[M3B] FAIL XCreateWindow\n");
      return 3;
   }
   XStoreName(dpy, win, "M3BENCH");
   XMapWindow(dpy, win);
   XSync(dpy, False);
   {
      uint64_t deadline = now_us() + 5000000ull;
      int got = 0;
      while (now_us() < deadline && !got) {
         while (XPending(dpy)) {
            XEvent ev;
            memset(&ev, 0, sizeof ev);
            XNextEvent(dpy, &ev);
            if (ev.type == Expose || ev.type == ConfigureNotify)
               got = 1;
         }
         if (!got)
            usleep(10000);
      }
      if (!got) {
         say("[M3B] FAIL the desktop never reported the window's content rect\n");
         return 3;
      }
   }
   {
      XWindowAttributes wa;
      XGetWindowAttributes(dpy, win, &wa);
      g_win_w = wa.width;
      g_win_h = wa.height;
      win_x = wa.x;
      win_y = wa.y;
   }

   int ctx_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                        GLX_CONTEXT_PROFILE_MASK_ARB,
                        GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
   GLXContext ctx = p_create_ctx(dpy, cfgs[0], NULL, True, ctx_attribs);
   if (!ctx) {
      say("[M3B] FAIL glXCreateContextAttribsARB(3.3 core)\n");
      return 3;
   }
   if (!x_glXMakeCurrent(dpy, win, ctx)) {
      say("[M3B] FAIL glXMakeCurrent\n");
      return 3;
   }
   say("[M3B] GL_VERSION : %s\n", (const char *)p_glGetString(GL_VERSION));
   say("[M3B] GL_RENDERER: %s\n", (const char *)p_glGetString(GL_RENDERER));
   say("[M3B] window %dx%d at %d,%d (requested %dx%d)\n", g_win_w, g_win_h,
       win_x, win_y, M3_WIN_W, M3_WIN_H);

   p_glViewport(0, 0, g_win_w, g_win_h);
   p_glGenVertexArrays(1, &g_vao);
   p_glBindVertexArray(g_vao);
   {
      static const float verts[8] = {0, 0, 1, 0, 0, 1, 1, 1};
      p_glGenBuffers(1, &g_vbo);
      p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
      p_glBufferData(GL_ARRAY_BUFFER, (long)sizeof verts, verts, GL_STATIC_DRAW);
      p_glEnableVertexAttribArray(0);
      p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
   }
   {
      GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC, "vs");
      GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC, "fs");
      GLint ok = 0;
      g_prog = p_glCreateProgram();
      p_glAttachShader(g_prog, vs);
      p_glAttachShader(g_prog, fs);
      p_glLinkProgram(g_prog);
      p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
      if (!ok) {
         char log[1024];
         p_glGetProgramInfoLog(g_prog, sizeof log, NULL, log);
         say("[M3B] FAIL link: %s\n", log);
         return 4;
      }
      p_glUseProgram(g_prog);
      g_u_rect = p_glGetUniformLocation(g_prog, "u_rect");
      g_u_color = p_glGetUniformLocation(g_prog, "u_color");
   }
   p_glEnable(GL_BLEND);
   p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   /* Warm up: shader/pipeline state and the first present (which allocates the
    * damage state with one full readback, so it must not be in the timing). */
   p_glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
   for (int i = 0; i < 3; i++) {
      p_glClear(GL_COLOR_BUFFER_BIT);
      if (strcmp(scene, "frames") == 0)
         scene_frames(0);
      else
         scene_ui(0);
      p_glFinish();
      x_glXSwapBuffers(dpy, win);
   }

   uint64_t t_raster = 0, t_present = 0;
   uint64_t t0 = now_us();
   for (unsigned f = 0; f < frames; f++) {
      uint64_t a, b, c;
      p_glClear(GL_COLOR_BUFFER_BIT);
      if (strcmp(scene, "frames") == 0)
         scene_frames(f);
      else
         scene_ui(f);
      a = now_us();
      p_glFinish();
      b = now_us();
      x_glXSwapBuffers(dpy, win);
      XSync(dpy, False);
      c = now_us();
      t_raster += b - a;
      t_present += c - b;
   }
   {
      uint64_t t1 = now_us();
      double dt = (double)(t1 - t0) / 1.0e6;
      double fps = dt > 0 ? (double)frames / dt : 0;
      say("[M3B] run frames=%u fps=%.2f ms_mean=%.3f ms_raster=%.3f "
          "ms_present=%.3f\n", frames, fps, 1000.0 * dt / (double)frames,
          1000.0 * (double)t_raster / (double)frames / 1.0e6,
          1000.0 * (double)t_present / (double)frames / 1.0e6);
   }

   /* ---- verification: what the client rendered vs what is on the screen --- */
   {
      size_t n = (size_t)g_win_w * (size_t)g_win_h * 4;
      unsigned char *px = malloc(n);
      long nz = 0;
      if (!px) {
         say("[M3B] FAIL out of memory\n");
         return 5;
      }
      /* The last frame again, so the pixels on screen and the pixels read here
       * are the same frame. */
      p_glClear(GL_COLOR_BUFFER_BIT);
      if (strcmp(scene, "frames") == 0)
         scene_frames(frames);
      else
         scene_ui(frames);
      p_glFinish();
      p_glReadPixels(0, 0, g_win_w, g_win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      x_glXSwapBuffers(dpy, win);
      usleep(200000);
      for (size_t i = 0; i < n; i += 4)
         if (px[i] | px[i + 1] | px[i + 2])
            nz++;
      say("[M3B] verify glReadPixels: crc=%016llx nz=%ld/%ld\n",
          (unsigned long long)crc64(px, n), nz, (long)(n / 4));
      if (fb_map() == 0) {
         long diff = fb_diff_rect(win_x, win_y, g_win_w, g_win_h, px);
         long total = (long)g_win_w * (long)g_win_h;
         say("[M3B] verify /dev/fb0: rect %dx%d at %d,%d diff=%ld/%ld\n",
             g_win_w, g_win_h, win_x, win_y, diff, total);
         {
            /* The desktop stamps its 8x12 mouse pointer into the framebuffer
             * after the present (M2 measured 96 px), so the budget is the
             * pointer plus a little -- anything structural is far larger. */
            long budget = total / 10000 > 512 ? total / 10000 : 512;
            if (diff > budget) {
               say("[M3B] FAIL the presented frame differs from the rendered one "
                   "(%ld px > the %ld-px budget: mouse pointer 96 px + the damage\n"
                   "rect's one-cell quantisation)\n",
                   diff, budget);
               g_fail = 1;
            }
         }
      } else {
         say("[M3B] FAIL cannot map /dev/fb0\n");
         g_fail = 1;
      }
      free(px);
   }

   say("[M3B] glGetError: 0x%x\n", (unsigned)p_glGetError());
   say("[M3B] done rc=%d\n", g_fail ? 1 : 0);
   x_glXMakeCurrent(dpy, None, NULL);
   x_glXDestroyContext(dpy, ctx);
   XCloseDisplay(dpy);
   return g_fail ? 1 : 0;
}
