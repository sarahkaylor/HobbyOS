/* ============================================================================
 *  nvabi_probe.c -- drive /dev/nvshim's DRM face from EL0 (WP-EL0).
 *
 *  WHAT THIS IS.  Every proof of the DRM_IOCTL_NOUVEAU_* surface up to round 3
 *  is a KERNEL unit test: src/kernel/nv_shim_drm_test.c calls nv_shim_drm_ioctl()
 *  with a kernel pointer and a hand-built process.  That proves the handler.  It
 *  cannot prove the ABI, because the ABI is what happens when the arguments come
 *  from a different privilege level: the request number travels through a real
 *  syscall, the struct lives at a user VA the kernel must translate, the mmap
 *  offset is a bare integer from another address space, and the fd can be closed
 *  by the client at any moment.
 *
 *  So this program is a CLIENT: a static x86_64 Linux-ABI ELF (the same class
 *  of binary a stock nvk is) that opens the node, runs the sequence a nouveau
 *  winsys runs -- VERSION, GET_CAP, GETPARAM, VM_INIT, GEM_NEW, mmap, VM_BIND,
 *  CHANNEL_ALLOC, EXEC, SYNCOBJ_WAIT -- with real user pointers and real
 *  integers it can only get from the previous answer, and prints ONE row per
 *  step carrying the numbers it saw.
 *
 *  WHY A LINUX-ABI ELF AND NOT A NATIVE .BIN.  HobbyOS's native syscall table
 *  has no ioctl row (src/libc/src/ioctl.c says so, src/include/syscall.h's
 *  SYS_MAX is frozen), and /dev/nvshim's requests are reached through the
 *  compat ioctl(16) route (src/kernel/linux_compat_fd.c).  A native .BIN
 *  therefore cannot issue a single DRM request -- named in the report as a
 *  finding, not worked around.  It is also the truthful vehicle: nvk is a Linux
 *  binary and will arrive the same way.
 *
 *  NO LIBC.  musl cannot run here (it needs a host musl to build and its
 *  startup would be a second thing under test), so the program is freestanding:
 *  its own syscall stubs, its own number formatting, its own FNV-1a.  The
 *  structs come from the SAME contract header the kernel validates against
 *  (nv_drm_uapi.h) with the client's own size/offset asserts next to them, so a
 *  drift in the contract breaks this program's build instead of confusing it.
 *
 *  EVERY ROW IS ONE write(1), so a row can never be interleaved with a kernel
 *  [NVSHIM] row mid-line (PLAN.md §6.4: one row per stage, and the row names
 *  the stage).  Anything this program cannot do is printed as a named refusal
 *  with the errno it got, never skipped.
 *
 *  Build: obj/intel/nvabi_probe.elf (Makefile target nvabi_elf).
 *  Run:   tools/nv/el0_wave.sh (MODE=test + /WAVE.CFG = NVABI.ELF).
 * ========================================================================== */

#ifdef __x86_64__

#include "../include/nv_drm_uapi.h"

/* ---- the contract, checked on the CLIENT side --------------------------
 * A client that links against a wrong layout is the failure mode this whole
 * lane exists to catch, so the numbers are asserted here as well as in the
 * kernel tier.  Each is the uAPI's own width on x86_64. */
_Static_assert(sizeof(struct nvshim_drm_version) == 64, "drm_version");
_Static_assert(sizeof(struct nvshim_drm_gem_new) == 48, "gem_new");
_Static_assert(sizeof(struct nvshim_drm_gem_info) == 40, "gem_info");
_Static_assert(sizeof(struct nvshim_drm_vm_init) == 16, "vm_init");
_Static_assert(sizeof(struct nvshim_drm_vm_bind) == 40, "vm_bind");
_Static_assert(sizeof(struct nvshim_drm_vm_bind_op) == 40, "vm_bind_op");
_Static_assert(sizeof(struct nvshim_drm_channel_alloc) == 88, "channel_alloc");
_Static_assert(sizeof(struct nvshim_drm_exec) == 40, "exec");
_Static_assert(sizeof(struct nvshim_drm_exec_push) == 16, "exec_push");
_Static_assert(sizeof(struct nvshim_drm_sync) == 16, "sync");
_Static_assert(sizeof(struct nvshim_drm_syncobj_wait) == 40, "syncobj_wait");
_Static_assert(NVSHIM_DRM_IOC_VERSION == 0xC0406400u, "VERSION number");
_Static_assert(NVSHIM_DRM_IOC_GETPARAM == 0xC0106440u, "GETPARAM number");
_Static_assert(NVSHIM_DRM_IOC_VM_INIT == 0xC0106450u, "VM_INIT number");
_Static_assert(NVSHIM_DRM_IOC_VM_BIND == 0xC0286451u, "VM_BIND number");
_Static_assert(NVSHIM_DRM_IOC_EXEC == 0xC0286452u, "EXEC number");
_Static_assert(NVSHIM_DRM_IOC_GEM_NEW == 0xC0306480u, "GEM_NEW number");
_Static_assert(NVSHIM_DRM_IOC_GEM_INFO == 0xC0286484u, "GEM_INFO number");
_Static_assert(NVSHIM_DRM_IOC_CHANNEL_ALLOC == 0xC0586442u, "CHANNEL_ALLOC number");

