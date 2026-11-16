# Cyberpunk 2077 on HobbyOS — the Proton track

Status: **v1 — P0 closed (GO for Venus), wave 1 merged, wave 2 executing.**
Planning + execution document; v0 written 2026-11-14, v1 2026-11-15. **§0.2
records what P0 and wave 1 measured and is binding where it disagrees with
older text below.** Every statement about HobbyOS carries a `path:line`; every
statement about the game, Wine, vkd3d-proton or Venus is either measured (with
its evidence doc) or marked **verify** — and **a verify is a task, not a
fact**.

Audience: agent lanes + maintainers. Read §0, §5 and §7 before touching
anything; §7.3 is binding once P0 closes.

Companion docs: `docs/rimworld-plan.md` (the precedent this plan extends — its
AD-1 explicitly deferred Wine to "its own plan"; this is that plan),
`docs/linux-abi.md` (the compat contract), `docs/gpu-abi.md` (the `/dev/gpu`
ABI this plan amends), `docs/rimworld-virgl-plan.md` (the host GPU stack and the
virtio-gpu 3D transport), `docs/rimworld-m7-perf.md` and `docs/l88-fps-report.md`
(why the present path matters), `docs/parallels-arm-port.md` (why the Mac is out).

---

## 0. TL;DR and how to use this document

**The ask**: run Cyberpunk 2077 — a proprietary Windows-only Direct3D 12 game —
on HobbyOS.

**The correction that defines the plan**: RimWorld was chosen *because* it had a
native Linux build, which let `docs/rimworld-plan.md` §1.2 reject the
Proton/Wine track as "strictly a superset of the Linux track". Cyberpunk has no
Linux build, so that superset is now the whole job:

```
            RimWorld (done to menu)                Cyberpunk (this plan)
            ───────────────────────                ─────────────────────
game        Linux ELF + Unity + Mono               Windows PE x64, REDengine 4
API         OpenGL 3.3 core                        Direct3D 12
translation none                                   Wine (Win32/NT) + vkd3d-proton (D3D12→Vulkan)
GPU path    Mesa virgl → host GL                   Mesa Venus → host Vulkan (RADV)
CPU ISA     SSE                                    AVX/AVX2 (verify)
memory      12 GiB guest                           ≥16 GiB guest + host-visible GPU heap
assets      436 MB                                 ~70 GB (verify), multi-GB archives
```

Everything RimWorld built (glibc 2.31 `ld.so`, threads/TLS/futex, fault signals,
NFS assets, an X server, the virtio-gpu 3D transport, `/dev/gpu`) is reused.
Nothing in this plan replaces it; it stacks on top.

**What HobbyOS must grow** (headline; detail in §4):

| # | Capability | Today | Why Cyberpunk needs it |
|---|---|---|---|
| 1 | AVX register state (XSAVE) | FXSAVE64 only (`src/kernel/arch/x64/fpu.c:9-33`) | the game's code uses AVX (verify); the first `vmovaps ymm` is a `#UD` |
| 2 | A compat address space that can map **low** virtual memory | user VA starts at 64 GiB because x64 identity-maps 0–8 GiB (`src/include/vm.h:11-27`) | Windows' `KUSER_SHARED_DATA` is fixed at `0x7ffe0000`; Wine's preloader reserves low ranges (verify) |
| 3 | User GS base | `L_ARCH_SET_GS` is defined (`src/include/linux_syscall.h:254`) but no kernel code handles it | Windows x64 addresses the TEB through `%gs`; Wine sets it with `arch_prctl(ARCH_SET_GS)` on Linux (verify at pin) |
| 4 | Wine (NT/Win32 on Linux) | nothing | the game is a PE executable |
| 5 | Vulkan 1.3 with the vkd3d-proton feature set | GL only; `gpu_abi.h:32`: "Not in M1: blob resources / dmabuf, mmap of a resource, async fences" | D3D12 → Vulkan |
| 6 | A writable, symlink-capable, large-file filesystem | FAT16 `disk.img`; NFS is read-only | Wine prefix (`dosdevices/` are symlinks), registry, saves, shader caches |
| 7 | Large file mappings | `VM_OBJ_MAX_BYTES` = 64 MiB per object (`src/include/vm.h:160`) *(stale: 1 GiB since cp/sys ca51e8cf, C3-pfx)* | Wine maps the PE image; the game's archives are multi-GB (verify how they are read) |
| 8 | A broad X client surface | `src/user/xcompat/libx11.c` implements the RimWorld-measured subset (`required-libx11.txt`) | `winex11.drv` needs real libX11 + XInput2, RandR, Render, Fixes, Cursor, XShm |
| 9 | Zero-copy presentation | present goes through X (`docs/l88-fps-report.md`: 1.61 → 7.40 fps after fixing the present path alone) | at 1080p a readback present is ~8 MB per frame |
| 10 | Audio | silent by decision (`docs/audio.md`, GB-029) | Wwise must at least initialise against "no device" (verify) |

**Verdict, in one paragraph.** A Cyberpunk **main menu** rendered inside HobbyOS
through Venus under KVM on the Strix Halo workstation is a credible stretch goal.
It is roughly 5–10× the RimWorld effort; the bulk is Wine bring-up (P2) and the
Vulkan path (P5–P7), and the riskiest single item is the address-space change
(P1), because every other milestone sits on it. **Playable** (P9) depends on a
number nobody has yet: how fast the game runs in a *plain Linux guest* on the
same Venus stack. P0 measures that number first, and if it is bad the plan
switches GPU strategy before a line of HobbyOS code is written.

**How to use this document**: §6 is the ladder (P0…P9, each with a runnable
gate and an evidence rule). §7 assigns lanes and freezes interfaces. A lane may
not tick a gate it did not run. Anything marked **verify** is a task.

### 0.1 The ladder at a glance

```
P0 evidence ─► P1 CPU+AS ─► P2 Wine ─► P3 storage ─► P4 windows/input ─┐
 (go/no-go)    (AVX, low VA,  (cmd.exe,   (ext2 rw,     (winecfg in      │
               GS base)       SEH, TLS)   >4 GiB)       IceWM, raw mouse)│
                                                                         ▼
               P5 lavapipe ─► P6 Venus ─► P7 scanout ─► P8 menu ─► P9 playable
               (vkcube.exe,   (host GPU)  (zero-copy    (screenshot) (save loads,
                D3D12 sample)              present)                   fps vs baseline)
```

P1 → P2 → P8 is the critical path. P3, P4 and P5 can run alongside P2 once P1
lands; P6 can start in parallel with P1 because it touches different files.

Each rung is independently useful: P1 gives HobbyOS a modern x86 FPU and a
Linux-shaped address space; P2 makes it run Windows console programs; P5/P6
give it Vulkan, which every future game and the browser can use; P7 fixes the
present path RimWorld is currently bottlenecked on.

### 0.2 v1 status — what P0 and wave 1 established (binding)

Evidence docs (all on branch `cyberpunk`): `docs/cp-p0-host.md` (host-native
column), `docs/cp-p0-guest.md` (Linux-guest-on-Venus column),
`docs/cyberpunk-evidence.md` + `docs/cp-pins.md` (Wine), `docs/cp-address-space.md`
(identity-map inventory + direct-map design), `docs/cp-ext2.md` (ext2).

**Machines.** GPU host is `strix` (192.168.0.123): Ryzen AI MAX+ 395, 93 GB,
**RX 9070 XT (RDNA4)** + 8060S iGPU, QEMU 10.2.1, virglrenderer **1.2.0**
(1.10.0 was the soname) built with Venus. KVM build/test box for HobbyOS gates
is `linuxci` (EPYC 7513, AVX2, no AVX-512, no render node). The game is the
user's **Steam** build 2.5 (app 1091500, 62 GB on an NFS share).

**P0 go/no-go: GO for Venus.** Low preset, RT/FG off, FSR 2.1 Auto (the
preset's own default), 60 s at the main menu:

| | host native (Proton, 9070 XT) | Linux guest on Venus | HobbyOS |
|---|---|---|---|
| menu fps median @720p | 388–399 | **142.8 / 144.4** | — |
| menu fps median @1080p | 393.5 | 144.1 | — |
| menu fps p90 @720p | 284–322 | 59.0 / 55.4 | — |
| built-in benchmark median / 1%-low @720p | 141.6–142.9 / 74.1–76.1 | not run (user: skip) | — |
| launch → menu (warm) | 36.7–47.6 s | 41.3–41.9 s | — |
| game RSS at menu / peak | 2.2 / 3.9–4.0 GiB | 2.1 GiB | — |
| game threads at menu | 90 | 68 | — |

The guest median is identical at 720p and 1080p → the guest is bound by the
present path (QEMU display), not by Venus; that is P7's target. GPU-heavy D3D12
work runs at 98–99 % of native through Venus; Venus adds ~64 µs per submit and
~225 µs per fence round trip (276 vs 48–53 µs). The 15 fps trigger (§1.4) did
**not** fire, and no vkd3d-proton-*required* feature is missing under Venus
(59/231 extensions are not forwarded; visible loss: FL 12_1 instead of 12_2,
no mesh shaders, no DGC/descriptor-buffer/memory-budget).

**Facts that change the plan** (each folded into the section named):

1. **No GOG build needed** (§1.5): the Steam build reaches the menu with no
   reachable Steam client (`[S_API FAIL]`, game continues). A fresh profile
   shows three first-run screens — user agreement, optional CDPR account offer,
   telemetry consent; P8's launcher must start from a profile in which the
   user has already made those choices, never automate them.
2. **AVX is not required to reach the menu** (AD-4): with every AVX-family
   CPUID bit masked and no AVX xstate, the guest reaches and renders the menu
   at the same fps. The exe has 1,641 ymm / 339 FMA instructions and
   cpuid/xgetbv dispatch, so gameplay without AVX stays unverified. XSAVE
   (already implemented in lane C1-cpu) is a correctness/performance item, not
   a gate for P8.
3. **No audio device needed** (AD-9): menu reached with no device at all
   (guest) and with Wine's audio drivers disabled (host).
4. **The kernel image does not have to move** (AD-3): it is linked/loaded 1:1
   at `0x70000000` (`linker_x64.ld:7`, measured end `0x75c72b90`). Nothing Wine
   needs intersects `[0x70000000, 0x78000000)`: preloader reserves
   `0x10000–0x110000`, `0x110000–0x68000000`, `0x7f000000–0x7fff0000`,
   `0x7ffffe000000–0x7fffffff0000` (Wine `loader/preloader.c:115-118`); TEB block
   `0x7fde0000`, KUSER `0x7ffe0000`. The game exe is DYNAMIC_BASE (0x8160) but
   Wine loads it at its preferred 0x140000000. Design: keep the image as a
   supervisor-only hole (K0), direct map at `0xffff800000000000`, ioremap at
   `0xffff808000000000`; 23 non-mechanical items < 50, so the AD-3 fallback
   does not fire — and it would not suffice anyway: the game uses **all of
   0–8 GiB** (4,091 of the first 4,096 MiB mapped/reserved; exe at 5 GiB; a
   15.4 GiB reservation from 6.6 GiB).
5. **Address-space scale** (F1): 141.5 GiB of VA at the menu (137 GiB
   reserved, not committed), largest single reservation 63.3 GiB, highest
   address `0x7fffffff0000`. Compat ASes must support ~64 GiB single
   `PROT_NONE` reservations and ≥ 141 GiB total without committing frames.
6. **RAM** (§4 Memory, R4): the x64 frame pool is fixed extents
   [0.5, 1.75) + [4, 8) GiB = **5.25 GiB** whatever `-m` says
   (`src/include/frame.h:36-39`); no firmware memory map is parsed. A ≥16 GiB
   guest needs P1's direct map **plus** a multiboot memory-map parser.
7. **No `swapgs` existed** (§3.2): both GS MSRs held `&cpu_locals[cpu]`. Lane
   C1-cpu introduced a full swapgs discipline (`trap_gs_fixup`, three
   iretq-to-ring-3 sites) before implementing `ARCH_SET_GS`.
8. **`MAP_FIXED_NOREPLACE` returns `-EINVAL`** (`linux_compat_mem.c:195`); Wine
   issues 1,335 of them during `wineboot` and concludes the AS ends at 4 GiB.
   Required semantics: NOREPLACE over an existing mapping or the kernel hole →
   **EEXIST**; `MAP_FIXED` into the hole → **ENOMEM**; never EINVAL (Wine
   `virtual.c:1535-1556`).
9. **KUSER_SHARED_DATA is a server-written `MAP_SHARED` memfd page** passed over
   SCM_RIGHTS and refreshed every ≤16 ms (Wine `server/mapping.c`, `fd.c`); Wine
   `exit(1)`s if it cannot map it at `0x7ffe0000`. P2 therefore needs low VA
   **and** coherent shared memfd/tmpfs pages.
10. **Measured Wine syscall gaps** replace v0's guesses (§3.2/§4). Console Wine
    (`wineboot -i`, `cmd /c echo`): 92 distinct syscalls, 11 unhandled —
    `symlink chmod fchdir fstatfs ftruncate sendmmsg getxattr fgetxattr
    fsetxattr fremovexattr utimensat` — plus the behaviour gaps
    `MAP_FIXED_NOREPLACE` and `ARCH_SET_GS`. **`fchdir` is critical**: every
    `NtQueryDirectoryFile` uses it. The game under Proton adds
    `PR_SET_SYSCALL_USER_DISPATCH` (every thread), `userfaultfd` (every Wine
    process; our 5.4-header build compiles it out), `eventfd2`, `inotify_*`,
    `process_vm_readv` (wineserver) and a heavy `getrusage(RUSAGE_THREAD)`.
    `signalfd`/`io_uring` are never used.
11. **Sync** (AD-8): upstream Wine 11 has only ntsync or server-side sync; our
    5.4 kernel headers compile ntsync out, so server-side sync is guaranteed
    with no kernel work. Its measured cost on the host: benchmark −23 %, menu
    −38 % (one run) — the fast-sync trigger in §1.4 should be expected to fire
    after P8.
12. **Mappings vs the 64 MiB cap** (gap #7): archives are never mmapped (read,
    single `read()`s up to 171.6 MB, 1.35 GB to the menu); the largest
    game-file mapping is 3.0 MiB. What hits the cap is Wine's largest memfd
    (exactly 64.0 MiB) and GPU buffer mappings (up to 192 MiB). The exe is
    66.7 MB but mapped section-wise.
13. **Process scale**: 15–16 processes and 173–190 threads at the menu on the
    host vs `MAX_PROCESSES 192` — raise it before P8.
14. **Venus host recipe** (P6): needs an LD_PRELOAD shim adding `DRM_RDWR` to
    the render server's dma-buf exports (`tools/cp/p0guest/dmabuf-rdwr-shim.c`;
    without it every guest `vkMapMemory` fails), GPU ordering via the host's
    device-select env (the `!` exclusive form breaks Venus on the host), and no
    `-daemonize`. vkd3d-proton writes its pipeline cache next to the exe /
    the CWD unless `VKD3D_SHADER_CACHE_PATH` is set; Steam's `steamclient.so`
    overrides `MESA_SHADER_CACHE_DIR` — HobbyOS's launcher sets both and the DLL
    overrides itself.
