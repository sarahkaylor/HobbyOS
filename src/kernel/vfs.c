/*
 * vfs.c - Path routing between the FAT-16 boot volume and mounted NFS
 * exports.  See vfs.h for the model.
 *
 * The canonical path computation mirrors FAT16's clean_path() (which is
 * what the process cwd and fat16_absolute_path() already produce), so
 * dispatch decisions agree with the FAT driver's own resolution.
 */

#include "vfs.h"
#include "vfs_bind.h"
#include "fs.h"
#include "fat16.h"
#include "process.h"
#include "kline.h"
#include "errno.h"
#include "ext2_vfs.h"

extern struct process *current_process(void);

void vfs_init(void) {
  nfs_init();
  /* cp/ext2: mount the optional data disk at /EXT (a no-op without one). */
  ext2v_init();
}

/* ==================================================================== */
/* path canonicalisation                                                */
/* ==================================================================== */

/* Canonicalise `path` ("//", "." and ".." collapsed, ".." at the root stays
 * at the root, no trailing '/') into out[cap].  Same result FAT16's
 * clean_path() gives for the paths it can hold.
 *
 * cp/ext2b: built in place, with no intermediate copy and no component cap.
 * The old version copied at most 255 bytes and kept at most 64 components and
 * then truncated the OUTPUT at cap -- every limit silently produced a
 * different, shorter path (a deep Wine prefix path named its own ancestor).
 * Now a result that does not fit `cap` is an error (-1), never a prefix. */
static int vfs_clean(const char *path, char *out, int cap) {
  /* starts[i] = output length before component i (the ".." pop target).  A
     component costs at least two output bytes ("/x"), so cap/2 slots can
     never overflow before `out` itself does. */
  uint16_t starts[VFS_PATH_MAX / 2];
  int top = 0;
  int pos = 1;
  if (cap < 2) return -1;
  if (cap > VFS_PATH_MAX) cap = VFS_PATH_MAX;
  out[0] = '/';
  const char *p = path;
  for (;;) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *c = p;
    while (*p && *p != '/') p++;
    int n = (int)(p - c);
    if (n == 1 && c[0] == '.') continue;
    if (n == 2 && c[0] == '.' && c[1] == '.') {
      if (top > 0) pos = starts[--top];
      continue;
    }
    if (top >= (int)(sizeof starts / sizeof starts[0])) return -1;
    starts[top++] = (uint16_t)pos;
    if (pos > 1) {
      if (pos >= cap - 1) return -1;
      out[pos++] = '/';
    }
    if (n >= cap - pos) return -1; /* the name plus the terminator */
    for (int i = 0; i < n; i++) out[pos++] = c[i];
  }
  out[pos] = '\0';
  return 0;
}

/* M5c: a bind may redirect this canonical path to another filesystem.  Applied
 * here, at the single point every path-based operation funnels through, so
 * open/stat/readdir/chdir all agree on what a bound path names.  A translation
 * that does not fit is refused (-1) rather than truncated: a truncated path
 * would name a different file. */
static int vfs_bind_apply(char *path, int cap) {
  char bound[VFS_PATH_MAX];
  int t = vfs_bind_translate(path, bound, sizeof bound);
  if (t <= 0) return t == 0 ? 0 : -1;

  int n = 0;
  while (bound[n]) n++;
  if (n >= cap) return -1;
  for (int i = 0; i <= n; i++) path[i] = bound[i];
  return 0;
}

int vfs_abs_path(const char *path, char *out, int cap) {
  if (!path || cap <= 0) return -1;
  if (path[0] == '/') {
    if (vfs_clean(path, out, cap) != 0) return -1;
    return vfs_bind_apply(out, cap);
  }

  /* cp/ext2b: cwd + '/' + path, refused (not cut) when it does not fit.  The
     old join stopped copying the cwd at 200 bytes and the path at 250, so a
     long relative name silently became a different file.  The raw join may be
     longer than the canonical result ("a/../b"), hence the doubled buffer. */
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
  char raw[2 * VFS_PATH_MAX];
  int pos = 0;
  while (cwd[pos]) {
    if (pos >= (int)sizeof raw - 2) return -1;
    raw[pos] = cwd[pos];
    pos++;
  }
  if (pos > 0 && raw[pos - 1] != '/') raw[pos++] = '/';
  for (int k = 0; path[k]; k++) {
    if (pos >= (int)sizeof raw - 1) return -1;
    raw[pos++] = path[k];
  }
  raw[pos] = '\0';
  if (vfs_clean(raw, out, cap) != 0) return -1;
  return vfs_bind_apply(out, cap);
}

int vfs_route(const char *abs, int *mount_idx, const char **rel) {
  return nfs_route(abs, mount_idx, rel);
}