/* ---- freestanding runtime ---------------------------------------------- */

/* Syscall numbers, x86_64 Linux (the numbers the compat table owns). */
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_MMAP   9
#define SYS_IOCTL  16
#define SYS_FORK   57
#define SYS_WAIT4  61
#define SYS_EXIT   60
#define SYS_EXIT_GROUP 231

#define O_RDWR 2
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
  register long r10 __asm__("r10") = d;
  register long r8 __asm__("r8") = e;
  register long r9 __asm__("r9") = f;
  long ret;
  __asm__ volatile("syscall"
                   : "=a"(ret)
                   : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory");
  return ret;
}
static long sc3(long n, long a, long b, long c) { return sc6(n, a, b, c, 0, 0, 0); }
static long sc1(long n, long a) { return sc6(n, a, 0, 0, 0, 0, 0); }

/* -fno-builtin is set for this TU, so these loops stay loops. */
static void bzero_n(void *p, unsigned long n) {
  volatile unsigned char *d = (volatile unsigned char *)p;
  for (unsigned long i = 0; i < n; i++) d[i] = 0;
}
static int bytes_eq(const unsigned char *a, const unsigned char *b, unsigned long n) {
  for (unsigned long i = 0; i < n; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}
/* ---- one row, one write ------------------------------------------------ */

static char g_row[256];
static int g_rn;

static void r_reset(void) { g_rn = 0; }
static void r_c(char c) {
  if (g_rn < (int)sizeof g_row - 2) g_row[g_rn++] = c;
}
static void r_s(const char *s) {
  while (s && *s) r_c(*s++);
}
static void r_hex(unsigned long v) {
  static const char hx[] = "0123456789abcdef";
  r_s("0x");
  int started = 0;
  for (int sh = 60; sh >= 0; sh -= 4) {
    int d = (int)((v >> sh) & 0xF);
    if (d || started || sh == 0) {
      r_c(hx[d]);
      started = 1;
    }
  }
}
static void r_dec(long v) {
  char b[24];
  int n = 0;
  if (v < 0) {
    r_c('-');
    v = -v;
  }
  if (v == 0) {
    r_c('0');
    return;
  }
  while (v > 0 && n < (int)sizeof b) {
    b[n++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (n > 0) r_c(b[--n]);
}
static void r_emit(void) {
  g_row[g_rn++] = '\n';
  (void)sc3(SYS_WRITE, 1, (long)g_row, g_rn);
  g_rn = 0;
}

/* ---- result accounting ------------------------------------------------- */

static int g_pass, g_fail, g_steps;

/* Begin one row: "[EL0] NN stage ".  Every step calls done() exactly once. */
static void begin(int n, const char *stage) {
  r_reset();
  r_s("[EL0] ");
  if (n < 10) r_c('0');
  r_dec(n);
  r_c(' ');
  r_s(stage);
  r_c(' ');
  g_steps++;
}
/* ok != 0 = PASS.  `extra` is appended before the verdict, already formatted
 * by the caller through the r_* helpers. */
static void done(int ok) {
  if (ok) {
    r_s(" PASS");
    g_pass++;
  } else {
    r_s(" FAIL");
    g_fail++;
  }
  r_emit();
}

/* ---- helpers ----------------------------------------------------------- */

static long ioctl_(int fd, unsigned long req, void *arg) {
  return sc3(SYS_IOCTL, fd, (long)req, (long)arg);
}

/* errno of a negative raw return, for a row; 0 when the call succeeded. */
static long err_of(long r) { return r < 0 ? -r : 0; }

static unsigned long fnv1a(const unsigned char *p, unsigned long n) {
  unsigned long h = 2166136261u;
  for (unsigned long i = 0; i < n; i++) {
    h ^= (unsigned long)p[i];
    h = (h * 16777619u) & 0xFFFFFFFFu;
  }
  return h;
}

/* The push stream this program writes through its CPU mapping: 64 bytes of a
 * pattern that is NOT the zero page the object was allocated with, so "the
 * kernel read the client's bytes" and "the kernel read fresh zeroed memory"
 * cannot produce the same digest.  Dword-aligned, as EXEC requires. */
static const unsigned int g_push_words[16] = {
    0x00000010u, 0x4E564231u, 0xBEEF0001u, 0x00000102u,
    0xDEAD0002u, 0x00000304u, 0xCAFE0003u, 0x00000506u,
    0xF00D0004u, 0x00000708u, 0x12340005u, 0x0000090Au,
    0x56780006u, 0x00000B0Cu, 0x9ABC0007u, 0x00000D0Eu};

int main(void);

/* The stack at process entry is 16-byte aligned (Linux's rule, and what this
 * kernel's Linux-ABI entry sets: the loader row prints sp=0x1800000000).  A
 * crt that passed rsp through untouched would be fine, but clang gives `_start`
 * a frame pointer of its own, so `call main` leaves main 8 bytes off -- and -O2
 * vectorises the 64-byte BO copy into `movaps %xmm0,0x2d0(%rsp)`, which faults
 * #GP(0) on a misaligned operand (measured on the first EL0 run: vector 13,
 * err=0, rip=main+0x38ac).  So the entry stub owns the alignment explicitly;
 * a freestanding client cannot rely on anyone else doing it. */
__attribute__((section(".text._start"), noreturn)) void _start(void) {
  __asm__ volatile("andq $-16, %%rsp" ::: "memory");
  int rc = main();
  (void)sc1(SYS_EXIT_GROUP, rc);
  for (;;) {
  }
}

int main(void) {
  r_reset();
  r_s("[EL0] nvabi_probe: /dev/nvshim DRM face from ring 3 (WP-EL0)\n");
  r_emit();

  /* --- 01 open ---------------------------------------------------------- */
  long fd = sc3(SYS_OPEN, (long)"/dev/nvshim", O_RDWR, 0);
  begin(1, "open");
  r_s("path=/dev/nvshim fd=");
  r_dec(fd);
  done(fd >= 0);
  if (fd < 0) {
    r_reset();
    r_s("[EL0] RESULT FAIL 0/1 (no device node: nothing else can be asked)\n");
    r_emit();
    return 1;
  }

  /* --- 02 VERSION, the way libdrm calls it: query, then fill ------------ */
  struct nvshim_drm_version ver;
  bzero_n(&ver, sizeof ver);
  long r = ioctl_((int)fd, NVSHIM_DRM_IOC_VERSION, &ver);
  unsigned long name_len_q = ver.name_len;
  char name[32];
  bzero_n(name, sizeof name);
  int have_name = 0;
  if (r == 0 && name_len_q > 0 && name_len_q <= sizeof name) {
    bzero_n(&ver, sizeof ver);
    ver.name = (uint64_t)(uintptr_t)name;
    ver.name_len = sizeof name;
    ver.date = 0;
    ver.date_len = 0;
    ver.desc = 0;
    ver.desc_len = 0;
    r = ioctl_((int)fd, NVSHIM_DRM_IOC_VERSION, &ver);
    have_name = (r == 0) && bytes_eq((const unsigned char *)name,
                                     (const unsigned char *)"nouveau", 8);
  }
  begin(2, "version");
  r_s("name=");
  r_s(have_name ? name : "(none)");
  r_s(" v=");
  r_dec((long)ver.major);
  r_c('.');
  r_dec((long)ver.minor);
  r_c('.');
  r_dec((long)ver.patchlevel);
  r_s(" name_len=");
  r_dec((long)name_len_q);
  done(have_name && ver.major == 1 && ver.minor == 3 && ver.patchlevel == 1);

  /* --- 03 GET_CAP ------------------------------------------------------- */
  struct nvshim_drm_cap cap;
  cap.capability = NVSHIM_DRM_CAP_SYNCOBJ;
  cap.value = 0xFFFFFFFFFFFFFFFFull;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_GET_CAP, &cap);
  begin(3, "get_cap");
  r_s("capability=SYNCOBJ value=");
  r_dec((long)cap.value);
  done(r == 0 && cap.value == 1);

  /* --- 04 GETPARAM: identity a winsys keys on -------------------------- */
  struct nvshim_drm_getparam q;
  unsigned long chipset = 0, pcidev = 0, fbsize = 0;
  int gp_ok = 1;
  q.param = NVSHIM_PARAM_CHIPSET_ID;
  q.value = 0;
  if (ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, &q) != 0) gp_ok = 0;
  chipset = (unsigned long)q.value;
  q.param = NVSHIM_PARAM_PCI_DEVICE;
  q.value = 0;
  if (ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, &q) != 0) gp_ok = 0;
  pcidev = (unsigned long)q.value;
  q.param = NVSHIM_PARAM_FB_SIZE;
  q.value = 0;
  if (ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, &q) != 0) gp_ok = 0;
  fbsize = (unsigned long)q.value;
  begin(4, "getparam");
  r_s("chipset=");
  r_hex(chipset);
  r_s(" pci=");
  r_hex(pcidev);
  r_s(" fb_size=");
  r_hex(fbsize);
  done(gp_ok && chipset == 0x192 && pcidev == 0x2684 && fbsize > 0);

  /* --- 05 ARG-SIZE VALIDATION, from the caller's side -------------------
   * The same 16-byte struct sent with a request whose _IOC size field says 8:
   * the number is real, the shape is wrong, and the answer must be -EINVAL.
   * It must ALSO not have created the VM -- step 06 would then say -EBUSY. */
  struct nvshim_drm_vm_init vmi;
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_ANY(0x50, 8), &vmi);
  long e_size = err_of(r);
  begin(5, "arg_size");
  r_s("vm_init_enc_size=8 ret_errno=");
  r_dec(e_size);
  done(r == -22 /* -EINVAL */);

  /* --- 06 VM_INIT with Mesa's exact arguments -------------------------- */
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_VM_INIT, &vmi);
  begin(6, "vm_init");
  r_s("umd_end=");
  r_hex((unsigned long)vmi.kernel_managed_addr);
  r_s(" total=");
  r_hex((unsigned long)(vmi.kernel_managed_addr + vmi.kernel_managed_size));
  r_s(" errno=");
  r_dec(err_of(r));
  done(r == 0);

  /* --- 07 GEM_NEW: a CPU-mappable object ------------------------------- */
  struct nvshim_drm_gem_new gn;
  bzero_n(&gn, sizeof gn);
  gn.info.domain = NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE;
  gn.info.size = 8192;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_GEM_NEW, &gn);
  unsigned long map_handle = (unsigned long)gn.info.map_handle;
  unsigned long bo_handle = gn.info.handle;
  unsigned long bo_bus = (unsigned long)gn.info.offset;
  begin(7, "gem_new");
  r_s("handle=");
  r_dec((long)bo_handle);
  r_s(" size=");
  r_hex((unsigned long)gn.info.size);
  r_s(" bus=");
  r_hex(bo_bus);
  r_s(" map_handle=");
  r_hex(map_handle);
  done(r == 0 && bo_handle != 0 && gn.info.size == 8192 && bo_bus != 0 &&
       map_handle == (bo_handle << 12));

  /* --- 08 GEM_INFO agrees, and refuses a foreign handle ---------------- */
  struct nvshim_drm_gem_info gi;
  bzero_n(&gi, sizeof gi);
  gi.handle = (uint32_t)bo_handle;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_GEM_INFO, &gi);
  int info_ok = (r == 0 && (unsigned long)gi.map_handle == map_handle &&
                 gi.size == 8192 && (unsigned long)gi.offset == bo_bus);
  struct nvshim_drm_gem_info gx;
  bzero_n(&gx, sizeof gx);
  gx.handle = (uint32_t)(bo_handle + 777);
  long r_foreign_info = ioctl_((int)fd, NVSHIM_DRM_IOC_GEM_INFO, &gx);
  begin(8, "gem_info");
  r_s("map_handle=");
  r_hex((unsigned long)gi.map_handle);
  r_s(" size=");
  r_hex((unsigned long)gi.size);
  r_s(" foreign_errno=");
  r_dec(err_of(r_foreign_info));
  done(info_ok && r_foreign_info == -22);

  /* --- 09/10 the mmap offset is the OBJECT's name ----------------------
   * DRM has no mmap request row: the offset is the only way a bare mmap says
   * which object it means.  Offset 0 and a handle the client does not own must
   * both be refused by name, or a mapping could land on someone else's BO. */
  long bad0 = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  begin(9, "mmap_off0");
  r_s("errno=");
  r_dec(err_of(bad0));
  done(bad0 < 0);

  long badf = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (long)((bo_handle + 777) << 12));
  begin(10, "mmap_foreign");
  r_s("offset=");
  r_hex((unsigned long)((bo_handle + 777) << 12));
  r_s(" errno=");
  r_dec(err_of(badf));
  done(badf < 0);

  /* --- 11 mmap the object through the fd ------------------------------- */
  long mva = sc6(SYS_MMAP, 0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (long)map_handle);
  begin(11, "mmap");
  r_s("offset=");
  r_hex(map_handle);
  r_s(" len=0x2000 va=");
  if (mva > 0) {
    r_hex((unsigned long)mva);
    r_s(" errno=0");
  } else {
    r_s("(none) errno=");
    r_dec(err_of(mva));
  }
  done(mva > 0);
  if (mva <= 0) {
    r_reset();
    r_s("[EL0] RESULT FAIL (no CPU mapping: the rest of the ABI needs it)\n");
    r_emit();
    return 1;
  }

  /* --- 12 write a pattern THROUGH the mapping, read it back ------------ */
  unsigned char *bo = (unsigned char *)mva;
  for (unsigned long i = 0; i < sizeof g_push_words; i++)
    bo[i] = ((const unsigned char *)g_push_words)[i];
  /* one byte past the last dword, so a mapping that covered only page 0 would
   * fail this store and not the 64-byte one above */
  bo[8191] = 0x5A;
  unsigned long written_digest = fnv1a(bo, sizeof g_push_words);
  unsigned char back[64];
  for (unsigned long i = 0; i < 64; i++) back[i] = bo[i];
  int rb_ok = bytes_eq(back, (const unsigned char *)g_push_words, sizeof g_push_words) &&
              bo[8191] == 0x5A;
  begin(12, "bo_write");
  r_s("bytes=64 digest=");
  r_hex(written_digest);
  r_s(" last_byte=");
  r_hex((unsigned char)bo[8191]);
  done(rb_ok);

  /* --- 13 VM_BIND the object at a GPU VA ------------------------------- */
  struct nvshim_drm_vm_bind_op op;
  bzero_n(&op, sizeof op);
  op.op = NVSHIM_DRM_VM_BIND_OP_MAP;
  op.handle = (uint32_t)bo_handle;
  op.addr = 0x10000;
  op.range = 8192;
  struct nvshim_drm_vm_bind vb;
  bzero_n(&vb, sizeof vb);
  vb.op_count = 1;
  vb.op_ptr = (uint64_t)(uintptr_t)&op;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_VM_BIND, &vb);
  unsigned long push_va = 0x10000;
  begin(13, "vm_bind");
  r_s("ops=");
  r_dec((long)vb.op_count);
  r_s(" addr=");
  r_hex(push_va);
  r_s(" range=");
  r_hex(8192);
  r_s(" handle=");
  r_dec((long)bo_handle);
  done(r == 0);

  /* --- 14 CHANNEL_ALLOC: the kernel owns the push buffer ---------------- */
  struct nvshim_drm_channel_alloc ca;
  bzero_n(&ca, sizeof ca);
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_CHANNEL_ALLOC, &ca);
  long ch = (long)ca.channel;
  begin(14, "channel");
  r_s("ch=");
  r_dec(ch);
  r_s(" domains=");
  r_hex((unsigned long)ca.pushbuf_domains);
  r_s(" nr_subchan=");
  r_dec((long)ca.nr_subchan);
  done(r == 0 && ch > 0);

  /* --- 15 binary syncobj ----------------------------------------------- */
  struct nvshim_drm_syncobj_create sc;
  sc.handle = 0;
  sc.flags = 0;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_CREATE, &sc);
  unsigned int sobj = sc.handle;
  begin(15, "syncobj_create");
  r_s("handle=");
  r_dec((long)sobj);
  done(r == 0 && sobj != 0);

  /* --- 16 EXEC: a push that names the BO's VA -------------------------- */
  struct nvshim_drm_exec_push push;
  push.va = push_va;
  push.va_len = 64;
  push.flags = 0;
  struct nvshim_drm_sync sig;
  sig.flags = NVSHIM_DRM_SYNC_SYNCOBJ;
  sig.handle = sobj;
  sig.timeline_value = 0;
  struct nvshim_drm_exec ex;
  bzero_n(&ex, sizeof ex);
  ex.channel = (uint32_t)ch;
  ex.push_count = 1;
  ex.sig_count = 1;
  ex.sig_ptr = (uint64_t)(uintptr_t)&sig;
  ex.push_ptr = (uint64_t)(uintptr_t)&push;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_EXEC, &ex);
  begin(16, "exec");
  r_s("ch=");
  r_dec(ch);
  r_s(" push_va=");
  r_hex(push_va);
  r_s(" push_len=64 digest=");
  r_hex(written_digest);
  r_s(" errno=");
  r_dec(err_of(r));
  done(r == 0);

  /* --- 17 SYNCOBJ_WAIT: completion is the syncobj, not a return code --- */
  unsigned int wait_h = sobj;
  struct nvshim_drm_syncobj_wait sw;
  bzero_n(&sw, sizeof sw);
  sw.handles = (uint64_t)(uintptr_t)&wait_h;
  sw.count_handles = 1;
  sw.timeout_nsec = (long)(~0ull); /* absolute deadline: "forever" clamps */
  sw.flags = 0;
  sw.first_signaled = 0xFFFFFFFFu;
  r = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &sw);
  begin(17, "syncobj_wait");
  r_s("handles=1 first_signaled=");
  r_dec((long)sw.first_signaled);
  r_s(" errno=");
  r_dec(err_of(r));
  done(r == 0 && sw.first_signaled == 0);

  /* --- 18/19 the same wait must be able to say NO -----------------------
   * A syncobj that is always signalled would make step 17 true by
   * construction (PLAN.md §6.5).  Reset it: the poll form of the wait must now
   * answer -ETIME; signal it again: the wait must answer 0. */
  struct nvshim_drm_syncobj_array sa;
  sa.handles = (uint64_t)(uintptr_t)&wait_h;
  sa.count_handles = 1;
  sa.pad = 0;
  long rr_reset = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_RESET, &sa);
  bzero_n(&sw, sizeof sw);
  sw.handles = (uint64_t)(uintptr_t)&wait_h;
  sw.count_handles = 1;
  sw.timeout_nsec = 0; /* poll: never blocks, never parks */
  long r_poll = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &sw);
  begin(18, "syncobj_reset_wait");
  r_s("reset_ret=");
  r_dec(rr_reset);
  r_s(" wait_errno=");
  r_dec(err_of(r_poll));
  done(rr_reset == 0 && r_poll == -62 /* -ETIME */);

  long r_sig = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_SIGNAL, &sa);
  bzero_n(&sw, sizeof sw);
  sw.handles = (uint64_t)(uintptr_t)&wait_h;
  sw.count_handles = 1;
  sw.timeout_nsec = 0;
  long r_again = ioctl_((int)fd, NVSHIM_DRM_IOC_SYNCOBJ_WAIT, &sw);
  begin(19, "syncobj_signal_wait");
  r_s("signal_ret=");
  r_dec(r_sig);
  r_s(" wait_errno=");
  r_dec(err_of(r_again));
  done(r_sig == 0 && r_again == 0);

  /* --- 20 a named refusal, from EL0 ------------------------------------ */
  long r_nvif = ioctl_((int)fd, NVSHIM_DRM_IOC_NVIF, &sa);
  begin(20, "refuse_nvif");
  r_s("reason=nvif errno=");
  r_dec(err_of(r_nvif));
  done(r_nvif == -38 /* -ENOSYS: refused by name, not skipped */);

  /* --- 21 a number nobody implements ----------------------------------- */
  long r_unk = ioctl_((int)fd, NVSHIM_DRM_IOC_ANY(0x7e, 8), &sa);
  begin(21, "unknown_nr");
  r_s("nr=0x7e errno=");
  r_dec(err_of(r_unk));
  done(r_unk == -25 /* -ENOTTY */);

  /* --- 22 VM_INIT after objects exist is -ENOSYS (the uAPI's own rule) - */
  vmi.kernel_managed_addr = 1ull << 39;
  vmi.kernel_managed_size = 1ull << 39;
  long r_vm2 = ioctl_((int)fd, NVSHIM_DRM_IOC_VM_INIT, &vmi);
  begin(22, "vm_reinit");
  r_s("reason=objects_exist errno=");
  r_dec(err_of(r_vm2));
  done(r_vm2 == -38);

  /* --- 23 pointer translation: the ARG itself is not a user address -----
   * The kernel test hands the handler a kernel pointer it wrote itself, so it
   * can never ask this question.  A client can pass anything; the answer must
   * be -EFAULT and the kernel must survive it (a copy-in to an unproven user
   * address is how a driver takes the CPU down). */
  long r_badarg = ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, (void *)0xdead0000);
  begin(23, "ptr_arg");
  r_s("arg=0xdead0000 errno=");
  r_dec(err_of(r_badarg));
  done(r_badarg == -14 /* -EFAULT */);

  /* --- 24 pointer translation INSIDE a struct (the batched copy path) --- */
  struct nvshim_drm_vm_bind vb_bad;
  bzero_n(&vb_bad, sizeof vb_bad);
  vb_bad.op_count = 1;
  vb_bad.op_ptr = 0xdead1000;
  long r_badop = ioctl_((int)fd, NVSHIM_DRM_IOC_VM_BIND, &vb_bad);
  begin(24, "ptr_inner");
  r_s("op_ptr=0xdead1000 errno=");
  r_dec(err_of(r_badop));
  done(r_badop == -14);

  /* --- 25 and the same for EXEC's push array --------------------------- */
  struct nvshim_drm_exec ex_bad;
  bzero_n(&ex_bad, sizeof ex_bad);
  ex_bad.channel = (uint32_t)ch;
  ex_bad.push_count = 1;
  ex_bad.push_ptr = 0xdead2000;
  long r_badpush = ioctl_((int)fd, NVSHIM_DRM_IOC_EXEC, &ex_bad);
  begin(25, "ptr_exec_push");
  r_s("push_ptr=0xdead2000 errno=");
  r_dec(err_of(r_badpush));
  done(r_badpush == -14);

  /* --- 26 a push VA with no binding behind it --------------------------
   * EXEC names its stream by GPU VA and by nothing else, so the kernel has to
   * resolve it.  An unmapped VA must be refused, not submitted: a submit would
   * accept bytes the kernel never saw (the reason WP-F3 made EXEC read the
   * stream at all). */
  struct nvshim_drm_exec_push push_bad;
  push_bad.va = 0x80000; /* 512 KiB: inside the VA space, outside every bind */
  push_bad.va_len = 64;
  push_bad.flags = 0;
  struct nvshim_drm_exec ex_ub;
  bzero_n(&ex_ub, sizeof ex_ub);
  ex_ub.channel = (uint32_t)ch;
  ex_ub.push_count = 1;
  ex_ub.push_ptr = (uint64_t)(uintptr_t)&push_bad;
  long r_unbound = ioctl_((int)fd, NVSHIM_DRM_IOC_EXEC, &ex_ub);
  begin(26, "exec_unbound_va");
  r_s("push_va=0x80000 errno=");
  r_dec(err_of(r_unbound));
  done(r_unbound == -22);

  /* --- 27 mmap geometry, from the caller's side ------------------------
   * Three shapes DRM refuses and this node must too, each by name: a length
   * past the object, MAP_PRIVATE (a copy is not a mapping of the object), and
   * PROT_EXEC (a command buffer is not code). */
  long g_len = sc6(SYS_MMAP, 0, 0x3000, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   (long)map_handle);
  long g_priv = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, 2 /*PRIVATE*/, fd,
                    (long)map_handle);
  long g_exec = sc6(SYS_MMAP, 0, 4096, PROT_READ | 4 /*EXEC*/, MAP_SHARED, fd,
                    (long)map_handle);
  begin(27, "mmap_geometry");
  r_s("len_past_object=");
  r_dec(err_of(g_len));
  r_s(" private=");
  r_dec(err_of(g_priv));
  r_s(" exec=");
  r_dec(err_of(g_exec));
  done(g_len == -22 && g_priv == -22 && g_exec == -22);

  /* --- 28 a second mapping of one object is the SAME memory ------------ */
  long mva2 = sc6(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (long)map_handle);
  int alias_ok = 0;
  if (mva2 > 0) {
    unsigned char *bo2 = (unsigned char *)mva2;
    for (int i = 0; i < 8; i++) bo2[i] = (unsigned char)(0x11 * (i + 1));
    alias_ok = (mva2 != mva);
    for (int i = 0; i < 8 && alias_ok; i++)
      if (bo[i] != (unsigned char)(0x11 * (i + 1))) alias_ok = 0;
  }
  begin(28, "mmap_alias");
  r_s("va2=");
  if (mva2 > 0) {
    r_hex((unsigned long)mva2);
    r_s(" errno=0");
  } else {
    r_s("(none) errno=");
    r_dec(err_of(mva2));
  }
  done(alias_ok);

  /* --- 29 a second fd is a second client, not the same one -------------
   * One client = one (process-group pid, fd).  The old handle must be foreign
   * to the new fd, and the new fd must be able to make objects of its own. */
  long fd2 = sc3(SYS_OPEN, (long)"/dev/nvshim", O_RDWR, 0);
  struct nvshim_drm_gem_info g2;
  bzero_n(&g2, sizeof g2);
  g2.handle = (uint32_t)bo_handle;
  long r_reuse = fd2 >= 0 ? ioctl_((int)fd2, NVSHIM_DRM_IOC_GEM_INFO, &g2) : -999;
  struct nvshim_drm_gem_new gn2;
  bzero_n(&gn2, sizeof gn2);
  gn2.info.domain = NVSHIM_DOMAIN_CPU | NVSHIM_DOMAIN_MAPPABLE;
  gn2.info.size = 4096;
  long r_new2 = fd2 >= 0 ? ioctl_((int)fd2, NVSHIM_DRM_IOC_GEM_NEW, &gn2) : -999;
  unsigned long h2 = gn2.info.handle;
  begin(29, "two_clients");
  r_s("fd2=");
  r_dec(fd2);
  r_s(" old_handle_errno=");
  r_dec(err_of(r_reuse));
  r_s(" new_handle=");
  r_dec((long)h2);
  done(fd2 > 0 && fd2 != fd && r_reuse == -22 && r_new2 == 0 && h2 != 0 &&
       h2 != bo_handle);
  if (fd2 >= 0) (void)sc1(SYS_CLOSE, fd2); /* and its objects go with it */

  /* --- 30 GEM_CLOSE takes the mapping down BEFORE the frames go back ---
   * The only way to see this from ring 3 is to touch the mapping afterwards.
   * A child does it: a fault kills the child, not the probe, and a child that
   * exits 0 means the mapping outlived its object -- frames a later allocation
   * would reuse. */
  struct nvshim_drm_gem_close gc;
  gc.handle = (uint32_t)bo_handle;
  gc.pad = 0;
  long r_closebo = ioctl_((int)fd, NVSHIM_DRM_IOC_GEM_CLOSE, &gc);
  long cpid = sc1(SYS_FORK, 0);
  long cst = -1;
  if (cpid == 0) {
    volatile unsigned char *dead = (volatile unsigned char *)mva;
    unsigned char v = dead[0]; /* must fault: the object is gone */
    (void)v;
    (void)sc1(SYS_EXIT_GROUP, 0); /* reached only if the mapping survived */
    for (;;) {
    }
  }
  if (cpid > 0) {
    int st = 0;
    (void)sc6(SYS_WAIT4, cpid, (long)&st, 0, 0, 0, 0);
    cst = st;
  }
  begin(30, "gem_close_unmaps");
  r_s("close_ret=");
  r_dec(r_closebo);
  r_s(" child_status=");
  r_dec(cst);
  done(r_closebo == 0 && cpid > 0 && cst != 0);

  /* --- 31 a forked child that inherits the fd --------------------------
   * Linux shares the open file (and so the DRM client) across fork; this node
   * keys a client on (process-group pid, fd), so a child is a DIFFERENT client
   * and the parent's handles are foreign to it.  That is a documented limit,
   * not a silent one -- but it can only be measured from EL0, and a stock nvk
   * that forks a worker and hands it the fd would meet it.  The child reports
   * through its exit code: 0 = "GETPARAM worked, the parent's handle did not". */
  long fpid = sc1(SYS_FORK, 0);
  if (fpid == 0) {
    struct nvshim_drm_getparam qc;
    bzero_n(&qc, sizeof qc);
    qc.param = NVSHIM_PARAM_CHIPSET_ID;
    long rc1 = ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, &qc);
    struct nvshim_drm_gem_info gc2;
    bzero_n(&gc2, sizeof gc2);
    gc2.handle = (uint32_t)bo_handle;
    long rc2 = ioctl_((int)fd, NVSHIM_DRM_IOC_GEM_INFO, &gc2);
    (void)sc1(SYS_EXIT_GROUP, (rc1 == 0 && rc2 == -22) ? 0 : 1);
    for (;;) {
    }
  }
  long fst = -1;
  if (fpid > 0) {
    int st = 0;
    (void)sc6(SYS_WAIT4, fpid, (long)&st, 0, 0, 0, 0);
    fst = st;
  }
  begin(31, "fork_inherit_fd");
  r_s("inherited_fd=");
  r_dec(fd);
  r_s(" child_exit=");
  r_dec(fst);
  done(fpid > 0 && fst == 0);

  /* --- 32 fd lifetime: closing the node ends the client ----------------
   * Linux answers a closed fd with EBADF on every ioctl.  Anything else (a
   * fall-through to ENOTTY, which is what a request-keyed route gives when the
   * fd is gone) hides a use-after-close from the caller. */
  (void)sc1(SYS_CLOSE, fd);
  long r_after = ioctl_((int)fd, NVSHIM_DRM_IOC_GETPARAM, &q);
  begin(32, "close_ioctl");
  r_s("closed_fd=");
  r_dec(fd);
  r_s(" errno=");
  r_dec(err_of(r_after));
  done(r_after == -9 /* -EBADF, as Linux */);

  /* --- 33 and a NEW fd is a NEW client: the old handle is gone --------- */
  long fd3 = sc3(SYS_OPEN, (long)"/dev/nvshim", O_RDWR, 0);
  struct nvshim_drm_gem_info g3;
  bzero_n(&g3, sizeof g3);
  g3.handle = (uint32_t)bo_handle;
  long r_reuse2 = fd3 >= 0 ? ioctl_((int)fd3, NVSHIM_DRM_IOC_GEM_INFO, &g3) : -999;
  begin(33, "fd_lifetime");
  r_s("new_fd=");
  r_dec(fd3);
  r_s(" old_handle=");
  r_dec((long)bo_handle);
  r_s(" errno=");
  r_dec(err_of(r_reuse2));
  done(fd3 >= 0 && r_reuse2 == -22);
  if (fd3 >= 0) (void)sc1(SYS_CLOSE, fd3);

  /* --- verdict --------------------------------------------------------- */
  r_reset();
  r_s("[EL0] RESULT ");
  r_s(g_fail == 0 ? "PASS " : "FAIL ");
  r_dec(g_pass);
  r_c('/');
  r_dec(g_steps);
  if (g_fail) {
    r_s(" failed=");
    r_dec(g_fail);
  }
  r_emit();
  return g_fail == 0 ? 0 : 1;
}

#else /* !__x86_64__ */

/* The Linux-ABI track is x86_64-only (linux_compat_fd.c), so this program has
 * no meaning on AArch64.  An empty TU keeps a stray ARM compile honest. */
typedef int nvabi_probe_not_for_this_arch;

#endif
