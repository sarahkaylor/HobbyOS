/*
 * ext2_test.c - host golden test for the ext2 core (src/kernel/ext2.c).
 *
 * The oracle is e2fsprogs, not this file: every image is made by mke2fs
 * (populated with `mke2fs -d` / debugfs), mutated through the driver, and
 * then judged by `e2fsck -fn` (must exit 0) and by `debugfs rdump` /
 * `debugfs stat` (must show exactly the tree and bytes the test wrote).
 * Corrupted images (bad rec_len, out-of-range block and indirect pointers,
 * a dirent naming a nonexistent inode, unknown features, a truncated device)
 * must fail with a clean -errno and never crash.
 *
 * Needs mke2fs, e2fsck, debugfs (Linux: e2fsprogs).  Without them the test
 * prints SKIP and exits 0 (macOS host_tests_portable); `make host_tests` on
 * linuxci has them, and the run log there is the evidence.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ext2.h"

static int failures, checks;
static char tmpdir[256];
static char mke2fs_bin[128], e2fsck_bin[128], debugfs_bin[128];

#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
      failures++;                                                             \
      printf("FAIL %s:%d  ", __FILE__, __LINE__);                             \
      printf(__VA_ARGS__);                                                    \
      printf("\n");                                                           \
    }                                                                         \
  } while (0)

/* ---- host services --------------------------------------------------- */

static int img_fd = -1;

static int dev_read(void *ctx, uint64_t sector, void *buf, uint32_t n) {
  (void)ctx;
  ssize_t r = pread(img_fd, buf, (size_t)n * 512, (off_t)(sector * 512));
  return r == (ssize_t)n * 512 ? 0 : -1;
}

/* cp/ext2w: a write log for the crash-prefix scenario (every device write in
 * order, split into 512-byte sectors' worth of blocks of the fs block size). */
struct wrec {
  uint64_t sector;
  uint32_t nsec;
  uint8_t *data;
};
static struct wrec *wlog;
static int wlog_n, wlog_cap, wlog_on;
static uint64_t dev_calls;

static void wlog_add(uint64_t sector, const void *buf, uint32_t n) {
  if (!wlog_on) return;
  if (wlog_n == wlog_cap) {
    wlog_cap = wlog_cap ? wlog_cap * 2 : 256;
    wlog = realloc(wlog, (size_t)wlog_cap * sizeof *wlog);
  }
  wlog[wlog_n].sector = sector;
  wlog[wlog_n].nsec = n;
  wlog[wlog_n].data = malloc((size_t)n * 512);
  memcpy(wlog[wlog_n].data, buf, (size_t)n * 512);
  wlog_n++;
}

static void wlog_clear(void) {
  for (int i = 0; i < wlog_n; i++) free(wlog[i].data);
  wlog_n = 0;
}

static int dev_write(void *ctx, uint64_t sector, const void *buf, uint32_t n) {
  (void)ctx;
  dev_calls++;
  wlog_add(sector, buf, n);
  ssize_t r = pwrite(img_fd, buf, (size_t)n * 512, (off_t)(sector * 512));
  return r == (ssize_t)n * 512 ? 0 : -1;
}

/* The vectored op (as the kernel's NVMe path offers it). */
static int dev_rwv(void *ctx, uint64_t sector, void *const *bufs, uint32_t nbuf,
                   uint32_t each, int wr) {
  (void)ctx;
  dev_calls++;
  for (uint32_t i = 0; i < nbuf; i++) {
    uint64_t sec = sector + (uint64_t)i * each;
    size_t len = (size_t)each * 512;
    if (wr) {
      wlog_add(sec, bufs[i], each);
      if (pwrite(img_fd, bufs[i], len, (off_t)(sec * 512)) != (ssize_t)len) return -1;
    } else if (pread(img_fd, bufs[i], len, (off_t)(sec * 512)) != (ssize_t)len) {
      return -1;
    }
  }
  return 0;
}

static void *host_alloc(uint32_t n) {
  void *p = 0;
  if (posix_memalign(&p, 4096, n) != 0) return 0;
  return p;
}

static uint32_t host_now(void) { return 1763000000u; }

static struct ext2_fs FS;

static int mount_img(const char *img, int ro, int cache) {
  if (img_fd >= 0) close(img_fd);
  img_fd = open(img, O_RDWR);
  if (img_fd < 0) return -EIO;
  struct stat st;
  fstat(img_fd, &st);
  struct ext2_dev d = {0, dev_read, dev_write, (uint64_t)st.st_size / 512, 0};
  /* cp/ext2w: the vectored path for the bigger caches, the per-block
     fallback for the tiny ones (both stay covered) */
  if (cache >= 64) d.rwv = dev_rwv;
  FS.now = host_now;
  FS.alloc_buf = host_alloc;
  FS.cache_cap = (uint32_t)cache;
  return ext2_mount(&FS, &d, ro);
}

static void umount_img(void) {
  if (FS.mounted && !FS.ro) {
    int r = ext2_flush(&FS);
    CHECK(r == 0, "flush rc=%d", r);
  }
  FS.mounted = 0;
  if (img_fd >= 0) close(img_fd);
  img_fd = -1;
}

static int run(const char *fmt, ...) {
  char cmd[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof cmd, fmt, ap);
  va_end(ap);
  int st = system(cmd);
  if (st == -1 || !WIFEXITED(st)) return 255;
  return WEXITSTATUS(st);
}

static int capture(char *out, size_t cap, const char *fmt, ...) {
  char cmd[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof cmd, fmt, ap);
  va_end(ap);
  FILE *f = popen(cmd, "r");
  if (!f) return -1;
  size_t n = fread(out, 1, cap - 1, f);
  out[n] = '\0';
  int st = pclose(f);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int find_tool(char *dst, size_t cap, const char *name) {
  const char *dirs[] = {"/usr/sbin", "/sbin", "/usr/bin", "/bin",
                        "/opt/homebrew/opt/e2fsprogs/sbin",
                        "/usr/local/opt/e2fsprogs/sbin"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    snprintf(dst, cap, "%s/%s", dirs[i], name);
    if (access(dst, X_OK) == 0) return 0;
  }
  return -1;
}

/* e2fsck -fn: exit 0 means "no problems found". */
static int fsck(const char *img, const char *what) {
  char out[16384];
  int rc = capture(out, sizeof out, "%s -fn %s 2>&1", e2fsck_bin, img);
  if (rc != 0) printf("---- e2fsck (%s) rc=%d ----\n%s----\n", what, rc, out);
  else {
    /* the last line is the summary ("... files, ... blocks") */
    char *last = out, *p;
    for (p = out; *p; p++)
      if (p[0] == '\n' && p[1]) last = p + 1;
    printf("  e2fsck -fn %s: rc=0  %s", what, last);
  }
  return rc;
}

/* ---- deterministic content -------------------------------------------- */

static void fill(uint8_t *b, size_t n, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b[i] = (uint8_t)x;
  }
}

static int write_host_file(const char *path, const uint8_t *b, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  size_t w = fwrite(b, 1, n, f);
  fclose(f);
  return w == n ? 0 : -1;
}

static uint8_t *read_host_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(sz > 0 ? (size_t)sz : 1);
  *n = fread(b, 1, (size_t)sz, f);
  fclose(f);
  return b;
}

/* ---- driver-side helpers ---------------------------------------------- */

static int res(const char *abs, int follow, struct ext2_res *r) {
  return ext2_resolve(&FS, "/EXT", abs, follow ? EXT2_RES_FOLLOW : 0, r);
}

static uint32_t ino_of(const char *abs) {
  struct ext2_res r;
  int rc = res(abs, 1, &r);
  return rc == 0 ? r.ino : 0;
}

/* create (or open) a file by path, write `n` bytes at `off`. */
static int put(const char *abs, uint64_t off, const uint8_t *b, size_t n) {
  struct ext2_res r;
  int rc = res(abs, 1, &r);
  uint32_t ino;
  if (rc == -ENOENT && r.parent) {
    rc = ext2_create(&FS, r.parent, r.name, r.name_len, 0644, &ino);
    if (rc) return rc;
  } else if (rc) {
    return rc;
  } else {
    ino = r.ino;
  }
  int64_t w = ext2_write(&FS, ino, off, b, n);
  return w == (int64_t)n ? 0 : (w < 0 ? (int)w : -EIO);
}

static int mkdir_p(const char *abs) {
  struct ext2_res r;
  int rc = res(abs, 1, &r);
  if (rc == 0) return 0;
  if (rc != -ENOENT || !r.parent) return rc;
  uint32_t ino;
  return ext2_mkdir(&FS, r.parent, r.name, r.name_len, 0755, &ino);
}

static int parent_of(const char *abs, uint32_t *dir, char *name, int *len) {
  struct ext2_res r;
  int rc = res(abs, 0, &r);
  if (rc != 0 && !(rc == -ENOENT && r.parent)) return rc;
  *dir = r.parent;
  memcpy(name, r.name, (size_t)r.name_len + 1);
  *len = r.name_len;
  return rc;
}

static int symlink_at(const char *abs, const char *target) {
  uint32_t dir, ino;
  char name[256];
  int len;
  int rc = parent_of(abs, &dir, name, &len);
  if (rc != -ENOENT) return rc == 0 ? -EEXIST : rc;
  return ext2_symlink(&FS, dir, name, len, target, (int)strlen(target), &ino);
}

