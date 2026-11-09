#!/usr/bin/env bash
# tools/rw/mkdisk-rimworld.sh -- build the HobbyOS x86_64 desktop image and put a
# RimWorld Linux payload INTO the FAT16 image, so the game is played from the
# guest's own filesystem instead of a host NFS export.
#
#   bash tools/rw/mkdisk-rimworld.sh                 # find payload, build, stage
#   bash tools/rw/mkdisk-rimworld.sh --no-build      # reuse an existing disk.img
#   bash tools/rw/mkdisk-rimworld.sh --payload /path/to/RimWorld
#
# Why a script and not a Makefile rule: the game is third-party software. It must
# never enter this repository, never be committed, and never be required for a
# normal build -- but staging it is a real build step with a payload to locate, an
# architecture to verify, a filesystem to fill and a capacity to check.
#
# PAYLOAD DISCOVERY (first hit wins; --payload or $RW_SRC overrides)
#   1. $RW_SRC / --payload                     explicit
#   2. $HOME/rimworld/rimworld-linux.tar       the tarball this Mac pulls from the NAS
#   3. $HOME/rimworld/RimWorld                 an already-extracted tree
#   4. NFS / farm paths                        /mnt/fast/games/..., /mnt/Flash/...
#   5. Steam libraries                         including the macOS one, which is
#                                              DETECTED AND REFUSED with a reason
#
# The payload is validated by content, not by name: RimWorldLinux must be an
# ELF 64-bit x86-64 executable and RimWorldLinux_Data/Managed must exist. A
# Windows (RimWorldWin64.exe) or macOS (RimWorld.app) copy is rejected, because
# neither can load in the HobbyOS Linux-ABI compat process.
#
# Nothing is ever written inside the git work tree except disk.img (ignored).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

ARCH=${ARCH:-intel}
DESKTOP=${DESKTOP:-icewm}
DEST=${RW_DEST:-::/GAMES/RIMWORLD}
# 1024 is the Makefile default; the payload (806 MB apparent) needs more room, so
# this script asks for 1536 unless told otherwise.  FAT16 must stay at 32 KiB
# clusters, i.e. <= ~1.5 GiB.
DISK_MB=${DISK_MB:-1536}
BUILD=1
PAYLOAD=${RW_SRC:-}
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/hobbyos-rimworld

say()  { printf '\n== %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --payload) PAYLOAD=${2:?--payload needs a path}; shift 2 ;;
    --dest)    DEST=${2:?--dest needs a path}; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

MTOOLS_PREFIX=""
for t in mcopy mmd mdir mkfs.fat; do
  command -v "$t" >/dev/null 2>&1 || die "missing $t (brew install mtools dosfstools)"
done

IMG=$ROOT/disk.img

