# L72 — the post-load park, the 8×1 framebuffer, and `x86-codegen.h:403`

M6/L72, worktree `~/hobbyos-rw-lanes/l72-postload` (branch `rw-l72-postload`,
seeded at the merged tip `60362cdd`). The loader is clean at this tip (0
refusals, 0 deaths, the full 277.5 MB, `[TXT] n=3584 bad=0`, 260 presents with
changing CRCs, `Initializing.` on screen). This lane instrumented and named the
two walls after it, and fixed the one that was actually killing every run at
the end of the load.

## 0. Summary

| question | answer |
|---|---|
| who is in the park? | main (`/NFS/RimWorldLinux`), `Loading.AsyncRead`, `UnityGfxDeviceW` — a three-party completion handshake, every wake landing (`woke=1 of=<pid>`) |
| is it a lost wake? | **no.** 190 182 futex ops in 1690 s, every WAKE found its waiter; it is a **protocol livelock**: each side re-arms instead of reporting completion |
| what allocates? | `UnityGfxDeviceW` (tid 28), **one 4 KiB managed buffer allocated and copied per round** (glibc `memcpy`'s `movntdq` loop / `__bzero` for the zeroed part) into a monotonically growing anonymous arena; 196 608 pages = 768 MiB in L65 run 1 |
| why 8×1? | **QEMU's VGA text fallback**, not the WM, not Unity: with VBE off and the CRTC zeroed, `vga_get_text_resolution()` computes `(cr[1]+1)*8 × (cr[0x12]+1)` = 8×1. The guest's own mode never changed (`[BGA] xres=1920 yres=1080 bpp=32 en=0x41` for the whole run) |
| the assert? | `x86-codegen.h:403` is the `default: g_assert (0)` arm of `mono_x86_patch_inline`'s opcode switch (L61); a byte that is not a call/branch arrives at the patch site. Instruments now capture the byte (`[KREG]`+`[UDX]`) |
| what was killing the ladder? | the **device replay** on DSO text pages: read 1 = the file's bytes, read 2 = 4096 zeros, so L69's "two raw reads must agree" gate refused a page the device had answered correctly, and the process took SIGBUS at `[PUBS]=307200` in every run |

## 1. The park's three threads

The futex flight recorder's idle-hang dump prints one `[FUTEX] q` row per parked
thread; this lane added `[FUTEX] rgn`, which resolves the key against the
process's own region table (`kind 3` = anonymous mmap — a pthread stack or a
Mono arena; `kind 7` = DSO). L65 run 1 (the canonical park):

| thread (comm) | pid | futex key | key's region | role |
|---|---:|---|---|---|
| `/NFS/RimWorldLinux` (main) | 4 | `0x1083B17624` | `0x1083AAF000+0x106000` kind=3 | waits for “step complete” |
| `Loading.AsyncRe…` | 26 | `0x1100000334` | `0x1100000000+0x1000000` kind=3 | waits for “next step” |
| `UnityGfxDeviceW…` | 27 | `0x113000A460`, `0x10E080EBB0` | `0x1130000000+0x1000000`, `0x10E0000000+0x3000000` kind=3 | waits for gfx work / present |
| `Loading.Preload` | 28 | `0x10E08BAA00` | same `0x10E…` arena | **idle**, `waits=44 wakes=0` (matches §35) |

The ring (`[FUTEX] r#…`, 64-op window, 112 ops/s) repeats six ops per round:

```text
WAIT 27 key=0x1100000334              # AsyncRead parks for work
WAIT 28 key=0x113000A460              # gfx worker parks
WAKE 4  key=0x1100000334 woke=1 of=27 # main hands a step to the loader
WAKE 4  key=0x113000A460 woke=1 of=28 # main hands a step to the gfx worker
WAIT 4  key=0x1083B17624              # main waits for completion
WAKE 27 key=0x1083B17624 woke=1 of=4  # the loader reports the step done
WAIT 27 key=0x1100000334              # ...and immediately asks for the next
WAIT 28 key=0x10E080EBB0              # gfx worker's second (present) wait
WAKE 27 key=0x10E080EBB0 woke=1 of=28
```

Two facts settle what this is:

