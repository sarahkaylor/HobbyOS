#!/usr/bin/env bash
# tools/rw/lane.sh — the NFS export identity of THIS worktree (M6/L63).
#
# Source this; it sets and exports:
#   RW_LANE    a filesystem-safe name for the worktree (default: its dirname)
#   RW_EXPORT  /srv/nfs/rimworld-<lane>   (the directory this lane owns)
#
# Why: every lane used to be served from /srv/nfs/rimworld, and each
# harness installs its own AD-9 launcher as <export>/RimWorldLinux.  Two
# lanes booting at once therefore raced on that one path -- a run could
# start with the other lane's binary (measured: a 96,432-byte build with a
# switch this lane was not testing).  One export directory per worktree
# makes the same race impossible: the guest mounts its own directory (see
# /NFSEXPORT and src/user_include/rwnfs_lane.h) and nothing else writes
# into it.
#
# Usage:
#   . "$ROOT/tools/rw/lane.sh"
#   bash tools/rw/nfs-export.sh sync      # uses $RW_EXPORT
#
# RW_EXPORT is an OUTPUT of this file, derived from RW_LANE, so changing the
# lane can never be defeated by a stale RW_EXPORT left in the environment.
# For a directory that is not /srv/nfs/rimworld-<lane>, set
# RW_EXPORT_OVERRIDE.  A deliberately shared export is still possible inside
# nfs-export.sh (RW_ALLOW_SHARED_EXPORT=1), but it must be asked for.

_rw_lane_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RW_LANE="${RW_LANE:-$(basename "$_rw_lane_root")}"
# Keep the name path-safe: the export path is also an NFS export string and
# a marker file the guest parses.
case "$RW_LANE" in
  ''|*[!A-Za-z0-9._-]*) RW_LANE="$(printf '%s' "${RW_LANE:-lane}" | tr -c 'A-Za-z0-9._-' '-')" ;;
esac
RW_EXPORT="${RW_EXPORT_OVERRIDE:-/srv/nfs/rimworld-$RW_LANE}"
export RW_LANE RW_EXPORT
