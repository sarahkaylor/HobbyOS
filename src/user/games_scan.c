/* games_scan.c — the S2 game-launcher menu source (GB-039).
 *
 * Implements the /GAMES scan + menu ordering behind src/user/games_scan.h.
 * The classify/order core is host-testable; games_scan_dir() is the thin
 * guest read_dir wrapper.  The kernel loader (program_loader.c, GB-013)
 * already dispatches both native .BIN and 0x7fELF at spawn, so launching
 * either kind is `spawn2(name, ...)` — this module only owns what to show and
 * in what order.
 */
#include "games_scan.h"
#include "libc.h"

/* Bounded name copy (desktop.c keeps an identically-shaped static helper; ours
 * is translation-unit-local). */
static void copy_name(char *dst, const char *src, int max) {
  int i = 0;
  while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
  dst[i] = '\0';
}

/* Case-insensitive compare of the trailing bytes of `name` against `ext`
 * (ext has no dot, e.g. "bin"/"elf"/"exe").  Returns 1 on match. */
static int ends_with_ci(const char *name, const char *ext) {
  int n = 0;
  while (name[n]) n++;
  int e = 0;
  while (ext[e]) e++;
  if (n < e) return 0;
  for (int i = 0; i < e; i++) {
    char c = name[n - e + i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c != ext[i]) return 0;
  }
  return 1;
}

int games_classify(const char *name) {
  if (!name || !name[0]) return GAMES_NONE;
  if (ends_with_ci(name, "bin") || ends_with_ci(name, "exe"))
    return GAMES_BIN;
  /* ELF-compat binaries usually have no extension (or .elf); anything else
   * short is a candidate the loader will dispatch by magic. */
  if (ends_with_ci(name, "elf") || ends_with_ci(name, "com"))
    return GAMES_ELF;
  /* Known non-launchable data extensions -> OTHER, so menus don't offer them. */
  if (ends_with_ci(name, "wad") || ends_with_ci(name, "txt")
      || ends_with_ci(name, "cfg") || ends_with_ci(name, "log")
      || ends_with_ci(name, "md"))
    return GAMES_OTHER;
  return GAMES_ELF; /* extension-less executable is an ELF-compat candidate */
}

int games_order(struct games_entry *out,
                const struct games_entry *pinned, int npinned,
                const struct games_entry *scanned, int nscanned,
                int max) {
  int w = 0;
  /* tier 1: the known pinned native games (stable, always surface first). */
  for (int i = 0; i < npinned && w < max; i++) {
    if (pinned[i].kind == GAMES_BIN || pinned[i].kind == GAMES_ELF)
      out[w++] = pinned[i];
  }
  /* tier 2: the scanned entries, in read_dir order, until the cap. */
  for (int i = 0; i < nscanned && w < max; i++) {
    if (scanned[i].kind == GAMES_BIN || scanned[i].kind == GAMES_ELF)
      out[w++] = scanned[i];
  }
  return w;
}

int games_kind_str(int kind, char *buf, int n) {
  static const char *s[4] = {"---", "BIN", "ELF", "OTHER"};
  int k = (kind < 0 || kind > GAMES_OTHER) ? 0 : kind;
  int i = 0;
  while (s[k][i] && i < n - 1) { buf[i] = s[k][i]; i++; }
  buf[i] = '\0';
  return i;
}

int games_scan_dir(struct games_entry *out, int max) {
  int n = 0;
  for (int idx = 0; n < max; idx++) {
    struct sys_dirent ent;
    if (read_dir("/GAMES", idx, &ent) < 0) break;
    if (!ent.name[0]) continue;
    out[n].kind = games_classify(ent.name);
    if (out[n].kind != GAMES_BIN && out[n].kind != GAMES_ELF) continue;
    copy_name(out[n].name, ent.name, GAMES_NAME);
    n++;
  }
  return n;
}
