#!/usr/bin/env bash
# build-sdl2-static-musl.sh — GB-031 reproducible SDL2 static-musl build profile.
#
# Produces a statically-linked musl SDL2 library + a statically-linked SDL2
# hello app from a CLEAN tree, reproducibly: build it twice and the artifacts
# hash identically (the coordinator's gate: "a profile that is not reproducible
# is not a profile").  Pinned inputs only — version, archive SHA, exact
# configure/make invocation.  Nothing here reads the network beyond the pinned
# tarball fetch+hash-check, so it can be run on a fresh box.
#
# Usage:   tools/sdl/build-sdl2-static-musl.sh [dest_dir]
# Output:  <dest>/SDL2-2.30.10/build/.libs/libSDL2.a
#          <dest>/sdl2_static_test            (static musl ELF, links libSDL2.a)
# Prints the artifact SHA256 for the reproducibility record.
set -euo pipefail

SDL2_VER="2.30.10"
SDL2_TGZ="SDL2-${SDL2_VER}.tar.gz"
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VER}/${SDL2_TGZ}"
# Locked by the archive's SHA256 (reproducibility gate: same input, same hash).
SDL2_SHA256="f59adf36a0fcf4c94198e7d3d776c1b3824211ab7aeebeb31fe19836661196aa"

DEST="${1:-$(pwd)/sdl2-build}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$DEST/stage"

if [ ! -f "$DEST/$SDL2_TGZ" ]; then
  curl -fsSL -o "$DEST/$SDL2_TGZ" "$SDL2_URL"
fi
echo "$SDL2_SHA256  $DEST/$SDL2_TGZ" | sha256sum -c -

# Clean tree: always extract afresh so the upstream untouched sources are the
# only input (no leftovers from a prior configure can leak into the hash).
rm -rf "$DEST/stage/SDL2-$SDL2_VER"
tar xzf "$DEST/$SDL2_TGZ" -C "$DEST/stage"

cd "$DEST/stage/SDL2-$SDL2_VER"

# Static musl via clang's x86_64-linux-musl target.  Disable everything HobbyOS
# won't use; keep the Linux evdev input driver and the dummy/offscreen video
# drivers (headless-safe; /dev/fb0 fbcon video is the later HobbyOS integration,
# tracked separately in GB-031's remaining half).
CC="clang --target=x86_64-linux-musl" ./configure --host=x86_64-pc-linux-musl \
  --disable-shared --enable-static \
  --disable-video-x11 --disable-video-wayland --disable-video-opengl \
  --disable-video-vulkan --disable-alsa --disable-pulseaudio --disable-audio \
  --disable-joystick --disable-haptic --disable-power --disable-filesystem \
  > configure.log

make -j"$(nproc)" > make.log

echo
echo "== libSDL2.a =="
sha256sum build/.libs/libSDL2.a | tee "$DEST/libSDL2.a.sha256"

# Link the hello app as a static musl ELF.  -static gives a static link; the
# app must also link as non-PIE for the HobbyOS loader (checked by `file`).
clang --target=x86_64-linux-musl -static -no-pie \
  -I include "${SDL2_STATIC_TEST_SRC:-$SCRIPT_DIR/sdl2_static_test.c}" \
  build/.libs/libSDL2.a -lm -lpthread -ldl -o "$DEST/sdl2_static_test"

echo
echo "== sdl2_static_test =="
file "$DEST/sdl2_static_test"
sha256sum "$DEST/sdl2_static_test" | tee "$DEST/sdl2_static_test.sha256"
