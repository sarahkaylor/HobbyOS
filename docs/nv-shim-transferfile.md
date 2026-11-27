# WP-TRANSFERFILE — a point imported from a sync_file moves to a second timeline

Card: `t_9daa63c3`. Base: `nv/integ-swarm` @ `8e33b361` (the WP-TRANSFER fold).
Branch: `nv/wp-transferfile`. Shim + its DRM test suite + docs only: no GPU
boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What this rung serves

`DRM_IOCTL_SYNCOBJ_TRANSFER` (`0xcc`) has been served since WP-TRANSFER for two
plain timeline handles. The one reachable form left is the **sync_file source**:
a handle created by `IMPORT_SYNC_FILE` used as `src_handle`. It was refused by
name (reason 19) and is now served, at the only point such a handle can name.

| row | form | nr | before | after |
|---|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | source handle from `IMPORT_SYNC_FILE`, `src_point` == its floor | `0xcc` | `-EINVAL`, reason `transfer_src_sync_file` (blanket) | **served** — the destination's wait observes the file's fence |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | same handle, `src_point` **below** its floor | `0xcc` | `-EINVAL`, reason `transfer_src_sync_file` | `-EINVAL`, reason `transfer_point_below_import` (new, 20) |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | same handle, `src_point` **above** its floor while still submitted | `0xcc` | `-EINVAL`, reason `transfer_src_sync_file` | `-EINVAL`, reason `transfer_src_sync_file` (19, narrowed meaning) |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | same handle, `src_point` above the submitted ledger | `0xcc` | reason `transfer_point_unsubmitted` | **unchanged** (16) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | — | `0xcf` | `-ENOSYS` by name | **unchanged** (docs/nv-shim-syncfile.md §5.2) |

No route-table or width change was needed: `0xcc` already left
`g_drm_refusals[]` in WP-TRANSFER and is in `g_drm_rows[]` at drm.h's width
(`{0xcc, 32}`). The encoded number is still `0xC02064CC`
(`src/include/nv_drm_uapi.h:590-592`), still pinned by
`test_nvshim_drm_layout` (`src/kernel/nv_shim_drm_test.c:85-126,159`), and the
new suite drives the row through that macro and the real route table.

## 2. What "served" means for an imported source

`IMPORT_SYNC_FILE` stores the point it installed in `c->sobj_floor[]`
(`src/kernel/nv_shim.c:4419`, `:3474`): the exported file's point for a binary
import, or the explicit `TIMELINE` point for a timeline import. An ordinary
handle has floor `0` (`src/kernel/nv_shim.c:3474`). That floor is the **only**
point the handle can name, so the served form is exactly

```
src_point == floor
```

and the frozen transfer machinery does the rest (`op_drm_syncobj_transfer`,
`src/kernel/nv_shim.c:3991`):

1. `client_reap_locked()` runs first, so a fence that retired while nobody was
   looking is visible to this decision;
2. the ledger check (`src_point > src->submitted`) runs **for every handle**
   before the floor checks, so `transfer_point_unsubmitted` (16) keeps its
   meaning;
3. then the floor decides: below → reason 20; above (and still inside the
   ledger) → reason 19; equal → served;
4. if the record has **already reached** the floor, no entry is created: the
   destination reaches `dst_point` inside the ioctl (`src->point >= src_point`,
   the same branch a plain transfer takes for an already-signalled fence);
5. if the record is **pending** at the floor, the destination gets its own
   pending entry with its own point on the **same** `(channel, fence)` — and,
   for an imported source, only on the entry whose point **is** the floor
   (`src/kernel/nv_shim.c:4074-4086`). A later entry is a different fence; when
   the floor's entry is gone, reason 17 is the honest answer.

`client_reap_locked()` is still the one completion path, so "the same retire
satisfies the file's fence and the destination" is a property of the code, not
of the test. The row says which shape it took:

```
[NVSHIM] drm syncobj transfer src=3 dst=2 src_point=0x9 dst_point=0x9 obj=18->19 chained=1 src_at=0x0
[NVSHIM] drm syncobj transfer src=3 dst=4 src_point=0x9 dst_point=0xd obj=18->20 src_reached=1 src_at=0x9
[NVSHIM] drm syncobj transfer src=6 dst=5 src_point=0xc dst_point=0xc obj=21->22 src_reached=1 src_at=0xc
```

