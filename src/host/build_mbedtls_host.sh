#!/bin/sh
# build_mbedtls_host.sh - host build + smoke for vendored mbedTLS 3.6.7 (L6).
#
# Usage (any cwd; paths are derived from the script location):
#   src/host/build_mbedtls_host.sh
#
# Recipe (browser.md AD-7; mbedTLS is curl's TLS backend on both browser
# tracks): default config, static libraries only, GNU make build.
#   1. verify third_party/mbedtls-3.6.7.tar.bz2 against its .sums
#   2. unpack the pinned tarball if the tree is missing (gitignored)
#   3. make lib            -> library/libmbed{crypto,x509,tls}.a
#   4. stage a prefix      -> obj/third_party/mbedtls-3.6.7/prefix/
#                             (include/mbedtls + include/psa + 3 .a files;
#                              skips the programs/tests tree)
#   5. build + run src/host/mbedtls_smoke.c against the prefix
#
# Overridable: CC, JOBS, REPO (repo root).  The HobbyOS cross build reuses
# this script with CC=<cross-cc>; no host-only flags are baked in.
set -eu

REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
CC=${CC:-cc}
JOBS=${JOBS:-4}
VER=mbedtls-3.6.7
SRC=$REPO/third_party/$VER
OUT=$REPO/obj/third_party/$VER
PREFIX=$OUT/prefix

# 1. checksum the pinned tarball, then unpack if the tree is absent
(cd "$REPO/third_party" && sha256sum -c "$VER.tar.bz2.sums")
if [ ! -d "$SRC" ]; then
  tar -xjf "$REPO/third_party/$VER.tar.bz2" -C "$REPO/third_party"
fi

# 2. static library build (default mbedTLS config; `make lib` only)
make -C "$SRC" -j"$JOBS" lib CC="$CC"

# 3. stage the prefix curl will be pointed at
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$PREFIX/include/"
cp "$SRC"/library/libmbedcrypto.a "$SRC"/library/libmbedx509.a \
   "$SRC"/library/libmbedtls.a "$PREFIX/lib/"

# 4. smoke: RNG + SHA-256 known-answer vectors
mkdir -p "$OUT"
$CC -O2 -Wall -Wextra -I"$PREFIX/include" "$REPO/src/host/mbedtls_smoke.c" \
  "$PREFIX/lib/libmbedtls.a" "$PREFIX/lib/libmbedx509.a" \
  "$PREFIX/lib/libmbedcrypto.a" -o "$OUT/mbedtls_smoke"
"$OUT/mbedtls_smoke"
