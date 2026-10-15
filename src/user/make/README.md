# GNU make 4.4.1 — HobbyOS port

Port of GNU make 4.4.1 (GPL-3+, `docs/gnu-ports.md` house style) for the
HobbyOS self-hosting program.  Binary: `MAKE.BIN` (`obj/<arch>/make.bin`).
The pristine upstream tree is vendored at `third_party/make-4.4.1/`
(sha256 `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3`,
pinned in `docs/selfhost/buildtools-audit.md` §1 by lane L4).

## Files

| file | origin | note |
|---|---|---|
| `ar.c` `arscan.c` `commands.c` `default.c` `dir.c` `expand.c` `file.c` `function.c` `getopt.c` `getopt1.c` `guile.c` `hash.c` `implicit.c` `job.c` `load.c` `loadapi.c` `main.c` `misc.c` `output.c` `posixos.c` `read.c` `remake.c` `remote-stub.c` `rule.c` `shuffle.c` `signame.c` `strcache.c` `variable.c` `version.c` `vpath.c` | `third_party/make-4.4.1/src/` | **byte-identical** transcription (verified `cmp`) |
| `commands.h` `debug.h` `dep.h` `filedef.h` `getopt.h` `gettext.h` `gnumake.h` `hash.h` `job.h` `makeint.h` `mkcustom.h` `os.h` `output.h` `rule.h` `shuffle.h` `variable.h` | `third_party/make-4.4.1/src/` | byte-identical headers |
| `glob.c` `fnmatch.c` | `third_party/make-4.4.1/lib/` | byte-identical (make bundles glob/fnmatch since 4.4) |
| `glob.h` `fnmatch.h` `intprops.h` `intprops-internal.h` | `lib/glob.in.h` `lib/fnmatch.in.h` `lib/intprops*.h` | pristine content (configure generates these from the `.in.h`) |
| `main.c` | `third_party/make-4.4.1/src/` | **byte-identical + two marked `[HobbyOS port]` patch points** — make's `main(argc,argv,envp)` needs the two-arg `main` (HobbyOS crt0 ABI) and `environ`, exactly its own z/OS fallback; see config.h `HOBBYOS_NO_ENVP_ARG` |
| `config.h` | hand-written | pins the configure-time feature set — see below |
| `hb_stubs.c` `hb_stubs.h` | first-party | sysroot-gap shims |
| `ar.h` | first-party (OS shim) | the sysroot has no `<ar.h>`; System-V layout |
| `README.md` | this file | |

Dropped on purpose: `amiga.c`, `vms*.c`, `w32/*`, `load.c` body (`MAKE_LOAD 0`
compiles it out).  `guile.c` is kept because `main.c` calls
`guile_gmake_setup()` unconditionally; with `HAVE_GUILE` undefined it
compiles to the upstream no-guile stub.  `remote-stub.c` is kept for the
same reason (`remote_description` / `remote_cleanup` are always referenced).

## config.h — the pinned configuration

`docs/selfhost/buildtools-audit.md` §3.1 lists the exact probe results the
port pins.  Highlights (all audited):

* **`-j1` v1 (the audit's `minimal -j1`):** `MAKE_JOBSERVER` is UNDEFINED, so
  the jobserver/load machinery compiles out of `posixos.c` and `os.h`
  (a value of 0 would NOT disable it — those files guard with
  `#ifdef`/`defined()`, which is why the config comments say UNDEF).  The
  audit's §3.3 records what `-j>1` needs later (jobserver pipe inheritance,
  SIGCHLD interrupting a blocked `read`, process-group kill, pselect).
  Consequence: `reap_children(1)` blocks in `waitpid`, which the sysroot
  supports.
* `MAKE_LOAD 0` — the `load` directive needs dlopen; none here.
* `ENABLE_NLS 0` — `gettext.h` maps `_()` to plain strings.
* Self-provided fallbacks kept OFF where make's own portable code exists:
  `HAVE_STRNDUP`/`HAVE_MEMPCPY`/`HAVE_WAIT3`/`HAVE_SIGSETMASK`/
  `HAVE_GETRLIMIT`/`HAVE_PSELECT`/`HAVE_TTYNAME`/`HAVE_GETPWNAM_R`/
  `HAVE_GETLOGIN_R`/`HAVE_STRUCT_DIRENT_D_TYPE` are all UNDEFINED (not 0).
  Value-guarded ones (`HAVE_STRSIGNAL 0`, `HAVE_STPCPY 0`,
  `HAVE_DECL_GETLOADAVG 0`) stay 0.
* `FILE_TIMESTAMP_HI_RES 0` — the sysroot `struct stat` has no nanosecond
  member (`st_mtime` is a `long`), so `ST_MTIM_NSEC` cannot be referenced;
  the RTC is whole-second anyway.
* `MAKE_HOST` = `aarch64-hobbyos` / `x86_64-hobbyos` (from `__aarch64__`).

## hb_stubs.c — sysroot gaps the port fills (each is audited)

| symbol | why | semantics |
|---|---|---|
| `getlogin()` | read.c:3114 `~` expansion calls it unconditionally on POSIX builds (row §3.2 "getlogin") | returns `LOGNAME` or `"user"` (the synthetic account from `src/libc/src/pwd.c`, so `getpwnam()` resolves home `/`) |
| `alarm()` | posixos.c load-throttle path only when the jobserver is enabled; compiled out under `-j1` | no-op returning 0 (audit: "stub returning 0" acceptable); shipped so later `-j` enablement links |
| `getloadavg()` | job.c:2138 under `-l` | returns -1 with `errno=0` → make's "cannot enforce load limits" note (audit's unsupported convention) |
| `dup()` | posixos.c `os_anontmp()` tmpfile fallback; `<unistd.h>` declares it but no libc object implements it | real POSIX `dup` over the sysroot's `dup2()` (lowest free fd) |

`umask()` is NOT stubbed: the sysroot implements it (src/libc/src/stat.c,
records the mask, FAT16 applies nothing), which is better than make's own
0-returning fallback; `HAVE_UMASK 1` merely stops `makeint.h` from
re-typedefing `mode_t`.

## dirent contract (v1) — make's dir cache and this OS

`opendir/readdir/closedir` are ENOSYS stubs in the sysroot today (parent
single-writer libc milestone; `src/libc/src/dirent.c`).  make degrades
gracefully: `find_directory` marks an unreadable directory
(`dc->dirfiles` = NULL) instead of aborting, so:

* **works:** `make -f FILE TARGET` with explicit file names — targets and
  prerequisites resolve via `stat`/`name_mtime` (remake.c:52).  This is the
  whole `Makefile.hobbyos` model (L4 §7.1): explicit lists, no wildcards.
* **unavailable until the dirent milestone lands:** `$(wildcard)`, VPATH
  search, implicit `%` rules, default-makefile discovery, `~` globbing.
  These are the `-j`/dirent items the audit waves forward.

`Makefile.hobbyos` MUST keep the L4 sketch discipline for the same reason —
one command per recipe line, absolute `/TOOLS/*.BIN` names, `SHELL=/TOOLS/SH.BIN`
(`sh -c` contract, audit §6).

## Build (host toolchain, in-tree)

```
make obj/arm/make.bin          # ARM (primary)
make ARCH=intel obj/intel/make.bin   # x86_64 (cheap, kept working)
```

Staged on the boot disk as `::/MAKE.BIN` (the disk recipe).  The in-OS
smoke is `MAKE --version`; the full in-OS contract (`make -f
Makefile.hobbyos ...`, SHELL/CC/LD env) is gate M3's, not this lane's.