static int unlink_at(const char *abs) {
  uint32_t dir;
  char name[256];
  int len;
  int rc = parent_of(abs, &dir, name, &len);
  if (rc) return rc;
  return ext2_unlink(&FS, dir, name, len);
}

static int rmdir_at(const char *abs) {
  uint32_t dir;
  char name[256];
  int len;
  int rc = parent_of(abs, &dir, name, &len);
  if (rc) return rc;
  return ext2_rmdir(&FS, dir, name, len);
}

static int rename_at(const char *a, const char *b) {
  uint32_t od, nd;
  char on[256], nn[256];
  int ol, nl;
  int rc = parent_of(a, &od, on, &ol);
  if (rc) return rc;
  rc = parent_of(b, &nd, nn, &nl);
  if (rc && rc != -ENOENT) return rc;
  return ext2_rename(&FS, od, on, ol, nd, nn, nl);
}

static int read_all(const char *abs, uint8_t *b, size_t cap, size_t *n) {
  uint32_t ino = ino_of(abs);
  if (!ino) return -ENOENT;
  int64_t r = ext2_read(&FS, ino, 0, b, cap);
  if (r < 0) return (int)r;
  *n = (size_t)r;
  return 0;
}

static int count_dir(const char *abs, int *n) {
  uint32_t dir = ino_of(abs);
  if (!dir) return -ENOENT;
  uint64_t pos = 0;
  uint32_t ino;
  uint8_t ft;
  char name[256];
  int nl, rc;
  *n = 0;
  while ((rc = ext2_readdir(&FS, dir, &pos, &ino, &ft, name, &nl)) == 1) (*n)++;
  return rc;
}

/* ---- expected tree (what debugfs must show after the mutations) --------- */

struct want {
  const char *path; /* relative to the fs root, no leading '/' */
  int kind;         /* 'f' file, 'd' dir, 'l' symlink, 'x' must not exist */
  uint32_t seed;    /* file content seed */
  size_t len;       /* file length (dense pattern) */
  const char *link; /* symlink target */
};

static int verify_rdump(const char *img, const struct want *w, int nw,
                        const char *what) {
  char out[256], dump[300];
  snprintf(dump, sizeof dump, "%s/rdump-%s", tmpdir, what);
  run("rm -rf %s && mkdir -p %s", dump, dump);
  /* rdump prints a chown warning per entry when not root: send everything
     to /dev/null (a short capture buffer would SIGPIPE debugfs mid-dump). */
  (void)out;
  run("%s -R 'rdump / %s' %s >/dev/null 2>&1", debugfs_bin, dump, img);
  int bad = 0;
  for (int i = 0; i < nw; i++) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dump, w[i].path);
    struct stat st;
    int ex = lstat(p, &st) == 0;
    if (w[i].kind == 'x') {
      CHECK(!ex, "[%s] %s must be gone (debugfs)", what, w[i].path);
      bad += ex;
      continue;
    }
    CHECK(ex, "[%s] %s missing in debugfs rdump", what, w[i].path);
    if (!ex) {
      bad++;
      continue;
    }
    if (w[i].kind == 'd') {
      CHECK(S_ISDIR(st.st_mode), "[%s] %s not a dir", what, w[i].path);
    } else if (w[i].kind == 'l') {
      char t[4200];
      ssize_t n = readlink(p, t, sizeof t - 1);
      if (n >= 0) t[n] = 0;
      CHECK(S_ISLNK(st.st_mode) && n >= 0 && strcmp(t, w[i].link) == 0,
            "[%s] symlink %s -> '%s' (want '%s')", what, w[i].path,
            n >= 0 ? t : "?", w[i].link);
    } else {
      size_t n = 0;
      uint8_t *got = read_host_file(p, &n);
      uint8_t *exp = malloc(w[i].len ? w[i].len : 1);
      fill(exp, w[i].len, w[i].seed);
      int ok = got && n == w[i].len && memcmp(got, exp, n) == 0;
      CHECK(ok, "[%s] %s content (len %zu want %zu)", what, w[i].path, n,
            w[i].len);
      bad += !ok;
      free(got);
      free(exp);
    }
  }
  return bad;
}

/* debugfs "stat" field probe: the first number after `key`. */
static long dbg_stat_num(const char *img, const char *path, const char *key) {
  char out[8192];
  capture(out, sizeof out, "%s -R 'stat %s' %s 2>/dev/null", debugfs_bin, path,
          img);
  char *p = strstr(out, key);
  if (!p) return -1;
  p += strlen(key);
  while (*p == ' ') p++;
  return strtol(p, 0, 10);
}

/* ---- scenario 1: read a tree mke2fs -d built ---------------------------- */

#define SLOW_TARGET \
  "/EXT/dir1/sub/deeper/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static void make_src_tree(const char *src) {
  run("rm -rf %s && mkdir -p %s/dir1/sub/deeper %s/big", src, src, src);
  uint8_t *b = malloc(3 << 20);
  fill(b, 100000, 11);
  write_host_file(strcat(strcpy((char[400]){0}, src), "/file100k"), b, 100000);
  fill(b, 3 << 20, 12);
  write_host_file(strcat(strcpy((char[400]){0}, src), "/dir1/file3m"), b,
                  3 << 20);
  fill(b, 17, 13);
  write_host_file(strcat(strcpy((char[400]){0}, src), "/dir1/sub/deeper/tiny"),
                  b, 17);
  write_host_file(strcat(strcpy((char[400]){0}, src), "/empty"), b, 0);
  for (int i = 0; i < 300; i++) {
    char p[500];
    snprintf(p, sizeof p, "%s/big/entry_with_a_longish_name_%04d", src, i);
    fill(b, (size_t)i, 1000u + (uint32_t)i);
    write_host_file(p, b, (size_t)i);
  }
  free(b);
  run("ln -s dir1/file3m %s/fastlink", src);
  run("ln -s %s %s/slowlink", SLOW_TARGET, src);
  /* a sparse file: 1 byte at 40 MiB */
  run("dd if=/dev/zero of=%s/sparse bs=1 count=1 seek=41943040 status=none", src);
}

static void scenario_read(int bs, const char *features) {
  char img[300], src[300];
  snprintf(img, sizeof img, "%s/r%d.img", tmpdir, bs);
  snprintf(src, sizeof src, "%s/src", tmpdir);
  make_src_tree(src);
  run("rm -f %s && truncate -s 96M %s", img, img);
  int rc = run("%s -q -F -t ext2 -b %d %s -d %s %s >/dev/null 2>&1", mke2fs_bin,
               bs, features, src, img);
  CHECK(rc == 0, "mke2fs -d bs=%d rc=%d", bs, rc);
  printf("scenario read bs=%d %s\n", bs, features);
  rc = mount_img(img, 1, 64);
  CHECK(rc == 0, "mount rc=%d", rc);
  if (rc) return;
  char desc[200];
  ext2_describe(&FS, desc, sizeof desc);
  printf("  mounted: %s\n", desc);
  uint8_t *b = malloc(3 << 20), *e = malloc(3 << 20);
  size_t n = 0;
  rc = read_all("/EXT/file100k", b, 3 << 20, &n);
  fill(e, 100000, 11);
  CHECK(rc == 0 && n == 100000 && !memcmp(b, e, n), "file100k rc=%d n=%zu", rc,
        n);
  rc = read_all("/EXT/dir1/file3m", b, 3 << 20, &n);
  fill(e, 3 << 20, 12);
  CHECK(rc == 0 && n == (3u << 20) && !memcmp(b, e, n), "file3m rc=%d n=%zu",
        rc, n);
  rc = read_all("/EXT/fastlink", b, 3 << 20, &n);  /* follows the link */
  CHECK(rc == 0 && n == (3u << 20) && !memcmp(b, e, n), "fastlink follow");
  rc = read_all("/EXT/dir1/sub/deeper/tiny", b, 100, &n);
  fill(e, 17, 13);
  CHECK(rc == 0 && n == 17 && !memcmp(b, e, 17), "tiny");
  rc = read_all("/EXT/empty", b, 100, &n);
  CHECK(rc == 0 && n == 0, "empty");
  /* sparse: size 40 MiB + 1, zeros before the last byte */
  uint32_t si = ino_of("/EXT/sparse");
  struct ext2_stat st;
  rc = ext2_stat(&FS, si, &st);
  CHECK(rc == 0 && st.size == 41943041ull, "sparse size %llu",
        (unsigned long long)st.size);
  CHECK(st.blocks <= (uint64_t)4 * (bs / 512) + 64, "sparse blocks=%llu",
        (unsigned long long)st.blocks);
  int64_t rr = ext2_read(&FS, si, 41943040ull - 4096, b, 8192);
  int zeros = 1;
  for (int i = 0; i < 4096; i++) zeros &= b[i] == 0;
  CHECK(rr == 4097 && zeros && b[4096] == 0, "sparse read rr=%lld",
        (long long)rr);
  char t[300];
  struct ext2_res r;
  rc = res("/EXT/fastlink", 0, &r);
  int tl = rc == 0 ? ext2_readlink(&FS, r.ino, t, sizeof t) : rc;
  CHECK(tl == 11 && !memcmp(t, "dir1/file3m", 11), "readlink fast tl=%d", tl);
  rc = res("/EXT/slowlink", 0, &r);
  tl = rc == 0 ? ext2_readlink(&FS, r.ino, t, sizeof t) : rc;
  CHECK(tl == (int)strlen(SLOW_TARGET) && !memcmp(t, SLOW_TARGET, (size_t)tl),
        "readlink slow tl=%d", tl);
  /* the slow link points at a missing file inside the fs: ENOENT */
  rc = res("/EXT/slowlink", 1, &r);
  CHECK(rc == -ENOENT, "follow dangling slowlink rc=%d", rc);
  int cnt;
  rc = count_dir("/EXT/big", &cnt);
  CHECK(rc == 0 && cnt == 302, "big dir entries=%d rc=%d", cnt, rc);
  for (int i = 0; i < 300; i += 37) {
    char p[200];
    snprintf(p, sizeof p, "/EXT/big/entry_with_a_longish_name_%04d", i);
    rc = read_all(p, b, 1000, &n);
    fill(e, (size_t)i, 1000u + (uint32_t)i);
    CHECK(rc == 0 && n == (size_t)i && !memcmp(b, e, n), "%s", p);
  }
  CHECK(res("/EXT/nope/x", 0, &r) == -ENOENT && r.parent == 0, "missing dir");
  CHECK(res("/EXT/file100k/x", 0, &r) == -ENOTDIR, "file as dir");
  /* write must be refused on a read-only mount */
  CHECK(ext2_write(&FS, si, 0, "x", 1) == -EROFS, "ro write");
  free(b);
  free(e);
  umount_img();
}