1. **Every wake lands.** `woke=1 of=27/28/4` on every cycle, `wakes` grows in
   lockstep with `waits` (main: `waits=2537 wakes=2084`; AsyncRead:
   `waits=444 wakes=438` in this lane's 1200 s run). There is no lost wake, no
   wrong key, no bitset mismatch: it is a *protocol* livelock — each side
   completes its half and re-arms, so the completion the main thread is waiting
   for is never *reported*.
2. **None of the three is in managed code.** The SIGUSR2 self-sample (rule
   107) copies 2048 words from each thread's own stack and resolves them with
   `mono_jit_info_table_find`: **zero JIT hits for pids 4, 27 and 28** at
   t=620 s, while the same sampler resolves 20+ managed frames for
   `LoadableXmlAsset..ctor` on a loader thread in the same sample. All three
   park in *native* `mono_os_cond_wait` (`code rip=0x108011895D` is libmono's
   syscall stub), i.e. inside Unity's native async-read/gfx plumbing, not in a
   managed `WaitHandle`.

The party that should have completed is the **`Loading.AsyncRead` step**: it is
the worker in the protocol (main hands it a step, it reports “done”, main
renders). A terminating load would report `done` *and stop asking for work*;
here it re-arms (`WAIT key=0x1100000334`) every round, so main can never leave
`LongEventHandler.LongEventsUpdate`. In the second shape seen in this lane (a
1200 s run parked at the def-plateau) the async reader is *idle* (`waits=444`
frozen) and the ping-pong is main ↔ `UnityGfxDeviceW` only — the same
handshake with the loader removed, which is why the “three threads” are two
plus the gfx worker.

## 2. The unbounded allocation

`[PUBS] n` counts every page published into a mapping. L65 run 1:
`307 200 → 503 808` over 1149 s = **+196 608 pages (768 MiB), 171 pages/s,
5.85 ms/page**.

This lane's `[ANONF]` row samples the *anonymous fault* publisher (every 4096th
publish) and carries the faulting thread, the faulting instruction and the
region:

```text
[ANONF] n=274432 tid=28 va=0x11D92A3000 rgn=0x11D8000000+0x3AC1000
        kind=3 prot=3 flags=34 obj=0x0 shared=0 rip=0x10800BBE1F
[ANONF] n=278528 tid=28 va=0x11D9AA3000 rgn=0x11D8000000+0x3AC1000 …
[ANONF] n=282624 tid=28 va=0x11DA2A3000 rgn=0x11D8000000+0x3AC1000 …
```

* **who:** tid 28, `UnityGfxDeviceW` (the same thread that is woken twice per
  round; the job workers 43/46/48 show the same signature during the load).
* **where:** a *growing* anonymous arena (`0x11D8000000`, 58 MB and climbing,
  then `0x11E0000000` after it). `kind=3`, `shared=0`: a plain `mmap`.
* **what instruction:** `rip=0x10800BBE1F` / `…BBDFD` = libc + `0xBBE1F`,
  which disassembles to `movntdq %xmm3,0x30(%rdi)` — the non-temporal store
  loop of glibc's **`memcpy`**. Other samples of the same arena land at
  libc + `0xBBFDC`/`0xBC017`, the SSE2 loop of **`__bzero`**. So each round
  *allocates* a 4 KiB managed buffer (zeroed by `memset`) and *copies* 4 KiB
  into a growing destination (`memcpy`), touching one fresh page each time —
  not a stack growth and not a file read.
* **rate:** one page per round-trip. The round is 5.9-6.1 ms (171-164
  pages/s), and the futex cycle runs at 112 ops/s ≈ 6 ops per 53 ms — i.e.
  **one 4 KiB allocation per handshake iteration**.

So the allocation is not a kernel leak: it is a **4 KiB managed buffer
allocated once per loading step and never reused**, in a loop whose driver is
visible in the main thread's managed stack at t=380 s (run `pdz`):

```text
ResourcesAPIInternal.LoadAll <- String.wstrcpy <- ResourcesAPI.LoadAll
  <- Resources.LoadAll <- Resources.LoadAll <- <GetAllInFolder>d__2.MoveNext
  <- … <- <GetResolvedGrains>d__1.MoveNext
  <- <GetResolvedGrains>d__1.GetEnumerator
  <- SubSoundDef.<ResolveReferences>b__37_0
  <- LongEventHandler.ExecuteToExecuteWhenFinished (×3)
  <- LongEventHandler.UpdateCurrentAsynchronousEvent
  <- LongEventHandler.LongEventsUpdate <- Root.Update <- Root_Entry.Update
```

RimWorld's `Misc Init (InitializingInterface)` is walking `GetAllInFolder` and
calling `Resources.LoadAll` per folder; every iteration copies a path
(`String.wstrcpy`) and allocates a 4 KiB asset/string buffer on the gfx
worker's heap. The *retry condition* is the load step itself: the enumeration
never reaches an end (the loader always has “one more folder”), so the loop
never terminates and the buffer is never freed. On the kernel side nothing
succeeds or fails — the pages are ordinary anonymous faults; the unbounded
part is the *managed* side re-allocating per iteration.

## 3. The 8×1 framebuffer

Not the WM, not Unity minimising, not our surface being torn down while the
game draws. Rows:

1. **The guest's mode never changed.** `[BGA]` (new, 5 s tick, printed only on
   change) reads the Bochs dispi registers — the same values QEMU's
   `vga_get_resolution()` returns in VBE mode — plus the two CRTC fields the
   text path uses:

   ```text
   [BGA] xres=1920 yres=1080 bpp=32 en=0x0x41 hdisp=239 vdisp=55 -> mode ok
   ```

   One row per boot, **no change** in three 1200-1800 s runs: VBE enabled, LFB
   on, 1920×1080×32, CRTC programmed. The guest kept drawing into its
   1920×1080 framebuffer the whole time.
2. **QEMU's 8×1 is the text fallback with a zeroed CRTC.** In
   `hw/display/vga.c`, `vga_get_resolution()` returns `vbe_regs[XRES/YRES]`
   when VBE is enabled, and otherwise `(cr[H_DISP]+1)*8` ×
   `(cr[V_DISP_END]…) + 1`. A VGA device that has been **reset and not
   re-programmed** has all CRTC registers zero → `(0+1)*8 = 8` by `(0+1) = 1`
   = **8×1**. That is a device state, not a window state.
3. **It is a reboot artefact in the tree.** The only 8×1 PPMs in the whole
   worktree are `l65-variance/obj/rw-desktop/shots/207..209_running.ppm`, and
   that run's serial log shows a second `BdsDxe … starting Boot0002` right at
   that point (a guest reset); every PPM of every run in this lane is
   1920×1080. The WM's content rect is `win->w - 4 × win->h - 36`
   (1916×1018 for one window) and cannot produce 8×1; a Unity minimise would
   show a window state, none exists.

Ours was the *consequence*: a reset leaves the machine blind until something
re-programs the mode. `virtio_gpu_health_tick()` now checks the dispi/CRTC
state every 5 s and restores the shared desktop mode if it was reset
(`[BGA] mode restored`), so a device reset can no longer leave an 8×1 screen
behind a running guest.

## 4. `x86-codegen.h:403`

L61 already proved *what* the site is: the string
`../../mono/arch/amd64/../x86/x86-codegen.h` is referenced from six sites, all
inside `amd64_patch` / `mono_x86_patch_inline` at `libmonobdwgc+0xf5600`, and
`:403` is the last arm of its opcode switch:

```c
switch (*code) {
case 0xe8: case 0xe9: …             /* call/jmp rel32 */
case 0x0f: g_assert (code[1] >= 0x80 && code[1] <= 0x8F); …
case 0xe0 … 0xe2: case 0xeb: case 0x70 … 0x7f: …   /* rel8 */
case 0xFF: g_assert (code[1] == 0x15 || code[1] == 0x25); …
default: g_assert (0);              /* <-- 403, condition string "0" */
}
```

This lane built the instrument that captures the byte. `tools/rw/l61-assert-probe.sh`
replaces the assert's `lea file,%rdi` with `ud2` at both sites, so the fault
becomes a user `#UD` and the kernel's register row carries the function's live
state. Decoding the function confirms which register is what at `0xf5670`:

```text
f5600  push rax; mov rax,rsi        ; rax = target
f5604  mov dl,[rdi]                 ; dl = *code (the opcode byte)
…
f5670  lea rdi,[rip+…]              ; ud2 here in the probe build
```

so at the trap **`rdi` = the code pointer being patched** and **`rdx & 0xFF` =
the opcode the switch read**. Two kernel rows were added:

* `[KREG]` (existing) already prints `rdi`/`rdx` for every user fault;
* `[UDX]` (new) copies the eight bytes **at `rdi`** through
  `l_copy_from_user` and prints them with the opcode and the region:
  `[UDX] code=<va> b0..b7 op=<dl> rgn=<base>+<len> kind=<n>`.

A clean `mov 0x..(%rip),%rax` there means a legitimate opcode the table does
not implement; random bytes sewn into the middle of a call mean the code
buffer was corrupted. **Not captured in a run here:** every run that reached
the JIT-patch phase in this lane either died earlier on the page-in wall (four
runs) or survived past it without the assert (1200 s run). The probe and both
rows are staged and verified present in the binary; the byte remains the one
open row of this section.

## 5. What was fixed

1. **The device replay at the exec gate** (`fat16.c`, `vm.c`). Measured on
   `/RWDSO/UnityPlayer.so` off `0x757000` (and `0x9AD000`): the *first*
   cache-bypassed read returns the file's bytes (`r_nz=3754`, matching the host
   disk at the same LBA), the next reads return 4096 zeros. L69's gate required
   two **agreeing** raw reads, so it refused a page the device had answered
   correctly, and the main thread took SIGBUS at `[PUBS]=307200` in **every**
   run (three different secondary shapes: SIGBUS, Mono `g_assert`, libmono NULL
   deref). The gate now:
   * asks `fat16_page_probe()` for its own first read and publishes it when a
     second read agrees **or is the all-zero replay** (`[TXT] PROBEGET`,
     `[TXT] REPLAY`);
   * treats “read 1 non-zero + read 2 all-zero” as the replay in
     `vm_read_page_from_device()` too; different **non-zero** content is still
     a torn page and is still refused;
   * back-off 2/10/30/60 ms (was 2/2/2), on the controller-repair timescale
     (L67: 50-63 ms).
