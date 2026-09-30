# FreeType 2.14.3 — vendored pin (L5 font stack)

Pinned for the HobbyOS browser program (`browser.md` §2 pin table; lane L5 —
fonts/images). Vendoring layout per AD-11 / Appendix C: **committed** = this
README + the tarball + its `.sha256`; **gitignored** = the extraction
(`third_party/freetype-2.14.3/`) and the build trees under `obj/`.

## Provenance

| Item | Value |
|---|---|
| Tarball | `freetype-2.14.3.tar.xz` — 2,670,220 bytes |
| URL | https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz |
| sha256 | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` |
| Verified against | `browser.md` §2 pin table (recorded at W0; re-verified at vendoring — `sha256sum -c` green) |
| License | FreeType License (FTL) / GPL-2 dual — see the extraction's `docs/LICENSE.TXT` |

## Extraction (rebuildable from the committed tarball)

```sh
cd third_party
sha256sum -c freetype-2.14.3.tar.xz.sha256
tar -xf freetype-2.14.3.tar.xz          # -> third_party/freetype-2.14.3/
```

## Host build — exact executed recipe

Run by `src/user/browser/fonts/host_build_fonts.sh` (idempotent; skipped when
`obj/third_party/freetype/prefix/lib/libfreetype.a` exists):

```sh
FT_SRC=<repo>/third_party/freetype-2.14.3
FT_PREFIX=<repo>/obj/third_party/freetype/prefix
mkdir -p <repo>/obj/third_party/freetype/build && cd <repo>/obj/third_party/freetype/build
"$FT_SRC/configure" --prefix="$FT_PREFIX" --disable-shared --enable-static \
    --without-zlib --without-bzip2 --without-png --without-brotli --without-harfbuzz
make -j4
make install
```

Rationale for the switches:

- **out-of-tree build** — FreeType's autotools flow supports `configure` from
  a separate build dir; keeps the gitignored extraction pristine.
- **`--disable-shared --enable-static`** — static only: HobbyOS links
  statically (AD-6), and the host smokes want no loader surprises.
- **all optional deps off** — the host smoke must not depend on
  zlib/bzip2/png/brotli/harfbuzz presence; the FreeType+HarfBuzz pairing is
  provided by HarfBuzz's build (`hb-ft`), which links this prefix. The
  cross/target build makes its own dependency choices later (revisit at the
  cross-compile step).

Toolchain at execution (2026-09-30, Ubuntu 26.04, rootless workstation):
gcc 15.2.0 (`15.2.0-16ubuntu1`), GNU Make 4.4.1.

Logs (everything under `obj/` is gitignored):

- `obj/third_party/logs/freetype-configure.log`, `-make.log`, `-install.log`
  (rc=0 each; the one `configure: WARNING` is the benign `make refdoc` /
  missing `docwriter` pip package notice — documentation tooling only)
- cold re-run (fresh build root, same recipe, rc=0):
  `obj/third_party-cold/logs/freetype-*.log`

Artifacts: `obj/third_party/freetype/prefix/lib/libfreetype.a`,
`include/freetype2/` (ft2build.h chain), `lib/pkgconfig/freetype2.pc`
(version 2.14.3).

## Consumers in this lane

- `src/user/browser/fonts/ft_memface_smoke.c` — memory-face loader + glyph
  renderer (`FT_New_Memory_Face`; no filesystem path reaches FreeType),
  deterministic per-render + total hashes.
- HarfBuzz host build — `-DHB_HAVE_FREETYPE=ON`; `hb-ft` interop compiled
  into `libharfbuzz.a` against this prefix.

## Boundary

Host build only here. The **cross build for HobbyOS** (sysroot clang, target
flags, target dependency choices) is the later L5 / WPE-port step that reuses
this recipe as its host leg (two-build rule, browser.md §8.1 item 5).
