#!/usr/bin/env bash
# tools/jdk/stage.sh -- put a Linux-ABI runtime + payload into disk.img.
#
# Staging is separate from booting on purpose: a guest that reads the image while
# mcopy is still writing sees a half-built FAT (tools/rw/mkdisk-rimworld.sh
# measures this as "the run simply never progresses"), so every caller stages
# first, waits for exit 0, then boots.
#
# What goes on the image, and why each piece:
#   /LIB64 + /lib/x86_64-linux-gnu  the pinned glibc 2.31 runtime the compat
#                                   loader resolves NEEDED from (ld.so's default
#                                   dir AND the rpath dir the JDK's own DSOs use)
#   /lib64/ld-linux-x86-64.so.2     the PT_INTERP every glibc binary names
#   /JDK                            the JDK tree (bin/java, lib/modules, ...)
#   /<NAME>                         probe binaries
#   /WAVE.CFG                       the MODE=test manifest (one prog per line)
#
# Usage:
#   tools/jdk/stage.sh --probe obj/jdk/hello-glibc.exe:HELLO.EXE \
#                      --wave "HELLO.EXE"
#   tools/jdk/stage.sh --jdk third_party/jdk/jdk-27 --wave "JDK/bin/java -version"
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2
IMG="${JDK_IMG:-$ROOT/disk.img}"
G="${GLIBC231:-$HOME/.hermes/cache/scratch/glibc231}"
LDIR="$G/lib/x86_64-linux-gnu"

PROBES=()   # src:guestname
DIRS=()     # srcdir:guestdir
WAVES=()
JDK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --probe) PROBES+=("$2"); shift 2 ;;
    --dir)   DIRS+=("$2"); shift 2 ;;
    --jdk)   JDK=$2; shift 2 ;;
    --wave)  WAVES+=("$2"); shift 2 ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

say() { printf '== %s\n' "$*"; }
mkd() { mmd -i "$IMG" "$1" >/dev/null 2>&1 || true; }
stage() {
  mcopy -o -i "$IMG" "$1" "$2" >/dev/null || { echo "mcopy FAILED: $1 -> $2"; exit 2; }
}

[ -f "$IMG" ] || { echo "no $IMG (build it first)"; exit 2; }
[ -f "$LDIR/ld-2.31.so" ] || { echo "no glibc 2.31 pin at $LDIR (set GLIBC231=)"; exit 2; }

say "glibc 2.31 runtime"
for d in ::/lib64 ::/lib ::/lib/x86_64-linux-gnu ::/LIB64; do mkd "$d"; done
for f in ld-2.31.so:ld-linux-x86-64.so.2 libc-2.31.so:libc.so.6 \
         libm-2.31.so:libm.so.6 libpthread-2.31.so:libpthread.so.0 \
         libdl-2.31.so:libdl.so.2 librt-2.31.so:librt.so.1 \
         libgcc_s.so.1:libgcc_s.so.1; do
  src=${f%%:*}; dst=${f##*:}
  [ -f "$LDIR/$src" ] || { echo "pin is missing $src"; exit 2; }
  stage "$LDIR/$src" "::/LIB64/$dst"
  stage "$LDIR/$src" "::/lib/x86_64-linux-gnu/$dst"
done
stage "$LDIR/ld-2.31.so" "::/lib64/ld-linux-x86-64.so.2"
echo "   /LIB64 + /lib/x86_64-linux-gnu + /lib64/ld-linux-x86-64.so.2"

if [ -n "$JDK" ]; then
  say "JDK tree: $JDK -> ::/JDK"
  [ -x "$JDK/bin/java" ] || { echo "no $JDK/bin/java"; exit 2; }
  mkd ::/JDK
  # -s: recursive.  -m: preserve modification times (the image is FAT; the JVM
  # does not care, but a re-stage of an unchanged tree is then attributable).
  mcopy -s -o -i "$IMG" "$JDK"/* ::/JDK >/dev/null || {
    echo "mcopy -s of the JDK failed"; exit 2; }
  # The lane's working dirs.  /JDKOUT exists BEFORE javac runs so the javac
  # wave measures the compiler and not the (still missing) mkdir syscall;
  # /JDKTMP is where Verify's files-nio check writes.
  for d in ::/JDKOUT ::/JDKTMP; do mkd "$d"; done
fi

for d in "${DIRS[@]:-}"; do
  [ -n "$d" ] || continue
  src=${d%%:*}; dst=${d##*:}
  case "$dst" in ::*) ;; /*) dst="::$dst" ;; *) dst="::$dst" ;; esac
  say "tree $src -> $dst"
  mkd "$dst"
  # mcopy's -s means "recurse INTO these directories"; given plain files it
  # copies nothing at all and still exits 0 (measured: a flat /T landed empty
  # while the stage reported rc=0).  Use it only when there is a subdirectory
  # to descend into.
  if [ -n "$(find "$src" -mindepth 1 -type d -print -quit)" ]; then
    mcopy -s -o -i "$IMG" "$src"/* "$dst" >/dev/null || { echo "mcopy -s failed: $src"; exit 2; }
  else
    mcopy -o -i "$IMG" "$src"/* "$dst" >/dev/null || { echo "mcopy failed: $src"; exit 2; }
  fi
  echo "   $(mdir -i "$IMG" "$dst" | grep -c '^[A-Za-z._]') entries in $dst"
done

for p in "${PROBES[@]:-}"; do
  [ -n "$p" ] || continue
  src=${p%%:*}; dst=${p##*:}
  stage "$src" "::/$dst"
  echo "   probe ::/$dst ($(stat -f%z "$src" 2>/dev/null || stat -c%s "$src") bytes)"
done

if [ ${#WAVES[@]} -gt 0 ]; then
  say "wave manifest"
  CFG="$(mktemp "${TMPDIR:-/tmp}/wave.XXXXXX")"
  : > "$CFG"
  for w in "${WAVES[@]}"; do printf '%s\n' "$w" >> "$CFG"; done
  echo "--- ::/WAVE.CFG ---"; sed 's/^/   /' "$CFG"
  stage "$CFG" "::/WAVE.CFG"
  rm -f "$CFG"
fi

say "image"
mdir -i "$IMG" :: | tail -3 | sed 's/^/   /'
exit 0