15. **Wine build** (AD-1/AD-2): Wine 11.0 (GPG-verified) builds 64-bit with
    plain clang (PE side: clang + lld-link, no MinGW) against the glibc 2.31
    sysroot plus focal's gcc-10 libgcc (gcc-15 libgcc pulls a GLIBC_2.35
    symbol and silently produces a fixed-address preloader). Max requirement
    **GLIBC_2.29** — the 2.31 pin holds. Runs `wineboot -i` and
    `cmd /c echo hello` on the host inside a pinned-only chroot. Staged tree
    202 MiB at `/CP/WINE`.
16. **ext2** (AD-7, P3): read/write ext2 at `/EXT` on **NVMe namespace 2** of the
    boot controller (`-device nvme-ns,nsid=2`, Makefile `CPDATA`), proven by a
    two-boot gate with host `e2fsck -fn` clean; mmap/exec of ext2 files, locks,
    chmod/utimens and `VFS_PATH_MAX` > 256 are still open.
17. **Pre-existing defects found** (not caused by this work; each verified on
    base `f7a51cfa`): `linux_probe_join` fails (`E clone failed errno=22` —
    compat clone refuses non-thread clones; kernel trace lines splice into probe
    output); `make test_intel` wedges after `MD5SUM_T.BIN` with no FAIL lines;
    `run_unit_tests_intel.sh` has 3 failures (`virtio_gpu_test.c:213`, `:243`,
    an interrupt-masking budget test); `make host_tests` fails two board tests
    for a missing `gaming.md`. These are recorded, not blocking; the T3 wave
    cannot be an acceptance signal until the wedge is fixed.
