/*
 * src/kernel/linux_exec.c - the compat (Linux-ABI) execve(2) / execveat(2)
 * (cp/exec, docs/cp-exec.md; contract in src/include/linux_exec.h).
 *
 * Wine re-execs itself (wine -> wine-preloader -> wine64) and starts
 * wineserver with fork+exec, so a Linux program must be able to replace its
 * own image with any ELF it can name.  The native SYS_EXEC path
 * (process_exec_current) maps flat FAT16 images through a 32-byte path; this
 * file is the Linux one.  It owns no loader of its own: the ELF is mapped by
 * the spawn loader's elf_loader_map_buffers(), the interpreter is read by the
 * shared elf_loader_read_interp(), and the auxv by compat_auxv_fill(), so a
 * spawned and an exec'd Linux program start from the same bytes.
 *
 * Order (Linux fs/exec.c, the point of no return marked):
 *   1. copy the path (VFS_PATH_MAX) and resolve it (cwd or dirfd);
 *   2. copy argv/envp into kernel frames (E2BIG / EFAULT);
 *   3. read the image whole from its filesystem (ENOENT/EACCES/ENOEXEC),
 *      following "#!" up to LX_SHEBANG_DEPTH times;
 *   4. parse it, read and parse its PT_INTERP (ENOENT / ELIBBAD);
 *   --- point of no return ---
 *   5. kill every other thread of the group; release the robust list;
 *   6. tear the address space down and build a fresh COMPAT one in its slot
 *      (C1-mmu layout: floor 0x10000, the kernel image a hole);
 *   7. map the image (+ interpreter), the main stack and the heap slot;
 *   8. lay the SysV startup block (argc/argv/envp/auxv) on the new stack;
 *   9. reset what Linux resets (handlers, altstack, FS/GS base, FPU, CLOEXEC
 *      fds, clear_child_tid), keep what it keeps (pid/tgid, other fds, the
 *      signal mask, pending signals, cwd), point /proc/self/exe at the image;
 *  10. enter it: rip/rsp from the startup block, every GPR zero.
 * A failure in 6-8 kills the process with SIGSEGV, as Linux does.
 *
 * Kernel constraints: freestanding, byte-wise copies, no FP/SIMD; user memory
 * only through vm_user_read / vm_kwrite.  x86_64 only (the compat class);
 * the ARM build compiles the stub at the bottom.
 */

#include "linux_exec.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "arch/trap.h"
#include "elf_image.h"
#include "errno.h"
#include "ext2_vfs.h"
#include "fat16.h"
#include "frame.h"
#include "fs.h"
#include "kline.h"
#include "kmem.h"
#include "linux_compat.h"
#include "linux_syscall.h"
#include "net.h"
#include "process.h"
#include "program_loader.h"
#include "tmpfs.h"
#include "vfs.h"
#include "vm.h"

#if defined(__x86_64__)

extern void uart_puts(const char *s);
extern void arch_fpu_reset(struct process *p);
extern void arch_fpu_restore(struct process *p);
extern void linux_compat_thread_exit(struct process *p);
extern void linux_compat_sig_exec(struct process *p);

#define LX_SHEBANG_DEPTH 4     /* Linux: bprm->interp recursion > 4 is ELOOP */
#define LX_SHEBANG_BUF 256     /* BINPRM_BUF_SIZE */
#define LX_SIGSEGV 11

/* ---- small helpers ------------------------------------------------------ */

static uint64_t lx_strlen(const char *s) {
  uint64_t n = 0;
  while (s[n]) n++;
  return n;
}

