#!/usr/bin/env bash
# tools/rw/run-rimworld.sh — M6 launch plumbing: stage the pinned glibc 2.31
# under /LIB64, stage the launcher (and optionally the /proc probe), boot the
# intel guest with the RimWorld NFS export live, and produce the ORDERED
# failure ladder from the real serial log.
#
#   tools/rw/run-rimworld.sh probe [--timeout S]   # /proc plumbing probe
#   tools/rw/run-rimworld.sh launchprobe [--timeout S]  # same probe, spawned
#                                                  # from NFS by the launcher
#   tools/rw/run-rimworld.sh game  [--timeout S]   # the real RimWorldLinux
#   tools/rw/run-rimworld.sh stub  [--timeout S]   # PIE stand-in with the
#                                                  # game's NEEDED list: the
#                                                  # wall AFTER the ET_EXEC one
#   tools/rw/run-rimworld.sh ad9   [--timeout S]   # AD-9 PlayerMain launcher
#                                                  # installed as RimWorldLinux,
#                                                  # DSOs staged locally (AD-10)
#
# Env: RW_NFS_SERVER (default 10.0.2.2), RW_NFS_ALT, RW_SKIP_EXPORT=1,
#      GLIBC231 / GLIBC231_DEV to reuse an extraction.
#
# Nothing from the game directory is copied into the tree: the guest reads it
# over NFS from /srv/nfs/rimworld (tools/rw/nfs-export.sh sync).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

MODE="${1:-game}"
[ "$MODE" = probe ] || [ "$MODE" = launchprobe ] || [ "$MODE" = game ] || \
  [ "$MODE" = stub ] || [ "$MODE" = ad9 ] || {
  echo "usage: $0 {probe|launchprobe|game|stub|ad9} [--timeout S]" >&2; exit 2; }
shift || true
TIMEOUT_S=180
while [ $# -gt 0 ]; do
  case "$1" in
    --timeout) TIMEOUT_S="$2"; shift 2 ;;
    *) echo "usage: $0 {probe|launchprobe|game|stub|ad9} [--timeout S]" >&2; exit 2 ;;
  esac
done

SERVER="${RW_NFS_SERVER:-10.0.2.2}"
ALT="${RW_NFS_ALT:-192.168.0.123}"
OUT="$ROOT/obj/rw-launch"
LOG="$OUT/serial-$MODE.log"
mkdir -p "$OUT"

# OVMF lives under the real HOME; a scratch HOME breaks the Makefile's path.
if [ ! -f "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" ] && \
   [ -f "/home/$(id -un)/.local/share/OVMF/OVMF_CODE_4M.fd" ]; then
  HOME="/home/$(id -un)"; export HOME
fi
PATH="/usr/sbin:/sbin:$PATH"; export PATH
for t in clang ld.lld llvm-objcopy mcopy mmd mkfs.fat gcc qemu-system-x86_64; do
  command -v "$t" >/dev/null 2>&1 || { echo "ENVIRONMENT: missing $t"; exit 2; }
done

G="${GLIBC231:-/tmp/glibc231}"
D="${GLIBC231_DEV:-/tmp/glibc231-dev}"
LDIR="$G/lib/x86_64-linux-gnu"
DLIB="$D/usr/lib/x86_64-linux-gnu"
LIBC6_SHA=1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0
LIBGCC_SHA=4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c
LIBC6DEV_SHA=11029900f2315b93ce487792ae1bf15f0927e5e50880c091e6a440b14d4e164a

fetch_deb() { # $1 url, $2 sha256, $3 dest.deb
  mkdir -p "$(dirname "$3")"
  if [ ! -f "$3" ]; then
    echo "rw: fetching $(basename "$3")"
    curl -fsS -o "$3" "$1" || { echo "rw: FETCH FAILED $1"; exit 2; }
  fi
  echo "$2  $3" | sha256sum -c --quiet - || { echo "rw: SHA256 MISMATCH $3"; exit 2; }
}

