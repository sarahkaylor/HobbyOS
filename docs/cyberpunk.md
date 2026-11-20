# Cyberpunk 2077 on HobbyOS — the Proton track

Status: **v7 — Gates P2–P8 passed; P9's functional half (play 10 min, save, reload) passes on Venus, its 30 fps target does not yet; wave 6 merged (integ11), wave 7 executing (§7.2g).**
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
    - **Wave 4 findings (2026-11-16; Gates P3, P4 and P5 passed).**
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
      - *Wine trap, C5-vk2 (0b7da2ba):* once winex11 works, `wineboot -i` runs
        appwiz's `install_mono` and blocks forever on the Wine Mono Installer
        dialog. Every in-guest Wine launch in an X session needs
        `mscoree,mshtml=` in `WINEDLLOVERRIDES` (C4-x2's P4 env already had it).
      - *Harness trap:* XSESSION relays child output with an `[X] ` prefix;
        graders that read probe rows from the serial log must strip it.
      - *Open:* C4-x2's one-screen `XHOBBY.BIN` (sha256 `935c760f…4c97`) is
        still a scratch override in the P4/P5 runs; `third_party/x11` must be
        rebuilt with its DDX so trees produce it themselves.

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
*Amended in waves 7–8:* virtio-sound + ALSA exists as an opt-in (`docs/cp-audio.md`). QEMU's virtio-sound returns a
TX buffer once its mixer has copied it, not once it has played, and after the device was starved its rate control
drains at up to 2× real time. Under QEMU, hw_ptr is therefore not a play position, and the driver's underrun rule uses
a real-time play clock (`docs/cp-audio2.md` §1). Exact zeros in the guest WAV come from winealsa's silence lead-in,
not from the device.

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
- [x] *(C5-vk2: Wine's `vulkan-1.dll` → winevulkan → `libvulkan.so.1` → lavapipe, run 7)* Vulkan loader + ICD JSON staging; winevulkan.
- [x] *(C5-vk2: Mesa's software `put_image` path; Present 1.2 fake-msc paces it at ~30 Hz, render median 1 ms)* A Vulkan X11 WSI path that works with lavapipe (software presentation
      is acceptable here — this rung is about correctness).
- [x] Probes: Linux `vkcube` *(done, C5-vk2: 65 s in the guest X session, 1853 frames, two distinct screendumps, `docs/cp-vk2-evidence/item2-vkcubex-x11.txt`)*; `vkcube.exe` under Wine; a DXVK D3D11 triangle;
      a vkd3d-proton D3D12 triangle (MinGW-built, in `src/user/cp/`) *(done, C5-vk2 run 7:
      `vkcube.exe`, `d3d11tri.exe` on DXVK 3.1.1, `d3d12tri.exe` on vkd3d-proton 3.1)*.

**Gate P5 — PASSED (2026-11-16, C5-vk2 run 7 on cyberpunk 9991149a, `docs/cp-vk2-evidence/winep5-run7-PASS.txt`; merged at 0b7da2ba):**
`vkcube.exe`, `d3d11tri.exe` (DXVK 3.1.1) and `d3d12tri.exe` (vkd3d-proton 3.1) each ran 65 s on
`llvmpipe (LLVM 21.1.8, 256 bits)` in the guest IceWM session, with two distinct screendumps each;
NFS clean (0 no-reply rows, 0 retries, 0 give-ups), no fault rows. Row 1, Linux `vkcube` on X11,
passed earlier (65 s). The Wine fps (~1000 vkcube, ~480 D3D) are unpaced, not lavapipe speeds:
winex11's Vulkan path presents through its own surface, which XHOBBY's fake-msc Present does not pace.
Original wording: all four render recognisably in screendumps and run ≥60 s without
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

- [x] WSI path that presents a swapchain blob with `SET_SCANOUT_BLOB` (AD-6)
      and hands the screen back to X on exit or focus loss. *(C6-p7, `docs/cp-p7.md`)*
- [x] Present-cost instrument comparable to RimWorld's `[HBT]` rows
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

- [x] Load a save made on the host; play 10 minutes; save; reload. *(wave 6 on Venus; 2/2 on integ17 with
  `tools/cp/hunt/reload2.p9`, §7.2h)*
- [ ] fps vs both baselines at 1280×720 Low and 1920×1080 Low. *(integ17, Venus: 720p median of medians 30.3, p10
  25.5–27.6; 1080p 34.5; the 28-minute soak median 37.6 with no leak. The target is met at the median, not with margin.)*
- [ ] Native context lane if P0 or P9 numbers call for it (§1.4).
- [x] virtio-sound + ALSA (AD-9). *(opt-in `CP_AUDIO_WAV`; C4-audio, H5-audio2)*
  - [ ] audio without dropouts: the kernel xrun cascades are fixed (H1-01 part A). The remaining 3–6 zero runs per
    fp720 boot happen while the game's audio thread is absent for 31–293 ms (H5-05). The `[AWAY]` kernel rows show
    it busy in the game's own code, or its peers parked on one game lock; NFS is not involved. No Wine-side gate
    helps (H5-04). This is game-side, not ours. Measure with `[SND] zeros up_ms=`, the `[AWAY-Z]` rows and
    `tools/cp/audio2/gamecheck.sh`.

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

### 7.2e Wave 5 (v5, 2026-11-16) — main in, Venus, the P8 groundwork and a first game boot

Wave 5 starts from `cyberpunk` @ edd8ebbf (Gates P2–P5 passed). The gate host is
still `strix`, with the §7.4 rules: QEMU slots (up to four guests), `exports.sh`,
`MAKEFLAGS=-j16`, and the NFS READ-size guards. The parity reference is
`strix:~/cp/base5` = edd8ebbf, with results in `~/cp/base5-gates.txt`.

The user decided three things on 2026-11-16:
- **P6 starts now.** The user's uncommitted `gpu_abi.c` work in the main checkout
  (RimWorld TRANSFER fixes and debug output) stays untouched. C6 builds on
  `cyberpunk`'s copy of the file, after `main` is merged in. Any conflict is
  resolved when the user commits that work.
- **The game is copied once to `strix:/srv/nfs/cp-game`.** This is a read-only
  copy of the Steam install (63 GB, file modes `a-w`). Only the C8 harness
  exports it, with the approved localhost read-only form through `exports.sh`.
  Nothing writes to `/mnt/fast`, and nothing reads it again. Game files never go
  in a worktree or a commit, and `tools/cp/check-no-game-files.sh` runs before
  every C8 commit.
- **A first in-guest game boot on lavapipe is allowed this wave.** It will be
  slow. Its job is to find P8's unplanned work early, not to measure fps.

The first-run profile comes from P0's host prefix: `user.gls`, `EnabledMods.json`,
`HDRUserSettings.json` and `UserSettings.json`. It is staged read-only at
`strix:~/cp/game-profile/`. The first-run screens are never automated (§0.2
item 1).

Two facts shape this wave:
- **`main` is 1,449 commits ahead** (the OpenJDK 27 and LibreOffice ports on
  the same x86_64 Linux-ABI lane, plus the scratch prune). 15 files changed on
  both sides, among them `linux_compat_{fd,mem,misc,proc}.c`, `trap.c`,
  `process.c`, `vm.h`, `fs.c`, `fat16.c`, `net.c` and the `Makefile`. The
  longer the merge waits, the worse it gets. C0-sync merges `main` first.
  Every other lane merges `cyberpunk` once C0-sync lands, before its final gate.
- **P8's known ceilings are in the kernel, not in Wine.** Today's limits are
  256 fds per process, 512 open files system-wide and 256 poll fds. Wine uses
  3 pipes per thread, and the game runs 173–190 threads (§0.2 item 13). The
  prefix is on ext2 at ~125 KB/s for 4 KiB writes.

| Lane | Scope (milestone) | Owns | Must not touch |
|---|---|---|---|
| **C0-sync** | Merge `main` @ b1f3ef56 into `cyberpunk` (one merge commit plus fix-ups). On conflicts, keep both sides' behaviour. Keep the cp/nfs side of `nfs.c`/`net.c` (§7.4). Main's new x86_64 ABI syscalls and cyberpunk's compat fixes must both survive. Gate: the full batch at parity with base5; Gate P2, `pfx_gate`, Gate P4 and Gate P5 reruns; main's own runnable x86_64 regressions (the OpenJDK hello/UI gates, the Doom ELF), with no banned targets | the merge and its conflict fix-ups, `Makefile` merge hunks | new features; `/etc` |
| **C1-k5** | P8 scale: `MAX_OPEN_FDS` ≥ 4096 per process with a real `RLIMIT_NOFILE` (get/set/prlimit), the system-wide file table ≥ 16384 (allocated, not static, if it grows the kernel image), poll/ppoll/select/epoll limits to match, eventfd/timerfd counts, cheap `getrusage(RUSAGE_THREAD)`, and `MAX_PROCESSES` headroom checked against 256 Wine threads. Probes: a Linux probe that opens 3000 pipes and polls 2000 fds; a Windows probe (`src/user/cp/thrstorm*`) that starts 256 threads, each waiting on events and a pipe, for 60 s under Wine. Both fail on base. Gate: both probes, plus the full batch at parity | the fd table and limits in `fs.c`/`file_obj.h`/`linux_compat_fd.c`, `poll`/`epoll` limits, rusage in `linux_compat_proc.c`, the process-table sizing in `process.c`, the new probes | ext2/NFS internals, `vm.c` |
| **C3-ext2w** | P3/P8 storage speed: batched write-through for ext2 (dirty blocks coalesced and written by `fsync`/`syncfs`/close/a short timer, with ordering rules written down), multi-block NVMe read and write commands, and FAT `ftruncate`/`O_TRUNC` under a live mapping (§0.2 item 18). Gate: ≥ 20× the 4 KiB write rate (from ~125 KB/s; report MB/s for 4 KiB and 1 MiB writes); a `wine reg add` survives `wineserver -k` and a reboot; `e2fsck -fn` clean after a pull-the-plug test (`quit` over QMP right after `sync` returns); `pfx_gate`, `ext2_gate` and the full batch at parity | `src/kernel/ext2*.c`, the block cache, the NVMe driver's command path, `fat16.c` truncate, the file-object truncate path in `vm.c`, `tools/cp/pfx/`, `tools/cp/ext2*` | NFS, the fd table (C1-k5) |
| **C4-x3** | X for P7/P8: MIT-SHM in XHOBBY (attach, PutImage, GetImage, Pixmap if cheap) over the compat class's SysV shm or memfd. C4-x2's one-screen DDX built by the tree's own `third_party/x11` rule, so P4/P5 run with no `XHOBBY.BIN` override. RandR modes 1280×720 and 1920×1080 that Wine can switch to and back. Wine fullscreen at 1280×720 under IceWM. Gate: P4 and P5 rerun with the tree-built server; `xdpyinfo` lists MIT-SHM and an XShm probe round-trips; a Wine fullscreen screendump at 1280×720 | `src/user/x11/**`, `third_party/x11` build rules, `tools/cp/x11/`, `tools/cp/p4/`, `tools/cp/p5/` (harness only) | kernel (report SysV shm gaps to C1-k5 with a probe) |
| **C6-venus** | P6: the host recipe (`tools/cp/host-venus.sh`: QEMU 10.2.1 `virtio-gpu-gl` with `venus=on,blob=on,hostmem=`, `/usr/libexec/virgl_render_server`, memfd-backed guest RAM, rendering on the RX 9070 XT `renderD129`, headless, never the user's display). The kernel M2 transport (`CONTEXT_INIT`, `RESOURCE_BLOB`, the host-visible shm region mapped into user space with the right cache attributes, blob create/map/unmap, ring fences completed from the IRQ path). The `docs/gpu-abi.md` M2 amendment (F3). Mesa `vn_renderer_hobbyos.c` with `-Dvulkan-drivers=virtio`. Probes: Linux `vkcube` on Venus, then the P5 D3D12 triangle on Venus, and a fence-latency benchmark. Host recipe and Mesa backend first; the kernel after C0-sync lands. Gate P6 | `src/kernel/virtio_gpu_3d.c`, `src/kernel/gpu_abi.c`, `src/include/gpu_abi.h`, `docs/gpu-abi.md`, `tools/cp/venus/`, `tools/cp/host-venus.sh`, the Mesa Venus build under `tools/mesa/` | the user's main checkout; the GUI session, Steam, `/mnt/fast` |
| **C8-game** | P8 scouting on lavapipe: the launch recipe (`tools/cp/p8/`): the game over NFS from `/srv/nfs/cp-game`, the prefix on ext2 preseeded from `~/cp/game-profile`, `--launcher-skip`, the vkd3d-proton/DXVK overrides and `mscoree,mshtml=`, `VKD3D_SHADER_CACHE_PATH`/`MESA_SHADER_CACHE_DIR` on ext2, the game's directory read-only. Boot it, and push it as far as it goes. Record every stop in order (fault rows, the missing syscall/ioctl, the Wine/vkd3d log line) and fix it, or hand it over: fix in your own files; for files another lane owns, write a minimal reproducing probe plus a proposed diff and report it. Never automate the first-run screens. Gate: P8's main menu if it gets there; otherwise a ranked list of blockers, each with evidence, and the furthest screendump | `tools/cp/p8/`, `src/user/cp/p8*`, `docs/cp-p8.md` | game files in git; `/mnt/fast`; Steam |

**C0-sync landed (2026-11-17, cp-sync 7f79b1c6 = merge f752acca + 5 fix-ups; `docs/cp-sync.md`).** Full batch,
Gate P2, `pfx_gate`, Gate P4 and Gate P5 pass at parity with base5. Two of main's auto-merged changes
broke this tree with no conflict marker: munmap write-back through a physical address (kernel page fault on
the first `munmap`), and compat fsync routing that dropped main's FAT16 mapping flush. Both are fixed and
covered by probes. Resolutions: `VM_OBJ_MAX_BYTES` 4 GiB (main's JDK heap); cyberpunk's `vfs_lock`
manager for `F_SETLKW`; main's `file_read_user` without its bounce buffer (a 176 MiB NFS read runs at
404–448 MB/s instead of 282). LibreOffice is staged on `disk.img` only when the X prefix exists
(`LO_ON_DISK=` turns it off). Not run: OpenJDK `sdktest`/UI gates (no JDK 27 tree or host `javac` on
strix) and the Doom ELF (its build downloads Alpine packages).

**C8-game first boots (2026-11-17, branch `cp-game`, `docs/cp-p8.md`):** the game loads modules for
≤ 42 s, then dies before its first window. The wineserver fills its 256-entry fd table; HobbyOS `recvmsg`
then returns EMFILE with SCM_RIGHTS still queued (Linux delivers the data and sets MSG_CTRUNC); Wine kills
the client and the wineserver crashes in `grab_object(NULL)`. Handed to C1-k5: fd limits, SCM_RIGHTS
truncation, ENFILE, the native→compat env cap (512 B / 24 entries, silently truncated), `fadvise64`, and
optionally SIGKILL of a group whose leader has exited. Handed to C3-ext2w: an `ext2_lock` wedge when Wine
processes start concurrently on the prefix (1 in 6 boots). Open, unowned: one corrupted read (`msvcrt.dll`
importing an empty-named library) in 1 of 9 Wine boots, with a clean NFS log. On lavapipe, vkd3d-proton
offers FL 11_1 by default; `VKD3D_FEATURE_LEVEL=12_0` creates an FL 12_0 device.

**integ5 landed (2026-11-17, cyberpunk = efd3a765):** cp-ext2w + cp-x3 + cp-k5 + one integrator fix-up
(ARM `linker.ld` sorts `.bss` by alignment: two 2 MiB-aligned objects had jumped a 2 MiB boundary and pushed
`__bss_end` past the limine assert; now 0x454cb1f0, about 1.2 MB of headroom). The gate batch
(`strix:~/cp/integ-gates8.txt`) passed: every probe, ext2/exec/pfx/ext2w gates, both NFS guards, unit (2 known
reds), host 38/40, ARM build, Gate P2, Gate P4, Gate P5 (with `fullscr`), thrstorm 256 threads (S1 PASS) and
C8's P8 probes. Known, not blocking:
- exitgroup X7 is load-flaky on base too (base: 12–16 bad at load ≈ 14).
- `reg_gate` boot 1 wedged on `ext2_lock` (holder `ext2v_read` under pread64), C8's old stop 6; with C3-ext2w.
- One pull-the-plug run in 8 is not fsck-clean (leak-class only, synced data intact); ext2 has no journal.
- The ARM **unit-test** image is over the placement window since the main merge (pre-existing on 15ad4a35).
- The ARM image is build-checked only (its boot config is a 16 GiB guest).

**Wave 5 results so far.**
- **C3-ext2w:** ext2 write-back with an ordered commit, plus multi-block NVMe commands. 4 KiB writes 14 →
  540–680 MB/s; pull-the-plug 8/8 clean in the lane (base 5/8); FAT ftruncate under a mapping fixed. Open:
  C8 stops 8/9 (flusher exit, `nvme_wait_cid` spinning on a CPU) and the `ext2_lock` wedge (back with the lane).
- **C4-x3:** SysV shm (compat 29/30/31/67; native row **SYS_SHM 106**, signed off by the integrator), MIT-SHM 1.1
  in XHOBBY (every winex11 window draws through XShm), the tree builds its own one-screen server (the override was
  a stale build dir), RandR 1280×720 ↔ 1920×1080 with IceWM patch 0005. Open: MIT-SHM 1.2 / DRI3 (fd passing),
  and the real scanout follows the mode only in P7.
- **C1-k5:** `MAX_OPEN_FDS` 4096 with RLIMIT_NOFILE 1024/4096; 16384 files / 8192 pipes from the frame pool;
  poll/ppoll/select/epoll scale; Linux SCM_RIGHTS truncation (MSG_CTRUNC) for Linux-ABI receivers; ENFILE; env
  2048 B / 128 entries with E2BIG; fadvise64; O(1) getrusage. Open: SIGKILL of a group whose leader has exited
  (p8killprobe K1/K2), signalfd, `ru_stime`.
- **C8-game: Gate P8 PASSED on lavapipe** (integ5 + `cp-game` run10; on integ6 again in run13 at +297 s):
  Cyberpunk 2077's main menu at launch + 328 s, OCR ×3 (NEW GAME … QUIT GAME), the user's preseeded profile
  (no first-run screen shown), 7.4 GiB of the 11.25 GiB frame pool in use, e2fsck clean, game copy unchanged.
  On Venus (integ6), FL 12_1 / SM 6.7 native, first window at +46 s, intro at 44–68 fps, then **stop 11**: a
  deterministic Venus-only fault (game code executes in or writes to the exe's `.rdata`) 60–100 s in; with C6.
- **C6-venus: Gate P6 PASSED** (lane run 13): vulkaninfo names `Virtio-GPU Venus (AMD Radeon RX 9070 XT (RADV
  GFX1201))`; D3D12 176–190 fps, D3D11 118–123 fps, vkcube.exe 204 fps, Linux vkcube 226 fps; fence p50 162 µs;
  timeline waits p50 76 µs. Mesa's software-WSI never completes `VK_KHR_present_wait`, so it is hidden;
  synchronous present (async faults at frame 2, open). The host render server is fenced to renderD129 by an
  LD_PRELOAD deny shim (the 8060S runs the user's desktop). F3 frozen at `docs/gpu-abi.md` §10 (ABI v2).
- **C1-pe:** x86 COW and MAP_FIXED flushed only the local TLB (stale or unrelocated DLL bytes for threads on
  other CPUs); fixed with probe `linux_probe_pemap` (base 6/6 FAIL, lane 11/11 PASS). Wine A/B 0 vs 0 faults,
  so the PE-loader faults are not proven gone. Open: a single-threaded wineserver `c0000130` read; a proposed
  `frame_free` double-free tripwire.
- `/srv/nfs/cp-game` is shared between P8 runs only through `run-p8.sh`'s refcount (`/tmp/cp-game-export.lock`).
- **integ7 landed (2026-11-17):** integ6 (cp-pe + cp-venus) + cp-venus-p8 (stop 11 fix + C8's harness `cp-game`)
  + cp-ext2w round 2. Batch `strix:~/cp/integ-gates10.txt` plus reruns (`integ10-rerun.txt`): every probe,
  ext2/exec/pfx/reg gates, both NFS guards, unit (2 known reds), host 38/40, ARM (`__bss_end` 0x454ea218),
  Gate P2, P4, the Venus D3D rows of P6, thrstorm, the P8 probes, and **Cyberpunk at its main menu on lavapipe
  (OCR) and on Venus in 2 of 3 boots** (menu at launch + 161–177 s; the third boot's game exited by itself with
  status 0 after 20 s, no window). P5's d3d11/d3d12 rows ran and passed but were graded FAIL: **kernel rows from
  two CPUs interleave character by character on the serial console** and destroyed `[P5] end d3d11` (also the
  likely cause of integ6's missing P2 "hello"). Console integrity is wave 6's first item.
- **Stop 11 closed (C6-venus, b8cc46c8):** x86 `clock_gettime` answered every monotonic clock in whole
  milliseconds, so a Windows game's `QueryPerformanceCounter` (Wine maps it to CLOCK_MONOTONIC) ticked at 1 ms;
  Cyberpunk's start-up then used engine lists before they were filled on the fast GPU. Now microseconds from
  the TSC. Also fixed: stale fence-IRQ wakes, a blob double-map race. Open: v19-style title stall,
  `VKD3D_FRAME_RATE` ignored, `clock_nanosleep(TIMER_ABSTIME)` EINVAL, async present.
- **C3-ext2w round 2:** stop 8 was `kernel_exit` freeing a task's block while still on its stack (now finished on
  the scheduler stack; one long-lived `ext2flush`); stop 6/9 was a lock held across user-memory copies (bounce
  buffers; no `f->lock`/`pos_lock` on ext2 I/O). `EXT2_OPEN_MAX` 16384. Open: the NVMe wait still polls (a
  yield put two CPUs on one stack); pull-the-plug can still leave leak-class errors even at delay 0 (no journal).

Merge order: C0-sync first. Then, in whatever order they go green: C1-k5,
C3-ext2w, C4-x3 and C6-venus (C1-k5 and C3-ext2w both touch `vm.c`/`fs.c` in
different functions; the integrator resolves). C8-game's harness merges at the
end, with its blocker list. Every merge is re-gated on the merged tree with the
full batch, both NFS guards, `pfx_gate`, and one Gate P2 run. Any guest larger
than 12 GiB, any download (Mesa is already on strix; virglrenderer is
installed), and any `/etc` change other than `exports.sh` wait for the user's
approval through the integrator.

### 7.2f Wave 6 (v6, 2026-11-17) — playing the game: P9 on Venus, P7, and the infrastructure under them

Wave 6 starts from `cyberpunk` @ e36f70a9: Gates P2–P6 passed, and Cyberpunk 2077 reaches its main menu in
HobbyOS on lavapipe (launch + ~300 s) and on Venus with the RX 9070 XT (launch + ~160–180 s, 2 of 3 boots on
integ7). The parity reference is the integ7 batch (`strix:~/cp/integ-gates10.txt`, `integ10-rerun.txt`).
Gate host, slots, `exports.sh`, `-j16` and the NFS guards are unchanged (§7.4).

The user decided two things on 2026-11-17:
- **P9 uses the user's own saves.** The newest autosave (AutoSave-16, 2025-08-01) and manual save
  (ManualSave-86, 2025-08-08) were copied read-only from P0's host prefix into `strix:~/cp/game-profile/`.
  Save files never go into git.
- **P9 game boots may use up to 32 GiB of guest RAM.** At most one such boot runs at a time
  (`flock /tmp/cp-bigguest.lock`); every other boot stays at 12 GiB.

Three facts shape the wave:
- **The serial console is not line-atomic.** Kernel rows from two CPUs interleave character by character and
  destroy probe rows (integ7's P5 grade, integ6's P2 "hello"). Every gate grades from that console, so it is
  the first thing to fix.
- **Presentation is a CPU copy.** Venus frames reach the screen through Mesa's software WSI and X PutImage
  (synchronous present, `VK_KHR_present_wait` hidden). P7's zero-copy scanout is the path to real fps.
- **Timers are coarse.** `Sleep(1)` costs 8–22 ms in the guest (C6's p8caps rows). Windows games pace frames
  and worker threads with short sleeps and waits.

| Lane | Scope (milestone) | Owns | Must not touch |
|---|---|---|---|
| **C0-console** | Console integrity: every kernel row is written to serial whole (per-CPU line buffers flushed under one lock, or an equivalent), user console writes relayed whole, and crash/fault dumps still printed when locks are held (a bounded trylock and fallback). A verbosity knob (`CP_DIAG=` or a boot arg) moves the high-volume diagnostic rows (`[PUB]`, `[MR]`, `[RESUME-*]`, `[ALIVE-SET]`, `[BLKD]`, `[PECOW]`…) off by default, keeping every row a gate grades. Graders (`tools/cp/p2`, `p4`, `p5`, `p8`, `run-wine-p2.sh`) match rows anywhere in a line as a second line of defence, and P5/P6 get separate output dirs. Gate: a probe with 8 CPUs of kernel rows plus user-space writers for 60 s and 0 interleaved lines; P2/P4/P5/P6/P8 graded clean twice each; serial time per boot reported | the console/serial print path, the diagnostic-row call sites (gating only), `tools/cp/*` graders, `docs/cp-console.md` | behaviour changes outside printing |
| **C9-play** | P9 on Venus: load ManualSave-86 or AutoSave-16 from the staged profile, play 10 minutes with QMP keyboard input (walk, look, open the map; drive if the save allows), save, quit, reboot, reload. fps rows at 1280×720 Low (winevulkan `+fps` or DXVK/vkd3d HUD counters), frame-pool and RSS use, and every stop in order with an owner. Guest up to `-m 32G` under the big-guest lock. Also chase P8's open game-side stops on Venus: the clean early exit (1 of 3 integ7 boots), the title/loading stall (C6 v19) and the game's own exit 3 / int3 (C3-ext2w boots) | `tools/cp/p9/`, `tools/cp/p8/` (shared with nobody now), `src/user/cp/p9*`, `docs/cp-p9.md` | kernel files (report with a probe) |
| **C6-p7** | P7: zero-copy presentation for Venus. A swapchain blob shown with `SET_SCANOUT_BLOB` (AD-6) when a window is fullscreen, with hand-back to X on exit or focus loss (F5, with the X server); then bring back `VK_KHR_present_wait` and async present (root-cause the async-present `pthread_mutex_lock(0x9)` fault first). A present-cost instrument (like RimWorld's `[HBT]`). Gate P7: `vkcube.exe` fullscreen at 1920×1080 at the host's refresh rate, or a measured reason why not; per-present cost; then the P8 menu on Venus with the new path, and its menu fps | `src/kernel/virtio_gpu*`, `gpu_abi*`, `docs/gpu-abi.md`, `tools/cp/venus/`, `tools/mesa/venus/`, the WSI patches, the X server's scanout hand-back (`src/user/x11/**`, `third_party/x11` server side) | timers, signals, console |
| **C1-k6** | Kernel debt under the game: high-resolution timeouts (nanosleep, futex, poll/epoll and `clock_nanosleep` deadlines below the tick, e.g. a one-shot TSC-deadline/APIC timer), so `Sleep(1)` costs ~1 ms; `clock_nanosleep(TIMER_ABSTIME)`; SIGKILL of a group whose leader thread has exited (p8killprobe K1/K2); `signalfd`; `ru_stime`; the remaining unlocked fd-install scans (eventfd, timerfd, epoll, memfd, `file_connect`); a bounded `[FRAME-DFREE]` tripwire for double `frame_free` (C1-pe's open item). Each with a probe that fails on base | timer/tick code, the scheduler's sleep/wake paths, signal delivery in `process.c`/`trap.c`, `linux_compat_*` for these calls, `frame.c` (tripwire only) | the console (C0-console), GPU files |

Merge order: C0-console first (every later grade depends on it). Then C1-k6, C6-p7 and C9-play's harness, in
whatever order they go green. Each merge is re-gated on the merged tree with the full batch, P2, P4, P5,
the Venus P6 rows and one P8 boot on each GPU.

#### Wave 6 results (integ11 = 13dc693b, landed on `cyberpunk` 2026-11-17)

integ11 is `cyberpunk` c0dbe5a2 (C0-console) plus integ9 (C1-k6 rounds 1–2, C6-p7, cp-fork, cp-play round 1), integ10
(cp-play round 2, cp-ext2w3, cp-p7b docs) and integ11 (cp-k6 9d47b051/ea464119, cp-fat, cp-present), then cp-p7c's
docs. Batch `strix:~/cp/integ-gates14.txt` (logs `~/cp/integ14/`): every probe passes (57 rows, including the new
kstack, wfibound, fatren, forkcow, forkva, hrtimer, killgrp, sigfd, rusage, fdrace, ipclat, epserver); ext2_gate,
ext2w_gate (green for the first time), reg_gate, exec_gate, pfx_gate, NFS guards, castscan, ARM build; P2, P4, P5 ×2,
P6 Venus, P7 fullscreen, thrstorm, P8 lavapipe ×2 (menu at 262 s and 299 s) and P8 Venus (menu at 200 s). Known reds:
unit `virtio_gpu_test.c:214/:244`, host 38/40. P5 run #2's one fault line is pre-existing (below).

- **Console (C0-console, `docs/cp-console.md`).** Kernel rows are written whole; diagnostic rows sit behind a knob;
  graders match rows anywhere in a line. integ10's console probe: 27 927 kernel + 12 807 user rows, 0 torn.
- **Timers, signals, fds (C1-k6, `docs/cp-k6.md`).** One-shot high-resolution timeouts (p50 error 19–37 µs, was
  9–10 ms), `clock_nanosleep(TIMER_ABSTIME)`, SIGKILL to a leader-exited group, `signalfd`, `ru_stime`, locked fd
  installs, the `[FRAME-DFREE]` tripwire. Round 2 cut READY-but-unscheduled threads from 1.0–1.8 to 0.24.
  **kstack race:** a preempted task was marked READY while its CPU was still on its stack; fixed with
  `cpu_locals.kstack_pin` (probe `linux_probe_kstack`). **Lost wakeup (stop P9-8):** an AP's only tick is CPU 0's
  broadcast, which stops while CPU 0 spins with IF=0; `safe_wfi` now arms a ≤1 ms one-shot first (9d47b051, probe
  `linux_probe_wfibound`), and `blk_token` release wakes user waiters (cp-fat f1cb959e).
- **Zero-copy present (C6-p7, Gate P7 PASS, `docs/cp-p7.md`).** `SET_SCANOUT_BLOB` flips for fullscreen
  swapchains with X hand-back; `VK_KHR_present_wait` and Venus async present are back (the async-present fault was
  root-caused). vkcube.exe fullscreen 1920×1080 93 fps (put_image 37); there is no host refresh rate to reach
  (monitor off, egl-headless), and each flip costs 6.1 ms of QEMU readback.
- **Fork (cp-fork, `docs/cp-fork.md`).** `vm_as_clone_into` walks only what is mapped: the crash reporter's
  CreateProcess drops from 16.5–17.6 s to 0.1 s and stop P9-1 (the game's own early exit) is gone (0 in 11 boots).
- **FAT (cp-fat).** Rename fixes for fontconfig's cache (`linux_probe_fatren`, unit
  `test_fat16_rename_fontconfig_cache`), `[BLKTOK]`/`[FAT-LEAK]` rows, and the P9-8 root cause above.
- **ext2 write grading (cp-ext2w3, `docs/cp-ext2w.md`).** reg_gate grades WEDGE / stack smash / QUARANTINE;
  HMP register dumps when the serial goes silent.
- **P9 (C9-play, `docs/cp-p9.md`).** Load ManualSave-86, 10.5 min of scripted play, F5, quit, reboot, reload to
  the identical position: **pass** on Venus. The game's 1280×720 window now fills a 1280×720 X screen
  (`HB_SCREEN_SIZE`) and every frame is a blob flip. **World fps 19–33 median (p10 12–21) across integ9–integ10
  boots; target 30 not reliably met.** Menus 90–110 fps.
- **Where the frame goes (C6-present, `docs/cp-present.md`).** The frame is CPU/kernel-bound in the guest, not
  present- or host-bound: the host GPU is 23–30 % busy; two game threads run 60–90 % of a vCPU with 11–25 % of
  their time in the kernel; wineserver uses 40–56 % of a vCPU, ~95 % of it in the kernel. Venus ioctls are now
  11–12 µs (posted unfenced SUBMIT2, `ioeventfd=on`), fps-neutral within the ±5 fps boot-to-boot spread.
- **P5 run #2's fault line (C6-p7c, `docs/cp-p7c-evidence/p5-services-quit-handler-fault.txt`).**
  `services.exe` NULL+0x3A8 in Wine's unix-side `quit_handler`: wineserver SIGQUITs a thread that is already in
  glibc's exit path (TEB key destroyed). Wine's own race; ~0.7 % of boots since 2026-11-16, also on trees without
  wave-6 kernel code. Fix in Wine (wave 7 crash hunt).
- **Open items carried into wave 7:** the 30 fps world target and the 1920×1080 Low column; Wine sync through
  wineserver; the P8 lavapipe early exit (1 of 3 on integ10, 0 of 2 on integ11); Wine's `quit_handler` race;
  `make host_tests` leaves `EDITOR.BIN_host` processes running; audio (AD-9); the §8.3 regression rows.

### 7.2g Wave 7 (v7, 2026-11-17) — the 30 fps target, audio, regressions, then a crash hunt

Wave 7 starts from integ11 (13dc693b). Parity reference: the integ11 batch above. On 2026-11-17 the user approved
the alsa-lib 1.2.14 download (sha256 `be9c88a0…7a32`, signature verified against the ALSA release key
F04DF50737AC1A884C4B3D718380596DA6E59C91, at `strix:~/cp/lanes/audio-dl/`) and the deletion of the merged wave 1–5
lane trees on strix (the disk was at 97 %). Once the main plan closes, the user asked for a broad testing wave to
find and fix every crash and glitch.

| Lane | Scope | Owns | Must not touch |
|---|---|---|---|
| **C1-k7** | Per-call syscall/trap entry and exit cost vs Linux on strix; why wineserver spends ~95 % of its CPU in the kernel; optionally per-CPU run queues. fpsprobe A/B, ≥3 boots per arm | x86_64 syscall/trap entry, scheduler, `process.c` | ntsync, GPU files |
| **C2-ntsync** | `/dev/ntsync` in the compat layer (probe vs Linux's selftest expectations), Wine 11 rebuilt with ntsync into `~/cp/lanes/ntsync-wine`, fps A/B | futex/sync paths, new ntsync files, its Wine build | entry/scheduler (C1-k7) |
| **C4-audio** | AD-9: virtio-sound driver, ALSA kernel ABI subset (`/dev/snd/*`), alsa-lib 1.2.14 on the 2.31 sysroot, Wine with `winealsa.drv`, a WASAPI tone test, the menu's music into a host WAV (never a host audio device), fps cost of audio | new `virtio_snd*`/`linux_snd*`, `tools/cp/audio/`, opt-in hooks in the harnesses and `build-wine.sh` | entry/scheduler, sync, MMU, NFS |
| **C10-regress** | §8.3's regression rows: Doom ELF, RimWorld (menu and main's colony battery), the IceWM desktop GUI tests, ARM64 boot and unit image, host tests; bisect and fix what Cyberpunk-era commits broke | fixes in files no running lane owns, `docs/cp-regress*` | running lanes' files |

Merge order: whichever goes green first; C1-k7 and C2-ntsync are re-measured together on the merged tree (fpsprobe ≥3
boots, plus one 1920×1080 Low run) before the 30 fps verdict. C2-ntsync's and C4-audio's Wine changes are combined
into one Wine build at integration.

#### Wave 7 results (integ15 = a23bfbeb, landed on `cyberpunk` 2026-11-18)

integ12 (C2-ntsync) landed first (39e4a288), then integ13 (cp-regress), integ14 (cp-audio plus build-wine
`CP_WINE_NTSYNC`) and integ15 (cp-k7). Batch `strix:~/cp/integ-gates16.txt` (logs `~/cp/integ16/`) passed:
- all 59 probes, including `snd`, `sysc` and `console` (0 torn);
- ext2/exec/pfx/reg gates, NFS guards, unit 463/0, host 39/41, castscan, ARM;
- P2, P4, P6 Venus, P7 fullscreen, thrstorm;
- P8 lavapipe ×2 (menu at 270 s and 280 s) and P8 Venus (menu at 129 s).

Its two reds were fixed in wave 8: `ext2w_gate` (a harness timer, below) and P5 run #1's `services.exe` NULL+0x3A8.

- **ntsync (C2-ntsync, `docs/cp-ntsync.md`).** `/dev/ntsync` in the compat layer; Wine is built with it. The combined
  default Wine (X11 + ntsync + winealsa) is `strix:~/cp/wine-cp`, built by `~/cp/build-wine-cp.sh`. P8 Venus
  reaches the menu in 124 s (was 200 s).
- **Kernel entry cost (C1-k7, `docs/cp-k7.md`).**
  - Changes: lock-free user copies, a futex wake that skips when there are no waiters, and one cache-line record per
    CPU on the trap path.
  - Invariants (binding): every futex slot `uaddr` write goes through `l_fx_key_set()`; a WAIT counts itself in
    `l_fx_cnt` before comparing; per-CPU trap/lock state is never a `[MAX_CPUS]` array.
  - The guest is about 60 % idle in the world. The remaining levers are the Venus fence/present path, host GPU
    sharing and wineserver.
  - `sysret` is not possible with the current GDT (user CS 0x1B sits below user DS 0x23).
- **Audio (C4-audio, `docs/cp-audio.md`).**
  - virtio-sound-pci with an ALSA subset (`/dev/snd/controlC0`, `pcmC0D0p`).
  - alsa-lib 1.2.14 in the guest, routed plug → mmap_emul → hw. Wine has `winealsa.drv`.
  - Opt-in per boot with `CP_AUDIO_WAV=/abs.wav`, which uses QEMU's `wav` backend only and never a host audio device.
  - `LSND_PARKED` is −100000, because −2 collides with −ENOENT.
- **Regressions (C10-regress, `docs/cp-regress-evidence/`).** The unit image's `virtio_gpu_test.c` rows (unit
  463/0), the ARM unit link, and a reaper for host-test spawns (atexit, `PR_SET_PDEATHSIG`).
- **fps:** 25–28 median of medians at 720p Low; not yet reliably ≥30. Wave 8 re-measured it (below).

### 7.2h Wave 8 (v8, 2026-11-18) — the crash and glitch hunt

Wave 8 hunts crashes, hangs, faults and glitches with many boots, k/n counts and a check seen failing before each fix.
It starts from integ15. Rules: `wave8-overrides` (lane ownership, at most 2 qemu slots per lane, audio via `wav`
only, no game content in git).

| Lane | Scope | Result |
|---|---|---|
| **H1-hunt** | discovery: 40 game boots over two rounds, the catalog `docs/cp-hunt.md` (H1-01..H1-18), P9 verdicts | 0 kernel faults, 0 early exits on integ15 and integ17; reload passes 2/2 |
| **H2-early** | the P8/P9 early exit after REDEngineErrorReporter starts (`docs/cp-early.md`) | kernel NFS race fixed; graders grade `early-exit` (rc 3) |
| **H3-winefix** | Wine-side crashes (`docs/cp-winefix.md`) | the `quit_handler` race fixed with two Wine patches |
| **H4-kglitch** | kernel glitches (`docs/cp-kglitch.md`) | console stall-proof; waitpid, rt_sigreturn and sentinel fixes; host_tests runs every step |
| **H5-audio2** | audio dropouts (`docs/cp-audio2.md`) | kernel xrun cascades fixed; the audible lead-ins are open |

#### Wave 8 results (integ19, landed on `cyberpunk` 2026-11-18)

integ16 = integ15 + cp-winefix + cp-hunt + the ext2w gate fix + cp-kglitch; integ17 = + cp-early; integ18 = + cp-audio2; integ19 = + cp-hunt2 and three P9/P8 harness fixes. Batch `strix:~/cp/integ-gates18.txt` (integ17, Wine `wine-cp2`):
- all 63 probes, ext2_gate and ext2w_gate, unit 463/0, host 40/42;
- P4, P5 ×3 (no `services.exe` fault), P6 Venus, P7, thrstorm;
- P8 lavapipe ×2 and Venus at the menu (229 s, 376 s and 139 s).

Its two reds, `exitgroup` #3 and P2 W6, were load flakes (below). The `integ-gates19.txt` delta on integ18 passed:
- all probes, unit, host, ARM;
- `exitgroup` ×4 paired with integ15;
- P2 ×3, P5 and P8 Venus.

integ19 adds only harness files and docs on top of integ18.

- **The early exit (H2-F1, 7 of ~150 boots → 0/10 after the fix).**
  - Cause: `nfs.c` kept one unlocked directory-listing cache for every lister. conhost, started for
    REDEngineErrorReporter, lists Wine's fonts directory (56 entries) while the game lists its config directories. The
    game got the font names (`[GDT] rel=/engine/config/base total=56`; the right count is 7), found no config and
    exited 0.
  - Fix: `nfs_dir_lock`, a ylock held across one fill; getdents64 holds it from the count to the last entry. Guard
    row G11 (6 listers) fails 6/6 before the fix and passes after.
- **Wine's shutdown race (W1, ~0.7 % of boots; P5's `services.exe` NULL+0x3A8).**
  - Cause: wineserver SIGQUITs threads that are already exiting, or not yet fully started.
  - Fix: `tools/cp/wine-patches/0001` (an exiting thread blocks SIGQUIT) and `0002` (`quit_handler` with no syscall
    frame).
  - Check: the quitrace repro failed 5/5 boots on stock Wine and 0/5 patched. P5 passed 3/3 on integ17.
  - **The default Wine stage is now `strix:~/cp/wine-cp2`** (`~/cp/build-wine-cp2.sh` = wine-cp +
    `CP_WINE_PATCHES`). Every Wine change goes in through `tools/cp/wine-patches/`, and each build records its patches
    in `share/wine/cp-wine-patches.txt`.
- **Kernel (H4-kglitch).**
  - The console survives host stalls: the takeover bound counts only time the waiter watched, and only the head of
    the queue may take over. With injected 350 ms stalls every second: base 0/4 clean, lane 3/3. This ends the
    "console torn under load" known red.
  - K13: a leader-exited group was reclaimed while it exited, so waitpid returned ECHILD (`exitrace` 5/5 → 0/7).
  - rt_sigreturn resumes into an unmapped RIP the way Linux does; this was Wine's "unexpected trap 128".
  - `GPU_ABI_PARKED` is now −100001.
  - Instrument rows (`[BADELR]`, `fence_wait2`, the watchdog's unsigned wrap) no longer fire falsely.
  - `[SND] xrun` rows are printed per xrun.
- **Audio (H1-01, H5-audio2).**
  - Fixed: the kernel's xrun cascades. QEMU returns a TX buffer once its mixer has copied it, not once it has played,
    and after a host stall it drains at up to 2× real time. The underrun rule now uses a real-time play clock. Kernel
    xruns per game boot: 534 → 2.
  - Open: the 5–6 audible exact-zero runs per fp720 boot. They are winealsa's silence lead-in when the game's padding
    falls under one period (H5-04 Wine, H5-05 game or event timing). Round 2 traced them to the game (below).
- **Harness.**
  - Game boots use `cache=unsafe` on their throwaway per-run drives: QEMU start → launcher median 61 s → 5.3 s.
  - `ext2w_gate`'s crash-run clock starts at the first console byte, so a slot wait no longer fails it.
  - `tools/cp/hunt/` holds the soak, classifier and comparison tools; `reload2.p9` is the working reload path.
  - The P9 seek works at 1080p (NEAREST scaling) and with tall focus frames.
- **P9 on integ17 (H1, Venus).**
  - fpsprobe 720p world medians 33.7 / 28.1 / 30.3 (median of medians 30.3, p10 25.5–27.6); 1080p 34.5.
  - 28-minute soak: median 37.6 (p10 29.7), 0 hitches, memory flat at the end.
  - Play → save → reload → world: 2/2.
  - The ≥30 fps target is met at the median, with a better low tail than integ15 (p10 20.6–26.4). It is not met with
    margin.
- **Known flakes under heavy host load (not regressions; paired A/B on 2026-11-18).**
  - `exitgroup` X7 can exceed its 1000 ms bound: integ17 and integ15 side by side gave 12–205 ms in all 8 runs.
  - winprobe W6 can see `GetTickCount64` lag a `Sleep(200)` when the VM is stalled. Wine's tick is written by
    wineserver, and the woken thread can read it first. P2 passed 3/3 on the rerun.
- **Open after wave 8:**
  - `wineserver -k` ignoring SIGINT for 10.5 s while parked in `epoll_wait` (H2-F2; H4's `[SIGINT-S]`/`[SIGINT-D]`
    rows are in place for the next repro);
  - the audible audio lead-ins (H5-04/05);
  - wineserver `grab_object(NULL)` after EMFILE (W2, no repro);
  - Wine's TEB leak after ~20 000 TerminateThread (W3);
  - errno and socket items K8–K11 and the ext2 flusher nap K12 (`docs/cp-kglitch.md`);
  - the menu focus after a sub-menu (H1-12, worked around in the harness).

#### Wave 8 round 2 (integ21)

integ20 = integ19 + cp-audio2 round 2 (tools and docs only); integ21 = + cp-kglitch2. Batch
`strix:~/cp/integ-gates21.txt`, all green:
- all 69 probes, `sigmask-gate` 8/8, ext2_gate, exec_gate, ext2w_gate, reg_gate, pfx, rsize_guard and rsize_scan;
- unit 463/0, host 40/42 (the known pair), castscan, ARM, `exitgroup` ×3;
- P2 (G3 `-k` clean), P4, P5 ×3, P6 Venus, P7, thrstorm, k5probes;
- P8 lavapipe ×2 and Venus at the menu (221 s, 206 s and 114 s).

- **`wineserver -k` ignored for 10.5 s (H2-F2): fixed.**
  - Cause: the compat signal mask, sigaltstack and SA_* flags live in per-pid-slot arrays, and only fork and clone set
    them. A process the kernel creates from scratch (spawn_ex, the wave and XSTART loaders) inherited the slot's last
    occupant. Wine clients block SIGINT in `server_block_set`, so from about the 4th launch p8launch's `wine` landed on
    a dirty slot. It forked the wineserver with SIGINT blocked.
  - Fix: process.c's fresh-PCB init calls `linux_compat_sig_slot_reset()`. In H2's relaunch loop, with both trees booted
    together: 9/12 slow `-k` on integ19, 0/12 on the lane. `tools/cp/kglitch/sigmask-gate.sh` fails 8/8 before the fix.
- **K10:** a blocking TCP connect now blocks instead of returning EINPROGRESS. Only AF_UNIX takes the non-blocking
  probe. The blocking path is `net_socket_connect`'s in-kernel `safe_wfi` wait, which holds its CPU for up to 10 s
  against a silent peer. Wine's winsock connects are non-blocking, so the game does not take it.
- **K11:** a blocking loopback accept parks (BLOCKED, 10 ms bound) and the connector wakes it: 341 ms → 0.4 ms of CPU
  per 500 ms wait.
- **K8/K9:** fs.c's generic −1 is EBADF on a closed fd (readv, writev, pread64, pwrite64 said EPERM) and EPERM for a
  sealed memfd/tmpfs write (write said EBADF).
- **H5-05: the audio dropouts are game-side.** The new `[AWAY]` rows (`linux_compat_away.c`) report what a periodic
  ntsync waiter does while away from its clock; `[AWAY-Z]` replays them at each `[SND] zeros` row. In 12 fp720 audio
  boots:
  - the audio clock's waiters (the game's main thread and one other game thread) were away 30–293 ms at every
    dropout, 66–70 % in user code and 16–25 % in fault handling;
  - the other periodic waiters were parked on one game lock (97–99 % of their absences), woken by a game thread that
    held it;
  - NFS was not involved: there were 0–1 `[NFSPIPE] stall` rows per boot against 4–5 dropouts each, and no sample was
    an NFS, blk_token or ylock wait.

  Demand-fault cost while the game streams is the only kernel share, and a possible future lever.
- **Still open:**
  - W2 (`grab_object(NULL)` after EMFILE, no repro);
  - W3 (TEB leak after ~20 000 TerminateThread);
  - K12 (the ext2 flusher nap);
  - H1-12 (menu focus, worked around in the harness);
  - H1-13 (NFS resend pauses of 150–260 ms, 14/18 boots; they cause no audio dropout);
  - fault-handling cost during streaming.

#### Wave 9 (integ22)

Two lanes run on integ21. H6-hunt3 is a wider discovery soak: 20+ cold boots into the world, hour-long soaks, load
and settings churn, clean exits, and every user save. H7-fault works on the kernel's share of hitches.
integ22 = integ21 + cp-fault. Batch `strix:~/cp/integ-gates22.txt`, all green:
- all 71 probes, `sigmask-gate`, the file-system and NFS gates;
- unit 464/0, host 40/42, castscan, ARM, `exitgroup` ×3;
- P2, P4, P5 ×3, P6, P7, thrstorm, k5probes;
- P8 lavapipe ×2 and Venus at the menu (252 s, 253 s and 129 s).

Those menu times ran while one of H6's game guests shared the host; integ21's ran with no other guest. The extra time
is in the CPU-rendered intro, and there were no NFS stalls. H7's own tree reached the Venus menu in 109 s against
integ21's 114 s.

- **Fault cost (H7-01/02/04, `docs/cp-fault.md`).**
  - Cause: 98 % of the ~1.9 M faults per fp720 boot are anonymous zero-fills, and each one zeroed its frame under the
    single global irqsave `vm_lock`. In a P9 guest that first store is usually the host's first touch of
    shmem-backed RAM (no THP), so every other faulting CPU spun behind a host page fault. The worst hold was 474 ms.
  - A forward-walking frame allocator kept touching never-backed RAM: 940 MiB were zero-filled in the world phase
    while the free pool shrank by only ~100 MiB.
  - Console rows printed under the lock held it for 4–15 ms each.
  - Fixes:
    - a per-CPU stash of 4 frames zeroed before the lock (724a793a);
    - frame reuse from a warm LIFO (2963d3da);
    - no console rows under `vm_lock`; diagnostics use `vm_row` (d419ec9d);
    - `vm_arch_reload_cr3` only flushes (4112438a). This was a latent unit-tier use-after-free that the stash
      exposed (H7-03).
  - fp720 A/B (5 base boots, 3 lane boots):
    - fault time 22.1 → 13.7 s per boot;
    - all-site `vm_lock` wait 10.3 → 1.6 s, and hold inside faults 18.4 → 0.9 s;
    - `[AWAY]` T share of the audio-clock threads 33 → 22 %;
    - fps and audio dropouts unchanged within the noise of a busy host.
- **NFS resend pauses (H1-13 / H7-05).**
  - A stall is either a lost reply, with one IP fragment never reaching reassembly, or a host stall that delays
    every reply. The host dropped nothing.
  - Every answered READ came back within 64 ms, and 99.96 % within 4 ms.
  - Fix (1164f98f): the first wait is now 50 ms instead of 150 ms, and the original reply is used if it arrives after
    the re-send. Each pause is now ~50 ms plus the resend.
  - cpnfsbench with every 5th reply dropped: read 2974 → 1374 ms, mmap 2501 → 910 ms.
  - The cause of the lost fragment is open. The suspect is the virtio-net RX re-queue order, now counted as
    `rxq_reuse=`.
- **New instruments:** `[PFK]/[PFH]/[PFL]/[PFI]/[PFT]/[PFX]` fault rows, `[NFSRTH]`/`[NFSRS]`, virtio-net TX full-ring and
  RX re-queue counters, probes `pfstat` and `pfpar`, and `tools/cp/fault/`. They print about 1 KB every 10 s.
- **Open from H7:**
  - H7-07: TLB shootdowns, 5.2 s per boot, up to 740 ms each;
  - H7-08: 14 µs per timer interrupt on user threads;
  - H7-10: mprotect/mmap/JIT unmap holding `vm_lock` for 3–5.5 ms;
  - the host first-touch cost. Hugepage- or prealloc-backed guest RAM would be a host setting, so it needs the
    user's approval.

#### Wave 9 round 2 (integ23)

integ23 = integ22 + cp-fault round 2 + cac5d3bd. Batch `strix:~/cp/integ-gates23.txt` was all green except thrstorm,
which caught a kernel crash (below). The fix was re-gated in `~/cp/integ-gates23b.txt`: the away/ntsync/snd/sigmask
probes, thrstorm ×4 with no faults, P8 Venus at the menu (155 s) and ARM. The full batch:
- all 78 probes, `sigmask-gate`, the file-system and NFS gates;
- unit 464/0, host 40/42, castscan, ARM, `exitgroup` ×3;
- P2, P4, P5 ×3, P6, P7, k5probes;
- P8 lavapipe ×2 and Venus at the menu (309 s, 259 s and 140 s, with H6 and H7 guests on the host).

- **Kernel #DE in `away_clock_set` (cac5d3bd).**
  - The `[AWAY]` clock table from integ21 is unlocked. When two CPUs set the same ntsync event at once, one could
    stamp `win_ms` 1 ms after the other read `now`. `now - win_ms + 1` then wrapped to 0 and the eviction-rate
    division faulted in kernel mode (vector 0, wineserver, thrstorm). That was 1 of 3 thrstorm runs since integ21.
  - Elapsed time is now clamped at 0, which also stops the 1 s window check from resetting on the wrap.
- **Lost NFS reply fragments (H7-05, f83a2dfe).**
  - virtio-net handed each RX buffer back to the device before copying it out, so a new frame could overwrite a
    fragment still waiting for reassembly. Once that delivered wrong bytes.
  - Every frame is now copied out per CPU first. Lost-fragment stalls fell from 2/5 boots to 0/6.
- **TLB shootdowns (H7-07).**
  - KVM PV TLB flush (MSR 0x4b564d03): a preempted target vCPU is flagged `KVM_VCPU_FLUSH_TLB` instead of being
    waited for.
  - A shootdown no longer waits behind another CPU's console write; owed flushes settle first.
  - A `vmzero` kernel task keeps a 64 MiB pool of pre-zeroed frames.
  - Probe `tlbstale` checks that no stale mapping survives.
- **Long `vm_lock` holds (H7-10):** mprotect works in chunks, and munmap/MAP_FIXED unhook whole 2 MiB page tables.
- **New probes:** `tlbstale`, `vmhold`, `shootwait`, `conswait`, `cpus`.
- fp720 A/B (5 base boots, 4 lane boots):
  - fault time 13.9 → 5.6 s per boot;
  - shootdown time 6.0 → 1.3 s;
  - audio dropouts 5.4 → 2.8, about the size of boot-to-boot noise;
  - hitches 0.6 → 0.2;
  - fps median unchanged.
- **Open:**
  - thrstorm storm rounds were 785–909 on integ23b against 1062–1250 on integ19–22. That batch ran at host load ~13
    with lane guests running. A paired A/B under equal load is pending (H7).
  - H7-14: the ext2 flusher holds one of the 8 vCPUs while the data disk is mounted (every P9 boot).
  - H7-08: the census runs inside the PIT interrupt (13–99 ms hitches).
  - H7-11/12: console cost; the NFS periodic rows are 41 % of console output.
  - CPU-time clocks: CLOCK_THREAD_CPUTIME_ID reads 0, and CLOCK_PROCESS_CPUTIME_ID over-reads (65 s over 2 s).
  - H6-07: a host stall of ~5 s ends Venus `submit2` with DEVICE_LOST (1 of 20 cold boots).
  - The host first-touch cost (hugepages need the user's approval).

#### Wave 9 H6-hunt3 (integ24)

integ24 = integ23 + cp-hunt3 + 4276c18c. Batch `strix:~/cp/integ-gates24.txt`, all green:
- all 79 probes, `sigmask-gate`, the file-system and NFS gates;
- unit 464/0, host 40/42, castscan, ARM;
- P2, P4, P5 ×3, P6, P7, k5probes;
- thrstorm (1013 rounds);
- P8 lavapipe ×2 and Venus at the menu (275 s, 213 s and 154 s).

A P9 cold boot (`~/cp/integ-gates24b.txt`, `tools/cp/hunt/r3/cold-m86-pq.p9`) loaded ManualSave-86 into the world and
quit from the pause menu. Game status was 0, `wineserver -k` took 226 ms, nothing was left, and the quiesce reported
`extra=0`.

- **H6-hunt3 coverage (`docs/cp-hunt.md` round 3):** 44 game boots (34 on integ21, 10 on integ22).
  - 30/30 cold boots reached the world.
  - Two soaks of more than 60 min.
  - Load churn: 12 pause-menu loads of 10 different saves, then 5 quit-to-menu reloads.
  - 10 in-world settings changes.
  - 34/34 clean quits.
  - Both user saves load every time (33/33 and 21/21).
  - No visual corruption in 6216 screendumps.
  - Not covered: Quick Preset, lavapipe cold boots, and Wine handle counts.
- **H6-03 (4276c18c):** `p8launch` `quiesce()` now reaps the exited session keepalive. Only `keep_pid` is reaped, so
  no other child's status is consumed. Every game boot's teardown is 30 s shorter, and the false console watchdog is
  gone.
- **H6-05 (4276c18c):** `boot_p9` adds a verdict detail when a play script ends with no `[P8] end game` row.
- **H6-01, not applied:** the first-run guard still stops a boot on the game's Settings → GAMEPLAY tab ("Share game
  telemetry"). Stopping is the safe direction, and `drive.py` steers around the tab.
- **For H7 (round 3):**
  - H6-07: Venus DEVICE_LOST after a ~5 s host stall, 1/34 on integ21 and 0/10 on integ22;
  - H6-02: the `[THRS]` 4000-row cap;
  - H6-09: an unacked TLB shootdown behind a 2 s console hold during a host stall, 1/10 on integ22. The boot
    recovered.
- **Open:**
  - H6-04: no keyboard focus on the main menu after EXIT TO MAIN MENU when the pointer moved (game UI);
  - H6-06: one 2.6 s frame during an exit to the main menu (Venus/host).

#### Wave 9 H7 round 3 (integ25)

integ25 = integ24 + cp-fault round 3 (388c7abd) + cfeb10ba. Batch `strix:~/cp/integ-gates25.txt`:
- all 82 probes (round 3 adds `census`, `conslat`, `cputime` and `consstall`), `sigmask-gate`, the file-system and
  NFS gates;
- unit 464/0, host 40/42, castscan, ARM;
- P2, P4, P5 ×3, P6, P7, k5probes;
- thrstorm ×3 (1231, 1166 and 1184 rounds, 0 faults);
- P8 lavapipe ×2 and Venus at the menu (232 s, 219 s and 176 s);
- a P9 cold boot (`cold-m86-pq.p9`): ManualSave-86 into the world and a pause-menu quit; status 0,
  `wineserver -k` 216 ms, quiesce `extra=0`;
- a P9 720p fps boot: world median 29.8 fps, the profiled stretch 37.0 fps.

`vmhold` failed once in the batch, with 3 faults waiting 2.0–3.0 ms for `vm_lock`. Every timing in that run was 2–5×
slower: munmap's max was 21 ms against 4 ms in integ24. Three serial reruns on the same tree (`~/cp/integ25-vmhold.txt`)
passed, with munmap max 2.5–3.6 ms. It goes with the host-load flakes in §7.2h.

- **H6-07 (Venus DEVICE_LOST after a host stall):** a GPU submit no longer fails after 5 s. It spins 1 ms, then parks
  until a fence retires. It fails only when nothing has retired anywhere for 30 s (`GPU3D_HANG_MS`). Holding the GPU
  for 7 s (`vn_probe_fencehold`) used to fail; now all 40 submits succeed, 6/6 runs.
- **H7-14 (the 8th CPU):** the ext2 flusher and vmzero now nap off the run queue, so the game gets all 8 vCPUs. The
  fps did not change: the game is bound by the GPU or the host.
- **H7-17 (thrstorm −20 %):** an idle vmzero stayed READY, and every CPU's idle scan found it. It now naps for 5 ms
  (`process_nap_ms`).
- **H7-08 and H6-02 (census):** the 5 s census prints from vmzero instead of the PIT IRQ (time in the IRQ went from
  11 ms to 0). There is a per-sample row bound instead of the 4000-row cap.
- **H7-11/12 (console):** a short line behind a large write now waits about 0.8 ms p50 and 3 ms worst, down from
  64 ms p50. Per-line wire hold. NFS statistics and `[PFX]` detail rows are off by default in game kernels.
- **H7-15 (clocks):** `clock_gettime` on the CPU-time clocks (2, 3 and the dynamic negative ids) returns
  `process_rusage` time instead of an error.
- **H6-09 (an unacked shootdown behind a console stall):** after 1 ms, a shootdown leaves the flush owed to a laggard
  that is inside `cons_hw_tx`, and records the root it is owed for. The laggard flushes before it leaves the console
  code, and it touches no user mapping until then.
  - The first version (a1fe2051) reloaded whatever CR3 held. A CPU still on a dead process's freed root re-walked it
    and took a kernel #PF in `lapic_send_eoi` (H7-18).
  - de8df5e1 reloads only the owed root.
- **cfeb10ba (integrator):**
  - `x64_in_console` is one 64-byte record per CPU (`struct x64_cons_mark`, `console.h`) instead of a packed byte
    array (the C1-k7 invariant).
  - `x64_tlb_service` writes `x64_tlb_owed_root[me]` only when it is set, not on every IPI.
- **Open:**
  - A CPU can still sit on an exited process's freed page-table root; H7-18 exposed it. It predates round 3.
  - vmzero at idle priority (not merged).
  - `[PFE]` rows are not in the pf class.
  - virtio_blk's test stress task.
  - `vn_probe_transport`'s check is stale.
  - H6-04 and H6-06 (above).

#### Wave 10 H8-root (integ26)

integ26 = integ25 + cp-root (23e21259, `docs/cp-root.md`) + 53d5e787 + 0d5aa2ae. Batch `strix:~/cp/integ-gates26.txt`,
all green:
- all 83 probes (H8 adds `rootfree`), including `shootwait` and `vmhold`;
- the infra gates, unit 464/0, host 40/42, castscan, ARM;
- P2, P4, P5 ×3, P6, P7, k5probes;
- thrstorm ×3 (1137, 1139 and 1110 rounds, 0 faults);
- P8 lavapipe ×2 and Venus at the menu (191 s, 190 s and 145 s);
- the P9 cold boot (status 0, `wineserver -k` 221 ms, quiesce `extra=0`) and the P9 fp720 boot (world median 24.0 fps,
  profiled stretch 29.3 fps; the GPU was shared with an H9 game guest);
- 0 page-table assertion rows in any console.

- **H8-01 (page tables freed under another CPU):**
  - The killed threads' CPUs reap the group's slot from `schedule()` while still on its root, and the L85 refusal saw
    only the lowest CPU on it.
  - `vm_arch_teardown` now parks a root that any other CPU may be on, by bookkeeping or by real CR3, kept in one
    64-byte record per CPU (`struct x64_as_pc`). It frees the root after every such CPU's next CR3 load, at the next
    teardown or idle loop. Nothing waits.
  - `linux_probe_rootfree`: integ25 completed 0/5 boots (the guest reset); the lane passes 5/5 with 0 unsafe frees.
    About one race per Wine shutdown appeared in the integ22/23 gates.
- **H8-02 (a thread published into a dying group):** `process_clone` published READY with no liveness check. A
  SIGKILL racing it left a READY member that was switched to after the teardown with CR3 = 0, a silent guest reset. It
  re-checks under `proc_lock` now. `vm_arch_switch` refuses a torn-down AS (`[ROOTSW0]`).
- **ARM:** the same park, the ASID TLBI before the frames are freed, and a per-CPU ASID-generation flush.
- **The paired P9 fp720 boots** (Venus, back to back, GPU shared with H9) show no fps change: world 26.6 fps lane vs
  25.5 integ25, profiled stretch 30.8 vs 30.6.
- **Integrator:**
  - `t3_linux_probe.sh` fails any probe whose console has `[FREERACE] freeing root=`, `[ROOTSW0] cpu=` or a
    `[ROOTFREE]` row with `unsafe` above 0 (53d5e787). Match the kernel rows' own text: the rootfree check's summary
    "0 [FREERACE] row(s)" would otherwise match.
  - The `[ROOTFREE] every64` and `[ROOTPARK]` rows print from teardown, possibly under `proc_lock` with interrupts off,
    through a console that spin-waits on a stalled host. They are test-kernel only (9c4b48b9).
  - `frame_test` image_reserved (0d5aa2ae): the walk "reaches the border" when no further 8 MiB run fits. The ARM
    unit tier ended 7 MiB short of the frontier with that 7 MiB free and failed the exact-landing check
    (`frame_test.c:326`, 437/1 → 437/0).
- **Host-wide QEMU killers:** `run_desktop_test.py` and most `run_*_test.py` harnesses, `tools/evidence_rig.py`,
  `tools/perf_game_cycle.py`, `tools/boot_arm.sh`, `t3_nullentry.sh` and `make clean` run `pkill -f qemu-system`. On a
  shared host they kill every guest; one killed H9's game boot h4-p06. On strix run them only with
  `PATH=$HOME/cp/safe-bin:$PATH`, whose `pkill` signals only qemu-system processes whose cwd is the calling tree.
- **Open:**
  - H8-05: `pthread_create` returns EAGAIN for a few ms while killed slots drain.
  - The ARM `MODE=unit_tests` image is over its `.bss` placement window on main and on cyberpunk alike, so the ARM
    unit tier does not link without relaxing the assertion.
  - A parked root waits for the next teardown or idle loop (264 ms at most so far).
  - `x64_tlb_last_targets[]` is still a packed per-CPU array.

#### Main merge and wave 11 (integ27)

`cyberpunk` was merged into GitHub `main` on 2026-11-19 (f2ac6208 = main d3041f1e + integ26) and the branch was
deleted; integration now targets `main`.
- **The merge's checks:**
  - main's own CI tiers, GitHub main vs the merge:
    - host 37/41 vs 40/42;
    - x64 unit 448/3 vs 464/0;
    - ARM unit (with the `.bss` ASSERT relaxed, which both trees overflow) 424/0 vs 437/0;
    - desktop and the ARM build PASS on both.
  - The full cyberpunk battery on the merge: `strix:~/cp/integ-gatesmm.txt`.

integ27 = main f2ac6208 + cp-hunt4 (H9-hunt4 round 4, `docs/cp-hunt.md`) + cp-pagein (H10-pagein,
`docs/cp-pagein.md`). Batch `strix:~/cp/integ-gates27.txt`, logs `~/cp/integ27/`:
- all 82 probes (H10 adds `pagein`), the infra gates except `ext2w_gate` (below), unit 465/0, host 40/42, castscan, ARM;
- P2, P4, P5 ×3, P7, k5probes;
- thrstorm ×3 (1240, 1235 and 1242 rounds, 0 faults);
- P8 lavapipe ×2 and Venus at the menu (202, 188 and 161 s);
- the P9 cold boot (status 0, `wineserver -k` 221 ms, quiesce `extra=0`) and the P9 fp720 boot;
- main's ARM unit tier (relaxed) 438/0 and `desktop_test` PASS;
- 0 page-table assertion rows.

- **H9-12 fixed (cp-pagein):**
  - The old bug: a file-backed page-in that found no free frame returned the same 0 as a failed read. The fault path
    made it a permanent EIO (`VM_MAP_IOERR` on the executable region, SIGBUS). In a 12 GiB lavapipe save load, that
    killed Wine's SIGTRAP handler and the game.
  - Now:
    - The page sources say `-ENOMEM`.
    - `vm_file_obj_materialize` reclaims in at most 2 rounds, with no lock held and no sleep: first the parked zeroed
      frames, then the unmapped FAT16/NFS page cache.
    - It then answers `VM_FAULT_NOMEM`: a `[PAGEIN-NOMEM]` row and SIGKILL, Linux's VM_FAULT_OOM shape. The mapping is
      never marked.
    - Real read errors keep M6/L69's EIO + SIGBUS.
  - Probe `pagein` and unit `test_vm_exec_page_nomem` fail on main and pass with the fix.
  - In H9's 12 GiB lavapipe recipe, the page that killed h4-l01/l02 is recovered (0 `[EIO]`, 0 SIGBUS). The game still
    dies of its own unhandled `int3` with the guest out of memory: a lavapipe save load does not fit in 12 GiB.
- **H9-hunt4 round 4:**
  - 28 game boots on integ25, 0 crashes on Venus, and a 125-minute active soak with no leaks or fps drift after
    minute 70.
  - Findings H9-01..H9-12, in `docs/cp-hunt.md`.
- **Reds, rerun:**
  - **`ext2w_gate`:** failed 2 of 8 runs on integ27 and 0 of 4 on main. Each time, a delay-0 crash point's e2fsck had
    leak-class damage only (`-` bitmap differences, counts) and the synced files were intact.
    - The writer keeps committing on close right after `SYNCED` (`closewb` thousands per run), so a delay-0 quit can
      land inside a commit. That is guarantee 2 (cp-ext2w §7.6). The gate's strict delay-0 rule assumes no commit is in
      flight.
    - The kernel delta is inert there: 0 `[PAGEIN-NOMEM]` rows.
  - **P6 Venus:** failed once; d3d12tri rendered at 245 fps but two screendumps 30 s apart were identical. It passed
    2/2 reruns. See the open items.
- **Open:**
  - **Glass freezes on Venus since the main merge:** the screen stops updating while rendering continues. Seen twice:
    - P8 Venus on the merge, the menu frozen for 172 s;
    - P6 d3d12 on integ27, frozen for at least 30 s.

    The 13 earlier P6 batches and H9's 24 integ25 Venus boots showed none. The merge brought `gpu_abi.c`'s transfer
    width check (a box.w up to GPU_ABI_MAX_BYTES for every target) and `fs.c`'s socket-option changes.
  - **The `ext2w_gate` delay-0 rule** races close write-back commits. Either accept leak-class damage there, or quiesce
    the writer before `SYNCED`.
  - **From H9 round 4:**
    - H9-03: IceWM's Shift+Esc window menu over the game; a one-line `KeySysWinMenu=""` in IceWM's preferences.
    - H9-05: the H6-07/H6-09 counters are never printed.
  - **From H10:**
    - NFS read-ahead windows (up to 256 MiB) are not reclaimed under pressure.
    - The anon/COW OOM answer is a catchable SIGSEGV where Linux sends SIGKILL.
  - **Host:** `/mnt/fast` on strix is down until about 2026-11-21. `lsof` hangs on it, and `~/cp/safe-bin/lsof` answers
    `lsof -t FILE` from /proc.

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
  both. Since cp-sync, syscall-side reads go through `file_read_user` (it makes the destination
  resident, then reads into it directly); do not reintroduce a bounce buffer, which costs about 35 % on
  large NFS reads while both guards still pass. Merges from `main` must also run castscan and the
  panic-sink allowlist, which main runs neither of. Since cp-nfs2 the boot guard also covers G6 (concurrent readers), G7
  (per-file windows), G8 (64-bit, large reads, stat times, symlinks) and G9
  (hard mount under injected reply loss). When a conflict lands in those files, keep the cp/nfs side; see
  `docs/cp-nfs.md` §6.
- Slots (wave 8): a lane uses at most 2 of the 4 qemu slots and leaves one free between its batches; probe loops run one stream at a time. A gate whose timer starts before `qemu-slot.sh` returns must not count the slot wait.
- Wine changes go only through `tools/cp/wine-patches/` (applied by `build-wine.sh` `CP_WINE_PATCHES`); the default stage is `~/cp/wine-cp2`. A seeded prefix made by another Wine stage re-runs the wineboot update on every boot (`run-p8.sh` warns): rebuild it with `P8_PHASE=pfx`.
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
- [x] `wine cmd /c echo` and `winprobe.exe` pass; prefix persists on ext2. *(Gates P2, P3)*
- [x] `winecfg` in IceWM; raw mouse deltas reach a Windows program. *(Gate P4)*
- [x] `vkcube.exe` and the D3D12 triangle on lavapipe and on Venus. *(Gates P5, P6)*
- [x] Fullscreen scanout present measured. *(Gate P7: vkcube.exe 93 fps at 1920×1080, flip 6.3 ms; `docs/cp-p7.md` §1)*
- [x] Cyberpunk main menu screendump, OCR-confirmed. *(Gate P8, lavapipe and Venus)*
- [ ] 10 minutes played from a host save; save/reload; fps against both
      baselines. *(play/save/reload pass on Venus, `docs/cp-p9.md`; the 30 fps
      target and the 1920×1080 Low column are open, wave 7)*
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
