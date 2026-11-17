/*
 * gl_probe.c -- the M4 in-guest gate: a real glibc program that gets a GL 3.3
 * core context out of the cross-built libGL.so.1 INSIDE HobbyOS, compiles a
 * GLSL-330 shader pair, draws a triangle, PRESENTS it through glXSwapBuffers
 * into a real X window, proves the pixels are on the desktop's framebuffer,
 * and tears the whole stack down.
 *
 * Two readbacks, because they prove different things:
 *
 *   1. an FBO readback (fnv1a64 of a 256x256 RGBA8 renderbuffer) -- the same
 *      code path on every host, no window, no swap chain, so it is the
 *      renderer-identity check (the guest number matches the host number);
 *   2. the desktop framebuffer (/dev/fb0) at the window's content rect, hashed
 *      in the same byte order -- only this proves the frame was *presented*.
 *      A GL readback can agree with itself and never reach the screen.
 *
 * The window is created through the compat libX11 (the same way SDL/Unity will)
 * and the probe waits for the desktop's ConfigureNotify/Expose before drawing:
 * the shim's XPutImage refuses to blit a window the desktop has not laid out,
 * so painting early silently drops every pixel (measured -- a first version of
 * this probe swapped into nothing and still reported success).
 *
 * The context request is Unity's, not a convenient one: the probe asks for
 * **4.5 core** with the game's exact attribute list (RELEASE_BEHAVIOR=FLUSH,
 * OPENGL_NO_ERROR=False), plus a second context sharing with it -- the shape
 * measured from Unity with apitrace.  The GLX frontend we ship must clamp that
 * request to the best context the driver has (3.3 core, plan AD-5) rather than
 * refuse it, so the whole render/present proof runs on the 4.5-requested
 * context.  A 3.3 request must still be served unchanged and a 2.1 request
 * honoured or refused cleanly; all four rows appear in the log before the
 * render, and only the 4.5 row failing makes the probe exit non-zero.
 *
 * The DSO is dlopen()ed, never linked: libGL.so.1 has no DT_NEEDED of its own,
 * so its libc/libX11 symbols resolve from the process that is already running
 * (the pinned glibc 2.31 + /LIB64/libX11.so.6).  RTLD_NOW is deliberate: it
 * turns "the DSO needs a symbol nobody provides" into a dlerror() here instead
 * of a crash inside a GL call later.  RTLD_GLOBAL matches what SDL/Unity do
 * (they dlopen libGL and then resolve gl* through glXGetProcAddress).
 *
 * Teardown is part of the claim: glXMakeCurrent(None) -> glXDestroyContext ->
 * XCloseDisplay -> dlclose(libGL) -> exit().  libGL is linked DF_1_NODELETE
 * because Mesa registers process-global atexit() handlers from inside the DSO;
 * without it the exit() after dlclose jumps into the unmapped text (see
 * tools/mesa/build-hobbyos.sh).
 *
 * Markers go to stderr: a desktop-launched program gets fd 0/1 as the pixel
 * protocol pipes, and fd 2 is the serial console (same rule as the M3 client).
 *
 * Build (tools/gate/m4_gl_probe.py does exactly this):
 *   gcc -O2 -std=gnu11 -fPIE -pie -nostdlib -nostartfiles \
 *       -o glprobe gl_probe.c Scrt1.o crti.o -lX11 libdl-2.31.so \
 *       libc_nonshared.a libc.so.6 crtn.o \
 *       -Wl,--allow-shlib-undefined -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2
 *
 * Exit status: 0 only when the 4.5 core request came back as a live 3.3 core
 * context, a 3.3 request was served unchanged, the shaders compiled and linked,
 * the FBO was complete, the readback covered the triangle, the desktop held
 * the swapped frame byte for byte, and teardown was clean.
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include "../include/gpu_abi.h"
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROBE_W 256
#define PROBE_H 256

static void say(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

static int g_fail;

/* A fault inside the GL stack is otherwise a dead end in the guest: there is
 * no debugger, and the kernel's report has no user stack.  This handler turns
 * a SIGSEGV into the faulting RIP plus the top of the stack, which is enough
 * to attribute the crash to a DSO (subtract the base printed by
 * dump_link_map) and to the caller. */
static void raw_say(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  (void)!write(2, s, n);
}

/* One write for the whole line: the serial console interleaves other CPUs'
 * output between two writes (measured: a watchdog dump landed in the middle of
 * the register report), so a multi-write report arrives unreadable. */
static void on_segv(int sig, siginfo_t *si, void *uc) {
  ucontext_t *u = (ucontext_t *)uc;
  static const char hexd[] = "0123456789abcdef";
  char b[192];
  int j = 0;
  const char *p = "[GL] SIGSEGV rip=";
  while (*p && j < 20) b[j++] = *p++;
  unsigned long long vals[5] = {
      (unsigned long long)u->uc_mcontext.gregs[REG_RIP],
      (unsigned long long)u->uc_mcontext.gregs[REG_RSP],
      (unsigned long long)u->uc_mcontext.gregs[REG_RDI],
      (unsigned long long)u->uc_mcontext.gregs[REG_RSI],
      (unsigned long long)u->uc_mcontext.gregs[REG_RDX],
  };
  static const char *names[5] = {" rip=", " rsp=", " rdi=", " rsi=", " rdx="};
  for (int k = 0; k < 5; k++) {
    if (k > 0) { const char *q = names[k]; while (*q) b[j++] = *q++; }
    b[j++] = '0'; b[j++] = 'x';
    for (int i = 0; i < 16; i++)
      b[j++] = hexd[(vals[k] >> (60 - 4 * i)) & 0xf];
  }
  b[j++] = ' '; b[j++] = 's'; b[j++] = 'i'; b[j++] = 'g'; b[j++] = '='; 
  b[j++] = (char)('0' + (sig % 10));
  b[j++] = ' '; b[j++] = 'a'; b[j++] = 'd'; b[j++] = 'd'; b[j++] = 'r'; b[j++] = '=';
  b[j++] = '0'; b[j++] = 'x';
  unsigned long long a = (unsigned long long)(uintptr_t)si->si_addr;
  for (int i = 0; i < 16; i++) b[j++] = hexd[(a >> (60 - 4 * i)) & 0xf];
  b[j++] = '\n';
  (void)!write(2, b, (size_t)j);
  _exit(9);
}

/* Every loaded object and its load bias: the only way to map a raw guest
 * address back to a DSO without /proc/self/maps paths. */
static void dump_link_map(void) {
  struct link_map *lm = NULL;
  void *mh = dlopen(NULL, RTLD_NOW);      /* the main program: head of the list */
  if (!mh || dlinfo(mh, RTLD_DI_LINKMAP, &lm) != 0) {
    say("[GL] dlinfo(RTLD_DI_LINKMAP) failed: %s\n", dlerror());
    return;
  }
  say("[GL] loaded objects (base = load bias):\n");
  for (struct link_map *m = lm; m; m = m->l_next) {
    const char *n = (m->l_name && *m->l_name) ? m->l_name : "(main)";
    say("[GL]   base=0x%llx %s\n", (unsigned long long)m->l_addr, n);
  }
}

/* ---- FNV-1a 64: the same hash the host harness reports for the cube ------ */
static uint64_t fnv1a64(const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)p;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

/* ---- the desktop framebuffer (/dev/fb0) --------------------------------
 * The compat libX11 paints into exactly this node (docs/linux-abi.md §9:
 * fixed 1920x1080 B8G8R8A8), so a pixel the probe reads here has already left
 * the GL stack, left libX11's shadow, and been blitted by XFlush.  That is the
 * only readback that is *evidence of presentation*; glReadPixels on the GL
 * side can agree with itself and still never reach the screen. */
#define FB_W 1920
#define FB_H 1080
static unsigned int *g_fb;

static int fb_map(void) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    say("[GL] desktop fb: open(/dev/fb0) failed\n");
    return -1;
  }
  void *m = mmap(0, (size_t)FB_W * FB_H * 4, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    say("[GL] desktop fb: mmap(/dev/fb0) failed\n");
    return -1;
  }
  g_fb = (unsigned int *)m;
  say("[GL] desktop fb: /dev/fb0 mapped at %p (%dx%d b8g8r8a8)\n", m, FB_W,
      FB_H);
  return 0;
}

/* Same FNV-1a over the same RGBA byte order the GL readback uses, so the two
 * numbers are directly comparable.  Two conversions, both measured:
 *
 *   - /dev/fb0 is B8G8R8A8 in the *memory* sense the shim writes: the 32-bit
 *     word is 0xAARRGGBB, so R is fed from bits 16..23.  Measured against the
 *     GL frame: GL (07,07,f1,ff) [blue vertex] reads back as 0xff0707f1.
 *   - X's origin is the top-left and GL's is the bottom-left, so the same frame
 *     is stored with opposite row order.  Walking the rows bottom-up gives the
 *     byte sequence glReadPixels produced.
 */
static uint64_t fb_hash_rect(int x, int y, int w, int h, int *nonbg_out,
                             const unsigned char bg[4]) {
  uint64_t hash = 0xcbf29ce484222325ull;
  int nonbg = 0;
  const unsigned int bgv = ((unsigned)bg[0] << 16) | ((unsigned)bg[1] << 8) |
                           (unsigned)bg[2];
  for (int row = h - 1; row >= 0; row--) {
    const unsigned int *p = g_fb + (size_t)(y + row) * FB_W + x;
    for (int col = 0; col < w; col++) {
      unsigned int v = p[col];
      unsigned char b[4] = {(unsigned char)((v >> 16) & 0xff),
                            (unsigned char)((v >> 8) & 0xff),
                            (unsigned char)(v & 0xff),
                            (unsigned char)((v >> 24) & 0xff)};
      for (int i = 0; i < 4; i++) {
        hash ^= b[i];
        hash *= 0x100000001b3ull;
      }
      if ((v & 0x00ffffffu) != bgv) nonbg++;
    }
  }
  if (nonbg_out) *nonbg_out = nonbg;
  return hash;
}

/* Pixel-exact comparison of the GL frame against the same rect of /dev/fb0.
 * Rows are matched bottom-up (X is top-down, GL bottom-up) and channels are
 * decoded from the fb's 0xAARRGGBB word.  Returns the number of differing
 * pixels and, in *bbox, the rect that contains all of them -- a small box is
 * the desktop re-stamping its mouse pointer over the app's pixels (which is
 * what ESC ] F asks it to do), not a partial present. */