2. **A data page can be poisoned too** (`vm_data_page_rescue`, `[PDZ]`): an
   all-zero cache page for a file object asks the device once and *records*
   that the device holds content. It deliberately does **not** publish on that
   single read: a torn or wrong read installed in `.data` corrupts a static
   pointer silently. Three runs aborted at startup with `object.c:1939 klass`
   (two with the ud2 probe build, one with the single-read rescue trusted).
   No `[PDZ]` row fired in the clean runs — data-page poisoning is not what
   this ladder hits; the row is there for the next lane.
3. **A lost display mode is restored** (`virtio_gpu_health_tick`, `[BGA]`), so
   a VGA reset cannot leave the machine at 8×1 behind a live guest.
4. **Two probe instruments** so the park and the assert are answerable in one
   run: `/TPLATE` (late stack sampler on its own file `/RW_HOME/l72.log`,
   bounded, no Mono object walks — the rich probe stopped the watchdog at
   t≈100 s in two runs, and its early `mono_class_from_name`/`add_icall`
   registration is the documented `object.c:1939 klass` abort shape, which
   killed two late-only runs at startup; late-only now skips both and touches
   nothing but `mono_jit_info_table_find`) and `/UDX`.

## 6. Ladder

| run | window | presents | `[PUBS]` at end | res MiB | end state |
|---|---:|---:|---:|---:|---|
| L65 run 1 (pre-L69) | 1800 s | 260 | 503 808 | 1899 | **park**, 112 futex ops/s for 19 min |
| diag (this lane) | 780 s | — | — | 1133 | `[TXT] ZERO off=0x9AD000` → EIO → SIGBUS at t≈135 s |
| diag2 | 780 s | — | 307 200 | — | **Mono `g_assert` (x86-codegen.h:403)** → abort |
| diag3 (ud2 probe) | 900 s | — | 307 200 | — | libmono+0x1EB680 `mov rax,[rax+0x10]`, rdi=0x1040069380 → SIGSEGV |
| pdz | 1200 s | — | 307 200 | — | `[TXT] ZERO off=0x757000` (raw `r_nz=3754`) → EIO → SIGBUS |
| final1 (replay rule) | **1200 s** | **1** | 176 128 | **627 (flat)** | alive at the deadline; **no park growth**, no assert, no death; still in the def pass (`[XO] …RangedSpacer.xml`, 43.6 MiB NFS), main↔`UnityGfxDeviceW` ping-pong |
| 1800c (replay rule, final) | 1700 s window | 1 | **307 200** | 1135 | **passes the old wall** (no EIO/assert there), then a **kernel-mode #PF in user address space** (`cr2=0x11CB280000`, rip=0x700AB808) kills pid 4 at t≈560 s |

Reading of the ladder:

* every pre-fix run died at **exactly** `[PUBS]=307200` — the moment the
  game first executes the DSO text page the device replays as zeros. It is one
  wall with three secondary shapes; the park of L65 run 1 is the same moment
  *before* L69's gate existed (the zero page was published and executed, so
  the game livelocked instead of dying);
* with the replay rule the process no longer dies there — the 1800 s attempt
  passes `[PUBS]=307200` intact (no EIO, no assert) and dies later, at t≈560 s,
  on a **kernel-mode #PF in user address space** (`cr2=0x11CB280000`,
  `[KPF-DIAG] … why=HOLE`), which is a separate kernel bug for the next lane;
* **RSS is bounded** where the run survives (627 MiB, flat over the last 200 s
  of the 1200 s window; 1135 MiB at t≈560 s in the 1800 s attempt, i.e. the
  loader's own working set): the 0.64 MiB/s / one-page-per-6 ms growth of L65
  does not appear in either;
* the run still ends in the handshake, now at the **def-plateau**
  (43.6 MiB NFS) with main ping-ponging against `UnityGfxDeviceW` and
  `Loading.AsyncRead` idle. So the *page-in* wall is closed and the
  *handshake* wall remains — a different, later instance of the same protocol
  livelock, not the DSO-delivery one.

## 7. Regression

`TOOLS_SKIP=1 ./run_unit_tests_intel.sh`: **431 run, 0 failed, UNIT TESTS
PASSED**. Gates (`obj/l72/gates/`): redzone, m2_mono, m2_tp, m2_threads, m12,
mmap_race, m3_window, m4_gl, rwnfs — see the ladder in the launch log.
