/* src/user/linux_probe_wad.c — read the IWAD directory two ways.
 *
 * WHY: in the guest, upstream Doom loads /DOOM1.WAD, prints the Shareware banner,
 * then dies with "W_GetNumForName: PNAMES not found!" — while the SAME binary on
 * a Linux host walks the same file fine.  So the question is not Doom, it is
 * whether a compat process can seek/read a 4 MB file at a ~4 MB offset through
 * both the raw syscalls AND musl's buffered stdio (Doom uses fopen/fseek/fread).
 *
 * What it prints (rows prefixed "[LP] ", ends with "[LP] done rc=<n>"):
 *   raw:   open/lseek/read of the 12-byte header, then seek->infotableofs,
 *          read the numlumps*16-byte directory, report how many names match.
 *   stdio: the identical walk with fopen/fseek/fread/ftell.
 * Both must agree with the directory the host reads, so a difference names the
 * broken layer.  Run it on a Linux host too:
 *     ./probe_wad /path/to/doom1.wad
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void row(const char *fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
  if (n > 0) (void)!write(1, b, (size_t)n);
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Lump names are 8 bytes, NOT NUL-terminated; print with the padding trimmed. */
static void lump_name(const unsigned char *p, char out[9]) {
  memcpy(out, p, 8);
  out[8] = 0;
  for (int i = 7; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

static int find_lump(const unsigned char *dir, uint32_t n, const char *want,
                     int *index_out) {
  for (uint32_t i = 0; i < n; i++) {
    char nm[9];
    lump_name(dir + (size_t)i * 16 + 8, nm);
    if (strcmp(nm, want) == 0) {
      if (index_out) *index_out = (int)i;
      return 1;
    }
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/DOOM1.WAD";

  /* ---- raw syscalls ---- */
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    row("[LP] wad raw open %s FAILED errno=%d\n", path, errno);
    row("[LP] done rc=1\n");
    return 1;
  }
  unsigned char hdr[12];
  if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) {
    row("[LP] wad raw header read FAILED errno=%d\n", errno);
    row("[LP] done rc=1\n");
    return 1;
  }
  uint32_t numlumps = le32(hdr + 4);
  uint32_t infotableofs = le32(hdr + 8);
  char id[5];
  memcpy(id, hdr, 4);
  id[4] = 0;
  row("[LP] wad raw id=%s lumps=%u infotableofs=%u\n", id, numlumps,
      infotableofs);
  if (strcmp(id, "IWAD") != 0 && strcmp(id, "PWAD") != 0) fails++;
  if (numlumps == 0 || numlumps > 20000) fails++;

  long pos = lseek(fd, 0, SEEK_END);
  row("[LP] wad raw size=%ld %s\n", pos, pos > 1000000 ? "ok" : "SHORT");
  if (pos <= 1000000) fails++;
  long back = lseek(fd, (long)infotableofs, SEEK_SET);
  if (back != (long)infotableofs) {
    row("[LP] wad raw seek FAILED got=%ld want=%u errno=%d\n", back,
        infotableofs, errno);
    fails++;
  }

  static unsigned char dir[20000 * 16];
  size_t dirlen = (size_t)numlumps * 16;
  if (dirlen > sizeof dir) dirlen = sizeof dir;
  size_t got = 0;
  while (got < dirlen) {
    ssize_t n = read(fd, dir + got, dirlen - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  row("[LP] wad raw dir bytes=%lu/%lu %s\n", (unsigned long)got,
      (unsigned long)dirlen, got == dirlen ? "ok" : "SHORT");
  if (got != dirlen) fails++;

  char n0[9], n1[9], n2[9];
  if (dirlen >= 48) {
    lump_name(dir + 8, n0);
    lump_name(dir + 24, n1);
    lump_name(dir + 40, n2);
    row("[LP] wad raw first3=%s,%s,%s\n", n0, n1, n2);
    if (!n0[0] || !n1[0]) fails++;  /* garbage names print as empty/short */
  }
  int pn = -1;
  int have_pnames = find_lump(dir, numlumps, "PNAMES", &pn);
  int have_texture1 = find_lump(dir, numlumps, "TEXTURE1", 0);
  row("[LP] wad raw PNAMES=%d(idx %d) TEXTURE1=%d %s\n", have_pnames, pn,
      have_texture1, (have_pnames && have_texture1) ? "ok" : "MISSING");
  if (!have_pnames || !have_texture1) fails++;
  close(fd);

  /* ---- musl stdio, the path Doom actually uses ---- */
  FILE *f = fopen(path, "rb");
  if (!f) {
    row("[LP] wad stdio fopen FAILED errno=%d\n", errno);
    row("[LP] done rc=%d\n", fails + 1);
    return fails + 1;
  }
  unsigned char h2[12];
  size_t r = fread(h2, 1, sizeof h2, f);
  uint32_t n2l = le32(h2 + 4), it2 = le32(h2 + 8);
  row("[LP] wad stdio header=%lu lumps=%u ofs=%u %s\n", (unsigned long)r, n2l,
      it2, (r == 12 && n2l == numlumps && it2 == infotableofs) ? "ok" : "MISMATCH");
  if (r != 12 || n2l != numlumps || it2 != infotableofs) fails++;

  if (fseek(f, 0, SEEK_END) != 0) {
    row("[LP] wad stdio fseek END FAILED errno=%d\n", errno);
    fails++;
  }
  long sz = ftell(f);
  if (fseek(f, (long)infotableofs, SEEK_SET) != 0) {
    row("[LP] wad stdio fseek dir FAILED errno=%d\n", errno);
    fails++;
  }
  size_t got2 = fread(dir, 1, dirlen, f);
  row("[LP] wad stdio size=%ld dir=%lu/%lu %s\n", sz, (unsigned long)got2,
      (unsigned long)dirlen, (sz == pos && got2 == dirlen) ? "ok" : "MISMATCH");
  if (sz != pos || got2 != dirlen) fails++;
  {
    char m0[9];
    lump_name(dir + 8, m0);
    int pn2 = -1;
    int hp = find_lump(dir, n2l, "PNAMES", &pn2);
    row("[LP] wad stdio first=%s PNAMES=%d(idx %d) %s\n", m0, hp, pn2,
        (hp && strcmp(m0, n0) == 0) ? "ok" : "MISMATCH");
    if (!hp || strcmp(m0, n0) != 0) fails++;
  }
  fclose(f);

  row("[LP] done rc=%d\n", fails);
  return fails;
}
