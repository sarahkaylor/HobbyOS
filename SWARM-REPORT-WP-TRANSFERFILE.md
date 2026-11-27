# SWARM-REPORT-WP-TRANSFERFILE — the sync_file source leg of SYNCOBJ_TRANSFER (t_9daa63c3)

Lane: `nv-card`. Card: `t_9daa63c3`. Base `nv/integ-swarm`: `8e33b361` (the
WP-TRANSFER fold). Branch: `nv/wp-transferfile`. Code commit: `6acc1bf2`.
No GPU boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What landed

`SYNCOBJ_TRANSFER` (`0xcc`) now serves its **sync_file source** leg: a handle
created by `IMPORT_SYNC_FILE` can be `src_handle`, and the destination timeline
reaches `dst_point` when the fence that sync_file names retires — not when the
ioctl returns. That was the last reachable form of the row: WP-TRANSFER served
the plain timeline handle→handle leg, and it refused this one by name.

An imported handle has exactly one point, the floor `IMPORT_SYNC_FILE` stored
in `c->sobj_floor[]` (the exported file's point for a binary import, or the
explicit `TIMELINE` point). So the served form is `src_point == floor`:

* record already reached at the floor → the destination reaches `dst_point`
  inside the ioctl (`drm_syncobj_add_point` of an already-signalled fence);
* record pending at the floor → the destination gets its own pending entry on
  the **same `(channel, fence)`**, and `client_reap_locked()` lands both in one
  pass. For an imported source the lookup requires the entry whose point **is**
  the floor: a later entry is a different fence.

The ledger check now runs **before** the floor checks, so `src_point >
submitted` keeps reason 16 for every handle; a point **below** the floor is its
own new name, reason 20 `transfer_point_below_import` (no silent clamp); a point
**above** the floor but still inside the ledger keeps reason 19
`transfer_src_sync_file`, narrowed to "the handle carries exactly one fence".

| file | Δ | what |
|---|---|---|
| `src/include/nv_shim.h` | +10/−3 | reason 19 re-documented as the above-the-floor form; reason 20 added |
| `src/kernel/nv_shim.c` | +49/−11 | the floor/ledger ordering in `op_drm_syncobj_transfer`, the exact-point chain for imported sources, `"transfer_point_below_import"` in `k_sync_reasons[]` |
| `src/kernel/nv_shim_drm_test.c` | +225/−12 | new `test_nvshim_drm_syncobj_transfer_syncfile` (pending, reached, explicit-TIMELINE floor); refusal suite §(g) rewritten for 20/19/16, §(h) gains the imported-source 17 |
| `docs/nv-shim-transfer.md` | +5/−3 | the WP-TRANSFER doc's forward-looking rows now point at the served leg |
| `docs/nv-shim-transferfile.md` | +201/−0 | this rung's doc |

Source+tests: 3 files, **+284/−26**. Total including docs: 5 files,
**+490/−29** (`git show --numstat 6acc1bf2`).

No route-table edit: `0xcc` has been in `g_drm_rows[]` at drm.h's width since
WP-TRANSFER, and the encoded number is still `0xC02064CC`, pinned by the layout
suite. The new suite drives the row through the real route table with that
macro.

## 2. The acceptance, measured

Both tiers ran in this card's worktree, in one round, TCG (`SKIP_KVM=1`; this
uid is not in group `kvm`):

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

| tier | fold baseline `8e33b361` | this rung | delta |
|---|---|---|---|
| intel (x86_64) | `Tests run: 604 / Tests failed: 0` | **`Tests run: 605 / Tests failed: 0 / UNIT TESTS PASSED`** (`unit-tier-intel.log:3027-3029`) | **+1** |
| aarch64 | `Tests run: 483 / Tests failed: 0` | **`Tests run: 484 / Tests failed: 0 / UNIT TESTS PASSED`** (`unit-tier-arm.log:2432-2434`) | **+1** |

The delta is `test_nvshim_drm_syncobj_transfer_syncfile`, one per arch. The new
refusal rows and the reason-20 name assertion went into the existing
`test_nvshim_drm_syncobj_transfer_refusals` suite, so they spend no extra test
count. The baselines are the parent fold card's own logs (intel 604/0 at
`attachments/t_e2620813/unit-tier-intel-merged.log:3003-3005`, ARM 483/0 at
`unit-tier-arm-merged.log:2409-2411`).

ARM reloc gate (`unit-tier-arm.log:14`):