# --- payload discovery ------------------------------------------------------
looks_like_linux_rimworld() {  # $1 = candidate directory
  [ -f "$1/RimWorldLinux" ] && [ -d "$1/RimWorldLinux_Data/Managed" ]
}
looks_like_macos_rimworld() {
  [ -d "$1/RimWorld.app" ] || [ -d "$1/Contents/MacOS" ] || [ -f "$1/Contents/MacOS/RimWorld" ]
}
looks_like_windows_rimworld() {
  ls "$1"/RimWorldWin64.exe "$1"/*.exe >/dev/null 2>&1
}

find_payload() {
  # 1-2: explicit, then the tarball this host knows how to make
  for c in "$PAYLOAD"; do
    [ -n "$c" ] && [ -e "$c" ] && { echo "$c"; return 0; }
  done
  [ -f "$HOME/rimworld/rimworld-linux.tar" ] && { echo "$HOME/rimworld/rimworld-linux.tar"; return 0; }
  [ -d "$HOME/rimworld/RimWorld" ] && { echo "$HOME/rimworld/RimWorld"; return 0; }

  # 4: mounted NAS / farm layouts
  local p
  for p in \
    /mnt/fast/games/steamapps/common/RimWorld \
    /mnt/Flash/storageds/games/steamapps/common/RimWorld \
    /mnt/*/games/steamapps/common/RimWorld \
    /Volumes/*/games/steamapps/common/RimWorld
  do
    [ -d "$p" ] && { echo "$p"; return 0; }
  done

  # 5: Steam libraries -- the macOS one is found so it can be refused by name
  for p in \
    "$HOME/Library/Application Support/Steam/steamapps/common/RimWorld" \
    "$HOME/.steam/steam/steamapps/common/RimWorld" \
    "$HOME/.local/share/Steam/steamapps/common/RimWorld"
  do
    [ -d "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}

# A payload path must never point inside the work tree: the game is not ours to
# commit, and a stray `git add -A` would be the end of it.
inside_repo() {
  local p=$1 top
  top=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null || echo "$ROOT")
  case "$(cd "$(dirname "$p")" 2>/dev/null && pwd)/$(basename "$p")" in
    "$top"/*) return 0 ;;
  esac
  return 1
}

say "payload"
SRC=$(find_payload) || die "no RimWorld payload found.
  Expected one of:
    \$RW_SRC / --payload
    \$HOME/rimworld/rimworld-linux.tar
    \$HOME/rimworld/RimWorld
    /mnt/fast/games/steamapps/common/RimWorld
    a Steam library path
  To bring one here from the NAS (no NFS, no sudo, read-only):
    mkdir -p \$HOME/rimworld && ssh proxmox47 \\
      'tar -C /mnt/Flash/storageds/games/steamapps/common -cf - RimWorld' \\
      > \$HOME/rimworld/rimworld-linux.tar"
note "found: $SRC"

inside_repo "$SRC" && die "payload lives inside the repository ($SRC).
  The game must never be committed: move it out, or pass --payload."

# --- validate by content ----------------------------------------------------
if [ -f "$SRC" ]; then
  case "$SRC" in
    *.tar|*.tar.gz|*.tgz) ;;
    *) die "$SRC is a file but not a .tar/.tar.gz/.tgz" ;;
  esac
  mkdir -p "$CACHE"
  TREE=$CACHE/RimWorld
  say "extract"
  rm -rf "$TREE"
  case "$SRC" in
    *.tar)    tar -C "$CACHE" -xf "$SRC" ;;
    *.tar.gz|*.tgz) tar -C "$CACHE" -xzf "$SRC" ;;
  esac
  [ -d "$TREE" ] || TREE=$(find "$CACHE" -maxdepth 2 -type d -name RimWorld | head -1)
  note "extracted to $TREE"
else
  TREE=$SRC
fi

say "validate"
if looks_like_macos_rimworld "$TREE"; then
  die "$TREE is the macOS build (RimWorld.app / Contents/MacOS).
  HobbyOS needs the LINUX x86-64 build: in Steam choose Properties > Betas, or
  use the depot download (download_depot 294100 294103)."
fi
if ! looks_like_linux_rimworld "$TREE"; then
  if looks_like_windows_rimworld "$TREE"; then
    die "$TREE is a Windows build (*.exe). HobbyOS needs RimWorldLinux."
  fi
  die "$TREE is not a Linux RimWorld tree (no RimWorldLinux + RimWorldLinux_Data/Managed)"
fi
ELF=$(file -b "$TREE/RimWorldLinux")
case "$ELF" in
  *"ELF 64-bit"*x86-64*) note "RimWorldLinux: x86-64 ELF (ok)" ;;
  *) die "RimWorldLinux is not an x86-64 ELF: $ELF" ;;
esac

# --- filtered staging copy (outside the repo, junk excluded) ----------------
say "stage (filtered)"
STAGE=$CACHE/stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
# mono_crash.*.blob is a dump left by a previous run of the game on the server;
# it is not part of the distribution and can be hundreds of MB.
rsync -a \
  --exclude 'mono_crash.*' --exclude '*.log' --exclude 'Player.log' \
  --exclude '.git' --exclude '*.pdb' \
  "$TREE"/ "$STAGE"/
rm -f "$STAGE"/mono_crash.* 2>/dev/null || true
BYTES=$(du -sk "$STAGE" | awk '{print $1}')
NFILES=$(find "$STAGE" -type f | wc -l | tr -d ' ')
note "$NFILES files, $(echo "$BYTES" | awk '{printf "%.0f MB", $1/1024}')"

# --- build the image --------------------------------------------------------
if [ "$BUILD" = 1 ]; then
  say "build disk image ($ARCH, $DESKTOP)"
  # A pre-existing image is the wrong SIZE as well as the wrong content, and make
  # will not rebuild it just because DISK_MB changed, so drop it when it does not
  # match what we need.
  if [ -f "$IMG" ]; then
    cur_mb=$(( $(stat -f%z "$IMG" 2>/dev/null || stat -c%s "$IMG") / 1048576 ))
    if [ "$cur_mb" -lt "$DISK_MB" ]; then
      note "existing image is ${cur_mb} MB < ${DISK_MB} MB: rebuilding"
      rm -f "$IMG"
    fi
  fi
  TOOLS_SKIP=1 DISK_MB="$DISK_MB" make -C "$ROOT" ARCH="$ARCH" MODE=desktop \
       DESKTOP="$DESKTOP" hobbyos.elf disk.img
else
  say "build (skipped: --no-build)"
  [ -f "$IMG" ] || die "--no-build but $IMG does not exist"
fi

# --- capacity check ---------------------------------------------------------
say "capacity"
# Re-running must reclaim the space the old copy holds, or the check below sees a
# full disk and refuses to replace a payload that is already there (measured:
# second run reported "336 MB free, 736 MB needed" on a correctly staged image).
if mdir -i "$IMG" "$DEST" >/dev/null 2>&1; then
  note "removing the previous copy at $DEST (space is reclaimed below)"
  mdeltree -i "$IMG" "$DEST" 2>/dev/null || die "could not clear $DEST"
fi
FREE_KB=$(mdir -i "$IMG" :: 2>/dev/null | awk '/bytes free/ {gsub(/[^0-9]/,"",$0); print int($0/1024)}' | tail -1)
FREE_KB=${FREE_KB:-0}
need_kb=$((BYTES + 4096))
note "free: $((FREE_KB/1024)) MB, need: $((need_kb/1024)) MB"
if [ "$FREE_KB" -lt "$need_kb" ]; then
  die "disk.img has no room for the payload ($((FREE_KB/1024)) MB free, $((need_kb/1024)) MB needed).
  Grow the image first (see the Makefile's disk size) or stage a subset."
fi

# --- copy into the image ----------------------------------------------------
say "copy into $DEST"
mmd -i "$IMG" ::/GAMES >/dev/null 2>&1 || true
mmd -i "$IMG" "${DEST}" >/dev/null 2>&1 || true
# -s recursive, -m keep mtimes, -o overwrite, -Q quit-on-error.  mtools handles
# FAT long names; the tree keeps the exact case Unity expects.
mcopy -s -m -o -Q -i "$IMG" "$STAGE"/* "${DEST}"/ || die "mcopy failed"

# --- verify from the image, not from the staging tree -----------------------
say "verify (read back out of disk.img)"
set +e
LIST=$(mdir -i "$IMG" -/ "${DEST}" 2>/dev/null)
set -e
# Herestring, not a pipe: `grep -q` exits on the first match and the writing
# side then takes SIGPIPE, which `set -e` + `pipefail` turn into a 141 abort --
# after the copy has already succeeded (measured: whole staging run failed on a
# successful copy).
if ! grep -qi 'RimWorldLinux' <<<"$LIST"; then
  head -20 <<<"$LIST"
  die "RimWorldLinux is not in $DEST after the copy"
fi
tail -3 <<<"$LIST"
note "RimWorldLinux present in $DEST"

cat <<EOF

== done
   image:   $IMG
   payload: $DEST (from $SRC, staged via $STAGE)
   never committed: the game lives only in disk.img and $CACHE

   Play it on this host:
     TOOLS_SKIP=1 make run ARCH=$ARCH MODE=desktop DESKTOP=$DESKTOP \\
       QEMU_DISPLAY=none QEMU_ARGS="-qmp unix:/tmp/hb-qmp.sock,server,nowait"
EOF