static int fb_diff_rect(int x, int y, int w, int h, const unsigned char *gl,
                        int *bbox, int *shown) {
  int diff = 0;
  bbox[0] = w; bbox[1] = h; bbox[2] = -1; bbox[3] = -1;
  for (int gy = 0; gy < h; gy++) {
    int dy = h - 1 - gy;                  /* GL row -> desktop row */
    const unsigned char *g = gl + (size_t)gy * w * 4;
    const unsigned int *p = g_fb + (size_t)(y + dy) * FB_W + x;
    for (int gx = 0; gx < w; gx++) {
      unsigned int v = p[gx];
      unsigned char b[4] = {(unsigned char)((v >> 16) & 0xff),
                            (unsigned char)((v >> 8) & 0xff),
                            (unsigned char)(v & 0xff),
                            (unsigned char)((v >> 24) & 0xff)};
      if (b[0] != g[gx * 4] || b[1] != g[gx * 4 + 1] || b[2] != g[gx * 4 + 2] ||
          b[3] != g[gx * 4 + 3]) {
        diff++;
        if (gx < bbox[0]) bbox[0] = gx;
        if (dy < bbox[1]) bbox[1] = dy;
        if (gx > bbox[2]) bbox[2] = gx;
        if (dy > bbox[3]) bbox[3] = dy;
        if (shown && *shown < 3) {
          say("[GL]   diff at (%d,%d): gl=%02x%02x%02x%02x desktop=%02x%02x%02x%02x\n",
              gx, gy, g[gx * 4], g[gx * 4 + 1], g[gx * 4 + 2], g[gx * 4 + 3],
              b[0], b[1], b[2], b[3]);
          (*shown)++;
        }
      }
    }
  }
  return diff;
}

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + (long)(ts.tv_nsec / 1000000);
}

/* ---- GL entry points, all through glXGetProcAddress --------------------- */
typedef void (*PFN_glClear)(GLbitfield);
typedef void (*PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (*PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                 GLenum, void *);
typedef void (*PFN_glFinish)(void);
typedef GLenum (*PFN_glGetError)(void);
typedef const GLubyte *(*PFN_glGetString)(GLenum);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
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
typedef void (*PFN_glGenFramebuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_glGenRenderbuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindRenderbuffer)(GLenum, GLuint);
typedef void (*PFN_glRenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFN_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFN_glCheckFramebufferStatus)(GLenum);
typedef GLint (*PFN_glGetAttribLocation)(GLuint, const GLchar *);
typedef void (*PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (*PFN_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                 GLenum, GLenum, const void *);
typedef void (*PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint,
                                           GLint);
typedef void (*PFN_glPixelStorei)(GLenum, GLint);
typedef void (*PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei,
                                    GLsizei, GLenum, GLenum, const void *);
typedef void (*PFN_glEnable)(GLenum);
typedef void (*PFN_glDisable)(GLenum);
typedef void (*PFN_glDepthFunc)(GLenum);
typedef void (*PFN_glDepthMask)(GLboolean);
typedef void (*PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void *);
typedef void (*PFN_glActiveTexture)(GLenum);
typedef void (*PFN_glUniform1i)(GLint, GLint);
typedef GLint (*PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void (*PFN_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint,
                                     GLint, GLint, GLbitfield, GLenum);

static struct {
  PFN_glClear Clear;
  PFN_glClearColor ClearColor;
  PFN_glViewport Viewport;
  PFN_glDrawArrays DrawArrays;
  PFN_glReadPixels ReadPixels;
  PFN_glFinish Finish;
  PFN_glGetError GetError;
  PFN_glGetString GetString;
  PFN_glGetIntegerv GetIntegerv;
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
  PFN_glGenFramebuffers GenFramebuffers;
  PFN_glBindFramebuffer BindFramebuffer;
  PFN_glGenRenderbuffers GenRenderbuffers;
  PFN_glBindRenderbuffer BindRenderbuffer;
  PFN_glRenderbufferStorage RenderbufferStorage;
  PFN_glFramebufferRenderbuffer FramebufferRenderbuffer;
  PFN_glCheckFramebufferStatus CheckFramebufferStatus;
  PFN_glGetAttribLocation GetAttribLocation;
  PFN_glGenTextures GenTextures;
  PFN_glBindTexture BindTexture;
  PFN_glTexParameteri TexParameteri;
  PFN_glTexImage2D TexImage2D;
  PFN_glFramebufferTexture2D FramebufferTexture2D;
  PFN_glPixelStorei PixelStorei;
  PFN_glTexSubImage2D TexSubImage2D;
  PFN_glEnable Enable;
  PFN_glDisable Disable;
  PFN_glDepthFunc DepthFunc;
  PFN_glDepthMask DepthMask;
  PFN_glDrawElements DrawElements;
  PFN_glActiveTexture ActiveTexture;
  PFN_glUniform1i Uniform1i;
  PFN_glGetUniformLocation GetUniformLocation;
  PFN_glBlitFramebuffer BlitFramebuffer;
} gl;

typedef __GLXextFuncPtr (*PFN_glXGetProcAddress)(const GLubyte *);
typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig,
                                                     GLXContext, Bool,
                                                     const int *);
typedef Bool (*PFN_glXQueryVersion)(Display *, int *, int *);
typedef const char *(*PFN_glXQueryExtensionsString)(Display *, int);
typedef GLXFBConfig *(*PFN_glXChooseFBConfig)(Display *, int, const int *,
                                              int *);
typedef XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(Display *, GLXFBConfig);
typedef Bool (*PFN_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
typedef void (*PFN_glXSwapBuffers)(Display *, GLXDrawable);
typedef void (*PFN_glXDestroyContext)(Display *, GLXContext);

static PFN_glXGetProcAddress p_getproc;
static PFN_glXQueryVersion x_glXQueryVersion;
static PFN_glXQueryExtensionsString x_glXQueryExtensionsString;
static PFN_glXChooseFBConfig x_glXChooseFBConfig;
static PFN_glXGetVisualFromFBConfig x_glXGetVisualFromFBConfig;
static PFN_glXMakeCurrent x_glXMakeCurrent;
static PFN_glXSwapBuffers x_glXSwapBuffers;
static PFN_glXDestroyContext x_glXDestroyContext;

/* Every glX* entry point comes out of the dlopen()ed DSO, never from the link
 * line: libGL.so.1 is not a DT_NEEDED of this program (it cannot be -- the
 * compat loader resolves libGL for Unity only through dlopen), and resolving
 * them by name is also the export-set check. */
static void *xsym(void *h, const char *name) {
  void *p = dlsym(h, name);
  if (!p) {
    say("[GL] MISSING export %s: %s\n", name, dlerror());
    g_fail = 1;
  }
  return p;
}

static void *gpa(const char *name) {
  void *p = p_getproc ? (void *)p_getproc((const GLubyte *)name) : NULL;
  if (!p) {
    say("[GL] MISSING entry point %s\n", name);
    g_fail = 1;
  }
  return p;
}

#define RESOLVE(field, name) (*(void **)(&gl.field) = gpa(name))

static int resolve_gl(void) {
  /* g_fail is process-wide (a refused 4.5 request sets it before the render
   * proof starts), so this function must answer for its own lookups only. */
  const int start = g_fail;
  RESOLVE(Clear, "glClear");
  RESOLVE(ClearColor, "glClearColor");
  RESOLVE(Viewport, "glViewport");
  RESOLVE(DrawArrays, "glDrawArrays");
  RESOLVE(ReadPixels, "glReadPixels");
  RESOLVE(Finish, "glFinish");
  RESOLVE(GetError, "glGetError");
  RESOLVE(GetString, "glGetString");
  RESOLVE(GetIntegerv, "glGetIntegerv");
  RESOLVE(GenVertexArrays, "glGenVertexArrays");
  RESOLVE(BindVertexArray, "glBindVertexArray");
  RESOLVE(GenBuffers, "glGenBuffers");
  RESOLVE(BindBuffer, "glBindBuffer");
  RESOLVE(BufferData, "glBufferData");
  RESOLVE(VertexAttribPointer, "glVertexAttribPointer");
  RESOLVE(EnableVertexAttribArray, "glEnableVertexAttribArray");
  RESOLVE(CreateShader, "glCreateShader");
  RESOLVE(ShaderSource, "glShaderSource");
  RESOLVE(CompileShader, "glCompileShader");
  RESOLVE(GetShaderiv, "glGetShaderiv");
  RESOLVE(GetShaderInfoLog, "glGetShaderInfoLog");
  RESOLVE(CreateProgram, "glCreateProgram");
  RESOLVE(AttachShader, "glAttachShader");
  RESOLVE(LinkProgram, "glLinkProgram");
  RESOLVE(GetProgramiv, "glGetProgramiv");
  RESOLVE(GetProgramInfoLog, "glGetProgramInfoLog");
  RESOLVE(UseProgram, "glUseProgram");
  RESOLVE(GenFramebuffers, "glGenFramebuffers");
  RESOLVE(BindFramebuffer, "glBindFramebuffer");
  RESOLVE(GenRenderbuffers, "glGenRenderbuffers");
  RESOLVE(BindRenderbuffer, "glBindRenderbuffer");
  RESOLVE(RenderbufferStorage, "glRenderbufferStorage");
  RESOLVE(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
  RESOLVE(CheckFramebufferStatus, "glCheckFramebufferStatus");
  RESOLVE(GetAttribLocation, "glGetAttribLocation");
  RESOLVE(GenTextures, "glGenTextures");
  RESOLVE(BindTexture, "glBindTexture");
  RESOLVE(TexParameteri, "glTexParameteri");
  RESOLVE(TexImage2D, "glTexImage2D");
  RESOLVE(FramebufferTexture2D, "glFramebufferTexture2D");
  RESOLVE(PixelStorei, "glPixelStorei");
  RESOLVE(TexSubImage2D, "glTexSubImage2D");
  RESOLVE(Enable, "glEnable");
  RESOLVE(Disable, "glDisable");
  RESOLVE(DepthFunc, "glDepthFunc");
  RESOLVE(DepthMask, "glDepthMask");
  RESOLVE(DrawElements, "glDrawElements");
  RESOLVE(ActiveTexture, "glActiveTexture");
  RESOLVE(Uniform1i, "glUniform1i");
  RESOLVE(GetUniformLocation, "glGetUniformLocation");
  RESOLVE(BlitFramebuffer, "glBlitFramebuffer");
  return g_fail != start ? -1 : 0;
}

/* ---- shaders (GLSL 3.30 core: #version 330 is the M4 target) ------------ */
static const char *VS_SRC =
    "#version 330 core\n"
    "in vec2 a_pos;\n"
    "in vec3 a_col;\n"
    "out vec3 v_col;\n"
    "void main() {\n"
    "  v_col = a_col;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in vec3 v_col;\n"
    "out vec4 o_col;\n"
    "void main() { o_col = vec4(v_col, 1.0); }\n";

static GLuint compile(GLenum type, const char *src, const char *label) {
  GLuint sh = gl.CreateShader(type);
  gl.ShaderSource(sh, 1, &src, NULL);
  gl.CompileShader(sh);
  GLint ok = 0;
  gl.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  char log[1024];
  GLsizei n = 0;
  log[0] = 0;
  gl.GetShaderInfoLog(sh, sizeof log, &n, log);
  say("[GL] %s compile: %s%s%s\n", label, ok ? "ok" : "FAILED",
      n > 0 ? " log=" : "", n > 0 ? log : "");
  if (!ok) g_fail = 1;
  return ok ? sh : 0;
}

/* ---- the readback census ------------------------------------------------ */
static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}

static void coverage_map(const unsigned char *px, int w, int h,
                         const unsigned char bg[4]) {
  /* 16x16 blocks: '#' mostly covered, '+' partly, '.' background.  Printed to
   * the serial log so the log itself carries the shape, not just a number. */
  int bw = w / 16, bh = h / 16;
  for (int by = 0; by < 16; by++) {
    char row[18];
    row[0] = '|';
    for (int bx = 0; bx < 16; bx++) {
      int hit = 0, total = 0;
      for (int y = by * bh; y < (by + 1) * bh; y += 2)
        for (int x = bx * bw; x < (bx + 1) * bw; x += 2) {
          const unsigned char *p = px + ((size_t)y * w + x) * 4;
          total++;
          if (memcmp(p, bg, 4) != 0) hit++;
        }
      row[1 + bx] = hit == total ? '#' : (hit * 4 > total ? '+' : '.');
    }
    row[17] = 0;
    say("[GL] %s\n", row);
  }
}

/* Print the process map once: when a fault address has to be attributed to a
 * DSO in the guest (there is no debugger), this is the only way to know which
 * one owns it. */
static void dump_maps(const char *tag) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    say("[GL] %s: /proc/self/maps unavailable\n", tag);
    return;
  }
  char line[256];
  say("[GL] %s: /proc/self/maps\n", tag);
  while (fgets(line, sizeof line, f)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) say("[GL]   %s\n", line);
  }
  fclose(f);
}

