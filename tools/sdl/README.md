# SDL2 static-musl build profile (GB-031)

Reproducible recipe that produces a **static-musl SDL2** library and a
statically-linked SDL2 app that the HobbyOS compat loader can accept. Worker
wgfx; the coordinator's DoD: "a reproducible recipe that produces a static
musl SDL2 app, plus the profile recorded where M2 can cite it. Gate: a build
from a clean tree succeeds twice in a row with the same artifact hash, and the
app links as a static non-PIE ELF that the loader accepts."

## Toolchain — musl-gcc, not clang-musl (GB-031-V RED fix, 2026-10-25)

The verifier's RED (#166) caught the previous profile producing a **glibc**
artifact: `clang --target=x86_64-linux-musl` does not locate a musl sysroot on
this toolchain and silently falls back to the host `x86_64-linux-gnu-ld.bfd`,
so `libSDL2.a` carried `__libc_start_main_impl` (a glibc symbol). The profile
now drives **`musl-gcc`** (musl-tools) end to end and **asserts musl in the
build**: the script fails if `strings libSDL2.a` lacks `musl` or if
`nm libSDL2.a` shows `__libc_start_main_impl`.

## Recipe

```
tools/sdl/build-sdl2-static-musl.sh <dest_dir>
```

Pins SDL2 `release-2.30.10` (archive SHA256 `f59adf36...661196aa`), configures
with `CC=musl-gcc --host=x86_64-pc-linux-musl`, `--disable-shared
--enable-static`, enabling the Linux **evdev** input driver and the
dummy/offscreen video drivers; disables X11/wayland/GL/audio/joystick (not
needed on HobbyOS). Outputs `<dest>/build/.libs/libSDL2.a` and a static musl
`sdl2_static_test` ELF.

## Reproducibility (the gate) — cross-destination

The script passes `-ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=.` to both
configure and the app link, so the artifact hash depends on inputs only, not on
where the tree was unpacked. Two clean builds at **different** dest dirs hash
identically:

```
(libSDL2.a and sdl2_static_test sha256 — recorded from the gate run below)
libSDL2.a         sha256 = e09876b411799963a8db82413bf9579168c20d53108751532e74bb9e3540db3d  (dest A)
libSDL2.a         sha256 = e09876b411799963a8db82413bf9579168c20d53108751532e74bb9e3540db3d  (dest B, identical)
sdl2_static_test  sha256 = 6674931d12d1f2ccdeea5994cce976cb82113ee9ce8f1f6352a8c49aafca8141  (dest A)
sdl2_static_test  sha256 = 6674931d12d1f2ccdeea5994cce976cb82113ee9ce8f1f6352a8c49aafca8141  (dest B, identical)
```

`file` on the linked app: `ELF 64-bit LSB executable, x86-64, statically linked`.
`nm` shows musl's `libc_start_main_stage2` and **no** `__libc_start_main_impl`.

## Remaining halves (not claimed here)

- **/dev/fb0 fbcon video driver integration.** SDL 2.30 dropped the legacy
  fbcon driver, so this profile builds the dummy/offscreen video drivers only —
  the on-screen driver is a small shim over the merged fb node (GB-025/026),
  tracked separately. As the verifier noted, "fbcon" is not a claim this
  profile's build carries; it is a follow-on half.
- The **loader-acceptance boot test** (static non-PIE ELF actually mapped by
  the running HobbyOS kernel) — needs the booted OS, tracked for the T3 tier.