if [ ! -f "$LDIR/ld-2.31.so" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6_SHA" "$T/libc6.deb"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb \
            "$LIBGCC_SHA" "$T/libgcc-s1.deb"
  rm -rf "$G" && mkdir -p "$G"
  dpkg-deb -x "$T/libc6.deb" "$G" && dpkg-deb -x "$T/libgcc-s1.deb" "$G"
fi
if [ ! -f "$DLIB/Scrt1.o" ]; then
  T="$ROOT/obj/m1-pins"
  fetch_deb http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6-dev_2.31-0ubuntu9.18_amd64.deb \
            "$LIBC6DEV_SHA" "$T/libc6-dev.deb"
  rm -rf "$D" && mkdir -p "$D"
  dpkg-deb -x "$T/libc6-dev.deb" "$D"
fi

echo "== build: kernel + disk"
TOOLS_SKIP=1 make -j8 ARCH=intel MODE=test hobbyos.elf disk.img \
  >"$OUT/build.log" 2>&1 || { echo "BUILD FAILED"; tail -30 "$OUT/build.log"; exit 2; }

echo "== build: launcher (native) + probe (glibc 2.31 PIE)"
make -s ARCH=intel obj/intel/libc.a >"$OUT/build_libc.log" 2>&1 || {
  echo "BUILD FAILED (libc.a)"; tail -20 "$OUT/build_libc.log"; exit 2; }
RESINC="$(clang -print-resource-dir)/include"
UCFLAGS=(-O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics
         -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding
         -mno-red-zone -nostdinc -isystem "$RESINC" -mcmodel=large)
clang "${UCFLAGS[@]}" -c src/user/rwlaunch.c -o "$OUT/rwlaunch.o" || exit 2
ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwlaunch.elf" "$OUT/rwlaunch.o" obj/intel/libc.a || exit 2
llvm-objcopy -O binary "$OUT/rwlaunch.elf" "$OUT/rwlaunch.bin"

gcc -O2 -fPIE -pie -nostdlib -nostartfiles -o "$OUT/rw_procprobe" src/user/rw_procprobe.c \
    "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
    "$LDIR/libc.so.6" "$DLIB/crtn.o" \
    -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || { echo "probe link failed"; exit 2; }
readelf -h "$OUT/rw_procprobe" | awk '/Type:/{print "  probe:",$2}'

if [ "$MODE" = ad9 ]; then
  # AD-9: the game's own executable, rebuilt as a glibc-2.31 PIE (AD-11: the
  # compat ABI is glibc's, so its headers and crt objects are the pin's).
  # -rdynamic keeps dlopen() in .dynsym: that wrapper is what redirects
  # Unity's Mono/plugin DSO loads to the local AD-10 staging dir.
  echo "== build: AD-9 PlayerMain launcher (glibc 2.31 PIE)"
  gcc -O2 -Wall -Wextra -fPIE -pie -nostdlib -nostartfiles -rdynamic \
      -nostdinc -isystem "$D/usr/include" \
      -isystem "$D/usr/include/x86_64-linux-gnu" \
      -isystem "$(gcc -print-file-name=include)" \
      -o "$OUT/RimWorldLinux.ad9" src/user/rwmain.c \
      "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
      "$LDIR/libc.so.6" "$LDIR/libdl-2.31.so" "$DLIB/crtn.o" \
      -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
      -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none || {
    echo "AD-9 launcher link failed"; exit 2; }
  readelf -h "$OUT/RimWorldLinux.ad9" | awk '/Type:/{print "  ad9:",$2}'
  readelf -sW "$OUT/RimWorldLinux.ad9" | grep -qw dlopen || {
    echo "AD-9 launcher does not export dlopen"; exit 2; }
fi

if [ "$MODE" = stub ] || [ "$MODE" = launchprobe ]; then
  if [ "$MODE" = stub ]; then
    # Same launcher, pointed at the stand-in; the stand-in carries the game's
    # NEEDED list and $ORIGIN rpath so ld.so must resolve UnityPlayer.so from
    # /NFS exactly as it would for the real binary.
    [ -f /srv/nfs/rimworld/UnityPlayer.so ] || { echo "ENVIRONMENT: no UnityPlayer.so in export"; exit 2; }
    clang "${UCFLAGS[@]}" -DRW_GAME='"/NFS/RWTEST.ELF"' -c src/user/rwlaunch.c -o "$OUT/rwstublaunch.o" || exit 2
    ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwstublaunch.elf" "$OUT/rwstublaunch.o" obj/intel/libc.a || exit 2
    llvm-objcopy -O binary "$OUT/rwstublaunch.elf" "$OUT/rwstublaunch.bin"
    gcc -O2 -fPIE -pie -nostdlib -nostartfiles -o "$OUT/RWTEST.ELF" src/user/rwstub.c \
        "$DLIB/Scrt1.o" "$DLIB/crti.o" "$DLIB/libc_nonshared.a" \
        "$LDIR/libc.so.6" "$DLIB/crtn.o" \
        -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
        -Wl,-rpath-link,"$LDIR" -Wl,--build-id=none \
        -Wl,--no-as-needed -Wl,-rpath,'$ORIGIN' -L/srv/nfs/rimworld -l:UnityPlayer.so \
        -Wl,--allow-shlib-undefined || { echo "stub link failed"; exit 2; }
    readelf -dW "$OUT/RWTEST.ELF" | grep -E 'NEEDED|RUNPATH' | sed 's/^/  stub: /'
  else
    # The /proc probe, but spawned by the launcher from NFS: proves the whole
    # launch path (env, argv, executable path, /proc answers) short of the
    # game binary itself.
    clang "${UCFLAGS[@]}" -DRW_GAME='"/NFS/RWPROBE.ELF"' -c src/user/rwlaunch.c -o "$OUT/rwprobelaunch.o" || exit 2
    ld.lld -T src/user/linker.ld -e _start -o "$OUT/rwprobelaunch.elf" "$OUT/rwprobelaunch.o" obj/intel/libc.a || exit 2
    llvm-objcopy -O binary "$OUT/rwprobelaunch.elf" "$OUT/rwprobelaunch.bin"
  fi
fi

echo "== stage: glibc runtime under /LIB64 (+ ld.so's own default dir)"
IMG="$ROOT/disk.img"
mmd -i "$IMG" ::/LIB64 ::/lib ::/lib64 ::/lib/x86_64-linux-gnu ::/NFS >/dev/null 2>&1 || true
stage() { mcopy -o -i "$IMG" "$1" "$2" || { echo "mcopy failed $2"; exit 2; }; }
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 libm-2.31.so:libm.so.6 \
         libpthread-2.31.so:libpthread.so.0 libdl-2.31.so:libdl.so.2 \
         librt-2.31.so:librt.so.1 libgcc_s.so.1:libgcc_s.so.1; do
  src="${f%%:*}"; dst="${f##*:}"
  stage "$LDIR/$src" "::/LIB64/$dst"
  stage "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst"
done
stage "$LDIR/ld-2.31.so" ::/lib64/ld-linux-x86-64.so.2

printf 'PROC-FIXTURE:0123456789abcdef\n' > "$OUT/PROBE.TXT"

if [ "$MODE" = probe ]; then
  # A path LONGER than the 32-byte display name on purpose: /proc/self/exe
  # must answer the full loaded path (p->exe_path, 160 bytes), not the
  # truncated p->name that the old implementation returned.
  mmd -i "$IMG" ::/RIMWORLD_PROBE_LONG_NAME >/dev/null 2>&1 || true
  stage "$OUT/rw_procprobe" ::/RIMWORLD_PROBE_LONG_NAME/PROC.ELF
  stage "$OUT/PROBE.TXT" ::/PROBE.TXT
  printf 'RIMWORLD_PROBE_LONG_NAME/PROC.ELF\n' > "$OUT/wave.cfg"
else
  # The export must be live and carry the real tree.  A sync refreshes the
  # read-only copy; nothing from it enters git.
  if [ "${RW_SKIP_EXPORT:-0}" != 1 ]; then
    bash tools/rw/nfs-export.sh sync >"$OUT/export.log" 2>&1 || {
      echo "EXPORT FAILED"; tail -20 "$OUT/export.log"; exit 2; }
    tail -3 "$OUT/export.log"
  fi
  [ -x /srv/nfs/rimworld/RimWorldLinux ] || { echo "ENVIRONMENT: /srv/nfs/rimworld/RimWorldLinux missing"; exit 2; }
  if [ "$MODE" = stub ]; then
    cp -f "$OUT/RWTEST.ELF" /srv/nfs/rimworld/RWTEST.ELF
    stage "$OUT/rwstublaunch.bin" ::/RWLAUNCH.BIN
  elif [ "$MODE" = launchprobe ]; then
    cp -f "$OUT/rw_procprobe" /srv/nfs/rimworld/RWPROBE.ELF
    stage "$OUT/rwprobelaunch.bin" ::/RWLAUNCH.BIN
    stage "$OUT/PROBE.TXT" ::/PROBE.TXT
  elif [ "$MODE" = ad9 ]; then
    # AD-10: the game's DSOs live on disk.img; the data tree stays on NFS.
    # 40.1 MiB: UnityPlayer.so 33.0 + the three Mono DSOs 6.4 + the two
    # plugins 0.7.  Copied straight from the read-only export (never the
    # worktree), so check-no-game-files.sh has nothing to see.
    echo "== stage: AD-10 DSOs locally under /RWDSO"
    mmd -i "$IMG" ::/RWDSO >/dev/null 2>&1 || true
    for rel in UnityPlayer.so \
               RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so \
               RimWorldLinux_Data/MonoBleedingEdge/x86_64/libmono-native.so \
               RimWorldLinux_Data/MonoBleedingEdge/x86_64/libMonoPosixHelper.so \
               RimWorldLinux_Data/Plugins/lib_burst_generated.so \
               RimWorldLinux_Data/Plugins/libsteam_api.so; do
      src="/srv/nfs/rimworld/$rel"
      [ -f "$src" ] || { echo "ENVIRONMENT: missing $src"; exit 2; }
      stage "$src" "::/RWDSO/${rel##*/}"
    done
    mdir -i "$IMG" ::/RWDSO | sed 's/^/  /'
    # libmonobdwgc / libMonoPosixHelper NEED libz.so.1, which /LIB64 does not
    # carry yet (the m2_mono gate stages it from the same Mono tree).  The
    # focal-era zlib from the Mono vendor tree matches the 2.31 pin.
    MZ="$ROOT/third_party/rw-mono/root/lib/libz.so.1"
    if [ ! -e "$MZ" ]; then
      bash tools/rw/fetch-mono.sh >"$OUT/mono-fetch.log" 2>&1 || {
        echo "ENVIRONMENT: no libz.so.1 for the Mono DSOs"; exit 2; }
    fi
    MZ="$(readlink -f "$MZ")"   # stage the ELF, not a 14-byte symlink
    stage "$MZ" ::/LIB64/libz.so.1
    stage "$MZ" ::/lib/x86_64-linux-gnu/libz.so.1
    # AD-9: install the launcher AS the game's executable, in the game dir; the
    # 14,720-byte ET_EXEC stub is preserved beside it as RimWorldLinux.orig.
    # A sync run above restores the stub, so this always re-installs.
    if [ "$(stat -c %s /srv/nfs/rimworld/RimWorldLinux)" = 14720 ]; then
      cp -f /srv/nfs/rimworld/RimWorldLinux /srv/nfs/rimworld/RimWorldLinux.orig
    fi
    cp -f "$OUT/RimWorldLinux.ad9" /srv/nfs/rimworld/RimWorldLinux
    echo "  installed: $(stat -c '%s bytes' /srv/nfs/rimworld/RimWorldLinux) -> RimWorldLinux (stub: $(stat -c '%s' /srv/nfs/rimworld/RimWorldLinux.orig 2>/dev/null || echo '?') bytes)"
    stage "$OUT/rwlaunch.bin" ::/RWLAUNCH.BIN
  else
    stage "$OUT/rwlaunch.bin" ::/RWLAUNCH.BIN
  fi
  printf 'RWLAUNCH.BIN\n' > "$OUT/wave.cfg"
fi
stage "$OUT/wave.cfg" ::/WAVE.CFG

OVMF_SRC=""
for c in "$HOME/.local/share/OVMF/OVMF_CODE_4M.fd" /usr/share/OVMF/OVMF_CODE_4M.fd; do
  [ -f "$c" ] && { OVMF_SRC="$c"; break; }
done
[ -n "$OVMF_SRC" ] || { echo "ENVIRONMENT: no OVMF_CODE_4M.fd"; exit 2; }
OVMF="$OUT/OVMF_CODE_4M.fd"
[ -f "$OVMF" ] || cp "$OVMF_SRC" "$OVMF"

echo "== boot: $MODE (deadline ${TIMEOUT_S}s, log $LOG)"
: >"$LOG"
( make -o disk.img ARCH=intel MODE=test EDK2_X86_64="$OVMF" run \
       QEMU_ARGS="-display none -enable-kvm" >>"$LOG" 2>&1 & )
for _ in $(seq 1 "$TIMEOUT_S"); do
  grep -aq "System halt" "$LOG" && break
  grep -aq "rwlaunch: done" "$LOG" && sleep 2 && break
  sleep 1
done
sleep 1

echo
echo "== guest programs' own rows"
grep -aE '^\[RW\]|^\[RWMAIN\]|^\[LP\]|^\[PROC\]|^\[WAVE-MANIFEST\]|PT_INTERP mapped' "$LOG" || echo "(none)"
echo
echo "== ordered failure ladder (raw serial lines, in order)"
grep -anE '\[LCOMPAT\]|\[RWMAIN\]|error while loading|failed to map|cannot open shared|undefined symbol|symbol lookup|version `|No such file|Operation not supported|ENOSYS|Loading program|PT_INTERP|memory fault|FATAL|\[EXITPC\]|System halt|\[RW\] rwlaunch|\[PROC\] (FAIL|done)' "$LOG" || true
echo
echo "raw serial: $LOG"
