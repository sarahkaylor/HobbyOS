/* src/host/tar_test.c — host-side test for the in-OS tar extractor.
 *
 * The extractor (src/user/tar.c) is included directly and driven with real
 * host file I/O, so this exercises the exact code that will run in-OS as
 * /TOOLS/TAR.BIN against two archive sources:
 *
 *   1. a SYNTHETIC archive built here from raw ustar bytes, covering the
 *      edge cases the payload may contain: directory entries, "./" roots,
 *      sizes that are and are not 512-multiples, a GNU 'L' long-name
 *      entry, a ustar prefix-split name, and the two-zero-block end marker;
 *   2. a REAL archive produced by the host's GNU tar (argv[1]) extracted
 *      and compared byte-for-byte against its source tree (argv[2]) —
 *      this pins the format contract between `tar --format=ustar` (the
 *      producer in tools/selfhost-src-tar.sh) and the reader.
 *
 * Negative cases: truncated archive and a corrupted header checksum must
 * both report failure (rc != 0); usage errors must report rc 2.
 *
 * Host build (Makefile tar_test_run): plain clang + glibc, no compat.c —
 * the extractor is a port-style program (main(argc, argv)) so it compiles
 * unmodified for both targets.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "ho_scratch.h"

#define main tar_main
#include "../user/tar.c"
#undef main

#define TB_TMP ho_scratch_dir()
#define TB_BUF (1 << 16)

static int tb_fail;
static int tb_checks;

static void tb_check(int ok, const char *what) {
  tb_checks++;
  if (ok) {
    printf("PASS: %s\n", what);
  } else {
    tb_fail++;
    printf("FAIL: %s\n", what);
  }
}

/* ---- tiny fs helpers ------------------------------------------------------ */

static int tb_write(const char *path, const void *buf, long n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }
  if (n > 0 && write(fd, buf, (int)n) != (int)n) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static long tb_read(const char *path, void *buf, long cap) {
  int fd = open(path, O_RDONLY);
  long n;
  if (fd < 0) {
    return -1;
  }
  n = read(fd, buf, (int)cap);
  close(fd);
  return n;
}

static int tb_same_file(const char *a, const char *b) {
  static unsigned char ba[TB_BUF];
  static unsigned char bb[TB_BUF];
  long na = tb_read(a, ba, sizeof(ba));
  long nb = tb_read(b, bb, sizeof(bb));
  return na >= 0 && na == nb && memcmp(ba, bb, (size_t)na) == 0;
}

static void tb_mkdir_p(const char *path) {
  char buf[512];
  size_t n = strlen(path);
  if (n >= sizeof(buf)) {
    return;
  }
  memcpy(buf, path, n + 1);
  for (size_t i = 1; i < n; i++) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      mkdir(buf, 0755);
      buf[i] = '/';
    }
  }
  mkdir(buf, 0755);
}

static void tb_rm_tree(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;
  if (!d) {
    return;
  }
  while ((e = readdir(d)) != NULL) {
    char sub[512];
    struct stat st;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
    if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
      tb_rm_tree(sub);
    } else {
      remove(sub);
    }
  }
  closedir(d);
  rmdir(path);
}

static int tb_is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- synthetic ustar archive builder -------------------------------------- */

static unsigned char tb_tar[TB_BUF];
static size_t tb_tlen;

static void tb_checksum(unsigned char *h) {
  long sum = 0;
  memset(h + 148, ' ', 8);
  for (int i = 0; i < 512; i++) {
    sum += h[i];
  }
  snprintf((char *)h + 148, 7, "%06lo", sum);
}

static void tb_header(const char *name, const char *prefix, char type, long size) {
  unsigned char *h = tb_tar + tb_tlen;
  memset(h, 0, 512);
  strncpy((char *)h, name, 100);
  if (prefix != NULL && prefix[0] != '\0') {
    strncpy((char *)h + 345, prefix, 155);
  }
  snprintf((char *)h + 100, 8, "%07o", 0644);
  snprintf((char *)h + 108, 8, "%07o", 0);
  snprintf((char *)h + 116, 8, "%07o", 0);
  snprintf((char *)h + 124, 12, "%011lo", size);
  snprintf((char *)h + 136, 12, "%011lo", 0L);
  h[156] = (unsigned char)type;
  memcpy(h + 257, "ustar", 5);
  h[262] = '\0';
  h[263] = '0';
  h[264] = '0';
  tb_checksum(h);
  tb_tlen += 512;
}

static void tb_data(const void *buf, long n) {
  long padded = (n + 511) & ~511L;
  memcpy(tb_tar + tb_tlen, buf, (size_t)n);
  memset(tb_tar + tb_tlen + n, 0, (size_t)(padded - n));
  tb_tlen += (size_t)padded;
}

