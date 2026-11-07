/*
 * gl_m2probe.c -- M2's measurement probe: the same shape of GL work the game
 * does, on whichever driver libGL selected, timed inside the guest.
 *
 * Why it exists: M1 answered "can the transport carry a draw" and left the
 * two numbers that decide M3/M4 open (docs/rimworld-virgl-plan.md §6.4):
 *
 *   p   marginal cost of one draw call, us   (virgl encode + command buffer)
 *   F   fixed cost of one frame, ms          (submit + present round trip)
 *
 * and the fill rate that virgl moves to the host, in Mpx/s.
 *
 * p is isolated by drawing the same frame with 10 and with 110 quads and
 * dividing the difference by 100: everything per-frame -- one submit, one
 * readback, one XPutImage, the GLX swap -- cancels, so what is left is the
 * encode side, the plan's `110 * p` term.
 *
 * F is measured on a clear-only frame (draw nothing, swap): the whole
 * per-frame protocol cost.  That is the quantity that must stay under 20 ms.
 *
 * Fill is k full-screen blended quads minus the k=10 frame, divided: the
 * host's blended fill rate at the window's real size.
 *
 * Correctness, not only numbers: the last frame is a known two-band pattern,
 * compared against a CPU reference computed from the same rule AND against
 * the same rect of /dev/fb0 (the surface the desktop scans out).  A fast
 * driver that draws the wrong pixels is worse than softpipe.
 *
 * Driver selection: /GLMODE is written before libGL is dlopen()ed, because a
 * desktop-menu launch cannot pass an environment (the DSO reads the file in
 * xlib_create_screen; see tools/mesa/patches/0010).  The softpipe twin is the
 * same source built with -DM2P_MODE='"softpipe"', so the two runs differ in
 * exactly one string.
 *
 * Build (tools/gate/m2_virgl.py does this):
 *   gcc -O2 -std=gnu11 -fPIE -pie -nostdlib -nostartfiles \
 *       -DM2P_MODE='"virgl"' -o m2probe src/user/gl_m2probe.c \
 *       Scrt1.o crti.o -lX11 libdl-2.31.so libm-2.31.so libpthread-2.31.so \
 *       libc_nonshared.a libc.so.6 crtn.o \
 *       -Wl,--allow-shlib-undefined -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
 */
#define _GNU_SOURCE 1
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <ucontext.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef M2P_MODE
#define M2P_MODE "virgl"
#endif

#define WIN_W_REQUEST 960
#define WIN_H_REQUEST 540
#define FB_W 1920
#define FB_H 1080
#define NFRAMES 20

static void
say(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fflush(stderr);
}

static int g_fail;

static uint64_t
now_us(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}


/* ---- crash attribution --------------------------------------------------- */
/* A fault inside the GL stack is otherwise a dead end: no debugger in the
 * guest, and the kernel's report has no user backtrace.  Print the faulting
 * RIP plus the top of the stack (return addresses are absolute, and
 * dump_link_map() gives the DSO bases), then exit. */
static void
raw_say(const char *s)
{
   size_t n = 0;
   while (s[n]) n++;
   (void)!write(2, s, n);
}

static void
on_segv(int sig, siginfo_t *si, void *uc)
{
   ucontext_t *u = (ucontext_t *)uc;
   static const char hexd[] = "0123456789abcdef";
   char b[512];
   int j = 0;
   const char *p = "[M2P] SIGSEGV rip=0x";
   while (*p) b[j++] = *p++;
   unsigned long long rip = (unsigned long long)u->uc_mcontext.gregs[REG_RIP];
   unsigned long long rsp = (unsigned long long)u->uc_mcontext.gregs[REG_RSP];
   for (int i = 0; i < 16; i++) b[j++] = hexd[(rip >> (60 - 4 * i)) & 0xf];
   p = " rsp=0x";
   while (*p) b[j++] = *p++;
   for (int i = 0; i < 16; i++) b[j++] = hexd[(rsp >> (60 - 4 * i)) & 0xf];
   p = " addr=0x";
   while (*p) b[j++] = *p++;
   unsigned long long a = (unsigned long long)(uintptr_t)si->si_addr;
   for (int i = 0; i < 16; i++) b[j++] = hexd[(a >> (60 - 4 * i)) & 0xf];
   b[j++] = '\n';
   (void)!write(2, b, (size_t)j);

   /* Eight words at rsp: the return addresses that name the caller. */
   if (rsp) {
      const unsigned long long *sp = (const unsigned long long *)(uintptr_t)rsp;
      for (int w = 0; w < 8; w++) {
         unsigned long long v = 0;
         __builtin_memcpy(&v, &sp[w], sizeof v);
         j = 0;
         p = "[M2P]   stack";
         while (*p) b[j++] = *p++;
         b[j++] = (char)('0' + w);
         p = "=0x";
         while (*p) b[j++] = *p++;
         for (int i = 0; i < 16; i++) b[j++] = hexd[(v >> (60 - 4 * i)) & 0xf];
         b[j++] = '\n';
         (void)!write(2, b, (size_t)j);
      }
   }
   _exit(9);
}

