# GLib 2.88.3 — vendored host build (L6, browser/l6-glib)

Host-first pin of GLib for the WPE 2.54.0 port.  WPE hard-requires GLib
`>= 2.70.0` — `find_package(GLib 2.70.0 REQUIRED COMPONENTS GioUnix Thread
Module)` (`Source/cmake/OptionsWPE.cmake:12`, quoted in
`docs/browser/f0-deps-audit.md` §1.1) — and the W0.4 audit correction
(browser.md §1.4) says: *"GLib (+deps) is added to the L6 host-first build
list; libsoup3 drops out when/if the curl port patch lands."*  This lane
delivers that host-first build.  Static libraries only — **not** wired into
the HobbyOS Makefile; `build-host.sh` is the recipe the WPE/port cross builds
reuse, and `cross-notes.md` is the target-build memo.

Trio members (hard deps, vendored alongside this directory): `../pcre2-10.49`
(GLib requires `libpcre2-8 >= 10.32`, meson.build:2252), `../libffi-3.4.8`
(required, `>= 3.0.0`, meson.build:2301), and the already-vendored
`../zlib-1.3.1` (reused from browser/l6-libs1 — not re-vendored).

| Field | Value |
|---|---|
| Tarball | `glib-2.88.3.tar.xz` (5,794,356 B) — downloaded by `fetch.sh` (gitignored) |
| Upstream | https://download.gnome.org/sources/glib/2.88/glib-2.88.3.tar.xz |
| sha256 | `ab24d24e698dfa1e408b7bcdb508f4aafc906185a8b8ce72fdf79bbbdc9b383b` |
| Checksum file | `glib-2.88.3.tar.xz.sha256` (committed) |
| License | LGPL-2.1-or-later — `COPYING` copied from the tarball (committed) |
| Vendored | 2026-09-30, downloaded from the URL above |

**Provenance:** the sha256 is the **official** value published by GNOME in the
release directory's `glib-2.88.3.sha256sum` file; the local download was
recomputed and matches byte-for-byte.

**Version choice:** 2.88.3 is the newest stable 2.8x release at vendoring time
(released 2026-07-29).  It carries the CVE-2026-15588 fix (GDBusServer
pre-auth DoS via unbounded SASL line buffering) and a GCC 17 `G_GNUC_CONST`
miscompilation fix.  Older stable fallback, checksum recorded but not built:
2.86.5 (`ce85a947bb8b3c0204dbeff79aec39bcb46371c6fafb64ba5b8726c71e038d5f`).

## Layout

    glib-2.88.3.tar.xz         pinned tarball (gitignored; fetch.sh rebuilds it)
    glib-2.88.3.tar.xz.sha256  checksum record (committed)
    fetch.sh                   download + verify + extract (committed)
    build-host.sh              SHARED orchestrator: deps → GLib → smoke (committed)
    smoke/glib_smoke.c         46-check static-link smoke (committed)
    cross-notes.md             target cross-build memo (committed)
    COPYING                    upstream license text (committed)
    README.md                  this file
    src/                       extracted upstream tree        (gitignored, rebuilt)
    build-host/                host build + smoke output      (gitignored, rebuilt)

## Build recipe (verified)

`sh build-host.sh` (idempotent; `JOBS` default 8, `CC`, `PCRE2_DIR`,
`LIBFFI_DIR`, `ZLIB_PREFIX` overridable).  It builds the trio in order —
pcre2 → libffi → zlib-if-missing → GLib — each dep via its own
`build-host.sh`, then:

    meson setup build-host/build src \
      --prefix=build-host/prefix --libdir=lib --buildtype=release \
      --wrap-mode=nofallback \
      -Ddefault_library=static -Dc_args=-fPIC \
      -Dnls=disabled -Dselinux=disabled -Dlibmount=disabled -Dsysprof=disabled \
      -Dxattr=true -Dman-pages=disabled -Ddocumentation=false \
      -Dintrospection=disabled -Ddtrace=disabled -Dsystemtap=disabled \
      -Dtests=false -Dinstalled_tests=false
    ninja -j8 && ninja install

Option rationale — all chosen for a hermetic, cross-reusable static build:

- `default_library=static` — the WPE port links these archives statically.
- `nls=disabled` — **no gettext needed** (see below); `libintl` is not built
  or linked.  `documentation=false`/`man-pages=disabled`/`introspection=disabled`
  also keep host-tool deps (gtk-doc, rst2man, g-ir-scanner) out of the build.