18. **Wave 3 findings (2026-11-16, toward and through Gate P2).**
    - **NFS READ size.** At rs=1344, `wineboot -i` over NFS took about 15
      minutes, roughly 1.7 ms per 1.3 KiB RPC. cp/nfs moved READs to 32 KiB
      (IPv4 reassembly + FSINFO); sequential reads went from 82 to 190 MB/s.
      That exposed an older readahead race in which concurrent readers were
      handed another fill's bytes, which crashed Wine; fixed by cp/nfsfix.
      Guards: `rsize_scan.py` (in `host_tests`) and `rsize_guard.sh` (boot);
      see `docs/cp-nfs.md` §6–7. The single global readahead window still
      thrashes: P2 read 1.66 GB to bring up about 200 MB of Wine, so a
      per-file window is the next NFS fix.
    - **Kernel defects that P2 found (fixed in wave 3).**
      - `trap_exit_frame_check` clobbered the user's rsi/rdx/r8–r11 and
        misfired for compat RIPs below 64 GiB.
      - compat `openat`/`newfstatat` clobbered rdi/rsi.
      - `compat_open` translated the path before trying the real one.
      - `st_rdev` was 0 for the console and `/dev/null`, so Wine turned its
        debug channels off.
      - Every CPU exception reached a compat process as SIGSEGV, and the #BP
        and #OF gates were DPL 0.
      - `NFS_DIR_CACHE_MAX` 256 silently truncated `x86_64-windows` (757
        entries).
      - A killed task could be re-parked (C1-thr2); pid allocation is now
        cyclic (C1-thr3).
    - **Process count.** At most 18 live at once in P2, so 192 slots are not
      tight for Wine. One P2 run uses about 130 pids. §0.2 item 13 (173–190
      game threads) still needs ≥ 256 before P8.
    - **Still open.**
      - *(fixed, C1-k4: epoll 32-fd/8-instance cap, 512-byte pipes, ignored
        O_NONBLOCK, killed tasks re-parking)* An intermittent wineboot-tree
        hang: every client BLOCKED with no futex and no timer.
      - *(fixed, C1-k4)* munmap/mprotect flush only the local TLB.
      - *(fixed, C3-nfs2)* fstat of an NFS fd reports mtime 0.
      - *(fixed, C1-k4; also CR0.NE=1, so an x87 fault no longer freezes the
        core on FERR#)* si_code details.
      - *(fixed, C3-pfx 205a73b6; open: `ftruncate`/`O_TRUNC` of a mapped FAT file)* FAT private mappings miss later `write()`s.
      - *(fixed, C3-nfs2)* NFS symlinks in intermediate path components aren't followed.
      - *(fixed, C1-k4)* exec from a secondary thread leaves tid ≠ pid.
      - *(fixed, C1-k4)* SIGBUS past EOF is reported as SIGSEGV.
      - *(stale: `VM_OBJ_MAX_BYTES` has been 1 GiB since cp/sys ca51e8cf —
        C3-pfx)* The 64 MiB per-file ext2 mapping cap.
    - **Wave 4 findings (2026-11-16, in progress).**
      - *Fixed:* `fat16_rename` stored the last path component through NULL
        (a kernel write to VA 0), so every FAT `rename(2)` from user space hung
        a CPU; IceWM's fontconfig hits it at session start. c5640214, guard row
        `linux_probe_tmpfs` T4f (61745d15).
      - *Fixed, C3-nfs2 (cp-nfs2 merged at 1cb981be):* after three unanswered
        READs the NFS client returned zero-filled data instead of an error, so
        Wine read an all-zero NLS section and crashed (C5-vk2 run 1) and
        winex11 failed to load. NFS is now hard-mounted (2555d642): a lost
        reply is re-sent with backoff for up to 180 s, and a failure is
        `-EIO`/SIGBUS, never zeros; guard G9 injects reply loss. The replies
        were lost at QEMU slirp's host UDP socket (212992-byte default receive
        buffer holds ~3 replies); at most 2 READs are now outstanding per socket
        (14e8e111), and run 3 had 1 stall where run 2 had 412. No host setting
        is needed. Also fixed: readahead windows per (file, 64 KiB block)
        (P2's wineboot phase read 43 MB where run23 read 269 MB), 64-bit
        offsets, stat times, symlinks in any path component.
      - *Open:* wineboot still ends at Wine's 300 s boot-event wait
        (status 53, ~303 s) with NFS no longer the cause; one NFS LOOKUP went
        unanswered for 95 s in P2 run 3 (suspected `exportfs -ra` cache flush,
        unproven).
      - *Wine trap:* win32u waits only 10 s for explorer
        (`dlls/win32u/winstation.c:877`); in the guest the first GUI client of
        a session stays on the null driver unless a keepalive client warms
        explorer first (`tools/cp/p5/winep5.c`).
      - *Merged, C1-k4 (2c5529f3):* the fixes above plus `MAX_PROCESSES` 512
        on x86_64 (ARM stays 192); new probes pipestorm, tlb, siginfo,
        manythr, execthr fail on base and pass. Gate P2 passes on the merged
        tree (G1 hello, G2 41/41, G3 clean).
      - *(fixed by cp-nfs2b, see below)* wineboot's 300 s boot-event wait
        expired (status 53, ~320 s) and the prefix install ran on to ~2200 s
        of uptime — the per-LOOKUP sockets, not tmpfs.
      - *Open, P8 ceiling:* `MAX_GLOBAL_FILES` 512 (so 256 pipes),
        `MAX_OPEN_FDS` 256, poll nfds 256; Wine uses 3 pipes per thread, so
        ~190 game threads need ~570 pipes.
      - *Merged, C3-pfx (bcae5139):* Gate P3 passed; `sync`/`syncfs`; FAT mapping
        coherence; big-object frees moved out of `vm_lock`.
      - *Open, ext2 speed:* `wineserver -k` gives the server 10.5 s, which can
        cut a registry hive save short on `/EXT` (~125 KB/s for 4 KiB writes:
        write-through, ~5 single-block NVMe commands per write). Next storage
        step: batched write-through and multi-block commands.
      - *Fixed, cp-nfs2b (03a1fccf):* the 180 s NFS give-ups and 46–102 s
        retries were host ephemeral-port exhaustion, not nfsd load: QEMU slirp
        keeps one host UDP socket per guest port for 240 s, and the client
        opened a fresh socket for every LOOKUP/GETATTR/READLINK/READDIRPLUS
        (one guest held 16384 host sockets; strix has 28232 ports). Every RPC
        on a mount now uses a pool of 6 long-lived sockets (11 host sockets at
        most per guest); guard G10 in rsize_guard. **wineboot -i now ends with
        status 0 in ~25 s** (run23: status 53 at 307 s), and Gate P2 reads
        208 MB in total (run23: 1.66 GB).
      - *(fixed, cp-k4b; see below)* Wild faults in Wine X sessions on 030a7f10.
      - *Fixed, cp-k4b (merged):* a dying thread group's members kept running
        after `exit_group` (the drain never kicked their CPUs), and a refused
        address-space release cleared its pointer anyway, so the survivor
        faulted with `in=V1`; reaped groups also held their address spaces
        until slot reuse (`linux_probe_exitgroup` X6: 5344 MiB → 32 MiB lost).
        Wine's nsiproxy NULL read came from `if_nameindex()` with no
        AF_NETLINK: a minimal NETLINK_ROUTE socket (link/addr/route dumps) now
        exists (`linux_probe_netlink`). The services.exe/explorer.exe wild
        faults of C4-x2 run 8 did not recur in three runs after cp-nfs2b.
      - *Gate host:* four QEMU slots and per-call `/etc/exports` edits (§7.4).

**Lane status (2026-11-15).** Merged to `cyberpunk`: C1-as (design), C2-wine
(pins/build/evidence), C3-ext2 (ext2 rw), C7-p0host, C7-p0guest. **C1-cpu**
(XSAVE/AVX, F2 frames, swapgs + `ARCH_SET_GS`, SIGCHLD default-ignore, SFMASK)
is implemented and its new probes pass, but it is **held**: an A/B on
`-cpu host` shows `linux_probe_tramp_sig` T2 (SA_RESTART-restarted FUTEX_WAIT
returns EAGAIN) passes on base and fails on the lane twice — a lane regression
being fixed. Wave 2 (§7.2b) proceeds in parallel.

---

## 1. Track selection

### 1.1 Requirements (from the ask, made mechanical)

1. Runs the user's own purchased copy; **no game files in the repository**,
   ever (same rule and guard as RimWorld).
2. No game source exists: every change is on the OS side or in open-source
   userspace (Wine, Mesa, vkd3d-proton).
3. Demonstrable: a real frame of the game on HobbyOS's screen, captured by
   screendump, not a build log.
4. Runs where HobbyOS's x86_64 build runs with hardware acceleration: KVM on the
   Linux workstation. (The Mac is ARM; Parallels refuses x86_64 guests outright —
   `docs/parallels-arm-port.md`.)

### 1.2 Survey — the software stack

| Track | What it actually requires | Verdict |
|---|---|---|
| **Wine + vkd3d-proton inside HobbyOS's Linux-ABI compat class** | Everything RimWorld needed, plus: low-VA compat AS, XSAVE, GS base, Wine's syscall surface, Vulkan, a writable FS | **SELECTED.** The only track that runs the actual binary. Wine's Linux host dependency is exactly the compat class HobbyOS already has. |
| Valve's Proton build (rather than upstream Wine) | Proton is built inside the Steam Runtime SDK container and carries extra patches (fsync, per-game fixes) | **Runner-up** (§1.4). Upstream Wine first because it builds with a normal toolchain against our sysroot. |
| A Linux guest VM *inside* HobbyOS (HobbyOS as hypervisor) | A VMM in HobbyOS, nested virtualisation | Rejected: it would be Linux running the game, not HobbyOS. |
| A native HobbyOS Win32 layer written from scratch | Re-implementing what Wine is | Rejected: unbounded, and Wine exists. |
| Run it on the Mac | x86_64 guest | Rejected: Parallels refuses x86_64; QEMU TCG is far too slow for this. |
| Game streaming into HobbyOS | A video client | Rejected: not the ask. |

### 1.3 Survey — the GPU path

The software stack ends in a Vulkan driver. Which one is the real decision.

| Option | What it is | Verdict |
|---|---|---|
| **Lavapipe** (Mesa, CPU Vulkan) | LLVM-JIT software Vulkan, conformant to recent Vulkan versions (verify on the Mesa pin) | **Bring-up driver** (P5). Proves Wine + winevulkan + vkd3d-proton with no GPU transport, runs in CI. Far too slow for the game itself — the softpipe lesson (`docs/rimworld-m7-perf.md`) applies even more strongly. |
| **Venus** (Mesa `virtio` Vulkan driver → virglrenderer → host RADV) | Vulkan commands serialised over virtio-gpu to the host's real driver | **SELECTED for the game** (P6). The host already runs QEMU 10.2.1 + virglrenderer 1.10.0 + an amdgpu render node (`docs/rimworld-virgl-plan.md` §2.1); the guest-side transport (virtio-gpu 3D, `/dev/gpu`) exists. Missing: blob resources, host-visible memory mapping, context-init, async fences. |
| **amdgpu DRM native context** (guest runs RADV; virglrenderer forwards amdgpu UAPI) | Near-native submission; no Vulkan serialisation | **Performance answer** (P9, revisit trigger §1.4). Newer, and the guest side assumes the Linux virtgpu DRM UAPI (verify the Mesa/libdrm/QEMU support on our pins). |
| Bare-metal amdgpu + RADV port to HobbyOS | A LinuxKPI-style shim under the amdgpu kernel driver (the FreeBSD `drm-kmod` approach) | Rejected for v1: a multi-month project on its own. Recorded as the only route to "Cyberpunk on bare metal". |
| NVIDIA (bare metal, or the RTX 4090 over RDMA) | NVIDIA's Vulkan userspace is closed and expects the Linux `nvidia.ko` ioctl ABI; the RDMA path measured ~0.2 ms per BAR read and has not yet produced a working `nvidia-smi` | Rejected. Register-poll-heavy GPU drivers cannot tolerate that latency. |

### 1.4 Runners-up and revisit triggers

- **Native context instead of Venus.** *(P0: did not fire — 143–144 fps
  median, no required feature missing; §0.2.)* Trigger: P0's Linux-guest-on-Venus
  baseline is below 15 fps at 1280×720 Low, **or** P6 shows Venus missing a
  feature vkd3d-proton requires. Then native context becomes the P6 target and
  the `/dev/gpu` amendment (F3) is redrawn around the virtgpu DRM UAPI.
- **Proton instead of upstream Wine.** Trigger: the game fails under the pinned
  upstream Wine on the *host* in P0 but works under Proton. Then the pin is
  Proton's Wine tree, built outside its container against our sysroot.
- **Fast sync (esync/fsync/ntsync).** Trigger: P8/P9 profiles show wineserver
  round-trips as a top-3 cost. Until then, Wine's in-server sync (no kernel work).
- **AVX-512.** Trigger: the game or a library is measured to need it. Until
  then it is masked off at QEMU so the XSAVE area stays small (AD-4).

### 1.5 Licenses

- Cyberpunk 2077: proprietary (CD PROJEKT RED EULA). **Never committed.** Only
  paths, sizes and hashes appear in the tree. Either build works: the user's
  copy is the **Steam** build, which P0 measured reaching the menu with no
  reachable Steam client (§0.2 item 1). Never circumvent DRM, never replace or
  patch game/Steam files. The game is installed **on the host** (served over
  NFS), never inside the guest.
- Wine: LGPL-2.1-or-later. vkd3d-proton: LGPL-2.1. DXVK: zlib. Mesa and
  virglrenderer: MIT. glibc: LGPL-2.1-or-later (already pinned for RimWorld).
- All of the above ship as fetch scripts with sha256s, not as binaries in git,
  matching the RimWorld runtime rule (`docs/rimworld-plan.md` §2).
- The tree is GPL-2.0; dynamically loading LGPL/MIT/zlib userspace is
  compatible. No GPL-3-only code enters the compat track.

### 1.6 Non-goals

Ray tracing and path tracing, DLSS and frame generation (NVIDIA-only or
vendor-gated), HDR, REDmod/mods, the Steam and GOG Galaxy clients, any DRM or
anti-cheat, 32-bit Windows code (the game is 64-bit; installers run on the
host), ARM, the Mac, bare metal, windowed mode in v1 (fullscreen only, AD-6),
gamepad in v1, multiplayer (there is none), and shipping any game asset.

---

## 2. Pinned sources & provenance

Pinned in P0 by lane C2-wine (`docs/cp-pins.md`, `tools/cp/fetch-runtime.sh`,
all sha256-verified, idempotent). Rows still marked "plan" have no pin yet.

| Component | Choice | Pin | Notes |
|---|---|---|---|
| Game | user's Steam build, game 2.5 (app 1091500) | `Cyberpunk2077.exe` 66,704,848 B; ImageBase 0x140000000, DllCharacteristics 0x8160 (DYNAMIC_BASE) | never in git; read-only on NFS |
| Wine | **11.0** (stable), 64-bit Unix side only | sha256 `c07a6857…6e0701`, GPG-verified (key `DA23579A…AF17519D`) | built with clang against glibc 2.31 + focal libgcc-10; max GLIBC_2.29 |
| libgcc (sysroot) | focal `libgcc-10-dev` 10.5.0 | sha256 `ebe5e967…b897` | gcc-15's libgcc pulls GLIBC_2.35 |
| vkd3d-proton | **3.1** release (prebuilt PE DLLs) | sha256 `b85ea821…120b` | **verify**: Proton Experimental bundles 3.2.0 / `2576c0b4a`; re-pin to the then-latest release before P5 |
| DXVK | **3.1.1** | sha256 `40565b4a…f54e608` | `dxgi.dll` (and `d3d11.dll` for the P5 DXVK probe) |
| Mesa | 26.2.6 — already pinned (`docs/rimworld-plan.md` §2) | existing sha256 | new build: `-Dvulkan-drivers=swrast,virtio` (lavapipe needs LLVM; venus does not) |
| LLVM (for lavapipe) | 21.1.8 — already in `third_party/llvm-21.1.8` | existing | runtime JIT ⇒ `PROT_EXEC` anonymous mappings (verify present) |
| libX11/libxcb + Xi, Xrandr, Xrender, Xfixes, Xcursor, Xext, Xinerama | upstream releases | tarballs + sha256 | built against glibc 2.31 (RimWorld AD-11: never against HobbyOS headers) |
| Vulkan loader + headers | `vulkan-sdk-1.4.363.0` | loader `941eff55…a6f7`, headers `4a078be1…7e7f7e` | `libvulkan.so.1` for winevulkan |
| glibc | 2.31 set — already pinned | existing | Wine needs ≤ GLIBC_2.29 (measured); Mesa: verify in P5 |
| Host virglrenderer | **1.2.0** (Ubuntu package; soname 1.10.0), Venus compiled in | — | plus the dma-buf `DRM_RDWR` shim (§0.2 item 14) |
| Host QEMU | 10.2.1 | — | recipe: `tools/cp/p0guest/host-venus.sh` |
| Guest reference | Ubuntu 26.04 cloud image (GPG + SHA256 verified), Mesa 26.0.8 venus | — | the Linux-guest column only |

---

## 3. Current-state snapshot (grounding)

### 3.1 What the game is (published facts; all **verify** in P0)

- Windows x64 PE, Direct3D 12 only on PC; REDengine 4.
- Published minimums for the 2.x patches: 12 GB RAM, SSD storage, ~70 GB disk,
  an AVX-capable CPU. Phantom Liberty adds disk.
- Audio middleware is Wwise; cutscenes are Bink video inside the engine.
- The REDlauncher can be skipped with `--launcher-skip`.
- It runs under Proton on Linux today (it is Steam Deck verified), which is the
  existence proof that Wine + vkd3d-proton + a Mesa Vulkan driver is enough.

### 3.2 What HobbyOS has (grepped 2026-11-14)

| Area | Fact | Source |
|---|---|---|
| Compat class | glibc 2.31 `ld.so`, threads/TLS/futex, fault signals, `/proc` — all proven by RimWorld M1/M2 | `docs/rimworld-plan.md` §6 M1, §11 |
| CPUs / processes | `MAX_CPUS 8`, `MAX_PROCESSES 192` (raised from 64 after RimWorld peaked at 145 threads) | `src/include/process.h:18-19`, `docs/rimworld-m2-thread-ceiling.md` |
| QEMU x64 | `-smp 8 -m 12288M`, q35, NVMe disk, virtio-net, virtio-tablet | `Makefile:71`, `Makefile:332` |
| User VA | `USER_VA_BASE 0x1000000000` (64 GiB); x64 identity map covers 0–8 GiB and the user window must clear it | `src/include/vm.h:11-27` |
| FPU | FXSAVE64/FXRSTOR64; no XSAVE, no `CR4.OSXSAVE`, no XCR0 | `src/kernel/arch/x64/fpu.c:9-60` |
| GS | *(v1 correction)* **no `swapgs` anywhere**: GS_BASE = KERNEL_GS_BASE = `&cpu_locals[cpu]`, `syscall_entry` uses `gs:` directly; `L_ARCH_SET_GS`/`GET_GS` defined but unhandled. Lane C1-cpu adds the swapgs discipline (held, §0.2) | `src/kernel/arch/x64/trap.c:5934-5941`, `:6058-6059`, `src/include/linux_syscall.h:254-257` |
| Kernel image | linked/loaded 1:1 at `0x70000000`, measured end `0x75c72b90` (92 MiB, mostly `.bss`) | `linker_x64.ld:7`, `docs/cp-address-space.md` §1.1 |
| Frame pool | fixed extents [0.5, 1.75) + [4, 8) GiB = 5.25 GiB; no RAM map parsed; RAM above 8 GiB unused | `src/include/frame.h:36-39` |
| Present syscalls | `epoll_*`, `timerfd_*`, `memfd_create`, `sigaltstack`, `tgkill`, AF_UNIX + `SCM_RIGHTS` | `src/kernel/linux_compat*.c`, `src/kernel/fs.c:3693` |
| Absent syscalls | *(v1: measured, replaces the v0 guess)* console Wine: `symlink chmod fchdir fstatfs ftruncate sendmmsg getxattr fgetxattr fsetxattr fremovexattr utimensat`; game under Proton adds `PR_SET_SYSCALL_USER_DISPATCH`, `userfaultfd`, `eventfd2`, `inotify_*`, `process_vm_readv` (ext2 lane since added `ftruncate`/`fdatasync`/`fstatfs` for all fds) | `docs/cyberpunk-evidence.md` §6, `docs/cp-p0-host.md` |
| File mappings | `VM_OBJ_MAX_BYTES` 64 MiB *(1 GiB since cp/sys ca51e8cf)* per mapped object; NFS-backed objects exist (`VM_OBJ_NFS`) | `src/include/vm.h:160`, `:201` |
| Storage | FAT16 `disk.img`; read-only NFSv3; no writable symlink-capable FS | `README.md`, `src/kernel/nfs.c` |
| GPU ABI | `/dev/gpu` M1: capset 1 (VIRGL), contexts, 3D resources, fenced submit, transfers; explicitly no blobs, no resource mmap, no async fences | `docs/gpu-abi.md` §1, `src/include/gpu_abi.h:18-32` |
| X | `XHOBBY.BIN` (X.Org-derived) + IceWM; compat `libX11.so.6` is a hand-written shim of the RimWorld-measured symbol set | `README.md`, `src/user/xcompat/libx11.c`, `required-libx11.txt` |
| Audio | silent by decision; `alsa_null.c`, `pulse_null.c` | `docs/audio.md`, `src/user/xcompat/` |
| Precedent perf | RimWorld main menu at 1920×1080 through virgl at ~1 fps, ~540 s launch-to-menu; present-path work moved a probe from 1.61 to 7.40 fps | `docs/rimworld-plan.md` (status 2026-11-07), `docs/l88-fps-report.md:136` |
| Host | `strix` 192.168.0.123: Ryzen AI MAX+ 395, 93 GB, RX 9070 XT (renderD129) + 8060S (renderD128), QEMU 10.2.1, virglrenderer 1.2.0 with Venus, KVM | `docs/cp-p0-guest.md` §1 |

### 3.3 What P0 must measure before anything is designed

1. Host-native fps/RSS/thread count/VRAM of the game under the pinned Wine +
   vkd3d-proton at 1280×720 Low and 1920×1080 Low.
2. **The Linux-guest column**: the same, inside a stock Linux guest in the same
   QEMU with Venus. This is the ceiling HobbyOS can reach on this GPU path, and
   it separates "Venus cost" from "HobbyOS cost" exactly as RimWorld's
   host-softpipe column did (`docs/rimworld-m7-perf.md` §1).
3. The syscall histogram of every Wine process (`wineserver`, the game,
   `services.exe`, `winedevice.exe`, `explorer.exe`, …).
4. The Vulkan features and extensions vkd3d-proton actually enables for this
   game, and whether Venus forwards each one.
5. The process's VA layout: which fixed low addresses are mapped, the largest
   single reservation, the highest address used.
6. File access patterns: which archives are `mmap`ed versus `read`, and the
   largest mapping (decides whether `VM_OBJ_MAX_BYTES` must grow or mappings
   must stream).
7. Whether the game reaches the menu with no audio device at all.
8. Whether the game runs with AVX masked off (decides whether XSAVE is a hard
   prerequisite or merely a performance item).

---

## 4. Gap analysis → scheduled work

| Capability | Gap | Work package |
|---|---|---|
| AVX state | no XSAVE/XRSTOR, no XCR0, signal frames carry only the FXSAVE image | P1 / C1 |
| Low VA in compat processes | identity map occupies 0–8 GiB of every AS | P1 / C1 (MMU) |
| GS base | no `ARCH_SET_GS`; user `MSR_GS_BASE` not saved/restored per thread | P1 / C1 |
| VA ceiling | Wine expects the full 47-bit user range and large `PROT_NONE` reservations (verify) | P1 / C1 |
| Wine syscall surface | measured in P0; likely `eventfd`, `pidfd`-family, `prctl` variants, `getrandom`, `fcntl` locks, `statfs`, `sched_*`, more of `ioctl` (verify) | P2 / C2 |
| Wine process model | `wineserver` as a separate daemon over AF_UNIX with fd passing; shared memory between server and clients | P2 / C2 (sockets exist; prove at scale) |
| Writable FS | prefix, registry, saves, shader caches, `dosdevices/` symlinks, `flock`/`fcntl` locks | P3 / C3 |
| Large files | >4 GiB archives over NFS; mappings >64 MiB | P3 / C3 |
| X client surface | real libX11/xcb + XInput2 raw events, RandR, Render, Fixes, Cursor, XShm, Xinerama — and the server side of each in `XHOBBY.BIN` | P4 / C4 |
| Vulkan on CPU | lavapipe + Vulkan loader + winevulkan in the compat class | P5 / C5 |
| Vulkan on GPU | `/dev/gpu` amendment (context-init, capset 4, blobs, map, ring fences); Mesa venus backend for `/dev/gpu`; host QEMU flags | P6 / C6 |
| Present | scanout of a swapchain blob without readback | P7 / C6+C4 |
| Memory | ≥16 GiB guest RAM + a host-visible heap BAR; frame allocator must keep the PCI-hole tripwire (`docs/rimworld-plan.md` status 2026-11-07) | P1 / C1, P6 / C6 |
| Audio | null first; virtio-sound + ALSA later | P8 / C4 (null), P9 (real) |
| Launch | launcher row, prefix preseeded with settings, `--launcher-skip` | P8 / C0 |

---

## 5. Architecture decisions (binding once P0 closes)

**AD-1 — Upstream Wine + vkd3d-proton + DXVK's `dxgi`, built by us. Not Valve's
Proton build system.** Upstream Wine builds with a normal cross toolchain
against our sysroot; Proton expects the Steam Runtime container. Build only the
64-bit Unix side; 32-bit Windows code is a non-goal. Revisit trigger: §1.4.

**AD-2 — Same compat class, same glibc pin as RimWorld.** Wine's Unix side
(`wine`, `wineserver`, `ntdll.so`, `winex11.so`, `winevulkan.so`) is an
ordinary glibc compat process. Every DSO is compiled against glibc 2.31
headers (RimWorld AD-11). If the pinned Wine needs newer glibc symbols, bump the
**set** (as RimWorld §2 already treats it) and record why; do not shim.

**AD-3 — Compat processes get a Linux-shaped address space: no identity map in
their page tables; the kernel reaches physical memory through a higher-half
direct map.** This is the MMU redesign `docs/rimworld-plan.md` AD-9 avoided
("an MMU redesign that buys nothing else this project needs"). It is
unavoidable here: Windows code reads `KUSER_SHARED_DATA` at the fixed address
`0x7ffe0000`, and Wine's preloader reserves low ranges before anything else
loads (verify the exact list at the Wine pin). Order of work: first a design
note that inventories every place the kernel dereferences a physical address
through the identity map (`grep` for physical-to-virtual casts, the frame
allocator, page-table walkers, DMA bounce paths, device MMIO), then the
direct map for *all* processes, then the compat AS that drops the identity
range. Native processes keep `USER_VA_BASE` unchanged. Fallback (dormant): keep
the identity map but carve the first 2 GiB out of it for compat processes only.
Trigger: the inventory shows >50 identity-dependent call sites that cannot be
mechanically converted.
*v1 resolution (`docs/cp-address-space.md`):* the kernel image stays at its
`0x70000000` 1:1 alias (K0) and is a fixed supervisor-only hole
`[0x70000000, CP_KHOLE_HI ≤ 0x78000000)` in compat ASes; direct map at
`0xffff800000000000` (PML4[256], reusing today's 0–8 GiB page directories),
ioremap at `0xffff808000000000`; helpers `phys_to_virt`/`virt_to_phys`/
`kimg_pa`/`ioremap` (identity on ARM); 2 MiB pages only. Inventory by clang
AST cast scan: 208 class-A (mechanical), 23 non-mechanical (11 sites + 12
design items) → fallback does not fire, and the 2 GiB carve-out would be
insufficient anyway (the game uses all of 0–8 GiB, §0.2 item 4). Migration
S0–S8 per that doc. K1 (higher-half kernel, needs a new boot protocol: the
kernel boots as ELF32 multiboot1) trigger: any required Wine/game mapping
inside the hole, or the image outgrowing 0x78000000. Kernel-mode faults on
compat user pages must be demand-filled below `USER_VA_BASE` too (today gated
on `cr2 >= USER_VA_BASE`, `trap.c:5646`), and low-linked `ET_EXEC` must be
accepted (`elf_loader.c:488`).

**AD-4 — XSAVE with XCR0 = x87 | SSE | AVX in v1; the kernel's XCR0 masks
AVX-512.** `CR4.OSXSAVE=1`, area size from CPUID leaf `0xD`, `XSAVE`/`XRSTOR` on context
switch (XSAVEOPT/XSAVES are later optimisations), Linux-shaped signal frames
with the extended state and `FP_XSTATE_MAGIC1/2`, `AT_HWCAP2` and
`AT_MINSIGSTKSZ` in auxv. *(v1)* QEMU-side AVX-512 masking is unnecessary:
XCR0 never enables AVX-512 state, so the area is 832 bytes on any CPU and
XGETBV-checking software never selects it; `X64_KVM_CPU='-cpu host,-avx512f'`
remains available. KVM boots default to `-cpu host` (`X64_KVM_CPU`), TCG is
untouched, `-cpu qemu64` restores the FXSAVE path. Wine reads extended state
only via MAGIC1 at fpregs+464 and a standard-format header at +512, so the
48 `sw_reserved` bytes must be zero when no XSAVE area follows, and ntdll runs
XSAVE/XRSTOR in user mode using CPUID.0xD ∩ `xgetbv(0)`. P0 showed AVX is not
needed to reach the menu (§0.2 item 2): this AD is required for correctness
of AVX-using threads, not as a P8 gate. ARM is untouched. The kernel C code
stays `-mno-avx` (`src/kernel/arch/x64/fpu.c:9`).

**AD-5 — Vulkan through Mesa Venus over a HobbyOS renderer backend; lavapipe
for bring-up; no DRM UAPI in the kernel (yet).** Mesa's venus driver
abstracts its transport behind `vn_renderer`; the HobbyOS backend
(`vn_renderer_hobbyos.c`, modelled on the vtest backend) speaks `/dev/gpu`,
exactly as RimWorld's virgl winsys does (`docs/gpu-abi.md`). The `/dev/gpu`
ABI grows an **M2 amendment** (F3): context-init with a capset id (Venus =
capset 4, verify), blob resource create, map/unmap of blobs into the caller's
address space through the device's host-visible shared-memory region,
ring-indexed async fences with interrupt completion, and scanout of a blob.
This keeps RimWorld AD-4's rule (graphics ABI is ours, not Linux's). Revisit:
if native context (§1.4) is promoted, evaluate a Linux-virtgpu-UAPI-compatible
node instead, because libdrm's amdgpu virtio path assumes it.

**AD-6 — Present by scanout, not by readback. Fullscreen only in v1.** The
swapchain images are host blobs; present is `SET_SCANOUT_BLOB` of the next
image, so pixels never cross into the guest. X keeps windowing and input; the
game's fullscreen window is the only thing on the screen while it runs (the
same "hand the screen to the game" idea as Doom's depth-8 `:0.1` screen in
`README.md`). Rationale: `docs/l88-fps-report.md` shows the present path alone
was worth 4.6× on a RimWorld present probe (1.61 → 7.40 fps); a 1080p BGRA readback is ~8 MB per frame before
X even copies it. Windowed presentation is a later lane.

