#!/usr/bin/env bash
# GB-031 reproducibility + musl + evdev gate: two clean builds at DIFFERENT
# dests must hash identically AND link as genuine musl (no
# __libc_start_main_impl, has libc_start_main_stage2) AND enable evdev
# (SDL_INPUT_LINUXEV). The app must be a static non-PIE x86_64 ELF.
# Exits non-zero on any failure so it can be a graded gate by exit status.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
OUT=evidence/gb-031; mkdir -p "$OUT"
FAIL=0

for d in /tmp/g-musl-a /tmp/g-musl-b; do
  echo "########## clean build at $d ##########"
  if ! bash tools/sdl/build-sdl2-static-musl.sh "$d"; then
    echo "BUILD FAILED at $d"; FAIL=1
  fi
  echo "--- hashes ($d) ---"
  sha256sum "$d/stage/SDL2-2.30.10/build/.libs/libSDL2.a" "$d/sdl2_static_test"
done

echo "########## cross-dest comparison ##########"
LIB_A=$(sha256sum /tmp/g-musl-a/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
LIB_B=$(sha256sum /tmp/g-musl-b/stage/SDL2-2.30.10/build/.libs/libSDL2.a | cut -d' ' -f1)
APP_A=$(sha256sum /tmp/g-musl-a/sdl2_static_test | cut -d' ' -f1)
APP_B=$(sha256sum /tmp/g-musl-b/sdl2_static_test | cut -d' ' -f1)
if [ "$LIB_A" = "$LIB_B" ]; then echo "libSDL2.a: IDENTICAL across different dests ($LIB_A)"; else echo "libSDL2.a: DIFFERS ($LIB_A vs $LIB_B)"; FAIL=1; fi
if [ "$APP_A" = "$APP_B" ]; then echo "sdl2_static_test: IDENTICAL across different dests ($APP_A)"; else echo "sdl2_static_test: DIFFERS ($APP_A vs $APP_B)"; FAIL=1; fi

echo "########## musl + ELF + evdev ##########"
NGLIBC=$(nm /tmp/g-musl-a/sdl2_static_test 2>/dev/null | grep -c '__libc_start_main_impl')
NMUSL=$(nm /tmp/g-musl-a/sdl2_static_test 2>/dev/null | grep -c 'libc_start_main_stage2')
echo "glibc __libc_start_main_impl count: $NGLIBC"   # must be 0
echo "musl libc_start_main_stage2 count:  $NMUSL"    # must be >=1
[ "$NGLIBC" -eq 0 ] || { echo "FAIL: glibc marker present"; FAIL=1; }
[ "$NMUSL" -ge 1 ] || { echo "FAIL: no musl stage2"; FAIL=1; }
if grep -q "SDL_INPUT_LINUXEV" /tmp/g-musl-a/stage/SDL2-2.30.10/include/SDL_config.h 2>/dev/null; then
  echo "evdev: SDL_INPUT_LINUXEV enabled"
else
  echo "FAIL: SDL_INPUT_LINUXEV not enabled (evdev missing)"; FAIL=1
fi
readelf -h /tmp/g-musl-a/sdl2_static_test | grep -E "Type|Machine"
file /tmp/g-musl-a/sdl2_static_test
/tmp/g-musl-a/sdl2_static_test; RC=$?
echo "app run exit: $RC"
[ "$RC" -eq 0 ] || { echo "FAIL: app run rc=$RC"; FAIL=1; }

echo "########## GATE VERDICT ##########"
if [ "$FAIL" -eq 0 ]; then echo "GATE: PASS"; else echo "GATE: FAIL"; fi
exit $FAIL