```
hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

### 2.1 The transfer rows (both logs; intel line [ARM line])

```
2692 [2126] [NVSHIM] drm syncobj transfer src=3 dst=2 src_point=0x9 dst_point=0x9 obj=18->19 chained=1 src_at=0x0
2693 [2127] [NVSHIM] drm syncobj transfer src=3 dst=4 src_point=0x9 dst_point=0xd obj=18->20 src_reached=1 src_at=0x9
2697 [2131] [NVSHIM] drm syncobj transfer src=6 dst=5 src_point=0xc dst_point=0xc obj=21->22 src_reached=1 src_at=0xc
2720 [2154] [NVSHIM] drm syncobj refuse transfer src_point=0x1 reason=transfer_point_below_import
2721 [2155] [NVSHIM] drm syncobj refuse transfer src_handle=0x5 reason=transfer_src_sync_file
2722 [2156] [NVSHIM] drm syncobj refuse transfer src_point=0x5 reason=transfer_point_unsubmitted
2727 [2161] [NVSHIM] drm syncobj refuse transfer src_point=0x7 reason=transfer_point_not_pending
2729 [2163] [NVSHIM] drm syncobj refuse transfer src_point=0x7 reason=transfer_point_not_pending
```

The source handles in the first three rows are the `IMPORT_SYNC_FILE` handles:
row 2692 is the pending shape (`chained=1 src_at=0x0` — the record had submitted
9 and reached nothing), row 2693 the same handle after the retire (the floor is
reached, so the destination is signalled inside the call), row 2697 the
explicit-TIMELINE import of a file exported after `TIMELINE_SIGNAL`.

Both logs carry **23** transfer rows — 6 served, 17 refused — under the same
bucket `r_synctransfer = 48` (`src/kernel/nv_shim.c:431`) through
`sync_refuse_rows()` (`src/kernel/nv_shim.c:3421`). All five existing transfer
refusal reasons still print in both: 15 at intel `:2705`, 16 at `:2708/:2709/:2722`,
17 at `:2727/:2729`, 18 at `:2711-2713`, 19 at `:2721`. The new
`reason=transfer_point_below_import` appears once, and the three `transfer src=`
rows of the imported source appear in both. A green tier without them would be
the silent-budget failure this project has paid for twice.

| acceptance bullet | how it is measured |
|---|---|
| real route table + `0xC02064CC` | `drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER, &tf, 32)` with the header macro; layout suite still pins size/offsets/encoding |
| pending source: dst wait returns only after the file's fence retires | `-ETIME` as a poll **and** with a real ms deadline while `nv_rm_mock_hold(1)` holds the kick; `0` after the same unsigned EXEC that satisfies `src`, `imp` and `dst` |
| already-retired source: dst wait returns immediately | `TIMELINE_SIGNAL` at 12, export at 12, import with `TIMELINE` 12, transfer → dst wait 0, `QUERY` reached 12 / submitted 12 |
| `src_point < floor` refused by its own name | `transfer(imp, dst, 1, 5)` with floor 2 → `-EINVAL`, row `reason=transfer_point_below_import`; name asserted by `sync_name_is` |
| five existing refusal rows still print | rows 15–19 quoted above, same bucket |
| nothing a refusal landed | `QUERY dst` still 4 in both modes after the refusal batch |

## 3. What is still refused, and why

| reason | when | note |
|---|---|---|
| `transfer_point_below_import` (20) | imported source, `src_point < floor` | the point never had a fence on this record; chaining it to the floor's entry would move the wrong fence. New this rung |
| `transfer_src_sync_file` (19) | imported source, `src_point > floor`, still submitted | narrowed: the handle carries exactly one fence |
| `transfer_point_unsubmitted` (16) | `src_point > submitted`, any handle | checked before the floor checks |
| `transfer_point_not_pending` (17) | submitted, no live entry | also measured through a sync_file fd that kept the record alive after its submitting handle was destroyed |
| `transfer_needs_timeline` (15), `transfer_handle_not_held` (18), `timeline_point_zero` (5), `bad_flags` (4), `bad_shape` (9), `timeline_point_order` (6), `no_syncobj_slot` (8) | unchanged | |

`SYNCOBJ_EVENTFD` (`0xcf`) stays refused by name (`-ENOSYS`): no kernel-side
eventfd writer exists (`docs/nv-shim-syncfile.md` §5.2). The binary transfer
stays refused (reason 15). No engine-class work was touched.

## 4. The one limitation, stated

The pending table is per client and keyed by record, so the served pending shape
requires the `IMPORT_SYNC_FILE` to happen on the same `/dev/nvshim` description
that submitted the fence. A second client importing the same `sync_file` fd
sees the ledger but not the first client's pending entry and gets reason 17 —
the same per-client property a plain transfer already had, not a new wall. The
suite imports and transfers through one client. Documented in
`docs/nv-shim-transferfile.md` §4.

## 5. Verification gaps

* no Mesa/nouveau client linked or run: real route table, real encoded number,
  32-byte struct, real processes — but the winsys use is source-level;
* no GPU/VM boot: the fence is the mock RM's, retired by one unsigned EXEC (the
  same `client_reap_locked()` path the timeline suite drives);
* the cross-client pending-table shape is stated, not measured (one client in
  the suite);
* `EXPORT_SYNC_FILE` does not require the exported point to be submitted, so a
  file exported at a never-submitted point can only ever answer reason 16; the
  suite measures the two shapes a client actually produces.

## 6. Artifacts

* `unit-tier-intel.log` — intel, `Tests run: 605 / Tests failed: 0 / UNIT TESTS PASSED`
* `unit-tier-arm.log` — aarch64, `Tests run: 484 / Tests failed: 0 / UNIT TESTS PASSED`,
  reloc gate 429 at line 14
* this report; `docs/nv-shim-transferfile.md` in the tree
