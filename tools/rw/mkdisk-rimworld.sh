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
RUN_QEMU=0
RUNTIME_ONLY=0
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
    --run) RUN_QEMU=1; shift ;;
    --runtime-only) RUNTIME_ONLY=1; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done


# The exact x86_64 commandline the Makefile's QEMU_CMD builds (minus the
# make-time plumbing), so the image can be booted WITHOUT re-running the recipe
# that would recreate it.
qemu_cmd() {
  local edk2
  edk2=$(ls "${HOMEBREW_PREFIX:-/opt/homebrew}"/share/qemu/edk2-x86_64-code.fd 2>/dev/null | head -1)
  edk2=${edk2:-/opt/homebrew/share/qemu/edk2-x86_64-code.fd}
  printf '%s\n' "qemu-system-x86_64 -M q35,vmport=on -smp ${QEMU_SMP:-8} -m 12288M \
    -pflash $edk2 -display none -serial stdio \
    -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 \
    -drive file=disk.img,format=raw,id=disk0,if=none \
    -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -action shutdown=poweroff -qmp unix:/tmp/hb-qmp.sock,server,nowait"
}

# The payload's RimWorldLinux is a normal Linux executable: its PT_INTERP is
# /lib64/ld-linux-x86-64.so.2 and it needs glibc 2.31, libX11 and libGL.  None of
# that is in the base image, so an image holding only the game tree fails at the
# first exec (measured: spawn_ex -> -1 with no /LIB64 and no /lib64 on the disk).
# These are the same three inputs the NFS harness stages, from the same helpers.
stage_runtime() {
  local G=${GLIBC231:-/tmp/glibc231}
  local LDIR="$G/lib/x86_64-linux-gnu"
  [ -f "$LDIR/ld-2.31.so" ] || die "glibc 2.31 not found under ${G} (set GLIBC231=)"
  say "runtime (glibc 2.31 + compat DSOs + Mesa)"
  mmd -i "$IMG" ::/LIB64 ::/lib ::/lib64 ::/lib/x86_64-linux-gnu >/dev/null 2>&1 || true
  local f src dst
  for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 \
           libm-2.31.so:libm.so.6 libpthread-2.31.so:libpthread.so.0 \
           libdl-2.31.so:libdl.so.2 librt-2.31.so:librt.so.1 \
           libgcc_s.so.1:libgcc_s.so.1; do
    src=${f%%:*}; dst=${f##*:}
    mcopy -o -i "$IMG" "$LDIR/$src" "::/LIB64/$dst" || die "could not stage $dst"
    mcopy -o -i "$IMG" "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst" || die "could not stage $dst"
  done
  mcopy -o -i "$IMG" "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2 \
    || die "could not stage the interpreter"
  note "glibc 2.31 under /LIB64 + /lib64/ld-linux-x86-64.so.2"

  bash "$ROOT/src/user/xcompat/stage.sh" "$IMG" | sed 's/^/    /' \
    || die "compat DSO staging failed"
  bash "$ROOT/tools/mesa/stage.sh" "$IMG" | sed 's/^/    /' \
    || die "Mesa staging failed"
}



# Three things the payload needs that are not "the game tree":
#   * UnityPlayer.so, which the AD-9 launcher dlopens from /RWDSO (Unity's own
#     stub finds it beside the exe, but the launcher does not) -- 31 MB, so it is
#     copied rather than linked;
#   * an AD-9 launcher IN PLACE OF the shipped RimWorldLinux, because that stub
#     is an ET_EXEC linked at 0x200000 and the kernel refuses it (measured:
#     spawn_ex errno=8 ENOEXEC).  The real stub is kept as RimWorldLinux.orig,
#     which is what the NFS harness does too;
#   * libz.so.1 and libstdc++.so.6 from a glibc-2.31-era distro: Mono's NEEDED
#     list names them and this host has no x86-64 Linux copy (measured: Mono
#     refused to load libmonobdwgc-2.0.so until libz appeared).
stage_payload_extras() {
  local tree=$1
  say "payload extras (UnityPlayer.so, AD-9 launcher, libz/libstdc++)"
  mmd -i "$IMG" ::/RWDSO >/dev/null 2>&1 || true
  mcopy -o -i "$IMG" "$tree/UnityPlayer.so" ::/RWDSO/UnityPlayer.so \
    || die "could not stage UnityPlayer.so"
  note "UnityPlayer.so -> /RWDSO ($(wc -c < "$tree/UnityPlayer.so" | tr -d ' ') bytes)"

  local G=${GLIBC231:-/tmp/glibc231} D=${GLIBC231_DEV:-/tmp/glibc231-dev}
  local LDIR="$G/lib/x86_64-linux-gnu" DLIB="$D/usr/lib/x86_64-linux-gnu"
  local ad9=$ROOT/obj/rw/RimWorldLinux.ad9
  mkdir -p "$ROOT/obj/rw"
  clang --target=x86_64-linux-gnu -fuse-ld=lld -O2 -w -fPIE -pie -nostdlib \
        -nostartfiles -rdynamic -nostdinc -isystem "$D/usr/include" \
        -isystem "$D/usr/include/x86_64-linux-gnu" \
        -isystem "$(clang --target=x86_64-linux-gnu -print-resource-dir)/include" \
        -o "$ad9" "$ROOT/src/user/rwmain.c" "$ROOT/tools/rw/mono-probe/condtrace.c" \
        "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
        "$LDIR/libc.so.6" "$LDIR/libpthread-2.31.so" "$LDIR/libdl-2.31.so" \
        "$DLIB/crtn.o" -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
        -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
    || die "AD-9 launcher link failed"
  # Keep the shipped stub, install the launcher over it (see above).
  mtype -i "$IMG" "${DEST}/RimWorldLinux" > "$CACHE/RimWorldLinux.orig" 2>/dev/null || true
  [ -s "$CACHE/RimWorldLinux.orig" ] \
    && mcopy -o -i "$IMG" "$CACHE/RimWorldLinux.orig" "${DEST}/RimWorldLinux.orig" || true
  mcopy -o -i "$IMG" "$ad9" "${DEST}/RimWorldLinux" || die "could not install the AD-9 launcher"
  note "AD-9 launcher installed as ${DEST}/RimWorldLinux ($(wc -c < "$ad9" | tr -d ' ') bytes)"

  # libz/libstdc++ come from the mono fetch (third_party/rw-mono/root/lib) or a
  # local drop in $RW_LIBS; absence is reported, not fatal, so a host that
  # already has them staged can still run.
  local libdirs="${RW_LIBS:-$ROOT/third_party/rw-mono/root/lib} /tmp/rwlibs/lib/x86_64-linux-gnu /tmp/rwlibs/cxx/usr/lib/x86_64-linux-gnu"
  local z std
  z=$(ls $libdirs/libz.so.1* 2>/dev/null | head -1)
  std=$(ls $libdirs/libstdc++.so.6.0.* 2>/dev/null | head -1)
  if [ -n "$z" ]; then
    mcopy -o -i "$IMG" "$z" ::/LIB64/libz.so.1 || die "could not stage libz"
    note "libz.so.1 <- $z"
  else
    say "WARNING: no x86-64 libz.so.1 found (set RW_LIBS=); Mono will fail to load"
  fi
  if [ -n "$std" ]; then
    mcopy -o -i "$IMG" "$std" ::/LIB64/libstdc++.so.6 || die "could not stage libstdc++"
    note "libstdc++.so.6 <- $std"
  else
    say "WARNING: no libstdc++.so.6 found (set RW_LIBS=)"
  fi
}

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

if [ "$RUNTIME_ONLY" = 1 ]; then
  [ -f "$IMG" ] || die "--runtime-only but $IMG does not exist"
  stage_runtime
  exit 0
fi

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

# --- payload extras, then the runtime they need -----------------------------
stage_payload_extras "$STAGE"
stage_runtime

# --- launcher + menu --------------------------------------------------------
# The desktop's RimWorld entry is GENERATED at disk-build time: gen-icewm-menu.sh
# emits `prog "RimWorld" app HBHOST.BIN RWLAUNCH.BIN` only if RWLAUNCH.BIN is
# already on the disk.  Staging the launcher after the build therefore produced a
# menu without the entry (measured: /ICEWM/menu read back from the image had no
# RimWorld row), so the launcher goes in first and the menu is regenerated.
say "launcher + menu"
LAUNCHER=$ROOT/obj/rw/rwlaunch.bin
if [ ! -f "$LAUNCHER" ]; then
  note "building rwlaunch.bin"
  make -s -C "$ROOT" ARCH="$ARCH" "obj/$ARCH/libc.a" >/dev/null 2>&1 \
    || die "libc.a build failed"
  RESINC="$(clang -print-resource-dir)/include"
  mkdir -p "$(dirname "$LAUNCHER")"
  clang -O2 -w -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include \
        -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone \
        -nostdinc -isystem "$RESINC" -mcmodel=large -DRW_WAIT_SECS=900 \
        -c src/user/rwlaunch.c -o "$(dirname "$LAUNCHER")/rwlaunch.o" \
    || die "rwlaunch.c did not compile"
  ld.lld -T src/user/linker.ld -e _start -o "$(dirname "$LAUNCHER")/rwlaunch.elf" \
        "$(dirname "$LAUNCHER")/rwlaunch.o" "obj/$ARCH/libc.a" || die "rwlaunch link failed"
  OBJCOPY=$(command -v llvm-objcopy || ls /opt/homebrew/opt/llvm/bin/llvm-objcopy 2>/dev/null)
  [ -n "$OBJCOPY" ] || die "llvm-objcopy not found (brew install llvm)"
  "$OBJCOPY" -O binary "$(dirname "$LAUNCHER")/rwlaunch.elf" "$LAUNCHER"
fi
mcopy -o -i "$IMG" "$LAUNCHER" ::/RWLAUNCH.BIN || die "could not stage RWLAUNCH.BIN"
note "RWLAUNCH.BIN: $(wc -c < "$LAUNCHER" | tr -d ' ') bytes"

# The launcher reads /GAMEDIR and, when it holds an absolute path, skips the NFS
# mount entirely and runs the tree in place.
printf '%s\n' "${DEST#::}" > "$CACHE/gamedir"
mcopy -o -i "$IMG" "$CACHE/gamedir" ::/GAMEDIR || die "could not stage /GAMEDIR"
note "/GAMEDIR = ${DEST#::}"

GEN=$(mktemp -d)
sh "$ROOT/src/user/x11sess/gen-icewm-menu.sh" "$IMG" "$ROOT/src/user/desktop.c" "$GEN" \
  || die "menu generation failed"
mcopy -o -i "$IMG" "$GEN/menu" ::/ICEWM/menu || die "could not install /ICEWM/menu"
[ -f "$GEN/toolbar" ] && mcopy -o -i "$IMG" "$GEN/toolbar" ::/ICEWM/toolbar || true
rm -rf "$GEN"
MENU=$(mtype -i "$IMG" ::/ICEWM/menu 2>/dev/null || true)
# Herestring again: `grep -q` exits early and the writer takes SIGPIPE, which
# `set -euo pipefail` would report as a failure of a successful check.
if grep -qi 'RIMWORLD' <<<"$MENU"; then
  note "menu entry present: $(grep -i rimworld <<<"$MENU" | head -1 | sed 's/^ *//')"
else
  die "the regenerated /ICEWM/menu has no RimWorld entry"
fi

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

   BOOT IT WITH QEMU DIRECTLY -- not with 'make run'.
   'make run' depends on disk.img, and disk.img depends on the PHONY target
   x11-staged, so every 'make run' re-runs the disk recipe: it recreates a
   ${DISK_MB} MB image from dd and mcopies the base files over it, silently
   throwing the staged game away (measured: a 1.5 GiB image with the payload came
   back as 1 GiB and empty after one 'make run').

     bash $0 --run

   which is:
     $(qemu_cmd | sed 's/^/     /')
EOF

if [ "$RUN_QEMU" = 1 ]; then
  say "booting (direct qemu -- the image is NOT rebuilt)"
  rm -f /tmp/hb-qmp.sock
  exec bash -c "$(qemu_cmd)"
fi
