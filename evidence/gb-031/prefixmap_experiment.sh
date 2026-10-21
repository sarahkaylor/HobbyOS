#!/usr/bin/env bash
# GB-031 experiment: does -ffile-prefix-map make libSDL2.a dest-independent?
# Builds the same pinned tree at two different absolute paths and compares hashes.
set -u
TGZ=/tmp/sdl2-repro/SDL2-2.30.10.tar.gz
[ -f "$TGZ" ] || { echo "missing pinned tarball"; exit 1; }

build() {  # build <dest>
  local d="$1"
  rm -rf "$d"; mkdir -p "$d"
  tar xzf "$TGZ" -C "$d"
  cd "$d/SDL2-2.30.10" || exit 1
  CC="clang --target=x86_64-linux-musl" \
  CFLAGS="-O2 -ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=." \
    ./configure --host=x86_64-pc-linux-musl --disable-shared --enable-static \
      --disable-video-x11 --disable-video-wayland --disable-video-opengl \
      --disable-video-vulkan --disable-alsa --disable-pulseaudio --disable-audio \
      --disable-joystick --disable-haptic --disable-power --disable-filesystem \
      > configure.log 2>&1
  make -j"$(nproc)" > make.log 2>&1
  sha256sum build/.libs/libSDL2.a
}

echo "=== build at /tmp/sdlp1 ==="; build /tmp/sdlp1
echo "=== build at /tmp/sdlp2 ==="; build /tmp/sdlp2