**AD-7 — Game data read-only on NFS; mutable state on a new writable ext2
volume; `/tmp` on tmpfs.** The game tree (~70 GB) is exported read-only from
the host, as RimWorld's assets are. The Wine prefix, saves, vkd3d-proton's
pipeline cache and Mesa's shader cache live on an ext2 filesystem on a
**second** NVMe disk (made with `mke2fs` on the host), so FAT16 `disk.img`
stays exactly as it is. ext2 because it is small (no journal), has symlinks,
large files and POSIX permissions, and the host can inspect it with standard
tools. Runner-up: NFSv3 write operations (CREATE/WRITE/SYMLINK/RENAME/REMOVE/
SETATTR/COMMIT) plus NLM for locks. Trigger to switch: the ext2 driver is still
unreliable at the end of P3.
*v1:* the data disk is **NVMe namespace 2** of the boot controller
(`-device nvme-ns,drive=cpdata,nsid=2`, attached when `CPDATA` — default
`cpdata.img` — exists), made by `tools/cp/mkcpdata.sh` (`mke2fs -t ext2 -b 4096
-I 256 -O ^dir_index -m 0`), mounted at **`/EXT`**. Feature policy: unknown
INCOMPAT (anything but FILETYPE) refuses the mount; unknown RO_COMPAT mounts
read-only; `dir_index` directories are read linearly and lose
`EXT2_INDEX_FL` when modified. `/tmp` must be a tmpfs with unlink-while-open
files and coherent `MAP_SHARED` pages **in P2**, not P3 (KUSER, §0.2 item 9).
Cache paths are set explicitly by the launcher: `VKD3D_SHADER_CACHE_PATH`
(else vkd3d-proton writes next to the exe/CWD) and `MESA_SHADER_CACHE_DIR`.

**AD-8 — Synchronisation: Wine's in-server sync first.** No `futex_waitv`, no
`eventfd`-based esync, no ntsync device until profiling asks for one (§1.4).
*v1:* guaranteed by the build — upstream Wine 11 has only ntsync or
server-side sync, and our 5.4 kernel headers compile ntsync out
(`server/inproc_sync.c:243-292`). Measured host cost of server-side sync:
benchmark −23 %, menu −38 % (one run, `docs/cp-p0-host.md`); expect the fast-sync
trigger after P8, and expect wineserver round-trips (pipes, `rt_sigprocmask`)
to be HobbyOS's hottest syscall paths.

**AD-9 — Audio: null in v1.** Wine with no audio endpoints; P0 item 7 proves
the game tolerates it. virtio-sound + an ALSA path is P9 work, and it also
lifts the GB-029 decision for every other program.

**AD-10 — Measured on KVM on the Linux workstation only.** No TCG numbers, no
Mac numbers. Every fps figure is reported next to two baselines: host-native
and the Linux-guest-on-Venus column (§3.3 item 2).

**AD-11 — x86_64 only.** ARM builds keep compiling (new code is
`#ifdef __x86_64__` where architecture-specific); no ARM acceptance is claimed.

**AD-12 — No game files in git.** `tools/cp/check-no-game-files.sh` (cloned
from `tools/rw/`) runs in every gate; `.gitignore` entries for the staging
paths.

---

## 6. Milestones