static void lx_copy(uint8_t *d, const uint8_t *s, uint64_t n) {
  for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static void lx_free_run(uint8_t *va, uint64_t pages) {
  if (!va) return;
  uint64_t pa = virt_to_phys(va);
  for (uint64_t i = 0; i < pages; i++) frame_free(pa + i * FRAME_SIZE);
}

static uint8_t *lx_alloc_run(uint64_t bytes, uint64_t *pages_out) {
  uint64_t pages = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
  if (pages == 0) pages = 1;
  uint64_t pa = frame_alloc_contig((int)pages);
  if (!pa) return 0;
  *pages_out = pages;
  return (uint8_t *)phys_to_virt(pa);
}

/* One evidence row per exec (bounded): the gate reads them from the serial. */
static int lx_rows;
static void lx_row(const char *what, struct process *p, const char *path,
                   long a, long b) {
  if (lx_rows >= 64) return;
  lx_rows++;
  struct kline k;
  kl_init(&k);
  kl_s(&k, "[EXEC] ");
  kl_s(&k, what);
  kl_s(&k, " pid=");
  kl_i(&k, p ? p->pid : -1);
  kl_s(&k, " ");
  kl_s(&k, path ? path : "?");
  kl_s(&k, " ");
  kl_i(&k, a);
  kl_s(&k, " ");
  kl_i(&k, b);
  kl_c(&k, '\n');
  kl_emit(&k);
}

/* ---- argv / envp -------------------------------------------------------- *
 * One frame run holds every string back to back: argc argv strings, then envc
 * environment strings, each with its NUL.  The two-pass copy (measure, then
 * copy into an exactly-sized run) keeps the common case (a few KiB) at one or
 * two frames, and the measured lengths bound the copy even if another thread
 * rewrites the strings in between (the last byte is forced to NUL). */
struct lx_args {
  uint8_t *buf;
  uint64_t pages;
  uint64_t len;
  uint64_t argc, envc;
};

static void lx_args_free(struct lx_args *a) {
  lx_free_run(a->buf, a->pages);
  a->buf = 0;
  a->pages = 0;
}

/* strnlen over user memory, a chunk at a time (vm_user_read per byte would
 * walk the page table per byte).  Returns the length (excluding the NUL) or
 * -EFAULT; a string of `max` bytes or more without a NUL is -E2BIG. */
static int64_t lx_ustrlen(struct process *p, uint64_t va, uint64_t max) {
  uint8_t chunk[128];
  uint64_t n = 0;
  for (;;) {
    uint64_t take = sizeof chunk;
    uint64_t room = FRAME_SIZE - ((va + n) & (FRAME_SIZE - 1));
    if (take > room) take = room;
    if (vm_user_read(p, va + n, chunk, (int)take) != 0) {
      /* The chunk may cross into an unmapped page past the NUL: retry
         byte-wise before calling it a fault. */
      for (uint64_t i = 0; i < take; i++) {
        uint8_t c;
        if (vm_user_read(p, va + n, &c, 1) != 0) return -EFAULT;
        if (c == 0) return (int64_t)n;
        if (++n >= max) return -E2BIG;
      }
      continue;
    }
    for (uint64_t i = 0; i < take; i++) {
      if (chunk[i] == 0) return (int64_t)(n + i);
    }
    n += take;
    if (n >= max) return -E2BIG;
  }
}

static int lx_uptr(struct process *p, uint64_t arr, uint64_t i, uint64_t *out) {
  uint8_t b[8];
  if (vm_user_read(p, arr + i * 8, b, 8) != 0) return -EFAULT;
  uint64_t v = 0;
  for (int k = 0; k < 8; k++) v |= (uint64_t)b[k] << (8 * k);
  *out = v;
  return 0;
}

/* Count one array (argv or envp; NULL array == empty) and the bytes of its
 * strings.  *total carries the running Linux accounting (strings + one
 * pointer each). */
static int lx_measure(struct process *p, uint64_t arr, uint64_t *count,
                      uint64_t *total) {
  *count = 0;
  if (!arr) return 0;
  for (uint64_t i = 0;; i++) {
    uint64_t s;
    if (lx_uptr(p, arr, i, &s) != 0) return -EFAULT;
    if (!s) break;
    int64_t len = lx_ustrlen(p, s, LX_EXEC_STRLEN_MAX);
    if (len < 0) return (int)len;
    *total += (uint64_t)len + 1 + 8;
    if (*total > LX_EXEC_ARG_MAX) return -E2BIG;
    (*count)++;
  }
  return 0;
}

static int lx_copy_strings(struct process *p, uint64_t arr, uint64_t count,
                           struct lx_args *a, uint64_t cap) {
  for (uint64_t i = 0; i < count; i++) {
    uint64_t s;
    if (lx_uptr(p, arr, i, &s) != 0) return -EFAULT;
    if (!s) return -EFAULT; /* the array shrank under us */
    int64_t len = lx_ustrlen(p, s, LX_EXEC_STRLEN_MAX);
    if (len < 0) return (int)len;
    if (a->len + (uint64_t)len + 1 > cap) return -E2BIG; /* it grew */
    if (len > 0 && vm_user_read(p, s, a->buf + a->len, (int)len) != 0)
      return -EFAULT;
    a->len += (uint64_t)len;
    a->buf[a->len++] = 0;
  }
  return 0;
}

static int lx_args_load(struct process *p, uint64_t uargv, uint64_t uenvp,
                        uint64_t extra, struct lx_args *a) {
  a->buf = 0;
  a->pages = 0;
  a->len = 0;
  uint64_t total = extra;
  int rc = lx_measure(p, uargv, &a->argc, &total);
  if (rc == 0) rc = lx_measure(p, uenvp, &a->envc, &total);
  if (rc != 0) return rc;
  /* Room for the strings; the #! rewrite below may prepend up to three. */
  uint64_t cap = total + 3 * (LX_SHEBANG_BUF + 8) + 64;
  a->buf = lx_alloc_run(cap, &a->pages);
  if (!a->buf) return -ENOMEM;
  cap = a->pages * FRAME_SIZE;
  rc = lx_copy_strings(p, uargv, a->argc, a, cap);
  if (rc == 0) rc = lx_copy_strings(p, uenvp, a->envc, a, cap);
  if (rc != 0) lx_args_free(a);
  return rc;
}

/* Offset of the string after the one at `off`. */
static uint64_t lx_next(const struct lx_args *a, uint64_t off) {
  while (off < a->len && a->buf[off]) off++;
  return off + 1;
}

/* "#!" rewrite: argv becomes [interp, (iarg), filename, argv[1..]] -- argv[0]
 * is dropped, as Linux's binfmt_script does (bprm->argc-- after
 * remove_arg_zero).  Rebuilt into a fresh run so the old one can be freed. */
static int lx_args_shebang(struct lx_args *a, const char *interp,
                           const char *iarg, const char *filename) {
  uint64_t li = lx_strlen(interp), la = iarg ? lx_strlen(iarg) : 0;
  uint64_t lf = lx_strlen(filename);
  uint64_t skip = a->argc ? lx_next(a, 0) : 0; /* bytes of argv[0] */
  uint64_t need = a->len - skip + li + 1 + (iarg ? la + 1 : 0) + lf + 1;
  uint64_t acct = need + (a->argc + a->envc + 2) * 8;
  if (acct > LX_EXEC_ARG_MAX) return -E2BIG;
  struct lx_args n;
  n.buf = lx_alloc_run(need, &n.pages);
  if (!n.buf) return -ENOMEM;
  n.len = 0;
  lx_copy(n.buf + n.len, (const uint8_t *)interp, li + 1);
  n.len += li + 1;
  n.argc = 1;
  if (iarg) {
    lx_copy(n.buf + n.len, (const uint8_t *)iarg, la + 1);
    n.len += la + 1;
    n.argc++;
  }
  lx_copy(n.buf + n.len, (const uint8_t *)filename, lf + 1);
  n.len += lf + 1;
  n.argc++;
  lx_copy(n.buf + n.len, a->buf + skip, a->len - skip);
  n.len += a->len - skip;
  n.argc += a->argc ? a->argc - 1 : 0;
  n.envc = a->envc;
  lx_args_free(a);
  *a = n;
  return 0;
}

/* ---- reading the image -------------------------------------------------- */

struct lx_image {
  uint8_t *buf;
  uint32_t size;
  uint64_t pages;
};

static void lx_image_free(struct lx_image *im) {
  lx_free_run(im->buf, im->pages);
  im->buf = 0;
  im->pages = 0;
}

/* Read the file at canonical `abs` (a VFS_PATH_MAX buffer, rewritten when a
 * symlink leaves tmpfs/ext2, and to the physical path on tmpfs) whole into
 * kernel frames.  The filesystem is chosen exactly as open() chooses it. */
static int lx_read_file(char *abs, struct lx_image *im) {
  im->buf = 0;
  im->pages = 0;
  im->size = 0;
  uint8_t *b = 0;
  uint32_t sz = 0, pg = 0;
  for (int hop = 0; hop < 8; hop++) {
    if (tmpfs_owns(abs)) {
      int r = tmpfs_read_image(abs, &b, &sz, &pg, (uint32_t)USER_IMG_SIZE);
      if (r == TMPFS_EXTERNAL) continue; /* abs names the target now */
      if (r != 0) return r;
      goto got;
    }
    if (ext2v_owns(abs)) {
      int r = ext2v_read_image(abs, &b, &sz, &pg, (uint32_t)USER_IMG_SIZE);
      if (r == EXT2_EXTERNAL) continue;
      if (r != 0) return r;
      goto got;
    }
    {
      int midx = -1;
      const char *rel = 0;
      if (vfs_route(abs, &midx, &rel)) {
        int r = fs_read_image(abs, &b, &sz, &pg);
        if (r == 1) goto got;
        return -ENOENT; /* the NFS reader reports no errno */
      }
    }
    /* FAT16.  fat16_resolve_path never creates (fat16_open does on a miss). */
    {
      struct fat16_dir_entry e;
      uint32_t sec = 0, eoff = 0;
      if (fat16_resolve_path(abs, &e, &sec, &eoff) != 0 || sec == 0)
        return -ENOENT;
      if (e.attr & 0x10) return -EACCES; /* a directory */
      if (e.file_size == 0 || (uint64_t)e.file_size > USER_IMG_SIZE)
        return -ENOEXEC;
      uint64_t p2 = 0;
      b = lx_alloc_run(e.file_size, &p2);
      if (!b) return -ENOMEM;
      if (fat16_read_direct_pos(e.start_cluster, e.file_size, virt_to_phys(b),
                                (int)e.file_size, 0) != (int)e.file_size) {
        lx_free_run(b, p2);
        return -EIO;
      }
      sz = e.file_size;
      pg = (uint32_t)p2;
      goto got;
    }
  }
  return -ELOOP;
got:
  im->buf = b;
  im->size = sz;
  im->pages = pg;
  return 0;
}

/* The PT_INTERP image.  A path on tmpfs or under an NFS mount is read like
 * any file (Linux resolves the interpreter as an ordinary path); everything
 * else goes through the spawn loader's reader (ext2, then the FAT16 resolver
 * with its basename / 8.3 alias table), so spawn and exec agree on which file
 * a glibc /lib64/ld-linux-x86-64.so.2 names. */
static int lx_read_interp(const char *name, struct lx_image *im) {
  im->buf = 0;
  im->pages = 0;
  im->size = 0;
  if (!name || name[0] != '/') return -ENOENT;
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(name, abs, sizeof abs) != 0) return -ENAMETOOLONG;
  int midx = -1;
  const char *rel = 0;
  if (tmpfs_owns(abs) || vfs_route(abs, &midx, &rel)) {
    int r = lx_read_file(abs, im);
    if (r == -EACCES) r = -EACCES; /* Linux: the interpreter needs read only,
                                      but an unreadable one is EACCES too */
    return r;
  }
  uint8_t *b = 0;
  uint64_t len = 0, pages = 0;
  char staged[64];
  int r = elf_loader_read_interp(name, &b, &len, &pages, staged,
                                 (int)sizeof staged, 0);
  if (r == -ENOSPC) r = -ENOEXEC;
  if (r != 0) return r;
  im->buf = b;
  im->size = (uint32_t)len;
  im->pages = pages;
  return 0;
}

