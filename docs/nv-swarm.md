# NV swarm — operating notes for parallel work packages

Read `PLAN.md` first (it is the contract), then this file (it is the harness).
This file exists so five agents can work at once without re-deriving the setup.

## 1. Topology

| Piece | Where | What |
|---|---|---|
| Repo | `git` with worktrees under `~/Documents/GitHub/hobbyos-nv/` | one worktree per work package, its own branch |
| Integration tip | `ada/` @ `nv/lane-rpc2` = `ce4c44af` | every WP branch is cut from `ce4c44af` |
| Build/test box | `sarah@192.168.0.250` (Proxmox c4, VM 200 `nv-swarm`, Ubuntu 26.04, 32 vCPU, 62 GiB, `/dev/kvm`) | remote tree `/srv/nv/<wp>`, pre-warmed |
| Card | Proxmox `r1` `root@192.168.10.174`, VM 300, RTX 4090 `0000:01:00` | only the WP-A agent ever touches it |

**Agents edit files on the Mac.** The c4 box only compiles, links and runs QEMU —
it is fast (32 vCPU, KVM) and it is the project's normal build host class.
Never edit sources over ssh; never build on the Mac (an 18-core laptop).

## 2. The harness: `~/bin/nvswarm`

```
nvswarm build <wp> [make args...]   # rsync worktree -> /srv/nv/<wp>, then make
nvswarm sync  <wp>                  # rsync only
nvswarm sh    <wp> '<command>'      # run a command in the remote tree
nvswarm status                      # tree sizes, load, /dev/kvm
```

Default make args are `-j12 ARCH=intel MODE=unit_tests LO_ON_DISK= hobbyos.elf disk.img`,
and the harness always adds `TOOLS_SKIP=1 BROWSER_SKIP=1`: the kernel unit tier
needs neither the in-OS LLVM distribution nor the WebKit staging, and the dist
build fails on a fresh host (undefined `pthread_setname_np` in the self-host link).
`obj/` and `third_party/` are **never synced** — the remote copy is pre-warmed
(staging builds for LLVM/ICU already exist there). Never run `make clean` on the
remote; never `--delete` third_party.

Useful invocations:

```
nvswarm build wp-d                                            # kernel + image, intel
nvswarm build wp-d ARCH=arm  MODE=unit_tests LO_ON_DISK= hobbyos.elf   # ARM compile check
nvswarm sh    wp-d 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'   # the unit-tier gate
nvswarm sh    wp-d 'tail -40 build.log'                       # when it fails
```

The tier script exports nothing itself, so pass the two skip vars on the same
line, or its internal `make` re-attempts the dist build.

`run_unit_tests_intel.sh` uses `-enable-kvm` when `/dev/kvm` exists (it does) and
refuses to report a pass it did not see. Its run budget is 20 s; raise it with
`UNIT_TESTS_TIMEOUT=1200` only if a run genuinely needs longer (say so in the report).

## 3. Non-negotiable project rules (they cost runs to learn)

From `PLAN.md` §6 and `docs/lock-scope.md`; also load the `hobbyos-kernel-constraints` skill.

1. A masked read is not evidence. Reads past the readable blocks return
   `0xBADF5620`; "not halted" and "unreadable" look identical. Judge by effect or
   use a channel that can answer.
2. Prefill anything whose zero means success. Two registers were bitten already.
3. Hex arithmetic belongs in a test, not a comment.
4. One `[NV-*]` row per stage, and the row names the stage that failed.
5. Check that an observation could have been negative before calling a subsystem working.
6. **Do not copy driver code.** Byte layouts, field tables and contracts yes; code no.
   Tag every claim MEASURED / SOURCE / INFERRED.
7. Kernel TUs: `-mgeneral-regs-only` (no SIMD), byte-wise parsing of packed/wire
   structures (no wide loads at odd offsets), Normal-NC DMA buffers, `QEMU_SMP`
   untouched, no literal `-smp` added.
8. Prefer `src/host/*` pure modules with an in-kernel test (`src/kernel/*_test.c`)
   over anything that needs hardware. That is how this project gets coverage.

## 4. Gates and reporting

Each agent owns one work package and must, before reporting:

- `nvswarm build <wp>` green (intel) **and** a per-TU ARM compile of every file it adds or changes
  (`nvswarm sh <wp> 'TOOLS_SKIP=1 BROWSER_SKIP=1 make ARCH=arm obj/arm/<file>.o'`);
  the **full ARM link is broken at `ce4c44af` for a pre-existing reason**
  (`kernel .bss exceeds the limine placement window`), reproduced on both the Mac and
  the c4 box on an untouched tree — it is not yours to fix, and the per-TU compile is
  what catches the real risk (an unguarded x86-only reference in shared code);
- `nvswarm sh <wp> './run_unit_tests_intel.sh'` → `0 failed` (state the test count);
- its own suite fails before the fix and passes after (say which case you watched fail);
- one commit on its branch, message in the repo's style (`feat(nv-…): …`), no push;
- `SWARM-REPORT-<wp>.md` at the worktree root: what changed (file list), the exact
  gate line, what is NOT verified, and the next step. Commit it with the code.

