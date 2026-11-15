#!/usr/bin/env bash
# tools/cp/mkcpdata.sh [IMAGE] [SIZE] — create the writable ext2 data disk
# the x86_64 guest mounts at /EXT (cp/ext2, docs/cp-ext2.md).
#
#   IMAGE  default ./cpdata.img (the Makefile's CPDATA default: `make run_intel`
#          and every x86_64 tier attach it as NVMe namespace 2 when it exists)
#   SIZE   default 8G (sparse: costs only what is written; any `truncate -s` size)
#
# The mke2fs flags are the driver's contract (src/include/ext2.h):
#   -t ext2          no journal, no extents, no 64bit, no flex_bg (INCOMPAT
#                    bits the driver would refuse; only FILETYPE is accepted)
#   -b 4096          4 KiB blocks (1 and 2 KiB work too; 4 KiB is the tested
#                    production geometry: fewest indirect levels, files > 4 GiB)
#   -I 256           256-byte inodes (mke2fs's default; the driver writes the
#                    128-byte base and leaves the rest zero)
#   -O ^dir_index    no hash-tree directories are ever created.  (The driver
#                    also copes with dir_index on: it reads indexed dirs
#                    linearly and clears EXT2_INDEX_FL on any dir it changes.)
#   -m 0             no root-reserved blocks (every guest process is uid 0)
#   -L cpdata        label
#   -E root_owner=0:0
# Result features: ext_attr resize_inode filetype sparse_super large_file —
# all mounted read-write.  Refuses to overwrite an existing image unless
# CPDATA_FORCE=1.
set -euo pipefail
IMG="${1:-cpdata.img}"
SIZE="${2:-8G}"
PATH="/usr/sbin:/sbin:$PATH"
command -v mke2fs >/dev/null || { echo "mkcpdata: mke2fs not found (install e2fsprogs)" >&2; exit 2; }
if [ -e "$IMG" ] && [ "${CPDATA_FORCE:-0}" != 1 ]; then
  echo "mkcpdata: $IMG exists (CPDATA_FORCE=1 to recreate)" >&2
  exit 1
fi
rm -f "$IMG"
truncate -s "$SIZE" "$IMG"
mke2fs -q -F -t ext2 -b 4096 -I 256 -O ^dir_index -m 0 -L cpdata \
  -E root_owner=0:0 "$IMG"
echo "mkcpdata: $IMG ($SIZE sparse) — $(dumpe2fs -h "$IMG" 2>/dev/null | sed -n 's/^Filesystem features: *//p')"