/* "#!interp [arg]\n" (Linux binfmt_script): leading blanks skipped, one
 * optional argument (the rest of the line, trailing blanks stripped).  0 and
 * the two strings, or -ENOEXEC for an empty interpreter. */
static int lx_parse_shebang(const uint8_t *b, uint32_t size, char *interp,
                            char *iarg, int *has_arg) {
  uint32_t end = size < LX_SHEBANG_BUF ? size : LX_SHEBANG_BUF;
  uint32_t i = 2;
  uint32_t eol = i;
  while (eol < end && b[eol] != '\n') eol++;
  /* Linux 5.x: a line that fills the buffer without a newline is still used
     when the interpreter itself fits; keep it simple and truncate. */
  while (i < eol && (b[i] == ' ' || b[i] == '\t')) i++;
  uint32_t s = i;
  while (i < eol && b[i] != ' ' && b[i] != '\t') i++;
  if (i == s) return -ENOEXEC;
  uint32_t n = 0;
  for (uint32_t k = s; k < i && n < LX_SHEBANG_BUF - 1; k++) interp[n++] = (char)b[k];
  interp[n] = 0;
  while (i < eol && (b[i] == ' ' || b[i] == '\t')) i++;
  uint32_t ae = eol;
  while (ae > i && (b[ae - 1] == ' ' || b[ae - 1] == '\t' || b[ae - 1] == '\r')) ae--;
  *has_arg = ae > i;
  n = 0;
  for (uint32_t k = i; k < ae && n < LX_SHEBANG_BUF - 1; k++) iarg[n++] = (char)b[k];
  iarg[n] = 0;
  return 0;
}

