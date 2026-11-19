# Cyberpunk track — P0 evidence (game-independent half: Wine)

Lane C2-wine, 2026-11-14/15. Everything below is either raw output from a real
run on `linuxci` (Ubuntu 26.04, kernel `7.0.0-43-generic`, AMD EPYC 7513 —
AVX2, XSAVEC, FSGSBASE, no AVX-512) or a `file:line` read in the pinned Wine
11.0 tree (`wine-11.0/…`, pins in `docs/cp-pins.md`). Nothing here needs the
game or a GPU; the game-dependent rows of `docs/cyberpunk.md` §3.3 (fps, the
Venus column, the game's own VA map and file pattern) are still open.

HobbyOS facts are `path:line` in this tree at `f7a51cfa`.

---

## 0. The kernel-image hole `[0x70000000, 0x78000000)` — answer for C1-as

**Nothing Wine 11.0 needs intersects `[0x70000000, 0x78000000)`.** Measured and
read, item by item:

| Wine object | Address | In the hole? | Evidence |
|---|---|---|---|
| preloader-reserved "DOS area" | `0x10000–0x110000` | no | `loader/preloader.c:115`; strace `mmap(0x10000, 1048576, PROT_NONE, MAP_PRIVATE\|MAP_FIXED\|MAP_ANONYMOUS\|MAP_NORESERVE)` in every process |
| preloader-reserved "low memory area" | `0x110000–0x68000000` (upper end **0x68000000**) | no — ends 128 MiB below the hole | `loader/preloader.c:116`; strace `mmap(0x110000, 0x67ef0000, PROT_NONE, … MAP_FIXED …) = 0x110000` ×34 |
| gap `0x68000000–0x7f000000` | **not reserved by Wine at all** on x86_64 | — | the x86_64 table (`preloader.c:115-118`) has no row there; `mmap_init` (no-preloader path) reserves the same three ranges (`dlls/ntdll/unix/virtual.c:752-754`) |
| preloader-reserved "32-bit top-down + shared user data" | `0x7f000000–0x7fff0000` | no | `preloader.c:117`; strace `mmap(0x7f000000, 0xff0000, PROT_NONE, … MAP_FIXED …)` ×34 |
| first TEB block (32×64 KiB, top-down below 2 GiB) | `0x7fde0000–0x7ffe0000` | no | `virtual.c:4054` (`limit_2g - 1`, `MEM_TOP_DOWN`); strace `mmap(0x7fde0000, 0x200000, PROT_NONE, MAP_FIXED…)` ×34; maps of all 10 processes |
| `KUSER_SHARED_DATA` | `0x7ffe0000` (1 page, `r--s`) | no (0x7ffe0000 > 0x78000000) | `virtual.c:205`, `:4046`, `:4407` |
| syscall-dispatcher pointer page | `0x7ffe1000` | no | `dlls/ntdll/unix/signal_x86_64.c:2843-2846` |
| preloader-reserved top-down / virtual heap | `0x7ffffe000000–0x7fffffff0000` | no | `preloader.c:118` |
| preloader itself | `-static-pie` (kernel-placed; measured at `0x7d25e5919000`) — fallback form `-static -Ttext=0x7d7d00000000` | no | `configure.ac:1046-1051`; `readelf`: `Type: DYN`, `LOAD … 0x0000000000000000`, no `INTERP` |
| Unix `wine`, `ntdll.so`, glibc | kernel-placed PIE / `mmap(NULL)` (measured `0x7d25e4e00000…`) | no | maps below |
| PE builtins (`ntdll.dll` … `shell32.dll`) | `0x6ffffe9e0000–0x6ffffffe9000` | no | maps below |
| main `.exe` (cmd.exe etc.) | `0x140000000` | no (but inside today's 0–8 GiB identity map, AD-3) | maps below; 109 staged `.exe` have `ImageBase 0x140000000` |
| PE DLLs with low preferred bases | `wow64cpu.dll 0x7a400000`, `opengl32.dll 0x7a800000`, `riched20.dll 0x7ac00000` | no (all ≥ 0x7a400000) | `llvm-objdump -p` over the staged tree; the only sub-4 GiB bases among 757 PE files |
| the one sub-2 GiB non-reserved fixed map observed | `0x7bfe0000` (9 pages, `wow64cpu.dll` during wineboot's file install) | no | strace `mmap(0x7bfe0000, 36864, …, MAP_FIXED_NOREPLACE) = 0x7bfe0000` |

`tools/cp/maps-summary.py` over the 10 live processes captured during
`wine cmd /c pause`: **"Any process maps inside [0x70000000, 0x78000000): no"**
(§5). The lowest user mapping in every Windows process after start-up is
`0x7fde0000` — the low reservation is released once the main image is known
(`virtual_set_large_address_space`, `virtual.c:5022-5031`: a 64-bit exe with
`HIGH_ENTROPY_VA`+`DYNAMIC_BASE` gets `free_reserved_memory(0, 0x7ffe0000)`).

**Does Wine tolerate a refused mmap inside its reserved range?** Yes, at every
reservation site, but the errno matters:

1. **Preloader** (`preloader.c:1447-1459`): each range is one
   `mmap(MAP_FIXED|MAP_NORESERVE, PROT_NONE)`. On failure it prints
   `preloader: Warning: failed to reserve range %p-%p` and *drops the whole
   range* (`remove_preload_range`), then continues. None of its ranges touch
   the hole, so this path is not exercised by the hole — but today every one of
   them fails on HobbyOS (§6) and Wine still continues.
2. **ntdll without a preloader** (`reserve_area`, `virtual.c:629-700`): tries
   `MAP_FIXED_NOREPLACE`; on failure halves the range recursively — tolerant
   of a hole anywhere.
3. **Later allocations** (`try_map_free_area`, `virtual.c:1535-1556`): a
   refused `MAP_FIXED_NOREPLACE` is skipped and the search steps 64 KiB
   **only if `errno == EEXIST`**; any other errno logs
   `mmap() error … range …` and the allocation fails (returns NULL).
   `map_fixed_area` (`virtual.c:2179-2235`) maps `EEXIST` →
   `STATUS_CONFLICTING_ADDRESSES`, `ENOMEM` → `STATUS_NO_MEMORY`, anything
   else → `STATUS_INVALID_PARAMETER` with an ERR.
4. **`anon_mmap_tryfixed`** (`virtual.c:590-622`): if the kernel *ignores*
   `MAP_FIXED_NOREPLACE` and returns another address (pre-4.17 Linux
   behaviour), Wine unmaps it and sets `errno = EEXIST` itself — so "treat the
   flag as a hint" is a fully supported kernel answer.

So for the hole the kernel must answer `MAP_FIXED_NOREPLACE` with **`EEXIST`**
(as if something were mapped there — which is the truth) or treat it as a hint;
`MAP_FIXED` there with `ENOMEM`; never `EINVAL`. With that, keeping the kernel
image at `[0x70000000, 0x78000000)` costs Wine nothing measured here. Two
caveats for P1: a top-down 64 KiB-step search that walks through the hole does
up to 2048 failing `mmap` calls once (cheap), and a *non-relocatable* game
`.exe` whose preferred range overlapped the hole would fail to load — that is
open question 1 (`objdump -p Cyberpunk2077.exe`), not a Wine fact.

**Blocking finding independent of the hole:** HobbyOS currently answers every
`MAP_FIXED_NOREPLACE` with `-EINVAL` (`src/kernel/linux_compat_mem.c:195-196`).
Wine issues 1335 of them in these runs (757 ok, 578 `ENOMEM` probes); the first
is at process start in `get_host_addr_space_limit` (`virtual.c:2758-2777`),
which loops `addr = 1<<63 … 1<<32` and stops only on success or `EEXIST`. With
`EINVAL` every probe fails, the loop ends at `1<<31`, and Wine concludes the
host address space ends at **4 GiB** (`(1<<32) - 64 KiB`). Fix (P1, tiny):
implement NOREPLACE (fail `EEXIST` on overlap, `ENOMEM` above the ceiling) or
drop the flag and treat `addr` as a hint.

---

## 1. Build: Wine 11.0 against glibc 2.31

Command (on linuxci): `bash tools/cp/build-wine.sh all` (configure 30 s, make
`-j12` **260 s**, `install-lib` + strip-debug). Toolchain: Unix side
`clang 21.1.8 --sysroot=<glibc 2.31 sysroot>`; PE side `x86_64_CC='clang -D__STDC__'`
(configure's own detection — no MinGW, nothing installed). Configure picked:

```
build-wine: configure: PE compiler = x86_64_CC='clang -D__STDC__'
build-wine: configure: preloader LDFLAGS = WINEPRELOADER_LDFLAGS='-nostartfiles -nodefaultlibs -static-pie'
build-wine: configure: loader LDFLAGS    = WINELOADER_LDFLAGS='-Wl,--export-dynamic -pie'
build-wine: install: 202M staged at /home/sarah/cp/wine-build/stage/CP/WINE (strip-debug=1)
build-wine: install: manifest /home/sarah/cp/wine-build/stage-manifest.sha256 (873 files)
build-wine: audit: 29 ELF objects; max GLIBC_ symbol version required = GLIBC_2.29
build-wine: audit: DT_NEEDED union: libc.so.6 libdl.so.2 libgcc_s.so.1 libm.so.6 libpthread.so.0 libresolv.so.2 ntdll.so win32u.so
```

Two measured toolchain traps (both now encoded in `tools/cp/build-wine.sh`):

- `gcc --sysroot` is not a sysroot: `gcc --sysroot=S -print-search-dirs` lists
  `/usr/lib/gcc/x86_64-linux-gnu/15/../../../x86_64-linux-gnu/` (the **host**
  `/usr/lib/x86_64-linux-gnu`) before `S/lib/x86_64-linux-gnu/`. clang finds a
  GCC installation *inside* the sysroot and keeps every `-L` there.
- With the host's gcc 15 `libgcc_eh.a` copied into the sysroot, configure's
  `-static-pie` probe failed (`config.log:5241-5254`: `undefined reference to
  '_dl_find_object'` — GLIBC_2.35 — from `unwind-dw2-fde-dip.o`) and Wine fell
  back to `WINEPRELOADER_LDFLAGS='… -static -Wl,-Ttext=0x7d7d00000000'`. Using
  focal's `libgcc-10-dev` gives the `-static-pie` preloader a stock distro gets.
  `CP_WINE_PRELOADER=fixed` still forces the fixed form if P1 prefers it.

Build-time feature facts that follow from the **5.4 UAPI headers** in the pin
(`build-x86_64/include/config.h`, sysroot greps):

| Feature | Compiled? | Why | Consequence |
|---|---|---|---|
| ntsync (`/dev/ntsync`) | **no** — `/* #undef HAVE_LINUX_NTSYNC_H */` | 5.4 has no `linux/ntsync.h` | in-server sync only (AD-8 for free); `server/inproc_sync.c:243-292` stubs |
| userfaultfd write-watch | **no** — `USE_UFFD_WRITEWATCH` needs `UFFD_FEATURE_WP_ASYNC` + `PM_SCAN_WP_MATCHING` (`virtual.c:79-80`); both absent (grep count 0) | 5.4 headers | write watches use `mprotect` + SIGSEGV write faults (needs the `err` bit, §4.4) |
| `MAP_FIXED_NOREPLACE` | **yes** (`0x100000`, `asm-generic/mman-common.h:29`) | 4.17+ header | §0 |
| `AT_HWCAP2` FSGSBASE check | **yes** (`bits/auxv.h:61`; compile probe with Wine's `config.h` defines it; `HAVE_SYS_AUXV_H 1`) | | §4.2 |
| `getrandom` | yes (`HAVE_GETRANDOM 1`; `ntdll.so` imports `getrandom@GLIBC_2.25`) | | syscall 318 |
| inotify | no (`--without-inotify`) | console phase | none in trace |

### 1.1 GLIBC_ symbol versions (the AD-2 check)

`objdump -T` over every staged ELF (`$WORK/audit.txt`); **maximum
`GLIBC_2.29`**, below the 2.31 pin, so the pin needs no bump:

| object | max GLIBC_ | symbols at the max |
|---|---|---|
| `x86_64-unix/win32u.so` | 2.29 | `pow`, `log` |
| `x86_64-unix/ntdll.so` | 2.25 | `getrandom` (also `clock_gettime`/`clock_getres` @2.17) |
| `bin/wineserver` | 2.17 | `clock_gettime` |
| `x86_64-unix/msv1_0.so` | 2.15 | |
| 16 others | 2.14 | |
| 11 others | 2.2.5 | |
| `x86_64-unix/wine-preloader` | none | no DT_NEEDED at all |

`libgcc_s.so.1` (needed by `ntdll.so` — unwinding, since `--without-unwind`)
is required at `GCC_3.0` only; the pinned libgcc-s1 10.5 satisfies it.

Wine's own stated minimum: "Linux version 2.6.22 or later" (`README.md:45`);
no glibc minimum is stated anywhere in the tree — the measured requirement is
the table above.

### 1.2 Staged layout (proposal for F4)

```
/CP/WINE/bin/{wine,wineserver,wineboot->wine,winecfg,…}   wine = loader front (PIE, interp /lib64/ld-linux-x86-64.so.2)
/CP/WINE/lib/wine/x86_64-unix/{wine,wine-preloader,ntdll.so,win32u.so,…}  25 Unix .so + 2 executables
/CP/WINE/lib/wine/x86_64-windows/*.{dll,exe,sys,drv,…}   PE builtins, 757 files
/CP/WINE/share/wine/{wine.inf,nls/*.nls}
```

Plus, outside `/CP/WINE`, exactly the RimWorld runtime: `/lib64/ld-linux-x86-64.so.2`
and `/lib/x86_64-linux-gnu/{libc,libm,libdl,libpthread,librt,libresolv}.so.*`
from libc6 2.31, and `libgcc_s.so.1` from libgcc-s1 10.5. Wine is
relocatable at runtime (it re-derives paths from `/proc/self/exe`; the strace
shows `readlink("/proc/self/exe")` first), but `--prefix=/CP/WINE` makes the
compiled-in paths correct anyway. Size 202 MiB (512 MiB of it was DWARF before
`--strip-debug`; the unstripped objects stay in the build tree).

---

## 2. Host proof: Wine on the pinned 2.31 runtime

`tools/cp/run-wine-host.sh` runs the staged tree in a chroot whose root holds
only libc6 2.31, libgcc-s1 10.5 and `/CP/WINE` (private mount namespace,
`/dev` rbind, fresh `/proc`, run as the calling user). Scoped
`LD_LIBRARY_PATH` (RimWorld's Appendix B method) cannot work for Wine: the
trace shows the loader chain `execve("/CP/WINE/bin/wineboot")` →
`execve(".../i386-unix/wine-preloader") = -1 ENOENT` →
`execve(".../x86_64-unix/wine-preloader", [preloader, wine, "wineboot", "-i"])`
→ wineserver and every `.exe` re-exec'd by path, each with
`PT_INTERP=/lib64/ld-linux-x86-64.so.2`. The maps of every Windows process
show `libc-2.31.so`, `ld-2.31.so`, `libpthread-2.31.so` from the chroot (§5).

```
$ bash tools/cp/run-wine-host.sh -- wineboot -i            # fresh prefix in $WORK/hostroot/tmp/pfx
wine: created the configuration directory '/tmp/pfx'
wine: failed to start L"C:\\windows\\syswow64\\rundll32.exe": c0000135
run-wine-host: exit status 0
real	0m18.991s

$ bash tools/cp/run-wine-host.sh --warm -- wine cmd /c echo hello
hello
run-wine-host: exit status 0
real	0m2.479s
```

The prefix afterwards: `.update-timestamp dosdevices drive_c system.reg (1831150 B) user.reg userdef.reg`;
`dosdevices/c: -> ../drive_c`, `z: -> /`, `com1..com32 -> /dev/ttyS*` (symlinks).
The `syswow64\rundll32.exe` failure (`c0000135` = DLL not found) is expected
in a 64-bit-only build (AD-1): `wine.inf` also runs the 32-bit setup step.
With `WINEDEBUG=err+all,fixme-all` the cold `wineboot -i` additionally logs
the console-only build's expected errors (`Wine was built without Vulkan
support`, `nodrv_CreateWindow … The graphics driver is missing`, `getaddrinfo
Failed to resolve your host name IP`) and exits 0 (`evidence/wineboot-cold.txt`).

One unreproduced anomaly: after the scripted `cmd /c pause` run the wineserver
printed `could not save registry branch to system.reg : No such file or
directory` (×3). A manual repeat of the same command saved all three `.reg`
files (mtimes advanced) with no message. Recorded, not explained.

---

## 3. Syscalls (P2 list)

Regenerate: `bash tools/cp/p0-wine-evidence.sh` (writes
`$CP_WINE_WORK/evidence/`; raw traces are 209 MB / 60 MB and are not
committed). Counting rule (`tools/cp/strace-summary.py`): a task is Wine-side
from its first successful `execve` of a `/CP/…` path, plus every task cloned
from a Wine-side task; the host `env`/`chroot` used to enter the root are
excluded (the plain `strace -c` tables in §3.4 include their ~20 calls).
HobbyOS coverage is derived from the tree: every `{L_SYS_<name>, fn, "…"}` row
in `src/kernel/linux_compat*.c` (131 names; `l_tab_core/fd/mem/proc/misc/thr/tmr`,
dispatched by `linux_compat_dispatch`, `src/kernel/linux_compat.c:325-387`) plus
the `case <nr>:` arms of the fallback switch in `src/kernel/arch/x64/trap.c:2905-3129`
(49 numbers, mapped to names through the host's `asm/unistd_64.h`); anything
else reaches `linux_compat_unhandled` → `-ENOSYS` (`trap.c:3129`).

Scenarios: (a) `wineboot -i` into a fresh prefix — 1,037,446 Wine-side
events, 128 tasks (services.exe 49, winedevice.exe 20, wineboot.exe 14,
rpcss.exe 9, plugplay.exe 8, wineboot loader 6, rundll32.exe 6,
winemenubuilder.exe 6, explorer.exe 5, svchost.exe 4, wineserver 1);
(b) `wine cmd /c echo hello` with a warm prefix — 232,665 events, 72 tasks.
Union: **92 distinct syscalls, 81 have a HobbyOS handler entry, 11 have none.**

### 3.1 Missing from HobbyOS, ranked by first use

Union of (a)+(b), first use in the cold `wineboot -i`:

| rank | syscall | nr | calls | first use | who / why (from the trace) |
|---|---|---|---|---|---|
| 1 | `symlink` | 88 | 66 | +0.05 s, wineboot loader | prefix creation: `dosdevices/c:`, `z:`, `com*` |
| 2 | `chmod` | 90 | 2 | +0.10 s, wineserver | `chmod("socket", 0600)` on the server socket, result ignored (`server/request.c:831`) |
| 3 | `fchdir` | 81 | 588 | +0.11 s, wineboot | **every directory listing**: `NtQueryDirectoryFile` does `fchdir(dir)` … `fchdir(cwd)` and returns `errno_to_status` if the first fails (`dlls/ntdll/unix/file.c:2914-2934`); also the server's path lookups (`server/fd.c:1948`, `:2091`, `:2097`), temp-file dir choice (`server/mapping.c:374-378`), `server.c:1462` |
| 4 | `fstatfs` | 138 | 5881 | +0.11 s, wineserver | `is_device_removable` on every opened file (`server/fd.c:1010-1021`; failure → "not removable", harmless); ntdll volume queries return `errno_to_status` on failure (`dlls/ntdll/unix/file.c:2194`) |
| 5 | `ftruncate` | 77 | 233 | +0.11 s, wineserver | `grow_file` for every anonymous section: `pwrite` one byte past the size, then `ftruncate` whose result is **ignored** (`server/mapping.c:306-325`) — ENOSYS leaves each section file 1 byte long-er, not fatal |
| 6 | `sendmmsg` | 307 | 6 | +1.03 s, wineboot.exe | DNS query to 127.0.0.1:53 (getaddrinfo) — tolerable as ENOSYS |
| 7 | `getxattr` | 191 | 2832 | +1.15 s, wineboot.exe | `user.WINEREPARSE` / DOS attributes; all `ENODATA` here |
| 8 | `fgetxattr` | 193 | 4082 | +2.07 s, wineboot.exe | same (all `ENODATA`) |
| 9 | `fsetxattr` | 190 | 3 | +2.10 s, wineboot.exe | storing DOS attributes |
| 10 | `utimensat` | 280 | 90 | +69 s, rundll32.exe | file times on installed files |
| 11 | `fremovexattr` | 199 | 90 | +69 s, rundll32.exe | (all `ENODATA`) |

Warm `cmd /c echo hello` alone needs 8 of them, in this order: `chmod`,
`fchdir`, `fstatfs`, `ftruncate`, `getxattr`, `fgetxattr`, `sendmmsg`,
`symlink` (winedevice.exe recreating `com*` links).

### 3.2 Present by name, wrong or missing in behaviour (read from the tree)

These have a handler, so the name-diff above calls them covered; the trace
proves Wine depends on the sub-behaviour. Ranked by first use in (a):

| first use | call (as traced) | HobbyOS today | Wine's reaction | owner |
|---|---|---|---|---|
| +0.02 s | `execve(wine-preloader)` — a static-PIE (ET_DYN, no INTERP, `LOAD` at vaddr 0) | supported by the image parser (`src/kernel/elf_image.c:397`); placement inside the 64–96 GiB window | needs a placement that avoids its own reserve list | P1 |
| +0.02 s | `mmap(0x10000 / 0x110000 / 0x7f000000 / 0x7ffffe000000, …, MAP_FIXED, PROT_NONE)` — 1 MiB, 1.66 GiB, 16 MiB, 32 MiB | refused: user VA is `[64 GiB, 96 GiB)` (`src/include/vm.h:27-29`), `l_fixed_ok` (`linux_compat_mem.c:214-216`) | warns and drops each range (`preloader.c:1456`) — non-fatal | P1 (F1) |
| +0.04 s | `mmap(…, MAP_FIXED_NOREPLACE)` (1335 calls; first = address-space probe) | **`-EINVAL` unconditionally** (`linux_compat_mem.c:195-196`) | host limit computed as 4 GiB; later allocations fail (§0) | **P1, first fix** |
| +0.05 s | `mmap(0x7ffe0000, 4096, PROT_READ, MAP_PRIVATE\|MAP_FIXED\|MAP_ANONYMOUS)` then `mmap(0x7ffe0000, 4096, PROT_READ, MAP_SHARED\|MAP_FIXED, <tmpmap fd>)` | address below `USER_VA_BASE` | `virtual_alloc_first_teb`: `ERR "failed to map the shared user data"` + `exit(1)` (`virtual.c:4046-4051`); remap failure also `exit(1)` (`:4406-4410`) | P1 — **fatal** |
| +0.10 s | `fcntl(lock, F_SETLK, F_WRLCK)` on `server-…/lock` | `K_F_SETLK` exists (tested in `src/kernel/fs_test.c:193-216`); `F_SETLKW` "degrades to one non-blocking attempt" | wineserver single-instance lock | check on tmpfs/ext2 (P3) |
| +0.10 s | `socket(AF_UNIX)` + `bind` + `listen` + `chmod` in `/tmp/.wine-<uid>/server-<dev>-<ino>/` | AF_UNIX present (`src/kernel/fs.c:1026`) | needs a writable, chmod-able `/tmp` | P2/P3 |
| +0.11 s | `sendmsg/recvmsg` with `SCM_RIGHTS` — **4721 fd passes** in (a) | present (§3.2 of the plan) | the wineserver protocol: every handle that maps to a Unix fd | prove at scale (P2) |
| +0.12 s | `mmap(…, MAP_SHARED[\|MAP_FIXED], fd)` of server-made temp files (`tmpmap-XXXXXXXX`, created `O_CREAT\|O_EXCL`, grown by `pwrite` past EOF + `ftruncate`, **unlinked while open**) — 37 `MAP_SHARED\|MAP_FIXED` + 6 `MAP_SHARED` | `VM_OBJ_MAX_BYTES` 64 MiB; shared file mappings on FAT/NFS? | `KUSER_SHARED_DATA` itself is one of these (§4.3): coherent server-writes → client-reads are mandatory | P2/P3 (tmpfs with shared file pages) |
| +0.12 s | `prctl(PR_SET_PTRACER, …)` ×34 | `-EINVAL` (default arm, `linux_compat_proc.c:1100-1101`) | return value ignored | none |
| +0.12 s | `sigaltstack({ss_sp=0x7ffc3800, ss_size=51200})` — per thread, **inside the TEB block below 2 GiB** | present | handlers are `SA_ONSTACK` (§4.4) | P1 (low VA) |
| +0.30 s | `arch_prctl(ARCH_SET_GS, teb)` ×155 (one per thread; TEB e.g. `0x7ffc0000`) | **`-EINVAL`** (`linux_compat_thr.c:2447-2477` handles only SET_FS/GET_FS/CPUID) | return value ignored → `%gs:0x30` faults on first use | **P1 — fatal** |
| +0.02 s | `arch_prctl(0x3001 /* ARCH_CET_STATUS */)` ×38 → `EINVAL` on Linux too | `-EINVAL` | glibc 2.31 CET probe; EINVAL is the right answer | none |
| +1.0 s | `socket(AF_INET, SOCK_DGRAM)`, `socket(AF_NETLINK, SOCK_RAW)`, `ioctl(SIOCGIF*)` on UDP, `/proc/net/dev` | AF_INET partial, no AF_NETLINK (`fs.c:1057` → `-EAFNOSUPPORT`) | nsiproxy/iphlpapi enumerate interfaces; failure tolerated in this run's paths (not proven) | P2, verify |
| — | `ioctl` set: `VFAT_IOCTL_READDIR_BOTH` (2839, all ENOTTY on Linux too), `FS_IOC_GETFLAGS` (4149 ok), `TCGETS` (64 ENOTTY) | `l_fd_ioctl` (`linux_compat_fd.c:606-`) answers `FIONBIO`/`FIONREAD`/…; the legacy arm answers `-ENOTTY` for the rest except fb nodes (`trap.c:~3100-3125`) | ENOTTY for these three is a valid answer (READDIR_BOTH → getdents64 fallback; GETFLAGS → "not casefolded") — **verify** each in P2 | none expected |

### 3.3 Sub-command inventories (union of (a)+(b))

`arch_prctl`: `ARCH_SET_FS` 72 ok, `ARCH_SET_GS` 155 ok, `0x3001` 38 EINVAL.
No `ARCH_GET_GS`/`ARCH_GET_FS` and no `rdgsbase`/`wrgsbase` path was taken (no
faults occurred; §4.2).

`prctl`: `PR_SET_NAME` 67, `PR_SET_PTRACER` 34. Thread names are also written
through `/proc/<pid>/task/<tid>/comm` (82 opens, services.exe).

`clone` flags: threads `CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID` (122);
processes `CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD` (74, fork);
`CLONE_VM|CLONE_VFORK|SIGCHLD` (2, posix_spawn). No `clone3` (glibc 2.31).

`futex`: `FUTEX_WAKE_PRIVATE` 888, `FUTEX_WAIT_PRIVATE` 666 (+31 EAGAIN, 12
ETIMEDOUT), `FUTEX_WAIT` (shared) 3. No `futex_waitv`, no PI, no requeue.

`fcntl`: `F_SETFD` 10816, `F_GETFL` 6551, `F_SETFL` 314, `F_SETLK` 2.

`socket`/`socketpair`: `AF_UNIX SOCK_STREAM` (socketpair 38, socket 10),
`AF_INET SOCK_DGRAM[|CLOEXEC|NONBLOCK]` 12, `AF_NETLINK SOCK_RAW` 3.
`setsockopt`: `SOL_SOCKET SO_PASSCRED` 72 (HobbyOS has `K_SO_PASSCRED`,
`fs.c:1757`), `SOL_IP IP_RECVERR` 6. `getsockopt`: none.

`ioctl`: `FS_IOC_GETFLAGS` 4149 ok, `VFAT_IOCTL_READDIR_BOTH` 2839 ENOTTY,
`TCGETS` 64 ENOTTY, `SIOCGIFHWADDR`/`SIOCGIFFLAGS`/`SIOCGIFMTU` 3 each ok,
`SIOCGIWNAME` 2 EOPNOTSUPP.

`mmap` flag sets: `MAP_PRIVATE|MAP_FIXED` (file) 6347, `…|MAP_FIXED|MAP_ANONYMOUS`
1528, `…|MAP_FIXED_NOREPLACE` 757 ok + 578 ENOMEM, `…|MAP_FIXED|MAP_DENYWRITE`
789, `…|MAP_FIXED|MAP_ANONYMOUS|MAP_NORESERVE` 416, `MAP_PRIVATE|MAP_ANONYMOUS` 292,
`MAP_SHARED|MAP_FIXED` 37, `MAP_SHARED` 6, `MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE` 80
(glibc per-thread malloc arenas: 128 MiB then 64 MiB `PROT_NONE` reservations,
e.g. `0x4000000` at `0x71306c000000`), `MAP_STACK` 2. `madvise`: `MADV_DONTNEED` 110.
`prlimit64`: `RLIMIT_NOFILE` 72, `RLIMIT_NICE` 72, `RLIMIT_AS` 68, `RLIMIT_STACK` 36, `RLIMIT_CORE` 2.

Signals actually delivered in the traces: `SIGQUIT` and `SIGUSR1` with
`si_code=SI_TKILL` from wineserver (`tgkill`, 29 calls; first
`tgkill(…, SIGQUIT)` at thread teardown), `SIGCHLD`. No SIGSEGV/SIGILL/SIGTRAP
occurred in these console workloads.

`rt_sigaction`: the Windows-side handlers (`SIGINT SIGFPE SIGABRT SIGQUIT
SIGUSR1 SIGTRAP SIGSEGV SIGILL SIGBUS`, 33 processes each) are all installed
with `SA_ONSTACK` (and `SA_SIGINFO|SA_RESTART`, `signal_x86_64.c:2862-2863`).

Pseudo-files opened: `/proc/self/exe` (readlink), `/proc/stat`,
`/proc/cpuinfo`, `/proc/<n>/task/<n>/comm`, `/proc/net/dev`, `/dev/null`,
`/dev/ttyS0..31` (`access`, for `com*` links); `/sys/devices/system/cpu/*`,
`/sys/class/dmi/id/*`, `/sys/class/net/*/carrier` were all `ENOENT` (no
`/sys` in the chroot) and Wine carried on — HobbyOS may leave `/sys` absent.

Hot paths (counts): `read` 335,715 and `write` 163,802 (the wineserver
request/reply pipes), **`rt_sigprocmask` 332,142** (every `wine_server_call`
brackets the request with `SIG_BLOCK`/`SIG_SETMASK`,
`dlls/ntdll/unix/server.c:308-310`), `epoll_wait` 118,334 (wineserver main
loop), `writev` 86,318, `lstat` 36,781.

### 3.4 Raw `strace -f -c -w` (all processes, incl. ~20 host env/chroot calls)

(a) `wineboot -i`, fresh prefix — top of `evidence/strace-wineboot.c.txt`:

```
syscall                calls    errors     seconds  usecs/call
------------------ --------- --------- ----------- -----------
read                  263436         2 1691.835176        6422
futex                    928        39  194.774069      209885
epoll_wait             94754             53.338288         562
recvfrom                   2         1   22.266406    11133203
rt_sigprocmask        263019             14.594275          55
write                 131122              9.543433          72
writev                 66307              4.417589          66
lstat                  31320        29    1.977225          63
getdents64             12474              1.544588         123
close                  22064        86    1.368920          62
openat                 18199      1316    1.300463          71
newfstatat             18341      9316    1.138625          62
fstat                  16256        19    0.935156          57
fcntl                  16184              0.875627          54
mprotect                8823              0.609776          69
mmap                    7252       357    0.535913          73
pread64                 7466        23    0.454486          60
ioctl                   6652      2716    0.359492          54
fstatfs                 5626              0.335523          59
recvmsg                 3816         3    0.304624          79
sendmsg                 3813              0.297661          78
fgetxattr               4074      4074    0.246453          60
...
total                1015144     21608 2004.161021        1974
```

(b) `wine cmd /c echo hello`, warm prefix — top of `evidence/strace-echo.c.txt`:

```
syscall                calls    errors     seconds  usecs/call
------------------ --------- --------- ----------- -----------
read                   65806         2  432.984217        6579
futex                    567        32   57.148619      100791
recvfrom                   2         1   12.774895     6387447
epoll_wait             21717              9.403221         432
rt_sigprocmask         63056              3.696997          58
select                   299              2.898315        9693
write                  29733              2.093948          70
writev                 18070              1.249031          69
lstat                   5097        19    0.303597          59
mprotect                3855              0.271180          70
mmap                    3597       187    0.260476          72
...
total                 233225      2445  524.790426        2250
```

(Wall time is dominated by blocking `read`s on the server pipes; the counts,
not the seconds, are the P2 workload. The -c run and the raw run are separate
runs, so counts differ slightly from §3.5.)

### 3.5 Full histogram by first use (union), with HobbyOS coverage

Generated by `tools/cp/strace-summary.py --tree . strace-wineboot.raw strace-echo.raw`.
Times are wall-clock of the cold `wineboot -i` run (start 03:29:30.760).

| # | syscall | nr | calls | errors | first use | HobbyOS |
|---|---|---|---|---|---|---|
| 1 | `execve` | 59 | 48 | ENOENT×10 | 03:29:30.760516 wineboot | legacy trap.c:3043 |
| 2 | `brk` | 12 | 248 | - | 03:29:30.762844 wineboot | linux_compat_mem.c:665 |
| 3 | `arch_prctl` | 158 | 265 | EINVAL×38 | 03:29:30.763162 wineboot | linux_compat_thr.c:4091 (partial, §3.2) |
| 4 | `access` | 21 | 111 | ENOENT×46 | 03:29:30.763364 | linux_compat_proc.c:1492 |
| 5 | `openat` | 257 | 20571 | ENOENT×2113 | 03:29:30.763767 | linux_compat_fd.c:881 |
| 6 | `stat` | 4 | 2816 | ENOENT×1331 | 03:29:30.764280 | linux_compat_proc.c:1492 |
| 7 | `read` | 0 | 335715 | - | 03:29:30.767722 | legacy trap.c:2917 |
| 8 | `fstat` | 5 | 18570 | EBADF×31 | 03:29:30.767847 | linux_compat_proc.c:1492 |
| 9 | `mmap` | 9 | 11165 | ENOMEM×578 | 03:29:30.767965 | linux_compat_mem.c:665 (partial, §3.2) |
| 10 | `close` | 3 | 25456 | EBADF×120 | 03:29:30.768608 | legacy trap.c:2926 |
| 11 | `pread64` | 17 | 10601 | EISDIR×38, EBADF×18 | 03:29:30.768968 | linux_compat_misc.c:277 |
| 12 | `mprotect` | 10 | 13073 | - | 03:29:30.770947 | linux_compat_mem.c:665 |
| 13 | `lstat` | 6 | 36781 | ENOENT×52 | 03:29:30.771738 | linux_compat_proc.c:1492 |
| 14 | `readlink` | 89 | 3557 | ENOENT×52 | 03:29:30.771970 | linux_compat_proc.c:1492 |
| 15 | `set_tid_address` | 218 | 36 | - | 03:29:30.778080 | linux_compat.c:261 |
| 16 | `set_robust_list` | 273 | 230 | - | 03:29:30.778200 | linux_compat_thr.c:4092 |
| 17 | `rt_sigaction` | 13 | 707 | - | 03:29:30.778333 | linux_compat_proc.c:1492 |
| 18 | `rt_sigprocmask` | 14 | 332142 | - | 03:29:30.778652 | linux_compat_proc.c:1492 |
| 19 | `prlimit64` | 302 | 250 | - | 03:29:30.778785 | linux_compat_proc.c:1492 |
| 20 | `open` | 2 | 68 | - | 03:29:30.781891 | legacy trap.c:2925 |
| 21 | `prctl` | 157 | 101 | - | 03:29:30.784570 | linux_compat_proc.c:1492 (PR_SET_PTRACER → EINVAL) |
| 22 | `futex` | 202 | 1600 | EAGAIN×31, ETIMEDOUT×12 | 03:29:30.797103 | linux_compat_thr.c:4089 |
| 23 | `munmap` | 11 | 494 | EINVAL×33 | 03:29:30.804365 | linux_compat_mem.c:665 |
| 24 | `chdir` | 80 | 10 | ENOENT×2 | 03:29:30.809623 | linux_compat_misc.c:277 |
| 25 | `getuid` | 102 | 29 | - | 03:29:30.809903 | linux_compat.c:261 |
| 26 | `mkdir` | 83 | 150 | EEXIST×2 | 03:29:30.810019 | legacy trap.c:3052 |
| 27 | `write` | 1 | 163802 | - | 03:29:30.810356 | legacy trap.c:2918 |
| 28 | `symlink` | 88 | 66 | - | 03:29:30.811290 | **MISSING** |
| 29 | `fcntl` | 72 | 17685 | - | 03:29:30.811628 | linux_compat_fd.c:912 |
| 30 | `clone` | 56 | 198 | - | 03:29:30.812170 | linux_compat_thr.c:4088 |
| 31 | `wait4` | 61 | 38 | - | 03:29:30.846745 | legacy trap.c:3044 |
| 32 | `socketpair` | 53 | 38 | - | 03:29:30.855864 wineserver | linux_compat_fd.c:886 |
| 33 | `shutdown` | 48 | 2 | - | 03:29:30.856297 | linux_compat_fd.c:896 |
| 34 | `poll` | 7 | 20 | - | 03:29:30.856452 | linux_compat_fd.c:908 |
| 35 | `pipe` | 22 | 46 | - | 03:29:30.860168 | legacy trap.c:3016 |
| 36 | `setsid` | 112 | 31 | - | 03:29:30.860935 | linux_compat_proc.c:1492 |
| 37 | `unlink` | 87 | 88 | ENOENT×34 | 03:29:30.862670 | linux_compat_misc.c:277 |
| 38 | `socket` | 41 | 25 | - | 03:29:30.862811 | linux_compat_fd.c:885 |
| 39 | `bind` | 49 | 5 | - | 03:29:30.863119 | linux_compat_fd.c:887 |
| 40 | `chmod` | 90 | 2 | - | 03:29:30.863403 | **MISSING** |
| 41 | `listen` | 50 | 2 | - | 03:29:30.863594 | linux_compat_fd.c:889 |
| 42 | `epoll_create` | 213 | 2 | - | 03:29:30.863851 | linux_compat_timers.c:716 |
| 43 | `epoll_ctl` | 233 | 409 | - | 03:29:30.863999 | linux_compat_timers.c:718 |
| 44 | `dup2` | 33 | 68 | - | 03:29:30.864154 | linux_compat_fd.c:903 |
| 45 | `exit_group` | 231 | 78 | - | 03:29:30.864876 | legacy trap.c:2987 |
| 46 | `setsockopt` | 54 | 78 | - | 03:29:30.866558 | linux_compat_fd.c:894 |
| 47 | `connect` | 42 | 14 | ENOENT×6 | 03:29:30.866762 | linux_compat_fd.c:888 |
| 48 | `fchdir` | 81 | 588 | - | 03:29:30.866959 | **MISSING** |
| 49 | `recvmsg` | 47 | 4729 | EAGAIN×5 | 03:29:30.867350 | linux_compat_fd.c:900 |
| 50 | `fstatfs` | 138 | 5881 | - | 03:29:30.869748 wineserver | **MISSING** |
| 51 | `pwrite64` | 18 | 6 | - | 03:29:30.871471 | linux_compat_misc.c:277 |
| 52 | `ftruncate` | 77 | 233 | - | 03:29:30.871632 wineserver | **MISSING** |
| 53 | `getpid` | 39 | 134 | - | 03:29:30.874042 | linux_compat.c:261 |
| 54 | `setpriority` | 141 | 4 | EACCES×2 | 03:29:30.874130 | linux_compat_proc.c:1492 |
| 55 | `epoll_wait` | 232 | 118334 | - | 03:29:30.876041 | linux_compat_timers.c:719 |
| 56 | `accept` | 43 | 2 | - | 03:29:30.876151 | linux_compat_fd.c:890 |
| 57 | `sendmsg` | 46 | 4721 | - | 03:29:30.876907 | linux_compat_fd.c:899 |
| 58 | `sigaltstack` | 131 | 156 | - | 03:29:30.877818 | linux_compat_proc.c:1492 |
| 59 | `pipe2` | 293 | 438 | - | 03:29:30.877947 | linux_compat_fd.c:905 |
| 60 | `gettid` | 186 | 156 | - | 03:29:30.878699 | linux_compat.c:261 |
| 61 | `sched_getaffinity` | 204 | 2 | - | 03:29:30.879304 | linux_compat_proc.c:1492 |
| 62 | `writev` | 20 | 86318 | - | 03:29:30.879406 | legacy trap.c:2990 |
| 63 | `umask` | 95 | 68 | - | 03:29:30.895403 | linux_compat_proc.c:1492 |
| 64 | `getcwd` | 79 | 2 | - | 03:29:30.903674 | linux_compat_misc.c:277 |
| 65 | `sysinfo` | 99 | 58 | - | 03:29:30.924722 | linux_compat_proc.c:1492 |
| 66 | `newfstatat` | 262 | 19802 | ENOENT×9799 | 03:29:30.925972 | linux_compat_proc.c:1492 |
| 67 | `dup` | 32 | 1111 | - | 03:29:30.929586 | linux_compat_fd.c:902 |
| 68 | `ioctl` | 16 | 7063 | ENOTTY×2903, EOPNOTSUPP×2 | 03:29:30.931696 | linux_compat_fd.c:911 |
| 69 | `getdents64` | 217 | 13121 | - | 03:29:30.932596 | legacy trap.c:2996 |
| 70 | `sched_setaffinity` | 203 | 154 | - | 03:29:30.986692 | linux_compat_proc.c:1492 |
| 71 | `uname` | 63 | 39 | - | 03:29:31.066874 wineboot.exe | legacy trap.c:3006 |
| 72 | `sendmmsg` | 307 | 6 | - | 03:29:31.789114 | **MISSING** |
| 73 | `sendto` | 44 | 7 | - | 03:29:31.789452 | linux_compat_fd.c:897 |
| 74 | `lseek` | 8 | 746 | - | 03:29:31.913221 | linux_compat_misc.c:277 |
| 75 | `getxattr` | 191 | 2832 | ENODATA×2832 | 03:29:31.914526 | **MISSING** |
| 76 | `fgetxattr` | 193 | 4082 | ENODATA×4082 | 03:29:32.833337 | **MISSING** |
| 77 | `fsetxattr` | 190 | 3 | - | 03:29:32.861846 | **MISSING** |
| 78 | `getrandom` | 318 | 26 | - | 03:29:34.724082 explorer.exe | legacy trap.c:3021 |
| 79 | `getrusage` | 98 | 610 | - | 03:29:35.456916 | linux_compat_proc.c:1492 |
| 80 | `sched_yield` | 24 | 305 | - | 03:29:35.457062 | linux_compat.c:261 |
| 81 | `madvise` | 28 | 110 | - | 03:29:35.464631 | linux_compat_mem.c:665 |
| 82 | `exit` | 60 | 116 | - | 03:29:35.464741 | linux_compat_thr.c:4090 |
| 83 | `kill` | 62 | 34 | ESRCH×34 | 03:29:36.909286 wineserver | linux_compat_proc.c:1492 |
| 84 | `rename` | 82 | 14 | - | 03:30:00.904122 | linux_compat_misc.c:277 |
| 85 | `utimensat` | 280 | 90 | - | 03:30:39.868015 rundll32.exe | **MISSING** |
| 86 | `fchmod` | 91 | 90 | - | 03:30:39.868342 | linux_compat_fd.c:882 |
| 87 | `fremovexattr` | 199 | 90 | ENODATA×90 | 03:30:39.868525 | **MISSING** |
| 88 | `recvfrom` | 45 | 4 | - | 03:31:12.255153 winedevice.exe | linux_compat_fd.c:898 |
| 89 | `tgkill` | 234 | 29 | - | 03:31:15.308555 wineserver | linux_compat_proc.c:1492 |
| 90 | `rt_sigreturn` | 15 | 5 | - | 03:31:45.388004 plugplay.exe | linux_compat_proc.c:1492 |
| 91 | `select` | 23 | 300 | - | 03:31:45.424631 | linux_compat_fd.c:909 |
| 92 | `getsockname` | 51 | 1 | - | 03:33:37.675755 winedevice.exe | linux_compat_fd.c:892 |

---

## 4. Wine source facts at the pin (open questions 2 and 3, and the rest of item 6)

### 4.1 Open question 2 — preloader ranges, and is it fixed-address?

- **Reserved ranges on x86_64** (`loader/preloader.c:107-122`):
  `0x000000010000 +0x00100000` "DOS area" (`:115`),
  `0x000000110000 +0x67ef0000` "low memory area" → up to `0x68000000` (`:116`),
  `0x00007f000000 +0x00ff0000` "32-bit top-down allocations + shared user data"
  → up to `0x7fff0000` (`:117`),
  `0x7ffffe000000 +0x01ff0000` "top-down allocations + virtual heap" (`:118`),
  plus one slot for the main PE image range passed in `WINEPRELOADRESERVE`
  (`:120`; set by `exec_wineloader` from the image's base/size,
  `dlls/ntdll/unix/loader.c:546-559`; parsed by `preload_reserve`,
  `preloader.c:1276-1326`). The first 64 KiB is *not* reserved on x86_64 (the
  i386 table's `low 64k` row is `#if`'d out, `:109-113`).
- Each is mapped `PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON|MAP_NORESERVE`;
  failure = warning + range dropped; a range containing the initial stack is
  skipped (`preloader.c:1437-1461`). The `0x80000000 - page` executable page
  trick applies only if that page is reserved (`:1463-1466`) — not on x86_64.
- ntdll later records them as reserved areas (`virtual.c:3666-3668`), maps the
  DOS area `PROT_READ|PROT_WRITE` (`virtual.c:3704-3707`; seen in strace as
  `mmap(0x10000, 0x100000, PROT_READ|PROT_WRITE, MAP_FIXED…)`), and releases
  `[0, 0x7ffe0000)` for high-entropy 64-bit exes (`virtual.c:5022-5031`).
- **Is the preloader a fixed-address executable?** It depends on the
  toolchain, by `configure.ac:1046-1051`: on x86_64 Linux it is linked
  `-static-pie` if the compiler can, otherwise `-static -Wl,-Ttext=0x7d7d00000000`
  (non-PIE, fixed). With the pinned sysroot (focal libgcc 10) it is
  **static-PIE** (`readelf -h`: `Type: DYN (Position-Independent Executable
  file)`, entry `0x1000`, four `LOAD`s from vaddr 0, no `INTERP`, no
  `DT_NEEDED`); the host kernel placed it at `0x7d25e5919000`. With gcc 15's
  libgcc in the sysroot it silently became the fixed `0x7d7d00000000` form
  (§1). Either way, neither form intersects `[0x70000000, 0x78000000)`; both
  need user VA above 96 GiB (F1).
- The preloader then maps the main binary (`lib/wine/x86_64-unix/wine`,
  PIE) and its interpreter itself (`preloader.c:1468-1474`), and rewrites
  auxv (`AT_PHDR/AT_PHENT/AT_PHNUM/AT_PAGESZ/AT_BASE/AT_FLAGS/AT_ENTRY`;
  drops `AT_SYSINFO*` if they fell inside a reserved range — `:1478-1500`).
  So HobbyOS's exec loads **only** the static-PIE; `ld.so` and `wine` are
  mapped by user code with ordinary `mmap(MAP_FIXED)` of the file.

### 4.2 Open question 3 — how the TEB is installed on x86_64 Linux

- **`arch_prctl(ARCH_SET_GS, teb)`, always**, once per thread in
  `init_syscall_frame` (`dlls/ntdll/unix/signal_x86_64.c:2907-2908`). Measured:
  155 `ARCH_SET_GS` calls (one per Windows thread), all with TEBs below 2 GiB
  (e.g. `0x7ffc0000`; the first TEB block is allocated top-down under
  `limit_2g`, `virtual.c:4054`; later blocks are unconstrained for 64-bit,
  `virtual.c:4088-4093`). The return value is not checked. Wine keeps
  `%gs = TEB` permanently in user mode — glibc owns `%fs`; ntdll's Unix side
  reads the TEB with `%gs:0x30` (`signal_x86_64.c:149`) and the syscall
  dispatcher with `%gs:0x378` / `%gs:0x30` (`:3028`, `:3059`).
- **`wrgsbase` is never used. `rdgsbase` is used in exactly one place**: the
  "invalid %gs" fault fix-up (`check_invalid_gsbase`, `signal_x86_64.c:2299-2386`),
  which reads the current GS base with `rdgsbase` if
  `ProcessorFeatures[PF_RDWRFSGSBASE_AVAILABLE]` else `arch_prctl(ARCH_GET_GS)`
  (`:2312-2316`), and restores it with `arch_prctl(ARCH_SET_GS)` (`:2377`).
  `wrfsbase`/`ARCH_SET_FS` (`:915`, `:3113-3126`, `:3405-3415`) only run when
  `fs32_sel != 0`, i.e. WoW64 32-bit processes (`:2852-2855`) — not in a
  64-bit-only build.
- **`AT_HWCAP2`**: `PF_RDWRFSGSBASE_AVAILABLE = CPUID.7.0:EBX[0]`
  (`dlls/ntdll/unix/system.c:511`) **AND** `getauxval(AT_HWCAP2) & 2`
  (`HWCAP2_FSGSBASE`, `system.c:518-519`; compiled in — §1). So HobbyOS must
  either not report `AT_HWCAP2` bit 1 (then Wine uses `ARCH_GET_GS`) or set
  `CR4.FSGSBASE` and report it. Reporting it without `CR4.FSGSBASE` turns the
  fault fix-up's `rdgsbase` into a `#UD` inside a SIGSEGV handler.
- HobbyOS today: `ARCH_SET_GS`/`ARCH_GET_GS` return `-EINVAL`
  (`src/kernel/linux_compat_thr.c:2447-2477`), and the plan's §3.2 notes user
  GS is not saved/restored. Required for P1: per-thread user GS base,
  `ARCH_SET_GS` + `ARCH_GET_GS`, no `AT_HWCAP2` FSGSBASE bit unless CR4 is set.

### 4.3 `KUSER_SHARED_DATA` at 0x7ffe0000

- Address constant: `virtual.c:205`. The server **creates** it as an anonymous
  section (`create_user_data_mapping`, `server/mapping.c:1495-1505`) backed by
  a temp file (`create_temp_file`, `server/mapping.c:362-392`: `tmpmap-%08x`
  in the server dir, `O_RDWR|O_CREAT|O_EXCL`, grown with `pwrite` one byte past
  the size then `ftruncate` (`grow_file`, `:306-325`), **unlinked while
  open**), maps it `PROT_WRITE, MAP_SHARED` in its own address space, and
  **updates** `SystemTime`, `InterruptTime`, `TickCount`(+`TickCountLowDeprecated`)
  and `TimeZoneBias` with plain stores (`server/fd.c:355-416`, x86 relies on
  TSO, `:362-372`) from its poll loop at least every
  `user_shared_data_timeout = 16 * 10000` 100-ns units = **16 ms**
  (`fd.c:356`, `:425`, `:898`).
- Each client: `virtual_alloc_first_teb` first allocates a private read-only
  page at `0x7ffe0000` (`virtual.c:4045-4051`; `exit(1)` on failure), then
  `virtual_map_user_shared_data` opens the section
  `\KernelObjects\__wine_user_shared_data`, receives its fd over the server
  socket (`SCM_RIGHTS`) and maps it `PROT_READ, MAP_SHARED|MAP_FIXED` over the
  same address (`virtual.c:4393-4413`; `exit(1)` on failure). The trace shows
  both `mmap`s in all 34 processes, and the maps show
  `7ffe0000-7ffe1000 r--s … tmpmap-474daabd (deleted)`.
  `virtual_init_user_shared_data` (wineboot only) maps it read-write to fill
  it (`:4422-…`). CPU features, including the XSAVE layout, are written into it
  by `init_shared_data_cpuinfo` (`system.c:485-536`).
- The page after it, `0x7ffe1000`, is a private RW page holding
  `__wine_syscall_dispatcher` (`signal_x86_64.c:2843-2846`).
- **HobbyOS requirement**: a genuinely shared, coherent page-cache page of an
  unlinked tmpfs file mapped `MAP_SHARED` at a fixed address in N processes,
  written by one; and the wineserver must run at least every 16 ms or
  `GetTickCount` stops advancing.

### 4.4 Signals Wine needs and with which semantics

Installed per process in `signal_init_process` (`signal_x86_64.c:2858-2880`),
all with `sa_mask = server_block_set` and `SA_SIGINFO|SA_RESTART|SA_ONSTACK`:
`SIGINT` (Ctrl-C thread), `SIGFPE`, `SIGABRT`, `SIGQUIT`, `SIGUSR1`,
`SIGTRAP`, `SIGSEGV`, `SIGILL`, `SIGBUS` (SEGV/ILL/BUS share `segv_handler`).
`server_block_set` includes `SIGUSR1`/`SIGUSR2` (`server.c:1631-1632`).

- **Alt stack**: one per thread, `ss_size = signal_stack_size = 0x10000 - 0x3800
  = 51,200 B` (`unix_private.h:155`), located at `(TEB & ~0xffff) + 0x3800`
  (`unix_private.h:424-427`) — i.e. inside the TEB block, below 2 GiB for the
  first 30 threads; set in `init_thread_pipe` (`server.c:1560-1565`; traced
  `sigaltstack({ss_sp=0x7ffc3800, ss_flags=0, ss_size=51200})`). The kernel
  frame (+XSAVE area) must fit in that with room for Wine's own CONTEXT copy.
- **SIGSEGV/SIGILL/SIGBUS** (`segv_handler`, `:2394-2476`) dispatch on
  `uc_mcontext.gregs[REG_TRAPNO]` (14 #PF, 13 #GP, 11 #NP, 12 #SS, 6 #UD,
  4 #OF, 5 #BR, 17 #AC) and `gregs[REG_ERR]`: for #PF the Windows access type is
  `(ERR >> 1) & 0x09` (write bit 1, instruction-fetch bit 4) and the address
  is `siginfo->si_addr` (`:2437-2439`); for #GP an `ERR` of 0 means
  privileged instruction, `(err&7)==2` is `int nn` (`handle_interrupt`). Page
  faults feed `virtual_handle_fault` (guard pages, write watches,
  stack growth). HobbyOS already writes `ERR`, `TRAPNO`, `CR2`
  (`src/kernel/linux_compat_thr.c:2925-2953`).
- **SIGTRAP** (`trap_handler`, `:2484-2510`): `TRAPNO` 1 → single step,
  3 → `int3` (address backed up by one).
- **SIGFPE** (`fpe_handler`, `:2518-2563`): `siginfo->si_code`
  (`FPE_INTDIV`, `FPE_INTOVF`, `FPE_FLTDIV`, …) chooses the exception;
  `TRAPNO` 19 (`#XM`) adds the MXCSR.
- **SIGUSR1** (`usr1_handler`, `:2622-…`): sent by wineserver with `tgkill`
  (`server/ptrace.c:222-226`, `:254-262`) to suspend a thread or fetch its
  context (`server/thread.c:960`, `:1450`, `:1467`); measured with
  `si_code=SI_TKILL`. It builds a CONTEXT on the kernel stack below
  `RSP - 128` (red zone) when inside a syscall.
- **SIGQUIT** (`quit_handler`, `:2608-2614`): wineserver kills a thread
  (`server/thread.c:1627`, "violent death"); measured.
- **SIGINT**: console Ctrl-C (`server/console.c:710`); handler spawns a thread.
- Every handler starts with `init_handler` (`:908-…`): clears `EFLAGS.AC`;
  on WoW64 resets FS. All rely on `ucontext->uc_mcontext.fpregs` (§4.5).

### 4.5 FP / XSAVE in the signal frame (for C1-cpu)

What Wine reads (`signal_x86_64.c:166-195`, `:1005-1018`, `:1045-1058`, `:1075`):

1. `uc_mcontext.fpregs` must point at a 512-byte FXSAVE image (copied whole
   into `CONTEXT.FltSave`; `MxCsr` taken from it). If the pointer is NULL Wine
   skips FP state entirely.
2. Extended state is recognised **only** if the 32-bit word at
   **`fpregs + 464`** (`((unsigned int*)Reserved4)[12]`, i.e. the first word of
   Linux's `_fpx_sw_bytes`) equals `FP_XSTATE_MAGIC1 = 0x46505853` (`:166-167`,
   `:195`). Then the XSAVE header is taken to be at **`fpregs + 512`** and is
   referenced *in place* as the `CONTEXT_EX` XState (`:1012-1017`; an `assert`
   checks the offset fits). Wine does **not** read `extended_size`,
   `xfeatures` or `xstate_size` from the sw_bytes, and does not look for
   `FP_XSTATE_MAGIC2`. Consequences for HobbyOS: (a) the 48 bytes at
   `fpregs+464` must be written (zeroed when no XSAVE area follows) — stale
   memory equal to the magic would make Wine read garbage; (b) the XSAVE area
   must start exactly at `fpregs+512` (Linux's layout: legacy area, header,
   then components at the **standard, non-compacted** CPUID.0xD offsets —
   Wine indexes components via `XState.Features[i].Offset` from CPUID and
   the header's `CompactionMask`, `system.c:275-325`, so `XCOMP_BV = 0`
   (standard format) is what it expects from a kernel frame).
3. Wine **writes back only the legacy 512 bytes** into the frame on
   `restore_context` (`:1075`); YMM state is restored by Wine itself with
   `xrstor64` in user mode (`:3205-3215`), not via `rt_sigreturn`.
4. Independently of signals, ntdll's syscall dispatcher executes
   **`xsavec64`** (if `CPUID.0xD.1:EAX[1]`), else **`xsave64`**, else
   `fxsave64`, and `xrstor64`/`fxrstor64` on the way back, in **user mode**,
   on every Win32 syscall (`:3062-3088`, `:3205-3215`), keyed on
   `PF_XSAVE_ENABLED = CPUID.1:ECX.OSXSAVE[27]` (`system.c:502`) and
   `EDX:EAX = XState.EnabledFeatures` = `CPUID.0xD.0` ∩ **`xgetbv(0)`** ∩
   {AVX, MPX, AVX-512} (`system.c:433-447`). So: with `CR4.OSXSAVE=0`
   (HobbyOS today) Wine cleanly uses FXSAVE. The moment C1-cpu sets
   `CR4.OSXSAVE`, Wine executes XSAVEC/XRSTOR with the kernel's XCR0 in user
   mode; the per-thread frame is sized from CPUID (`XState.Size`,
   `system.c:476-477`; `frame_size`, `signal_x86_64.c:2838-2839`).
   CPUID.0xD must therefore describe exactly the XCR0 the kernel programmed
   (masking AVX-512 at QEMU, AD-4, keeps it ~0x340 B compacted).

### 4.6 Synchronisation

- Wine 11.0 upstream has exactly two mechanisms: **ntsync** (`/dev/ntsync`
  kernel driver) and **server-side** waits. There is no esync/fsync in
  upstream (no `WINEESYNC`/`WINEFSYNC`/`eventfd` sync code; grep of
  `server/` and `dlls/ntdll/unix/` finds only `inproc_sync.c`/`sync.c`).
- ntsync is used iff the build saw `linux/ntsync.h`
  (`server/inproc_sync.c:37-41`, `dlls/ntdll/unix/sync.c:63`) **and**
  `open("/dev/ntsync")` succeeds at server start (`inproc_sync.c:48-52`;
  every object constructor checks `get_inproc_device_fd() >= 0`,
  `server/event.c:94`, `:116`, `server/mutex.c:160`,
  `server/semaphore.c:137`, `server/thread.c:567`; clients get the fd from
  the server, `sync.c:767-830` returns `STATUS_NOT_IMPLEMENTED` when absent).
- **Forcing server-side sync**: build against headers without
  `linux/ntsync.h` (our 5.4 pin does this: `HAVE_LINUX_NTSYNC_H` undefined, so
  the `#else` stubs at `inproc_sync.c:243-292` and `sync.c:453-501` are
  compiled), or simply provide no `/dev/ntsync`. There is no environment
  switch. Measured: no `ntsync` open, no `eventfd`, no `futex_waitv` in either
  trace. AD-8 holds with zero kernel work.

### 4.7 Minimum glibc / kernel

- Stated: "Linux version 2.6.22 or later" (`README.md:45`); no glibc minimum.
- Measured: `GLIBC_2.29` max (§1.1). Kernel features actually exercised:
  `pipe2`, `epoll`, `getrandom` (3.17), `MAP_FIXED_NOREPLACE` (4.17, optional
  — emulated by Wine if ignored), `SCM_RIGHTS`, `tgkill`, `sigaltstack`,
  `arch_prctl`, `fcntl` locks, `MAP_SHARED` file mappings, `/proc/self/exe`.
  Optional features it probes and survives without: `/sys`, `/dev/ntsync`,
  `userfaultfd`, `AF_NETLINK`, `inotify`.

---

## 5. Address-space snapshot (`/proc/<pid>/maps`)

Captured during `wine cmd /c pause` (stdin a pipe held open 12 s; snapshot
after 6 s) by `run-wine-host.sh --maps`, summarised by
`tools/cp/maps-summary.py`. 10 processes: start.exe, wineserver, services.exe,
explorer.exe, 2× winedevice.exe, plugplay.exe, svchost.exe, rpcss.exe,
cmd.exe. **Any process maps inside [0x70000000, 0x78000000): no.**

Every Windows process has the same five mappings below 8 GiB (cmd.exe shown;
the TEB-commit split differs per process), plus its `.exe` at `0x140000000`:

```
7fde0000-7ffc0000 ---p 00000000 00:00 0                     TEB block (reserved)
7ffc0000-7ffe0000 rw-p 00000000 00:00 0                     TEB + PEB + signal stacks (committed)
7ffe0000-7ffe1000 r--s 00000000 08:01 5603241   …/tmp/.wine-1000/server-801-557fa0/tmpmap-474daabd (deleted)   KUSER_SHARED_DATA
7ffe1000-7ffe2000 rw-p 00000000 00:00 0                     syscall-dispatcher pointer page
7ffe2000-7fff0000 ---p 00000000 00:00 0                     rest of the 0x7f000000 reservation
140000000-140001000 r--p 00000000 08:01 5352887 …/x86_64-windows/cmd.exe
140001000-14001d000 r-xp 00001000 08:01 5352887 …/x86_64-windows/cmd.exe
…
140025000-14003a000 rw-p 00000000 00:00 0
14003a000-140050000 r--p 00025000 08:01 5352887 …/x86_64-windows/cmd.exe
```

The rest of cmd.exe's map, by region:

| range | what |
|---|---|
| `55558a2a8000-55558a2c9000` | `[heap]` (brk of the Unix `wine` loader) |
| `6ffffe9e0000-6ffffffe9000` | PE builtins, top-down from below 112 TiB: imm32, shell32, shcore, shlwapi, win32u, gdi32, user32, msvcrt, ucrtbase, sechost, advapi32, kernelbase, kernel32, ntdll.dll |
| `7d25e4e00000-7d25e5911000` | Unix side, kernel-placed: win32u.so, libm-2.31, libgcc_s.so.1, ntdll.so, libc-2.31, libpthread-2.31, libdl-2.31, ld-2.31, `x86_64-unix/wine` |
| `7d25e5911000-7d25e5919000` | `[vvar]`, `[vvar_vclock]`, `[vdso]` |
| `7d25e5919000-7d25e591f000` | `wine-preloader` (static-PIE, kernel-placed) |
| `7ffff43e6000-7ffff4407000` | `[stack]` |
| `7ffffe000000-7fffffff0000` | the preloader's top reservation, now holding the virtual heap, `apisetschema.dll`, the `.nls` files (`locale.nls`, `sortdefault.nls` 3.2 MiB, …), a second server tmpmap (`tmpmap-eaf48f4c`, 1.3 MiB, shared with wineserver's `rw-s` mapping), and `---p` slack |
| `ffffffffff600000` | `[vsyscall]` |

Per process (from `maps-summary.md`): lowest user mapping `0x7fde0000` for
every Windows process (wineserver: `0x5f47fed91000`); highest user end
`0x7fffffff0000`; 151–249 mappings each; largest single mappings are glibc
malloc-arena reservations of 63.9 MiB `---p` (several per process), the
preloader top reservation remainder (≈19 MiB `---p`), `shell32.dll` 7.9 MiB,
`light.msstyles` 7.2 MiB (explorer). Nothing above `0x7fffffff0000` except
vsyscall.

During start-up (before the low reservation is released) the strace shows the
full preloader set mapped in every process (§3.3 / §0 table): `0x10000` 1 MiB,
`0x110000` 1.66 GiB, `0x7f000000` 16 MiB, `0x7ffffe000000` 32 MiB, all
`PROT_NONE` `MAP_NORESERVE` — HobbyOS must accept 1.7 GiB of `PROT_NONE`
reservation per process without committing frames.

---

## 6. What this means for HobbyOS (ranked by first use)

Order is the order in which a Wine process would hit the gap on HobbyOS
today, taken from the cold `wineboot -i` trace; "fatal" means Wine exits or
the first `%gs` access faults; "degraded" means Wine continues.

| # | first use | item | today | effect | where it belongs |
|---|---|---|---|---|---|
| 1 | +0.02 s | exec of a static-PIE (`wine-preloader`) then user-space `mmap(MAP_FIXED)` of `wine` + `ld.so` | static-PIE parse exists (`elf_image.c:397`) | must place it outside Wine's reserve list | P1 |
| 2 | +0.02 s | `MAP_FIXED` reservations at `0x10000`…`0x68000000`, `0x7f000000`, `0x7ffffe000000` | refused (VA window 64–96 GiB) | degraded (warnings) | P1/F1 |
| 3 | +0.04 s | `MAP_FIXED_NOREPLACE` (`EEXIST`/`ENOMEM` semantics, or hint) | `-EINVAL` (`linux_compat_mem.c:195`) | address space believed to end at 4 GiB; later allocations fail | **P1 — one-line-class fix, do first** |
| 4 | +0.05 s | `KUSER_SHARED_DATA` at `0x7ffe0000` (private, then `MAP_SHARED\|MAP_FIXED` of a server tmpfile) | below `USER_VA_BASE` | **fatal** `exit(1)` | P1 (low VA) + P2 (shared file mapping) |
| 5 | +0.05 s | `symlink` | ENOSYS | prefix `dosdevices` cannot be created (wineboot), `com*` links | P2 (+ FS support, P3) |
| 6 | +0.10 s | wineserver bring-up in `/tmp/.wine-<uid>/server-<dev>-<ino>/`: `mkdir`, `F_SETLK` lock, `socket/bind/listen`, `chmod` (ignored) | `chmod` ENOSYS | degraded (socket mode) | P2 |
| 7 | +0.11 s | **`fchdir`** (588) | ENOSYS | **every `NtQueryDirectoryFile` fails** (`file.c:2914-2934`): no `FindFirstFile`, no directory scan — wineboot's setup and any game asset scan | **P2 — first syscall to add** |
| 7b | +0.11 s | `fstatfs` (5881), `ftruncate` (233) | ENOSYS | both results tolerated in the measured paths (`fd.c:1020`, `mapping.c:320`); volume-info queries fail | P2 (cheap) |
| 8 | +0.11 s | anonymous sections = unlinked tmpfs files, `pwrite` past EOF, `MAP_SHARED` in server + clients, fd over `SCM_RIGHTS` (4721 passes) | not proven | KUSER/heap sections | P2/P3 (tmpfs `/tmp` with shared file pages) |
| 9 | +0.12 s | `sigaltstack` at TEB-relative low address (51,200 B), handlers `SA_ONSTACK` | present | needs low VA | P1 |
| 10 | +0.30 s | **`arch_prctl(ARCH_SET_GS)`** per thread (+ `ARCH_GET_GS` in the fault path); no `AT_HWCAP2` FSGSBASE unless CR4 set | `-EINVAL` | **fatal** at first `%gs:` | P1 (C1-cpu) |
| 11 | +0.30 s | user-mode `xsavec`/`xsave`/`xrstor` keyed on OSXSAVE+XCR0; signal-frame magic at `fpregs+464`, XSAVE at `+512`, standard format | FXSAVE only (`fpu.c`) | works today via FXSAVE path; must stay consistent once OSXSAVE is set | P1 (C1-cpu), §4.5 |
| 12 | +1.0 s | `sendmmsg`, `AF_NETLINK`, `SIOCGIF*` | ENOSYS / EAFNOSUPPORT | DNS and interface enumeration degrade | later (tolerable) |
| 13 | +1.15 s | `getxattr`/`fgetxattr`/`fsetxattr`/`fremovexattr` (7007 calls, all `ENODATA` here) | ENOSYS | Wine maps ENOSYS like ENOTSUP for DOS attributes — **verify** in P2; cheapest: return `-ENOTSUP`/`-ENODATA` | P2 |
| 14 | +69 s | `utimensat` (90) | ENOSYS | file times after copy | P2/P3 |
| — | steady state | hot paths: `rt_sigprocmask` (332k), `read`/`write`/`writev` on server pipes (586k), `epoll_wait` (118k), `lstat`/`newfstatat` (56k) | present | performance of P2 = cost of these four | P2 perf |
| — | — | wineserver must be scheduled every ≤16 ms (KUSER time) | 100 Hz tick | ok | — |

---

## 7. Regeneration

```bash
# on linuxci, in the lane checkout
bash tools/cp/fetch-runtime.sh                       # pins (+ GPG)
bash tools/cp/check-no-game-files.sh                 # AD-12 guard
bash tools/cp/build-wine.sh all                      # ~5 min at -j12; audit prints max GLIBC_
bash tools/cp/run-wine-host.sh -- wineboot -i        # fresh prefix
bash tools/cp/run-wine-host.sh --warm -- wine cmd /c echo hello
bash tools/cp/p0-wine-evidence.sh                    # traces, maps, summaries -> $CP_WINE_WORK/evidence
python3 -I tools/cp/strace-summary.py --tree . EVID/strace-wineboot.raw EVID/strace-echo.raw
python3 -I tools/cp/maps-summary.py EVID/maps --hole 0x70000000-0x78000000
# PE image bases
for f in $CP_WINE_WORK/stage/CP/WINE/lib/wine/x86_64-windows/*; do
  llvm-objdump -p "$f" | awk -v f="${f##*/}" '/^ImageBase/{print $2, f; exit}'; done | sort | uniq -c -w16
```
