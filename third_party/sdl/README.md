# third_party/sdl — the SDL test app's prebuilt Linux/binaries cache

`cache/sdlapp-x86_64-musl.elf` is the **musl static-PIE** build of
`tools/sdl/sdl_test_app.c` (the `SDLAPP.BIN` fixture the GB-013 loader maps).
It exists for hosts that have no musl C compiler at all — macOS — where
`make run` still has to stage `SDLAPP.BIN` onto `disk.img`.

## Why a cached artifact and not a build

GB-137 (2026-11-01) fixed a genuine loader bug hunt by proving the fixture must
be a real musl static-PIE: a glibc static-PIE built by
`clang --target=x86_64-linux-musl` walks `DT_RELA` at link-time addresses and
faults at `VA=0xCF8` in the v2 address space.  So the staged binary may never
silently be a glibc build.

Linux satisfies that rule with `musl-gcc` (`apt install musl-tools`).  macOS
has no `musl-gcc`, and none of the Homebrew musl cross toolchains can stand in
for it, because `-static-pie` needs a **PIC** `libc.a`:

| macOS route | result |
|---|---|
| `messense/macos-cross-toolchains` `x86_64-unknown-linux-musl-gcc` | `libc.a` is `--disable-shared` (non-PIC) → `relocation R_X86_64_32 ... can not be used when making a PIE object` |
| `FiloSottile/musl-cross` `x86_64-linux-musl-gcc` | same failure (static-only musl) |
| `zig cc -target x86_64-linux-musl` | links, but the image carries `PT_TLS` + `R_X86_64_TPOFF64`; the loader refuses unknown relocation types by name (GB-013) |

Building a PIC musl on macOS is possible but needs a cross gcc *plus* a musl
source build, which is a heavier dependency than the fixture is worth.  The
cache is the same pattern the tree already uses for the host-limited WebProcess
build (`third_party/webkit-hobbyos/cache/browser-*.bin.xz`, committed,
resolved by `browser-staged`).

## Provenance

Built on the Kubuntu dev VM (x86_64 Linux, `musl-gcc` from `musl-tools`) at
commit `dc736206` with exactly the Makefile recipe:

```bash
musl-gcc -fPIE -static-pie -Isrc/include tools/sdl/sdl_test_app.c \
    -o third_party/sdl/cache/sdlapp-x86_64-musl.elf
```

Verified with the tree's own gate on Linux — `sdl_test_app_test_host` reports
`20 checks, 0 failed` — and the ELF properties GB-137 measured:

```
Type: DYN (Position-Independent Executable file)   # ET_DYN, no PT_INTERP
11 R_X86_64_RELATIVE relocations
4 PT_LOAD segments (R, R E, R, RW)
size 51,616 bytes   sha256 da2a44424c3dd9a49713a8d6aa6f2faae82a1202e0e25ad7754d3f5c884e46cf
```

## Regenerating

Any change to `tools/sdl/sdl_test_app.c` or its headers makes the cache stale;
`tools/sdl/build_sdl_pie.sh` refuses a stale cache by name rather than staging
it.  To refresh, on a host with `musl-gcc`:

```bash
make obj/sdl_test_app_pie                       # builds from source there
cp obj/sdl_test_app_pie third_party/sdl/cache/sdlapp-x86_64-musl.elf
bash tools/sdl/build_sdl_pie.sh --record        # refreshes .sha256 + .deps
./sdl_test_app_test_host                        # 20 checks, 0 failed
```

The stage path (`tools/sdl/build_sdl_pie.sh`) refuses a cache whose recorded
source hashes no longer match, so an edited `sdl_test_app.c` cannot silently
ship the old fixture to a macOS build.
