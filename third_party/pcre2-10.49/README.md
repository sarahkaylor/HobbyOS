# pcre2 10.49 — vendored host build (L6, browser/l6-glib)

Host-first pin of PCRE2 for the GLib + WPE stack.  GLib 2.88.3 hard-requires
`libpcre2-8 >= 10.32` (`glib-2.88.3/meson.build:2251-2259` — `pcre2_req =
'>=10.32'`; `docs/browser/f0-deps-audit.md` §1.1 carries the requirement via
the GLib row).  GLib's GRegex — which WebCore uses for all pattern matching —
is PCRE2-backed (`glib/gregex.c`), so this library is on the critical path for
the WPE 2.54.0 port.

**Version choice — 10.49 is a security release.**  10.49 (2026-09-28) is a
security-only release fixing GHSA-r9hj-j2rw-4q3m: an out-of-bounds write when
a growable JIT stack (`pcre2_jit_stack_create`/`pcre2_jit_stack_assign`) is
used with unusually high JIT stack usage; release notes: *"Users should
upgrade to 10.49"*, affects 10.48 and earlier.  10.48 and older are therefore
**not** acceptable pins.

| Field | Value |
|---|---|
| Tarball | `pcre2-10.49.tar.bz2` (2,185,271 B) — downloaded by `fetch.sh` (gitignored) |
| Upstream | https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49 |
| sha256 | `53c156e1ba416a20da8e65395daa132da0d80e76910424caca3fcdae7831d384` |
| Checksum file | `pcre2-10.49.tar.bz2.sha256` (committed) |
| GPG signature | `pcre2-10.49.tar.bz2.sig` (committed, 566 B) — **verified GOOD** |
| License | BSD-3-Clause — `LICENCE.md` copied from the tarball (committed) |
| Vendored | 2026-09-30, downloaded from the release URL above |

**Provenance:** upstream publishes no `.sha256` file — the GPG-signed tarball
is the official integrity mechanism.  Verified against the official release
key documented in PCRE2 `SECURITY.md` (releases ≥ 10.45):

    gpg --recv-keys A95536204A3BB489715231282A98E77EB6F24CA8
    gpg --verify pcre2-10.49.tar.bz2.sig pcre2-10.49.tar.bz2
    → Good signature from "Nicholas Wilson <nicholas@nicholaswilson.me.uk>"
      primary key A955 3620 4A3B B489 7152 3128 2A98 E77E B6F2 4CA8

The recorded sha256 (recomputed locally) additionally matches the Homebrew
formula for the same release asset (`homebrew-core` master
`Formula/p/pcre2.rb`, `.tar.bz2`, same 64-hex value).

## Layout

    pcre2-10.49.tar.bz2         pinned tarball (gitignored; fetch.sh rebuilds it)
    pcre2-10.49.tar.bz2.sha256  checksum record (committed)
    pcre2-10.49.tar.bz2.sig     upstream GPG signature (committed)
    fetch.sh                    download + verify + extract (committed)
    build-host.sh               host static build + smoke recipe (committed)
    smoke/pcre2_smoke.c         static-link smoke: named groups/JIT/lookbehind/substitute (committed)
    LICENCE.md                  upstream license text (committed)
    README.md                   this file
    src/                        extracted upstream tree        (gitignored, rebuilt)
    build-host/                 host build + smoke output      (gitignored, rebuilt)

## Build recipe (verified)

`sh build-host.sh` (idempotent; `JOBS`, `CC` overridable) — static 8-bit only,
exactly what GLib/WPE consume via `libpcre2-8.pc`:

    configure --prefix=build-host/prefix \
      --disable-shared --enable-static \
      --enable-jit --disable-dependency-tracking   (CFLAGS="-O2 -fPIC")
    make -j8 && make install

Toolchain used: gcc 15.2.0, GNU make; the release tarball ships its own
`configure` (no autotools bootstrap needed).  Installed into
`build-host/prefix/`: `include/pcre2.h`, `include/pcre2posix.h`,
`lib/libpcre2-8.a` (**909,944 B**), `lib/libpcre2-posix.a` (6,194 B),
`lib/pkgconfig/libpcre2-8.pc` + `libpcre2-posix.pc`, `bin/pcre2test` +
`bin/pcre2grep`.

JIT is enabled and proven: smoke prints `pcre2 jit support: 1` and asserts a
successful `pcre2_jit_compile` + JIT match (the code path 10.49 fixes).

## Smoke test

`smoke/pcre2_smoke.c` — 12 deterministic checks: version string, named-group
compile/match/extract, NOMATCH, JIT compile + JIT match, lookbehind (a PCRE
feature, not POSIX ERE), and `pcre2_substitute` with `$2/$1`.  Run by
`build-host.sh`; output lands in `build-host/SMOKE.RESULT`.

Result (2026-09-30): `12 checks, 0 failures` → `ALL TESTS PASSED SUCCESSFULLY!`
(SMOKE.RESULT sha256 `0ccc7649ad25bdadb9ccc5825dfa191aafca5b75ffbd346e0c001ec9b5ad7b91`);
`ldd` of the smoke binary lists only libc — libpcre2-8.a is statically linked.

## Cross reuse (see ../glib-2.88.3/cross-notes.md)

Same recipe with `--host=<triplet>` for the target toolchain.  Recommendation
for the HobbyOS cross build: **drop `--enable-jit`** unless the target offers
writable+executable mappings and an instruction-cache flush — the interpreter
fallback is fully functional (sljit does have aarch64/x86_64 backends if JIT
is wanted later).