Branch/worktree map: `wp-a`→`nv/wp-a-fwsec`, `wp-c`→`nv/wp-c-rpc`,
`wp-d`→`nv/wp-d-dma`, `wp-e`→`nv/wp-e-present`, `wp-f`→`nv/wp-f-shim`.
Work only inside your own worktree. Do not touch `ada/`, `lane-*/`, or another `wp-*`.

## 5. Work packages as assigned

### WP-A — FWSEC launch + fuse check (card lane; owns VM 300)
`PLAN.md` §2 F1–F5 and the fuse check. One deploy per experiment,
`tools/nv/deploy.sh --mode test --gpu --wave NVSMI.BIN`, run **on the c4 box**
(`nvswarm sh wp-a 'tools/nv/deploy.sh …'`; `~/.ssh/mac_to_r1` is installed there).
Read `docs/nvidia-driver.md` §5, `docs/nv-pmu-acr-contract.md`,
`docs/nv-gsp-wpr-contract.md`, `docs/nv-devinit-contract.md`.
Positive/negative reading for each experiment is written in the plan — do not
invent new ones; do not touch SEC2 until `0x1438` hi16 stops reading `0xBEEF`.

### WP-C — GSP-RPC transport, offline
Implement exactly `docs/nv-rpc-implementation-spec.md` (block map, producer
header, 48-byte elements, checksum over whole slots, SWAP_RX, doorbell) with
cases T-01…T-22 passing in the kernel unit tier on synthetic queues.
Files: `src/include/nv_rpc.h`, `src/kernel/nv_rpc.c`, `src/kernel/nv_rpc_test.c`,
one line in `src/kernel/unit_test.c`. Fake MMIO comes from `struct nv_mmio_ops`
(`src/include/nv.h`). **Dependency decision:** the transport never calls the DMA
facility — it takes `uint64_t block_bus` as an argument (PTEs are built from it),
so this branch links standalone.

### WP-D — DMA facility
`src/include/nv_dma.h` + `src/kernel/nv_dma.c` + `src/kernel/nv_dma_test.c`.
Interface (fixed by the plan; WP-C compiles against it):

```c
struct nv_dma_map { uint64_t phys, bus, len; uint32_t pages; };
int      nv_dma_alloc(uint64_t len, struct nv_dma_map *out);  /* contiguous frames, bus resolved */
void     nv_dma_free(struct nv_dma_map *m);
int      nv_dma_translation_enabled(void);   /* VT-d translation live? (iommu_vtd.h) */
uint64_t nv_dma_bus(const struct nv_dma_map *m);             /* the ONLY way a call site gets a bus addr */
int      nv_dma_guard(uint64_t phys, uint64_t bus);          /* -EIO if translation && bus != phys */
void     nv_dma_test_set_translation(int on);                /* test seam, documented as such */
```
Gate: a unit test rejects a mapping whose `bus != phys` under translation, and
`nv_dma_bus` never returns `phys` by accident. Both arches compile.

### WP-E — present gate (rung N0)
`/dev/fb0` mmap at 1920×1080: `vm_map_fb()` maps `USER_FB_SIZE`
(10,485,760 B) over an 8,294,400 B framebuffer; `src/kernel/vm.c` ~5303-5310.
Goal: an 8,294,400 B image maps (not `-ENOMEM`) and the **pixel at (1919,1079)
in a QMP screendump** is the one the program wrote. Tooling that already exists:
`tools/qmp_drive.py`, `capture_screenshots.py`, `src/host/fb_mmap*.c`.
Evidence must be the screendump path + the pixel value, in `evidence/nv/`.

### WP-F — `/dev/nv*` shim over a mock RM
nouveau-subset ABI (device, alloc, VM bind, channel, submit, fence) over a mock
RM so a Vulkan stack can attach before GSP-RM boots. Gate: a probe creates
device/VM/channel and a fence retires, in the unit tier. Scope it to the ABI +
mock; no card, no GSP.

## 6. Round 2 (2026-11-23, after the first merge)

All four branches are cut from `d44659a2` (`nv/integ-swarm`), which is round 1 merged and
green at `561 tests / 0 failed`. Worktrees: `wp-a2`→`nv/wp-a2-fwsec2`, `wp-g`→`nv/wp-g-glue`,
`wp-f2`→`nv/wp-f2-nouveau`, `wp-arm`→`nv/wp-arm-bss`.

Round-1 state you inherit (do not re-derive): `nv_rpc` (transport, T-01…T-24) and `nv_dma`
(bus≠phys) are merged and tested; `/dev/nvshim` exists over a mock RM with its own ioctl
numbers; `/dev/fb0` maps exactly one frame; and the FWSEC run ends with
`status1438=0x0 frts=0 SEC2=still fenced`, with a DMEM read-back that reports
`MISMATCH` because the read path mixes word index and byte offset.

The full ARM link is still broken (`.bss exceeds the limine placement window`,
`linker.ld` ASSERT `__bss_end <= 0x45600000`) — that is `wp-arm`'s job now, and until it
is fixed every other agent keeps the per-TU ARM check.