The first is shape (1) of the suite: source handle 3 is the `IMPORT_SYNC_FILE`
handle, the record had submitted 9 and reached 0, so the destination got a
chained pending entry (`chained=1 src_at=0x0`). The second is the same handle
after the retire: the floor is reached, so the destination was signalled inside
the call. The third is shape (3): a file exported **after** `TIMELINE_SIGNAL`
and imported with an explicit `TIMELINE` point (12) — the destination's wait
returns immediately.

## 3. The refusal names

`nv_shim.h` gains reason 20 (`src/include/nv_shim.h:428-443`), rendered by
`k_sync_reasons[]` (`src/kernel/nv_shim.c:3373-3397`). Reason 19 is kept and
narrowed; the numbers are part of the log contract, so nothing is renumbered.

| reason | errno | when | why its own name |
|---|---|---|---|
| `transfer_point_below_import` (20) | `-EINVAL` | imported source, `src_point < floor` | the point never had a fence on this record; chaining it to the floor's entry would move the WRONG fence (and report "reached" for a point that was never submitted) — refused by name, never clamped |
| `transfer_src_sync_file` (19) | `-EINVAL` | imported source, `src_point > floor` and `src_point <= submitted` | the handle carries exactly one fence (its floor); a later point the record has submitted since is not the fence this handle names |
| `transfer_point_unsubmitted` (16) | `-EINVAL` | `src_point > source.submitted` (any handle, and checked first) | unchanged from WP-TRANSFER |
| `transfer_point_not_pending` (17) | `-EINVAL` | submitted, not reached, no live entry | unchanged; now also the shape where a sync_file fd kept the record alive after the submitting handle was destroyed (`src/kernel/nv_shim_drm_test.c:3578-3631`) |
| `transfer_needs_timeline` (15) | `-EINVAL` | binary transfer | unchanged |
| `transfer_handle_not_held` (18) | `-EINVAL` | an unheld / destroyed handle | unchanged |
| `timeline_point_zero` (5), `bad_flags` (4), `bad_shape` (9), `timeline_point_order` (6), `no_syncobj_slot` (8) | as before | as before | unchanged |

## 4. The per-client pending table — the one limitation, stated

The pending table is per client and keyed by record
(`struct shim_sigpend`, `src/kernel/nv_shim.c:70-75`; the transfer's lookup is
`src/kernel/nv_shim.c:4074-4086`). The served pending shape therefore requires
the `IMPORT_SYNC_FILE` to happen on the **same** `/dev/nvshim` description that
submitted the fence: a second client importing the same `sync_file` fd sees the
record's ledger but not the first client's pending entry, and gets reason 17.
That is the existing model (a plain transfer between handles of two different
clients has the same property), not a new wall; the suite imports and transfers
through one client, which is how a winsys uses a sync_file it just imported.

## 5. The rows get their own budget

Every row this rung prints — 3 served transfers and 1 new refusal in the new
suite, plus the narrowed 19 and the imported-source 17 in the refusal suite —
uses `r_synctransfer = 48` (`src/kernel/nv_shim.c:431`) through
`sync_refuse_rows()` (`src/kernel/nv_shim.c:3421`). Both tier logs carry
**23** transfer rows (6 served, 17 refused; WP-TRANSFER's 17 were 3 served and
14 refused), and both carry the new `transfer src=` and
`reason=transfer_point_below_import` rows. A green tier without them is the
silent-budget failure this project has already paid for twice.

## 6. Measured (both arches, one round)

| tier | invocation | baseline (fold `8e33b361`) | this rung | delta |
|---|---|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh` | `Tests run: 604 / Tests failed: 0` | `Tests run: 605 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` | `Tests run: 483 / Tests failed: 0` | `Tests run: 484 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |

The delta is `test_nvshim_drm_syncobj_transfer_syncfile`
(`src/kernel/nv_shim_drm_test.c:3295`), one suite per arch. The refusal rows
and the reason-20 name assertion were added to the existing
`test_nvshim_drm_syncobj_transfer_refusals` suite, so they spend no extra test
count. No KVM (`SKIP_KVM=1`; `/dev/kvm` is not readable by this uid).

