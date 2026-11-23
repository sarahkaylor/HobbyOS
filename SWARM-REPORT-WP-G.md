# SWARM-REPORT-WP-G — the transport/driver seam (round 2)

Branch `nv/wp-g-glue`, cut from `d44659a2`. Files: `src/include/nv_gsp_glue.h`
(new), `src/kernel/nv_gsp_glue_test.c` (new), `src/kernel/nv_gsp.c`,
`src/include/nv.h`, `src/include/nv_fw.h` (comment only), `src/kernel/unit_test.c`,
`src/kernel/unit_test.h`.

## What was wired

The two merged facilities were built to be independent: `nv_rpc_block_build()`
takes `block_bus` and never calls `nv_dma`; `nv_dma` owns `nv_dma_bus()` and
refuses to answer under VT-d translation. This work package is the only place
they meet.

**Stage 8 of the boot now exists** (`nv_gsp_rpc_stage8`, called from
`nv_gsp_firmware_check` after the layout/booter stages):

1. `nv_dma_alloc(NV_RPC_BLOCK_SIZE)` — 129 pages, contiguous, zeroed, pool-checked.
2. `nv_rpc_block_build(block, nv_dma_bus(&map))` — 129 PTEs, command-queue
   producer header published, status header deliberately left zero (RM's write
   of it is the boot's first proof of life).
3. `nv_rpc_queue_init()` — both queue offsets, SWAP_RX polarity.
4. `nv_rpc_link_apply()` — one read of the peer's header. It returns `-EAGAIN`
   because RM has not run, so **the doorbell is withheld** and the row says so.
   The only notification path in the driver is `nv_gsp_rpc_notify()` =
   `nv_rpc_tx_prepare` + `nv_rpc_tx_commit` + **one** `nv_rpc_kick`.

**The ad-hoc parts it replaced:**

- `frame_alloc_contig()` → `nv_dma_alloc()` for the 256-byte WPR metadata block,
  and the address that goes into SEC2's MAILBOX0/1 is now `nv_dma_bus()`
  (`[NV-GSP] meta … block_phys=… block_bus=…`, `[NV-GSP] stage5`).  `struct
  nv_dev` carries `struct nv_dma_map gsp_meta` instead of `gsp_meta_phys`; the
  `nv_info` ABI field is untouched.
- `#define NV_GSP_QUEUE_HEAD0` in `nv_gsp.c` deleted — unused, and a second copy
  of the doorbell offset is how a driver ends up with two doorbell writes.
  `NV_RPC_DOORBELL_OFF` (nv_rpc.h) is the only definition.
- New rows, one per stage, naming the stage that failed:
  `[NV-RPC] stage=block …` (bus/phys/pages/pte0/pte128/identity),
  `[NV-RPC] stage=link …`, plus `nv_rpc_dump("link")`.  Every pre-existing
  `[NV-*]` row is intact.

## Gates (exact lines, on the committed tree)

```
$ nvswarm build wp-g
build_exit=0            # tail of the log is the disk-image stage; no warnings in nv_gsp.c/nv_gsp_glue_test.c
```

```
$ nvswarm sh wp-g 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
Tests run:    568
Tests failed: 0
UNIT TESTS PASSED
```

Baseline on this branch was 561 / 0. `+7` = the six new cases plus the
boot-path case in `nv_gsp_glue_test.c`.

```
$ nvswarm sh wp-g 'make ARCH=arm MODE=unit_tests obj/arm/<f>.o'   # per-TU
nv_gsp             OK      nv_gsp_glue_test  OK      unit_test  OK
nv                 OK      nv_fw             OK
```

The full ARM link is still broken by another lane (`.bss exceeds the limine
placement window`); per-TU is the gate, as SWARM.md §4 says.  `nv_gsp.c` and
`nv_gsp_glue_test.c` are `__x86_64__`-only, so on ARM those two TUs compile
empty — that is a property of the driver lane, not a check that passed by luck.

## Test that can fail — and was watched failing

`src/kernel/nv_gsp_glue_test.c`, 7 cases:

| case | claim |
|---|---|
| G-01 | 129 pages through `nv_dma`; `pages * 4096 == len == 0x81000`; teardown returns every frame |
| **G-02** | **PTE[0] (read out of the block) == `nv_dma_bus(&map)`**, and all 129 PTEs equal `bus + i*4096` |
| G-03 | command header published, status header 36 bytes still zero, `link_apply` → `-EAGAIN` |
| **G-04** | commit alone rings nothing; one message → **one** doorbell call, queue 0; two messages → two |
| G-05 | no bus address, no block: setup refused under forced translation with **no frames taken**, and a rebuild of an existing mapping refused with **the block left byte-for-byte as it was** |
| G-06 | `notify` with no built queue / no message → `-EINVAL`, zero doorbells |
| G-07 | the **boot-path function itself** (`nv_gsp_rpc_stage8_into`, fake MMIO device): publishes the block, returns `-EAGAIN`, and **writes zero registers**; a second call reuses the same 129 pages |

Mutation 1 — *pass phys where bus is expected* (the mutation PLAN.md names):

```c
- uint64_t bus = nv_dma_bus(&r->block);
+ uint64_t bus = r->block.phys;
```

```
Tests run:    568
Tests failed: 1
UNIT TESTS FAILED
EXPECT_EQ FAILED: rc == -ENOTSUP at src/kernel/nv_gsp_glue_test.c:285
```

Mutation 2 — a second notification (an extra `nv_rpc_kick()` before the real one):

```
Tests run:    568
Tests failed: 1
UNIT TESTS FAILED
EXPECT_EQ FAILED: c.calls == 1 at src/kernel/nv_gsp_glue_test.c:237
```

Both reverted; `grep -rn MUTATION src/` → no matches; the green run above is the
post-revert tree.

Why G-05 and not G-02 catches mutation 1: with VT-d translation off, `bus` and
`phys` are the same number (MEASURED: `identity=1` in every `[NV-RPC] stage=block`
row in this tier), so an assertion against `phys` cannot see the substitution.
The only state in which the two answers differ is translation-on, and that is
exactly the state `nv_dma_bus()` exists to refuse in.

## What is NOT verified

- **Stage 8 has never run on the card.** `nv_gsp_firmware_check()` is gated on
  `d->hardware` (`nv.c:335`); no boot was attempted this round — the card belongs
  to another lane. What has been executed is the same function against a fake
  MMIO device in the unit tier (G-07).
- **The doorbell has never been written on silicon.** It cannot be, in this
  round's state: `nv_rpc_link_apply()` returns `-EAGAIN` until RM writes the
  status-queue header, and the notification is deliberately downstream of that.
  `nv_gsp_rpc_notify()` is proven only against the counting seam (G-04).
  No message is staged by the boot path yet: 72/73 (`SET_SYSTEM_INFO`,
  `SET_REGISTRY`) need the bootargs page, which this lane does not build.
- **"bus == the address the GPU dereferences" is WP-D's caveat, not a new
  measurement.** GSTS.TES is clear on this box, so `bus == phys`; the test
  reaches the translated branch only through `nv_dma_test_set_translation()`.
- **The block is not yet compared against RM's own view** (no RMARGS page, no
  bootargs): the geometry is pinned by T-01..T-04 and the self-covering PTE
  check, not by the firmware accepting it.
