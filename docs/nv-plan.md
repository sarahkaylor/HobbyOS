# HobbyOS native NVIDIA driver (Ada / RTX 4090) — execution plan

Canonical plan for branch `nv/ada-driver`. Public status doc: `docs/nvidia-driver.md`.
Contract docs this work stands on: §5. Written to be read cold after any pause.

Owner's goal, verbatim: *"create a driver that will work for an nvidia card"*,
validated by *"games like Rimworld and Cyberpunk"* with *"reasonable performance"*.
No copying from other drivers; studying them is allowed.

Hardware: RTX 4090 (AD102 `10de:2684`) on Proxmox `r1` (`root@192.168.10.174`),
driver VM 300, ROM-work VM 312. Build host `sarah@192.168.0.123`.

---

## 0. Definition of done, honestly staged

| Level | Claim | Status |
|---|---|---|
| L1 | HobbyOS owns the card, verifies NVIDIA's firmware, and builds every structure GSP-RM will consume | **done, measured** |
| L2 | FWSEC runs → SEC2 unfences → booter runs → GSP-RM reports `INIT_DONE` | **moved: SEC2's START is now taken — reset, then the GSP's four fixups, and the core runs (round 7, execution measured). The booter's own verdict at ≥105 ms is the open measurement** |
| L3 | A kernel submit reaches the 3-D engine and a clear reads back | not started |
| L4 | nvk enumerates AD102; `vkcube` draws and presents | not started |
| L5 | RimWorld / Cyberpunk render at 720p ≥30 fps | not started |

L1 stands on its own: a from-scratch driver that identifies the part, brings up
the register window, reads 38 MB of vendor firmware from disk and proves it
byte-for-byte, and lays out VRAM the way the vendor booter expects. L2 is the wall.

---

## 1. What is true today (measured; each row cites its log line)

| Stage | Evidence |
|---|---|
| PCIe; BAR0 16 MiB, BAR1 32 GiB (1 GiB window mapped), BAR3 32 MiB | `[NV] found 10DE:2684 at 2:0.0` |
| Identification | `boot0=0x192000A1 chipset=0x192 AD102 stepping=161 gsp=required` |
| PTIMER live | `ptimer hz~999MHz`, advance proved |
| Register map vs NVIDIA 580.178.04 on the same card | 9/9 rows equal (`docs/nvidia-driver.md` §5.5) |
| Falcon probe | PMU/GSP `present`; SEC2 `fenced` |
| 38,061,600 B firmware → contiguous frames, digest == host | `phys=0x200c29000`, `sha256 e30231ec…` |
| radix3 over `.fwimage` | `root=0x2054c3000 pages=21 maps 9288 image pages` |
| 256-byte WPR metadata block, DMA-able | `magic ok wpr=0x5F8700000..0x5FE400000 block_phys=0x2054e2000` |
| VRAM layout (matches contract field for field) | `image=0x5FBEB0000 bootbin=0x5FE2F8000 frts=0x5FE300000` |
| VBIOS readable; FWSEC ucode located | `desc 0x43E3C`, payload `0x44168..0x540E8`, `IMEM+DMEM==StoredSize` |
| Falcon page accepts **writes** (read fence only) | `[NV-PROBE] verdict=WRITES LAND` |
| Ucode staged on the **GSP** with signature + correct BROM params | `[NV-FWSEC] GSP IMEM+DMEM loaded`, `brom para=0xB24 engid=0x400 ucode=9` |
| FWSEC executes | **NO** — `status1438=0xBEEF0000 frts=NEVER WRITTEN`, `SEC2=still fenced` |
| Unit tier | **513 tests, 0 failed** |

Reproduce in ~4 minutes: `tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`.

---

## 2. The wall: FWSEC does not execute

The verdict channel is sound: BAR0 `0x1438` bits 31:16, prefilled with
`0xBEEF0000` so "nobody wrote" is distinguishable from "success". (`MAILBOX0` is
**not** the FWSEC channel — the vendor passes NULL there.) The ucode reaches the
GSP with the right block size, the right BROM parameters and the PKC signature
patched at DMEM `0xB24`. It still does not run.

**Experiment queue — cheapest first, one deploy each, yes/no answers.**