/* ---- context requests: Unity's exact attrib list, and the clamp ---------
 *
 * The game asks for **4.5 core** with its own attribute list (measured from
 * the apitrace capture, docs/rimworld-evidence.md §6):
 *
 *   {MAJOR,4, MINOR,5, PROFILE_MASK,CORE,
 *    RELEASE_BEHAVIOR,FLUSH, OPENGL_NO_ERROR,False, 0}
 *
 * and creates a second context that shares with the first.  Mesa's xlib GLX
 * frontend has two independent reasons to return NULL for that request: it
 * rejects attributes it does not know (BadValue), and its state tracker
 * refuses any version above the one the driver's core profile advertises
 * (GLXBadFBConfig).  Both are the *frontend's* policy, not the game's, so the
 * frontend we ship has to clamp the request to the best context it can serve
 * instead of refusing it (plan AD-5).  All of the probe's requests go through
 * this table so a rejection is always accompanied by the X error that caused
 * it -- generate_error() is silent when the client installed no handler.
 */
static PFN_glXCreateContextAttribsARB p_create_ctx;
static int g_xerr;

static int on_xerror(Display *dpy, XErrorEvent *ev) {
  (void)dpy;
  g_xerr = ev->error_code;
  return 0;
}

static const char *xerr_name(int code) {
  /* GLX errors are offset by the extension's base (0 in the compat shim: its
   * XQueryExtension knows no GLX extension), so 9 is GLXBadFBConfig. */
  switch (code) {
    case 0: return "";
    case 2: return " (BadValue)";
    case 8: return " (BadMatch)";
    case 9: return " (GLXBadFBConfig)";
    default: return "";
  }
}

static GLXContext request_ctx(Display *dpy, GLXFBConfig fbc, GLXContext share,
                              const char *label, const int *attribs) {
  g_xerr = 0;
  GLXContext c = p_create_ctx ? p_create_ctx(dpy, fbc, share, True, attribs)
                              : NULL;
  say("[GL] ctx request %-18s -> ctx=%p X-error=0x%x%s\n", label, (void *)c,
      g_xerr, xerr_name(g_xerr));
  return c;
}

/* The version a context actually reports, through glXMakeCurrent: this is the
 * only row that proves a request was *served* rather than merely accepted. */
static const char *ctx_version(Display *dpy, Window win, GLXContext c) {
  if (!c) return "(no context)";
  if (!x_glXMakeCurrent(dpy, win, c)) return "(glXMakeCurrent failed)";
  PFN_glGetString gs = (PFN_glGetString)gpa("glGetString");
  const GLubyte *v = gs ? gs(GL_VERSION) : NULL;
  return v ? (const char *)v : "(glGetString returned NULL)";
}

/* ---- per-level texture upload probe (RimWorld's planet-ramp shape) ------
 * The colony's map renders as black wedges while the HUD is crisp and
 * Player.log carries no GL errors, and the world map's planet ramp is an
 * 8192x4096 RGBA8 texture with last_level=4.  The probe uploads a different
 * solid colour into EVERY mip level, attaches each level to the FBO and reads
 * one pixel back.  If every level returns its own colour, per-level content
 * arrival is correct and the colony fault is in draw coverage; if levels
 * 1..n come back zero or with level 0's colour, the mip upload path is the
 * bug.  This runs in seconds inside the M4 gate, instead of a 35-minute
 * RimWorld run per hypothesis. */
static int mip_probe_one(const char *tag, int w0, int h0, int nlev) {
  static const unsigned char palette[6][4] = {
      {255, 0, 0, 255},   {0, 255, 0, 255},   {0, 0, 255, 255},
      {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}};
  int bad = 0;
  GLuint tex = 0;
  gl.GenTextures(1, &tex);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nlev - 1);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                   GL_NEAREST_MIPMAP_NEAREST);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  /* Mesa's rule (fbobject.c:test_attachment_completeness): a *mutable non
   * base level* texture attached to an FBO must be mipmap complete, so the
   * uploads all have to happen before any level is attached -- attaching
   * level 1 while levels 2+ are still missing legitimately returns
   * INCOMPLETE_ATTACHMENT.  That is a property of the probe, not of the
   * stack, and it is exactly the interleave that would have been read as a
   * mip bug. */
  for (int l = 0; l < nlev; l++) {
    int w = w0 >> l, h = h0 >> l;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    size_t n = (size_t)w * h * 4;
    unsigned char *buf = (unsigned char *)malloc(n);
    if (!buf) {
      say("[RAMP] %s lvl=%d: malloc(%zu) failed\n", tag, l, n);
      return 1;
    }
    for (size_t i = 0; i < n; i += 4)
      memcpy(buf + i, palette[l % 6], 4);
    gl.TexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, buf);
    free(buf);
    GLenum e1 = gl.GetError();
    say("[RAMP] %s upload lvl=%d %dx%d err=0x%x\n", tag, l, w, h, e1);
  }
  for (int l = 0; l < nlev; l++) {
    int w = w0 >> l, h = h0 >> l;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            tex, l);
    GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    unsigned char px[4] = {0x77, 0x77, 0x77, 0x77};
    gl.ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl.Finish();
    GLenum e2 = gl.GetError();
    int ok = st == GL_FRAMEBUFFER_COMPLETE && px[0] == palette[l % 6][0] &&
             px[1] == palette[l % 6][1] && px[2] == palette[l % 6][2] &&
             px[3] == palette[l % 6][3];
    say("[RAMP] %s lvl=%d %dx%d status=0x%x rd_err=0x%x "
        "px=%u,%u,%u,%u expect=%u,%u,%u,%u %s\n",
        tag, l, w, h, st, e2, px[0], px[1], px[2], px[3],
        palette[l % 6][0], palette[l % 6][1], palette[l % 6][2],
        palette[l % 6][3], ok ? "OK" : "FAIL");
    if (!ok) bad++;
  }
  return bad;
}

/* ---- glTexSubImage2D atlas-region upload (Unity's sprite-atlas path) -----
 * Full-level uploads all arrive intact (the [RAMP] rows pass even for the
 * 8192x4096 five-level planet ramp), so the colony's black tile-shaped wedges
 * have to come from the way sprites are written INTO a texture, not from the
 * texture's own allocation.  This writes a 16x16 cell out of a much larger
 * source atlas with UNPACK_ROW_LENGTH set to the atlas width -- the exact
 * case whose row stride differs from the destination box's width -- and then
 * reads the whole 256x256 destination back and verifies every cell. */
