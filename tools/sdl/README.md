# SDL2 static-musl build profile (GB-031)

Reproducible recipe that produces a **static-musl SDL2** library and a
statically-linked SDL2 app that the HobbyOS compat loader can accept. Worker
wgfx; the coordinator's DoD: "a reproducible recipe that produces a static
musl SDL2 app, plus the profile recorded where M2 can cite it. Gate: a build
from a clean tree succeeds twice in a row with the same artifact hash, and the
app links as a static non-PIE ELF that the loader accepts."

## Recipe

```
tools/sdl/build-sdl2-static-musl.sh <dest_dir>
```

Pins SDL2 `release-2.30.10` (archive SHA256
`f59adf36...661196aa`) and builds with `clang --target=x86_64-linux-musl`,
`--disable-shared --enable-static`, enabling the Linux **evdev** input driver
and the dummy/offscreen video drivers; disables X11/wayland/GL/audio/joystick
(not needed on HobbyOS). Outputs `<dest>/build/.libs/libSDL2.a` and a static
musl `sdl2_static_test` ELF.

## Reproducibility (the gate)

Two clean rebuilds (fresh extraction each run, `rm -rf` of the stage) to the
same canonical build path hash identically:

```
libSDL2.a  sha256 = 54827f3bec3c24733dfec0849eaf91541ac5319d00348a24141c9a38613aed0d  (run 1)
libSDL2.a  sha256 = 54827f3bec3c24733dfec0849eaf91541ac5319d00348a24141c9a38613aed0d  (run 2)
```
`file` on the linked app: `ELF 64-bit LSB executable, x86-64, statically
linked`.

**Cross-destination caveat — FIXED 2026-10-21:** the archive carried the absolute
source path via DWARF, so runs to different dest dirs hashed differently. The
script now passes `CFLAGS="-O2 -ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=."`
to configure (and the same flags to the app link), which removes the path from the
embedded DWARF. Measured after the change: two clean builds at **different** dests
(`/tmp/sdl-a`, `/tmp/sdl-b`) produce `libSDL2.a = 82e72c09…` and
`sdl2_static_test = c9f6c351…` — identical, so the hash is now citable across
machines rather than only within one directory.

## Measured on this VM (2026-10-21, DEEP verifier under the debug reserve)

Environment: `Ubuntu clang version 21.1.8 (6ubuntu1)`, `Linux 7.0.0-38-generic`, x86_64.
Command (twice, `rm -rf` of the dest between runs — a clean tree each time):

```
DEST=/tmp/sdl2-repro
rm -rf $DEST && tools/sdl/build-sdl2-static-musl.sh $DEST
```

```
libSDL2.a        sha256 82e72c095e0f4ee773dfcc5313284f2a8e726faf3d6f543596b65f2ac0428d53   (both runs, both dests)
sdl2_static_test sha256 c9f6c35126ebf9b533cffabd120c0d4a354340314212e559b837ac763f4a77ab   (both runs, both dests)
```

Reproducibility verdict: **identical hashes across two clean builds at two different dest dirs**
(the pre-fix profile only reproduced within one dest — see the caveat above, now fixed).

ELF shape (`readelf -h`): `Type: EXEC (Executable file)`, `Machine: … X86-64` — static, **non-PIE**;
`file` reports `statically linked`. It runs on the Linux host (`SDL_VIDEODRIVER=dummy`, exit 0).

Loader acceptance, measured with the host ELF-image validator on the exact artifact:

```
$ ELF_REAL_BINARY=/tmp/sdl2-repro/sdl2_static_test ./elf_image_test_host
  PASS real binary /tmp/sdl2-repro/sdl2_static_test -> ok (entry 0x401ac0, 4 segs)
  PASS real binary plan: 4 region(s), span 0x26b000, first off 0x0 fl 0x4
=== 84 checks, 0 failed ===
ALL ELF IMAGE HOST TESTS PASSED
```

**The hash recorded further up does not reproduce here** (`54827f3b…` vs `82e72c09…`) — it was taken
before the prefix-map fix, on a tree unpacked at a different path. After the fix the value depends only
on inputs, so this section's numbers are the ones to cite.

Cross-destination check (the stronger form of the gate), after the fix:

```
$ bash tools/sdl/build-sdl2-static-musl.sh /tmp/sdl-a   # clean build
$ bash tools/sdl/build-sdl2-static-musl.sh /tmp/sdl-b   # clean build, different dest
libSDL2.a:        IDENTICAL across different dests (82e72c095e0f4ee773dfcc5313284f2a8e726faf3d6f543596b65f2ac0428d53)
sdl2_static_test: IDENTICAL across different dests (c9f6c35126ebf9b533cffabd120c0d4a354340314212e559b837ac763f4a77ab)
```

## Remaining halves (not claimed here)

- The HobbyOS **/dev/fb0 fbcon video driver** integration (SDL 2.30 dropped
  the legacy fbcon driver; needs a small shim/layer over the merged fb node —
  GB-025/026).
- The **loader-acceptance boot test** (static non-PIE ELF actually mapped by
  the running HobbyOS kernel) — needs the booted OS, tracked for the T3 tier.