/* ---- scenario 2: mutate, then judge with e2fsck + debugfs -------------- */

static void scenario_write(int bs, const char *features, int cache) {
  char img[300];
  snprintf(img, sizeof img, "%s/w%d.img", tmpdir, bs);
  run("rm -f %s && truncate -s 128M %s", img, img);
  int rc = run("%s -q -F -t ext2 -b %d %s %s >/dev/null 2>&1", mke2fs_bin, bs,
               features, img);
  CHECK(rc == 0, "mke2fs bs=%d", bs);
  /* seed some content from debugfs too, so we modify a tree we did not make */
  run("%s -w -R 'mkdir pre' %s >/dev/null 2>&1", debugfs_bin, img);
  run("%s -w -R 'mkdir pre/inner' %s >/dev/null 2>&1", debugfs_bin, img);
  printf("scenario write bs=%d %s cache=%d\n", bs, features, cache);
  rc = mount_img(img, 0, cache);
  CHECK(rc == 0 && !FS.ro, "mount rw rc=%d ro=%d", rc, FS.ro);
  if (rc) return;
  uint64_t free0 = FS.free_blocks;
  size_t big = (size_t)(bs == 1024 ? 1 : 2) * (1u << 20) + 12345;
  uint8_t *b = malloc(big + 8192);
  /* plain files, partial-block appends, multiple indirect levels */
  fill(b, 5000, 1);
  CHECK(put("/EXT/a.txt", 0, b, 5000) == 0, "put a.txt");
  fill(b, big, 2);
  CHECK(put("/EXT/big.bin", 0, b, 777) == 0, "put big head");
  CHECK(put("/EXT/big.bin", 777, b + 777, big - 777) == 0, "put big rest");
  CHECK(mkdir_p("/EXT/d1") == 0, "mkdir d1");
  CHECK(mkdir_p("/EXT/d1/d2") == 0, "mkdir d2");
  CHECK(mkdir_p("/EXT/d1/d2/d3") == 0, "mkdir d3");
  CHECK(mkdir_p("/EXT/d1") == 0, "mkdir exists ok (helper)");
  uint32_t dino;
  CHECK(ext2_mkdir(&FS, ino_of("/EXT"), "d1", 2, 0755, &dino) == -EEXIST,
        "mkdir EEXIST");
  fill(b, 3, 3);
  CHECK(put("/EXT/d1/d2/d3/leaf", 0, b, 3) == 0, "leaf");
  /* many entries: directory grows past one block, then half are removed */
  for (int i = 0; i < 400; i++) {
    char p[200];
    snprintf(p, sizeof p, "/EXT/many/f%03d_padding_padding_padding", i);
    if (i == 0) CHECK(mkdir_p("/EXT/many") == 0, "mkdir many");
    fill(b, (size_t)(i % 50), 500u + (uint32_t)i);
    rc = put(p, 0, b, (size_t)(i % 50));
    if (rc) {
      CHECK(0, "put %s rc=%d", p, rc);
      break;
    }
  }
  for (int i = 0; i < 400; i += 2) {
    char p[200];
    snprintf(p, sizeof p, "/EXT/many/f%03d_padding_padding_padding", i);
    rc = unlink_at(p);
    if (rc) {
      CHECK(0, "unlink %s rc=%d", p, rc);
      break;
    }
  }
  /* symlinks */
  CHECK(symlink_at("/EXT/fast", "d1/d2/d3/leaf") == 0, "symlink fast");
  char slow[200];
  memset(slow, 'z', 150);
  slow[150] = 0;
  CHECK(symlink_at("/EXT/slow", slow) == 0, "symlink slow");
  CHECK(symlink_at("/EXT/abs", "/EXT/d1") == 0, "symlink abs");
  CHECK(symlink_at("/EXT/out", "/NFS/game") == 0, "symlink out");
  CHECK(symlink_at("/EXT/up", "../CP/WINE") == 0, "symlink up");
  struct ext2_res r;
  rc = res("/EXT/abs/d2/d3/leaf", 1, &r);
  CHECK(rc == 0 && strcmp(r.path, "/EXT/d1/d2/d3/leaf") == 0,
        "abs link resolve rc=%d path=%s", rc, r.path);
  rc = res("/EXT/out/x", 1, &r);
  CHECK(rc == EXT2_EXTERNAL && strcmp(r.path, "/NFS/game/x") == 0,
        "external rc=%d path=%s", rc, r.path);
  rc = res("/EXT/up/bin", 1, &r);
  CHECK(rc == EXT2_EXTERNAL && strcmp(r.path, "/CP/WINE/bin") == 0,
        "escape rc=%d path=%s", rc, r.path);
  rc = res("/EXT/fast", 1, &r);
  CHECK(rc == 0 && r.ino == ino_of("/EXT/d1/d2/d3/leaf"), "fast follow");
  /* loops */
  CHECK(symlink_at("/EXT/loop1", "loop2") == 0, "loop1");
  CHECK(symlink_at("/EXT/loop2", "loop1") == 0, "loop2");
  CHECK(res("/EXT/loop1", 1, &r) == -ELOOP, "ELOOP");
  CHECK(unlink_at("/EXT/loop1") == 0 && unlink_at("/EXT/loop2") == 0,
        "unlink loops");
  /* sparse files: data far out (5 GiB) -> triple/double indirect */
  fill(b, 4096, 9);
  uint64_t far = 5ull << 30;
  CHECK(put("/EXT/sparse5g", far, b, 4096) == 0, "sparse at 5G");
  CHECK(put("/EXT/sparse5g", 3, b, 10) == 0, "sparse head");
  uint32_t sp = ino_of("/EXT/sparse5g");
  struct ext2_stat st;
  ext2_stat(&FS, sp, &st);
  CHECK(st.size == far + 4096, "sparse size %llu", (unsigned long long)st.size);
  uint8_t chk[4096];
  CHECK(ext2_read(&FS, sp, far, chk, 4096) == 4096 && !memcmp(chk, b, 4096),
        "sparse readback");
  CHECK(ext2_read(&FS, sp, far - 4096, chk, 4096) == 4096 && chk[0] == 0 &&
            chk[4095] == 0,
        "sparse hole zero");
  /* truncate: shrink into the middle of a block, regrow -> zeros */
  fill(b, 20000, 4);
  CHECK(put("/EXT/trunc", 0, b, 20000) == 0, "put trunc");
  uint32_t ti = ino_of("/EXT/trunc");
  CHECK(ext2_truncate(&FS, ti, 5001) == 0, "truncate 5001");
  CHECK(ext2_truncate(&FS, ti, 9000) == 0, "truncate up 9000");
  CHECK(ext2_read(&FS, ti, 0, chk, 4096) == 4096 && !memcmp(chk, b, 4096),
        "trunc head");
  uint8_t t2[5000];
  CHECK(ext2_read(&FS, ti, 5000, t2, 4000) == 4000 && t2[0] == b[5000] &&
            t2[1] == 0 && t2[3999] == 0,
        "trunc tail zero");
  /* the 5 GiB file shrunk to 1 byte gives every indirect block back */
  CHECK(ext2_truncate(&FS, sp, 1) == 0, "truncate sparse");
  ext2_stat(&FS, sp, &st);
  CHECK(st.blocks == (uint64_t)(bs / 512), "sparse blocks after trunc %llu",
        (unsigned long long)st.blocks);
  /* a second 5 GiB sparse file that STAYS (fsck walks the triple tree) */
  fill(b, 4096, 9);
  CHECK(put("/EXT/keep5g", far, b, 4096) == 0, "keep5g");
  /* rename: same dir, across dirs, over an existing file, dir over empty dir */
  fill(b, 100, 5);
  CHECK(put("/EXT/r1", 0, b, 100) == 0, "r1");
  CHECK(rename_at("/EXT/r1", "/EXT/r2") == 0, "rename same dir");
  CHECK(rename_at("/EXT/r2", "/EXT/d1/d2/r3") == 0, "rename across");
  fill(b, 50, 6);
  CHECK(put("/EXT/victim", 0, b, 50) == 0, "victim");
  fill(b, 100, 5);
  CHECK(rename_at("/EXT/d1/d2/r3", "/EXT/victim") == 0, "rename replace");
  CHECK(mkdir_p("/EXT/mvdir") == 0 && mkdir_p("/EXT/mvdir/child") == 0,
        "mvdir");
  fill(b, 7, 7);
  CHECK(put("/EXT/mvdir/child/f", 0, b, 7) == 0, "mvdir file");
  CHECK(mkdir_p("/EXT/d1/emptytarget") == 0, "empty target");
  CHECK(rename_at("/EXT/mvdir", "/EXT/d1/emptytarget") == 0,
        "rename dir over empty dir");
  CHECK(rename_at("/EXT/d1", "/EXT/d1/emptytarget/child/x") == -EINVAL,
        "rename into own subtree");
  CHECK(mkdir_p("/EXT/full") == 0 && put("/EXT/full/x", 0, b, 1) == 0, "full");
  CHECK(rename_at("/EXT/pre", "/EXT/full") == -ENOTEMPTY, "ENOTEMPTY");
  CHECK(rename_at("/EXT/a.txt", "/EXT/full") == -EISDIR, "EISDIR");
  CHECK(rename_at("/EXT/full", "/EXT/a.txt") == -ENOTDIR, "ENOTDIR");
  CHECK(rename_at("/EXT/pre/inner", "/EXT/inner2") == 0, "rename debugfs dir");
  /* hard link, then unlink one name */
  uint32_t hi = ino_of("/EXT/a.txt");
  CHECK(ext2_link(&FS, hi, ino_of("/EXT/d1"), "a_link", 6) == 0, "link");
  ext2_stat(&FS, hi, &st);
  CHECK(st.nlink == 2, "nlink 2");
  /* rmdir */
  CHECK(mkdir_p("/EXT/gone") == 0, "mkdir gone");
  CHECK(rmdir_at("/EXT/d1") == -ENOTEMPTY, "rmdir nonempty");
  CHECK(rmdir_at("/EXT/gone") == 0, "rmdir");
  CHECK(rmdir_at("/EXT/a.txt") == -ENOTDIR, "rmdir file");
  CHECK(unlink_at("/EXT/d1") == -EISDIR, "unlink dir");
  /* open-unlinked: freed at the last iput, not at unlink */
  fill(b, 9000, 8);
  CHECK(put("/EXT/tmpfile", 0, b, 9000) == 0, "tmpfile");
  uint32_t tmpi = ino_of("/EXT/tmpfile");
  uint32_t fb_before = FS.free_blocks;
  ext2_iget(&FS, tmpi);
  CHECK(unlink_at("/EXT/tmpfile") == 0, "unlink open");
  CHECK(FS.free_blocks == fb_before, "blocks kept while open");
  CHECK(ext2_read(&FS, tmpi, 0, chk, 100) == 100 && !memcmp(chk, b, 100),
        "read after unlink");
  ext2_iput(&FS, tmpi);
  CHECK(FS.free_blocks > fb_before, "freed at last close");
  /* name validation */
  uint32_t xi;
  char longname[300];
  memset(longname, 'n', 256);
  CHECK(ext2_create(&FS, ino_of("/EXT"), longname, 256, 0644, &xi) ==
            -ENAMETOOLONG,
        "ENAMETOOLONG");
  CHECK(ext2_create(&FS, ino_of("/EXT"), "a/b", 3, 0644, &xi) == -EINVAL,
        "slash in name");
  /* fill the disk: ENOSPC is a clean short write, fsck still clean after */
  {
    struct ext2_statfs sf;
    ext2_statfs(&FS, &sf);
    uint64_t room = sf.bfree * (uint64_t)bs;
    uint64_t want_bytes = room + (1u << 20);
    uint8_t *chunk = malloc(1 << 20);
    fill(chunk, 1 << 20, 77);
    uint32_t fi = 0;
    struct ext2_res fr;
    res("/EXT/filler", 0, &fr);
    ext2_create(&FS, fr.parent, fr.name, fr.name_len, 0644, &fi);
    uint64_t off = 0;
    int64_t w = 0;
    while (off < want_bytes) {
      w = ext2_write(&FS, fi, off, chunk, 1 << 20);
      if (w <= 0) break;
      off += (uint64_t)w;
      if (w < (1 << 20)) {
        w = ext2_write(&FS, fi, off, chunk, 1 << 20);
        break;
      }
    }
    CHECK(w == -ENOSPC, "ENOSPC at full (w=%lld off=%llu)", (long long)w,
          (unsigned long long)off);
    CHECK(FS.free_blocks == 0, "free 0 at full: %u", FS.free_blocks);
    /* give it back */
    CHECK(unlink_at("/EXT/filler") == 0, "unlink filler");
    free(chunk);
  }
  CHECK(FS.corrupt == 0, "no corruption detected on a healthy image");
  uint64_t free1 = FS.free_blocks;
  umount_img();
  printf("  free blocks %llu -> %llu, dev reads %llu writes %llu\n",
         (unsigned long long)free0, (unsigned long long)free1,
         (unsigned long long)FS.st_dev_reads,
         (unsigned long long)FS.st_dev_writes);
  /* ---- the oracle ---- */
  CHECK(fsck(img, "after writes") == 0, "e2fsck clean after writes bs=%d", bs);
  char slowt[200];
  memset(slowt, 'z', 150);
  slowt[150] = 0;
  struct want w[] = {
      {"a.txt", 'f', 1, 5000, 0},
      {"big.bin", 'f', 2, big, 0},
      {"d1/d2/d3/leaf", 'f', 3, 3, 0},
      {"d1/a_link", 'f', 1, 5000, 0},
      {"victim", 'f', 5, 100, 0},
      {"d1/d2/r3", 'x', 0, 0, 0},
      {"r1", 'x', 0, 0, 0},
      {"r2", 'x', 0, 0, 0},
      {"mvdir", 'x', 0, 0, 0},
      {"d1/emptytarget/child/f", 'f', 7, 7, 0},
      {"inner2", 'd', 0, 0, 0},
      {"pre/inner", 'x', 0, 0, 0},
      {"gone", 'x', 0, 0, 0},
      {"tmpfile", 'x', 0, 0, 0},
      {"filler", 'x', 0, 0, 0},
      {"fast", 'l', 0, 0, "d1/d2/d3/leaf"},
      {"slow", 'l', 0, 0, slowt},
      {"abs", 'l', 0, 0, "/EXT/d1"},
      {"out", 'l', 0, 0, "/NFS/game"},
      {"up", 'l', 0, 0, "../CP/WINE"},
      {"many/f001_padding_padding_padding", 'f', 501, 1, 0},
      {"many/f399_padding_padding_padding", 'f', 899, 49, 0},
      {"many/f000_padding_padding_padding", 'x', 0, 0, 0},
      {"many/f398_padding_padding_padding", 'x', 0, 0, 0},
  };
  verify_rdump(img, w, (int)(sizeof w / sizeof w[0]), "write");
  long links = dbg_stat_num(img, "/a.txt", "Links:");
  CHECK(links == 2, "debugfs Links a.txt = %ld", links);
  long d1links = dbg_stat_num(img, "/d1", "Links:");
  CHECK(d1links == 4, "debugfs Links d1 = %ld (., .., d2, emptytarget)",
        d1links);
  long ksz = dbg_stat_num(img, "/keep5g", "Size:");
  CHECK(ksz == (long)(far + 4096), "debugfs Size keep5g = %ld", ksz);
  /* remount with the driver: everything persisted */
  rc = mount_img(img, 0, cache);
  CHECK(rc == 0, "remount rc=%d", rc);
  size_t n;
  fill(b, big, 2);
  CHECK(read_all("/EXT/big.bin", chk, sizeof chk, &n) == 0 && n == 4096 &&
            !memcmp(chk, b, 4096),
        "remount read");
  int cnt = 0;
  CHECK(count_dir("/EXT/many", &cnt) == 0 && cnt == 202, "many count %d", cnt);
  /* delete everything we made except the root: fsck must still be clean */
  CHECK(unlink_at("/EXT/big.bin") == 0 && unlink_at("/EXT/keep5g") == 0,
        "cleanup unlinks");
  for (int i = 1; i < 400; i += 2) {
    char p[200];
    snprintf(p, sizeof p, "/EXT/many/f%03d_padding_padding_padding", i);
    if (unlink_at(p)) {
      CHECK(0, "unlink %s", p);
      break;
    }
  }
  CHECK(rmdir_at("/EXT/many") == 0, "rmdir many");
  umount_img();
  CHECK(fsck(img, "after cleanup") == 0, "e2fsck clean after cleanup bs=%d", bs);
  free(b);
}

