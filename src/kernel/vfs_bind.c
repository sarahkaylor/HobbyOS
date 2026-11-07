/*
 * vfs_bind.c - prefix aliases in the VFS namespace.  See vfs_bind.h for the
 * contract and docs/rimworld-m5-managed.md for why the game needs one.
 *
 * Freestanding by construction: no libc, no allocation, no locks.  The same
 * translation unit is linked into the kernel and compiled into the host unit
 * test (src/host/vfs_bind_test.c), so a rule that only holds in one of them
 * cannot exist.
 */

#include "vfs_bind.h"

struct vfs_bind_entry {
  char from[VFS_BIND_PATH_MAX];
  char to[VFS_BIND_PATH_MAX];
};

static struct vfs_bind_entry binds[VFS_BIND_MAX];
static int bind_count;

void vfs_bind_reset(void) {
  bind_count = 0;
  for (int i = 0; i < VFS_BIND_MAX; i++) {
    binds[i].from[0] = '\0';
    binds[i].to[0] = '\0';
  }
}

static int b_len(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static int b_copy(char *dst, int cap, const char *src, int n) {
  if (n >= cap) return -1;
  for (int i = 0; i < n; i++) dst[i] = src[i];
  dst[n] = '\0';
  return 0;
}

/* Is `path` inside (or equal to) the absolute, canonical prefix `pre`?
 * Component-boundary aware, so "/A/B" does not claim "/A/BC". */
static int inside(const char *pre, const char *path) {
  int i = 0;
  while (pre[i]) {
    if (path[i] != pre[i]) return 0;
    i++;
  }
  return path[i] == '\0' || path[i] == '/';
}

/* Accept only a canonical absolute path: leading '/', no trailing '/', no
 * empty component, no "." / "..".  The VFS's own canonicaliser collapses
 * those before translation is consulted, so a bind registered with them
 * could never match anyway.  Returns the length, or -1. */
static int canonical_len(const char *p) {
  if (!p || p[0] != '/') return -1;
  int n = b_len(p);
  if (n <= 1 || n >= VFS_BIND_PATH_MAX) return -1;
  if (p[n - 1] == '/') return -1;
  for (int i = 0; i < n; i++) {
    if (p[i] != '/' || p[i + 1] != '/') continue;
    return -1; /* "//" */
  }
  int start = 1;
  while (start < n) {
    int end = start;
    while (end < n && p[end] != '/') end++;
    int clen = end - start;
    if (clen == 1 && p[start] == '.') return -1;
    if (clen == 2 && p[start] == '.' && p[start + 1] == '.') return -1;
    start = end + 1;
  }
  return n;
}

/* Refuse a bind whose endpoints would let one translation feed another.
 *
 * The requirement is idempotence: translating an already-translated path must
 * be the identity.  That holds exactly when no `to` lies inside any `from`
 * (including its own).  A `from` nested in another `from` is fine (longest
 * prefix picks one), and so is a `to` nested in another `to` (neither can
 * re-match).  What must not exist is a `to` inside a `from`, in either
 * direction of registration -- those are the chains that would drift on the
 * second resolution. */
static int chains(const char *from, const char *to) {
  if (inside(from, to) || inside(to, from)) return 1;
  for (int i = 0; i < bind_count; i++) {
    const struct vfs_bind_entry *e = &binds[i];
    if (inside(e->from, to) || inside(to, e->from)) return 1;
    if (inside(e->to, from) || inside(from, e->to)) return 1;
  }
  return 0;
}

int vfs_bind_add(const char *from, const char *to) {
  int fl = canonical_len(from);
  int tl = canonical_len(to);
  if (fl < 0 || tl < 0) return -1;
  if (chains(from, to)) return -2;
  if (bind_count >= VFS_BIND_MAX) return -3;

  struct vfs_bind_entry *e = &binds[bind_count];
  if (b_copy(e->from, sizeof e->from, from, fl) != 0) return -1;
  if (b_copy(e->to, sizeof e->to, to, tl) != 0) return -1;
  bind_count++;
  return 0;
}

int vfs_bind_count(void) { return bind_count; }

int vfs_bind_info(int idx, char *from, int fcap, char *to, int tcap) {
  if (idx < 0 || idx >= bind_count) return -1;
  const struct vfs_bind_entry *e = &binds[idx];
  if (from && b_copy(from, fcap, e->from, b_len(e->from)) != 0) return -1;
  if (to && b_copy(to, tcap, e->to, b_len(e->to)) != 0) return -1;
  return 0;
}

int vfs_bind_translate(const char *abs, char *out, int cap) {
  if (!abs || !out || cap <= 0 || bind_count == 0) return 0;

  int best = -1, best_len = 0;
  for (int i = 0; i < bind_count; i++) {
    if (!inside(binds[i].from, abs)) continue;
    int n = b_len(binds[i].from);
    if (n > best_len) {
      best_len = n;
      best = i;
    }
  }
  if (best < 0) return 0;

  const struct vfs_bind_entry *e = &binds[best];
  int tl = b_len(e->to);
  int rest = b_len(abs) - best_len;     /* 0 or starts with '/' */
  if (tl + rest >= cap) return -1;
  for (int i = 0; i < tl; i++) out[i] = e->to[i];
  for (int i = 0; i < rest; i++) out[tl + i] = abs[best_len + i];
  out[tl + rest] = '\0';
  return 1;
}
