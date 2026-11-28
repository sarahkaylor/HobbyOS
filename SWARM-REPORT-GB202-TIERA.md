# GB202 — the rm67 vendor-shape mode and the vendor diff's Tier A rows on `nv/integ-swarm`

Card `t_9630728b`. **Zero boots.** No VM was started, no GPU touched, VMs 313/315
were left alone. Everything here is source + the two unit tiers.

Predecessor: `t_240a0ac8` (one boot `rm67` on VM 313) retired the *field-value*
class and left the *sequence* class open. This card lands both halves: mode 67
(the harness that boot measured) and the six host-step rows of
`docs/gb202-vendor-diff.md` §5 Tier A that had never been carded.

Base: `8288ae63` (`nv/integ-swarm`). Branch: `nv/gb202-tier-a`.

---

## 1. Mode 67 is in the tree, and it is the mode the boot ran

The rm67 changes existed only as `nv_fsp_cot.c.mode67.diff` (plus the four
companion diffs in the same evidence tarball) against the **pre-fold** tree.
Re-applied by hand onto the folded tip:

* `nv_fw.c`, `nv_fw.h`, `nv_fw_test.c`, `nv_fsp.c` applied mechanically.
* `nv_fsp_cot.c`: 4 of 15 hunks were moved by the harness fold (`fff4afd2`);
  those four (the mode-list print, the FMC blob name, the heap-pair guard and
  the COT size list) were re-applied against the folded text, keeping the
  fold's own mode 66 and its `FMCV.BIN` naming.

The gate is reconstruction, not a clean `patch`: `x67/*.pre67 + *.mode67.diff`
was applied to produce the booted source, and the only differences against the
folded tree are the fold's own additions (the FSP DMEM log, mode 66, the
`FMCV.BIN` blob table, the COT-block de-duplication) — no mode-67 hunk is
missing and no B-row value moved.

`tools/nv/check67.py` (committed with the vendor layout measurer and its
captured offsets): **25 assertions, 0 failures** on the folded tree, including
the compiled 580.178.04 header (`sizeof(GspFwWprMeta)=256`, `pmuReservedSize`
at `0x0F4`, `verified` at `0x0F8`, `nonWprHeapSize` at `0x068`).

Mode 67 resolves `FMCV.BIN` → the 216,204-byte 580.178.04 container and
`BOOTIMG.BIN` → the 217,088-byte GspRmBoot image, and gates the container size.
`git grep` for `mode == 67` is non-zero in `nv_fsp_cot.c` (mode list, blob
table, size gate, heap pair, pmu field, COT FRTS pair, args target, wire rows).

---

## 2. Tier A rows

`docs/gb202-vendor-diff.md` §5 Tier A is the source. Every row landed with the
unit proof that table names; `tools/nv/check_tiera.py` is that proof as a gate
(**30 assertions, 0 failures**).