static GLuint g_atlas_tex = 0;
static int atlas_probe(void) {
  enum { TW = 256, TH = 256, CELL = 16, GRID = TW / CELL, SW = 512 };
  unsigned char *atlas = (unsigned char *)malloc((size_t)SW * SW * 4);
  unsigned char *zero = (unsigned char *)calloc((size_t)TW * TH, 4);
  unsigned char *back = (unsigned char *)malloc((size_t)TW * TH * 4);
  if (!atlas || !zero || !back) {
    say("[ATLAS] allocation failed\n");
    return 1;
  }
  for (int j = 0; j < GRID; j++)
    for (int i = 0; i < GRID; i++) {
      unsigned char col[4] = {(unsigned char)(i * 16),
                              (unsigned char)(j * 16),
                              (unsigned char)(((i + j) * 8) & 0xff), 255};
      for (int r = 0; r < CELL; r++)
        for (int c = 0; c < CELL; c++)
          memcpy(atlas + (((size_t)(j * CELL + r) * SW) + i * CELL + c) * 4,
                 col, 4);
    }
  GLuint tex = 0;
  gl.GenTextures(1, &tex);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TW, TH, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, zero);
  g_atlas_tex = tex;
  GLenum first_err = gl.GetError();
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, SW);
  int bad = 0;
  for (int j = 0; j < GRID; j++)
    for (int i = 0; i < GRID; i++) {
      const unsigned char *src =
          atlas + (((size_t)(j * CELL) * SW) + (size_t)(i * CELL)) * 4;
      gl.TexSubImage2D(GL_TEXTURE_2D, 0, i * CELL, j * CELL, CELL, CELL,
                       GL_RGBA, GL_UNSIGNED_BYTE, src);
      GLenum e = gl.GetError();
      if (e != GL_NO_ERROR) {
        if (bad < 4)
          say("[ATLAS] upload cell(%d,%d) glError=0x%x\n", i, j, e);
        bad++;
      }
    }
  gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          tex, 0);
  GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  gl.ReadPixels(0, 0, TW, TH, GL_RGBA, GL_UNSIGNED_BYTE, back);
  gl.Finish();
  int mismatch = 0, shown = 0;
  for (int j = 0; j < GRID && mismatch >= 0; j++)
    for (int i = 0; i < GRID; i++) {
      unsigned char want[4] = {(unsigned char)(i * 16),
                               (unsigned char)(j * 16),
                               (unsigned char)(((i + j) * 8) & 0xff), 255};
      size_t off = ((size_t)(j * CELL) * TW + (size_t)(i * CELL)) * 4;
      if (memcmp(back + off, want, 4) != 0) {
        mismatch++;
        if (shown < 6) {
          say("[ATLAS] cell(%d,%d) dst=(%u,%u,%u,%u) want=(%u,%u,%u,%u)\n", i,
              j, back[off], back[off + 1], back[off + 2], back[off + 3], want[0],
              want[1], want[2], want[3]);
          shown++;
        }
      }
    }
  say("[ATLAS] 16x16 grid: uploads_err=%d readback fbstatus=0x%x "
      "first_err=0x%x mismatched_cells=%d/%d %s\n",
      bad, st, first_err, mismatch, GRID * GRID,
      mismatch == 0 ? "OK" : "FAIL");
  free(atlas);
  free(zero);
  free(back);
  return (bad || mismatch) ? 1 : 0;
}

/* ---- textured, indexed, grid-of-quads pass (RimWorld's map pass) --------
 * glDrawElements rather than glDrawArrays, interleaved vertex attributes and
 * an atlas texture bound to a sampler: the primitives whose output is corrupt
 * in the colony.  256 quads cover the 256x256 FBO as a 16x16 grid, each cell
 * sampling the atlas cell with the matching colour.  Any tile that comes out
 * black, shifted or wrong implicates the drawing or the sampling path, which
 * the two upload probes above have already cleared. */
static const char *TEX_VS =
    "#version 330 core\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
static const char *TEX_FS =
    "#version 330 core\n"
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_col;\n"
    "void main() { o_col = texture(u_tex, v_uv); }\n";

static int tile_mesh_probe(GLuint tex, int show_depth, int grid)
{
  enum { W = 256, H = 256 };
  const int GRID = grid, CELL = W / GRID;
  GLuint vs = compile(GL_VERTEX_SHADER, TEX_VS, "tile-vs");
  GLuint fs = compile(GL_FRAGMENT_SHADER, TEX_FS, "tile-fs");
  if (!vs || !fs) return 1;
  GLuint prog = gl.CreateProgram();
  gl.AttachShader(prog, vs);
  gl.AttachShader(prog, fs);
  gl.LinkProgram(prog);
  GLint ok = 0;
  gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    say("[TILE] link failed\n");
    return 1;
  }
  gl.UseProgram(prog);

  float vx[GRID * GRID * 4 * 4];
  unsigned short idx[GRID * GRID * 6];
  int nv = 0, ni = 0;
  for (int j = 0; j < GRID; j++)
    for (int i = 0; i < GRID; i++) {
      float x0 = -1.0f + 2.0f * i / GRID, x1 = -1.0f + 2.0f * (i + 1) / GRID;
      float y0 = -1.0f + 2.0f * j / GRID, y1 = -1.0f + 2.0f * (j + 1) / GRID;
      float u0 = (float)i / GRID, u1 = (float)(i + 1) / GRID;
      float v0 = (float)j / GRID, v1 = (float)(j + 1) / GRID;
      unsigned short base = (unsigned short)(nv / 4);
      float quad[4][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0},
                          {x0, y1, u0, v1}, {x1, y1, u1, v1}};
      for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++) vx[nv++] = quad[k][c];
      unsigned short tri[6] = {(unsigned short)(base + 0),
                               (unsigned short)(base + 1),
                               (unsigned short)(base + 2),
                               (unsigned short)(base + 2),
                               (unsigned short)(base + 1),
                               (unsigned short)(base + 3)};
      for (int k = 0; k < 6; k++) idx[ni++] = tri[k];
    }

  GLuint vao = 0, vbo = 0, ibo = 0;
  gl.GenVertexArrays(1, &vao);
  gl.BindVertexArray(vao);
  gl.GenBuffers(1, &vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof vx, vx, GL_STATIC_DRAW);
  gl.GenBuffers(1, &ibo);
  gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
  GLint a_pos = gl.GetAttribLocation(prog, "a_pos");
  GLint a_uv = gl.GetAttribLocation(prog, "a_uv");
  gl.EnableVertexAttribArray((GLuint)a_pos);
  gl.VertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), (const void *)0);
  gl.EnableVertexAttribArray((GLuint)a_uv);
  gl.VertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), (const void *)(2 * sizeof(float)));

  GLint uloc = gl.GetUniformLocation(prog, "u_tex");
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.Uniform1i(uloc, 0);
  gl.Viewport(0, 0, W, H);
  gl.ClearColor(0, 0, 0, 1);
  gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (show_depth) {
    gl.Enable(GL_DEPTH_TEST);
    gl.DepthFunc(GL_LEQUAL);
    gl.DepthMask(GL_TRUE);
  } else {
    gl.Disable(GL_DEPTH_TEST);
  }
  gl.DrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, 0);
  gl.Finish();
  GLenum de = gl.GetError();

  unsigned char *back = (unsigned char *)malloc((size_t)W * H * 4);
  gl.ReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, back);
  gl.Finish();
  int bad = 0, shown = 0;
  for (int j = 0; j < GRID; j++)
    for (int i = 0; i < GRID; i++) {
      unsigned char want[4] = {(unsigned char)(i * 16),
                               (unsigned char)(j * 16),
                               (unsigned char)(((i + j) * 8) & 0xff), 255};
      size_t off = ((size_t)(j * CELL + CELL / 2) * W + i * CELL + CELL / 2) * 4;
      if (memcmp(back + off, want, 4) != 0) {
        bad++;
        if (shown < 6) {
          say("[TILE] cell(%d,%d) got=%u,%u,%u,%u want=%u,%u,%u,%u\n", i, j,
              back[off], back[off + 1], back[off + 2], back[off + 3], want[0],
              want[1], want[2], want[3]);
          shown++;
        }
      }
    }
  say("[TILE] grid indexed quads depth=%d grid=%d vbytes=%zu ibytes=%zu "
      "draw_err=0x%x %d/%d cells wrong %s\n",
      show_depth, GRID, sizeof vx, sizeof idx, de, bad, GRID * GRID,
      bad == 0 ? "OK" : "FAIL");
  free(back);
  return bad ? 1 : 0;
}

/* ---- shader sampling of every mip level (not just FBO attachment) -------
 * Upload and FBO-readback of each level is proven intact, but that is not the
 * path Unity's planet/map pass uses: those sample levels through a shader
 * sampler, which resolves base/max level at the host.  Force base==max=l, draw
 * a full-screen textured triangle and read one pixel; the only colour it can
 * come out is the one uploaded into level l.  Any other answer pins the fault
 * to sampler-level selection rather than to transfer or rasterisation. */
static int lod_sample_probe(const char *tag, int w0, int h0, int nlev)
{
  static const unsigned char palette[6][4] = {
      {255, 0, 0, 255},   {0, 255, 0, 255},   {0, 0, 255, 255},
      {255, 255, 0, 255}, {255, 0, 255, 255}, {0, 255, 255, 255}};
  GLuint tex = 0;
  gl.GenTextures(1, &tex);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, nlev - 1);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                   GL_NEAREST_MIPMAP_NEAREST);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  for (int l = 0; l < nlev; l++) {
    int w = w0 >> l, h = h0 >> l;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    size_t n = (size_t)w * h * 4;
    unsigned char *buf = (unsigned char *)malloc(n);
    if (!buf) return 1;
    for (size_t i = 0; i < n; i += 4) memcpy(buf + i, palette[l % 6], 4);
    gl.TexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, buf);
    free(buf);
  }

  GLuint vs = compile(GL_VERTEX_SHADER, TEX_VS, "lod-vs");
  GLuint fs = compile(GL_FRAGMENT_SHADER, TEX_FS, "lod-fs");
  if (!vs || !fs) return 1;
  GLuint prog = gl.CreateProgram();
  gl.AttachShader(prog, vs);
  gl.AttachShader(prog, fs);
  gl.LinkProgram(prog);
  GLint ok = 0;
  gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) return 1;
  gl.UseProgram(prog);
  GLint uloc = gl.GetUniformLocation(prog, "u_tex");
  GLint a_pos = gl.GetAttribLocation(prog, "a_pos");
  GLint a_uv = gl.GetAttribLocation(prog, "a_uv");

  static const GLfloat tri[] = {-1, -1, 0, 0, 3, -1, 2, 0, -1, 3, 0, 2};
  GLuint vao = 0, vbo = 0;
  gl.GenVertexArrays(1, &vao);
  gl.BindVertexArray(vao);
  gl.GenBuffers(1, &vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
  gl.EnableVertexAttribArray((GLuint)a_pos);
  gl.VertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), (const void *)0);
  gl.EnableVertexAttribArray((GLuint)a_uv);
  gl.VertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), (const void *)(2 * sizeof(float)));
  gl.ActiveTexture(GL_TEXTURE0);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.Uniform1i(uloc, 0);
  gl.Disable(GL_DEPTH_TEST);
  gl.ClearColor(0x11 / 255.0f, 0x11 / 255.0f, 0x11 / 255.0f, 1);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  int bad = 0;
  for (int l = 0; l < nlev; l++) {
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, l);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, l);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.Finish();
    unsigned char px[4] = {0x77, 0x77, 0x77, 0x77};
    gl.ReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    gl.Finish();
    int good = px[0] == palette[l % 6][0] && px[1] == palette[l % 6][1] &&
               px[2] == palette[l % 6][2] && px[3] == palette[l % 6][3];
    say("[LOD] %s lvl=%d sample px=%u,%u,%u,%u expect=%u,%u,%u,%u %s\n", tag,
        l, px[0], px[1], px[2], px[3], palette[l % 6][0], palette[l % 6][1],
        palette[l % 6][2], palette[l % 6][3], good ? "OK" : "FAIL");
    if (!good) bad++;
  }
  return bad;
}