/* ---- case 1: synthetic archive -------------------------------------------- */

static char tb_longname[256];   /* "./LONG-xxx….TXT", >100 chars */
static char tb_longrel[256];    /* the same without the "./" root */

static void tb_build_synthetic(void) {
  const char *greet = "hello from synthetic tar\n";
  static unsigned char big[1024];
  static unsigned char odd[777];

  for (int i = 0; i < 1024; i++) {
    big[i] = (unsigned char)('A' + (i % 26));
  }
  for (int i = 0; i < 777; i++) {
    odd[i] = (unsigned char)((i * 7) & 0xFF);
  }
  {
    char stem[160];
    memset(stem, 'x', 118);
    stem[118] = '\0';
    snprintf(tb_longname, sizeof(tb_longname), "./LongName-%s.TXT", stem);
  }
  snprintf(tb_longrel, sizeof(tb_longrel), "%s", tb_longname + 2);

  tb_tlen = 0;
  tb_header("./", NULL, '5', 0);
  tb_header("./SUB/", NULL, '5', 0);
  tb_header("./SUB/DEEPER/", NULL, '5', 0);
  tb_header("./GREET.TXT", NULL, '0', (long)strlen(greet));
  tb_data(greet, (long)strlen(greet));
  tb_header("./BIG.TXT", NULL, '0', 1024);
  tb_data(big, 1024);
  tb_header("./ODD.BIN", NULL, '0', 777);
  tb_data(odd, 777);
  /* GNU long name: 'L' entry carries the name, the next header the file. */
  tb_header("./L-ENTRY", NULL, 'L', (long)strlen(tb_longname) + 1);
  tb_data(tb_longname, (long)strlen(tb_longname) + 1);
  tb_header(tb_longname, NULL, '0', 14);
  tb_data("long content\n\0\0", 14);
  /* ustar prefix split: prefix "PFX/DIR" + name "PREFIX.TXT". */
  tb_header("PREFIX.TXT", "PFX/DIR", '0', 16);
  tb_data("prefix content\n\0", 16);
  /* end-of-archive marker: two zero blocks. */
  memset(tb_tar + tb_tlen, 0, 1024);
  tb_tlen += 1024;
}

static void tb_case_synthetic(void) {
  static unsigned char tb_rb[512];
  char synth[256];
  char out[256];
  char path[512];
  char *argv4[4];
  long rc;
  char expected[256];

  snprintf(synth, sizeof(synth), "%s/synthetic.tar", TB_TMP);
  snprintf(out, sizeof(out), "%s/out", TB_TMP);

  tb_build_synthetic();
  tb_check(tb_write(synth, tb_tar, (long)tb_tlen) == 0, "synthetic tar written");

  argv4[0] = (char *)"tar";
  argv4[1] = (char *)"xf";
  argv4[2] = synth;
  argv4[3] = out;
  rc = tar_main(4, argv4);
  tb_check(rc == 0, "extract synthetic archive rc=0");

  snprintf(expected, sizeof(expected), "%s/GREET.TXT", out);
  tb_check(tb_read(expected, tb_rb, sizeof(tb_rb)) > 0, "extracted GREET.TXT exists");
  snprintf(path, sizeof(path), "%s/expected_greet", TB_TMP);
  tb_write(path, "hello from synthetic tar\n", 25);
  tb_check(tb_same_file(expected, path), "GREET.TXT content byte-equal");

  snprintf(path, sizeof(path), "%s/expected_big", TB_TMP);
  {
    static unsigned char big[1024];
    for (int i = 0; i < 1024; i++) {
      big[i] = (unsigned char)('A' + (i % 26));
    }
    tb_write(path, big, 1024);
  }
  snprintf(expected, sizeof(expected), "%s/BIG.TXT", out);
  tb_check(tb_same_file(expected, path), "BIG.TXT (1024B, 512-multiple) byte-equal");

  snprintf(path, sizeof(path), "%s/expected_odd", TB_TMP);
  {
    static unsigned char odd[777];
    for (int i = 0; i < 777; i++) {
      odd[i] = (unsigned char)((i * 7) & 0xFF);
    }
    tb_write(path, odd, 777);
  }
  snprintf(expected, sizeof(expected), "%s/ODD.BIN", out);
  tb_check(tb_same_file(expected, path), "ODD.BIN (777B, not 512-multiple) byte-equal");

  snprintf(expected, sizeof(expected), "%s/%s", out, tb_longrel);
  tb_check(tb_read(expected, tb_rb, sizeof(tb_rb)) == 14, "GNU long-name entry extracted");

  snprintf(expected, sizeof(expected), "%s/PFX/DIR/PREFIX.TXT", out);
  tb_check(tb_read(expected, tb_rb, sizeof(tb_rb)) == 16, "ustar prefix path assembled");

  snprintf(expected, sizeof(expected), "%s/SUB/DEEPER", out);
  tb_check(tb_is_dir(expected), "explicit directory entries created");

  /* listing mode */
  argv4[1] = (char *)"tf";
  rc = tar_main(3, argv4);
  tb_check(rc == 0, "list synthetic archive rc=0");

  /* ---- negative: truncated archive (cut inside the last entry) ---- */
  snprintf(path, sizeof(path), "%s/trunc.tar", TB_TMP);
  {
    char out_trunc[256];
    long cut = (long)tb_tlen - 1024 - 100; /* inside PREFIX content/padding */
    tb_check(tb_write(path, tb_tar, cut) == 0, "truncated tar written");
    snprintf(out_trunc, sizeof(out_trunc), "%s/out_trunc", TB_TMP);
    argv4[0] = (char *)"tar";
    argv4[1] = (char *)"xf";
    argv4[2] = path;
    argv4[3] = out_trunc; /* a real dir — the failure must be the archive */
    rc = tar_main(4, argv4);
    tb_check(rc != 0, "truncated archive reports failure");
  }

  /* ---- negative: corrupted header checksum ---- */
  snprintf(path, sizeof(path), "%s/corrupt.tar", TB_TMP);
  {
    static unsigned char bad[TB_BUF];
    memcpy(bad, tb_tar, tb_tlen);
    bad[0] ^= 0xFF; /* first header name byte: checksum now mismatches */
    tb_check(tb_write(path, bad, (long)tb_tlen) == 0, "corrupt tar written");
    argv4[2] = path;
    rc = tar_main(4, argv4);
    tb_check(rc != 0, "corrupt checksum reports failure");
  }

  /* ---- negative: usage ---- */
  {
    char *arg1[1];
    arg1[0] = (char *)"tar";
    rc = tar_main(1, arg1);
    tb_check(rc == 2, "usage error rc=2");
  }
}