| # | Experiment | Positive result means | Negative means |
|---|---|---|---|
| F1 | Read GSP `0x110114` right after the IMEM load, before any reset (window still readable): expect `0xF100` | the DMA landed in the GSP | it did not → F2 |
| F2 | Load IMEM/DMEM by **PIO** (`0x180/0x184`, `0x1C0/0x1C4`), with a wait that does not poll the fenced `XFER_STATUS` | PIO runs the core → the DMA request encoding is wrong | still no run → ucode/params, not transport |
| F3 | After an attempt, read the RISC-V switch `0x111668`: `0x111` = running in RISC-V mode | it switched then stopped → FRTS question | never set → core never got that far |
| F4 | Re-check `0x840000` in the same run for the privilege drop (secondary oracle) | FWSEC's job was done | confirms F2/F3 |
| F5 | Vary the DMA context id: `0x600` is `TRANSCFG(ctx)`, ctx = register index; sysmem-no-coherent is **4**, not 5 | a ctx mismatch explains it | rules the DMA path out |

Still unrun and cheap: **B. fuse check** — read `0x8241C0 + (UcodeId-1)*4` and AND
against the descriptor's `SignatureVersions` (`0x0003`). If the image is not valid
for this part's fuses, the boot ROM rejects it and no amount of transport fixing
helps.

**Sequencing rule:** no FWSEC-first shortcut — but note the §2.5 correction: SEC2's
blocker is its **RISC-V core select** (`0x841668` reading `0x110`), not a privilege fence.
Rounds 1-4 misread that as a privilege wall.

### 2.1 Round 1 of the local swarm (2026-11-23): what eight deploys actually showed

Branch `nv/wp-a-fwsec`, evidence in `wp-a/evidence/nv/*.log` (8 runs, one per
experiment, chronological). Verified by the coordinator against the logs and the
code, not from the agent's summary.

| # | Result | Status |
|---|---|---|
| F1 | `0x114 = 0xF100` after the IMEM load, `dma state post-imem 110=0x2054E30 114=0xF100` | **POSITIVE, MEASURED** — the DMA now addresses the image into IMEM (before: every block wrote falcon offset 0, `dst first=0x0 last=0x0`) |
| F2 | PIO path: no DMA request, no context, no status word — same failure as DMA | **NEGATIVE, and it answers F5** — no context exists in PIO, so the DMA context id is not the blocker; `0x600` is one register with a ctx field in bit 16, not an array index |
| F3 | `0x111668` reads `0xBADF5620` before the core select is cleared, real values after | **INSTRUMENTAL** — clearing `0x111668` (`0x110 → 0x1`) is what un-fences `0x110..0x120`; without it every `110/114/118/120` read is the fence word and START is refused |
| F4 | SEC2 `0x840000`/`0x840100` = `0xBADF5620` in all 8 runs | **NEGATIVE** — the privilege drop did not happen |
| B | `fuse@0x8241E0=0x1`, `sigvers=0x3` → `[NV-FUSE] verdict=COMPATIBLE`, **signature index 1** (the code had patched index 0) | **POSITIVE, MEASURED** — the image is valid for this part's fuses; this branch is ruled out |
| — | Verdict word: `0x1438` reads `0xBEEF0000` for 2 s in 7 runs, then `0x0000000000000000` once (run 8, after the three DMA/SIZE/completion-bit fixes) | **MEASURED transition; success NOT established** |

Six defects were fixed to get there, each with its own log line: the DMA destination
never advanced; the SIZE field `8` masked to 0 (4-byte transfers, silently accepted);
completion polled on the wrong bit (`0x120` bit 1 instead of `0x118` bit 1); the RISC-V
core select never cleared after reset; signature index 0 instead of the fuse-selected 1;
FRTS `init_cmd` never patched.

**What is NOT established, and why the word "success" is not used:** SEC2 stays
fenced, the core never halts (`halted=0`), the RISC-V switch stays unreadable
(`0x111668=0xBADF5620` at the end of the same run), and the DMEM read-back disagrees
with the staged buffer (`init_cmd=0x34D93D3C/0x15`, `verdict=DMEM MISMATCH`) — so the
image the falcon holds is not yet proven to be the image that was staged. A zero in
`0x1438` is consistent with "no error code"; it is also consistent with a core that
cleared the word on an exception path. Next: make the DMEM read-back trustworthy
(word-index vs byte-offset) before reading anything into the status value.