ARM reloc gate, ARM log line 14:

```
hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

### 6.1 The rows (verbatim; intel line numbers, ARM in brackets)

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

The five existing transfer refusal reasons still print in both logs: 15 at
intel `:2705`, 16 at `:2708/:2709/:2722`, 17 at `:2727/:2729`, 18 at
`:2711-2713`, 19 at `:2721` (the narrowed above-the-floor form).

| acceptance bullet | how it is measured | where |
|---|---|---|
| the row is driven through the real route table and `0xC02064CC` | `drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER, &tf, 32)` with the header's macro; the layout suite pins the encoding | new suite + `test_nvshim_drm_layout` |
| pending source: dst `TIMELINE_WAIT` returns only after the file's fence retires | `-ETIME` as a poll **and** with a real ms deadline while the mock holds the kick; `0` after the same unsigned EXEC that satisfies `src` and `imp` | new suite §(1)(2) |
| already-retired source: dst wait returns immediately | `TIMELINE_SIGNAL` at 12 → export at 12 → import with `TIMELINE` 12 → transfer; dst wait 0, `QUERY` reached 12 | new suite §(3) |
| a `src_point` below the floor is refused by its own name | `transfer(imp, dst, 1, 5)` with floor 2 → `-EINVAL`, row `reason=transfer_point_below_import`; name asserted with `sync_name_is` | refusal suite §(g) |
| the five existing refusal rows still print | rows 15-19 quoted above, same bucket `r_synctransfer` | both logs |
| nothing a refusal touched landed | `QUERY dst` still 4 in both modes after the batch | refusal suite tail |

### 6.2 The stale expectations this rung had to change

| site | before | after |
|---|---|---|
| `test_nvshim_drm_syncobj_transfer_refusals` §(g) | one blanket `transfer_src_sync_file` row from a never-signalled binary import | three rows: below the floor (20), above it (19), above the ledger (16) |
| `nv_shim.h` reason 19 comment | "the source handle came from IMPORT_SYNC_FILE ... separate rung" | narrowed to "src_point above the imported floor"; reason 20 added |
| `docs/nv-shim-transfer.md` §1, §4, §8 | the sync_file source as a refused, future rung | served at the floor; see this doc |

## 7. Divergences from Linux, stated

1. **Only the floor is transferable from an imported handle.** Linux's
   `drm_syncobj_find_fence(src, point)` walks the timeline chain; an imported
   sync_file installs one fence at one point, and this node serves exactly that
   point. Below it there is no fence (20), above it there is a later fence the
   handle does not carry (19).
2. **Still no fence object.** The destination names the same
   `(channel, fence)` event, not an independent copy of the file's fence; this
   is the WP-TRANSFER model (`docs/nv-shim-transfer.md` §7.3) and applies
   unchanged.
3. **The pending entry must be this client's.** See §4.
4. **`SYNCOBJ_EVENTFD` (`0xcf`) stays refused** — no kernel-side eventfd writer
   exists (`docs/nv-shim-syncfile.md` §5.2). Not this rung.
5. **The binary transfer stays refused** (reason 15): a point-per-record node
   has no fence pointer to move.

## 8. Verification gaps

* no Mesa/nouveau client was linked or run: the row is exercised through the
  real route table, the real encoded number, the 32-byte struct and real
  processes, but the winsys use is source-level;
* no GPU boot and no VM boot: the fence is the mock RM's, retired by one
  unsigned EXEC, so "retire" is a synchronous kick, not an interrupt (the same
  `client_reap_locked()` path the timeline suite has always driven);
* `EXPORT_SYNC_FILE` does not require the exported point to be submitted
  (`src/kernel/nv_shim.c:4219-4255` checks shape, not the ledger), so a file
  exported at a never-submitted point imports with a floor that can only answer
  reason 16; the suite measures the two states a client actually produces
  (pending, already retired);
* the cross-client pending-table shape (§4) is stated, not measured: the suite
  uses one client, and a second-client import would be a new fixture.
