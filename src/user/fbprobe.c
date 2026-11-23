/*
 * fbprobe.c - the N0 present-gate probe (PLAN.md work package E).
 *
 * What it does, and why each step is there:
 *
 *   1. open("/dev/fb0") and mmap() the FULL 1920x1080 B8G8R8A8 frame
 *      (8,294,400 B) against offset 0.  This is the exact shape SDL's fbcon
 *      and the compat libX11 probe issue, and the shape that failed
 *      -ENOMEM (`[LCOMPAT] mmap fail fd=6 len=8294400 off=0 -> -12`) because
 *      the kernel mapped its 10,485,760 B USER_FB_SIZE slot instead of the
 *      8,294,400 B framebuffer it stood on.  The probe prints the raw
 *      SYS_MMAP return so a failure names its errno and a success names the
 *      VA - it never turns a failure into a quiet continue.
 *   2. Paint the whole frame a flat fill, then a 64x64 magenta corner block,
 *      then ONE pixel at (1919,1079) in pure red.  The corner pixel is the
 *      gate: it is the last byte of the framebuffer, so it can only be
 *      visible if the mapping covered the whole 8,294,400 B.  A mapping
 *      clamped short (e.g. the old 4 MiB slot) shows the fill colour there,
 *      and a mapping that never landed shows whatever the scanout had.
 *   3. Read the corner pixel back through the mapping and print it.  This is
 *      supporting evidence only: it proves the write reached the frame the
 *      kernel scans out, not that the host displayed it.  The gate itself is
 *      the QMP screendump (tools/nv/present_gate.py).
 *   4. flush_fb() (SYS_FLUSH_FB) so the virtio-gpu transfers + flushes the
 *      resource to the host scanout, then hold the frame long enough for the
 *      harness to grab it.
 *
 * Build/run: MODE=fb_test boots ONLY this program (main.c), so nothing else
 * paints over the frame between the write and the screendump.
 */
#include "libc.h"
#include <fcntl.h> /* O_RDWR for the /dev/fb0 open */
#include "syscall.h"

#define FB_W 1920
#define FB_H 1080
#define FB_BYTES ((unsigned long)FB_W * (unsigned long)FB_H * 4ul)

#ifndef MAP_SHARED
#define MAP_SHARED 1
#endif
#ifndef PROT_READ
#define PROT_READ 1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 2
#endif

/* B8G8R8A8: the 32-bit memory word is 0xAARRGGBB, so these constants name
 * the colour a PPM reader sees (R,G,B) but are stored in BGRA byte order. */
#define COLOR_FILL   0xFF203040u /* R=20 G=30 B=40: dark, unique in the UI */
#define COLOR_BLOCK  0xFFFF00FFu /* magenta */
#define COLOR_CORNER 0xFFFF0000u /* pure red - the gate pixel */

/* Raw 6-argument syscall (SYS_MMAP keeps the Linux-shaped 6-register ABI).
 * The native sysroot exposes only the wrappers libc.c implements, and mmap
 * is not one of them today, so the probe issues the syscall itself - the
 * same shape src/user/libc.c and the *_test programs use for 4/5 args. */
