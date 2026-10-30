#!/usr/bin/env bash
# Build the patched QEMU that `make run` uses for the x64 GL display path.
# See tools/qemu-glfix/README.md for why stock QEMU 10.2.1 is not usable there.
#
#   tools/qemu-glfix/build.sh            # build + install into $PREFIX
#   PREFIX=/elsewhere tools/qemu-glfix/build.sh
#
# Linux only.  Needs the GTK/epoxy/virglrenderer/GBM/slirp/pixman/glib
# development headers (see README.md), meson and ninja.
set -euo pipefail

VERSION=10.2.1
SHA256=a3717477d8e2c84d630bfffbc20f6cd3293eb45aa1e6dac6d0cc27689991c9e1
URL="https://download.qemu.org/qemu-${VERSION}.tar.xz"

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${QEMU_GLFIX_ROOT:-$HOME/.local/share/hobbyos-tools/qemu-glfix}
PREFIX=${PREFIX:-$ROOT/prefix}
JOBS=${JOBS:-$(nproc)}

mkdir -p "$ROOT/dl" "$ROOT/src"
TARBALL="$ROOT/dl/qemu-${VERSION}.tar.xz"
if [ ! -f "$TARBALL" ]; then
  curl -fL --retry 3 -o "$TARBALL.part" "$URL"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "${SHA256}  ${TARBALL}" | sha256sum -c -

SRC="$ROOT/src/qemu-${VERSION}"
rm -rf "$SRC"
tar -C "$ROOT/src" -xf "$TARBALL"
for p in "$HERE"/*.patch; do
  echo "== applying $(basename "$p")"
  patch -d "$SRC" -p1 < "$p"
done

mkdir -p "$SRC/build"
cd "$SRC/build"
../configure --prefix="$PREFIX" \
  --target-list=x86_64-softmmu,aarch64-softmmu \
  --with-pkgversion=hobbyos-glfix \
  --enable-gtk --enable-opengl --enable-virglrenderer --enable-slirp --enable-kvm \
  --disable-sdl --disable-docs --disable-werror
ninja -j"$JOBS"
ninja install

echo "== installed: $("$PREFIX/bin/qemu-system-x86_64" --version | head -1)"
echo "== make run (ARCH=intel, desktop) now picks $PREFIX/bin/qemu-system-x86_64 for the GL path"
