# Cross-build notes — GLib trio (GLib 2.88.3 / pcre2 10.49 / libffi 3.4.8)

Memo for the target cross build of the L6 GLib trio (lane browser/l6-glib,
host-first build proven — see `README.md` and the two dep READMEs).  Scope is
what's **needed next and where the landmines are**; no cross build was
attempted (time-boxed probe of the host-built tree + the WPE sources).

## 0. Shape of the target build

Order: pcre2 → libffi → GLib, same recipes as host, with cross toolchain:

1. `pcre2-10.49/build-host.sh` with a cross `CC` and `--host=<triplet>` —
   current script has no `--host` knob yet; either export `CC` as
   `aarch64-none-elf-gcc` (autoconf infers the host triplet) or add a small
   `CONFIGURE_EXTRA`/`CROSS_HOST` env pass-through (one-line change, do it on
   first use).  **Drop `--enable-jit`** unless the target has W+X mmap + cache
   flush; interpreter mode is fine and sljit covers aarch64/x86_64 if wanted.
2. `libffi-3.4.8/build-host.sh` likewise (`--host=<triplet>`; keep
   `--disable-shared --enable-static --disable-docs`).  GLib only needs the
   `ffi_call` path (`gobject/gclosure.c`; grep shows **no `ffi_closure`** use
   anywhere in GLib), and `ffi_call` has no OS requirements — the `mmap`
   dependency only appears on the unused-by-GLib closure-trampoline path.
3. GLib via a **meson cross file**.  Skeleton for aarch64 (x86_64 analogue):

       [binaries]
       c = 'aarch64-none-elf-gcc'
       ar = 'aarch64-none-elf-ar'
       strip = 'aarch64-none-elf-strip'
       pkg-config = 'pkg-config'
       [host_machine]
       system = 'hobbyos'        # see caveat below
       cpu_family = 'aarch64'
       cpu = 'aarch64'
       endian = 'little'
       [properties]
       needs_exe_wrapper = true  # only if meson must run target binaries

   then the same option set as host (`--wrap-mode=nofallback`,
   `-Ddefault_library=static`, nls/selinux/libmount/sysprof/man-pages/
   documentation/introspection/dtrace/systemtap all off, `tests=false`), plus
   `PKG_CONFIG_LIBDIR` pointed at the cross pcre2/libffi prefixes (LIBDIR, not
   PATH — so host system .pc files cannot leak in).

   **Caveat, decide early:** GLib's meson builds Linux-specific defaults
   (`host_system == 'linux'` branches: eventfd wakeup, inotify file monitors,
   some gio paths).  Two strategies: (a) declare `system = 'hobbyos'` and fix
   the fallbacks the configure log flags — cleanest, but expect a fistful of
   checks to fail the first time; (b) report `system: 'linux'` with a
   custom `c_lib` and stub the handful of Linux-only syscalls.  Recommend
   trying (a) first and keeping the meson-log as the audit driver.

## 1. Runtime assumptions to audit/stub (host facts + grep pointers)