### 2.2 Round 2 (2026-11-23): FWSEC executes — execution proven, completion still not

8 commits on `nv/wp-a2-fwsec2`, six of ten deploys used. Merged and re-gated at
`578 tests / 0 failed`.

**The read path was the bug.** `0x1C0`/`0x180` take the address as a **word index in
bits 15:2**; the driver wrote `addr>>2` into bits 0..13 — one shift too far — so every
PIO access addressed byte `addr/4`. It is self-consistent (a write and a read at the
same wrong address both answer), which is why three rounds of round-trips "passed".
Caught only by comparing against the staged bytes: `[NV-LAND] dmem probes=9 match=9
displaced=0 verdict=LANDED`.

| Observation | Value | Reading |
|---|---|---|
| `r2a` sentinel → status | `0x5EC20000 → 0x0000000000000000` at **+0x63E1540 ns (≈104 ms)**, `halted=0`, `ctl=0x0` | START consumed, word written |
| `r2e` (DMEM only, no IMEM write) | `0x5EC25A5A → 0x00005A5A` — **hi16 zeroed, lo16 kept** | a field write of a zero code, not a clear |
| `r2e` fence timing | `fenced=1 at +0xDA0 ns` (3.5 µs), write at 104 ms | the host only reads; it cannot have written the word |
| `r2f` **ablation** (empty IMEM) | `status1438=0x5EC25A5A` untouched, `ctl_1us=0x10` = **START refused**, `changed_words=0` | no image ⇒ no execution ⇒ no write |

So **FWSEC executes** (MEASURED: the write happens only when the image is loaded, only
after START is consumed, and inside a window the host cannot write). What is still not
true: **completion** — SEC2 reads `0xBADF5620` in all six runs, the metadata's
`verified` word stayed at its `0x5EC2A0A0` sentinel in two *forced* booter attempts, and
IMEM cannot be read back at all (a marker comes back `0xDEAD5EC1` when loaded and `0x0`
when not; the port write stops the DMA engine and an engine store wedges the queue, so
both read-back paths are compiled out).

Next in this lane: fix the engine STORE encoding (the only channel that survives the
3.5 µs fence — likely `0x11C`'s owner bit on a store, with an unambiguous prefill such
as `0xA6A6A6A6`), then poll `0x110100`/`0x111668`/`0x1438` non-blocking across the full
2 s to see whether the GSP window re-opens at ~104 ms and read DMEM at that instant.

### 2.3 Round 3 (2026-11-23): the read path is closed, so the verdict is halt time

Four deploys (`r3a`-`r3d`), merged and re-gated at **intel 583/0 and ARM 465/0**.

| Question | Measured | Reading |
|---|---|---|
| Does the engine STORE work? | DIR-clear control moved `0xB0A6A6A6` into DMEM `0x1800`; all four DIR variants + IMEM `sec=1` delivered **0/64 words** to host memory while `0x120`'s stores-pending count walked `1,2,3,4` and none retired | **no host channel reads falcon memory after START** — the store direction does not execute for host requests, and it is not an encoding bug (DIR bit, ctx 1/4/5, `0x11C` owner all change nothing) |
| Does the window re-open at ~104 ms? | `0x5EC25A5A → 0x00005A5A` at **+105,758,880 ns**, core halts **+4 µs earlier**, 649,152 samples → **1 event**; 40,572 DMEM samples, **all** `0xBADF5620` | the write happens, the window never re-opens; **the earlier "re-opens at 104 ms" hypothesis is refuted** |
| Is the fence caused by our latency? | 80 ms idle pause before START → verdict still at +105,788,608 ns | delay exonerated; queued stores implicated (one variable) |
| Does the verdict word carry a status? | FRTS `init_cmd 0x15 → 0x99`: halt at **27.6 ms** and `0x1438` keeps its sentinel | **no error code exists on that channel** |

**So the verdict instrument is halt time**: 27.6 ms = the FRTS command was rejected, 105.8 ms = it completed. The word is conditional evidence and nothing more. SEC2 read `0xBADF5620` in all four runs, so no privilege drop and the booter was not attempted, per the plan's sequencing rule.