static void
dump_link_map(void)
{
   struct link_map *lm = NULL;
   void *mh = dlopen(NULL, RTLD_NOW);
   if (!mh || dlinfo(mh, RTLD_DI_LINKMAP, &lm) != 0) {
      say("[M2P] dlinfo(RTLD_DI_LINKMAP) failed\n");
      return;
   }
   say("[M2P] loaded objects (base = load bias):\n");
   for (struct link_map *m = lm; m; m = m->l_next) {
      const char *n = (m->l_name && *m->l_name) ? m->l_name : "(main)";
      say("[M2P]   base=0x%llx %s\n", (unsigned long long)m->l_addr, n);
   }
}

/* ---- the desktop framebuffer (same decode as src/user/gl_probe.c) -------- */
static unsigned int *g_fb;

static int
fb_map(void)
{
   int fd = open("/dev/fb0", O_RDWR);
   if (fd < 0) {
      say("[M2P] desktop fb: open(/dev/fb0) failed\n");
      return -1;
   }
   void *m = mmap(0, (size_t)FB_W * FB_H * 4, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, 0);
   close(fd);
   if (m == MAP_FAILED) {
      say("[M2P] desktop fb: mmap(/dev/fb0) failed\n");
      return -1;
   }
   g_fb = (unsigned int *)m;
   return 0;
}

/* /dev/fb0 is B8G8R8A8 (word 0xAARRGGBB) and X's origin is top-left while
 * GL's is bottom-left, so the comparison walks GL rows bottom-up. */
static int
fb_diff_rect(int x, int y, int w, int h, const unsigned char *gl, int *shown)
{
   int diff = 0;
   for (int gy = 0; gy < h; gy++) {
      int dy = h - 1 - gy;
      const unsigned char *g = gl + (size_t)gy * w * 4;
      const unsigned int *p = g_fb + (size_t)(y + dy) * FB_W + x;
      for (int gx = 0; gx < w; gx++) {
         unsigned int v = p[gx];
         unsigned char b[4] = {(unsigned char)((v >> 16) & 0xff),
                               (unsigned char)((v >> 8) & 0xff),
                               (unsigned char)(v & 0xff),
                               (unsigned char)((v >> 24) & 0xff)};
         if (b[0] != g[gx * 4] || b[1] != g[gx * 4 + 1] ||
             b[2] != g[gx * 4 + 2] || b[3] != g[gx * 4 + 3]) {
            diff++;
            if (shown && *shown < 3) {
               say("[M2P]   diff (%d,%d): gl=%02x%02x%02x%02x fb=%02x%02x%02x%02x\n",
                   gx, gy, g[gx * 4], g[gx * 4 + 1], g[gx * 4 + 2], g[gx * 4 + 3],
                   b[0], b[1], b[2], b[3]);
               (*shown)++;
            }
         }
      }
   }
   return diff;
}

/* ---- GL entry points ----------------------------------------------------- */
typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef GLenum (*PFN_glGetError)(void);
typedef void (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glClear)(GLbitfield);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glBlendFunc)(GLenum, GLenum);
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean,
                                          GLsizei, const void *);
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
typedef void (*PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                 void *);

