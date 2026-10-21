#!/usr/bin/env bash
# GB-031 gate after the prefix-map change: two clean builds at DIFFERENT dests
# must hash identically; the app must still be a static non-PIE ELF the loader accepts.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
OUT=evidence/gb-031; mkdir -p "$OUT"

for d in /tmp/sdl-a /tmp/sdl-b; do
  echo "########## clean build at $d ##########"
  rm -rf "$d"
  bash tools/sdl/build-sdl2-static-musl.sh "$d" 2>&1 | tail -6
  echo "--- hashes ---"
  sha256sum "$d/stage/SDL2-2.30.10/build/.libs/libSDL2.a" "$d/sdl2_static_test"
done

echo "########## cross-dest comparison ##########"
h1=$(sha256sum /tmp/sdl-a/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
h2=$(sha256sum /tmp/sdl-b/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
a1=$(sha256sum /tmp/sdl-a/sdl2_static_test | cut -d' ' -f1)
a2=$(sha256sum /tmp/sdl-b/sdl2_static_test | cut -d' ' -f1)
[ "$h1" = "$h2" ] && echo "libSDL2.a: IDENTICAL across different dests ($h1)" || echo "libSDL2.a: DIFFERS ($h1 vs $h2)"
[ "$a1" = "$a2" ] && echo "sdl2_static_test: IDENTICAL across different dests ($a1)" || echo "sdl2_static_test: DIFFERS ($a1 vs $a2)"

echo "########## ELF shape + loader acceptance ##########"
readelf -h /tmp/sdl-a/sdl2_static_test | grep -E "Type|Machine"
file /tmp/sdl-a/sdl2_static_test
ELF_REAL_BINARY=/tmp/sdl-a/sdl2_static_test ./elf_image_test_host | tail -3