/* ---- scenario 3: dir_index directories made by e2fsck -D --------------- */

static void scenario_htree(void) {
  char img[300], src[300];
  snprintf(img, sizeof img, "%s/h.img", tmpdir);
  snprintf(src, sizeof src, "%s/hsrc", tmpdir);
  run("rm -rf %s && mkdir -p %s/hdir", src, src);
  for (int i = 0; i < 600; i++) {
    char p[400];
    snprintf(p, sizeof p, "%s/hdir/name_%04d_xxxxxxxxxxxxxxxxxxxxxxxx", src, i);
    write_host_file(p, (const uint8_t *)"h", 1);
  }
  run("rm -f %s && truncate -s 64M %s", img, img);
  CHECK(run("%s -q -F -t ext2 -b 1024 -d %s %s >/dev/null 2>&1", mke2fs_bin,
            src, img) == 0,
        "mke2fs htree");
  /* -D builds hash-tree indexes for multi-block directories */
  run("%s -fyD %s >/dev/null 2>&1", e2fsck_bin, img);
  long flags = 0;
  {
    char out[8192];
    capture(out, sizeof out, "%s -R 'stat /hdir' %s 2>/dev/null", debugfs_bin,
            img);
    char *p = strstr(out, "Flags: 0x");
    if (p) flags = strtol(p + 9, 0, 16);
  }
  printf("scenario htree: /hdir flags=0x%lx (INDEX_FL=0x1000)\n", flags);
  CHECK(flags & 0x1000, "e2fsck -D produced an indexed dir");
  CHECK(mount_img(img, 0, 64) == 0, "mount htree");
  int cnt = 0;
  CHECK(count_dir("/EXT/hdir", &cnt) == 0 && cnt == 602, "htree readdir %d",
        cnt);
  CHECK(ino_of("/EXT/hdir/name_0599_xxxxxxxxxxxxxxxxxxxxxxxx") != 0,
        "htree lookup");
  for (int i = 0; i < 600; i += 3) {
    char p[200];
    snprintf(p, sizeof p, "/EXT/hdir/name_%04d_xxxxxxxxxxxxxxxxxxxxxxxx", i);
    if (unlink_at(p)) {
      CHECK(0, "htree unlink %d", i);
      break;
    }
  }
  CHECK(put("/EXT/hdir/added_after_index", 0, (const uint8_t *)"abc", 3) == 0,
        "htree add");
  umount_img();
  CHECK(fsck(img, "htree modified") == 0, "e2fsck clean after modifying htree dir");
  {
    char out[8192];
    capture(out, sizeof out, "%s -R 'stat /hdir' %s 2>/dev/null", debugfs_bin,
            img);
    char *p = strstr(out, "Flags: 0x");
    flags = p ? strtol(p + 9, 0, 16) : -1;
  }
  CHECK(flags >= 0 && !(flags & 0x1000), "INDEX_FL cleared on write (0x%lx)",
        flags);
}

