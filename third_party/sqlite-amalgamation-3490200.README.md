# SQLite 3.49.2 (amalgamation) — vendored, host build + smoke (L6 batch 2)

Storage engine for the browser tracks (browser.md AD-8; P6.2 ports this to
the OS with a custom VFS). Host build + smoke:
`src/host/build_sqlite_host.sh` / `src/host/sqlite_smoke.c`.

## Pin & provenance

- browser.md §2 lists "**SQLite** (3.4x)" as a W0-time pin ("To pin at W0 …
  versions recorded then"); W0.1 deferred the L6 tarballs to vendoring time.
  **Recorded here: 3.49.2 — the newest release of the documented 3.4x
  series** (released 2025-05-07). Note for the integrator: upstream has
  since moved on — 3.50.x … 3.53.x exist and **current is 3.53.4**
  (2026-09-11). If a newer pin is wanted, it is a one-line change (URL +
  `.sums` + the sha3 constant in the build script); this lane kept the
  documented series rather than silently upgrading past it.
- URL: https://sqlite.org/2025/sqlite-amalgamation-3490200.zip
- sha256 (computed; SQLite publishes SHA3-256, not SHA-256):
  `921fc725517a694df7df38a2a3dfede6684024b5788d9de464187c612afb5918`
- Version identity: `sqlite3_sourceid()` = `2025-05-07 10:39:52
  17144570b0d96ae63cd6f3edca39e27ebd74925252bbaf6723bcb2f6b4861fb1`
- Committed (AD-11 layout):
  - `sqlite-amalgamation-3490200.zip` (2 826 584 bytes)
  - `sqlite-amalgamation-3490200.zip.sums` — one `sha256sum -c`-compatible line
  - this README
- Extraction `third_party/sqlite-amalgamation-3490200/` (`sqlite3.c`,
  `sqlite3.h`, `sqlite3ext.h`, `shell.c`) is rebuilt from the zip
  (gitignored); no patches.
- License: public domain.
- **Proposed §2 pin-table row**: `sqlite-amalgamation-3490200.zip |
  https://sqlite.org/2025/sqlite-amalgamation-3490200.zip |
  921fc725517a694df7df38a2a3dfede6684024b5788d9de464187c612afb5918`
  (or bump to 3.53.4 with its download-page SHA3-256
  `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e`).

## Verification (2026-09-30)

- zip sha256 computed and recorded in `.sums` (re-checked on every build).
- Official source check: the amalgamation's `sqlite3.c` must match the
  SHA3-256 published in SQLite's own release log
  (https://sqlite.org/releaselog/3_49_2.html), which the build script
  verifies with Python on every run:

```
sqlite3.c sha3-256: 17f4857fc6a0def2749d248d5365c59282c044270533c6c2b21287295f01eb23
sqlite3.c sha3-256 matches the official 3.49.2 release log
```

## Host build recipe

`src/host/build_sqlite_host.sh` (any cwd; `REPO`/`CC` overridable). One
translation unit, straight amalgamation compile - no configure, no CMake,
no pkg-config:

```
cc -O2 -DSQLITE_THREADSAFE=1 -c sqlite3.c -o obj/.../sqlite3.o
ar rcs obj/.../libsqlite3.a obj/.../sqlite3.o
cc -O2 -DSQLITE_THREADSAFE=1 -o obj/.../sqlite3_cli sqlite3.c shell.c -lpthread -ldl -lm
```

- Toolchain: gcc 15.2.0. `-DSQLITE_THREADSAFE=1` (serialized; the P6.2
  requirement) is the only option define; everything else is the
  amalgamation default. The unix VFS is the host's; the OS build replaces
  it (target deltas below).
- Artifacts: `libsqlite3.a` 1 422 878 B, `sqlite3.o` 1 415 992 B,
  `sqlite3_cli` 1 504 400 B.
- Note: gcc 15 emits two identical `-Wdiscarded-qualifiers` warnings from
  upstream `sqlite3.c:124685` (`sqlite3ShadowTableName`, a C23
  `strrchr`-const interaction). Upstream code is left pristine; the
  warnings are cosmetic and do not fail the build.
- Raw log (fresh extract → smoke):
  `/home/sarah/hobbyos-lanes/l6-libs2/obj/third_party/logs/sqlite-3.49.2-build-smoke.log`

## Smoke + thread-safety record (for P1/P6)

`src/host/sqlite_smoke.c` — in-memory CREATE / INSERT / SELECT with exact
row checks, `PRAGMA compile_options` dump, and the thread-safety mode.
Run (exit 0 = all pass):

```
sqlite3_libversion()  = 3.49.2
sqlite3_sourceid()    = 2025-05-07 10:39:52 17144570b0d96ae63cd6f3edca39e27ebd74925252bbaf6723bcb2f6b4861fb1
sqlite3_threadsafe()  = 1
PASS: serialized thread-safe build (P6.2 target)
row 1..3 match; PASS: SELECT returned 3 rows
...
SMOKE PASS: SQLite in-memory CREATE/INSERT/SELECT OK
```

**Thread-safety mode: `sqlite3_threadsafe()` returns 1 — serialized
(SQLITE_THREADSAFE=1), and `THREADSAFE=1` is listed in
`PRAGMA compile_options`; the mutex layer is `MUTEX_PTHREADS`.** This is
what P6.2 asks for. Implication for the coming P1 threads work: one
connection is safe to use from multiple threads once pthreads exist (SQLite
serializes internally around its mutexes), at a single-writer-per-moment
cost; revisit only if profiling wants mode-2 multi-thread builds plus
per-thread connections. `MAX_WORKER_THREADS=8` bounds SQLite's own worker
threads.

Full `PRAGMA compile_options` record (37 rows):

```
ATOMIC_INTRINSICS=1 COMPILER=gcc-15.2.0 DEFAULT_AUTOVACUUM
DEFAULT_CACHE_SIZE=-2000 DEFAULT_FILE_FORMAT=4 DEFAULT_JOURNAL_SIZE_LIMIT=-1
DEFAULT_MMAP_SIZE=0 DEFAULT_PAGE_SIZE=4096 DEFAULT_PCACHE_INITSZ=20
DEFAULT_RECURSIVE_TRIGGERS DEFAULT_SECTOR_SIZE=4096 DEFAULT_SYNCHRONOUS=2
DEFAULT_WAL_AUTOCHECKPOINT=1000 DEFAULT_WAL_SYNCHRONOUS=2
DEFAULT_WORKER_THREADS=0 DIRECT_OVERFLOW_READ MALLOC_SOFT_LIMIT=1024
MAX_ATTACHED=10 MAX_COLUMN=2000 MAX_COMPOUND_SELECT=500
MAX_DEFAULT_PAGE_SIZE=8192 MAX_EXPR_DEPTH=1000 MAX_FUNCTION_ARG=1000
MAX_LENGTH=1000000000 MAX_LIKE_PATTERN_LENGTH=50000 MAX_MMAP_SIZE=0x7fff0000
MAX_PAGE_COUNT=0xfffffffe MAX_PAGE_SIZE=65536 MAX_SQL_LENGTH=1000000000
MAX_TRIGGER_DEPTH=1000 MAX_VARIABLE_NUMBER=32766 MAX_VDBE_OP=250000000
MAX_WORKER_THREADS=8 MUTEX_PTHREADS SYSTEM_MALLOC TEMP_STORE=1 THREADSAFE=1
```

## Reuse notes (target builds / P6.2)

- Cross build: `CC=<cross-cc> src/host/build_sqlite_host.sh` (drop
  `-lpthread -ldl -lm` to the target's equivalents; the required linker
  pieces are none beyond libc once the VFS is ours).
- P6.2 deltas (not applied here; host build stays vanilla unix-VFS):
  `-DSQLITE_OS_OTHER=1` + a single HobbyOS VFS over the libc file API
  (FAT16 first), `fcntl` locks per P6.1, WAL off initially, no extension
  loading (no `dlopen`), `-DSQLITE_OMIT_LOAD_EXTENSION` candidate.
- WebKit needs no specific SQLite minimum beyond `3.7.15` feature gates
  (f0-deps-audit §1.3: `SQLITE_VERSION_NUMBER >= 3007015` for
  `sqlite3_errstr`); 3.49.2 satisfies every in-tree gate.
