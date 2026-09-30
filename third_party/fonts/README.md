# DejaVu fonts 2.37 — vendored font assets (L5 font stack)

The browser program's UI/text font set (`browser.md` §2 pin; Appendix C
`fonts/dejavu-2.37/`; lane L5). Same vendoring split as the library pins:
**committed** = the release zip + `.sha256` files + this README;
**gitignored** = the extraction (`third_party/fonts/dejavu-2.37/`).

## Provenance

| Item | Value |
|---|---|
| Archive | `dejavu-fonts-ttf-2.37.zip` — 5,522,795 bytes |
| URL | https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip |
| sha256 | `7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a` |
| Verified against | `browser.md` §2 pin table (recorded at v1/W0). GitHub publishes no per-asset digest for this release (API digest was empty) — the §2 pin is the authority; re-verified at vendoring with `sha256sum -c` |
| License | DejaVu license (Bitstream Vera derivative, permissive) — `dejavu-2.37/LICENSE` |

## Extraction (rebuildable from the committed archive)

```sh
cd third_party/fonts
sha256sum -c dejavu-fonts-ttf-2.37.zip.sha256
tmp=$(mktemp -d) && unzip -q dejavu-fonts-ttf-2.37.zip -d "$tmp" \
  && mv "$tmp/dejavu-fonts-ttf-2.37" dejavu-2.37 && rmdir "$tmp"
sha256sum -c dejavu-2.37.sha256          # per-file verification (36 files)
```

The zip's top-level directory `dejavu-fonts-ttf-2.37/` is renamed to
`dejavu-2.37/` on extraction to match the Appendix C layout.

## Contents + per-file checksums

`dejavu-2.37.sha256` lists sha256 for **all 36 extracted files** (22 TTFs in
`ttf/`, 6 fontconfig files, docs, LICENSE) — run `sha256sum -c
dejavu-2.37.sha256` from `third_party/fonts/` after extraction to verify.

The TTF set: DejaVu Sans / Sans-Bold / Sans-Oblique / Sans-BoldOblique /
SansCondensed (4) / SansMono (4) / Sans-ExtraLight / Serif (4) /
SerifCondensed (4) / DejaVuMathTeXGyre.

**Font used by the L5 smoke tests:** `dejavu-2.37/ttf/DejaVuSans.ttf`
(sha256 `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954`).

## Boundary / plan-time notes

- The host smokes load the TTF through a memory buffer only (no fontconfig,
  no font paths on the FreeType side — see `ft_memface_smoke.c`).
- **On-device file naming:** the FAT16 image is 8.3-constrained (kernel gap:
  names longer than 8 chars alias on truncated prefixes). When fonts get
  staged onto `disk.img` for the browser work, use an 8.3-safe name (e.g.
  `DEJAVUS.TTF`) via the `mcopy` recipe — do not expect the full upstream
  name on the image.
- The whole set is 5.5 MiB zipped / 12 MiB extracted (`du`); only the faces
  the browser actually needs should be staged on the image (budget per W0.3).
