/*
 * LFN_T.BIN — in-OS acceptance for VFAT long-file-name coverage of the
 * self-host build payload (lane L3, selfhost.md W-A / F2 / AD-11).
 *
 * The FAT16 driver already reads+writes VFAT long names; this test proves
 * the EXACT shape the in-OS build needs:
 *   - deep long-name trees written by mtools mcopy (the payload staging):
 *     open/read by the real long name, read_dir shows the long names;
 *   - a 30+ char component name (mirrors the libcxx vendor depth);
 *   - a collision family (longfilename / longfilename2 / longfil~1) that
 *     must resolve unambiguously;
 *   - long-name outputs created IN-OS (write path), renamed, mkdir'd with a
 *     long directory name, listed and unlinked.
 * Deep-path limit probe: a libcxx-style nested path (~85 chars, 7 components).
 *
 * Status goes to the console (print_console), matching the other wave
 * tests; stdout is a harness pipe.  Exit code = failure count.
 */
#include <fcntl.h>
#include <unistd.h>
#include "libc.h" /* print_console(), read_dir(), mkdir(), unlink() */

static int failures = 0;

static int slen(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int smatch(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static void report(const char *tag, const char *what, int ok) {
  print_console(tag);
  if (ok) {
    print_console(" PASS ");
  } else {
    print_console(" FAIL ");
    failures++;
  }
  print_console(what);
  print_console("\n");
}

/* Open `path`, read its (tiny) content and require a byte-exact match
 * with `expect` — the expected length is derived from the string, so a
 * mismatch can only be a real content difference.  Returns 1 on success. */
static int read_expect(const char *path, const char *expect) {
  int want = slen(expect);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  char buf[64];
  int n = (int)read(fd, buf, sizeof buf);
  close(fd);
  if (n != want) return 0;
  for (int i = 0; i < n; i++) {
    if (buf[i] != expect[i]) return 0;
  }
  return 1;
}

/* Does a read_dir listing of `path` contain an entry named exactly
 * `needle`?  (read_dir returns -1 past the end of the listing.) */
static int dir_has(const char *path, const char *needle) {
  for (int i = 0; i < 512; i++) {
    struct sys_dirent ent;
    if (read_dir(path, i, &ent) != 0) break;
    if (smatch((const char *)ent.name, needle)) return 1;
  }
  return 0;
}

/* Size of the file at `path` after open (create-on-open makes missing
 * files 0 bytes). */
static long file_size(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  long sz = lseek(fd, 0, SEEK_END);
  close(fd);
  return sz;
}

static void check_fixtures(void) {
  /* 1. The payload shape: deep paths + long basenames written by mcopy. */
  report("[LFN]", "payload deep path src/kernel/program_loader.c",
         read_expect("/src/kernel/program_loader.c",
                     "L3LFN:program_loader.c:0001\n"));
  report("[LFN]", "long dir + nested src/user_include/graphics/window.h",
         read_expect("/src/user_include/graphics/window.h",
                     "L3LFN:window.h:0002\n"));

  /* 2. A 30+ char component (36 chars: mirrors the libcxx vendor depth). */
  report("[LFN]", "30+ char component llvm-project-21.1.8.src-EXAMPLE.c",
         read_expect("/src/llvm-project-21.1.8.src-EXAMPLE.c",
                     "L3LFN:EXAMPLE:0003\n"));

  /* 3. Deep libcxx-style nesting (~85 char path, 7 components). */
  report("[LFN]", "deep nested libcxx-style path upper_bound.h",
         read_expect("/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/"
                     "libcxx/include/upper_bound.h",
                     "L3LFN:upper_bound:0020\n"));

  /* 4. Mixed case: all-caps path must reach the lowercase long names. */
  report("[LFN]", "case-insensitive path /SRC/KERNEL/PROGRAM_LOADER.C",
         read_expect("/SRC/KERNEL/PROGRAM_LOADER.C",
                     "L3LFN:program_loader.c:0001\n"));

  /* 5. Collision family: three long names whose 8.3 aliases collide/edge
     must each resolve to their own content. */
  report("[LFN]", "collision longfilename.txt",
         read_expect("/longfilename.txt", "L3LFN:longfilename:0010\n"));
  report("[LFN]", "collision longfilename2.txt",
         read_expect("/longfilename2.txt", "L3LFN:longfilename2:0011\n"));
  report("[LFN]", "collision longfil~1.txt",
         read_expect("/longfil~1.txt", "L3LFN:longfil~1:0012\n"));

  /* 6. read_dir shows the LONG names (pre-fix it returned the 8.3 mangle). */
  report("[LFN]", "read_dir /src/kernel lists program_loader.c",
         dir_has("/src/kernel", "program_loader.c"));
  report("[LFN]", "read_dir / lists longfilename.txt",
         dir_has("/", "longfilename.txt"));
  report("[LFN]", "read_dir / lists longfilename2.txt",
         dir_has("/", "longfilename2.txt"));
  /* The 33-char name is capped at 31 chars by the read_dir syscall; its
     long-name PREFIX must appear (never an 8.3 mangle like LLVM-PR~1.C). */
  report("[LFN]", "read_dir / shows 30+ char name (31-char syscall cap)",
         dir_has("/", "llvm-project-21.1.8.src-EXAMPLE"));
}

static void check_inos_writes(void) {
  /* 7. Create a NEW long-name file in-OS (write path), re-open, read back. */
  {
    const char *path = "/lfn_newfile_created.txt";
    const char *body = "L3LFN-inos-create-1\n";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      (void)write(fd, body, slen(body));
      close(fd);
    }
    report("[LFN]", "create+reopen long-name file in-OS",
           read_expect(path, body));

    /* 8. Short-name fallback: the SAME file is reachable through the 8.3
       alias the driver derived for it. */
    report("[LFN]", "8.3 alias of in-OS long name resolves",
           read_expect("/LFN_NEWF.TXT", body));

    /* 9. Mixed-case reopen of the same created file. */
    report("[LFN]", "mixed-case reopen of created long name",
           read_expect("/LFN_NEWFILE_CREATED.TXT", body));
  }

  /* 10. Rename a long name -> new long name; reopen by the new name and
      confirm the old long name is gone (pre-fix the 0x0F records were left
      stale and the new name resolved to a fresh empty file). */
  {
    const char *src = "/lfn_rename_src.txt";
    const char *dst = "/lfn_rename_dst.txt";
    const char *body = "L3LFN-inos-rename-src\n";
    int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      (void)write(fd, body, slen(body));
      close(fd);
    }
    report("[LFN]", "rename long name to long name rc",
           (rename(src, dst) == 0) ? 1 : 0);
    report("[LFN]", "rename: new long name resolves with content",
           read_expect(dst, body));
    report("[LFN]", "rename: old long name no longer listed",
           !dir_has("/", "lfn_rename_src.txt"));
  }

  /* 11. mkdir a LONG dir name, then create/read a file inside it — the
      exact shape of the build's in-OS output dirs (pre-fix the 8.3-only
      dir entry was unreachable by the long name). */
  {
    const char *dir = "/lfn_long_dir_created";
    const char *inner = "/lfn_long_dir_created/INNER.TXT";
    const char *body = "L3LFN-inos-inner\n";
    (void)mkdir(dir); /* tolerate EEXIST (no rmdir; fresh disk each gate) */
    int fd = open(inner, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
      (void)write(fd, body, slen(body));
      close(fd);
    }
    report("[LFN]", "mkdir long dir + create file inside",
           read_expect(inner, body));
    report("[LFN]", "long dir listed with long name",
           dir_has("/", "lfn_long_dir_created"));
  }

  /* 12. Unlink the created long-name file: gone from listings, and a plain
      open now fails (the kernel's POSIX-style SYS_OPEN: no O_CREAT -> ENOENT). */
  {
    const char *path = "/lfn_newfile_created.txt";
    report("[LFN]", "unlink long-name file rc", (unlink(path) == 0) ? 1 : 0);
    report("[LFN]", "unlinked long-name file not listed", !dir_has("/", path));
    report("[LFN]", "reopen-after-unlink fails (ENOENT)",
           (file_size(path) == -1) ? 1 : 0);
  }
}

__attribute__((section(".text._start")))
void _start(void) {
  print_console("\n[LFN] VFAT long-name build-payload verification\n");
  check_fixtures();
  check_inos_writes();
  if (failures == 0) {
    print_console("[LFN] ALL PASSED\n");
  } else {
    print_console("[LFN] FAILED: ");
    print_hex((long)failures);
    print_console("\n");
  }
  exit(failures);
}