/* ---- path resolution ---------------------------------------------------- */

/* Is the final component of `abs` a symbolic link?  (AT_SYMLINK_NOFOLLOW.) */
static int lx_is_symlink(char *abs) {
  struct k_stat st;
  char tmp[VFS_PATH_MAX];
  for (int i = 0; i < VFS_PATH_MAX; i++) {
    tmp[i] = abs[i];
    if (!abs[i]) break;
  }
  tmp[VFS_PATH_MAX - 1] = 0;
  int r = -1;
  if (tmpfs_owns(tmp)) r = tmpfs_stat(tmp, 0, &st);
  else if (ext2v_owns(tmp)) r = ext2v_stat(tmp, 0, &st);
  if (r != 0) return 0;
  return (st.st_mode & 0170000) == 0120000;
}

/* path (as given) + dirfd -> canonical absolute path. */
static int lx_resolve(struct process *p, int dirfd, const char *path,
                      int flags, char *abs) {
  if (path[0] == '\0') {
    if (!(flags & L_AT_EMPTY_PATH)) return -ENOENT;
    if (dirfd == L_AT_FDCWD) return -ENOENT; /* Linux: the cwd is not a file */
    int r = file_path_of(process_group(p), dirfd, abs, VFS_PATH_MAX);
    return r == -EBADF ? -EBADF : (r != 0 ? -ENOENT : 0);
  }
  if (path[0] == '/' || dirfd == L_AT_FDCWD)
    return vfs_abs_path(path, abs, VFS_PATH_MAX) == 0 ? 0 : -ENAMETOOLONG;
  char dir[VFS_PATH_MAX];
  int r = file_path_of(process_group(p), dirfd, dir, (int)sizeof dir);
  if (r == -EBADF) return -EBADF;
  if (r != 0) return -ENOTDIR;
  struct k_stat st;
  int err = 0;
  if (file_stat_fd(p, dirfd, &st, &err) == 0 &&
      (st.st_mode & 0170000) != 0040000)
    return -ENOTDIR;
  uint64_t dl = lx_strlen(dir), pl = lx_strlen(path);
  if (dl + 1 + pl + 1 > VFS_PATH_MAX) return -ENAMETOOLONG;
  char joined[VFS_PATH_MAX];
  uint64_t n = 0;
  for (uint64_t i = 0; i < dl; i++) joined[n++] = dir[i];
  if (n == 0 || joined[n - 1] != '/') joined[n++] = '/';
  for (uint64_t i = 0; i < pl; i++) joined[n++] = path[i];
  joined[n] = 0;
  return vfs_abs_path(joined, abs, VFS_PATH_MAX) == 0 ? 0 : -ENAMETOOLONG;
}

