# SDL2 static-musl build profile (GB-031)

Reproducible recipe that produces a **static-musl SDL2** library and a
statically-linked SDL2 app that the HobbyOS compat loader can accept. Worker
wgfx; the coordinator's DoD: "a reproducible recipe that produces a static
musl SDL2 app, plus the profile recorded where M2 can cite it. Gate: a build
from a clean tree succeeds twice in a row with the same artifact hash, and the
app links as a static non-PIE ELF that the loader accepts."

*(Corrected at GB-031-V: the loader's contract is **static-PIE**, not static
non-PIE — an `ET_EXEC` below the user window is refused with "a -static-pie
build is required". The profile now links and asserts static-PIE.)*

## Toolchain — musl-gcc, not clang-musl (GB-031-V RED fix, 2026-10-25)

The verifier's RED (#166) caught the previous profile producing a **glibc**
artifact: `clang --target=x86_64-linux-musl` does not locate a musl sysroot on
this toolchain and silently falls back to the host `x86_64-linux-gnu-ld.bfd`,
so `libSDL2.a` carried `__libc_start_main_impl` (a glibc symbol). The profile
now drives **`musl-gcc`** (musl-tools) end to end and asserts, on the **linked
binary**, that it is genuine musl: `nm` shows musl's `libc_start_main_stage2`
and **no** `__libc_start_main_impl`.

**evdev (round-3 fix):** switching to musl-gcc exposed that its default search
path omits the kernel UAPI under `/usr/include`, so SDL configured
`SDL_INPUT_LINUXEV` OFF. The configure CFLAGS now add
`-idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu` (idirafter,
not -I, so musl's own headers still win over glibc's `features.h`), which lets
configure find `linux/input.h`; the script asserts `SDL_INPUT_LINUXEV` appears
in the generated `include/SDL_config.h`.

## Recipe

```
tools/sdl/build-sdl2-static-musl.sh <dest_dir>
```

Pins SDL2 `release-2.30.10` (archive SHA256 `f59adf36...661196aa`), configures
with `CC=musl-gcc --host=x86_64-pc-linux-musl`, `--disable-shared
--enable-static`, enabling the Linux **evdev** input driver and the
dummy/offscreen video drivers; disables X11/wayland/GL/audio/joystick (not
needed on HobbyOS). Outputs `<dest>/build/.libs/libSDL2.a` and a **static-PIE**
musl `sdl2_static_test` that initialises video, creates a 64×48 window, fills
it and updates the surface.

## Reproducibility (the gate) — cross-destination

The script passes `-ffile-prefix-map=$PWD=. -fdebug-prefix-map=$PWD=.` to both
configure and the app link, so the artifact hash depends on inputs only, not on
where the tree was unpacked. Two clean builds at **different** dest dirs hash
identically:

```
(libSDL2.a and sdl2_static_test sha256 — recorded from the gate run below)
libSDL2.a         sha256 = 141e47ccb2a54914141f43d59304f88ba65490dbf5b0420bf9a87b718a2ba35a  (dest A)
libSDL2.a         sha256 = 141e47ccb2a54914141f43d59304f88ba65490dbf5b0420bf9a87b718a2ba35a  (dest B, identical)
sdl2_static_test  sha256 = 593874048a2b2a3c706ae84e12180bd5b982d8de5c34c249b9807bb55a46a44f  (dest A)
sdl2_static_test  sha256 = 593874048a2b2a3c706ae84e12180bd5b982d8de5c34c249b9807bb55a46a44f  (dest B, identical)
```

`file` on the linked app: `ELF 64-bit LSB pie executable, x86-64, static-pie linked`.
`nm` shows musl's `libc_start_main_stage2` and **no** `__libc_start_main_impl`.

The gate **asserts**, rather than prints, the ELF class the loader needs — a PIE
or dynamically-linked artifact is a failure on exit status, not a line in a log:

```
readelf -h <app> | grep -q 'Type:.*DYN'         # static-PIE (loader contract)
readelf -h <app> | grep -q 'Machine:.*X86-64'
readelf -l <app> | grep -q 'INTERP'  -> FAIL    # no PT_INTERP (not dynamic)
```

The cross-dest comparison also fails if either build produced no artifact, so
two missing files can never compare "IDENTICAL".

## Remaining halves (not claimed here)

- **/dev/fb0 fbcon video driver integration.** SDL 2.30 dropped the legacy
  fbcon driver, so this profile builds the dummy/offscreen video drivers only —
  the on-screen driver is a small shim over the merged fb node (GB-025/026),
  tracked separately. As the verifier noted, "fbcon" is not a claim this
  profile's build carries; it is a follow-on half.
- The **loader-acceptance boot test** (static-PIE ELF actually mapped by the
  running HobbyOS kernel) — needs the booted OS, tracked for the T3 tier. The
  vehicle now dispatches at `abi=0x1` and reaches `SDL_Init(SDL_INIT_VIDEO)`;
  it currently parks on the compat `futex(202)`/`clone(56)` gap, tracked on
  GB-031.
