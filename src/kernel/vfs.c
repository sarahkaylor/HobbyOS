/*
 * vfs.c - Path routing between the FAT-16 boot volume and mounted NFS
 * exports.  See vfs.h for the model.
 *
 * The canonical path computation mirrors FAT16's clean_path() (which is
 * what the process cwd and fat16_absolute_path() already produce), so
 * dispatch decisions agree with the FAT driver's own resolution.
 */

#include "vfs.h"
#include "fs.h"
#include "fat16.h"
#include "process.h"

extern struct process *current_process(void);

void vfs_init(void) {
  nfs_init();
}

/* ==================================================================== */
/* path canonicalisation                                                */
/* ==================================================================== */

static void vfs_clean(const char *path, char *out, int cap) {
  char temp[256];
  int len = 0;
  while (path[len] && len < 255) {
    temp[len] = path[len];
    len++;
  }
  temp[len] = '\0';

  char *stack[16];
  int top = 0;
  char *p = temp;
  while (*p == '/') p++;
  while (*p) {
    char *comp = p;
    while (*p && *p != '/') p++;
    if (*p == '/') {
      *p = '\0';
      p++;
    }
    while (*p == '/') p++;
    if (comp[0] == '.' && comp[1] == '\0') continue;
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
      if (top > 0) top--;
      continue;
    }
    if (top < 16) stack[top++] = comp;
  }

  if (cap <= 0) return;
  int pos = 0;
  if (pos < cap - 1) out[pos++] = '/';
  for (int i = 0; i < top; i++) {
    if (i > 0 && pos < cap - 1) out[pos++] = '/';
    for (int j = 0; stack[i][j] && pos < cap - 1; j++)
      out[pos++] = stack[i][j];
  }
  out[pos] = '\0';
}

int vfs_abs_path(const char *path, char *out, int cap) {
  if (!path || cap <= 0) return -1;
  if (path[0] == '/') {
    vfs_clean(path, out, cap);
    return 0;
  }

  struct process *cur = current_process();
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
  char raw[256];
  int pos = 0;
  while (cwd[pos] && pos < 200) {
    raw[pos] = cwd[pos];
    pos++;
  }
  if (pos > 0 && raw[pos - 1] != '/') raw[pos++] = '/';
  int k = 0;
  while (path[k] && pos + k < 250) {
    raw[pos + k] = path[k];
    k++;
  }
  raw[pos + k] = '\0';
  vfs_clean(raw, out, cap);
  return 0;
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
  struct process *cur = current_process();
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

int vfs_chdir(const char *path, char *out_new_cwd, int cap) {
  if (!path) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;

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

  return fat16_chdir(path, out_new_cwd);
}

int vfs_read_dir(const char *path, int index, char *name, int ncap,
                 uint8_t *attr, uint32_t *size) {
  if (!path) return -1;

  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;

  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) {
    const struct nfs_mount *m = nfs_mount_at(midx);
    if (!m) return -1;
    return nfs_list_dir(m, rel, index, name, ncap, attr, size);
  }

  return fat16_read_dir(path, index, name, attr, size);
}

int vfs_unlink(const char *path) {
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -1;      /* NFS: read-only */
  return fat16_unlink(path);
}

int vfs_rename(const char *oldp, const char *newp) {
  if (!oldp || !newp) return -1;
  char abs_old[VFS_PATH_MAX];
  char abs_new[VFS_PATH_MAX];
  if (vfs_abs_path(oldp, abs_old, sizeof abs_old) != 0) return -1;
  if (vfs_abs_path(newp, abs_new, sizeof abs_new) != 0) return -1;

  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs_old, &midx, &rel)) return -1;   /* NFS: read-only */
  if (vfs_route(abs_new, &midx, &rel)) return -1;   /* and no cross-fs moves */
  return fat16_rename(oldp, newp);
}

int vfs_mkdir(const char *path) {
  char abs[VFS_PATH_MAX];
  if (vfs_abs_path(path, abs, sizeof abs) != 0) return -1;
  int midx = -1;
  const char *rel = 0;
  if (vfs_route(abs, &midx, &rel)) return -1;       /* NFS: read-only */
  return fat16_mkdir(path);
}

int vfs_cwd_writable(void) {
  struct process *cur = current_process();
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
  int midx = -1;
  const char *rel = 0;
  return vfs_route(cwd, &midx, &rel) ? 0 : 1;
}

int vfs_stats(uint64_t *total, uint64_t *free_bytes) {
  struct process *cur = current_process();
  const char *cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";

  int midx = -1;
  const char *rel = 0;
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

  f->type = FILE_TYPE_NFS;
  f->nfs.size = attr.size;
  f->nfs.cursor = 0;
  f->nfs.is_dir = (attr.type == NF3DIR);
  f->nfs.mount_idx = midx;
  return 1;
}