/* ==================================================================== */
/* mounts                                                               */
/* ==================================================================== */

int vfs_mount(const char *source, const char *target) {
  if (!source || !target) return -1;
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(target, abs, sizeof abs) != 0) return -1;

  /* Validate before touching the FAT tree: a target inside an existing
   * mount (or an existing mount point itself) is refused, and a mount
   * that is going to fail must not leave a freshly created directory
   * behind. */
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -1;
  if (abs[1] == '\0') return -1;               /* never mount over "/" */
  if (ext2v_owns(abs)) return -1;              /* cp/ext2: /EXT is taken */

  /* Make the mount point visible in FAT listings: create it if needed
   * (an already existing directory is fine). */
  (void)fat16_mkdir(abs);

  return nfs_mount_add(source, abs);
}

int vfs_umount(const char *target) {
  if (!target) return -1;
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(target, abs, sizeof abs) != 0) return -1;

  int rc = nfs_mount_remove(abs);
  if (rc != 0) return -1;

  /* A process whose cwd lived inside the unmounted tree falls back to the
   * root: its old cwd no longer has a filesystem behind it. */
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  if (cur && cur->cwd[0] == '/') {
    const char *cwd = cur->cwd;
    int i = 0;
    while (abs[i] && cwd[i] && cwd[i] == abs[i]) i++;
    if (abs[i] == '\0' && (cwd[i] == '\0' || cwd[i] == '/'))
      cur->cwd[0] = '/', cur->cwd[1] = '\0';
  }
  return 0;
}

int vfs_mount_count(void) {
  return nfs_mount_count();
}

int vfs_mount_info(int idx, struct vfs_mountinfo *out) {
  if (!out) return -1;
  char point[NFS_PATH_MAX];
  char source[NFS_SRC_MAX];
  int type = 0;
  if (nfs_mount_info(idx, point, sizeof point, source, sizeof source, &type) != 0)
    return -1;
  int i = 0;
  while (point[i] && i < (int)sizeof out->point - 1) {
    out->point[i] = point[i];
    i++;
  }
  out->point[i] = '\0';
  i = 0;
  while (source[i] && i < (int)sizeof out->source - 1) {
    out->source[i] = source[i];
    i++;
  }
  out->source[i] = '\0';
  out->type = type;
  return 0;
}

/* ==================================================================== */
/* path operations                                                      */
/* ==================================================================== */

/* Every operation below resolves through vfs_abs_path() first and hands the
 * CANONICAL path to FAT16, not the caller's original spelling: since M5c a
 * bind can move a path onto the FAT volume, so the caller's string is no
 * longer necessarily something FAT16 can resolve.  With no bind registered the
 * canonical path is exactly what fat16_absolute_path() would have produced. */

int vfs_chdir(const char *path, char *out_new_cwd, int cap) {
  if (!path) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;

  if (ext2v_owns(abs)) { /* cp/ext2 */
    int r = ext2v_chdir(abs);
    if (r == 0) {
      int i = 0;
      while (abs[i] && i < cap - 1) {
        out_new_cwd[i] = abs[i];
        i++;
      }
      out_new_cwd[i] = '\0';
      return abs[i] ? -1 : 0; /* a cwd that does not fit is refused */
    }
    if (r != EXT2_EXTERNAL) return -1;
  }

  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) {
    const struct nfs_mount *m = nfs_mount_at(midx);
    if (!m) return -1;
    if (nfs_is_dir(m, rel) != 1) return -1;
    int i = 0;
    while (abs[i] && i < cap - 1) {
      out_new_cwd[i] = abs[i];
      i++;
    }
    out_new_cwd[i] = '\0';
    return 0;
  }

  return fat16_chdir(abs, out_new_cwd);
}

int vfs_read_dir(const char *path, int index, char *name, int ncap,
                 uint8_t *attr, uint32_t *size) {
  if (!path) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;

  if (ext2v_owns(abs)) { /* cp/ext2 */
    int r = ext2v_read_dir(abs, index, name, ncap, attr, size);
    if (r != EXT2_EXTERNAL) return r < 0 ? -1 : r;
  }

  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) {
    const struct nfs_mount *m = nfs_mount_at(midx);
    if (!m) return -1;
    return nfs_list_dir(m, rel, index, name, ncap, attr, size);
  }

  return fat16_read_dir(abs, index, name, attr, size);
}

int vfs_unlink(const char *path) {
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;
  if (ext2v_owns(abs)) { /* cp/ext2 */
    int r = ext2v_unlink(abs);
    if (r != EXT2_EXTERNAL) return r;
  }
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -1;      /* NFS: read-only */
  return fat16_unlink(abs);
}