Next in this lane: stop trying to read past the fence. Read the FRTS region (`0x5FE300000`, 1 MiB) and the metadata block through **BAR1** after a launch, use halt time as the verdict, and never issue a store before START.

### 2.4 Round 4 (2026-11-23): the fence was hiding a carve, and N1 is met

Merged and re-gated at **intel 584/0 and ARM 468/0**.

**A4 — FWSEC's signature is an exclusion, not a word.** BAR1 (the VRAM window) needs
no re-basing for `[0, fbSize)` and takes a 1 MiB prefill that reads back complete
(262,144/262,144 words — too large for a write buffer, so it is really in VRAM).
After a launch, the FRTS region `[0x5FE300000, 0x5FE400000)` reads back **host memory
before START in all six runs**, and **as an access counter (`0xBAD0AC34`, +1 per read)
only when the FRTS command was accepted** (halt 105.8 ms). When the command is rejected
(`init_cmd 0x99`, halt 27.6 ms) the region is byte-for-byte the prefill —
`untouched=0x40000 changed=0`. Edge walks (`MMMMPPPPP` at `0x5FE2FC000`,
`PPPPPMMMM` at `0x5FE3FB000`) put the fence exactly at `frtsOffset` and
`frtsOffset + frtsSize`. So the host loses sight of precisely the 1 MiB FWSEC was
handed, and only when it ran: **the carve is the evidence**. Neighbours — the page
below, the page above, the bootbin inside WPR2, the metadata page — stay memory and
writable. The privilege drop is still not visible from that side (`verified=0`,
`nonzero_words=0`, SEC2 `0xBADF5620`), so "the carve happened, its consequence did not".

**RELOC — the AArch64 relocation class, closed systemically.** The class was **411
pointer words in 21 tables**, not three call sites (308 of them the Linux-ABI syscall
dispatch). The kernel now links `-pie -z notext` and applies `R_AARCH64_RELATIVE` at
boot, before `main()`: `[RELOC] entries=0x1A1 applied=0x1A1 slide=0x3FA400000
outside=0 status=0 ok`. A build guard fails the link if the image carries relocations
nothing applies (`FAIL -- 417 kernel pointer(s) with no applied relocation`), so the
failure mode is a red build instead of an EC=0 boot. `--emit-relocs` was rejected
measured, not by taste: its `.rela.rodata` is not `SHF_ALLOC`, so it is never loaded.
This also exposed `nv_shim_refusal_name()` handing back a *link* address on ARM (a test
had passed only because a link address is non-NULL).

**N1 — met.** The blocker was one function: `libx11.c:input_target()` delivered input
to the Unity container XID, whose only selection is `StructureNotifyMask`, so
`mouse_event()` looked for `ButtonPressMask` there and dropped every press (motion
worked because Unity polls `XQueryPointer`). With the target chosen by selection class,
three presses dismiss the Steam modal and the **menu itself** presents at
**51.02 fps median** (720p, 7,600 presents / 149,848 guest ms, guest clock only).
Coordinator-verified: the dumped menu frame is the sha256 the report quotes, and it is
RimWorld's main menu with no overlay. Renderer is still host **llvmpipe** (r1 has no
NVIDIA driver; the 4090 stays on `vfio-pci`), and **audio is not reachable** and is not
claimed.

**EL0 — the DRM ABI is proven from the caller's side.** `src/user/nvabi_probe.c`
(static-PIE, no libc) drives VERSION → GET_CAP → GETPARAM → VM_INIT → GEM_NEW → mmap →
write → VM_BIND → CHANNEL_ALLOC → SYNCOBJ → EXEC → WAIT and `[EL0] RESULT PASS 33/33`.
The keystone row: the kernel prints `stream=0x80bce9ae` for a push, and the client
independently computes `0x80bce9ae` over the bytes **it wrote through its own mmap** —
two implementations, one buffer. It found a real defect: `ioctl(closed_fd, …)` answered
`ENOTTY` where Linux answers `EBADF` (use-after-close disguised as "wrong request").
A native `.BIN` cannot issue a DRM request at all — there is no `SYS_IOCTL` row — so the
honest caller is a Linux ELF, which is what nvk is.

