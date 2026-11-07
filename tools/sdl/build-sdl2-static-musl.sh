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

# Static musl via musl-gcc.  NOTE: clang --target=x86_64-linux-musl does NOT
# find a musl sysroot here and silently links glibc (__libc_start_main_impl),
# so the profile must drive a real musl toolchain.  musl-gcc (musl-tools) uses
# the musl-gcc.specs to link against musl libc proper.  Disable everything
# HobbyOS won't use; keep the Linux evdev input driver and the dummy/offscreen
# video drivers (headless-safe; /dev/fb0 fbcon video is the later HobbyOS
# integration, tracked separately in GB-031's remaining half).
CC=musl-gcc ./configure --host=x86_64-pc-linux-musl \
  --disable-shared --enable-static \
  --disable-video-x11 --disable-video-wayland --disable-video-opengl \
  --disable-video-vulkan --disable-alsa --disable-pulseaudio --disable-audio \
  --disable-joystick --disable-haptic --disable-power --disable-filesystem \
  CFLAGS="-O2 -ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=. -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu" \
  > configure.log

make -j"$(nproc)" > make.log

echo
echo "== libSDL2.a =="
sha256sum build/.libs/libSDL2.a | tee "$DEST/libSDL2.a.sha256"

# Link the test app as a static-PIE musl ELF.  The HobbyOS v2 loader requires
# static-PIE: a static non-PIE ET_EXEC linked at 0x400000 is refused with
# "ET_EXEC linked below the user window; a -static-pie build is required"
# (GB-031-V measured, docs/elf-loader.md §1).
musl-gcc -fPIE -static-pie \
  -ffile-prefix-map="$PWD"=. -fdebug-prefix-map="$PWD"=. \
  -I include "${SDL2_STATIC_TEST_SRC:-$SCRIPT_DIR/sdl2_static_test.c}" \
  build/.libs/libSDL2.a -lm -lpthread -ldl -o "$DEST/sdl2_static_test"

# Assert the artifact shape the loader actually accepts: static-PIE (ET_DYN)
# with no PT_INTERP.  Printing the type is not asserting it (GB-024's lesson).
if ! readelf -h "$DEST/sdl2_static_test" 2>/dev/null | grep -q 'Type:.*DYN'; then
  echo "FATAL: linked app is not ET_DYN (static-PIE) — the v2 loader refuses ET_EXEC below the window" >&2
  exit 1
fi
if readelf -l "$DEST/sdl2_static_test" 2>/dev/null | grep -q 'INTERP'; then
  echo "FATAL: linked app has PT_INTERP (dynamically linked) — not a static-PIE" >&2
  exit 1
fi

# Assert the artifact is genuinely musl on the LINKED binary (the SDL archive
# is libc-agnostic; the libc markers appear only once musl's libc.a is pulled
# in at link).  musl shows libc_start_main_stage2 and never the glibc-only
# __libc_start_main_impl (which clang-musl produced when it fell back to glibc).
# Use "grep -c > 0" rather than "grep -q": with set -o pipefail, grep -q exits
# on its first match and SIGPIPEs its producer (nm on a large binary), which
# the pipeline reports as failure and would spuriously trip the !-check.
if [ "$(nm "$DEST/sdl2_static_test" 2>/dev/null | grep -c '__libc_start_main_impl')" -gt 0 ]; then
  echo "FATAL: linked app is glibc (__libc_start_main_impl) — musl toolchain not used" >&2
  exit 1
fi
if [ "$(nm "$DEST/sdl2_static_test" 2>/dev/null | grep -c 'libc_start_main_stage2')" -eq 0 ]; then
  echo "FATAL: linked app shows no musl libc_start_main_stage2" >&2
  exit 1
fi

# Assert the Linux evdev input driver is enabled.  SDL's configure writes
# SDL_INPUT_LINUXEV into include/SDL_config.h when it finds linux/input.h.
# musl-gcc's default search path omits the kernel UAPI under /usr/include, so
# the -idirafter flags above are what let it find the header; assert it took.
if ! grep -q "SDL_INPUT_LINUXEV" include/SDL_config.h 2>/dev/null; then
  echo "FATAL: SDL_INPUT_LINUXEV not enabled — evdev input driver missing" >&2
  exit 1
fi

echo
echo "== sdl2_static_test =="
file "$DEST/sdl2_static_test"
sha256sum "$DEST/sdl2_static_test" | tee "$DEST/sdl2_static_test.sha256"