Each milestone: tasks with paths, a **runnable gate**, and an **evidence**
requirement (raw output from the real case, pasted into the lane report and the
milestone's `## Evidence` block here). Sizes are relative (S < M < L < XL),
not promises.

### P0 — Evidence, pins and the go/no-go (size M; host-only, no HobbyOS code)

- [x] `tools/cp/`: `trace-host.sh` (strace `-f -c` histogram across all Wine
      processes), `fps.sh` (MangoHud CSV → median/p90), `vkfeatures.sh`
      (vkd3d-proton log of enabled features/extensions), `vamap.sh`
      (`/proc/<pid>/maps` snapshots at launch, menu, in-game), `files.sh`
      (archive list with sizes; mmap vs read from strace).
- [x] `tools/cp/fetch-runtime.sh`: Wine, vkd3d-proton, DXVK, Vulkan loader,
      X client libraries — sha256-verified into `third_party/cp-runtime/`
      (git-ignored). Fills §2.
- [x] `tools/cp/check-no-game-files.sh` + `.gitignore`.
- [x] Host-native run under the pinned Wine (not Proton) to the menu and
      in-game; record §3.3 items 1, 3, 5, 6.
- [x] Linux guest under the same QEMU with Venus (`tools/cp/host-venus.sh`):
      same game, same settings; record item 2 and item 4.
- [x] AVX-masked Linux-guest run (item 8) and no-audio run (item 7).
- [x] `docs/cyberpunk-evidence.md`: all of the above, raw.

**Gate P0:** `docs/cyberpunk-evidence.md` contains the histogram, the fps table
with both columns, the Vulkan feature list with a Venus yes/no per row, and the
VA map. **Go/no-go:** if the Linux-guest-on-Venus column is below 15 fps at
1280×720 Low, promote native context (§1.4) before P6 starts. If Venus lacks a
feature vkd3d-proton requires, same. Either way P1–P5 proceed.

**P0 status: CLOSED 2026-11-15 — GO for Venus.** All items above were done by
lanes C2-wine, C7-p0host and C7-p0guest; the evidence is split across
`docs/cyberpunk-evidence.md` (Wine), `docs/cp-p0-host.md` and
`docs/cp-p0-guest.md` rather than one file. Deviations: the host column used
Proton Experimental (wine-11.0) because it was the user's working setup —
distro wine-10.0 with Proton's DLLs reached the user-agreement screen, so the
Proton trigger (§1.4) did not fire; the X client library pins moved to P4; the
guest benchmark was skipped at the user's request. Headline numbers: §0.2.

### P1 — CPU state and the compat address space (size XL; the risk)

- [x] Design note `docs/cp-address-space.md` (lane C1-as, merged): inventory,
      direct-map layout, F1, migration S0–S8.
- [x] *(wave 2, C1-mmu)* Higher-half direct map for the x64 kernel (S0–S3): helpers, convert the
      208 class-A sites, the 23 design items; *(v1 tripwire)* native v2 roots
      drop the identity map so any leftover dereference faults as
      `[IDMAP-TRIP]`; the AST cast scan (`docs/cp-address-space.md` App. A)
      gates new code.
- [x] *(wave 2, C1-mmu; 16 GiB guest → 15.6 GiB pool)* **Multiboot memory-map parser** and a frame pool that grows above 8 GiB
      only after the direct map is live (S3b) — prerequisite for a ≥16 GiB
      guest (§0.2 item 6).
- [x] *(wave 2, C1-mmu; low `ET_EXEC` reviewed, not run — no execve yet)* Compat AS: user mappings permitted from `0x10000` up to
      `0x7ffffffff000` minus the kernel hole; ships first with an 8 GiB floor,
      then drops to `0x10000`; `MAP_FIXED` (hole → ENOMEM),
      `MAP_FIXED_NOREPLACE` (existing mapping or hole → EEXIST, never EINVAL),
      `PROT_NONE` reservations of ≥64 GiB singly and ≥141 GiB in total without
      committing frames; kernel-mode demand faults on compat user pages below
      `USER_VA_BASE`; low-linked `ET_EXEC` accepted.
- [x] XSAVE (AD-4) — lane C1-cpu, merged (XCR0=7 / 832 B even on an AVX-512 host).
- [x] swapgs discipline + `ARCH_SET_GS`/`ARCH_GET_GS` — lane C1-cpu, merged.
- [x] Probes: `linux_probe_avx.c`, `linux_probe_gs.c` (C1-cpu, passing);
      `linux_probe_lowva.c` (map `0x7ffe0000` and `0x10000`, read/write them;
      `MAP_FIXED` into the kernel hole → ENOMEM, NOREPLACE → EEXIST).
- [ ] Regression: Doom ELF, RimWorld to menu, and the full host/unit tiers on
      the new MMU, plus `-cpu qemu64` (FXSAVE fallback) runs of the probes.
      *v1:* `doom_elf_test` needs `doom10.elf`, which is not on linuxci
      (needs the id sources + a musl X11 sysroot fetched); the T3 wave wedges
      on base (§0.2 item 17) — use the probe set + unit tiers until fixed.

**Gate P1:** the three probes print their markers and exit 0 in QEMU/KVM;
`make doom_elf_test` passes; RimWorld still reaches its menu. Evidence: serial
logs of all of it.

### P2 — Wine runs Windows console programs (size XL)

- [x] `tools/cp/build-wine.sh`: Wine 11.0 (64-bit) against the glibc 2.31
      sysroot, staged `/CP/WINE/{bin,lib/wine/x86_64-unix,lib/wine/x86_64-windows,share/wine}`
      (202 MiB); proven on the host in a pinned-only chroot (lane C2-wine).
- [x] *(wave 2, C2-sys + C3-ext2b)* Syscalls from the measured list (§0.2 item 10), in order of first use:
      **`fchdir`** first, then `symlink`, `chmod`, `fstatfs`, `ftruncate`
      (the last two now exist for all fds via the ext2 lane), `sendmmsg`, the
      xattr family (answer ENODATA/ENOTSUP as Wine tolerates — verify),
      `utimensat`; each with a probe or a golden row.
- [x] *(wave 2, C2-sys; `MAP_PRIVATE` of a tmpfs/memfd object still → ENOTSUP, wave 3)* tmpfs `/tmp`: unlink-while-open files, coherent `MAP_SHARED` pages across
      processes (wineserver writes KUSER, clients read it), memfd up to and
      beyond 64 MiB (Wine's largest is exactly 64.0 MiB, §0.2 item 12).
- [x] *(wave 3, C2-wine2 + C1-thr)* `wineserver` lifecycle: daemon start, AF_UNIX socket under `/tmp`,
      fd passing, shared memory mapping, lock file, clean shutdown; SIGUSR1/
      SIGQUIT via `tgkill` (suspend / thread kill).
- [x] *(wave 3; NFS at 32 KiB READs, cp/nfs + cp/nfsfix)* Staging: `/CP/WINE` reachable as executable + mmappable files (NFS
      exec/mmap exist today; ext2 mmap/exec is P3 work) and the 2.31 runtime.
- [x] *(wave 3, C2-wine2; `MAP_PRIVATE` CoW over memfd/tmpfs)* Prefix on tmpfs first (decouples P2 from P3): `wineboot -i` completes
      (it runs past Wine's 5-minute boot-event wait, status 53, and the tree
      then finishes on its own; see §0.2 item 18).
- [x] *(wave 3, C2-wine2; built with `clang -target x86_64-windows` against
      Wine's own import libs, not MinGW)* `src/user/cp/winprobe.c`: threads, TLS slots,
      `%gs:[0x30]` TEB read, SEH (`__try`/vectored handler catching an access
      violation and a divide by zero), `QueryPerformanceCounter`,
      `GetTickCount64` (reads `KUSER_SHARED_DATA`), file I/O in `C:\`.

**Gate P2:** in QEMU/KVM, `wine cmd /c echo hello` prints `hello`;
`winprobe.exe` prints every marker and exits 0; `wineserver -k` leaves no
processes. Evidence: serial log + `ps` before/after.

**Gate P2 PASSED (2026-11-16, strix, QEMU/KVM, 12 GiB guest).** On the
merged tree (cp-wine2 72039e9c = `cyberpunk` cfc30b4f + C2-wine2), run23:
- G1: `hello`, status 0.
- G2: winprobe 41/41 markers + `[WP] done rc=0`, exit 0, both from `Z:`
  (NFS) and from `C:\` (the tmpfs prefix).
- G3: `wineserver -k` status 0, and `ps` after equals `ps` before
  (`WINEP2.BIN` only, no extra processes).

Evidence is in `docs/cp-wine2-evidence/p2-run23-PASS-merged-strix.txt`; the
same gate passed first on the pre-cp-nfs tree (run22). Commands:
`bash tools/cp/run-wine-p2.sh`, and
`P2_LS_ONLY=1 TIMEOUT_S=900 bash tools/cp/run-wine-p2.sh` for the six-reader
hash check.

### P3 — Storage for a Windows game (size L)

- [x] `src/kernel/ext2.c` (+ `ext2_vfs.c`, `ext2_compat.c`): read/write,
      symlinks, rename, unlink, truncate, `fsync`, large/sparse files; mounted
      at `/EXT` from NVMe namespace 2 (lane C3-ext2; two-boot gate, `e2fsck -fn`
      clean).
- [x] *(wave 2, C3-ext2b)* mmap of ext2 files (a VM object for ext2) and exec from ext2 — needed
      before the prefix can hold PE/`.so` files.
- [x] *(wave 2, C3-ext2b: one lock manager, `vfs_lock.c`, for FAT16/ext2/tmpfs)* `flock`/`fcntl` record locks (Wine and the vkd3d-proton cache use them;
      verify which); `chmod`/`fchmod`/`chown`/`utimensat` on ext2.
- [x] *(wave 2, C3-ext2b)* `VFS_PATH_MAX` 256 → ≥ 1024 (Windows MAX_PATH plus the prefix path).
- [x] *(C3-nfs2 26a36445, 46b7c0ba: a 5 GiB file's sha256 matches the host's, one 176 MiB `read()` matches; guard G8)* NFS: files >4 GiB (64-bit offsets throughout); single `read()`s up to
      ~172 MB (the game's `shader_final.cache`, §0.2 item 12). Archive mmap is
      *not* needed (P0: archives are read, not mapped); the 64 MiB
      `VM_OBJ_MAX_BYTES` cap matters for memfd and GPU blob mappings instead.
- [x] *(C3-pfx: two-boot run 7, P1–P5 PASS, `docs/cp-pfx-evidence/`)* Prefix moves to ext2; tmpfs for `/tmp`.

**Gate P3 — PASSED (2026-11-16):** prefix survives a reboot *(C3-pfx run 7: boot 2 reuses all 1,012 entries, hello + winprobe 41/41 from Z: and from C:\ on ext2, `e2fsck -fn` clean)*; a 5 GiB file over NFS matches the host's sha256 *(C3-nfs2 G8)*; the game's largest mapping shape (66,704,848-byte file, 3,166,208 bytes at 0x397a000, `MAP_PRIVATE|MAP_FIXED` RW) and 200 MiB memfd/shared-anon/ext2 objects fault in correctly *(`linux_probe_bigobj`)*. Original wording: prefix survives a reboot; a >4 GiB archive read over NFS in the
guest matches the host's sha256; an `mmap` of the game's largest mapped file
(from P0) faults in correctly. Evidence: serial log + hashes.

### P4 — Windows, input and the X surface Wine needs (size L)

- [x] *(C4-x, C4-x2: 39 libraries incl. Xi/Xcomposite/FreeType 2.14.3, max GLIBC_2.27, staged at `/CP/X11`; Wine rebuilt with x/xinput2/xrandr/xrender/xfixes/xcursor/xinerama/xcomposite/xshm/freetype/vulkan, `CP_WINE_VARIANT=x`)* Real libX11/libxcb and the extension libraries built against glibc 2.31
      and staged for compat processes (the RimWorld shim stays for RimWorld
      until it is proven redundant).
- [x] *(C4-x2, in-OS xprobe: one-screen session `HB_SECOND_SCREEN=0` gives XINERAMA + Composite; XI2 raw deltas exact from a relative slave fed by kernel REL events; 12 RandR modes with a real mode set; MIT-SHM still absent, plan in `docs/cp-x2.md` §6)* `XHOBBY.BIN`: the server side of XInput2 (raw motion), RandR (one mode
      per supported resolution), Render, Fixes, Cursor, MIT-SHM — **verify**
      which are already present in the X.Org-derived server.
- [x] *(C4-x2 harness, C1-k4b runs: a fullscreen 1280×720 window fills the glass after a RandR mode set; `ClipCursor` holds under IceWM and raw deltas keep arriving)* Wine X11 driver configuration: fullscreen without a window-manager fight
      (IceWM and Wine's fullscreen hints), pointer grab/clip for mouselook.
- [x] *(Wine is built with no audio driver; winecfg's Audio tab opens and responds)* Null audio (AD-9).

**Gate P4 — PASSED (2026-11-16, `docs/cp-k4-evidence/p4-k4b-strix.txt`; three runs on strix):** winecfg 3/3 tab clicks change the dialog, notepad shows the typed text, and the raw-input probe reads relative deltas (free dx=70 dy=30, clip dx=850 dy=25, cursor inside the clip). Original wording: `winecfg` opens in an IceWM window and its tabs respond to QMP
clicks; `notepad` accepts typed text; a MinGW raw-input probe prints relative
mouse deltas from QMP-injected motion. Evidence: screendumps + serial log.

### P5 — Vulkan on the CPU (size L)

- [x] *(C5-vk, C5-vk2: `docs/cp-vk.md` §1–4, §8.1)* Mesa lavapipe built for the compat class; LLVM JIT works under compat
      (`PROT_EXEC` anonymous memory, `mprotect` transitions).
- [ ] Vulkan loader + ICD JSON staging; winevulkan.
- [x] *(C5-vk2: Mesa's software `put_image` path; Present 1.2 fake-msc paces it at ~30 Hz, render median 1 ms)* A Vulkan X11 WSI path that works with lavapipe (software presentation
      is acceptable here — this rung is about correctness).
- [ ] Probes: Linux `vkcube` *(done, C5-vk2: 65 s in the guest X session, 1853 frames, two distinct screendumps, `docs/cp-vk2-evidence/item2-vkcubex-x11.txt`)*; `vkcube.exe` under Wine; a DXVK D3D11 triangle;
      a vkd3d-proton D3D12 triangle (MinGW-built, in `src/user/cp/`).

**Gate P5:** all four render recognisably in screendumps and run ≥60 s without
a fault. Evidence: screendumps + serial log.

### P6 — Venus: the host GPU (size XL; can start alongside P1)

- [ ] Host: `tools/cp/host-venus.sh` — virglrenderer with Venus enabled, QEMU
      device flags (Appendix B), memfd-backed shared guest RAM.
- [ ] Kernel (`src/kernel/virtio_gpu_3d.c`, `src/kernel/gpu_abi.c`):
      `VIRTIO_GPU_F_CONTEXT_INIT`, `VIRTIO_GPU_F_RESOURCE_BLOB`, the
      shared-memory capability region (host-visible BAR) mapped into user
      space with the right cache attributes, blob create/map/unmap,
      ring-indexed fences completed from the interrupt path.
- [ ] `docs/gpu-abi.md` M2 amendment (F3). **Coordinate with the in-progress
      uncommitted work in `src/kernel/gpu_abi.c` before starting.**
- [ ] Mesa: `vn_renderer_hobbyos.c` over `/dev/gpu`; build `-Dvulkan-drivers=virtio`.
- [ ] Probes: Linux `vkcube` on Venus; then the P5 D3D12 triangle on Venus;
      a fence-latency microbenchmark (submit→signal µs, p50/p99).

**Gate P6:** both probes render on the host GPU (vulkaninfo inside the guest
names the Radeon via Venus); fence latency reported. Evidence: screendumps,
`vulkaninfo --summary` output, benchmark rows.

### P7 — Zero-copy presentation (size M)

- [ ] WSI path that presents a swapchain blob with `SET_SCANOUT_BLOB` (AD-6)
      and hands the screen back to X on exit or focus loss.
- [ ] Present-cost instrument comparable to RimWorld's `[HBT]` rows
      (`docs/l88-fps-report.md`).

**Gate P7:** `vkcube.exe` fullscreen at 1920×1080 runs at the host's refresh
rate or reports why not; per-present cost measured. Evidence: fps rows +
screendump.

### P8 — Cyberpunk reaches its main menu (size L, mostly unknowns)

- [ ] Launch recipe: launcher row in the Games menu, `--launcher-skip`, the
      game's settings preseeded in the prefix (1280×720, Low, RT off, FSR on —
      exact file and keys from P0).
      *v1:* the preseeded profile must already carry the **user's own** first-run
      choices (agreement, account offer skipped, telemetry rejected — written
      by the game to `Saved Games/CD Projekt Red/Cyberpunk 2077/user.gls` and its
      settings); never automate those screens. The launcher sets
      `VKD3D_SHADER_CACHE_PATH`, `MESA_SHADER_CACHE_DIR`, the vkd3d-proton/DXVK
      DLL overrides (without them Proton's Wine silently uses upstream
      `d3d12`/`dxgi`: FL 11_1, adapter "Red Hat VirtIO GPU"), and runs with the
      game's own `bin/x64` read-only (the vkd3d cache otherwise lands there).
- [ ] `MAX_PROCESSES` ≥ 256 (host: 173–190 threads at the menu, §0.2 item 13);
      cheap `getrusage(RUSAGE_THREAD)`.
- [ ] Shader/pipeline caches on ext2, warmed by a first run; first-run compile
      time recorded separately from launch time.
- [ ] Fix whatever the first boots find: this milestone is where most
      unplanned work will land, as RimWorld's M6 showed.

**Gate P8:** a screendump of the game's own main menu, confirmed by OCR of its
menu items, plus launch-to-menu time. Evidence: screendump, OCR output, serial
log, `check-no-game-files` output.

### P9 — Playability (size L; open-ended)

- [ ] Load a save made on the host; play 10 minutes; save; reload.
- [ ] fps vs both baselines at 1280×720 Low and 1920×1080 Low.
- [ ] Native context lane if P0 or P9 numbers call for it (§1.4).
- [ ] virtio-sound + ALSA (AD-9).

**Gate P9:** recorded session (screendump sequence + fps rows), no crash, the
save reloads. Target: ≥30 fps at 1280×720 Low, or within 70 % of the
Linux-guest column, whichever is lower.

---

## 7. Parallel execution plan

### 7.1 Lanes

| Lane | Scope | Owns (files/dirs) | Depends on | Runs parallel with |
|---|---|---|---|---|
| **C0** integrator | Makefile/QEMU lines/disk recipes, this doc, merges, gates on the merged tree | `Makefile`, `docs/cyberpunk.md`, disk recipes | all | — |
| **C1** cpu+mmu | P1: direct map, compat AS, XSAVE, GS base | `src/kernel/arch/x64/{mmu,fpu,trap}.c` (MMU/FPU/GS paths), `src/kernel/vm.c`, `src/include/vm.h` | P0 | C6, C7 |
| **C2** wine | P2: build, syscalls, wineserver, probes | `tools/cp/build-wine.sh`, `src/kernel/linux_compat_*.c`, `src/user/cp/` | C1 | C3, C4, C5 |
| **C3** storage | P3: ext2, locks, NFS large files | `src/kernel/ext2.c` (new), `src/kernel/nfs.c`, `src/kernel/vfs.c`, `src/kernel/fs.c` (FS paths only) | — (needs C1 only for the final prefix test) | all |
| **C4** x-surface | P4: client libs, server extensions, input, null audio | `src/user/x11/**` server side, `tools/cp/build-xlibs.sh` | C1 for in-guest tests | C3, C5, C6 |
| **C5** lavapipe | P5: Mesa swrast Vulkan, loader, winevulkan staging | `tools/mesa/**` (Vulkan builds), `tools/cp/vk-*` | C2 for the `.exe` probes | C3, C4, C6 |
| **C6** venus | P6+P7: host recipe, `/dev/gpu` M2, venus backend, scanout | `src/kernel/virtio_gpu_3d.c`, `src/kernel/gpu_abi.c`, `src/include/gpu_abi.h`, `docs/gpu-abi.md`, Mesa backend patch | P0 | C1, C3, C4, C5 |
| **C7** evidence | P0 harness and every later fps/perf row | `tools/cp/` (harness), `docs/cyberpunk-evidence.md` | — | all |

**One writer per file.** `Makefile`, the disk recipes and this document are
C0-only; lanes submit diffs in their reports. `linux_compat_*.c` are C2-only.
`src/kernel/arch/x64/trap.c`: C1 owns the GS/FPU paths, C2 owns the compat
syscall arm — touch only your branch of the file.

### 7.2 Waves

- **Wave 1**: C7 (P0, host-only) and C6's host recipe. Nothing in the guest
  changes until P0's go/no-go is recorded.
- **Wave 2**: C1 design note → direct map; C3 ext2 on the existing kernel;
  C6 kernel transport (blobs/fences are independent of the AS redesign);
  C4 builds the client libraries.
- **Wave 3**: C1 lands the compat AS + XSAVE + GS; C2 starts Wine bring-up on
  the merged tree; C5 brings up lavapipe with Linux probes.
- **Wave 4**: C2's `.exe` probes; C4 in-guest windows/input; C5 `vkcube.exe`
  and D3D12 triangle; C6 Venus probes.
- **Wave 5**: C6 scanout (P7); C0 integrates → P8 → P9.

### 7.2b Wave 2 (v1, 2026-11-15) — the lanes actually running

Wave 1 (C1-as, C1-cpu, C2-wine, C3-ext2, C7-p0host, C7-p0guest) is done or
held (§0.2). Wave 2 starts from branch `cyberpunk` at the v1 commit; every
lane works in `~/hobbyos-cp-lanes/<lane>` (branch `cp-<lane>`) and gates on
`linuxci` under `flock /tmp/cp-qemu.lock`.

| Lane | Scope (milestone) | Owns | Must not touch |
|---|---|---|---|
| **C1-mmu** | P1 S0–S3b + compat AS: direct map, helpers, class-A conversions, multiboot memory map + frame pool above 8 GiB, compat AS from `0x10000` with the kernel hole, `MAP_FIXED`/`MAP_FIXED_NOREPLACE` semantics, ≥64 GiB `PROT_NONE` reservations, low `ET_EXEC`, `linux_probe_lowva` | `src/kernel/arch/x64/mmu.c`, `src/kernel/vm.c`, `src/include/vm.h`, `src/kernel/frame.c`, `src/include/frame.h`, the page-fault path in `trap.c`, `elf_loader.c` (load-address policy), the mmap path in `linux_compat_mem.c`, the S1 one-line conversions anywhere (as minimal diffs) | the syscall/IRQ entry stubs, FPU and signal-frame code (C1-cpu) |
| **C2-sys** | P2 kernel prerequisites: `fchdir`, `sendmmsg`, xattr family (Wine-tolerated errnos), a tmpfs `/tmp` with symlinks, unlink-while-open and coherent `MAP_SHARED`, memfd > 64 MiB, `eventfd2`; probes | `src/kernel/linux_compat_fd.c`, `linux_compat_misc.c`, `linux_compat_proc.c`, a new `src/kernel/tmpfs.c`, the memfd/shared-file paths | the mmap address-placement logic (C1-mmu), ext2 (C3) |
| **C3-ext2b** | P3 remainder: mmap + exec of ext2 files, `chmod`/`fchmod`/`chown`/`utimensat` (syscalls + VFS + ext2), `flock`/`fcntl` locks, `VFS_PATH_MAX` ≥ 1024 | `src/kernel/ext2*.c`, `src/kernel/vfs.c`, the file-object/VM-object glue for ext2 | tmpfs (C2-sys) |
| **C4-x** | P4 groundwork: build real libX11/libxcb + Xext/Xi/Xrandr/Xrender/Xfixes/Xcursor/Xinerama/xcb-shm/xcb-present against the 2.31 sysroot (pins + sha256), stage them for compat processes; audit `XHOBBY.BIN` for XInput2/RandR/MIT-SHM/Present | `tools/cp/x11/`, `docs/cp-x11.md`, `third_party/cp-runtime` pins for X | kernel sources |
| **C5-vk** | P5 groundwork: Mesa lavapipe + the pinned Vulkan loader for the compat class; first a headless probe (`vulkaninfo`, offscreen triangle read back and blitted to `/dev/fb0` or dumped) inside HobbyOS, then X11 WSI once C4's libxcb exists | `tools/cp/vk/`, `tools/mesa/` (Vulkan builds only), `src/user/cp/vk*` | kernel sources except a reported diff if JIT needs a kernel fix |
| C6-venus | P6 kernel transport (`/dev/gpu` M2) — **not started**: it edits `src/kernel/gpu_abi.c`, which has uncommitted parallel work in the user's main repo; needs the user's decision first | — | — |
| C1-cpu | (wave 1, held) fix tramp_sig T2, then merge | as wave 1 | — |

Merge order: C1-cpu → C3-ext2b / C2-sys (either order) → C1-mmu (rebased on
all of them; it is the riskiest). C4-x and C5-vk merge whenever green. Shared
hot files (`linux_compat.c` dispatch tables, `Makefile`): each lane adds rows
or blocks, never reorders; the integrator resolves conflicts.

### 7.2c Wave 3 (v2, 2026-11-15) — toward Gate P2

Wave 2 merged in full (`cyberpunk` @ 3da44468; merge order cpu → sys →
ext2b → mmu, each re-gated on the merged tree). **Gate host is `strix`**
(linuxci's disk failed on 2026-11-15): checkouts under `strix:~/cp/lanes/`,
every boot under `flock /tmp/cp-qemu.lock`, every build with
`MAKEFLAGS=-j16` (the gate scripts' nested makes have no `-j`).

What wave 2 left between HobbyOS and `wine cmd /c echo hello`:
`wineserver --version`, `wine --version` and `wineserver -f`/`-k` already run
in the guest (C2-sys smoke). Missing: a compat `execve(2)` that loads an ELF
from any filesystem (Wine re-execs itself through its preloader); thread-shaped
`clone()` flag sets that `process_clone()` refuses with EINVAL (join row E);
`MAP_PRIVATE` of tmpfs/memfd objects (ENOTSUP today, so Wine moves its section
files onto FAT); then the Windows side proper (TEB via `%gs`, KUSER at
`0x7ffe0000`, SEH).

| Lane | Scope (milestone) | Owns | Must not touch |
|---|---|---|---|
| **C2-exec** | P2: compat `execve(2)`/`execveat` from FAT16, ext2, NFS and tmpfs with full-length paths; static, static-pie (incl. `R_X86_64_IRELATIVE`) and PT_INTERP images; argv/envp/auxv; O_CLOEXEC; pid/tgid kept; dispositions reset; other threads killed; `process.cwd`/`exe_path` to `VFS_PATH_MAX`; `linux_probe_exec` | the exec path (`process_exec_current` and its helpers in `process.c`), `elf_loader.c`, `elf_image.c`, `program_loader.c`, the execve rows in `linux_compat_proc.c` | `process_clone`, the scheduler, futex (C1-thr); `vm.c` (C2-wine2) |
| **C1-thr** | P2: every `clone()` flag set glibc 2.31's `pthread_create` and Wine use; join rows A and E; the ~100 ms late delivery of a signal to a parked thread; `tgkill` SIGUSR1/SIGQUIT to a chosen thread (wineserver suspend); the `[FUTEX]` trace that splices into probe output | `process_clone` and thread creation in `process.c`, `linux_compat_thr.c`, futex, the scheduler wake paths | the exec path (C2-exec), `vm.c` |
| **C2-wine2** | Gate P2: `MAP_PRIVATE` copy-on-write of tmpfs/memfd (and ext2) objects; prefix on tmpfs; `wineboot -i`; `wine cmd /c echo hello`; `winprobe.exe` (threads, TLS, `%gs:[0x30]`, SEH, QPC, `GetTickCount64` via KUSER, `C:\` I/O); `wineserver -k` leaves no processes. Pulls `cyberpunk` as C2-exec and C1-thr merge | `vm.c` private-mapping/CoW paths, the mmap rows in `linux_compat_mem.c`, `tools/cp/` Wine harnesses, `src/user/cp/winprobe.c` | the exec path, clone/futex |

Merge order: whichever of C2-exec / C1-thr is green first, then the other,
then C2-wine2 last. Each merge is re-gated on the merged tree (all probes,
`ext2_gate.sh`, unit tier, host tests, castscan, ARM build).

**Status (2026-11-16): wave 3 is complete and Gate P2 passed.** Merged in
order: C2-wine2's CoW, truncate and mremap work; C1-thr; C2-exec;
C1-thr2/thr3; cp/nfs (32 KiB READs); cp/nfsguard; cp/nfsfix (the readahead
race); then C2-wine2 (the P2 harness, winprobe and the trap/fs fixes). Each
merge was re-gated on strix. The next wave's candidates are the open items in
§0.2 item 18, P3's remaining items (NFS > 4 GiB, prefix on ext2), and P4/P5; they are wave 4
(§7.2d). Still held: C6-venus (needs the user's `gpu_abi.c` decision).

### 7.2d Wave 4 (v4, 2026-11-16) — P3, P4 and P5, and the stability debt

Wave 4 starts from `cyberpunk` @ d7a63823 (Gate P2 passed). Gate host is
still `strix` (§7.2c rules: lanes under `strix:~/cp/lanes/`, every build with
`MAKEFLAGS=-j16`, and the §7.4 QEMU-slot, `/etc/exports` and NFS READ size
rules; since 2026-11-16 up to four guests run at once). `cmake`, `meson` and `ninja`
exist on strix in `~/.local/bin`, which a non-login ssh shell does not put on
`PATH`; the "x11 probe needs cmake" item of §7.2c was that, not a missing
package.

Two facts shape the wave:
- **Every Wine boot pays the NFS page-in.** P2 read 1.66 GB for about 200 MB of
  Wine, and the QEMU lock serialises all five lanes' boots, so the per-file
  readahead fix (C3-nfs2) speeds up every other lane. It merges first.
- **The staged Wine was built `--without-x --without-vulkan --without-freetype`**
  (`tools/cp/build-wine.sh` `WITHOUT=`). P4 and P5 need a second build with
  X11, the X extensions, FreeType and Vulkan. C4-x2 owns that build; it goes to
  a new work dir, so the P2 stage in `strix:~/cp/wine-build/` stays untouched.

| Lane | Scope (milestone) | Owns | Must not touch |
|---|---|---|---|
| **C3-nfs2** | P3 NFS + the page-in cost: a readahead window per file (keyed by file handle, a small LRU), keeping the invalidate-before-refill rule of §0.2 item 18 per window; 64-bit offsets everywhere and files > 4 GiB; single `read()`s up to ~172 MB; `fstat` mtime on NFS fds; symlinks in intermediate path components. Gate: P3's "> 4 GiB file over NFS matches the host's sha256", both NFS guards, and P2's page-in down from 1.66 GB | `src/kernel/nfs.c`, `src/include/nfs.h`, `tools/cp/nfs/`, `src/user/cpnfsbench.c`, `docs/cp-nfs.md` | `net.c` except a reported minimal diff |
| **C3-pfx** | P3 prefix: the Wine prefix on ext2 survives a reboot (`wineboot -i` in boot 1, `wine cmd /c echo hello` with no re-init in boot 2, `e2fsck -fn` clean); `VM_OBJ_MAX_BYTES` 64 MiB → ≥ 256 MiB for memfd, `MAP_SHARED` anon and ext2 objects (P0: Wine's largest memfd is exactly 64 MiB; GPU BOs of 192 MiB come later); FAT private mappings see later `write()`s; the "largest mapped file" part of Gate P3 | `src/kernel/ext2*.c`, `src/kernel/vfs.c`, `vfs_lock.c`, `tmpfs.c`, the VM-object size and file-object coherence code in `vm.c`/`vm.h`, `tools/cp/run-wine-p2.sh`, `src/user/winep2.c` | the unmap/protect/TLB paths of `vm.c` (C1-k4), NFS (C3-nfs2) |
| **C1-k4** | Stability: the intermittent wineboot-tree hang (all clients BLOCKED, no futex, no timer — suspect the `pipe.c` park race), reproduced by a probe before it is fixed; cross-CPU TLB shootdown for `munmap`/`mprotect`/`mremap`; `MAX_PROCESSES` ≥ 256 (the game runs 173–190 threads); `execve` from a secondary thread makes that thread the leader (tid = pid); SIGBUS past EOF; `si_code` from the real fault (`ILL_ILLOPN`, `FPE_FLT*` from MXCSR/FSW, `SEGV_MAPERR`/`SEGV_ACCERR`) | `src/kernel/pipe.c`, the scheduler park/wake paths, `src/kernel/arch/x64/mmu.c` (TLB), the unmap/protect/remap and fault-to-signal paths of `vm.c`, the process table and the exec-from-thread step in `process.c`, signal mapping in `trap.c` | the VM-object code (C3-pfx), the compat syscall arm of `trap.c` except signal mapping |
| **C4-x2** | P4: Wine rebuilt with X11 + Xi2/RandR/Render/Fixes/Cursor/Xinerama/XShm + FreeType (from `third_party/freetype-2.14.3` against the 2.31 sysroot) + Vulkan; the C4-x libraries staged by a `make` rule; a one-screen DDX mode for Wine sessions (Composite and Xinerama come back); RandR modes or a clean refusal; relative pointer motion from the kernel mouse path through XI2 raw events; Wine fullscreen and pointer grab under IceWM; null audio (AD-9). Gate P4 | `src/user/x11/**`, `third_party/x11/ddx` (server side), `tools/cp/x11/`, `tools/cp/build-wine.sh`, the kernel mouse/input driver path for relative events, `tools/cp/p4/`, `src/user/cp/rawinput*` | NFS, ext2, `vm.c` |
| **C5-vk2** | P5: the lavapipe tree rebuilt on strix (linuxci's work dir is gone), now with the loader's xlib/xcb WSI and Mesa `-Dplatforms=x11` against C4-x's libraries; Linux `vkcube` in an X window for ≥ 60 s; then, on C4-x2's Wine build, winevulkan, `vkcube.exe`, a DXVK D3D11 triangle and a vkd3d-proton D3D12 triangle (MinGW-style PE probes built with clang + Wine's import libs). Gate P5 | `tools/cp/vk/`, `tools/mesa/` (Vulkan builds), `src/user/cp/vk*`, `src/user/cp/d3d*`, `tools/cp/p5/` | `tools/cp/x11/` and `build-wine.sh` (use them, report diffs) |
| C6-venus | P6 — still held for the user's `gpu_abi.c` decision | — | — |

Merge order: C3-nfs2 first, then whichever of C1-k4 / C3-pfx is green (both
touch `vm.c`, in different functions; the integrator resolves), then C4-x2,
then C5-vk2 (it needs C4-x2's Wine build for its `.exe` probes). Every merge is
re-gated on the merged tree with the full batch, including both NFS guards and
one Gate P2 run. Downloads beyond what strix already holds (DXVK 3.1.1 and
vkd3d-proton 3.1 from `docs/cp-pins.md`, any X library, glslang) wait for the
user's approval through the integrator.

### 7.3 Frozen interfaces (binding)

| # | Interface | Owner (sole writer) | Change protocol |
|---|---|---|---|
| **F1** | Compat address-space layout: lowest mappable VA, reserved ranges, 47-bit ceiling, direct-map base | C1 | Amendment here + C2 + integrator sign-off |
| **F2** | x86_64 signal frame with XSAVE state (Linux `ucontext`/`fpstate` shape, magic words, sizes) and auxv additions | C1 | Host golden regenerated; C2 sign-off (Wine parses this frame) |
| **F3** | `/dev/gpu` M2 amendment: new requests, structs, capset ids, fence semantics | C6 | `docs/gpu-abi.md` amendment + C5 sign-off (shared Mesa build) |
| **F4** | Staging layout: `/CP/WINE`, `/CP/VK`, prefix path on ext2, NFS mount of the game | C0 | Amendment here |
| **F5** | WSI ↔ scanout boundary: who owns the screen, hand-back on exit/focus loss | C6 + C4 jointly | Both lanes + integrator |

### 7.4 Integration protocol

- One integrator clone, branch `cyberpunk`; each lane gets
  `git worktree add ~/hobbyos-cp-lanes/<lane> -b cp-<lane>` from it.
- Local commits only, one phase per commit, repo message style; **no pushes**.
- The integrator merges lanes and runs gates on the merged tree, including the
  Doom ELF and RimWorld regressions after any C1 merge.
- **QEMU slots on the gate host (binding; 2026-11-16).** The user allowed up
  to four guests at once on strix (12 GiB each). Every boot runs under
  `bash tools/cp/qemu-slot.sh CMD...` (or `. tools/cp/qemu-slot.sh;
  cp_qemu_slot_acquire` for the rest of a shell), which takes one of four
  slots. Slot 1 is the old `/tmp/cp-qemu.lock`, so a leftover
  `flock /tmp/cp-qemu.lock` still counts as one slot; nested calls reuse the
  caller's slot. One boot per slot; never hold a slot across a batch.
- **`/etc/exports` on the gate host (binding).** Harnesses add and remove their
  export only through `bash tools/cp/exports.sh add|remove /srv/nfs/cp-<name>`.
  It holds `/tmp/cp-exports.lock` only while it edits the file (never across a
  boot, so several guests can each have an export), writes only the approved
  `127.0.0.1(ro,sync,no_subtree_check,insecure,all_squash,anonuid=65534,anongid=65534)`
  form, touches only its own line, verifies that every non-cp line is
  byte-identical before and after, and restores its snapshot on any failure.
  Harnesses call `remove` from an EXIT trap. Self-test:
  `bash tools/cp/exports_selftest.sh`. (Before 2026-11-16 each harness held
  the exports lock across its whole boot and `cmp`ed the whole file; one
  whole-file restore had put back another harness's line on 2026-11-15.)
- **NFS READ size guard (binding; the 32 KiB fix has been lost to merges more
  than once).** Any merge that touches `src/kernel/nfs.c`, `src/include/nfs.h`
  or `src/kernel/net.c`, and above all any merge from `main` (which still has
  the 1344-byte clamp), must pass `python3 tools/cp/nfs/rsize_scan.py` (also
  part of `make host_tests`) and the boot guard `tools/cp/nfs/rsize_guard.sh`
  on the merged tree before `cyberpunk` moves. Every integration gate batch runs
  both. Since cp-nfs2 the boot guard also covers G6 (concurrent readers), G7
  (per-file windows), G8 (64-bit, large reads, stat times, symlinks) and G9
  (hard mount under injected reply loss). When a conflict lands in those files, keep the cp/nfs side; see
  `docs/cp-nfs.md` §6.
- Game files: never in a worktree, never in a commit.

---

## 8. Testing & acceptance strategy

### 8.1 Tiers

1. **Host golden** (`make host_tests`): AS layout planner, signal-frame layout
   (F2), ext2 on-disk structures against `mke2fs` images, `/dev/gpu` M2 struct
   layouts.
2. **Kernel unit** (`./run_unit_tests_intel.sh`): direct-map conversions, XSAVE
   area sizing, blob map/unmap bookkeeping, ext2 allocator.
3. **In-OS probes**: Linux probes (`src/user/linux_probe_*.c`) and Windows
   probes (`src/user/cp/*.c`, MinGW-built), all on KVM.
4. **E2E**: the game, screendumped and OCR'd.

### 8.2 New tests inventory

| Test | Kind | Where |
|---|---|---|
| `linux_probe_avx` | in-OS | `src/user/linux_probe_avx.c` |
| `linux_probe_lowva` | in-OS | `src/user/linux_probe_lowva.c` |
| `linux_probe_gs` | in-OS | `src/user/linux_probe_gs.c` |
| `xstate_frame_test` (F2 layout) | host | `src/host/` |
| `ext2_test` | host + unit | `src/kernel/ext2_test.c` |
| `winprobe.exe` (threads, TEB, SEH, timers, files) | in-OS | `src/user/cp/winprobe.c` |
| `rawinput.exe` | in-OS | `src/user/cp/rawinput.c` |
| `tri_d3d11.exe`, `tri_d3d12.exe` | in-OS | `src/user/cp/` |
| `vn_fence_bench` | in-OS | `src/user/cp/vn_fence_bench.c` |
| `check-no-game-files` | host | `tools/cp/check-no-game-files.sh` |
| `rsize_scan` (32 KiB NFS READ regressions R1–R6, with selftest) | host (`make host_tests`) | `tools/cp/nfs/rsize_scan.py` |
| `rsize_guard` (2 READ RPCs per 64 KiB against a real nfsd) | boot | `tools/cp/nfs/rsize_guard.sh` |

### 8.3 Acceptance checklist (v1 "done")

- [ ] AVX, low-VA and GS probes pass; Doom ELF and RimWorld regressions pass on
      the new MMU.
- [ ] `wine cmd /c echo` and `winprobe.exe` pass; prefix persists on ext2.
- [ ] `winecfg` in IceWM; raw mouse deltas reach a Windows program.
- [ ] `vkcube.exe` and the D3D12 triangle on lavapipe and on Venus.
- [ ] Fullscreen scanout present measured.
- [ ] Cyberpunk main menu screendump, OCR-confirmed.
- [ ] 10 minutes played from a host save; save/reload; fps against both
      baselines.
- [ ] `git status` clean of game artifacts; ARM build still compiles.

### 8.4 Evidence standards

As `docs/rimworld-plan.md` §8.4 and `docs/gaming-retrospective.md`: no "done"
without raw output from the real case — serial logs for in-OS gates,
screendumps for visuals, harness rows for fps. A clean build is not a gate.
Never fabricate a number; if a measurement could not be taken, say so.

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | **The AS redesign (P1) destabilises the whole kernel** | Inventory first; tripwire on identity dereferences; full host/unit/in-OS tiers plus Doom and RimWorld after every C1 merge; dormant carve-out fallback (AD-3) |
| R2 | Venus is too slow, or lacks a feature vkd3d-proton needs | P0 measures both *before* P6; native context is the pre-agreed alternative (§1.4) |
| R3 | Wine's syscall surface is much larger than RimWorld's | P0 histogram drives the order; `-ENOSYS` for optional calls where Wine has fallbacks; one probe per new row |
| R4 | Memory: a 16+ GiB guest and a host-visible heap stress the frame allocator | Keep the RAM-only pool with the PCI-hole tripwire (`docs/rimworld-plan.md` status 2026-11-07); P0 records peak RSS to size the guest |
| R5 | 70 GB over UDP NFS gives very long load times | Measure in P8; local caching of the hottest archives on ext2; TCP NFS only if measured necessary |
| R6 | Shader/pipeline compilation stalls dominate first runs under Venus | Persistent caches on ext2 (AD-7); report first-run and warm-run times separately |
| R7 | `MAX_PROCESSES 192` or the fd table is exceeded by Wine + the engine's workers | P0 records peak threads/fds; raise with the RimWorld L12 method |
| R8 | A game patch changes requirements mid-plan | Pin the game version in P0; re-run P0 on any update before trusting older evidence |
| R9 | Scope creep into a general Windows-compatibility project | Non-goals (§1.6) are binding; every new Wine feature must be traced to a P0 or P8 failure |
| R10 | The XSAVE signal frame is subtly wrong and Wine corrupts register state on exceptions | F2 host golden against Linux's documented layout; the P1 ymm-edit-in-handler probe |
| R11 | *(v1)* Measurement contamination: setup work outside the measurement lock (VM boots, builds) overlapped 6 of 10 host benchmark runs | Every process that loads the GPU or many cores takes the lock, not only "measured" runs; contaminated runs stay in the evidence, marked |
| R12 | *(v1)* Writes into the user's Steam library: vkd3d-proton and `steamclient.so` wrote caches into the game folder / Steam shadercache during early P0 runs (no game, prefix or save data changed; audited) | All game launches run in a namespace where the Steam library is read-only; cache paths are set explicitly (AD-7) |
| R13 | *(v1)* The x64 T3 wave wedges on base and `join` fails on base, so "all tiers green" is not available as an acceptance signal | Gates use the probe set + unit tiers with base comparison; a hygiene lane fixes the wedge before P8 |

---

## 10. Open questions (close in P0/P1)

*v1 answers:* **Q1** — DYNAMIC_BASE (0x8160), loaded at its preferred
0x140000000; outside the hole. **Q2** — preloader ranges in §0.2 item 4; with
our sysroot the preloader is static-PIE (fixed form at `0x7d7d00000000` only
if `-static-pie` fails); nothing intersects `[0x70000000, 0x78000000)`; Wine
tolerates refused low mappings only with EEXIST (§0.2 item 8). **Q3** —
`arch_prctl(ARCH_SET_GS, teb)` once per thread, return unchecked; no
`wrgsbase`; `rdgsbase` only if CPUID.7:EBX[0] **and** `AT_HWCAP2 & 2`.
**Q4** — no required feature missing; 59/231 extensions not forwarded
(§0.2). **Q5** — archives are read, never mapped; largest game-file mapping
3.0 MiB. **Q6** — menu reached with AVX masked and with no audio device.
**Q7** — `docs/cp-p0-guest.md` §2 (memfd-shared RAM, `virtio-gpu-gl-pci,
blob=true,hostmem=16G,venus=true`, egl-headless on renderD129, the dma-buf
shim). **Q8, Q9** — open (P4/P7).

1. Is `Cyberpunk2077.exe` relocatable (`DYNAMIC_BASE`), or must it load at its
   preferred base? (If fixed, the compat AS must also free that range — it is
   inside the 0–8 GiB identity region either way.)
2. Exactly which low ranges does the pinned Wine's preloader reserve, and is
   the preloader itself a fixed-address executable?
3. Does the pinned Wine set the TEB through `ARCH_SET_GS`, `wrgsbase`
   (FSGSBASE), or both, and what does it check in `AT_HWCAP2`?
4. Which vkd3d-proton-required Vulkan features does Venus on virglrenderer
   1.10.0 + host RADV expose, and which does it not?
5. Does the game `mmap` its archives, and what is the largest single mapping?
6. Does the game reach the menu with AVX masked off? With no audio device?
7. What do QEMU 10.2.1 and virglrenderer 1.10.0 need, exactly, to expose Venus
   (device properties, shared memory backend, build flags)?
8. Does the game's fullscreen mode cooperate with a scanout-only present, or
   does it require a borderless window composited by X?
9. How much of the X server side (XInput2, RandR, MIT-SHM) does `XHOBBY.BIN`
   already implement?

---

## Appendix A — Frozen interface drafts

**F1 (compat AS, x86_64, draft)** — user VA from `0x10000` to
`0x7fffffffffff` for compat processes; the kernel direct map in the higher half
(base chosen in `docs/cp-address-space.md`); native processes unchanged at
`USER_VA_BASE`. `KUSER_SHARED_DATA` is an ordinary user page that Wine maps —
the kernel only has to *allow* the address.

**F3 (`/dev/gpu` M2, draft request list)** — names only, numbering follows
`docs/gpu-abi.md` §2's `_IOC` scheme:

| request | purpose |
|---|---|
| `GPU_IOC_CTX_CREATE2` | context with `capset_id` and ring count (context-init) |
| `GPU_IOC_RES_CREATE_BLOB` | blob resource: memory type, flags, size, blob id |
| `GPU_IOC_RES_MAP_BLOB` | map a mappable blob into the caller's AS through the host-visible region; returns VA |
| `GPU_IOC_RES_UNMAP_BLOB` | unmap |
| `GPU_IOC_SUBMIT2` | submit with a ring index; returns a per-ring fence |
| `GPU_IOC_FENCE_WAIT2` | wait on (ring, fence) with a timeout |
| `GPU_IOC_SCANOUT_BLOB` | display a blob (AD-6) |

M1 requests stay valid and unchanged, so RimWorld's virgl path is unaffected.

---

## Appendix B — P0 command sketches (**verify** each at P0; none has been run)

```bash
# host-native run, all Wine processes traced
strace -f -c -o cp-syscalls.txt wine Cyberpunk2077.exe --launcher-skip

# Vulkan features vkd3d-proton enables (log level per vkd3d-proton docs)
VKD3D_DEBUG=info wine Cyberpunk2077.exe --launcher-skip 2> cp-vkd3d.log

# fps rows
MANGOHUD=1 MANGOHUD_CONFIG=autostart_log=1,log_duration=120 \
  wine Cyberpunk2077.exe --launcher-skip

# VA layout of the running game
cat /proc/$(pgrep -f Cyberpunk2077.exe)/maps > cp-maps.txt

# image base / relocatability
objdump -p Cyberpunk2077.exe | grep -E 'ImageBase|DllCharacteristics'

# Linux guest with Venus (device properties to confirm against QEMU 10.2.1)
qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 24G \
  -object memory-backend-memfd,id=mem1,size=24G,share=on \
  -machine q35,memory-backend=mem1 \
  -device virtio-gpu-gl-pci,blob=true,hostmem=8G,venus=true \
  -display egl-headless,gl=on ...
```

---

## 11. Fix log (append-only)

- 2026-11-14 — v0 created. Track selected (upstream Wine + vkd3d-proton in the
  existing compat class; Venus for the GPU, lavapipe for bring-up, native
  context as the performance alternative). Grounding grepped from the tree:
  FXSAVE-only FPU, 64 GiB `USER_VA_BASE` with a 0–8 GiB identity map,
  `ARCH_SET_GS` unhandled, 64 MiB per-object mapping cap, `/dev/gpu` M1
  without blobs/fences, FAT16 + read-only NFS, null audio. Nothing measured
  about the game yet; P0 is the first executable step.
- 2026-11-15 — **v1: P0 closed (GO for Venus), wave 1 merged, wave 2 defined.**
  Merged into `cyberpunk`: C1-as (`docs/cp-address-space.md`), C2-wine (pins,
  Wine 11.0 build, `docs/cyberpunk-evidence.md`), C7-p0guest
  (`docs/cp-p0-guest.md`; menu 143–144 fps median in a Linux guest on Venus),
  C3-ext2 (`/EXT`), C7-p0host (`docs/cp-p0-host.md`). C1-cpu held on a
  tramp_sig T2 regression. Corrections folded in: no swapgs existed; kernel
  image at 0x70000000 stays (K0); 5.25 GiB frame-pool cap; measured syscall
  gaps replace v0's guess; AVX and audio not needed for the menu; Steam build
  instead of GOG; virglrenderer 1.2.0 + dma-buf shim; server-side sync
  guaranteed and costs ~23 %; `MAP_FIXED_NOREPLACE` must return EEXIST;
  tmpfs `/tmp` moves to P2. Pre-existing defects recorded (§0.2 item 17).
  The user accepted the CD PROJEKT RED user agreement for the test VM's
  profile only (via the coordinator) and asked to skip the guest benchmark.
- 2026-11-15 — **v2: wave 2 merged; gate host moved to strix.** linuxci's root
  filesystem went read-only with on-disk corruption at 08:23; every wave-2
  result was re-produced on strix (Zen 5, AVX-512) and linuxci's discarded.
  Merged into `cyberpunk`: C1-cpu (XSAVE XCR0=7/832 B even on AVX-512 hosts,
  GS base, SFMASK/cld, `-cpu host`, SIG_DFL ignore of CHLD/CONT/URG/WINCH at
  send and delivery, the tramp_sig T2 idle-CPU kick), C2-sys (tmpfs `/tmp`,
  memfd, eventfd, SCM_RIGHTS on SOCK_STREAM, fchdir/sendmmsg/xattrs/inotify/
  SUD/`process_vm_*`, fork-shaped clone, 1 GiB VM objects, FAT `open(dir)`;
  `wineserver`/`wine --version` run in the guest), C3-ext2b (ext2 mmap incl.
  `MAP_SHARED` write-back, exec of ext2 images and PT_INTERP via the spawn
  loader, setattr, a generic POSIX/OFD/flock lock manager shared with FAT16 and
  tmpfs, `VFS_PATH_MAX` 1024), C1-mmu (direct map + ioremap, identity map
  dropped from native roots with an `[IDMAP-TRIP]` tripwire, multiboot memory
  map with frames above 8 GiB, compat AS from `0x10000`, castscan gate, fork
  keeps the Linux ABI). Merged tree: 17/17 probes, `ext2_gate` PASS, unit
  450/2 and host 38/40 (the known reds: `virtio_gpu_test.c` ×2 — pre-existing,
  in the user's `gpu_abi.c` area — and the untracked `gaming.md` ×2); join's
  row E (`process_clone` EINVAL on thread flags) and row A race are base
  defects. New open items → wave 3 (§7.2c): compat `execve`, thread clone,
  `MAP_PRIVATE` of tmpfs/memfd, glibc static-pie IRELATIVE, `cwd[128]`, the
  64 MiB per-file ext2 mapping cap, SIGBUS past EOF, ~100 ms parked-signal
  latency.