| row | vendor | what changed | unit case | log row (verbatim from the green run) |
|---|---|---|---|---|
| **A3** FALCON_OS after lockdown release | `kernel_gsp_gh100.c:955-956` | `nv_gsp_write_falcon_os()` writes `NV_PFALCON_FALCON_OS` (0x110080) with the boot descriptor's `appVersion` and reads it back; the FSP path calls it at the first lockdown-released sample with MAILBOX0 still 0, before the link | `test_nv_gsp_falcon_os_write_read_back` | `[NV-GSP] falcon_os write=0x0x000…0000 read=0x0x000…0000 app_version_src=580.178.04+0x1C` |
| **A4** one-shot status-queue link | `message_queue_cpu.c:317-382` (msgqRxLink); `msgq.c:331-338` | `nv_gsp_rpc_peer_link_into()` links once and latches `rx_linked`; a second call returns 0 without touching the read pointer (the vendor's `rxLinked` rule) | `test_nv_gsp_peer_link_is_one_shot` | `[NV-RPC] peer link linked=1 swapped=1 … (GSP-RM wrote the status header; readPtr written once)` then `… one-shot (already linked; no second link, readPtr untouched)` |
| **A5** GPU time before the COT | `kernel_gsp.c:4145`; `timer_gh100.c:49-100` | `nv_gsp_set_gpu_time()` reads the GC6 secure timer hi-lo-hi, subtracts it from the boot-anchored wall clock, writes `SYS_TIMER_OFFSET_1` then `_0|UPDATE` when the FSP ucode is ≥ 0x44C (bug 3957833); 0x44B records without writing | `test_nv_gsp_time_offset_conversion`, `test_nv_gsp_time_write_gated_on_fsp_version` | `[NV-GSP] gpu-time sec=… wall_ns=… offset_ns=… fsp_ucode=0x0x000…044C wrote=1` and `… sys_timer_offset rb_hi=… rb_lo=…`; the 0x44B control prints `wrote=0` |
| **A7** the two pre-boot RPCs | `kernel_gsp.c:3792-3815` (kgspQueueAsyncInitRpcs) | `nv_gsp_rpc_queue_async_init()` commits SET_SYSTEM_INFO (72) then SET_REGISTRY (73) into the command queue after the block is published and before the COT, **without a doorbell**; payload sizes are the vendor's (888 and 8) | `test_nv_gsp_async_init_queues_two_rpcs` | `[NV-RPC] async-init queue elements=2 writePtr=2 …`; `async-init[0] fn=72 len=920 elem=1`; `async-init[1] fn=73 len=40 elem=1` |
| **A9** drop the pre-COT mailbox write | absent on the FSP path (D13) | the `MAILBOX0/1 = args address` writes are gone from `nv_fsp_rm_boot()`; the read-back row stays and prints the pair plus `wrote=0` | source gate in `check_tiera.py` (row is a removal; no helper) | on a boot: `[NV-FSPCOT] gsp mailbox pre-cot args=… m0=… m1=… wrote=0 landed=0` |
| **A10** stop writing the `0x4F4/8/C` alias | `kern_fsp_gh100.c:1646-1679` (only 0x14F4/8/C) | the three `NV_PBUS_ZB_FRTS_*` writes are gone; the 0x14F4/8/C pair is programmed and both read-back sets stay, with `wrote_zb=0` | source gate in `check_tiera.py` | on a boot: `[NV-FSPCOT] frts regs lo=… wrote_zb=0 gh100_rb_lo=… gb202_rb_lo=0xBADF5040 …` |

A9/A10 are removals: the reads that fed the old log rows are kept, so the rows
survive as *observations*; only the writes the vendor never makes are gone.
Neither is behind a mode — no mode-58 regression is measurable without a boot,
and the card's zero-boot constraint forbids one here. If the next boot shows a
58-class regression, the two blocks are small enough to gate on a mode.

**A3's value is 0, and that is the vendor's own.** 580.178.04's GB202 PROD
descriptor (`g_gsp_gb20x_dyn_prd_desc.bin`: 28 raw-deflate bytes → 84 bytes,
label `UCODE_DESC_PROD`) has `version 5` at `+0x00` and `appVersion 0` at
`+0x1C`. The writer is still exercised with a non-zero value in the unit case,
and a write that does not land returns `-EIO`, so the row is not vacuous.

**A7's limits, stated.** The system-info payload fills the fields this driver
knows (BAR0/1/3 physical addresses, the PCI bus/slot/function identity, the
user-VA limit, the config mirror); ACPI method data, the VF block and the
hypervisor words are zero (bare-metal, non-VF, no-ACPI client). The registry
table is structurally valid with `numEntries=0`: reproducing the vendor's
private key list is not something the sources in this tree support, and the row
prints the table as sent. Both lengths are the vendor's `sizeof(GspSystemInfo)`
(888, measured by compiling `gsp_static_config.h:143-172` with the RM types and
the nested ACPI structs, `MAX_DISPLAYS=16`) and `sizeof(PACKED_REGISTRY_TABLE)`
(8).

No B-row value was changed: B1 (`0x1C00000` / 1 MiB), B2 (`0x1920000`), B3
(target 2) and A1 (`0x230000`) are exactly the numbers `rm67` booted.

---

## 3. Both tiers, from this tree

Host fact: `/dev/kvm` is `root:kvm` and this worker's uid is not in `kvm`, so
the intel runner is invoked with `SKIP_KVM=1` (TCG). That is an environment
fact, not a code change; the tier is the same suite.

| tier | command | result |
|---|---|---|
| intel | `SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 bash run_unit_tests_intel.sh` | **Tests run 616 / Tests failed 0 / UNIT TESTS PASSED** (baseline `8288ae63`: 611/0; +5 cases) |
| ARM | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 bash run_unit_tests.sh` (ARCH=arm) | **Tests run 486 / Tests failed 0 / UNIT TESTS PASSED** (baseline `8288ae63`: 486/0; the five new cases are x86-64-only in `nv_gsp_glue_test.c`, so the ARM count is unchanged by design) |

Added cases (5): `test_nv_gsp_falcon_os_write_read_back` (A3),
`test_nv_gsp_peer_link_is_one_shot` (A4),
`test_nv_gsp_time_offset_conversion` and
`test_nv_gsp_time_write_gated_on_fsp_version` (A5),
`test_nv_gsp_async_init_queues_two_rpcs` (A7). A9/A10 are removals and are
covered by `check_tiera.py`, not by a runtime case.

---

## 4. Gates

```
python3 tools/nv/check67.py     # 25 assertions, 0 failures (mode 67, A1/A2/B1/B2/B3)
python3 tools/nv/check_tiera.py # 30 assertions, 0 failures (A3/A4/A5/A7/A9/A10)
```

`check67.py` compiles 580.178.04's `gsp_fw_wpr_meta.h` when a C compiler is
present and falls back to the captured offsets file otherwise, so it runs on a
bare checkout.

---

## 5. What this card did not do

* No boot, no VM start, no host change. The A9/A10 boot rows and the A3/A5/A7
  rows on real silicon are *unit-model* rows here; the boot-path call sites are
  in place and print the same rows.
* No re-spend of the four B-row values, the 615/620 FMC class, any hoster/ACR
  blob, any FSP staging, or `docs/gb202-rm-parked.md` §5.
* The `590.48.01` class point remains a separate decision
  (`docs/gb202-rm-objects.md` §13.5); this card does not authorise it.

## 6. Push

`nv/integ-swarm`, fast-forward from `8288ae63`, no force-push. The pushed tip
sha is stated in the card comment and in the completion metadata; by
construction a file cannot contain the sha of the commit that adds it, so the
report names the branch and the card carries the number.
