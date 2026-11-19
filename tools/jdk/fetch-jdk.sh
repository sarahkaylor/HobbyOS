#!/usr/bin/env bash
# tools/jdk/fetch-jdk.sh -- reproduce the exact runtime this lane boots.
#
# PIN: Eclipse Temurin **27.0.1.1+4** (Adoptium, linux/x64 hotspot).
#
# Why .1 and not 27.0.1: CVE-2026-114938 ("Java4Shell", disclosed 2026-11-17) is
# a heap out-of-bounds write in the x86-64 AVX-512 VBMI Base64 decode stub,
# StubGenerator::generate_base64_decodeBlock().  A padded tail shorter than one
# 4-character group gives output_size = 0, the padding branch decrements it to
# -1, BZHI leaves the store mask all ones, and the final evmovdqub writes 64
# bytes -- 62 of them past the decoded array -- before Java throws.  The OpenJDK
# advisory of 2026-11-17 lists 27.0.1.0.3 and earlier as affected; 27.0.1.1 is
# the fixed release.  Upstream fix: JDK-8393212, commit 5177a70b385c, which adds
#
#     __ cmpl(length, 4);
#     __ jcc(Assembler::less, L_bruteForce);
#
# before the vector tail, so a short padded tail is left to Java.  This lane
# used to pin 27.0.1.0.2+1, which is inside the affected range.
#
# Usage:
#   tools/jdk/fetch-jdk.sh                     # JDK -> $JDK_DEST, third_party/jdk-27 -> it
#   tools/jdk/fetch-jdk.sh --glibc             # ... and the glibc 2.31 pin stage.sh needs
#   JDK_DEST=... GLIBC231=... tools/jdk/fetch-jdk.sh
#
# Every download is checked against its published SHA-256 before it is unpacked
# or exec'd, and the script exits non-zero on mismatch: a runtime that is only
# probably the fixed one is not the fixed one.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

JDK_VER="27.0.1.1+4"
JDK_TARBALL="OpenJDK27U-jdk_x64_linux_hotspot_27.0.1.1_4.tar.gz"
JDK_SHA256="005e57ee860978acfeeff1be2b30f770664fe2b941e9b803c61c76992086d107"
JDK_URL="https://github.com/adoptium/temurin27-binaries/releases/download/jdk-27.0.1.1%2B4/${JDK_TARBALL}"

JDK_DEST="${JDK_DEST:-$HOME/.hermes/cache/scratch/jdk}"
PIN_DIR="${PIN_DIR:-$ROOT/obj/jdk/pins}"

# The glibc pin the compat layer is proven against, and the SHAs the gate
# scripts already use (tools/gate/m1_glibc_hello.sh).  Kept here so one command
# rebuilds everything stage.sh needs after a cache prune.
LIBC6_URL="http://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.31-0ubuntu9.18_amd64.deb"
LIBC6_SHA256="1b2281aac4935dfea1f89dfc19e445fdbb45303202af60679ccb8bf035f081a0"
LIBGCC_URL="http://archive.ubuntu.com/ubuntu/pool/main/g/gcc-10/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb"
LIBGCC_SHA256="4aa7b9c9f3225df65a750ae0ff5c890fc6108c82a609b3c5abd45d211838bf3c"
GLIBC231="${GLIBC231:-$HOME/.hermes/cache/scratch/glibc231}"

DO_GLIBC=0
while [ $# -gt 0 ]; do
  case "$1" in
    --glibc) DO_GLIBC=1; shift ;;
    *) echo "unknown arg $1"; exit 2 ;;
  esac
done

say() { printf '== %s\n' "$*"; }
sha_of() { shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'; }

# fetch PINNED url sha dest -- reuse a cached file only if it still matches.
fetch() {
  local url=$1 sha=$2 dest=$3
  mkdir -p "$(dirname "$dest")"
  if [ -f "$dest" ] && [ "$(sha_of "$dest")" = "$sha" ]; then
    echo "   cached  $(basename "$dest")"
    return 0
  fi
  echo "   fetch   $(basename "$dest")"
  curl -fsSL -o "$dest.part" "$url" || { echo "FETCH FAILED $url"; rm -f "$dest.part"; exit 2; }
  local got; got=$(sha_of "$dest.part")
  if [ "$got" != "$sha" ]; then
    echo "SHA256 MISMATCH $(basename "$dest")"
    echo "   want $sha"
    echo "   got  $got"
    rm -f "$dest.part"; exit 2
  fi
  mv "$dest.part" "$dest"
}

say "jdk $JDK_VER"
fetch "$JDK_URL" "$JDK_SHA256" "$PIN_DIR/$JDK_TARBALL"

TREE="$JDK_DEST/jdk-$JDK_VER"
if [ -f "$TREE/release" ] && grep -q "JAVA_VERSION=\"${JDK_VER%%+*}\"" "$TREE/release"; then
  echo "   tree    $TREE (already unpacked)"
else
  mkdir -p "$JDK_DEST"
  rm -rf "$JDK_DEST/jdk-$JDK_VER"
  tar xzf "$PIN_DIR/$JDK_TARBALL" -C "$JDK_DEST" || { echo "unpack failed"; exit 2; }
  echo "   tree    $TREE (unpacked)"
fi

# The lane (stage.sh, sdktest.sh, cycle.sh) all read third_party/jdk-27.
ln -sfn "$TREE" "$ROOT/third_party/jdk-27"
echo "   link    third_party/jdk-27 -> $TREE"

VER="$(grep '^JAVA_VERSION=' "$TREE/release" | cut -d'"' -f2)"
REL="$(grep '^JAVA_VERSION_DATE=' "$TREE/release" | cut -d'"' -f2)"
echo "   pinned  JAVA_VERSION=$VER  ($REL)"
# 27.0.1.0.x is the affected range: refuse to leave the lane on it silently.
case "$VER" in
  27.0.1|27.0.1.0) echo "   WARNING: $VER is in the CVE-2026-114938 affected range"; exit 3 ;;
esac
[ "$VER" = "${JDK_VER%%+*}" ] || { echo "   WARNING: expected ${JDK_VER%%+*}, got $VER"; exit 3; }

if [ "$DO_GLIBC" = 1 ]; then
  say "glibc 2.31 pin"
  fetch "$LIBC6_URL" "$LIBC6_SHA256" "$PIN_DIR/libc6_2.31-0ubuntu9.18_amd64.deb"
  fetch "$LIBGCC_URL" "$LIBGCC_SHA256" "$PIN_DIR/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb"
  rm -rf "$GLIBC231"; mkdir -p "$GLIBC231"
  dpkg-deb -x "$PIN_DIR/libc6_2.31-0ubuntu9.18_amd64.deb" "$GLIBC231" || exit 2
  dpkg-deb -x "$PIN_DIR/libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb" "$GLIBC231" || exit 2
  for f in ld-2.31.so libc-2.31.so libm-2.31.so libpthread-2.31.so libdl-2.31.so \
           librt-2.31.so libgcc_s.so.1; do
    [ -f "$GLIBC231/lib/x86_64-linux-gnu/$f" ] || { echo "pin is missing $f"; exit 2; }
  done
  echo "   pin     $GLIBC231 (7 runtime objects verified)"
fi

say "next"
echo "   tools/jdk/sdktest.sh          # 4 boots, the acceptance gate"
echo "   tools/jdk/README.md           # what each check means"
exit 0