/* ---- the startup block --------------------------------------------------- *
 * The spawn loader's layout (program_loader.c compat_stack_block), without its
 * 16-argv / 24-env / 512-byte bounds and without its LD_LIBRARY_PATH fallback
 * (execve passes exactly the environment it is given):
 *
 *   top -> AT_EXECFN string, "x86_64", the argv+envp strings, AT_RANDOM[16],
 *          auxv (16-aligned top), envp[], NULL, argv[] (argv[0] 8 mod 16),
 *   rsp -> argc                                   (16-aligned)
 */
static int lx_put(struct process *p, uint64_t va, const void *src, uint64_t n) {
  const uint8_t *s = (const uint8_t *)src;
  while (n) {
    uint64_t take = n > (1u << 20) ? (1u << 20) : n;
    if (vm_kwrite(p, va, s, (int)take) != 0) return -EFAULT;
    va += take;
    s += take;
    n -= take;
  }
  return 0;
}

static int lx_stack_block(struct process *p, const struct lx_args *a,
                          const char *execfn, const struct elf_load_info *li,
                          uint64_t *sp_out) {
  uint64_t top = USER_MAIN_STK_TOP_V2;
  uint64_t sp = top;
  uint64_t fl = lx_strlen(execfn);
  sp -= fl + 1;
  uint64_t execfn_addr = sp;
  if (lx_put(p, sp, execfn, fl + 1) != 0) return -EFAULT;
  sp -= 7;
  uint64_t platform_addr = sp;
  if (lx_put(p, sp, "x86_64", 7) != 0) return -EFAULT;
  sp -= a->len;
  uint64_t strings = sp;
  if (a->len && lx_put(p, sp, a->buf, a->len) != 0) return -EFAULT;

  uint64_t rnd_addr = (sp - 16) & ~(uint64_t)0xF;
  uint8_t rnd[16];
  net_get_random_bytes(rnd, 16);
  if (lx_put(p, rnd_addr, rnd, 16) != 0) return -EFAULT;

  uint64_t w[COMPAT_AUXV_WORDS];
  int k = compat_auxv_fill(w, li->phdr, li->phnum, li->interp_base,
                           li->exe_entry, platform_addr, rnd_addr, execfn_addr);
  uint64_t argc = a->argc;
  uint64_t arrays = (argc + 1) * 8 + (a->envc + 1) * 8;
  uint64_t auxv_top = (rnd_addr - (uint64_t)k * 8) & ~(uint64_t)0xF;
  uint64_t argv_addr = (auxv_top - arrays) & ~(uint64_t)0x7;
  if ((argv_addr & 0xF) == 0) argv_addr -= 8; /* argv[0] at 8 mod 16 */
  uint64_t envp_addr = argv_addr + (argc + 1) * 8;
  uint64_t auxv_addr = envp_addr + (a->envc + 1) * 8;
  sp = argv_addr - 8;
  if (sp < USER_MAIN_STK_LIMIT_V2 + 0x10000) return -E2BIG;
  /* The x64 resume tail runs on [sp - 320, sp) at CPL0 and cannot
     demand-page; everything above sp is written below (vm_kwrite
     materializes), so commit from a page below sp to the top. */
  uint64_t lo = (sp - 0x1000) & ~(uint64_t)0xFFF;
  if (vm_touch(p, lo, top - lo, 1) != 0) return -ENOMEM;
  if (lx_put(p, auxv_addr, w, (uint64_t)k * 8) != 0) return -EFAULT;

  /* argv[] then envp[] pointers, in chunks. */
  uint64_t ptr[64];
  uint64_t off = 0, at = argv_addr;
  uint64_t total = argc + a->envc;
  uint64_t idx = 0;
  while (idx <= total) { /* argc + envc strings, plus the argv NULL */
    int n = 0;
    while (n < 64 && idx <= total) {
      if (idx == argc) {
        ptr[n++] = 0; /* argv terminator */
      } else {
        ptr[n++] = strings + off;
        off = lx_next(a, off);
      }
      idx++;
    }
    if (lx_put(p, at, ptr, (uint64_t)n * 8) != 0) return -EFAULT;
    at += (uint64_t)n * 8;
  }
  uint64_t z = 0;
  if (lx_put(p, at, &z, 8) != 0) return -EFAULT; /* envp terminator */
  if (lx_put(p, sp, &argc, 8) != 0) return -EFAULT;
  *sp_out = sp;
  return 0;
}

