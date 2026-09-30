# L5 font stack — host-first builds + deterministic smoke tests

Lane L5 ("fonts/images", `browser.md` §7.1/§7.5) first deliverable: vendor and
host-build the FreeType + HarfBuzz stack, vendor the DejaVu assets, and prove
the exact API shape the HobbyOS font backend will use with byte-for-byte
reproducible smoke tests. Everything here is **host-first** — no HobbyOS/QEMU
build is involved.

Vendored-pin READMEs (provenance, extraction, exact build recipes):

- `third_party/freetype-2.14.3.README.md`
- `third_party/harfbuzz-14.5.0.README.md`
- `third_party/fonts/README.md` (DejaVu 2.37)

## Run it (one command, idempotent, rootless)

```sh
./src/user/browser/fonts/host_build_fonts.sh
```

The script: verifies every pinned tarball against the committed `.sha256`
files, extracts what is missing (gitignored), host-builds FreeType 2.14.3 and
HarfBuzz 14.5.0 (+FreeType +ICU), compiles both smokes, runs each **twice**
and diffs — the smoke gate. All build trees and logs live under `obj/`
(gitignored). Env overrides: `OBJ=…` (build root), `ICU_DEV_ROOT=…` (ICU dev
prefix), `CC` / `CXX`, `JOBS` (default 4).

Prerequisites: `cc`/`g++` (system gcc 15.2), cmake >= 3.20 + ninja (rootless
install documented in `third_party/harfbuzz-14.5.0.README.md`), and the
rootless ICU dev extraction (`libicu-dev` deb; recipe in the same README).

## The two smokes

### `ft_memface_smoke.c` — memory-face loader + glyph renderer

- Reads the TTF into a caller-owned buffer and creates the face with
  `FT_New_Memory_Face()` — FreeType never opens a filesystem path (no
  `FT_New_Face`, no file name in `FT_Open_Face`). This is the shape the
  HobbyOS font backend needs; on-device the buffer comes from the disk layer.
- Renders `H`, `g`, `8` at 12/16/24/32 pixel sizes (hinted grayscale), prints
  the metrics per render and an FNV-1a 64 checksum of each bitmap, and writes
  the raw bitmap bytes to `ft_bitmap_dump.bin` (fixed order: size-major,
  glyph-minor) for external sha256 comparison.

### `hb_shape_smoke.c` — shaping, two configurations

- `HB-OT` (design units at upem scale) and `HB-FT` (hb-ft funcs over a
  FreeType memory face, 16 px em) shape `"Hello, HobbyOS!"` and `"AVATAR To"`;
  prints every glyph id / cluster / advance / offset plus FNV-1a 64 checksums
  over the glyph-id array and the position array.
- Cross-checks that both configurations produce identical glyph ids.
- Calls `hb_icu_get_unicode_funcs()` — closes the harfbuzz-ICU linkage loop
  (the `COMPONENTS ICU` WebKit's FindHarfBuzz requires).

## Recorded results (smoke gate — stable across runs AND builds)

Executed 2026-09-30; identical values from three independent executions
(probe run, in-tree run, cold-build run — the cold run rebuilt FreeType and
HarfBuzz from source into a fresh `obj/third_party-cold/`):

| Output | Value |
|---|---|
| FT-TOTAL | `renders=12 dump_bytes=2482 hash=4e88e851d05bd62e` |
| ft bitmap dump sha256 | `70c8407d9f63031811895991cbac18d05caac06d5f86ac73d583fc55a7f97cd8` |
| HB-TOTAL | `strings=2 ot_gids=26e142c5aa61672a ot_pos=4d4686aebec0ff59 ft_gids=26e142c5aa61672a ft_pos=24f120e747d7cb9f` |
| Font used | `DejaVuSans.ttf` sha256 `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954` |

Pairing sanity from the recorded output: HB-OT `H` advance `1540` at the
2048 upem = 12.03 px, HB-FT `770/1024` = 12.03 px at the 16 px em — the two
scales agree (see `obj/third_party/fonts/smoke/run1/hb_stdout.txt`).

## Evidence (absolute paths; worktree `~/hobbyos-lanes/l5-fonts`)

- Build logs: `obj/third_party/logs/` — `tarball-verify.log`,
  `freetype-configure.log`, `freetype-make.log`, `freetype-install.log`,
  `harfbuzz-configure.log`, `harfbuzz-build.log`, `harfbuzz-install.log`,
  `smoke-build-ft.log`, `smoke-build-hb-c.log`, `smoke-build-hb-link.log`,
  `smoke-diff-ft.log`, `smoke-diff-hb.log`, `dejavu-manifest-check.log`,
  `artifact-sha256.log` (0 warnings in every smoke/build log; the single
  FreeType `configure` WARNING is the benign `make refdoc`/`docwriter` notice)
- Warm runs: `obj/third_party/fonts/smoke/run1/` + `run2/`
  (`ft_stdout.txt`, `hb_stdout.txt`, `ft_bitmap_dump.bin`)
- Cold re-run (full rebuild proof): `obj/third_party-cold/logs/` and
  `obj/third_party-cold/fonts/smoke/run1/` + `run2/`

## Boundaries (explicit; see also browser.md)

- **ICU is host-only here.** The on-device ICU port + data trim is L6's item
  (browser.md §6 L6; §10 Q3 open); harfbuzz-ICU on target waits for L6's
  cross-built ICU. The host linkage itself is done and proven
  (`hb_icu_get_unicode_funcs()` exercised by the smoke).
- **Cross-compile is not done in this lane.** These are host builds per the
  lane rule ("host-first"); the HobbyOS cross build reuses these recipes as
  the host leg of the two-build rule (browser.md §8.1 item 5).
- **Makefile wiring not touched** (F6: Integrator only). Proposal for I:
  add a `fonts_host_smoke` target (or a `host_tests` dependency) that runs
  `host_build_fonts.sh` in the worktree; deliberately not done here.
- The WebKit font-backend contract with L8 (seam frozen at CP-1; F5) is not
  this lane's deliverable; `ft_memface_smoke.c` is the seed API shape for it.