/* The GLX entry points (glx.h declares the functions, not pointer typedefs). */
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
   void *p = p_getproc((const GLubyte *)name);
   if (!p) {
      say("[M2P] FAIL glXGetProcAddress(%s) == NULL\n", name);
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
      say("[M2P] FAIL %s compile: %s\n", label, log);
      g_fail = 1;
   }
   return s;
}

/* ---- scenes -------------------------------------------------------------- */
static GLuint g_prog, g_vao, g_vbo;
static GLint g_u_rect, g_u_color;
static int g_win_w, g_win_h;

/* A deterministic scatter of quads: the game's UI shape (many small blended
 * draws), not one big triangle. */
static void
scene_quads(int k)
{
   uint32_t seed = 12345;
   for (int i = 0; i < k; i++) {
      seed = seed * 1664525u + 1013904223u;
      float x = -1.0f + (float)((seed >> 8) & 0x3ff) / 512.0f;
      float y = -1.0f + (float)((seed >> 18) & 0x3ff) / 512.0f;
      float w = 0.05f + (float)((seed >> 4) & 0xff) / 2550.0f;
      float h = 0.05f + (float)((seed >> 12) & 0xff) / 2550.0f;
      p_glUniform4f(g_u_rect, x, y, w, h);
      p_glUniform4f(g_u_color, (float)((seed >> 2) & 0xff) / 255.0f,
                    (float)((seed >> 10) & 0xff) / 255.0f,
                    (float)((seed >> 20) & 0xff) / 255.0f, 0.5f);
      p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }
}

static void
scene_fill(int k)
{
   for (int i = 0; i < k; i++) {
      p_glUniform4f(g_u_rect, -1.0f, -1.0f, 2.0f, 2.0f);
      p_glUniform4f(g_u_color, 0.25f, 0.5f, 0.75f, 0.25f);
      p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }
}

/* The verification frame: two bands, alpha 1, so the CPU reference needs no
 * model of blending. */
#define VC_R 0.05f
#define VC_G 0.10f
#define VC_B 0.15f
#define VB_R 0.90f
#define VB_G 0.20f
#define VB_B 0.10f