### 2.5 Round 5 (2026-11-23/24): SEC2 was never privilege-fenced, and Cyberpunk has numbers

Merged and re-gated on both arches. Four lanes: the booter attempt, NVIF, the Cyberpunk
benchmark, and an independent soak.

**A5 — the booter was negative twice over, but the wall was misdiagnosed in the plan.**
All eight runs: metadata `verified` stayed at its `0x5EC2A0A0` prefill, the bootbin at
`0x5FE2F8000` stayed `0x2000/0x2000` words of the host's `0xA5A5A5A5` (page map
`MMMMMMMMMP` before *and* after, so no WPR2 either), no metadata copy at `gspFwWprStart`,
DMEM untouched (0/0x1800 words), MAILBOX0 still holding our argument. The premise held
every run (FRTS carve, halt 105.73-105.92 ms, edges `MMMMPPPPP`/`PPPPPMMMM`).

What actually changed: **`0x841668` read `0x110`** — RISCV|BRFETCH, the same word the GSP's
`0x111668` read before F3. Clearing it gave `ctl=0x10`, `transcfg=0x5`, and with the GSP's
pre-load DMA configuration (`0x624` `0x110 -> 0x190`) SEC2 **loaded and executed for the
first time** (`halted=1`, DMEM landing 8/8 LANDED) — writing nothing.

> **Correction to §2 and §5.** "SEC2's fence is privilege-gated (measured twice)" is
> **wrong**. Rounds 1-4 were reading a *core-select* state as a privilege wall. SEC2 sits
> on its RISC-V core; nothing about privilege had to be dropped for it to run. The
> sequencing rule stands (FWSEC first), but its stated reason does not.

Three transport defects were fixed on the way: the load header's offsets used as file
rather than data-region offsets (signature bytes landing at the head of IMEM), the DMA
destination never advancing, and the missing DMA configuration that stalled the queue at
block 19. Next: measure START->STOPPED now that `ctl` answers (microseconds = START never
reached the core; milliseconds = the ROM ran and rejected), then settle whether the file's
`0x7600` app-code bytes are ciphertext or plaintext, then the cheap `(engid,ucode)` variants.