static long sys6(long num, long a0, long a1, long a2, long a3, long a4,
                 long a5) {
#ifdef __x86_64__
  long ret;
  register long rdi __asm__("rdi") = a0;
  register long rsi __asm__("rsi") = a1;
  register long rdx __asm__("rdx") = a2;
  register long r10 __asm__("r10") = a3;
  register long r8 __asm__("r8") = a4;
  register long r9 __asm__("r9") = a5;
  __asm__ volatile("syscall\n"
                   : "=a"(ret)
                   : "a"(num), "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10),
                     "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
#else
  register long x8 __asm__("x8") = num;
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  __asm__ volatile("svc #0\n"
                   : "+r"(x0)
                   : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                   : "memory");
  return x0;
#endif
}

static void kv(const char *key, long value) {
  print_console("[FBP] ");
  print_console(key);
  print_console("=");
  print_dec(value);
  print_console("\n");
}

static void kvhex(const char *key, unsigned long value) {
  print_console("[FBP] ");
  print_console(key);
  print_console("=0x");
  print_hex((long)value);
  print_console("\n");
}

static void paint(volatile unsigned int *fb) {
  for (unsigned long i = 0; i < (unsigned long)FB_W * FB_H; i++)
    fb[i] = COLOR_FILL;
  /* A 64x64 block at the bottom-right corner, then the single gate pixel
   * inside it: the block makes the corner findable in the image, the single
   * pixel makes the read-back value unambiguous. */
  for (int y = FB_H - 64; y < FB_H; y++) {
    for (int x = FB_W - 64; x < FB_W; x++)
      fb[(unsigned long)y * FB_W + x] = COLOR_BLOCK;
  }
  fb[(unsigned long)(FB_H - 1) * FB_W + (FB_W - 1)] = COLOR_CORNER;
}

/* The edge of the slot contract: the first byte PAST the frame
 * (mapping + 8,294,400) must not be mapped at all.  Before the WP-E fix the
 * whole 10,485,760 B slot was mapped, so that read completed and returned
 * whatever the linker placed after the kernel's framebuffer static (kernel
 * .bss handed to user space).  A child does the read: a fault kills the
 * child, not the probe. */
static void probe_past_frame(volatile unsigned int *fb) {
  int pid = fork();
  if (pid == 0) {
    volatile unsigned int *past = fb + (unsigned long)FB_W * FB_H;
    unsigned int v = *past;
    print_console("[FBP] PAST_FRAME read COMPLETED (defect) value=0x");
    print_hex((long)v);
    print_console("\n");
    exit(7); /* the marker the parent reads */
  }
  if (pid < 0) {
    kv("past_frame_fork_failed", pid);
    return;
  }
  int st = 0;
  int w = waitpid(pid, &st, 0);
  kv("past_frame_status", (long)st);
  if (w == pid && (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 7) {
    print_console("[FBP] RESULT FAIL past_frame (slot maps past the frame)\n");
  } else {
    print_console("[FBP] RESULT PASS past_frame (page past the frame is unmapped)\n");
  }
}

int main(void) {
  print_console("[FBP] /dev/fb0 present-gate probe (N0)\n");

  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    kv("open_failed", fd);
    return 1;
  }
  kv("fd", fd);
  kv("want_bytes", (long)FB_BYTES);

  long va = sys6(SYS_MMAP, 0, (long)FB_BYTES, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (va < 0) {
    /* Raw negative errno, not -1: the number is the diagnosis. */
    kv("mmap_errno", -va);
    print_console("[FBP] RESULT FAIL mmap\n");
    return 1;
  }
  kvhex("mmap_va", (unsigned long)va);
  print_console("[FBP] RESULT PASS mmap\n");

  volatile unsigned int *fb = (volatile unsigned int *)va;
  paint(fb);

  unsigned int corner = fb[(unsigned long)(FB_H - 1) * FB_W + (FB_W - 1)];
  unsigned int inside = fb[(unsigned long)(FB_H - 1) * FB_W + (FB_W - 2)];
  unsigned int far = fb[0];
  kvhex("corner_read", corner);
  kvhex("inside_read", inside);
  kvhex("far_read", far);
  if (corner != COLOR_CORNER || inside != COLOR_BLOCK || far != COLOR_FILL) {
    print_console("[FBP] RESULT FAIL readback\n");
    return 1;
  }
  print_console("[FBP] RESULT PASS readback\n");

  probe_past_frame(fb);

  flush_fb();
  print_console("[FBP] RESULT PASS flush\n");

  /* Hold the frame so the harness can screendump it.  Keep re-painting: a
   * present path that only paints once is a present path that races the
   * capture. */
  for (int i = 0; i < 60; i++) {
    sleep(250);
    if ((i % 8) == 0) {
      paint(fb);
      flush_fb();
      kv("hold_ms", (long)((i + 1) * 250));
    }
  }
  print_console("[FBP] done\n");
  return 0;
}