static void
scene_verify(void)
{
   p_glClearColor(VC_R, VC_G, VC_B, 1.0f);
   p_glClear(GL_COLOR_BUFFER_BIT);
   p_glDisable(GL_BLEND);
   p_glUniform4f(g_u_rect, -1.0f, 0.0f, 2.0f, 1.0f); /* upper half */
   p_glUniform4f(g_u_color, VB_R, VB_G, VB_B, 1.0f);
   p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static unsigned char
b8(float f)
{
   return (unsigned char)(f * 255.0f + 0.5f);
}

/* ---- main ---------------------------------------------------------------- */
int
main(void)
{
   say("[M2P] probe start (baked mode=%s)\n", M2P_MODE);
   {
      struct sigaction sa;
      memset(&sa, 0, sizeof sa);
      sa.sa_sigaction = on_segv;
      sa.sa_flags = SA_SIGINFO;
      sigaction(SIGSEGV, &sa, NULL);
      sigaction(SIGBUS, &sa, NULL);
   }

   /* The runtime switch: written before libGL is loaded, read by
    * xlib_create_screen() in the DSO. */
   {
      FILE *mf = fopen("/GLMODE", "w");
      if (!mf) {
         say("[M2P] FAIL cannot write /GLMODE\n");
         return 2;
      }
      fprintf(mf, "%s\n", M2P_MODE);
      fclose(mf);
   }

   Display *dpy = XOpenDisplay(NULL);
   if (!dpy) {
      say("[M2P] FAIL XOpenDisplay(NULL)\n");
      return 2;
   }

   void *h = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
   if (!h) {
      say("[M2P] FAIL dlopen(libGL.so.1): %s\n", dlerror());
      return 2;
   }
   dump_link_map();

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
      say("[M2P] FAIL GLX export set incomplete\n");
      return 2;
   }
   p_create_ctx =
      (PFN_glXCreateContextAttribsARB)gpa("glXCreateContextAttribsARB");
   resolve_gl();
   if (g_fail)
      return 2;

   int attribs[] = {GLX_X_RENDERABLE, True,  GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                    GLX_RENDER_TYPE,   GLX_RGBA_BIT, GLX_RED_SIZE, 8,
                    GLX_GREEN_SIZE,    8,     GLX_BLUE_SIZE,  8,
                    GLX_ALPHA_SIZE,    8,     GLX_DEPTH_SIZE, 24,
                    GLX_DOUBLEBUFFER,  True,  None};
   int ncfg = 0;
   GLXFBConfig *cfgs =
      x_glXChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &ncfg);
   if (!cfgs || ncfg < 1) {
      say("[M2P] FAIL glXChooseFBConfig n=%d\n", ncfg);
      return 3;
   }
   XVisualInfo *vi = x_glXGetVisualFromFBConfig(dpy, cfgs[0]);
   if (!vi) {
      say("[M2P] FAIL glXGetVisualFromFBConfig\n");
      return 3;
   }

   Window root = RootWindow(dpy, vi->screen);
   XSetWindowAttributes swa;
   memset(&swa, 0, sizeof swa);
   swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
   swa.event_mask = ExposureMask | StructureNotifyMask;
   Window win = XCreateWindow(dpy, root, 0, 0, WIN_W_REQUEST, WIN_H_REQUEST, 0,
                              vi->depth, InputOutput, vi->visual,
                              CWBorderPixel | CWColormap | CWEventMask, &swa);
   if (!win) {
      say("[M2P] FAIL XCreateWindow\n");
      return 3;
   }
   XStoreName(dpy, win, "M2PROBE");
   XMapWindow(dpy, win);
   XSync(dpy, False);

   /* The desktop answers with the content rect; painting before it arrives is
    * silently dropped (measured in the M4 probe). */
   {
      long deadline = (long)(now_us() / 1000) + 5000;
      int got = 0;
      while ((long)(now_us() / 1000) < deadline && !got) {
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
         say("[M2P] FAIL the desktop never reported the window's content rect\n");
         return 3;
      }
   }
   XWindowAttributes wa;
   XGetWindowAttributes(dpy, win, &wa);
   g_win_w = wa.width;
   g_win_h = wa.height;

   int ctx_attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                        GLX_CONTEXT_PROFILE_MASK_ARB,
                        GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
   GLXContext ctx = p_create_ctx(dpy, cfgs[0], NULL, True, ctx_attribs);
   if (!ctx) {
      say("[M2P] FAIL glXCreateContextAttribsARB(3.3 core)\n");
      return 3;
   }
   if (!x_glXMakeCurrent(dpy, win, ctx)) {
      say("[M2P] FAIL glXMakeCurrent\n");
      return 3;
   }

   say("[M2P] GL_VERSION : %s\n", (const char *)p_glGetString(GL_VERSION));
   say("[M2P] GL_RENDERER: %s\n", (const char *)p_glGetString(GL_RENDERER));
   say("[M2P] window %dx%d at %d,%d\n", g_win_w, g_win_h, wa.x, wa.y);

   say("[M2P] step 0a: gpa(glGetIntegerv)\n");
   {
      PFN_glGetIntegerv gi = (PFN_glGetIntegerv)gpa("glGetIntegerv");
      say("[M2P] step 0b: gi=%p\n", (void *)gi);
      GLint v = -1;
      if (gi) {
         say("[M2P] step 0c: calling glGetIntegerv\n");
         gi(GL_MAX_TEXTURE_SIZE, &v); say("[M2P] limits: MAX_TEXTURE_SIZE=%d", (int)v);
         gi(GL_MAX_VERTEX_ATTRIBS, &v); say(" MAX_VERTEX_ATTRIBS=%d", (int)v);
         gi(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &v); say(" MAX_TEX_UNITS=%d\n", (int)v);
      }
   }
   say("[M2P] step 1: viewport + VAO\n");
   p_glViewport(0, 0, g_win_w, g_win_h);
   p_glGenVertexArrays(1, &g_vao);
   p_glBindVertexArray(g_vao);
   static const float verts[8] = {0, 0, 1, 0, 0, 1, 1, 1};
   say("[M2P] step 2: VBO\n");
   p_glGenBuffers(1, &g_vbo);
   p_glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
   p_glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
   p_glEnableVertexAttribArray(0);
   p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);

   say("[M2P] step 3: shaders\n");
   GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC, "vs");
   say("[M2P] step 3a: vs done\n");
   GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC, "fs");
   say("[M2P] step 3b: fs done\n");
   g_prog = p_glCreateProgram();
   p_glAttachShader(g_prog, vs);
   p_glAttachShader(g_prog, fs);
   p_glLinkProgram(g_prog);
   {
      GLint ok = 0;
      char log[1024];
      p_glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
      if (!ok) {
         p_glGetProgramInfoLog(g_prog, sizeof log, NULL, log);
         say("[M2P] FAIL link: %s\n", log);
         return 4;
      }
   }
   say("[M2P] step 4: program linked\n");
   p_glUseProgram(g_prog);
   g_u_rect = p_glGetUniformLocation(g_prog, "u_rect");
   g_u_color = p_glGetUniformLocation(g_prog, "u_color");
   p_glEnable(GL_BLEND);
   p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   say("[M2P] step 5: first frame\n");
   for (int i = 0; i < 5; i++) {
      p_glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
      p_glClear(GL_COLOR_BUFFER_BIT);
      scene_quads(110);
      x_glXSwapBuffers(dpy, win);
   }
   say("[M2P] warmup 5 frames done (err=0x%x)\n", p_glGetError());

   /* ---- F: fixed cost of one frame (clear + swap only) ------------------ */
   uint64_t f_sum = 0;
   for (int i = 0; i < NFRAMES; i++) {
      uint64_t t0 = now_us();
      p_glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
      p_glClear(GL_COLOR_BUFFER_BIT);
      x_glXSwapBuffers(dpy, win);
      f_sum += now_us() - t0;
   }
   uint64_t f_us = f_sum / NFRAMES;
   say("[M2P] F fixed-per-frame: %d clear+swap frames, mean=%llu us (%.2f ms)\n",
       NFRAMES, (unsigned long long)f_us, (double)f_us / 1000.0);

   /* ---- p: marginal per-draw cost -------------------------------------- *
    * 10 vs 1010 quads, not 10 vs 110: the guest clock is 1 ms-granular (M1
    * measured this), so a 100-draw difference divided into a ~9 ms frame has a
    * +/-5 us/draw error bar.  1000 extra draws put the error at ~0.5 us/draw
    * and cost one 60 KB command stream -- still under the kernel's 128 KiB
    * per-submit limit, so it measures the same path a frame uses. */
   uint64_t k10_sum = 0, k1000_sum = 0;
   for (int round = 0; round < 2; round++) {
      for (int i = 0; i < NFRAMES; i++) {
         uint64_t t0 = now_us();
         p_glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
         p_glClear(GL_COLOR_BUFFER_BIT);
         scene_quads(10);
         x_glXSwapBuffers(dpy, win);
         k10_sum += now_us() - t0;
      }
      for (int i = 0; i < NFRAMES; i++) {
         uint64_t t0 = now_us();
         p_glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
         p_glClear(GL_COLOR_BUFFER_BIT);
         scene_quads(1010);
         x_glXSwapBuffers(dpy, win);
         k1000_sum += now_us() - t0;
      }
   }
   uint64_t k10_us = k10_sum / (2 * NFRAMES);
   uint64_t k1000_us = k1000_sum / (2 * NFRAMES);
   double p_us = (double)((int64_t)k1000_us - (int64_t)k10_us) / 1000.0;
   double p110_ms = (double)k10_us / 1000.0 + 110.0 * p_us / 1000.0;
   say("[M2P] p per-draw: k10=%llu us k1010=%llu us -> p=%.2f us/draw "
       "(projected 110-draw frame=%.2f ms, %.1f fps)\n",
       (unsigned long long)k10_us, (unsigned long long)k1000_us, p_us,
       p110_ms, 1000.0 / p110_ms);

   /* ---- fill: full-screen blended quads --------------------------------- */
   uint64_t fill10_sum = 0, fill110_sum = 0;
   for (int i = 0; i < 10; i++) {
      uint64_t t0 = now_us();
      p_glClear(GL_COLOR_BUFFER_BIT);
      scene_fill(10);
      x_glXSwapBuffers(dpy, win);
      fill10_sum += now_us() - t0;
   }
   for (int i = 0; i < 10; i++) {
      uint64_t t0 = now_us();
      p_glClear(GL_COLOR_BUFFER_BIT);
      scene_fill(110);
      x_glXSwapBuffers(dpy, win);
      fill110_sum += now_us() - t0;
   }
   uint64_t f10 = fill10_sum / 10, f110 = fill110_sum / 10;
   double ms_per_quad = (double)((int64_t)f110 - (int64_t)f10) / 100.0 / 1000.0;
   double mpx = (double)g_win_w * (double)g_win_h / 1.0e6;
   double mpx_s = ms_per_quad > 0.0 ? mpx / (ms_per_quad / 1000.0) : 0.0;
   say("[M2P] fill: k10=%llu us k110=%llu us -> %.3f ms per fullscreen quad "
       "= %.2f Mpx/s at %dx%d (marginal; a lower bound on the host's fill)\n",
       (unsigned long long)f10, (unsigned long long)f110, ms_per_quad, mpx_s,
       g_win_w, g_win_h);

   /* ---- correctness: the pattern vs a CPU reference, and vs /dev/fb0 ---- */
   scene_verify();
   unsigned char *gl_px = malloc((size_t)g_win_w * g_win_h * 4);
   if (!gl_px) {
      say("[M2P] FAIL out of memory for the readback\n");
      return 5;
   }
   p_glReadPixels(0, 0, g_win_w, g_win_h, GL_RGBA, GL_UNSIGNED_BYTE, gl_px);
   x_glXSwapBuffers(dpy, win);
   usleep(150000);

   long match = 0, total = (long)g_win_w * g_win_h;
   for (int y = 0; y < g_win_h; y++) {
      int band = (y >= g_win_h / 2); /* GL row 0 is the bottom */
      int er = band ? b8(VB_R) : b8(VC_R);
      int eg = band ? b8(VB_G) : b8(VC_G);
      int eb = band ? b8(VB_B) : b8(VC_B);
      for (int x = 0; x < g_win_w; x++) {
         const unsigned char *p = gl_px + ((size_t)y * g_win_w + x) * 4;
         int dr = (int)p[0] - er, dg = (int)p[1] - eg, db = (int)p[2] - eb;
         if (dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 && db <= 2)
            match++;
      }
   }
   say("[M2P] verify glReadPixels: match=%ld/%ld (%.2f%%) band=%02x%02x%02x "
       "clear=%02x%02x%02x\n", match, total, 100.0 * (double)match / (double)total,
       b8(VB_R), b8(VB_G), b8(VB_B), b8(VC_R), b8(VC_G), b8(VC_B));

   int fb_diff = -1;
   if (fb_map() == 0) {
      int shown = 0;
      fb_diff = fb_diff_rect(wa.x, wa.y, g_win_w, g_win_h, gl_px, &shown);
      say("[M2P] verify /dev/fb0: rect %dx%d at %d,%d diff=%d/%ld (%.2f%% match)\n",
          g_win_w, g_win_h, wa.x, wa.y, fb_diff, total,
          100.0 * (double)(total - fb_diff) / (double)total);
   }

   /* The desktop stamps its mouse pointer into the framebuffer after the
    * present (the M4 gate measured the same thing), so a few dozen differing
    * pixels are the pointer, not a partial present: accept < 0.01% and say
    * so.  Anything structural (a shifted row, a wrong channel) is far bigger. */
   int pointer_px = (int)(total / 10000);
   int fb_ok = (fb_diff == 0) || (fb_diff > 0 && fb_diff <= pointer_px);
   int ok = !g_fail && match == total && fb_ok;
   say("[M2P] done rc=%d%s\n", ok ? 0 : 1,
       (fb_ok && fb_diff > 0) ? " (fb diff within the mouse-pointer budget)" : "");

   x_glXMakeCurrent(dpy, None, NULL);
   x_glXDestroyContext(dpy, ctx);
   XCloseDisplay(dpy);
   return ok ? 0 : 1;
}