/* ---- FBO -> FBO blit (the offscreen-map composite path) ------------------
 * Every upload, FBO attachment, sampler read and indexed draw pass above is
 * clean.  What none of them exercises is the composite step Unity uses to put
 * an offscreen-rendered world into the visible frame: glBlitFramebuffer with
 * different source and destination sizes.  Blit a known source colour into a
 * differently sized destination, then read the destination back -- once at
 * same size, and once from an 8192x4096 source (the planet pass's size) down
 * to a 1280x720 window-sized target. */
static int blit_probe_one(const char *tag, int sw, int sh, int dw, int dh)
{
  static const unsigned char col[4] = {37, 173, 91, 255};
  unsigned char *fill = (unsigned char *)malloc((size_t)sw * sh * 4);
  if (!fill) return 1;
  for (size_t i = 0; i < (size_t)sw * sh * 4; i += 4) memcpy(fill + i, col, 4);
  GLuint tex = 0;
  gl.GenTextures(1, &tex);
  gl.BindTexture(GL_TEXTURE_2D, tex);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, fill);
  free(fill);
  GLuint srcfbo = 0, dstfbo = 0, drbo = 0;
  gl.GenFramebuffers(1, &srcfbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, srcfbo);
  gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                          tex, 0);
  gl.GenFramebuffers(1, &dstfbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, dstfbo);
  gl.GenRenderbuffers(1, &drbo);
  gl.BindRenderbuffer(GL_RENDERBUFFER, drbo);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, dw, dh);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, drbo);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, srcfbo);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dstfbo);
  gl.BlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT,
                     GL_NEAREST);
  gl.BindFramebuffer(GL_FRAMEBUFFER, dstfbo);
  gl.Finish();
  GLsizei samplew = dw < 32 ? dw : 32, sampleh = dh < 32 ? dh : 32;
  unsigned char *back = malloc((size_t)samplew * sampleh * 4);
  gl.ReadPixels(0, 0, samplew, sampleh, GL_RGBA, GL_UNSIGNED_BYTE, back);
  gl.Finish();
  GLenum err = gl.GetError();
  int bad = 0;
  for (int y = 0; y < sampleh; y++)
    for (int x = 0; x < samplew; x++) {
      size_t off = ((size_t)y * samplew + x) * 4;
      if (memcmp(back + off, col, 4) != 0) {
        if (bad < 3)
          say("[BLIT] %s got=%u,%u,%u,%u at (%d,%d) expect=%u,%u,%u,%u\n", tag,
              back[off], back[off + 1], back[off + 2], back[off + 3], x, y,
              col[0], col[1], col[2], col[3]);
        bad++;
      }
    }
  say("[BLIT] %s %dx%d -> %dx%d err=0x%x bad=%d/%d %s\n", tag, sw, sh, dw, dh,
      err, bad, samplew * sampleh, bad == 0 ? "OK" : "FAIL");
  free(back);
  return bad ? 1 : 0;
}

/* ---- direct byte-wide TRANSFER contract ---------------------------------
 * This tests the ABI itself, not an indirect GL effect.  A PIPE_BUFFER's
 * TRANSFER box.width is the number of BYTEs to move.  A legal multi-megabyte
 * buffer upload used to be rejected by the /dev/gpu ioctl layer as if it were
 * a texture wider than 16384 pixels, leaving its host resource unmodified.
 * (The kernel cap is 512 MiB; the virgl host has its own 1-D transfer limit,
 * so the gate uses 8 MiB -- far above the old 16 KiB bug and below that host
 * limit.)  Do the transfer both ways and compare every byte. */
static int abi_buffer_transfer_probe(void)
{
  const size_t bytes = (size_t)8 * 1024 * 1024;
  int fd = -1, bad = 0;
  unsigned char *page = NULL;
  struct gpu_ctx_create cc;
  struct gpu_ctx_destroy cd;
  memset(&cc, 0, sizeof cc);
  fd = open("/dev/gpu", O_RDWR);
  if (fd < 0) {
    say("[ABIX] skipped: /dev/gpu unavailable\n");
    return 0;
  }
  if (ioctl(fd, GPU_IOC_GET_CAPSET_INFO, &(struct gpu_capset_info){0}) == 0) {
    /* The subsequent create asks for the capset explicitly below. */
  }
  cc.capset_id = 1;
  cc.capset_version = 1;
  const char *name = "byte-wide-transfer";
  snprintf(cc.name, sizeof cc.name, "%s", name);
  if (ioctl(fd, GPU_IOC_CTX_CREATE, &cc) != 0) {
    say("[ABIX] ctx create failed errno=%d\n", errno);
    close(fd);
    return 1;
  }
  struct gpu_res_create rc;
  memset(&rc, 0, sizeof rc);
  rc.ctx_id = cc.ctx_id;
  rc.target = 0;        /* VIRGL_TEXTURE_TARGET_BUFFER */
  rc.format = 1;        /* PIPE_FORMAT_R8_UNORM */
  rc.bind = 0;
  rc.width = (uint32_t)bytes;
  rc.height = 1;
  rc.depth = 1;
  rc.array_size = 1;
  if (ioctl(fd, GPU_IOC_RES_CREATE, &rc) != 0) {
    say("[ABIX] buffer create failed errno=%d\n", errno);
    bad = 1;
  }
  if (!bad && posix_memalign((void **)&page, 4096, bytes) != 0) bad = 1;
  if (!bad) {
    for (size_t i = 0; i < bytes; i++) page[i] = (unsigned char)(i * 13);
    struct gpu_res_attach ra;
    memset(&ra, 0, sizeof ra);
    ra.res_id = rc.res_id;
    ra.va = (uint64_t)(uintptr_t)page;
    ra.len = bytes;
    if (ioctl(fd, GPU_IOC_RES_ATTACH, &ra) != 0) {
      say("[ABIX] attach failed errno=%d\n", errno);
      bad = 1;
    }
  }
  if (!bad) {
    struct gpu_transfer t;
    memset(&t, 0, sizeof t);
    t.res_id = rc.res_id;
    t.to_host = 0;
    t.stride = (uint32_t)bytes;
    t.box.w = (uint32_t)bytes;
    t.box.h = 1;
    t.box.d = 1;
    errno = 0;
    int up = ioctl(fd, GPU_IOC_TRANSFER, &t);
    say("[ABIX] byte-wide upload w=%u rc=%d errno=%d\n", t.box.w, up, errno);
    if (up != 0) {
      bad = 1;
    } else {
      for (size_t i = 0; i < bytes; i++) page[i] = 0;
      t.to_host = 1;
      errno = 0;
      int down = ioctl(fd, GPU_IOC_TRANSFER, &t);
      say("[ABIX] byte-wide readback rc=%d errno=%d\n", down, errno);
      if (down != 0) {
        bad = 1;
      } else {
        for (size_t i = 0; i < bytes; i++) {
          if (page[i] != (unsigned char)(i * 13)) {
            say("[ABIX] mismatch at %zu got=%u want=%u\n", i, page[i],
                (unsigned)(i * 13) & 0xff);
            bad = 1;
            break;
          }
        }
      }
    }
    say("[ABIX] %zu MiB buffer transfer round trip %s\n", bytes / (1024 * 1024),
        bad ? "FAIL" : "OK (every byte)");
  }
  free(page);
  if (rc.res_id) {
    struct gpu_res_unref ru = {rc.res_id, 0};
    ioctl(fd, GPU_IOC_RES_UNREF, &ru);
  }
  memset(&cd, 0, sizeof cd);
  cd.ctx_id = cc.ctx_id;
  ioctl(fd, GPU_IOC_CTX_DESTROY, &cd);
  close(fd);
  return bad;
}

#define GLX_CONTEXT_OPENGL_NO_ERROR_ARB_LOCAL 0x31B3
#ifndef GLX_CONTEXT_OPENGL_NO_ERROR_ARB
#define GLX_CONTEXT_OPENGL_NO_ERROR_ARB GLX_CONTEXT_OPENGL_NO_ERROR_ARB_LOCAL
#endif


