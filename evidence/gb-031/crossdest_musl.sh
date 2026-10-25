#!/usr/bin/env bash
# GB-031 reproducibility + musl gate: two clean builds at DIFFERENT dests must
# hash identically AND link as genuine musl (no __libc_start_main_impl, has
# libc_start_main_stage2), and the app must be a static non-PIE x86_64 ELF.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
OUT=evidence/gb-031; mkdir -p "$OUT"

for d in /tmp/g-musl-a /tmp/g-musl-b; do
  echo "########## clean build at $d ##########"
  bash tools/sdl/build-sdl2-static-musl.sh "$d" >&2   # script prints its own tail
  echo "--- hashes ($d) ---"
  sha256sum "$d/stage/SDL2-2.30.10/build/.libs/libSDL2.a" "$d/sdl2_static_test"
done

echo "########## cross-dest comparison ##########"
LIB_A=$(sha256sum /tmp/g-musl-a/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
LIB_B=$(sha256sum /tmp/g-musl-b/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
APP_A=$(sha256sum /tmp/g-musl-a/sdl2_static_test | cut -d' ' -f1)
APP_B=$(sha256sum /tmp/g-musl-b/sdl2_static_test | cut -d' ' -f1)
[ "$LIB_A" = "$LIB_B" ] && echo "libSDL2.a: IDENTICAL across different dests ($LIB_A)" || echo "libSDL2.a: DIFFERS ($LIB_A vs $LIB_B)"
[ "$APP_A" = "$APP_B" ] && echo "sdl2_static_test: IDENTICAL across different dests ($APP_A)" || echo "sdl2_static_test: DIFFERS ($APP_A vs $APP_B)"

echo "########## musl + ELF shape ##########"
echo "glibc __libc_start_main_impl count: $(nm /tmp/g-musl-a/sdl2_static_test 2>/dev/null | grep -c '__libc_start_main_impl')"   # must be 0
echo "musl libc_start_main_stage2 count:  $(nm /tmp/g-musl-a/sdl2_static_test 2>/dev/null | grep -c 'libc_start_main_stage2')" # must be >=1
readelf -h /tmp/g-musl-a/sdl2_static_test | grep -E "Type|Machine"
file /tmp/g-musl-a/sdl2_static_test
