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

**Cross-destination caveat (follow-on):** the archive carries the absolute
source path via DWARF, so two runs to *different* dest dirs embed different
paths and hash differently. The gate (clean tree, stable profile dir) is met;
fully dest-independent hashing is a strip/`-fdebug-prefix-map` follow-on,
tracked in GB-031's remaining half.

## Remaining halves (not claimed here)

- The HobbyOS **/dev/fb0 fbcon video driver** integration (SDL 2.30 dropped
  the legacy fbcon driver; needs a small shim/layer over the merged fb node —
  GB-025/026).
- The **loader-acceptance boot test** (static non-PIE ELF actually mapped by
  the running HobbyOS kernel) — needs the booted OS, tracked for the T3 tier.