/* ---- the PCB side -------------------------------------------------------- */

/* Mirror the (bounded) argv/env into the PCB blobs: `ps`, the native
 * SYS_GETARGV/GETENV readers and a later native spawn see the new program.
 * Truncated at the PCB sizes; the authoritative copy is the user stack. */
static void lx_pcb_blobs(struct process *grp, struct process *cur,
                         const struct lx_args *a) {
  /* Scratch for the two blobs (4.5 KiB: too big for the stack next to the
     loader's frames).  Without it the mirror is skipped, never the reset. */
  uint64_t spg = 0;
  uint8_t *scratch = lx_alloc_run(HO_EXEC_ARGV_LEN + HO_ENV_LEN, &spg);
  char *blob = (char *)scratch;
  char *env = scratch ? (char *)scratch + HO_EXEC_ARGV_LEN : 0;
  int argc = 0, envc = 0;
  if (scratch) {
    uint64_t off = 0;
    int pos = 0;
    for (uint64_t i = 0; i < a->argc; i++) {
      uint64_t l = lx_strlen((const char *)a->buf + off);
      if ((uint64_t)pos + l + 1 >= HO_EXEC_ARGV_LEN || argc >= HO_EXEC_MAX_ARGS) break;
      lx_copy((uint8_t *)blob + pos, a->buf + off, l + 1);
      pos += (int)l + 1;
      argc++;
      off += l + 1;
    }
    blob[pos] = 0;
    off = 0;
    for (uint64_t i = 0; i < a->argc; i++) off = lx_next(a, off);
    int epos = 0;
    for (int i = 0; i < HO_ENV_LEN; i++) env[i] = 0;
    for (uint64_t i = 0; i < a->envc; i++) {
      uint64_t l = lx_strlen((const char *)a->buf + off);
      if ((uint64_t)epos + l + 1 >= HO_ENV_LEN) break;
      lx_copy((uint8_t *)env + epos, a->buf + off, l + 1);
      epos += (int)l + 1;
      envc++;
      off += l + 1;
    }
  }
  /* The env blob and the signal/CLOEXEC reset share process_exec_apply; Linux
     keeps pending signals across exec, which that helper clears (native P5
     contract), so carry them over. */
  uint64_t pend = grp->sig_pending, pend_dir = cur->sig_pending_dir;
  process_exec_apply(grp, env, envc);
  grp->sig_pending = pend;
  cur->sig_pending_dir = pend_dir;
  proc_set_argv_array(grp, blob, argc);
  /* Flat args (get_args()): argv[1..] space-joined. */
  int fi = 0;
  uint64_t off = a->argc ? lx_next(a, 0) : 0;
  for (uint64_t i = 1; i < a->argc && fi < HO_FLAT_ARGS_LEN - 1; i++) {
    if (fi) cur->args[fi++] = ' ';
    while (off < a->len && a->buf[off] && fi < HO_FLAT_ARGS_LEN - 1)
      cur->args[fi++] = (char)a->buf[off++];
    off = lx_next(a, off);
  }
  cur->args[fi] = 0;
  lx_free_run(scratch, spg);
}

static void lx_set_name(struct process *pcb, const char *abs) {
  const char *b = abs;
  for (const char *q = abs; *q; q++)
    if (*q == '/') b = q + 1;
  int i = 0;
  for (; b[i] && i < 31; i++) pcb->name[i] = b[i];
  pcb->name[i] = 0;
}

/* ---- execve ---------------------------------------------------------------- */