/* ---- scenario: setattr (cp/ext2b: chmod/chown/utimensat) ---------------- */

/* debugfs `stat <path>` field -> number.  `key` like "Mode:" / "User:" /
   "atime: 0x" (hex). */
static long dbg_field(const char *img, const char *path, const char *key,
                      int hex) {
  char out[8192];
  capture(out, sizeof out, "%s -R 'stat %s' %s 2>/dev/null", debugfs_bin, path,
          img);
  char *p = strstr(out, key);
  if (!p) return -1;
  p += strlen(key);
  while (*p == ' ') p++;
  return strtol(p, 0, hex ? 16 : (key[0] == 'M' ? 8 : 10));
}

static void scenario_setattr(int bs, int isize) {
  char img[300];
  snprintf(img, sizeof img, "%s/sa%d.img", tmpdir, bs);
  run("rm -f %s && truncate -s 32M %s", img, img);
  CHECK(run("%s -q -F -t ext2 -b %d -I %d -O ^dir_index %s >/dev/null 2>&1",
            mke2fs_bin, bs, isize, img) == 0,
        "mke2fs setattr bs=%d", bs);
  printf("scenario setattr bs=%d inode=%d\n", bs, isize);
  CHECK(mount_img(img, 0, 32) == 0, "mount setattr");
  CHECK(mkdir_p("/EXT/sa") == 0, "mkdir sa");
  CHECK(put("/EXT/sa/f", 0, (const uint8_t *)"data", 4) == 0, "put f");
  CHECK(mkdir_p("/EXT/sa/d") == 0, "mkdir d");
  CHECK(symlink_at("/EXT/sa/l", "f") == 0, "symlink l");
  uint32_t fi = ino_of("/EXT/sa/f"), di = ino_of("/EXT/sa/d");
  struct ext2_res lr;
  CHECK(res("/EXT/sa/l", 0, &lr) == 0, "resolve l nofollow");
  uint32_t li = lr.ino;
  struct ext2_stat st;
  struct ext2_attr a;

  /* chmod: permission bits replaced, type kept, ctime = now */
  memset(&a, 0, sizeof a);
  a.valid = EXT2_ATTR_MODE;
  a.mode = 0104751; /* type bits in the request are ignored */
  CHECK(ext2_setattr(&FS, fi, &a) == 0, "chmod f");
  CHECK(ext2_stat(&FS, fi, &st) == 0 && st.mode == (EXT2_S_IFREG | 04751),
        "mode 0%o", st.mode);
  CHECK(st.ctime == host_now(), "ctime now %u", st.ctime);
  /* chown of a regular file: S_ISUID dropped; S_ISGID dropped with S_IXGRP */
  a.valid = EXT2_ATTR_MODE;
  a.mode = 06751;
  CHECK(ext2_setattr(&FS, fi, &a) == 0, "chmod 06751");
  memset(&a, 0, sizeof a);
  a.valid = EXT2_ATTR_UID | EXT2_ATTR_GID;
  a.uid = 70000; /* > 16 bits: needs the osd2 high half */
  a.gid = 1001;
  CHECK(ext2_setattr(&FS, fi, &a) == 0, "chown f");
  CHECK(ext2_stat(&FS, fi, &st) == 0 && st.uid == 70000 && st.gid == 1001 &&
            st.mode == (EXT2_S_IFREG | 0751),
        "chown f uid=%u gid=%u mode=0%o", st.uid, st.gid, st.mode);
  /* a directory keeps S_ISGID/sticky across chown */
  a.valid = EXT2_ATTR_MODE;
  a.mode = 03775;
  CHECK(ext2_setattr(&FS, di, &a) == 0, "chmod d");
  a.valid = EXT2_ATTR_GID;
  a.gid = 50;
  CHECK(ext2_setattr(&FS, di, &a) == 0, "chgrp d");
  CHECK(ext2_stat(&FS, di, &st) == 0 && st.mode == (EXT2_S_IFDIR | 03775) &&
            st.gid == 50,
        "dir mode 0%o gid %u", st.mode, st.gid);
  /* lchown changes the link inode, not its target */
  a.valid = EXT2_ATTR_UID;
  a.uid = 5;
  CHECK(ext2_setattr(&FS, li, &a) == 0, "lchown l");
  CHECK(ext2_stat(&FS, li, &st) == 0 && st.uid == 5, "link uid %u", st.uid);
  CHECK(ext2_stat(&FS, fi, &st) == 0 && st.uid == 70000, "target uid kept");
  /* times: explicit, negative (pre-1970), clamped, now; omitted keeps */
  memset(&a, 0, sizeof a);
  a.valid = EXT2_ATTR_ATIME | EXT2_ATTR_MTIME;
  a.atime = 1000000000;
  a.mtime = 1234567890;
  CHECK(ext2_setattr(&FS, fi, &a) == 0, "utimes f");
  CHECK(ext2_stat(&FS, fi, &st) == 0 && st.atime == 1000000000u &&
            st.mtime == 1234567890u,
        "times %u %u", st.atime, st.mtime);
  a.valid = EXT2_ATTR_MTIME;
  a.mtime = -86400;
  CHECK(ext2_setattr(&FS, di, &a) == 0, "negative mtime d");
  CHECK(ext2_stat(&FS, di, &st) == 0 && (int32_t)st.mtime == -86400,
        "negative mtime %d", (int32_t)st.mtime);
  a.valid = EXT2_ATTR_ATIME_NOW;
  a.atime = 0;
  CHECK(ext2_setattr(&FS, di, &a) == 0 && ext2_stat(&FS, di, &st) == 0 &&
            st.atime == host_now() && (int32_t)st.mtime == -86400,
        "atime now, mtime kept");
  a.valid = EXT2_ATTR_ATIME;
  a.atime = (int64_t)1 << 40;
  CHECK(ext2_setattr(&FS, li, &a) == 0 && ext2_stat(&FS, li, &st) == 0 &&
            st.atime == 0x7FFFFFFFu,
        "clamped atime 0x%x", st.atime);
  a.valid = 0;
  CHECK(ext2_setattr(&FS, fi, &a) == 0, "empty setattr is a no-op");
  umount_img();
  CHECK(fsck(img, "setattr") == 0, "e2fsck clean after setattr");
  /* the oracle: debugfs reads the same values back from the image */
  long m = dbg_field(img, "/sa/f", "Mode:", 0);
  long u = dbg_field(img, "/sa/f", "User:", 0);
  long g = dbg_field(img, "/sa/f", "Group:", 0);
  long at = dbg_field(img, "/sa/f", "atime: 0x", 1);
  long mt = dbg_field(img, "/sa/f", "mtime: 0x", 1);
  printf("  debugfs /sa/f: Mode 0%lo User %ld Group %ld atime 0x%lx mtime 0x%lx\n",
         m, u, g, at, mt);
  CHECK(m == 0751 && u == 70000 && g == 1001, "debugfs f mode/owner");
  CHECK(at == 1000000000L && mt == 1234567890L, "debugfs f times");
  long dm = dbg_field(img, "/sa/d", "Mode:", 0);
  long dmt = dbg_field(img, "/sa/d", "mtime: 0x", 1);
  printf("  debugfs /sa/d: Mode 0%lo mtime 0x%lx\n", dm, dmt);
  CHECK(dm == 03775 && dmt == 0xFFFEAE80L, "debugfs d mode/mtime");
  CHECK(dbg_field(img, "/sa/l", "User:", 0) == 5, "debugfs link owner");
  /* read-only mount refuses */
  CHECK(mount_img(img, 1, 32) == 0, "mount ro");
  a.valid = EXT2_ATTR_MODE;
  a.mode = 0600;
  CHECK(ext2_setattr(&FS, ino_of("/EXT/sa/f"), &a) == -EROFS, "ro EROFS");
  umount_img();
}