- **iconv** — `g_convert`/`g_locale_to_utf8` need `iconv_open` et al.
  meson does `dependency('iconv')` (glib-2.88.3/meson.build:2239-2249); glibc
  provides it in libc, a bare-metal libc must provide a minimal iconv
  (UTF-8/UTF-16/UCS-4/latin1 is enough for WebKit's usage) or GLib needs a
  `libiconv` port.  **Order of work: iconv shim early — it's a hard probe.**
- **locale** — `setlocale`/`nl_langinfo(CODESET)` used by `g_get_charset`/
  `g_get_filename_charsets`.  A C-locale stub suffices (charset falls back to
  UTF-8/ASCII strings); do not assume libc locale machinery.
- **wakeup fd** — `glib/gwakeup.c` prefers `eventfd()` and falls back to
  `pipe()` when `sys/eventfd.h` is absent (both compile paths in-tree).  A
  HobbyOS `eventfd` or plain `pipe()` implements this; nothing to change in
  GLib if the header is hidden in the sysroot.
- **entropy** — `glib/grand.c` reads `/dev/urandom`, else seeds from time
  (lines 164-191).  `/dev/urandom` or an entropy syscall shim at some point
  (not critical for smoke).
- **threads** — `GThread`/`GMutex`/`GCond` over pthreads (L2 P1 threads
  design is the mapping target); GLib's meson probes for POSIX threads and
  atomic ops; aarch64 gcc atomics are fine.
- **time** — `clock_gettime(CLOCK_MONOTONIC)` (g_get_monotonic_time),
  `time()`; standard.
- **dlopen** — see gmodule below.  `gmodule/gmodule.c` compiles with
  `G_MODULE_IMPL = ''` when the meson probe for dlopen/dlsym fails, i.e. no
  loader; **WPE does use GModule**: `Source/WebKit/Platform/glib/ModuleGlib.cpp:38`
  and `WebProcess/InjectedBundle/glib/InjectedBundleGlib.cpp:39` call
  `g_module_open(...)`.  Target options: implement a HobbyOS loader for the
  `.so`-equivalent format, or register those two modules statically and stub
  `g_module_open` to resolve from a built-in symbol table.
- **spawn/fork — the multi-process landmine.**  WPE's UIProcess launcher
  uses **`g_spawn_async`**: `Source/WebKit/UIProcess/Launcher/glib/ProcessLauncherGLib.cpp`
  (also `WPEProcessManager.cpp`, `WTF/wtf/glib/Sandbox.cpp` reference spawn).
  The file itself notes GLib picks `posix_spawn()` vs `fork()/exec()`
  (g_spawn_async_with_pipes_and_fds rules, "As of GLib 2.74").  So the
  target needs: `posix_spawn`/`fork` + `execve` + `pipe`/`dup2` family +
  `waitpid`, or dedicated HobbyOS implementations behind those names; the
  /proc-based fd cleanup in gspawn is the other stubbed area (audit
  `glib/gspawn.c` fd-walking code path for the no-/proc case).
- **gio subset** — host built the full static gio (5.18 MB archive, no
  loadable modules).  For target, decide minimal-vs-full with this audit list:
  - `gio/gunixmounts.c` (mntent/getmntent) — `GUnixMountMonitor` users only.
  - `gio/glocalfile*`, `glocalfileinfo.c` — statfs, xattr syscalls (xattr
    can be disabled at configure: `-Dxattr=false`).
  - file-monitor backend: `-Dfile_monitor_backend=inotify` is the Linux
    default; options for target: keep inotify (if HobbyOS has it) or a stub
    backend.
  - `gio/gthreadedresolver.c` — DNS via libc; WebKit goes through libcurl +
    mbedTLS for networking, so likely unused → candidate to stub.
  - GDBus (`gio/gdbus*`) — sockets + SCM_RIGHTS; audit whether the WPE build
    enables any D-Bus path before deciding to trim.
  - `GioUnix` component: `gio-unix-2.0.pc` is installed (no separate unix
    library in 2.88) — WPE's `find_package(GLib COMPONENTS GioUnix)` should
    resolve with the prefix on `CMAKE_PREFIX_PATH`.
- **gettext** — not needed: `-Dnls=disabled` (documented in README.md).
  Keep that decision in the cross file too.

## 2. Suggested next steps (exact)

1. Add `--host`/extra-configure-args knob to pcre2 and libffi `build-host.sh`
   (small, host build stays default).
2. Write `glib-cross-aarch64.txt` meson cross file per §0; first run is a
   configure-only (`meson setup --cross-file ... build-cross`), capture the
   log, and let the failing probes drive the shim list (§1) — start with
   iconv + locale + wakeup + threads.
3. Cross-build pcre2 (no JIT) and libffi; re-run GLib configure with
   `PKG_CONFIG_LIBDIR` set to the cross prefixes.
4. Link `smoke/glib_smoke.c` against the cross prefixes and run it on target
   (qemu) once the HobbyOS userland can execute such a binary; the smoke has
   no host-only dependencies (stdio + libc only).
5. Audit the WPE side against the §1 list before the first WPE cmake run
   (~cmake will surface missing GLib pieces immediately).

## 3. Known risks

- `system='hobbyos'` may need small meson-ecosystem hardening; the fallback
  is pretending `linux` and stubbing syscalls (recorded above).
- gspawn/posix_spawn is a hard requirement for the WPE multi-process model —
  if HobbyOS can't provide it, the launcher becomes a port-specific fork.
- gmodule without dlopen needs a decision (loader vs static registry) before
  InjectedBundle works.
- libffi on a non-ELF ABI target may need `--with-abi`-like tweaks; 3.4.8's
  aarch64/x86_64 ports are mature, so this is polish, not research.