int vfs_rename(const char *oldp, const char *newp) {
  if (!oldp || !newp) return -1;
  char abs_old[VFS_PATH_MAX];
  char abs_new[VFS_PATH_MAX];
  if (vfs_abs_path(oldp, abs_old, sizeof abs_old) != 0) return -1;
  if (vfs_abs_path(newp, abs_new, sizeof abs_new) != 0) return -1;
  /* cp/ext2: both ends on ext2, or -EXDEV (no cross-volume moves). */
  if (ext2v_owns(abs_old) || ext2v_owns(abs_new)) {
    if (!ext2v_owns(abs_old) || !ext2v_owns(abs_new)) return -EXDEV;
    return ext2v_rename(abs_old, abs_new, 0);
  }

  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs_old, &midx, &rel)) return -1;   /* NFS: read-only */
  if (vfs_route(abs_new, &midx, &rel)) return -1;   /* and no cross-fs moves */
  return fat16_rename(abs_old, abs_new);
}

int vfs_mkdir(const char *path) {
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;
  if (ext2v_owns(abs)) { /* cp/ext2 */
    int r = ext2v_mkdir(abs, 0777);
    if (r != EXT2_EXTERNAL) return r;
  }
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -1;       /* NFS: read-only */
  return fat16_mkdir(abs);
}

/* W2-14 (selfhost): remove an EMPTY directory.  fat16_rmdir returns the
 * precise negative errno (ENOENT/ENOTDIR/ENOTEMPTY/EBUSY); a routed
 * (read-only NFS) mount refuses with -EROFS instead of the -1 the other
 * vfs_* helpers use. */
int vfs_rmdir(const char *path) {
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -ENOENT;
  if (ext2v_owns(abs)) { /* cp/ext2 */
    int r = ext2v_rmdir(abs);
    if (r != EXT2_EXTERNAL) return r;
  }
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -EROFS;   /* NFS: read-only */
  return fat16_rmdir(abs);
}

int vfs_cwd_writable(void) {
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
  int midx = -1;
  const char *rel = 0;
  if (ext2v_owns(cwd)) return !ext2v_readonly(); /* cp/ext2 */
  return vfs_route(cwd, &midx, &rel) ? 0 : 1;
}

int vfs_stats(uint64_t *total, uint64_t *free_bytes) {
  struct process *cur = process_group(current_process()); /* P1 (D7) */
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";

  int midx = -1;
  const char *rel = 0;
  if (ext2v_owns(cwd)) { /* cp/ext2 */
    struct ext2_statfs sf;
    if (ext2v_statfs(&sf) != 0) return -1;
    *total = sf.blocks * sf.bsize;
    *free_bytes = sf.bavail * sf.bsize;
    return 0;
  }
  if (vfs_route(cwd, &midx, &rel)) {
    const struct nfs_mount *m = nfs_mount_at(midx);
    if (!m) return -1;
    return nfs_statfs(m, rel, total, free_bytes);
  }
  return fat16_stats(total, free_bytes);
}

int vfs_open_routed(const char *path, struct file *f) {
  if (!path || !f) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;

  int midx = -1;
  const char *rel = 0;
  if (!vfs_route(abs, &midx, &rel)) return 0;       /* FAT16 handles it */

  const struct nfs_mount *m = nfs_mount_at(midx);
  if (!m) return -1;

  struct nfs_attr attr;
  if (nfs_lookup(m, rel, &f->nfs.fh, &attr) != 0) return -1;

  /* L58: the def-file-set question needs the game's own open list.  One row
   * per successful XML open under an NFS mount, capped so a runaway caller
   * cannot bury the ladder.  `[XO] CAPPED` says the list is incomplete. */
  {
    int rl = 0;
    while (rel[rl]) rl++;
    if (rl > 4 && rel[rl - 4] == '.' &&
        (rel[rl - 3] == 'x' || rel[rl - 3] == 'X') &&
        (rel[rl - 2] == 'm' || rel[rl - 2] == 'M') &&
        (rel[rl - 1] == 'l' || rel[rl - 1] == 'L')) {
      static int xo_rows;
      if (xo_rows < 2400) {
        xo_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[XO] ");
        kl_s(&kl, rel);
        kl_c(&kl, '\n');
        kl_emit(&kl);
      } else if (xo_rows == 2400) {
        xo_rows++;
        struct kline kl;
        kl_init(&kl);
        kl_s(&kl, "[XO] CAPPED at 2400\n");
        kl_emit(&kl);
      }
    }
  }

  f->type = FILE_TYPE_NFS;
  f->nfs.size = attr.size;
  f->nfs.cursor = 0;
  f->nfs.is_dir = (attr.type == NF3DIR);
  f->nfs.mount_idx = midx;
  return 1;
}
