/* src/user/tar.c — minimal tar archive extractor for HobbyOS.
 *
 * First-party tool (not a GNU port): extracts the SRC.TAR source payload
 * that the HobbyOS disk image carries (see tools/selfhost-src-tar.sh and
 * docs/selfhost/inos-build-kernel.md).  The archive is created on the host
 * with `tar --format=ustar`, so the reader covers exactly the ustar shape
 * plus the two GNU extensions a normal tar may still emit for long names:
 *
 *   - v7 / ustar headers (magic "ustar"), prefix field supported;
 *   - GNU 'L' long-name entries (name in the next data block);
 *   - regular files, directories; symlink/hardlink/other entries are
 *     skipped with a note (the payload has none);
 *   - header checksum verification (a corrupt archive stops with rc 1).
 *
 * Usage (in-OS and host):
 *   tar xf ARCHIVE [DEST]   extract into DEST (default /)
 *   tar tf ARCHIVE          list entry names
 *
 * The DEST directory tree is created as needed; parent directories of every
 * entry are created as needed (single-level FAT mkdir, so components are
 * created one at a time).  Existing files are truncated on overwrite
 * (O_TRUNC is honored by the kernel's SYS_OPEN).
 *
 * Build flavors:
 *   - in-OS:  compiled with USER_CFLAGS against the repo sysroot, linked
 *     with libc.a (crt0 provides argc/argv) — runs as /TOOLS/TAR.BIN.
 *   - host:   the same source compiles against glibc (src/host/tar_test.c
 *     includes it directly), so the extractor is exercised on the real
 *     host filesystem before it ever runs in-OS.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define BLOCK 512
#define WINDOW (32 * 1024)
#define MAX_PATH 512

/* ---- archive read window: 512-byte blocks served from a 32 KiB buffer so
 * a multi-MiB archive costs ~one read() per window instead of one per
 * block (matters under TCG). ------------------------------------------------- */

static unsigned char win[WINDOW];
static long win_len;
static long win_pos;
static int rd_fd = -1;
static int at_eof;

static int refill(void) {
  long n;
  win_pos = 0;
  win_len = 0;
  if (at_eof) {
    return 0;
  }
  n = read(rd_fd, win, WINDOW);
  if (n <= 0) {
    at_eof = 1;
    return 0;
  }
  win_len = n;
  return 1;
}

/* read_exact(): copy n bytes out of the window, refilling as needed.
 * Returns 1 on success, 0 on unexpected EOF. */
static int read_exact(unsigned char *dst, long n) {
  while (n > 0) {
    long avail;
    long take;
    if (win_pos >= win_len && !refill()) {
      return 0;
    }
    avail = win_len - win_pos;
    take = n < avail ? n : avail;
    memcpy(dst, win + win_pos, take);
    win_pos += take;
    dst += take;
    n -= take;
  }
  return 1;
}

static int skip_exact(long n) {
  static unsigned char scratch[BLOCK];
  while (n > 0) {
    long take = n < BLOCK ? n : BLOCK;
    if (!read_exact(scratch, take)) {
      return 0;
    }
    n -= take;
  }
  return 1;
}

/* ---- header field helpers ------------------------------------------------- */

/* field_str(): copy an ASCII field (NUL/space padded) into out. */
static void field_str(const unsigned char *h, int off, int len, char *out, int cap) {
  int i = 0;
  while (i < len && i < cap - 1 && h[off + i] != '\0') {
    out[i] = (char)h[off + i];
    i++;
  }
  out[i] = '\0';
}

/* field_octal(): parse an octal field; accepts leading spaces, terminates at
 * NUL/space.  Returns -1 on invalid input.  Handles GNU base-256 (high bit
 * set) for completeness. */
static long field_octal(const unsigned char *h, int off, int len) {
  long v = 0;
  int i = 0;
  if (h[off] & 0x80) {
    v = h[off] & 0x7F;
    for (i = 1; i < len; i++) {
      v = v * 256 + h[off + i];
    }
    return v;
  }
  while (i < len && (h[off + i] == ' ' || h[off + i] == '0')) {
    i++;
  }
  if (i >= len) {
    return 0; /* all spaces/zeros: value 0 */
  }
  for (; i < len; i++) {
    unsigned char c = h[off + i];
    if (c == ' ' || c == '\0') {
      break;
    }
    if (c < '0' || c > '7') {
      return -1;
    }
    v = v * 8 + (c - '0');
  }
  return v;
}

/* header_checksum(): sum of all 512 bytes with the checksum field read as
 * spaces — the ustar stored-checksum algorithm. */
static long header_checksum(const unsigned char *h) {
  long sum = 0;
  int i;
  for (i = 0; i < BLOCK; i++) {
    sum += (i >= 148 && i < 156) ? ' ' : h[i];
  }
  return sum;
}

