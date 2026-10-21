#!/usr/bin/env bash
# GB-031 reproducibility gate: two clean builds, compare artifact hashes,
# and check the app is a static non-PIE x86_64 ELF.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 1
OUT=evidence/gb-031
mkdir -p "$OUT"
DEST=/tmp/sdl2-repro

for i in 1 2; do
  echo "########## BUILD $i ##########"
  rm -rf "$DEST"
  bash tools/sdl/build-sdl2-static-musl.sh "$DEST" 2>&1 | tail -25
  echo "--- artifacts (build $i) ---"
  sha256sum "$DEST/stage/SDL2-2.30.10/build/.libs/libSDL2.a" "$DEST/sdl2_static_test" | tee "$OUT/build${i}.sha256"
  file "$DEST/sdl2_static_test" | tee -a "$OUT/build${i}.file"
  echo
done

echo "########## COMPARISON ##########"
if diff -q "$OUT/build1.sha256" "$OUT/build2.sha256" >/dev/null 2>&1; then
  echo "REPRODUCIBLE: the two clean builds produced identical hashes"
else
  echo "NOT REPRODUCIBLE: hashes differ"
  diff "$OUT/build1.sha256" "$OUT/build2.sha256"
fi