int main(void) {
  say("[GL] probe start (pid %d)\n", (int)getpid());

  {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
  }

  /* 0. The display first: libGL's GLX frontend talks to it through libX11,
   *    and the probe's DT_NEEDED libX11.so.6 is what puts Xlib in scope. */
  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    say("[GL] FAIL XOpenDisplay(NULL)\n");
    return 2;
  }
  say("[GL] XOpenDisplay ok\n");

  /* Isolate the compat Xlib's XImage before Mesa gets to it: the GLX frontend
   * asserts on a non-NULL XImage and later writes its data/width/bytes_per_line
   * fields, so a broken XCreateImage must be diagnosed here, not three frames
   * deep inside glXChooseFBConfig. */
  {
    XImage *im = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                              2 /* ZPixmap */, 0, NULL, 8, 4, 32, 0);
    if (!im)
      say("[GL] XCreateImage smoke: NULL (broken compat Xlib)\n");
    else
      say("[GL] XCreateImage smoke: img=%p data=%p bpl=%d bpp=%d fmt=%d unit=%d\n",
          (void *)im, (void *)im->data, im->bytes_per_line, im->bits_per_pixel,
          im->format, im->bitmap_unit);
  }

  /* The Xext subset Mesa's sw/xlib winsys references (XShmCreateImage and
   * friends).  Mesa links libXext in a normal build; here libGL has no
   * DT_NEEDED, so those names must come from the process.  On a glibc host
   * that means dlopen("libXext.so.6") -- exactly what an SDL app does.  The
   * compat class needs nothing: its libX11.so.6 already exports the XShm*
   * surface (the M3 export list), so a missing libXext.so.6 is not an error. */
  {
    void *he = dlopen("libXext.so.6", RTLD_NOW | RTLD_GLOBAL);
    say("[GL] libXext.so.6: %s\n", he ? "loaded" : "absent (Xext comes from libX11)");
  }

  /* 1. dlopen.  RTLD_NOW: resolve everything now, so a missing symbol is a
   *    reported dlerror() and not a crash inside a GL call.  RTLD_GLOBAL:
   *    what SDL/Unity do, and what lets libGL's gl* entries stay findable. */
  void *h = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!h) {
    say("[GL] FAIL dlopen(libGL.so.1, RTLD_NOW|RTLD_GLOBAL): %s\n", dlerror());
    return 2;
  }
  Dl_info di;
  if (dladdr(h, &di) && di.dli_fname)
    say("[GL] dlopen ok: libGL.so.1 -> %s @ %p\n", di.dli_fname, h);
  else
    say("[GL] dlopen ok: libGL.so.1 @ %p\n", h);

  p_getproc = (PFN_glXGetProcAddress)dlsym(h, "glXGetProcAddress");
  if (!p_getproc) p_getproc = (PFN_glXGetProcAddress)dlsym(h, "glXGetProcAddressARB");
  if (!p_getproc) {
    say("[GL] FAIL dlsym(glXGetProcAddress): %s\n", dlerror());
    return 2;
  }
  say("[GL] glXGetProcAddress @ %p\n", (void *)p_getproc);

  x_glXQueryVersion = (PFN_glXQueryVersion)xsym(h, "glXQueryVersion");
  x_glXQueryExtensionsString =
      (PFN_glXQueryExtensionsString)xsym(h, "glXQueryExtensionsString");
  x_glXChooseFBConfig = (PFN_glXChooseFBConfig)xsym(h, "glXChooseFBConfig");
  x_glXGetVisualFromFBConfig =
      (PFN_glXGetVisualFromFBConfig)xsym(h, "glXGetVisualFromFBConfig");
  x_glXMakeCurrent = (PFN_glXMakeCurrent)xsym(h, "glXMakeCurrent");
  x_glXSwapBuffers = (PFN_glXSwapBuffers)xsym(h, "glXSwapBuffers");
  x_glXDestroyContext = (PFN_glXDestroyContext)xsym(h, "glXDestroyContext");
  if (g_fail) {
    say("[GL] FAIL the glX export set is incomplete\n");
    return 2;
  }
  say("[GL] glX exports resolved: query/extensions/fbconfig/visual/makecurrent/"
      "swap/destroy\n");

  p_create_ctx =
      (PFN_glXCreateContextAttribsARB)gpa("glXCreateContextAttribsARB");
  if (!p_create_ctx) {
    say("[GL] FAIL glXCreateContextAttribsARB is not resolvable\n");
    return 2;
  }
  say("[GL] glXCreateContextAttribsARB @ %p\n", (void *)p_create_ctx);

  /* A rejected context request is reported through Xlib's error handler, and
   * the frontend's generate_error() does nothing at all when the client has
   * none installed (measured: Mesa's glx_api.c calls XSetErrorHandler(NULL),
   * sees NULL and returns).  Install one so "NULL" always comes with a code. */
  XSetErrorHandler(on_xerror);

  /* 2. GLX version + extensions, now that the display exists. */
  int glx_major = 0, glx_minor = 0;
  int have_glx = x_glXQueryVersion(dpy, &glx_major, &glx_minor);
  say("[GL] XOpenDisplay ok; glXQueryVersion=%d %d.%d\n", have_glx, glx_major,
      glx_minor);
  const char *ext = x_glXQueryExtensionsString(dpy, DefaultScreen(dpy));
  say("[GL] GLX extensions: %s\n", ext ? ext : "(none)");

  /* 3. an RGBA8/depth24 double-buffered window config. */
  int attribs[] = {GLX_X_RENDERABLE,  True,
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
  say("[GL] calling glXChooseFBConfig\n");
  dump_link_map();
  GLXFBConfig *cfgs = x_glXChooseFBConfig(dpy, DefaultScreen(dpy), attribs, &ncfg);
  say("[GL] glXChooseFBConfig returned %p, n=%d\n", (void *)cfgs, ncfg);
  if (!cfgs || ncfg < 1) {
    say("[GL] FAIL glXChooseFBConfig: %d configs\n", ncfg);
    return 3;
  }
  say("[GL] glXChooseFBConfig: %d configs, using [0]\n", ncfg);
  GLXFBConfig fbc = cfgs[0];

  XVisualInfo *vi = x_glXGetVisualFromFBConfig(dpy, fbc);
  if (!vi) {
    say("[GL] FAIL glXGetVisualFromFBConfig\n");
    return 3;
  }
  say("[GL] visual 0x%lx depth=%d class=%d\n", (unsigned long)vi->visualid,
      vi->depth, vi->class);

  Window root = RootWindow(dpy, vi->screen);
  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof swa);
  swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
  swa.border_pixel = 0;
  swa.event_mask = ExposureMask | StructureNotifyMask;
  Window win = XCreateWindow(dpy, root, 0, 0, 320, 240, 0, vi->depth,
                             InputOutput, vi->visual,
                             CWBorderPixel | CWColormap | CWEventMask, &swa);
  if (!win) {
    say("[GL] FAIL XCreateWindow\n");
    return 3;
  }
  XStoreName(dpy, win, "GLPROBE");
  XMapWindow(dpy, win);
  XSync(dpy, False);
  say("[GL] window 0x%lx mapped (320x240 requested)\n", (unsigned long)win);

  /* A real X client does not get to paint the instant it maps: the desktop
   * answers with the content rect (the protocol's geometry message), and the
   * shim's XPutImage refuses to blit before it has one.  Wait for it the way
   * the M3 client does -- unbounded painting into a window the desktop has not
   * laid out yet silently drops every pixel. */
  {
    long deadline = now_ms() + 5000;
    int got = 0;
    while (now_ms() < deadline && !got) {
      while (XPending(dpy)) {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        XNextEvent(dpy, &ev);
        if (ev.type == Expose || ev.type == ConfigureNotify) {
          got = 1;
          say("[GL] %s %dx%d at %d,%d\n",
              ev.type == Expose ? "Expose" : "ConfigureNotify",
              ev.type == Expose ? ev.xexpose.width : ev.xconfigure.width,
              ev.type == Expose ? ev.xexpose.height : ev.xconfigure.height,
              ev.type == Expose ? ev.xexpose.x : ev.xconfigure.x,
              ev.type == Expose ? ev.xexpose.y : ev.xconfigure.y);
        }
      }
      if (!got) usleep(10000);
    }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, win, &wa)) {
      say("[GL] FAIL XGetWindowAttributes after map\n");
      return 3;
    }
    say("[GL] window content rect x=%d y=%d %dx%d (laid out=%d)\n", wa.x, wa.y,
        wa.width, wa.height, got);
    if (!got) {
      say("[GL] FAIL the desktop never reported the window's content rect\n");
      return 3;
    }
  }
  XWindowAttributes wa;
  XGetWindowAttributes(dpy, win, &wa);
  const int WIN_W = wa.width, WIN_H = wa.height;

  /* 4. The context-version matrix.  Unity asks for 4.5 core with its own
   *    attribute list and creates a second context that shares with it; the
   *    clamp we ship (tools/mesa/patches/0004) must let both through and hand
   *    back the best context the driver has -- 3.3 core on softpipe.  A 3.3
   *    request must still be passed through unchanged, and a 2.1 request must
   *    be honoured or refused cleanly (never a hang, never a bogus context).
   *
   *    Order matters for the evidence: the 4.5 row is printed first, so a
   *    tree without the clamp shows the refusal before anything else. */
  int unity45[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
                   GLX_CONTEXT_MINOR_VERSION_ARB, 5,
                   GLX_CONTEXT_PROFILE_MASK_ARB,
                   GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                   GLX_CONTEXT_RELEASE_BEHAVIOR_ARB,
                   GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB,
                   GLX_CONTEXT_OPENGL_NO_ERROR_ARB, False, None};
  int plain45[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
                   GLX_CONTEXT_MINOR_VERSION_ARB, 5,
                   GLX_CONTEXT_PROFILE_MASK_ARB,
                   GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                   GLX_CONTEXT_FLAGS_ARB, 0, None};
  int core33[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                  GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                  GLX_CONTEXT_PROFILE_MASK_ARB,
                  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                  GLX_CONTEXT_FLAGS_ARB, 0, None};
  int compat21[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                    GLX_CONTEXT_MINOR_VERSION_ARB, 1,
                    GLX_CONTEXT_PROFILE_MASK_ARB,
                    GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, None};

  GLXContext c45 = request_ctx(dpy, fbc, NULL, "unity-4.5-core", unity45);
  GLXContext cp45 = request_ctx(dpy, fbc, NULL, "plain-4.5-core", plain45);
  GLXContext c33 = request_ctx(dpy, fbc, NULL, "core-3.3", core33);
  GLXContext c21 = request_ctx(dpy, fbc, NULL, "compat-2.1", compat21);
  GLXContext c45s = c45 ? request_ctx(dpy, fbc, c45, "unity-4.5-shared",
                                     unity45)
                        : NULL;

  /* What each accepted request was actually served.  A context that exists
   * but cannot be made current is not a context. */
  if (c45) say("[GL] ctx served unity-4.5-core : %s\n", ctx_version(dpy, win, c45));
  if (c45s) say("[GL] ctx served unity-4.5-shared: %s\n", ctx_version(dpy, win, c45s));
  if (c33) say("[GL] ctx served core-3.3        : %s\n", ctx_version(dpy, win, c33));
  if (c21) say("[GL] ctx served compat-2.1      : %s\n", ctx_version(dpy, win, c21));
  if (!c33) {
    say("[GL] FAIL a plain 3.3 core request must still work\n");
    g_fail = 1;
  }
  /* The 4.5 request is the lane's whole point: it must not be refused. */
  if (!c45) {
    say("[GL] FAIL the 4.5 core request returned NULL -- the GLX frontend "
        "must clamp it to the best context it can serve (AD-5)\n");
    g_fail = 1;
  }

  /* Pick the one context the render/present proof runs on -- the 4.5 request
   * when it was served, otherwise the 3.3 one -- then drop the current binding
   * and every context we are not using. */
  GLXContext ctx = c45 ? c45 : c33;
  if (!ctx) {
    say("[GL] FAIL no usable GLX context at all\n");
    return 4;
  }
  x_glXMakeCurrent(dpy, None, NULL);
  if (cp45 && cp45 != ctx) x_glXDestroyContext(dpy, cp45);
  if (c45s && c45s != ctx) x_glXDestroyContext(dpy, c45s);
  if (c33 && c33 != ctx) x_glXDestroyContext(dpy, c33);
  if (c21 && c21 != ctx) x_glXDestroyContext(dpy, c21);

  say("[GL] using %s context -> %p\n",
      c45 ? "the 4.5-requested (clamped)" : "the 3.3", (void *)ctx);

  if (!x_glXMakeCurrent(dpy, win, ctx)) {
    say("[GL] FAIL glXMakeCurrent\n");
    return 4;
  }
  say("[GL] make current ok\n");

  if (resolve_gl() != 0) {
    say("[GL] FAIL resolving the GL 3.3 entry points\n");
    return 5;
  }

  const GLubyte *ver = gl.GetString(GL_VERSION);
  const GLubyte *ren = gl.GetString(GL_RENDERER);
  const GLubyte *vendor = gl.GetString(GL_VENDOR);
  const GLubyte *sl = gl.GetString(GL_SHADING_LANGUAGE_VERSION);
  say("[GL] GL_VERSION  : %s\n", ver ? (const char *)ver : "(null)");
  say("[GL] GL_RENDERER : %s\n", ren ? (const char *)ren : "(null)");
  say("[GL] GL_VENDOR   : %s\n", vendor ? (const char *)vendor : "(null)");
  say("[GL] GLSL        : %s\n", sl ? (const char *)sl : "(null)");
  if (!ver || !strstr((const char *)ver, "3.3")) {
    say("[GL] FAIL GL_VERSION is not 3.3\n");
    g_fail = 1;
  }
  if (!sl || !strstr((const char *)sl, "3.30")) {
    say("[GL] FAIL GLSL version is not 3.30\n");
    g_fail = 1;
  }

  /* 5. FBO: colour in an RGBA8 renderbuffer, nothing else.  A core-profile
   *    default framebuffer would also work, but an FBO's readback is exact
   *    and independent of the window/swap path. */
  GLuint fbo = 0, rbo = 0;
  gl.GenFramebuffers(1, &fbo);
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
  gl.GenRenderbuffers(1, &rbo);
  gl.BindRenderbuffer(GL_RENDERBUFFER, rbo);
  gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, PROBE_W, PROBE_H);
  gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, rbo);
  GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
  say("[GL] FBO %ux%u status=0x%x (complete=0x%x)\n", PROBE_W, PROBE_H, st,
      GL_FRAMEBUFFER_COMPLETE);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    say("[GL] FAIL FBO incomplete\n");
    return 6;
  }

  /* 6. shaders + one triangle. */
  GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC, "vertex");
  GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC, "fragment");
  if (!vs || !fs) return 7;
  GLuint prog = gl.CreateProgram();
  gl.AttachShader(prog, vs);
  gl.AttachShader(prog, fs);
  gl.LinkProgram(prog);
  GLint linked = 0;
  gl.GetProgramiv(prog, GL_LINK_STATUS, &linked);
  {
    char log[1024];
    GLsizei n = 0;
    log[0] = 0;
    gl.GetProgramInfoLog(prog, sizeof log, &n, log);
    say("[GL] program link: %s%s%s\n", linked ? "ok" : "FAILED",
        n > 0 ? " log=" : "", n > 0 ? log : "");
  }
  if (!linked) return 7;

  /* NDC triangle with red/green/blue corners -> interpolated interior. */
  static const GLfloat verts[] = {
      -0.85f, -0.80f, 1.0f, 0.0f, 0.0f,
       0.85f, -0.80f, 0.0f, 1.0f, 0.0f,
       0.00f,  0.85f, 0.0f, 0.0f, 1.0f,
  };
  GLuint vao = 0, vbo = 0;
  gl.GenVertexArrays(1, &vao);
  gl.BindVertexArray(vao);
  gl.GenBuffers(1, &vbo);
  gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
  gl.BufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
  GLint a_pos = gl.GetAttribLocation(prog, "a_pos");
  GLint a_col = gl.GetAttribLocation(prog, "a_col");
  say("[GL] attribs: a_pos=%d a_col=%d vao=%u vbo=%u\n", a_pos, a_col, vao, vbo);
  if (a_pos < 0 || a_col < 0) {
    say("[GL] FAIL shader inputs were optimised away\n");
    return 7;
  }
  gl.EnableVertexAttribArray((GLuint)a_pos);
  gl.VertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                         (const void *)0);
  gl.EnableVertexAttribArray((GLuint)a_col);
  gl.VertexAttribPointer((GLuint)a_col, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat),
                         (const void *)(2 * sizeof(GLfloat)));

  gl.Viewport(0, 0, PROBE_W, PROBE_H);
  gl.ClearColor(0.05f, 0.05f, 0.10f, 1.0f);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  gl.UseProgram(prog);
  gl.DrawArrays(GL_TRIANGLES, 0, 3);
  gl.Finish();
  GLenum err = gl.GetError();
  say("[GL] draw done, glGetError=0x%x\n", err);

  /* 7. read back and hash. */
  unsigned char *px = (unsigned char *)malloc((size_t)PROBE_W * PROBE_H * 4);
  if (!px) {
    say("[GL] FAIL out of memory for the readback\n");
    return 8;
  }
  memset(px, 0, (size_t)PROBE_W * PROBE_H * 4);
  gl.ReadPixels(0, 0, PROBE_W, PROBE_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
  gl.Finish();
  err = gl.GetError();
  say("[GL] readback %dx%d rgba8 glGetError=0x%x\n", PROBE_W, PROBE_H, err);

  const unsigned char bg[4] = {px[0], px[1], px[2], px[3]};
  int nonbg = 0;
  for (int i = 0; i < PROBE_W * PROBE_H; i++)
    if (memcmp(px + (size_t)i * 4, bg, 4) != 0) nonbg++;

  uint32_t *cols = (uint32_t *)malloc((size_t)PROBE_W * PROBE_H * 4);
  int ncol = 0;
  if (cols) {
    for (int i = 0; i < PROBE_W * PROBE_H; i++) {
      uint32_t v;
      memcpy(&v, px + (size_t)i * 4, 4);
      cols[ncol++] = v;
    }
    qsort(cols, (size_t)ncol, 4, cmp_u32);
    int uniq = ncol > 0 ? 1 : 0;
    for (int i = 1; i < ncol; i++)
      if (cols[i] != cols[i - 1]) uniq++;
    ncol = uniq;
  }

  say("[GL] readback %ux%u: fnv1a64=0x%016llx glGetError=0x%x\n", PROBE_W,
      PROBE_H, (unsigned long long)fnv1a64(px, (size_t)PROBE_W * PROBE_H * 4),
      err);
  say("[GL] non-background=%d/%d (%d%%) colours=%d bg=%u,%u,%u,%u\n", nonbg,
      PROBE_W * PROBE_H, nonbg * 100 / (PROBE_W * PROBE_H), ncol, bg[0], bg[1],
      bg[2], bg[3]);
  coverage_map(px, PROBE_W, PROBE_H, bg);
  gl.BindVertexArray(0);
  {
    int bad = 0;
    bad += mip_probe_one("small", 64, 64, 4);
    bad += mip_probe_one("planet", 8192, 4096, 5);
    say("[GL] mip-probe: %d level(s) failed\n", bad);
    if (bad) g_fail = 1;
    if (atlas_probe() != 0) g_fail = 1;
    /* Diagnostic only: the current virgl host rejects 1-D transfers wider
     * than 16 KiB, so this cannot be a default gate until the host path gains
     * a real buffer upload.  Keep it runnable with GL_PROBE_ABI=1. */
    const char *abi_env = getenv("GL_PROBE_ABI");
    if (abi_env && *abi_env == '1' && abi_buffer_transfer_probe()) g_fail = 1;
    if (g_atlas_tex) {
      gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
      /* atlas_probe leaves its texture attached to this FBO; rendering into
       * the texture we sample is a feedback loop and makes the whole mesh
       * pass produce no pixels (measured: draw_px=0).  Restore the independent
       * RGBA8 renderbuffer the tile pass expects. */
      gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_RENDERBUFFER, rbo);
      /* The mesh pass is a 16x16 grid.  Coarser grids are not comparable:
       * their UVs span multiple atlas cells, so per-cell colour expectations
       * would be wrong rather than informative. */
      if (tile_mesh_probe(g_atlas_tex, 0, 16) != 0) g_fail = 1;
      if (tile_mesh_probe(g_atlas_tex, 1, 16) != 0) g_fail = 1;
      if (lod_sample_probe("small", 64, 64, 4) > 0) g_fail = 1;
      if (lod_sample_probe("planet", 8192, 4096, 5) > 0) g_fail = 1;
      if (blit_probe_one("same", 256, 256, 256, 256)) g_fail = 1;
      if (blit_probe_one("down", 8192, 4096, 1280, 720)) g_fail = 1;
    }
  }
  gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);


  /* Shared-object transfer from a second GLX screen.  Unity's renderer uses
   * worker contexts; a second X display connection forces a second Mesa screen
   * and therefore a second /dev/gpu fd in the winsys.  GLX still permits the
   * objects to be shared, so this upload must not be refused merely because
   * the res_id was created by the other context. */
  if (g_atlas_tex) {
    GLenum serr = gl.GetError();
    Display *d2 = XOpenDisplay(NULL);
    GLXContext c2 = NULL;
    XVisualInfo *vi2 = NULL;
    Window w2 = 0;
    int made = 0;
    if (d2) {
      int cfg_n = 0;
      GLXFBConfig *configs =
          x_glXChooseFBConfig(d2, DefaultScreen(d2), attribs, &cfg_n);
      if (configs && cfg_n > 0) {
        vi2 = x_glXGetVisualFromFBConfig(d2, configs[0]);
        c2 = request_ctx(d2, configs[0], ctx, "share-second-display", NULL);
        if (vi2 && c2) {
          XSetWindowAttributes swa2;
          memset(&swa2, 0, sizeof swa2);
          swa2.colormap = XCreateColormap(
              d2, RootWindow(d2, vi2->screen), vi2->visual, AllocNone);
          w2 = XCreateWindow(d2, RootWindow(d2, vi2->screen), 0, 0, 64, 64, 0,
                             vi2->depth, InputOutput, vi2->visual, CWColormap,
                             &swa2);
          XMapWindow(d2, w2);
          XSync(d2, False);
          made = x_glXMakeCurrent(d2, w2, c2);
          if (made) {
            gl.BindTexture(GL_TEXTURE_2D, g_atlas_tex);
            unsigned char cp[4] = {19, 87, 233, 255};
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, 3, 3, 1, 1, GL_RGBA,
                             GL_UNSIGNED_BYTE, cp);
            gl.Finish();
            serr = gl.GetError();
          }
        }
        XFree(configs);
      }
    }
    say("[SHARE] second-display ctx=%d made_current=%d upload_err=0x%x\n",
        c2 != NULL, made, serr);
    if (d2 && made) x_glXMakeCurrent(d2, None, NULL);
    x_glXMakeCurrent(dpy, win, ctx);
    if (c2) x_glXDestroyContext(d2, c2);
    if (w2) XDestroyWindow(d2, w2);
    if (vi2) XFree(vi2);
    if (d2) XCloseDisplay(d2);
    resolve_gl();
  }

  /* 8. leave the pixels on the disk image too: a PPM the harness copies out,
   *    so a reader can look at the frame without a screenshot of a window. */
  FILE *f = fopen("/GLPROBE.PPM", "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", PROBE_W, PROBE_H);
    /* GL's origin is bottom-left, PPM's is top-left: flip rows. */
    for (int y = PROBE_H - 1; y >= 0; y--)
      for (int x = 0; x < PROBE_W; x++) {
        const unsigned char *p = px + ((size_t)y * PROBE_W + x) * 4;
        fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
      }
    fclose(f);
    say("[GL] wrote /GLPROBE.PPM (256x256 P6)\n");
  } else {
    say("[GL] note: could not write /GLPROBE.PPM (read-only root?)\n");
  }

  /* 9. draw to the window's default framebuffer and PRESENT it.  The GLX
   *    drawable is a real window; glXSwapBuffers runs Mesa's own present path
   *    (xlib_sw_display -> XPutImage -> the compat Xlib's shadow ->
   *    XFlush -> /dev/fb0), which is exactly what this step has to prove. */
  gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl.Viewport(0, 0, WIN_W, WIN_H);
  gl.ClearColor(0.05f, 0.05f, 0.10f, 1.0f);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  gl.UseProgram(prog);
  gl.BindVertexArray(vao);
  gl.EnableVertexAttribArray((GLuint)a_pos);
  gl.EnableVertexAttribArray((GLuint)a_col);
  gl.DrawArrays(GL_TRIANGLES, 0, 3);
  gl.Finish();
  say("[GL] window: default framebuffer %dx%d drawn, glGetError=0x%x\n", WIN_W,
      WIN_H, gl.GetError());

  /* Snapshot the back buffer BEFORE the swap: those are the exact bytes the
   * present path is about to ship, so this is the reference the desktop rect
   * is compared against afterwards. */
  unsigned char *back = (unsigned char *)malloc((size_t)WIN_W * WIN_H * 4);
  uint64_t back_hash = 0;
  if (back) {
    gl.ReadPixels(0, 0, WIN_W, WIN_H, GL_RGBA, GL_UNSIGNED_BYTE, back);
    gl.Finish();
    back_hash = fnv1a64(back, (size_t)WIN_W * WIN_H * 4);
    say("[GL] gl back buffer %dx%d fnv1a64=0x%016llx glGetError=0x%x\n", WIN_W,
        WIN_H, (unsigned long long)back_hash, gl.GetError());
  }

  say("[GL] swap start drawable=0x%lx ctx=%p\n", (unsigned long)win,
      (void *)ctx);
  x_glXSwapBuffers(dpy, win);
  /* Mesa's own XFlush runs inside xlib_sw_display; this one is the probe's
   * own, so presentation does not depend on Mesa's ordering. */
  XSync(dpy, False);
  say("[GL] swap returned\n");

  /* 10. read the pixels back OFF THE DESKTOP: /dev/fb0, the same surface the
   *     desktop scans out.  The GL readback above can agree with itself and
   *     still never be presented; this cannot. */
  if (fb_map() == 0) {
    int fb_nonbg = 0;
    uint64_t fbh = fb_hash_rect(wa.x, wa.y, WIN_W, WIN_H, &fb_nonbg, bg);
    say("[GL] desktop rect %d,%d %dx%d fnv1a64=0x%016llx nonbg=%d/%d\n", wa.x,
        wa.y, WIN_W, WIN_H, (unsigned long long)fbh, fb_nonbg, WIN_W * WIN_H);
    if (back) {
      say("[GL] frame hashes: desktop=0x%016llx gl=0x%016llx\n",
          (unsigned long long)fbh, (unsigned long long)back_hash);
      int bbox[4], shown = 0;
      int diff = fb_diff_rect(wa.x, wa.y, WIN_W, WIN_H, back, bbox, &shown);
      say("[GL] pixel diff: %d of %d differ (%.4f%%)\n", diff, WIN_W * WIN_H,
          diff * 100.0 / ((double)WIN_W * WIN_H));
      if (diff) {
        say("[GL] diff bbox: x %d..%d y %d..%d (%dx%d)\n", bbox[0], bbox[2],
            bbox[1], bbox[3], bbox[2] - bbox[0] + 1, bbox[3] - bbox[1] + 1);
      }
      /* The frame is presented if (nearly) every pixel matches: the desktop
       * re-stamps its mouse pointer over the app's pixels after ESC ] F, so a
       * cursor-sized patch legitimately differs.  Anything larger is a partial
       * or stale present and fails. */
      say("[GL] present check: %s\n",
          diff == 0 ? "exact" :
          (diff <= 4096 ? "desktop holds the frame (only the re-stamped cursor "
                          "differs)" : "MISMATCH"));
      if (diff > 4096) {
        say("[GL] FAIL the desktop does not hold the frame GL presented\n");
        g_fail = 1;
      }
    }
    /* Two interior samples (the blue vertex above the middle, the red one at
     * lower left), each shown against BOTH row orders: an exact match under
     * one of them names a flip, and no match at all names a channel-order or
     * never-arrived frame.  GL row 0 is the bottom; the desktop's is the top. */
    if (back) {
      static const struct { int fx, fy; const char *name; } pts[] = {
          {50, 88, "blue-vertex"}, {20, 20, "red-vertex"}};
      for (int k = 0; k < 2; k++) {
        int gx = WIN_W * pts[k].fx / 100, gy = WIN_H * pts[k].fy / 100;
        const unsigned char *g = back + ((size_t)gy * WIN_W + gx) * 4;
        unsigned int same = g_fb[(size_t)(wa.y + (WIN_H - 1 - gy)) * FB_W + wa.x + gx];
        unsigned int flip = g_fb[(size_t)(wa.y + gy) * FB_W + wa.x + gx];
        say("[GL] px %s (%d,%d): gl=%02x%02x%02x%02x desktop[rowflip]=%08x "
            "desktop[direct]=%08x\n", pts[k].name, gx, gy, g[0], g[1], g[2],
            g[3], same, flip);
      }
    }
    /* A scaled copy of the desktop rect, so the frame can be looked at on the
     * host without going through a screenshot of the whole screen. */
    {
      FILE *f = fopen("/GLDESK.PPM", "wb");
      if (f) {
        int dw = WIN_W / 4, dh = WIN_H / 4;
        fprintf(f, "P6\n%d %d\n255\n", dw, dh);
        for (int y = 0; y < dh; y++)
          for (int x = 0; x < dw; x++) {
            unsigned int v = g_fb[(size_t)(wa.y + y * 4) * FB_W + wa.x + x * 4];
            fputc((v >> 16) & 0xff, f);
            fputc((v >> 8) & 0xff, f);
            fputc(v & 0xff, f);
          }
        fclose(f);
        say("[GL] wrote /GLDESK.PPM (%dx%d, desktop rect, 1:4)\n", dw, dh);
      }
    }
    if (fb_nonbg < 1000) {
      say("[GL] FAIL the desktop framebuffer has no frame at the window rect\n");
      g_fail = 1;
    }
  } else {
    say("[GL] FAIL no desktop readback path\n");
    g_fail = 1;
  }

  if (nonbg < 1000) {
    say("[GL] FAIL the triangle did not cover the framebuffer\n");
    g_fail = 1;
  }

  /* 11. hold the window on screen: the harness screendumps this interval, so
   *     the receipt is a photo of the desktop, not a number in a log. */
  say("[GL] hold start\n");
  {
    long deadline = now_ms() + 6000;
    while (now_ms() < deadline) {
      while (XPending(dpy)) {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        XNextEvent(dpy, &ev);
      }
      usleep(20000);
    }
  }
  say("[GL] hold done\n");

  /* 12. teardown, layer by layer.  Each of glXMakeCurrent(None) /
   *     glXDestroyContext / XCloseDisplay / dlclose releases a different part
   *     of the compat stack; a clean run must print all four rows. */
  say("[GL] teardown: glXMakeCurrent(None)\n");
  Bool mc = x_glXMakeCurrent(dpy, None, NULL);
  say("[GL] teardown: glXMakeCurrent(None) -> %d\n", (int)mc);
  say("[GL] teardown: glXDestroyContext\n");
  x_glXDestroyContext(dpy, ctx);
  say("[GL] teardown: glXDestroyContext returned\n");
  say("[GL] teardown: XCloseDisplay\n");
  XCloseDisplay(dpy);
  say("[GL] teardown: XCloseDisplay returned\n");
  say("[GL] teardown: dlclose(libGL)\n");
  int dc = dlclose(h);
  say("[GL] teardown: dlclose -> %d\n", dc);
  say("[GL] teardown: clean\n");

  say("[GL] done rc=%d\n", g_fail);
  return g_fail ? 1 : 0;
}