static int block_is_zero(const unsigned char *b) {
  int i;
  for (i = 0; i < BLOCK; i++) {
    if (b[i] != 0) {
      return 0;
    }
  }
  return 1;
}

/* ---- path plumbing -------------------------------------------------------- */

/* path_join(): dest + '/' + rel with '/' normalization (drop duplicate
 * slashes); keeps the result inside dest.  Returns 0 on success. */
static int path_join(const char *dest, const char *rel, char *out, int cap) {
  int n = 0;
  const char *p;
  if (dest[0] != '/') {
    return -1;
  }
  while (*dest && n < cap - 2) {
    out[n++] = *dest++;
  }
  while (n > 1 && out[n - 1] == '/') {
    n--; /* trim trailing slashes (keep a bare "/") */
  }
  p = rel;
  while (*p == '/') {
    p++; /* no absolute names: stay under dest */
  }
  if (*p && out[n - 1] != '/') {
    out[n++] = '/';
  }
  while (*p && n < cap - 1) {
    if (*p == '/' && n > 0 && out[n - 1] == '/') {
      p++; /* collapse "//" */
      continue;
    }
    out[n++] = *p++;
  }
  out[n] = '\0';
  return 0;
}

/* mkdir_p(): create every component of path (single-level mkdir, one at a
 * time).  EEXIST-class failures are ignored — a real problem surfaces when
 * the subsequent open() fails. */
static void mkdir_p(const char *path) {
  char buf[MAX_PATH];
  int n;
  n = (int)strlen(path);
  if (n == 0 || n >= MAX_PATH) {
    return;
  }
  memcpy(buf, path, n + 1);
  for (int i = 1; i < n; i++) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      mkdir(buf, 0755);
      buf[i] = '/';
    }
  }
  mkdir(buf, 0755);
}

static void make_parents(const char *full) {
  char buf[MAX_PATH];
  int n;
  n = (int)strlen(full);
  if (n == 0 || n >= MAX_PATH) {
    return;
  }
  memcpy(buf, full, n + 1);
  for (int i = n - 1; i > 0; i--) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      if (buf[0] != '\0') {
        mkdir_p(buf);
      }
      return;
    }
  }
}

/* ---- extraction ----------------------------------------------------------- */

static long n_files;
static long n_dirs;
static long n_skipped;
static long n_errors;
static long n_bytes;

static void note_err(const char *what, const char *name) {
  fprintf(stderr, "[TAR] error: %s: %s\n", what, name);
  n_errors++;
}

/* extract_file(): write size bytes of entry data to full, then consume the
 * 512-padding. */
static int extract_file(const char *full, long size) {
  static unsigned char chunk[16 * 1024];
  int fd;
  long left = size;
  int ok = 1;

  make_parents(full);
  fd = open(full, O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    note_err("cannot create", full);
    if (!skip_exact(size)) {
      return 0;
    }
  } else {
    while (left > 0) {
      long take = left < (long)sizeof(chunk) ? left : (long)sizeof(chunk);
      if (!read_exact(chunk, take)) {
        note_err("truncated archive writing", full);
        ok = 0;
        break;
      }
      if (write(fd, chunk, (int)take) != (int)take) {
        note_err("short write", full);
        ok = 0;
        break;
      }
      left -= take;
    }
    close(fd);
    if (!ok) {
      return 0;
    }
  }
  if (!skip_exact((BLOCK - (size % BLOCK)) % BLOCK)) {
    note_err("truncated archive padding", full);
    return 0;
  }
  n_files++;
  n_bytes += size;
  return 1;
}