/* ---- scenario: paths longer than the old 256-byte VFS_PATH_MAX ---------- */

static void scenario_longpath(void) {
  char img[300];
  snprintf(img, sizeof img, "%s/lp.img", tmpdir);
  run("rm -f %s && truncate -s 16M %s", img, img);
  CHECK(run("%s -q -F -t ext2 -b 4096 %s >/dev/null 2>&1", mke2fs_bin, img) == 0,
        "mke2fs longpath");
  printf("scenario longpath (EXT2_PATH_MAX=%d)\n", EXT2_PATH_MAX);
  CHECK(mount_img(img, 0, 32) == 0, "mount longpath");
  /* 12 components of 50 bytes: a ~620-byte path */
  char path[EXT2_PATH_MAX + 64];
  int n = snprintf(path, sizeof path, "/EXT");
  for (int i = 0; i < 12; i++) {
    n += snprintf(path + n, sizeof path - (size_t)n,
                  "/d%02d_abcdefghijklmnopqrstuvwxyz_0123456789_ABCDEFGHI", i);
    CHECK(mkdir_p(path) == 0, "mkdir depth %d", i);
  }
  n += snprintf(path + n, sizeof path - (size_t)n, "/leaf.txt");
  printf("  long path length %d\n", n);
  CHECK(n > 600, "path is > 600 bytes");
  CHECK(put(path, 0, (const uint8_t *)"deep", 4) == 0, "put long-path file");
  struct ext2_res r;
  CHECK(res(path, 1, &r) == 0 && strcmp(r.path, path) == 0,
        "resolve long path (canonical form round-trips)");
  /* a ".." inside a long path */
  char dd[EXT2_PATH_MAX + 64];
  snprintf(dd, sizeof dd, "%s/../leaf.txt", path);
  CHECK(res(dd, 1, &r) == 0 && strcmp(r.path, path) == 0, "long path with ..");
  /* past EXT2_PATH_MAX: refused, never truncated */
  char toolong[EXT2_PATH_MAX + 200];
  int k = snprintf(toolong, sizeof toolong, "%s", path);
  while (k < EXT2_PATH_MAX + 20) toolong[k++] = 'x';
  toolong[k] = '\0';
  CHECK(res(toolong, 1, &r) == -ENAMETOOLONG, "over-long path ENAMETOOLONG");
  umount_img();
  CHECK(fsck(img, "longpath") == 0, "e2fsck clean after long paths");
  char out[4096];
  char dpath[EXT2_PATH_MAX + 64];
  snprintf(dpath, sizeof dpath, "%s", path + 4); /* strip /EXT */
  capture(out, sizeof out, "%s -R 'cat %s' %s 2>/dev/null", debugfs_bin, dpath,
          img);
  CHECK(strcmp(out, "deep") == 0, "debugfs cat long path = '%s'", out);
}

/* ---- scenario 4: corrupted / unsupported images must fail cleanly ------- */

static uint32_t dbg_first_block(const char *img, const char *path) {
  char out[4096];
  capture(out, sizeof out, "%s -R 'blocks %s' %s 2>/dev/null", debugfs_bin,
          path, img);
  return (uint32_t)strtoul(out, 0, 10);
}

static void patch_bytes(const char *img, uint64_t off, const void *b,
                        size_t n) {
  int fd = open(img, O_RDWR);
  if (fd < 0) return;
  ssize_t w = pwrite(fd, b, n, (off_t)off);
  (void)w;
  close(fd);
}

static void scenario_corrupt(void) {
  char base[300], img[300], src[300];
  snprintf(base, sizeof base, "%s/c-base.img", tmpdir);
  snprintf(img, sizeof img, "%s/c.img", tmpdir);
  snprintf(src, sizeof src, "%s/csrc", tmpdir);
  run("rm -rf %s && mkdir -p %s/dir %s/dir2", src, src, src);
  uint8_t *b = malloc(200000);
  fill(b, 200000, 21);
  char p[400];
  snprintf(p, sizeof p, "%s/file", src);
  write_host_file(p, b, 200000);
  snprintf(p, sizeof p, "%s/dir/inner", src);
  write_host_file(p, b, 10);
  snprintf(p, sizeof p, "%s/dir2/ok", src);
  write_host_file(p, b, 10);
  run("rm -f %s && truncate -s 32M %s", base, base);
  CHECK(run("%s -q -F -t ext2 -b 4096 -d %s %s >/dev/null 2>&1", mke2fs_bin,
            src, base) == 0,
        "mke2fs corrupt base");
  printf("scenario corrupt\n");
  struct ext2_res r;
  uint8_t buf[4096];

  /* (a) bad rec_len in /dir's first block */
  run("cp %s %s", base, img);
  uint32_t dblk = dbg_first_block(img, "/dir");
  uint8_t bad[2] = {3, 0}; /* rec_len 3: not a multiple of 4, < 8 */
  patch_bytes(img, (uint64_t)dblk * 4096 + 4, bad, 2);
  CHECK(mount_img(img, 0, 32) == 0, "mount (a)");
  CHECK(res("/EXT/dir/inner", 0, &r) == -EIO, "bad rec_len lookup -> EIO");
  {
    uint64_t pos = 0;
    uint32_t ino;
    uint8_t ft;
    char nm[256];
    int nl;
    int rc = ext2_readdir(&FS, ino_of("/EXT/dir") ? ino_of("/EXT/dir") : 0,
                          &pos, &ino, &ft, nm, &nl);
    CHECK(rc == -EIO, "bad rec_len readdir rc=%d", rc);
  }
  CHECK(ino_of("/EXT/dir2/ok") != 0, "other dirs still readable");
  CHECK(FS.corrupt == 1, "corrupt flag set");
  umount_img();

  /* (b) rec_len running past the block end */
  run("cp %s %s", base, img);
  uint8_t past[2] = {0x00, 0x20}; /* 8192 > 4096 */
  patch_bytes(img, (uint64_t)dblk * 4096 + 4, past, 2);
  CHECK(mount_img(img, 1, 32) == 0, "mount (b)");
  CHECK(res("/EXT/dir/inner", 0, &r) == -EIO, "rec_len past block -> EIO");
  umount_img();

  /* (c) out-of-range direct block pointer in a file */
  run("cp %s %s", base, img);
  run("%s -w -R 'sif /file block[0] 4000000' %s >/dev/null 2>&1", debugfs_bin,
      img);
  CHECK(mount_img(img, 1, 32) == 0, "mount (c)");
  {
    uint32_t fi = ino_of("/EXT/file");
    int64_t rr = ext2_read(&FS, fi, 0, buf, 4096);
    CHECK(rr == -EIO, "bad block[0] read -> %lld", (long long)rr);
    rr = ext2_read(&FS, fi, 4096, buf, 4096);
    CHECK(rr == 4096 && !memcmp(buf, b + 4096, 4096), "block[1] still fine");
  }
  umount_img();

  /* (d) out-of-range single-indirect pointer (file has > 12 blocks) */
  run("cp %s %s", base, img);
  run("%s -w -R 'sif /file block[IND] 4000001' %s >/dev/null 2>&1",
      debugfs_bin, img);
  CHECK(mount_img(img, 0, 32) == 0, "mount (d)");
  {
    uint32_t fi = ino_of("/EXT/file");
    int64_t rr = ext2_read(&FS, fi, 13 * 4096, buf, 4096);
    CHECK(rr == -EIO, "bad IND read -> %lld", (long long)rr);
    rr = ext2_write(&FS, fi, 13 * 4096, buf, 4096);
    CHECK(rr == -EIO, "bad IND write -> %lld", (long long)rr);
    CHECK(ext2_truncate(&FS, fi, 0) == -EIO, "bad IND truncate -> EIO");
  }
  umount_img();

  /* (e) a dirent naming an inode beyond s_inodes_count */
  run("cp %s %s", base, img);
  uint8_t huge[4] = {0xF0, 0xFF, 0xFF, 0x0F};
  /* third entry of /dir (".", ".." are 12 bytes each) is "inner" */
  patch_bytes(img, (uint64_t)dblk * 4096 + 24, huge, 4);
  CHECK(mount_img(img, 1, 32) == 0, "mount (e)");
  CHECK(res("/EXT/dir/inner", 0, &r) == -EIO, "dirent ino out of range -> EIO");
  umount_img();

  /* (f) bad magic */
  run("cp %s %s", base, img);
  uint8_t zero2[2] = {0, 0};
  patch_bytes(img, 1024 + 56, zero2, 2);
  CHECK(mount_img(img, 0, 32) == -EINVAL, "bad magic -> EINVAL");

  /* (g) unknown INCOMPAT feature (extents) refuses the mount */
  run("cp %s %s", base, img);
  run("%s -w -R 'feature extent' %s >/dev/null 2>&1", debugfs_bin, img);
  CHECK(mount_img(img, 0, 32) == -ENOTSUP, "INCOMPAT extent -> ENOTSUP");

  /* (h) unknown RO_COMPAT feature mounts read-only */
  run("cp %s %s", base, img);
  run("%s -w -R 'feature huge_file' %s >/dev/null 2>&1", debugfs_bin, img);
  CHECK(mount_img(img, 0, 32) == 0 && FS.ro == 1, "RO_COMPAT huge_file -> ro");
  CHECK(ino_of("/EXT/file") != 0, "ro mount still reads");
  CHECK(put("/EXT/newfile", 0, buf, 1) == -EROFS, "ro create -> EROFS");
  umount_img();

  /* (i) truncated device */
  run("cp %s %s && truncate -s 8M %s", base, img, img);
  CHECK(mount_img(img, 0, 32) == -EINVAL, "short device -> EINVAL");

  /* (j) group descriptor pointing outside the fs */
  run("cp %s %s", base, img);
  uint8_t gbad[4] = {0xFF, 0xFF, 0xFF, 0x7F};
  patch_bytes(img, 4096 + 8, gbad, 4); /* bg_inode_table of group 0 */
  CHECK(mount_img(img, 0, 32) == -EINVAL, "bad gd -> EINVAL");

  /* (k) indirect block full of garbage pointers: truncate must not free
     random blocks (refuses with EIO, bitmaps untouched) */
  run("cp %s %s", base, img);
  {
    char out[4096];
    capture(out, sizeof out, "%s -R 'stat /file' %s 2>/dev/null | grep -o '(IND):[0-9]*'",
            debugfs_bin, img);
    char *c = strchr(out, ':');
    uint32_t ind = c ? (uint32_t)strtoul(c + 1, 0, 10) : 0;
    CHECK(ind != 0, "found IND block");
    uint8_t g[4096];
    for (int i = 0; i < 4096; i += 4) {
      g[i] = 0x10;
      g[i + 1] = 0x27;
      g[i + 2] = 0x5A;
      g[i + 3] = 0x00; /* 5908240 > blocks_count */
    }
    if (ind) patch_bytes(img, (uint64_t)ind * 4096, g, 4096);
    CHECK(mount_img(img, 0, 32) == 0, "mount (k)");
    uint32_t fb = FS.free_blocks;
    uint32_t fi = ino_of("/EXT/file");
    CHECK(ext2_read(&FS, fi, 20 * 4096, buf, 4096) == -EIO, "garbage IND read");
    CHECK(ext2_truncate(&FS, fi, 0) == -EIO, "garbage IND truncate -> EIO");
    CHECK(FS.free_blocks >= fb, "no bogus frees");
    umount_img();
  }
  free(b);
}

