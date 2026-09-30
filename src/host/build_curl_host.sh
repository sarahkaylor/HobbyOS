#!/bin/sh
# build_curl_host.sh - host build + smoke for vendored libcurl 8.22.0 (L6).
#
# Usage (any cwd; paths are derived from the script location):
#   src/host/build_curl_host.sh
#
# Recipe (browser.md AD-7): static libcurl, mbedTLS TLS backend, zlib,
# lean protocol set (http + https + file), everything else disabled.
#   1. verify third_party/curl-8.22.0.tar.xz against its .sums
#   2. unpack the pinned tarball if the tree is missing (gitignored)
#   3. build the mbedTLS prefix first if absent (build_mbedtls_host.sh)
#   4. configure (see FLAGS below; mbedTLS via --with-mbedtls=<prefix>)
#   5. make + make install -> obj/third_party/curl-8.22.0/prefix/
#   6. build + run src/host/curl_smoke.c against the prefix
#
# Overridable: CC, JOBS, REPO.  Target-build deltas (documented in the
# README, not applied here): --with-ca-bundle=/CERTS/CA.PEM,
# --disable-threaded-resolver (until P1 pthreads land) and any further
# feature trims.
set -eu

REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
CC=${CC:-cc}
JOBS=${JOBS:-4}
VER=curl-8.22.0
SRC=$REPO/third_party/$VER
OUT=$REPO/obj/third_party/$VER
PREFIX=$OUT/prefix
MBEDTLS_PREFIX=$REPO/obj/third_party/mbedtls-3.6.7/prefix

# 1. checksum the pinned tarball, then unpack if the tree is absent
(cd "$REPO/third_party" && sha256sum -c "$VER.tar.xz.sums")
if [ ! -d "$SRC" ]; then
  tar -xJf "$REPO/third_party/$VER.tar.xz" -C "$REPO/third_party"
fi

# 2. mbedTLS must be built first (it is curl's TLS backend)
if [ ! -f "$MBEDTLS_PREFIX/lib/libmbedtls.a" ]; then
  sh "$REPO/src/host/build_mbedtls_host.sh"
fi

# 3. configure + build + install
mkdir -p "$OUT/build"
cd "$OUT/build"
"$SRC/configure" \
  --prefix="$PREFIX" \
  --disable-shared --enable-static \
  --with-mbedtls="$MBEDTLS_PREFIX" \
  --with-zlib \
  --without-brotli --without-zstd \
  --without-libidn2 --without-libpsl \
  --without-libssh2 --without-libssh \
  --without-nghttp2 --without-nghttp3 --without-ngtcp2 \
  --without-quiche \
  --disable-ldap --disable-ldaps \
  --disable-ftp --disable-rtsp --disable-dict --disable-telnet \
  --disable-tftp --disable-pop3 --disable-imap --disable-smtp \
  --disable-gopher --disable-mqtt --disable-smb --disable-ipfs \
  --disable-websockets \
  --disable-unix-sockets \
  --disable-manual --disable-docs \
  --enable-http --enable-file
make -j"$JOBS"
make install

# 4. smoke: capability check + one HTTPS HEAD request
$CC -O2 -Wall -Wextra -I"$PREFIX/include" "$REPO/src/host/curl_smoke.c" \
  "$PREFIX/lib/libcurl.a" \
  "$MBEDTLS_PREFIX/lib/libmbedtls.a" "$MBEDTLS_PREFIX/lib/libmbedx509.a" \
  "$MBEDTLS_PREFIX/lib/libmbedcrypto.a" \
  -lz -lpthread -o "$OUT/curl_smoke"
"$OUT/curl_smoke"