**NVIF — served from ring 3.** `DRM_IOCTL_NOUVEAU_NVIF` (nr `0x47`) answers `NEW` route
`0x00` class `0x0080` NV_DEVICE (object keyed by the client's cookie, as nouveau does),
`MTHD NV_DEVICE_V0_INFO` (AD102, chipset `0x192`, rev `0xa1`, family ADA), and `SCLASS`;
six engine classes are refused **by name** with `need=gsp_rm`. Unit tier 588/0, EL0
`[EL0] RESULT PASS 44/44`, both mutations red. And the honest answer to "where does a stock
nvk stop": **`GETPARAM GRAPH_UNITS` first** (Mesa's `goto out_err`), then the engine class
object, which is GSP-RM. Also found: NVIF's encoded `_IOC` size is a *buffer length*, not a
struct width — the only row on that surface that cannot use an equality check.

**Cyberpunk 2077's own benchmark, three stacks, one machine, one game copy** (1280x720,
`strix`, capture from the game's result page and its `summary.json`/`frames.csv`):

| stack | avg / median / p90 / 1%-low fps | settings | what it is |
|---|---|---|---|
| host native (Proton, RADV, 9070 XT) — *control* | 139.41 / 145.6 / 108.2 / 77.3 | Custom=Low, 720p, FSR off, RT off | instrument check: reproduces P0's 141.6-142.9 / 74.1-76.1 |
| **HobbyOS on Venus** | **29.19 / 29.7 / 22.6 / 18.2** | Custom, scaling **Off**, 720p | **platform**, not driver: the `nv` driver has no 3D engine |
| Linux guest on Venus | 25.86 / 27.0 / 20.4 / 16.5 | Low, FSR 2.1 Auto | the row `docs/cyberpunk.md` had left unrun |

HobbyOS **reached and completed** the benchmark: it wrote `benchmarkResults/*/{summary,frames}`
to the ext2 prefix (`e2fsck -fn` rc 0). The three columns are not averaged or ranked — the
two guests differ in upscaling and vkd3d-proton version.

**SOAK — the merged tree holds, and the click instrument was broken.** Intel 584/0 and ARM
468/0, three runs each, identical counts; `[RELOC] entries=0x1A1 applied=0x1A1 slide=0x3FA400000`
on every ARM link; desktop boots and paints (frame inspected); RimWorld menu at 50.51 fps on
the merged kernel. It also found that `tools/gate/xptr_click.py` as committed could not have
produced the PASS its own report quoted — its regex required `exposes=(\d+) rc=` adjacent
while the client prints `exposes=... to_container=... rc=...`, so it graded a passing run as
FAIL. Fixed with a tolerant parse plus a QEMU-free `--self-test`.

---

### 2.6 Round 7 (2026-11-24): the START write lands — the sequence was refusing it

Full report: `SWARM-REPORT-WP-A7.md`. Instrument: one register row per phase
(`[NV-SEC2T]`), `ctl` + `0x841668` + `0x840624` + `0x840600` in the same row,
read before the write, at the read nearest the write (2.27-2.43 us later), and
at 10 ms / 100 ms; 8 boots on three images, every claim repeated in ≥3.

Three states, one variable between each:

| sequence | bit 1 after the write | core |
|---|---|---|
| A/B: the four fixups, **no falcon reset** (rounds 1-6) | cleared by 2.3 us | never leaves STOPPED (0 of ~131 k samples) |
| R: falcon reset (`0x048 &= ~3`, `0x014 = ones`, pulse `0x3C0`), no fixups | **latched** (`ctl=0x12` at 100 ms, `0x130` mirrors it) | still stopped |
| S: reset, **then** select clear, **then** the four fixups | **consumed** (`ctl=0x0`) | **runs**: 129,313/129,313 and 131,073/131,073 samples running over 100 ms |

So the trigger was being refused (cleared without a start) in every state this
project had run, and it is a real start once the falcon has been reset and the
GSP's four fixups — `0x624 |= bit 7`, `0x10C = 0`, `0x600 = 5`,
`0x084 <- PMC_BOOT_0` — are written **after** that reset. Both halves are
load-bearing; the halt step is what skips the first (it issues UC_CTRL's reset
bits only when ctl is unreadable or not already STOPPED, and on this card ctl is
always readable and STOPPED).

Three corrections this round makes to earlier rounds:

* **2.3 us is the instrument.** `wr_to_read_ns` = 2272-2304 ns in all three
  states, including the one where the core runs. Round 6's "START → STOPPED at
  2.3 us" was the write+read round trip.
* **The fence follows state, not privilege.** After a reset `0x841668` reads
  `0x110` (RISCV|BRFETCH) and the whole falcon page answers `0xBADF5620` until
  the select is cleared; once the core runs, everything except `ctl` fences
  again. Both are visibility states, grounded in rows.
* **A read-back is not evidence, but a latch is.** `0x104` and `0x084` take a
  value that cannot occur spontaneously and read it back (16/16 attempts), the
  post-reset START bit latches and is plain RW in a one-write-per-bit sweep, and
  `0x130` mirrors bit 1 but does not set it. The write reaches the block in
  every state; what changes is whether the core takes it.

Open: does the booter complete (poll the metadata `verified` word ≥300 ms — S's
core is still running at +100 ms and the reference's own halt is ~105.8 ms);
which of the four fixups is load-bearing (one per boot); and the entry/IMEM
layout if the core runs but never writes the verdict.

---

## 3. Work packages (parallel, each with a gate)

| WP | Scope | Gate | Needs 4090? | Status 2026-11-23 |
|---|---|---|---|---|
| **A. FWSEC** | F1-F5, then the launch | `0x1438` hi16 becomes a real code **and** SEC2 unfences | yes | **partial**: 3 mechanisms fixed, image lands, verdict word written; and (round 7) SEC2's START is taken once the falcon is reset then given the GSP's four fixups — the core runs. **Next: the booter's own verdict at ≥105 ms** |
| **B. fuses** | fuse bitmask vs `SignatureVersions` | non-zero AND, documented | yes, read-only | **done**: `COMPATIBLE`, sig index 1 |
| **C. RPC (offline)** | `docs/nv-rpc-implementation-spec.md` — pure half only | the spec's 24 cases pass in the kernel unit tier on synthetic queues | **no** | **done**: T-01…T-24, two mutation runs recorded; wired into the boot path by G |
| **D. DMA facility** | `nv_dma.{c,h}`: bus address ≠ phys, identity guard, translation probe | a unit test rejects a mapping whose `bus != phys` under translation | **no** | **done**: 13 cases, two red runs |
| **E. Present gate** | `/dev/fb0` mmap over an 8,294,400 B framebuffer | pixel at (1919,1079) in a QMP screendump | **no** | **done**: pixel red, 535-page over-map closed, one word past the frame now faults |
| **F. `/dev/nv*` shim** | nouveau-subset ABI over a mock RM | probe creates device/VM/channel; fence retires | **no** | **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows + 21 named refusals, CPU-mapped BOs + `mmap` (583/0 on intel, 465/0 on ARM); stock nvk still needs NVIF and PRIME |
| **G. SEC2 booter / glue** | `nv_gsp_sec2_start_booter` + the transport seam | metadata `verified` == `0xA0A0…`; block PTE[0] == `nv_dma_bus()`, one doorbell | yes / **no** for the seam | **seam done** (`nv_gsp_rpc_setup/notify/stage8`, 568/0, two mutations red); booter still blocked: two forced attempts left `verified` at its sentinel |

Ordering by leverage: D unblocks C; C unblocks the driver's upper half; E unblocks
every present measurement; F lets a real Vulkan stack attach before GSP-RM is up.
C/D/E/F are how this project progresses on days when the card is busy or FWSEC is
stuck.

---

## 4. The ladder to games (`docs/nv-games-ladder.md`; weeks are ESTIMATES)

| # | Rung | Gate | Today |
|---|---|---|---|
| N0 | 1080p present gate | 8,294,400 B maps; pixel visible | **closed 2026-11-23**: full frame maps, `(1919,1079)=(255,0,0)` in a QMP screendump; the recorded `-ENOMEM` did not reproduce on this tip (see `SWARM-REPORT-WP-E.md`) |
| N1 | RimWorld's own menu on virgl 720p | ≥30 fps median, input, audio | **MET on fps and input (2026-11-23)**: the real menu (steam modal dismissed by 3 presses after the `input_target` fix) presents at **51.02 fps median**, verified from a dumped frame; **audio not reachable**, renderer host **llvmpipe** (r1 has no NVIDIA driver — the 4090 stays on `vfio-pci`). Round 3's 51.02 fps was the same number measured on a modal no press could dismiss |
| N2 | `/dev/nv*` shim over a mock RM | probe + fence retire | **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows (magic `'d'`, real struct sizes), 21 named refusals, CPU-mapped BOs with `mmap`; a stock nvk still needs NVIF and PRIME |
| N3 | GSP-RM boots | mailbox moves, first RPC answered | FWSEC never ran |
| N4 | RM objects + first submit | 64×64 clear reads back | absent |
| N5 | nvk enumerates AD102 | `vulkaninfo` lists `AD102 (NVK)` | absent |
| N6 | **first frame** (`vkcube`) | draws + presents | absent |
| N7 | RimWorld via Zink+nvk | menu ≥30 fps, world loads | 170.9 fps synthetic only |
| N8 | Cyberpunk via vkd3d+nvk | P9 script end to end | **benchmark run on HobbyOS (2026-11-24)**: 29.19 avg / 29.7 median / 18.2 1%-low at 720p, Custom preset, scaling off — on **Venus** (platform), not the nv driver; host native control 139.41 avg. The nv driver cannot run it: no 3D engine |
| N9 | "reasonable performance" | 720p ≥30 fps incl. present | unmeasurable today |

**Highest-leverage rung: N6** — the one observation where VRAM, VA, submit,
fences, shader upload and present all work at once, and exactly where both games'
already-proven userspace attaches. Critical path N3→N6 ≈ 20-37 weeks.

Two facts that keep it honest: both titles' non-graphics stacks already work on
HobbyOS (audio via virtio-snd/ALSA; Cyberpunk played 10.5 min and reloaded), and
Venus and the native driver are mutually exclusive on this card — Venus needs the
GPU on the host, `nv` needs it in the guest, and r1 has one.

---

## 5. Contracts this work stands on (read before coding)

| Doc | What it settles |
|---|---|
| `docs/nvidia-driver.md` | what the driver does; every claim anchored to a log line |
| `docs/nv-gsp-firmware.md` | blob formats, HS headers, what the boot ROM validates |
| `docs/nv-gsp-reference.md` | NVIDIA's own driver on this card: pre/post BAR0 dumps |
| `docs/nv-gsp-wpr-contract.md` | WPR2 ownership (the **booter** programs it), layout, meta fields |
| `docs/nv-pmu-acr-contract.md` | boot order: host → FWSEC on the GSP → SEC2 booter; FRTS |
| `docs/nv-vbios-map.md` | finding ucode in the ROM (EFI-stripped logical base rule) |
| `docs/nv-devinit-contract.md` | why devinit replay is dead on Turing+; the sentinel method |
| `docs/nv-fwsec-launch-contract.md` | launch sequence, BROM values, diagnosis table |
| `docs/nv-rpc-implementation-spec.md` | transport field by field + 24 test cases |
| `docs/nv-games-ladder.md` | the ladder, effort ranges, "build now" list |


7. **A static function-pointer table in `.rodata` is broken on AArch64** — the kernel
   applies no load-time relocations to static data, so a folded table holds *link*
   addresses and the first call executes zeros (ESR `0x2000000`, EC=0, "unknown
   reason"). The kernel now self-relocates (411 words, `[RELOC] … ok`) and the link
   gate fails if anything is left unapplied. Never write such a table; build it at
   runtime or let the gate stop you.
8. **BAR1 is a different channel from BAR0.** After START the falcon windows answer the
   fence word, but the VRAM window still reads — and the FRTS region is excluded from it
   precisely when FWSEC ran. Judge a launch by halt time; use the carve as the receipt.

Corrections already applied, so nobody re-derives them: the fence is **read-side
only**; `0x1FA824/828` are `WPR2_ADDR_LO/HI` as (start,END) in 4 KiB units and are
read-protected; there is **no** descriptor to write on GA102; FWSEC's verdict is
`0x1438`, not `MAILBOX0`; VBIOS pointers are relative to a logical image with the
EFI ROM removed (`+0x14E00` above `0xFC00`).

---

## 6. Rules of evidence (each cost at least one run)

1. **A masked read is not evidence.** Reads beyond the readable blocks return
   `0xBADF5620`, whose bit 4 is clear — "not halted" and "unreadable" look
   identical. Use a channel that can answer, or judge by effect.
2. **Prefill anything whose zero means success.** Bitten twice, in two registers.
3. **Hex arithmetic belongs in a test, not a comment.** Every "the document must
   be wrong" so far was my own subtraction slip.
4. **One `[NV-*]` row per stage; the row names the stage when it fails.**
5. **Check that an observation could have been negative** before calling a
   subsystem working.
6. **Do not copy.** Contracts and byte layouts yes; code no. Tag claims
   MEASURED / SOURCE / INFERRED.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| FWSEC becomes a long RE problem | F1-F5 are single-deploy tests; WP C/D/E/F proceed without the card |
| One card, many lanes | `tools/nv/gpu-switch.sh`; never two VMs with the same hostpci; VM 300 = driver, 312 = ROM work |
| A wrong write wedges the card | every write preceded by a read or a contract; VM restart + FLR clears state (observed) |
| Session/attention decay | this file + `docs/nvidia-driver.md` §5.x are the memory; every claim cites a log line |
| Estimates are optimistic | the ladder states its assumptions; N0-N2 are deliberately driver-independent |

---

## 8. Resuming cold — checklist

1. `git log --oneline -12` on `nv/ada-driver`; the last commit message states the
   current obstacle in its own words.
2. Run `tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`; read the
   `[NV-FWSEC]`, `[NV-PROBE]`, `[NV-MAP]`, `[NV-GSP]` rows.
3. If FWSEC still reports the sentinel, run the next un-tried §2 experiment.
4. If a contract doc contradicts this file, the doc wins — fix this file in the
   same commit.
5. Keep the tier green: `UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` on
   `sarah@192.168.0.123` (513 tests today).
