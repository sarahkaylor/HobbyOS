#!/bin/bash
# =============================================================================
# tools/nv/fetch-firmware.sh -- fetch the GSP firmware this driver will have to
# load onto an AD102 (RTX 4090), verify it, and record exactly what landed.
#
# The split this script exists for (measured 2026-11-22):
#   nvidia/ad102/gsp/  has booter_load, booter_unload, bootloader -- and NO RM
#                      image.
#   nvidia/ga102/gsp/  has the same three AND gsp-<ver>.bin, whose ELF carries
#                      both .fwsignature_ga10x and .fwsignature_ad10x.
# So an AD102 needs blobs from BOTH directories.  A fetch that only looks under
# ad102/ finds everything except the 38 MB file that actually matters.
#
# Nothing here is committed to the repository (third_party/ is gitignored).
# =============================================================================
set -euo pipefail

VERSION="${NV_GSP_VERSION:-535.113.01}"
DEST="${NV_FW_DEST:-third_party/nvfw}"
BASE="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"
DIRS="ad102 ga102"

say() { echo "[nvfw] $*"; }

# sha256 of the pinned 535.113.01 set, computed from the files themselves and
# keyed by DIRECTORY AS WELL AS NAME: the AD102 and GA102 booters carry the same
# version string but are different programs (measured: ad102/booter_load is
# 55 928 B, ga102's is 59 768 B).  Keying on the name alone made the first run of
# this script fail on its own checksum table -- a good bug to have found here
# rather than after placing the wrong booter in IMEM.  A mismatch means upstream
# changed the blobs under a fixed version string: stop, do not proceed.
expected() { # $1 = dir, $2 = name
  case "$1/$2" in
  ad102/booter_load-535.113.01.bin) echo 7d4ef3258da016f5208cfb1b348a9a1d69aaa13c2f8d4ee8c05d66bd05b45cd6 ;;
  ad102/booter_unload-535.113.01.bin) echo d036dfee0545c36e2194bc555cb268a5e11a6f13b5f66a7dec40838efc6ad87e ;;
  ad102/bootloader-535.113.01.bin) echo 044d3a21eff73eb12f22840e5f145d685727f40165b3f2d265cf664753e9ff9c ;;
  ga102/booter_load-535.113.01.bin) echo 3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0 ;;
  ga102/booter_unload-535.113.01.bin) echo 09240c83595856a0fe70d2ab3ec7325455cd8adfc48ec7b47f6fced1e5431beb ;;
  ga102/bootloader-535.113.01.bin) echo 076d2a0675131ad8a8de215c611ce4271289adcc03e2cf39995f9b29079e2136 ;;
  ga102/gsp-535.113.01.bin) echo e30231ec0d317021770e16c81ae2c0553f728d0344248827582f78132c9447d4 ;;
  *) echo "" ;;
  esac
}

mkdir -p "$DEST"
MANIFEST="$DEST/MANIFEST.txt"
: >"$MANIFEST.tmp"

for dir in $DIRS; do
  out="$DEST/$dir/gsp"
  mkdir -p "$out"
  for f in booter_load booter_unload bootloader gsp; do
    name="$f-$VERSION.bin"
    url="$BASE/nvidia/$dir/gsp/$name"
    if ! curl -fsSL --retry 3 -o "$out/$name.part" "$url"; then
      rm -f "$out/$name.part"
      say "absent: nvidia/$dir/gsp/$name (expected for gsp-* under ad102)"
      continue
    fi
    mv "$out/$name.part" "$out/$name"
    got=$(sha256sum "$out/$name" | awk '{print $1}')
    want=$(expected "$dir" "$name")
    size=$(stat -c%s "$out/$name" 2>/dev/null || stat -f%z "$out/$name")
    if [ -n "$want" ] && [ "$got" != "$want" ]; then
      say "CHECKSUM MISMATCH $dir/$name"
      say "  want $want"
      say "  got  $got"
      exit 1
    fi
    printf '%-10s %-30s %10s  %s\n' "$dir" "$name" "$size" "$got" >>"$MANIFEST.tmp"
    say "ok $dir/$name ($size bytes)"
  done
done

{
  echo "# GSP firmware staged by tools/nv/fetch-firmware.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# version $VERSION; sizes and sha256 computed from the files"
  echo "# dir        file                                bytes  sha256"
  cat "$MANIFEST.tmp"
} >"$MANIFEST"
rm -f "$MANIFEST.tmp"

say "manifest: $MANIFEST"
grep -c . "$MANIFEST" | xargs -I{} echo "[nvfw] {} lines recorded"
say "next: docs/nv-gsp-firmware.md sections 3 and 6 are the map these bytes describe"