/* ---- scenario 5: a 1 KiB fs with a file > 4 GiB of LOGICAL size and data
   in the triple-indirect range (1 KiB: triple starts at ~64.3 MiB) -------- */

static void scenario_large_1k(void) {
  char img[300];
  snprintf(img, sizeof img, "%s/l1k.img", tmpdir);
  run("rm -f %s && truncate -s 48M %s", img, img);
  CHECK(run("%s -q -F -t ext2 -b 1024 -O ^large_file %s >/dev/null 2>&1",
            mke2fs_bin, img) == 0,
        "mke2fs 1k");
  printf("scenario large-file bs=1024 (large_file initially off)\n");
  CHECK(mount_img(img, 0, 128) == 0, "mount 1k");
  CHECK(!(FS.feat_ro & EXT2_FEATURE_RO_COMPAT_LARGE_FILE), "large_file off");
  uint8_t b[3000];
  fill(b, sizeof b, 31);
  uint64_t offs[] = {7 * 1024, 20 * 1024 + 7, 300 * 1024, 70ull << 20,
                     (4ull << 30) + 123, 9ull << 30};
  for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
    CHECK(put("/EXT/huge", offs[i], b, sizeof b) == 0, "put @%llu",
          (unsigned long long)offs[i]);
  CHECK(FS.feat_ro & EXT2_FEATURE_RO_COMPAT_LARGE_FILE, "large_file set");
  uint32_t hi = ino_of("/EXT/huge");
  uint8_t c[3000];
  for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
    CHECK(ext2_read(&FS, hi, offs[i], c, sizeof c) == (int64_t)sizeof c &&
              !memcmp(c, b, sizeof c),
          "read @%llu", (unsigned long long)offs[i]);
  /* past the 1 KiB geometry's maximum: EFBIG */
  CHECK(ext2_write(&FS, hi, 17ull << 30, b, 10) == -EFBIG, "EFBIG");
  umount_img();
  CHECK(fsck(img, "1k large") == 0, "e2fsck clean 1k large file");
  long sz = dbg_stat_num(img, "/huge", "Size:");
  CHECK(sz == (long)((9ull << 30) + 3000), "debugfs size %ld", sz);
  /* debugfs's own block map (bmap) of the triple-indirect tail, read
     straight from the image: an oracle independent of the driver's bmap. */
  int ok = 1;
  int fd = open(img, O_RDONLY);
  for (uint64_t k = 0; k < 3; k++) {
    char out[256];
    uint64_t lblk = ((9ull << 30) >> 10) + k;
    capture(out, sizeof out, "%s -R 'bmap /huge %llu' %s 2>/dev/null",
            debugfs_bin, (unsigned long long)lblk, img);
    uint64_t pb = strtoull(out, 0, 10);
    uint8_t t[1024];
    size_t want = k < 2 ? 1024 : 3000 - 2048;
    ssize_t rr = (fd >= 0 && pb) ? pread(fd, t, want, (off_t)(pb * 1024)) : -1;
    int m = rr == (ssize_t)want && !memcmp(t, b + k * 1024, want);
    printf("  debugfs bmap /huge %llu -> %llu %s\n", (unsigned long long)lblk,
           (unsigned long long)pb, m ? "match" : "MISMATCH");
    ok &= m;
  }
  if (fd >= 0) close(fd);
  CHECK(ok, "debugfs-mapped tail blocks match the written bytes");
}

/* ---- cp/ext2w: crash-prefix check of the ordered commit ------------------
 *
 * For every batch: the image as the previous commit left it, then EVERY
 * prefix of the device writes the batch made (eviction write-backs and the
 * commit, split per block) is checked with e2fsck -fn.  A prefix may show only
 * repairable leaks (docs/cp-ext2w.md section 2.3): '-' bitmap differences,
 * wrong free/directory counts, unattached inodes, link counts that are too
 * HIGH.  Anything else -- a reference to a free block or inode ('+' bitmap
 * differences), a directory entry naming a deleted or uninitialised inode,
 * illegal or multiply-claimed blocks, a link count too low -- is a failure.
 * The same batches with the commit in plain block order (fs->ordered = 0,
 * the pre-ext2w behaviour) must show such failures, or the check proves
 * nothing. */
static int fsck_class(const char *img, char *why, size_t wcap) {
  static char out[65536];
  int rc = capture(out, sizeof out, "%s -fn %s 2>&1", e2fsck_bin, img);
  if (rc == 0) return 0;
  int worst = 1;
  why[0] = 0;
  char *save = 0;
  for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(0, "\n", &save)) {
    while (*ln == ' ') ln++;
    if (!*ln) continue;
    static const char *ok_pfx[] = {
        "e2fsck ", "Pass ", "Fix? no", "Clear? no", "Connect to /lost+found? no",
        "Free blocks count wrong", "Free inodes count wrong",
        "Directories count wrong", "Unattached inode", "Unattached zero-length inode",
        "Padding at end of", "Inode bitmap differences:", "Block bitmap differences:",
        "Fix? yes", "/lost+found not found", "Create? no", "Relocate? no", 0};
    int ok = 0;
    for (int i = 0; ok_pfx[i]; i++)
      if (!strncmp(ln, ok_pfx[i], strlen(ok_pfx[i]))) ok = 1;
    if (strstr(ln, "***** WARNING: Filesystem still has errors")) ok = 1;
    if (strstr(ln, " files (") && strstr(ln, " blocks")) ok = 1; /* summary */
    if (!strncmp(ln, "Block bitmap differences:", 25) ||
        !strncmp(ln, "Inode bitmap differences:", 25)) {
      if (strchr(ln + 25, '+')) ok = 0; /* in use but marked free */
    }
    if (!strncmp(ln, "Unconnected directory inode", 27)) ok = 1; /* -> lost+found */
    if (!strncmp(ln, "'..' in ", 8)) ok = 1; /* a moved/unconnected dir's parent */
    unsigned ino = 0, a = 0, b = 0;
    if (sscanf(ln, "Inode %u ref count is %u, should be %u", &ino, &a, &b) == 3)
      ok = a > b || b == a + 1; /* too high: a leak; one short: a rename's
                                   second name (Linux ext2 alike) */
    if (!ok) {
      worst = 2;
      if (!why[0]) snprintf(why, wcap, "%s", ln);
      if (getenv("EXT2_CRASH_VERBOSE")) printf("    danger: %s\n", ln);
    }
  }
  return worst;
}