int linux_execve(struct process *p, struct trap_frame *tf, int dirfd,
                 uint64_t upath, uint64_t uargv, uint64_t uenvp, int flags) {
  if (!p || !tf) return -EINVAL;
  struct process *cur = p;
  struct process *grp = process_group(cur);
  if (!grp || !grp->as) return -EINVAL;
  if (flags & ~(L_AT_EMPTY_PATH | L_AT_SYMLINK_NOFOLLOW)) return -EINVAL;

  /* 1. the path */
  char path[VFS_PATH_MAX];
  int rc = l_copy_cstr(cur, path, upath, sizeof path);
  if (rc != 0) return rc; /* -EFAULT / -ENAMETOOLONG */
  char abs[VFS_PATH_MAX];
  rc = lx_resolve(cur, dirfd, path, flags, abs);
  if (rc != 0) return rc;
  if ((flags & L_AT_SYMLINK_NOFOLLOW) && lx_is_symlink(abs)) return -ELOOP;
  /* AT_EXECFN / the #! filename: the path as given (Linux bprm->filename);
     for an fd-relative or empty path, "/dev/fd/N[/path]". */
  char execfn[VFS_PATH_MAX];
  if (path[0] == '/' || dirfd == L_AT_FDCWD) {
    for (int i = 0; i < VFS_PATH_MAX; i++) {
      execfn[i] = path[i];
      if (!path[i]) break;
    }
  } else {
    static const char pfx[] = "/dev/fd/";
    int n = 0;
    for (; pfx[n]; n++) execfn[n] = pfx[n];
    char num[12];
    int nn = 0;
    unsigned v = (unsigned)dirfd;
    do { num[nn++] = (char)('0' + v % 10); v /= 10; } while (v && nn < 11);
    while (nn) execfn[n++] = num[--nn];
    if (path[0]) {
      execfn[n++] = '/';
      for (int i = 0; path[i] && n < VFS_PATH_MAX - 1; i++) execfn[n++] = path[i];
    }
    execfn[n] = 0;
  }

  /* 2. argv / envp (the filename counts toward the limit, as in Linux) */
  struct lx_args args;
  rc = lx_args_load(cur, uargv, uenvp, lx_strlen(execfn) + 1, &args);
  if (rc != 0) return rc;

  /* 3. the image, through "#!" */
  struct lx_image im = {0, 0, 0}, ii = {0, 0, 0};
  int depth = 0;
  for (;;) {
    rc = lx_read_file(abs, &im);
    if (rc != 0) goto fail_pre;
    if (im.size >= 4 && im.buf[0] == 0x7f && im.buf[1] == 'E' &&
        im.buf[2] == 'L' && im.buf[3] == 'F')
      break;
    if (im.size >= 2 && im.buf[0] == '#' && im.buf[1] == '!') {
      if (++depth > LX_SHEBANG_DEPTH) { rc = -ELOOP; goto fail_pre; }
      char interp[LX_SHEBANG_BUF], iarg[LX_SHEBANG_BUF];
      int has_arg = 0;
      rc = lx_parse_shebang(im.buf, im.size, interp, iarg, &has_arg);
      lx_image_free(&im);
      if (rc != 0) goto fail_pre;
      rc = lx_args_shebang(&args, interp, has_arg ? iarg : 0, execfn);
      if (rc != 0) goto fail_pre;
      if (vfs_abs_path(interp, abs, sizeof abs) != 0) { rc = -ENAMETOOLONG; goto fail_pre; }
      continue;
    }
    rc = -ENOEXEC;
    goto fail_pre;
  }

  /* 4. parse; the interpreter */
  struct elf_image eimg;
  rc = elf_parse_ex(im.buf, im.size, &eimg, ELF_PARSE_ALLOW_INTERP);
  if (rc != ELF_OK) {
    lx_row("refused", cur, abs, rc, im.size);
    rc = -ENOEXEC;
    goto fail_pre;
  }
  if (eimg.has_interp && elf_interp_is_native(eimg.interp, eimg.interp_len)) {
    rc = -ENOEXEC; /* a native HobbyOS (LDHOBBY.SO) program: not this ABI */
    goto fail_pre;
  }
  if (elf_loader_probe_native_buf(im.buf, im.size) == 1) {
    rc = -ENOEXEC;
    goto fail_pre;
  }
  if (eimg.has_interp) {
    rc = lx_read_interp(eimg.interp, &ii);
    if (rc != 0) goto fail_pre;
    struct elf_image iimg;
    if (ii.size < 4 || ii.buf[0] != 0x7f || ii.buf[1] != 'E' ||
        ii.buf[2] != 'L' || ii.buf[3] != 'F' ||
        elf_parse(ii.buf, ii.size, &iimg) != ELF_OK) {
      rc = -ELIBBAD;
      goto fail_pre;
    }
  }

  /* ---- point of no return ---- */
  lx_row("load", cur, abs, (long)args.argc, (long)args.envc);

  /* 5. other threads; the robust list (Linux exec_mm_release) */
  if (cur->is_thread || grp->live_threads > 1)
    process_exec_terminate_siblings(grp, cur);
  linux_compat_thread_exit(cur);
  cur->clear_child_tid = 0;
  /* C1-k4: from a secondary thread, continue as the leader (tid == pid). */
  cur = process_exec_become_leader(grp, cur);

  /* 6. a fresh compat address space in the same (tgid-indexed) slot */
  vm_arch_restore_kernel();
  /* C1-thr: a vfork/posix_spawn child runs in its parent's AS until here;
     give it back (and wake the parent) BEFORE tearing anything down, or the
     teardown below would destroy the parent's memory.  After this grp->as
     is the child's own (empty) AS. */
  process_vfork_release(grp);
  {
    struct addr_space *old = grp->as;
    grp->as = 0;
    vm_as_teardown(old);
  }
  struct addr_space *nas = vm_as_create((uint64_t)grp->pid);
  if (!nas) goto fail_post;
  if (CP_COMPAT_AS && vm_as_set_layout(nas, VM_LAYOUT_COMPAT) != 0)
    uart_puts("[EXEC] compat layout refused; native layout\n");
  grp->as = nas;
  vm_arch_switch(nas);
  grp->heap_brk = USER_HEAP_BASE_V2;
  grp->anon_map_count = 0;

  /* 7. image + interpreter, stack, heap */
  struct elf_load_info li;
  int mrc = elf_loader_map_buffers(nas, im.buf, im.size, ii.buf, ii.size,
                                   ELF_LOAD_COMPAT_INTERP, &li);
  if (mrc != 0) {
    lx_row("map-failed", cur, abs, mrc, li.parse_rc);
    goto fail_post;
  }
  lx_image_free(&im);
  lx_image_free(&ii);
  if (vm_region_insert(nas, USER_MAIN_STK_LIMIT_V2, USER_MAIN_STK_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_STACK, VM_MAP_PRIVATE,
                       0, 0) != 0)
    goto fail_post;
  if (vm_region_insert(nas, USER_HEAP_BASE_V2, USER_HEAP_SIZE,
                       VM_PROT_READ | VM_PROT_WRITE, VMK_HEAP, VM_MAP_PRIVATE,
                       0, 0) != 0)
    goto fail_post;

  /* 8. the startup block */
  uint64_t sp = 0;
  if (lx_stack_block(cur, &args, execfn, &li, &sp) != 0) goto fail_post;

  /* 9. the state Linux resets / keeps */
  lx_pcb_blobs(grp, cur, &args);
  linux_compat_sig_exec(cur);
  lx_set_name(cur, abs);
  if (grp != cur) lx_set_name(grp, abs);
  set_exe_path(cur, abs);
  if (grp != cur) set_exe_path(grp, abs);
  grp->abi_flags |= PROC_FLAG_LINUX_ABI;
  cur->tls_base = 0; /* FS base 0 (process_set_tls refuses 0: it checks a map) */
  __asm__ volatile("wrmsr" : : "a"(0u), "d"(0u), "c"(0xC0000100u));
  cur->gs_base = 0;
  (void)process_set_user_gs(cur, 0);
  arch_fpu_reset(cur);
  arch_fpu_restore(cur); /* the live registers: we return straight to ring 3 */

  /* 10. enter: rip = the interpreter's (or the image's) entry, rsp = argc,
     every GPR zero (rdx = 0 is glibc's "no rtld_fini"), RFLAGS = IF. */
  for (int i = 0; i < 30; i++) tf->regs[i] = 0;
  tf->elr = li.entry;
  tf->lr = sp;
  tf->spsr = 0x202;
  cur->context[33] = sp;
  cur->context[30] = sp;
  arch_set_user_sp(sp);
  lx_row("enter", cur, abs, (long)li.interp_base, (long)(li.entry & 0x7fffffff));
  lx_args_free(&args);
  return LX_EXEC_DONE;

fail_pre:
  lx_image_free(&im);
  lx_image_free(&ii);
  lx_args_free(&args);
  return rc;

fail_post:
  /* Linux: past the point of no return a failed exec is a fatal SIGSEGV. */
  lx_row("dead", cur, abs, 0, 0);
  lx_image_free(&im);
  lx_image_free(&ii);
  lx_args_free(&args);
  process_kill_group_sig(grp, LX_SIGSEGV);
  schedule(tf, 0);
  return LX_EXEC_GONE;
}

#else /* !__x86_64__: the compat class is x86_64-only */

int linux_execve(struct process *p, struct trap_frame *tf, int dirfd,
                 uint64_t upath, uint64_t uargv, uint64_t uenvp, int flags) {
  (void)p; (void)tf; (void)dirfd; (void)upath; (void)uargv; (void)uenvp;
  (void)flags;
  return -ENOSYS;
}

#endif
