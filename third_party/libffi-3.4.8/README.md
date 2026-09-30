# libffi 3.4.8 — vendored host build (L6, browser/l6-glib)

Host-first pin of libffi for the GLib + WPE stack.  GLib treats libffi as a
hard dependency (`glib-2.88.3/meson.build:2301` — `dependency('libffi',
version : '>= 3.0.0')`, no `required : false`), because GObject closures are
libffi-backed: `g_cclosure_marshal_generic` is the fallback marshaller for
every `g_signal_connect`-style closure and calls through `ffi_call`
(`gobject/gclosure.c`; **no `ffi_closure` use anywhere in GLib** — verified by
grep, so the target build only needs the call path, not executable-memory
trampolines).

**Version choice.**  The lane directive pins the **3.4.x series**; 3.4.8
(2025-04-10) is the final release of that line — the widest distro/CI
exposure of any libffi (Debian trixie ships `3.4.8-2`).  Newer stable lines
exist upstream (3.5.x → 3.8.0, and GLib's own `subprojects/libffi.wrap` pins
the wrapdb build of 3.5.2), but GLib's floor is only `>= 3.0.0`, so this is a
deliberate, revisitable pin — moving to 3.5.x later is a one-constant change
in `fetch.sh` + README re-record.

| Field | Value |
|---|---|
| Tarball | `libffi-3.4.8.tar.gz` (1,397,992 B) — downloaded by `fetch.sh` (gitignored) |
| Upstream | https://github.com/libffi/libffi/releases/tag/v3.4.8 |
| sha256 | `bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b` |
| Checksum file | `libffi-3.4.8.tar.gz.sha256` (committed) |
| License | MIT — `LICENSE` copied from the tarball (committed) |
| Vendored | 2026-09-30, downloaded from the release URL above |

**Provenance:** upstream publishes no `.sha256`/`.sig` for this asset (the
GitHub release carries only the tarball).  The recorded hash was recomputed
locally and cross-checked against Gentoo's `dev-libs/libffi` Manifest:

    SHA512 libffi-3.4.8.tar.gz = 05344c6c...a70e24ffe   (matches locally)
    size                       = 1397992               (matches locally)

plus the Debian trixie source package (`libffi 3.4.8-2`,
`sources.debian.org/api/src/libffi/`).

## Layout

    libffi-3.4.8.tar.gz          pinned tarball (gitignored; fetch.sh rebuilds it)
    libffi-3.4.8.tar.gz.sha256   checksum record (committed)
    fetch.sh                     download + verify + extract (committed)
    build-host.sh                host static build + smoke recipe (committed)
    smoke/ffi_smoke.c            static-link smoke: ffi_call + ffi_closure (committed)
    LICENSE                      upstream license text (committed)
    README.md                    this file
    src/                         extracted upstream tree        (gitignored, rebuilt)
    build-host/                  host build + smoke output      (gitignored, rebuilt)

## Build recipe (verified)

`sh build-host.sh` (idempotent; `JOBS`, `CC` overridable):

    configure --prefix=build-host/prefix \
      --disable-shared --enable-static --disable-docs   (CFLAGS="-O2 -fPIC")
    make -j8 && make install

Docs are disabled so no texinfo/makeinfo is needed.  Toolchain used: gcc
15.2.0, GNU make; the release tarball ships its own `configure`.  Installed
into `build-host/prefix/`: `include/ffi.h`, `include/ffitarget.h`,
`lib/libffi.a` (**83,886 B**), `lib/pkgconfig/libffi.pc`.

## Smoke test

`smoke/ffi_smoke.c` — 8 deterministic checks: `ffi_prep_cif` + `ffi_call` for
an int×3→int function and a double×2→double function (SSE path), then
`ffi_closure_alloc` + `ffi_prep_closure_loc` + trampoline call (the closure
machinery GLib can use).  Run by `build-host.sh`; output lands in
`build-host/SMOKE.RESULT`.

Result (2026-09-30): `8 checks, 0 failures` → `ALL TESTS PASSED SUCCESSFULLY!`
(SMOKE.RESULT sha256 `5f03056feefee577840d1f81af74ad59d7ad70d3d74793a457b0b09814a64e2c`);
`ldd` of the smoke binary lists only libc — libffi.a is statically linked.

## Cross reuse (see ../glib-2.88.3/cross-notes.md)

Same recipe with `--host=<triplet>`; keep `--disable-shared --enable-static`.
For a bare-metal target: the `ffi_call` path GLib needs has no OS
requirements; `ffi_closure_alloc` needs `mmap(PROT_EXEC)` and is only on the
closure-trampoline path — a shim is only needed if closures are ever used at
runtime.