static int crash_apply(const char *img, const struct wrec *w, int blk_bytes,
                       int part) {
  int fd = open(img, O_RDWR);
  if (fd < 0) return -1;
  uint64_t off = w->sector * 512 + (uint64_t)part * (uint64_t)blk_bytes;
  ssize_t r = pwrite(fd, w->data + (size_t)part * (size_t)blk_bytes,
                     (size_t)blk_bytes, (off_t)off);
  close(fd);
  return r == blk_bytes ? 0 : -1;
}

/* One random batch of namespace and data operations under /EXT/c. */
static void crash_batch(uint32_t *seed, int round) {
  static uint8_t buf[60000];
  char a[64], b2[64];
  for (int op = 0; op < 24; op++) {
    *seed = *seed * 1103515245u + 12345u;
    uint32_t r = *seed >> 8;
    int f1 = (int)(r % 12), f2 = (int)((r >> 4) % 12);
    snprintf(a, sizeof a, "/EXT/c/f%d", f1);
    snprintf(b2, sizeof b2, "/EXT/c/f%d", f2);
    switch ((r >> 9) % 9) {
    case 0:
    case 1: { /* write (often past 12 blocks: indirect) */
      size_t n = 1 + (r >> 13) % sizeof buf;
      fill(buf, n, r);
      (void)put(a, ((r >> 3) & 1) ? 0 : (r % 9000), buf, n);
      break;
    }
    case 2: { /* rename over */
      uint32_t d1, d2;
      char n1[256], n2[256];
      int l1, l2;
      if (parent_of(a, &d1, n1, &l1) == 0 && parent_of(b2, &d2, n2, &l2) >= -ENOENT && f1 != f2)
        (void)ext2_rename(&FS, d1, n1, l1, d2, n2, l2);
      break;
    }
    case 3: (void)unlink_at(a); break;
    case 4: { /* truncate */
      uint32_t ino = ino_of(a);
      if (ino) (void)ext2_truncate(&FS, ino, (r >> 11) % 70000);
      break;
    }
    case 5: { /* mkdir / rmdir */
      char d[64];
      snprintf(d, sizeof d, "/EXT/c/d%d", f1 % 4);
      uint32_t dir;
      char nm[256];
      int ln;
      int rc = parent_of(d, &dir, nm, &ln);
      if (rc == -ENOENT) (void)mkdir_p(d);
      else if (rc == 0) (void)ext2_rmdir(&FS, dir, nm, ln);
      break;
    }
    case 6: { /* hard link */
      uint32_t ino = ino_of(a), dir;
      char nm[256];
      int ln;
      if (ino && f1 != f2 && parent_of(b2, &dir, nm, &ln) == -ENOENT)
        (void)ext2_link(&FS, ino, dir, nm, ln);
      break;
    }
    case 7: { /* a file inside a subdirectory */
      char p2[80];
      snprintf(p2, sizeof p2, "/EXT/c/d%d/g%d", f1 % 4, f2);
      fill(buf, 3000, r);
      (void)put(p2, 0, buf, 1 + (r % 3000));
      break;
    }
    default: { /* create + delete in the same batch */
      char t[64];
      snprintf(t, sizeof t, "/EXT/c/tmp%d", round);
      fill(buf, 20000, r);
      (void)put(t, 0, buf, 20000);
      (void)unlink_at(t);
      break;
    }
    }
  }
}

static void scenario_crash(int bs, int ordered, int rounds, int *danger_out) {
  char img[300], base[300], test[300];
  snprintf(img, sizeof img, "%s/crash%d.img", tmpdir, bs);
  snprintf(base, sizeof base, "%s/crash%d.base", tmpdir, bs);
  snprintf(test, sizeof test, "%s/crash%d.test", tmpdir, bs);
  run("rm -f %s && truncate -s 24M %s", img, img);
  CHECK(run("%s -q -F -t ext2 -b %d -O ^dir_index %s >/dev/null 2>&1", mke2fs_bin,
            bs, img) == 0, "mke2fs crash bs=%d", bs);
  int rc = mount_img(img, 0, 512);
  CHECK(rc == 0, "crash mount rc=%d", rc);
  if (rc) return;
  if (!ordered) FS.ordered = 0;
  printf("scenario crash bs=%d %s commit, %d batches\n", bs,
         ordered ? "ORDERED" : "plain-order (pre-ext2w)", rounds);
  CHECK(mkdir_p("/EXT/c") == 0, "mkdir c");
  CHECK(ext2_flush(&FS) == 0, "first commit");
  uint32_t seed = 777u + (uint32_t)bs;
  int prefixes = 0, leaks = 0, dangers = 0;
  char first[512] = "";
  for (int round = 0; round < rounds; round++) {
    run("cp %s %s", img, base);
    wlog_clear();
    wlog_on = 1;
    crash_batch(&seed, round);
    rc = ext2_flush(&FS);
    wlog_on = 0;
    CHECK(rc == 0, "commit %d rc=%d", round, rc);
    run("cp %s %s", base, test);
    for (int k = 0; k < wlog_n; k++) {
      int parts = (int)(wlog[k].nsec * 512 / (uint32_t)bs);
      if (parts < 1) parts = 1;
      for (int part = 0; part < parts; part++) {
        char why[400];
        int c = fsck_class(test, why, sizeof why);
        prefixes++;
        if (c == 1) leaks++;
        if (c == 2) {
          dangers++;
          if (!first[0])
            snprintf(first, sizeof first, "batch %d write %d/%d: %s", round, k,
                     wlog_n, why);
        }
        int bb = (int)(wlog[k].nsec * 512) < bs ? (int)(wlog[k].nsec * 512) : bs;
        CHECK(crash_apply(test, &wlog[k], bb, part) == 0, "apply");
      }
    }
    char why[400];
    CHECK(fsck_class(test, why, sizeof why) == 0, "batch %d complete: clean (%s)",
          round, why);
  }
  umount_img();
  CHECK(fsck(img, "after crash batches") == 0, "crash image clean");
  printf("  crash prefixes checked=%d clean=%d leak-only=%d DANGER=%d%s%s\n",
         prefixes, prefixes - leaks - dangers, leaks, dangers,
         first[0] ? "  first: " : "", first);
  *danger_out = dangers;
  wlog_clear();
}

int main(void) {
  if (find_tool(mke2fs_bin, sizeof mke2fs_bin, "mke2fs") ||
      find_tool(e2fsck_bin, sizeof e2fsck_bin, "e2fsck") ||
      find_tool(debugfs_bin, sizeof debugfs_bin, "debugfs")) {
    printf("ext2_test: SKIP (mke2fs/e2fsck/debugfs not found; install e2fsprogs)\n");
    return 0;
  }
  snprintf(tmpdir, sizeof tmpdir, "/tmp/cp-ext2-host-XXXXXX");
  if (!mkdtemp(tmpdir)) {
    printf("FAIL mkdtemp\n");
    return 1;
  }
  printf("ext2_test: tools %s %s %s, scratch %s\n", mke2fs_bin, e2fsck_bin,
         debugfs_bin, tmpdir);
  scenario_read(1024, "");
  scenario_read(2048, "");
  scenario_read(4096, "-O ^dir_index");
  scenario_write(1024, "", 64);
  scenario_write(2048, "-O ^dir_index", 16);
  scenario_write(4096, "-O ^dir_index", 512);
  scenario_write(4096, "", 8); /* default ext2 features, tiny cache: eviction */
  scenario_htree();
  scenario_setattr(1024, 128);
  scenario_setattr(4096, 256);
  scenario_longpath();
  scenario_large_1k();
  scenario_corrupt();
  {
    int d1 = 0, d4 = 0, dplain = 0;
    scenario_crash(1024, 1, 6, &d1);
    scenario_crash(4096, 1, 8, &d4);
    CHECK(d1 == 0 && d4 == 0, "ordered commit: no crash prefix is worse than a leak");
    scenario_crash(4096, 0, 8, &dplain);
    CHECK(dplain > 0, "plain-order commit shows unsafe prefixes (the check is sensitive)");
  }
  if (failures && getenv("EXT2_TEST_KEEP"))
    printf("ext2_test: scratch kept at %s\n", tmpdir);
  else
    run("rm -rf %s", tmpdir);
  printf("ext2_test: %d checks, %d failures\n", checks, failures);
  printf(failures ? "ext2_test: FAIL\n" : "ext2_test: PASS\n");
  return failures ? 1 : 0;
}