/* ---- case 2: real GNU-tar archive vs its source tree ---------------------- */

static int tb_verify_tree(const char *expected_root, const char *actual_root, int *files) {
  DIR *d = opendir(expected_root);
  struct dirent *e;
  if (!d) {
    return -1;
  }
  while ((e = readdir(d)) != NULL) {
    char ep[512];
    char ap[512];
    struct stat st;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
      continue;
    }
    snprintf(ep, sizeof(ep), "%s/%s", expected_root, e->d_name);
    snprintf(ap, sizeof(ap), "%s/%s", actual_root, e->d_name);
    if (stat(ep, &st) != 0) {
      closedir(d);
      return -1;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!tb_is_dir(ap)) {
        closedir(d);
        return -1;
      }
      if (tb_verify_tree(ep, ap, files) != 0) {
        closedir(d);
        return -1;
      }
    } else {
      if (!tb_same_file(ep, ap)) {
        printf("FAIL: tree mismatch at %s\n", ap);
        closedir(d);
        return -1;
      }
      (*files)++;
    }
  }
  closedir(d);
  return 0;
}

int main(int argc, char **argv) {
  char out[256];
  char *argv4[4];
  long rc;

  tb_rm_tree(TB_TMP);
  tb_mkdir_p(TB_TMP);

  tb_case_synthetic();

  if (argc >= 3) {
    /* argv[1] = real tar archive, argv[2] = its source tree (repo-relative) */
    int files = 0;
    snprintf(out, sizeof(out), "%s/real_out", TB_TMP);
    argv4[0] = (char *)"tar";
    argv4[1] = (char *)"xf";
    argv4[2] = argv[1];
    argv4[3] = out;
    rc = tar_main(4, argv4);
    tb_check(rc == 0, "extract real GNU-tar archive rc=0");
    tb_check(tb_verify_tree(argv[2], out, &files) == 0,
             "real archive tree byte-identical to source");
    tb_check(files >= 6, "real archive carried all fixture files");
    printf("INFO: real-archive files verified: %d\n", files);
  } else {
    printf("INFO: no real-archive argv — synthetic cases only\n");
  }

  if (tb_fail == 0) {
    tb_rm_tree(TB_TMP);
    printf("TAR_TEST OK (%d checks)\n", tb_checks);
    return 0;
  }
  printf("TAR_TEST FAILED (%d/%d checks failed; scratch kept at %s)\n",
         tb_fail, tb_checks, TB_TMP);
  return 1;
}