- **Three ad-hoc DMA handoffs remain unconverted**, deliberately named rather
  than half-done: `nv_fw_load_blob()`'s frames for the booter, the loader and
  the RM image (`d->booter_phys`, the radix3 root), and the FWSEC ucode staging
  buffer inside `nv_gsp_fwsec_launch()` (`frame_alloc_contig`, `src_phys >> 8`
  into the XFER source register). Same class of bug, different owner: the fix is
  an `nv_dma` constructor that adopts an existing verified run (so `nv_fw` can
  hand back a mapping instead of a physical address), and that is a shared-module
  change, not a transport-seam change.
- **The full ARM link** (another lane) and **any GSP-RM behaviour**.

## Next step

1. Bootargs page + RMARGS cross-check + pre-queue 72/73, then `nv_rpc_link()`
   and `GET_GSP_STATIC_INFO` (docs/nv-rpc-implementation-spec.md §11 step 6).
   `nv_gsp_rpc_notify()` is the send path; it needs only a staged element.
2. Give `nv_dma` an adopt-style constructor and convert the three remaining
   blob handoffs, so `bus` has exactly one origin everywhere a device reads.
3. On the next real boot, the rows to read are `[NV-RPC] stage=block`
   (does `pte0 == nv_dma_bus` on silicon, `identity=?`), then
   `[NV-RPC] stage=link linked=0 peer=none` — until RM publishes its header,
   that row is the honest state of the transport.
