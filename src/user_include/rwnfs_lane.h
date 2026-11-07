/*
 * rwnfs_lane.h — which NFS export does THIS run own? (M6/L63)
 *
 * The RimWorld harness used to serve every lane from one host directory,
 * /srv/nfs/rimworld.  Two lanes booting at once therefore raced on the same
 * files: each harness installs its own AD-9 launcher as
 * <export>/RimWorldLinux, so a run could boot with the OTHER lane's binary
 * (measured: a run started with the other lane's 96,432-byte build, i.e.
 * without the switch it was testing).
 *
 * The fix is one export directory per worktree (/srv/nfs/rimworld-<lane>),
 * and the guest has to be told which one.  The harness stages /NFSEXPORT
 * with that path; rwlaunch and the probes read it here.  Absent the marker
 * (an old image, the boot-wave loaders, a unit build) the compiled-in
 * default stands, so nothing depends on the new file existing.
 */
#ifndef RWNFS_LANE_H
#define RWNFS_LANE_H

#include <fcntl.h>

/* Returns `dflt` unless /NFSEXPORT holds an absolute path. */
static char rw_nfs_export_buf[104] __attribute__((unused));

static const char *rw_nfs_export_path(const char *dflt) __attribute__((unused));
static const char *rw_nfs_export_path(const char *dflt) {
  int fd = open("/NFSEXPORT", O_RDONLY);
  if (fd >= 0) {
    int n = (int)read(fd, rw_nfs_export_buf, (int)sizeof rw_nfs_export_buf - 1);
    close(fd);
    if (n > 0) {
      int i = 0, j = 0;
      while (i < n && (rw_nfs_export_buf[i] == ' ' || rw_nfs_export_buf[i] == '\t'))
        i++;
      if (rw_nfs_export_buf[i] == '/') {
        while (i < n && j < (int)sizeof rw_nfs_export_buf - 1) {
          char c = rw_nfs_export_buf[i];
          if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            break;
          rw_nfs_export_buf[j++] = c;
          i++;
        }
        rw_nfs_export_buf[j] = 0;
        if (j > 1)
          return rw_nfs_export_buf;
      }
    }
  }
  return dflt;
}

#endif /* RWNFS_LANE_H */