- `selinux`/`libmount`/`sysprof` disabled — no system library can leak in.
- `--wrap-mode=nofallback` — GLib must resolve pcre2/libffi/zlib from the
  vendored prefixes (via `PKG_CONFIG_PATH`, prefixes first); a silent wrapdb
  fallback download would invalidate the pin.

**gettext decision (explicit):** not vendored.  GLib only needs gettext when
`nls` is enabled; with `-Dnls=disabled` the build is fully functional and the
smoke passes.  If NLS is ever wanted, it becomes a follow-on vendor
(libintl/static gettext + GLib `po/` catalogs) — nothing in the WPE-port
critical path requires translations.

Toolchain used: meson 1.12.1 (rootless `uv tool install meson`, `~/.local/bin/meson`),
ninja 1.13.2, gcc 15.2.0; GLib 2.88.3 requires meson `>= 1.4.0`.

## Dependency resolution (from meson-log.txt — the pin gate)

    Run-time dependency libpcre2-8 found: YES 10.49
    Run-time dependency libffi     found: YES 3.4.8
    Run-time dependency zlib       found: YES 1.3.1

All three resolved to the vendored `build-host/prefix` builds (prefixes first
in `PKG_CONFIG_PATH`), not to any system copy.

## Installed artifacts + sizes (2026-09-30)

| Artifact | Size (bytes) |
|---|---|
| `lib/libglib-2.0.a` | 2,636,850 |
| `lib/libgobject-2.0.a` | 863,232 |
| `lib/libgio-2.0.a` | 5,175,508 |
| `lib/libgmodule-2.0.a` | 18,942 |
| `lib/libgthread-2.0.a` | 2,790 |
| `lib/libgirepository-2.0.a` | 826,254 |
| deps: `libpcre2-8.a` | 909,944 |
| deps: `libffi.a` | 83,886 |
| deps: `libz.a` (reused l6-libs1 recipe) | 132,716 |

pkg-config files installed: `glib-2.0`, `gobject-2.0`, `gio-2.0`,
**`gio-unix-2.0`** (the file WPE's `GioUnix` component resolves),
`gmodule-2.0`, `gmodule-export-2.0`, `gmodule-no-export-2.0`, `gthread-2.0`,
`girepository-2.0`.  No gio loadable modules are installed (static build —
`lib/gio/modules/` is empty), which is what a statically linked port wants.

## Smoke test

`smoke/glib_smoke.c` — 46 deterministic static-link checks covering the
mission list plus the WPE component libs:

- GString append/printf/prepend; GHashTable insert/lookup/remove + sorted-key
  iteration; GPtrArray add/sort/find/remove; g_ascii (strup/strdown/casecmp/
  isdigit/strtod); **GRegex** (named groups, NOMATCH, `\2/\1` and
  `\g<lane>/\g<tool>` replacement, lookbehind, `escape_string` literal
  roundtrip) — i.e. the PCRE2 path end-to-end; GDateTime (format, day-of-year,
  UTC offset, add_days, difference); GMutex + 4×GThread counter; GThreadPool
  8-task sum; GObject (instantiate, signal registration, emit through
  `g_cclosure_marshal_generic` — the ffi-backed marshaller — GValue roundtrip);
  GModule (supported, `g_module_open(NULL)`, symbol lookup); GIO
  (GMemoryInputStream read, GFile basename).

Result (2026-09-30): `46 checks, 0 failures` →
`ALL TESTS PASSED SUCCESSFULLY!`
(SMOKE.RESULT sha256 `ad2733cc7558d7a265236b0a4f422ee3bbf8e3ea391478fb9dca92da1a3336f3`).

Determinism: two consecutive runs diff clean (byte-identical output).
Static-link proof: `ldd` lists only `libc.so.6`; the binary embeds 116
`pcre2_*`/`ffi_*` symbols (`nm`), e.g. `pcre2_jit_compile_8`,
`ffi_closure_alloc`.

## WPE / cross-build reuse

- Host consumption: point WPE's cmake at the prefixes —
  `CMAKE_PREFIX_PATH` (and `PKG_CONFIG_PATH`) over
  `glib-2.88.3/build-host/prefix`, `pcre2-10.49/build-host/prefix`,
  `libffi-3.4.8/build-host/prefix`.
- Target consumption: see `cross-notes.md` (meson cross file, pcre2/libffi
  static cross builds, no-dlopen/locale/urandom audit list, gio subset and
  fork/spawn findings with file pointers).