int main(int argc, char **argv) {
  const char *cmd;
  const char *archive;
  const char *dest = "/";
  char name[256];
  char prefix[156];
  char pending_long[256];
  unsigned char h[BLOCK];
  int have_pending_long = 0;
  int list_mode = 0;
  long entries = 0;

  if (argc < 3) {
    printf("Usage: tar xf ARCHIVE [DEST] | tar tf ARCHIVE\n");
    return 2;
  }
  cmd = argv[1];
  archive = argv[2];

  /* Reset all per-archive state.  The reader window and the counters are
   * file-static; in-OS a process extracts one archive, but the host test
   * drives main() repeatedly in a single process, and a fresh call must
   * never inherit the previous archive's window position or counts. */
  win_len = 0;
  win_pos = 0;
  at_eof = 0;
  n_files = 0;
  n_dirs = 0;
  n_skipped = 0;
  n_errors = 0;
  n_bytes = 0;
  if (strcmp(cmd, "xf") == 0) {
    dest = (argc >= 4) ? argv[3] : "/";
  } else if (strcmp(cmd, "tf") == 0) {
    list_mode = 1;
  } else {
    printf("tar: unknown command '%s' (use xf or tf)\n", cmd);
    return 2;
  }

  rd_fd = open(archive, O_RDONLY);
  if (rd_fd < 0) {
    fprintf(stderr, "[TAR] error: cannot open archive: %s\n", archive);
    return 1;
  }
  if (!list_mode) {
    printf("[TAR] %s -> %s\n", archive, dest);
    mkdir_p(dest);
  }

  pending_long[0] = '\0';

  for (;;) {
    long stored;
    long calc;
    long size;
    char type;
    char full[MAX_PATH];

    /* A clean EOF exactly at a block boundary ends the archive (some
     * writers omit the trailing zero blocks); anything else is truncation. */
    if (win_pos >= win_len && !refill()) {
      break;
    }
    if (!read_exact(h, BLOCK)) {
      fprintf(stderr, "[TAR] error: truncated archive (header)\n");
      n_errors++;
      break;
    }
    if (block_is_zero(h)) {
      break; /* end-of-archive marker */
    }

    stored = field_octal(h, 148, 8);
    calc = header_checksum(h);
    if (stored < 0 || stored != calc) {
      fprintf(stderr, "[TAR] error: bad header checksum at entry %ld\n", entries + 1);
      n_errors++;
      break;
    }

    /* name: pending GNU long name wins over the header's own name. */
    if (have_pending_long) {
      memcpy(name, pending_long, sizeof(name));
      name[sizeof(name) - 1] = '\0';
      have_pending_long = 0;
      prefix[0] = '\0';
    } else {
      field_str(h, 0, 100, name, sizeof(name));
      field_str(h, 345, 155, prefix, sizeof(prefix));
      if (prefix[0] != '\0' && memcmp(h + 257, "ustar", 5) == 0) {
        char joined[256];
        int n = 0;
        int i;
        for (i = 0; prefix[i] && n < 154; i++) {
          joined[n++] = prefix[i];
        }
        joined[n++] = '/';
        for (i = 0; name[i] && n < 255; i++) {
          joined[n++] = name[i];
        }
        joined[n] = '\0';
        memcpy(name, joined, n + 1);
      }
    }

    type = (char)h[156];
    size = field_octal(h, 124, 12);
    if (size < 0) {
      fprintf(stderr, "[TAR] error: bad size field for %s\n", name);
      n_errors++;
      break;
    }
    entries++;

    /* GNU long name: the name is the next entry's data block(s). */
    if (type == 'L') {
      if (size <= 0 || size >= (long)sizeof(pending_long)) {
        n_skipped++;
        if (!skip_exact(size + (BLOCK - (size % BLOCK)) % BLOCK)) {
          note_err("truncated archive skipping", name);
          break;
        }
        continue;
      }
      if (!read_exact((unsigned char *)pending_long, size)) {
        note_err("truncated archive (long name)", name);
        break;
      }
      pending_long[size] = '\0';
      have_pending_long = 1;
      if (!skip_exact((BLOCK - (size % BLOCK)) % BLOCK)) {
        n_errors++;
        break;
      }
      continue;
    }

    /* strip a trailing slash (directory entries) and a leading "./". */
    {
      int n = (int)strlen(name);
      while (n > 1 && name[n - 1] == '/') {
        name[--n] = '\0';
      }
      while (name[0] == '.' && name[1] == '/') {
        memmove(name, name + 2, strlen(name + 2) + 1);
      }
    }

    if (list_mode) {
      if (strcmp(name, ".") != 0) {
        printf("%s\n", name);
      }
      if (!skip_exact(size + (BLOCK - (size % BLOCK)) % BLOCK)) {
        n_errors++;
        break;
      }
      continue;
    }

    if (strcmp(name, ".") == 0) {
      if (!skip_exact(size)) {
        break;
      }
      continue;
    }

    if (path_join(dest, name, full, sizeof(full)) != 0) {
      note_err("bad path", name);
      if (!skip_exact(size)) {
        break;
      }
      continue;
    }

    if (type == '0' || type == '\0') {
      if (!extract_file(full, size)) {
        break;
      }
      if ((n_files % 100) == 0) {
        printf("[TAR] ... %ld files, %ld bytes\n", n_files, n_bytes);
      }
    } else if (type == '5') {
      mkdir_p(full);
      n_dirs++;
    } else {
      /* symlink/hardlink/unknown: skip the data, note the entry. */
      n_skipped++;
      if (!skip_exact(size + (BLOCK - (size % BLOCK)) % BLOCK)) {
        note_err("truncated archive skipping", name);
        break;
      }
    }
  }

  close(rd_fd);
  if (list_mode) {
    printf("[TAR] %ld entries\n", entries);
    return n_errors == 0 ? 0 : 1;
  }
  printf("[TAR] done: files=%ld dirs=%ld bytes=%ld skipped=%ld errors=%ld\n",
         n_files, n_dirs, n_bytes, n_skipped, n_errors);
  return n_errors == 0 ? 0 : 1;
}
