# WP-TRANSFER — a submitted point moves between two timelines

Card: `t_b227267a`. Base: `nv/integ-swarm` @ `95bae60f` (the WP-SYNCFILE fold,
i.e. WP-PRIME + WP-SYNC + SYNCOBJ_QUERY + the sync_file object). Branch:
`nv/wp-transfer`. Shim + its DRM test suite only: no GPU boot, no VM boot, no
`docs/nv-plan.md` edit.

## 1. What this rung serves, and what it does not

Of the seven syncobj rows, six were served before this card and two were refused
**by number**: `SYNCOBJ_TRANSFER` (`0xcc`) and `SYNCOBJ_EVENTFD` (`0xcf`).
`TRANSFER`'s handle→handle leg needs no sync_file at all — two timelines, no fd —
so it was the last reachable piece of the syncobj surface and it is now served
for the timeline form:

| row | form | nr | before | after |
|---|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | `TRANSFER_FLAGS_TIMELINE`, handle→handle | `0xcc` | `-ENOSYS`, reason `syncobj_transfer` | **served** — the destination's wait observes the source's retire |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | no `TRANSFER_FLAGS_TIMELINE` (binary) | `0xcc` | `-ENOSYS` (whole row) | `-EINVAL`, reason `transfer_needs_timeline` |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | source handle from `IMPORT_SYNC_FILE` | `0xcc` | `-ENOSYS` (whole row) | `-EINVAL`, reason `transfer_src_sync_file` (§2) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | — | `0xcf` | `-ENOSYS` by name | **unchanged** (docs/nv-shim-syncfile.md §5.2) |

The row left `g_drm_refusals[]` (`src/kernel/nv_shim.c:1533` is now EVENTFD
alone) and joined `g_drm_rows[]` at drm.h's width
(`src/kernel/nv_shim.c:1494`, `{0xcc, 32}`). Serving a row takes both edits: a
refusal is answered before any argument is read, and a served row is
size-checked.

## 2. The uAPI: 32 bytes, pinned

`struct nvshim_drm_syncobj_transfer` (`src/include/nv_drm_uapi.h:290-297`) is
drm.h's `struct drm_syncobj_transfer`, field for field:

```c
struct nvshim_drm_syncobj_transfer {
  uint32_t src_handle;   /* 0 */
  uint32_t dst_handle;   /* 4 */
  uint64_t src_point;    /* 8 */
  uint64_t dst_point;    /* 16 */
  uint32_t flags;        /* 24: 0 or TRANSFER_FLAGS_TIMELINE */
  uint32_t pad;          /* 28: must be 0 */
};                       /* 32 bytes */
```

The macro now derives its encoding from the struct
(`src/include/nv_drm_uapi.h:590-592`) and still produces `0xC02064CC` — the same
value the old refusal-only `ANY(nr, 32)` produced, so a probe written against
the previous header is a request a real client could have sent. The layout test
asserts the size, all six offsets and the literal encoding
(`src/kernel/nv_shim_drm_test.c:88`, `:107-114`, `:162`). A size mismatch is the
one way this row can be "served" and still wrong for every winsys built against
`drm.h`; it is now checked at both ends — the kernel's `drm_row_size()` equality
(`src/kernel/nv_shim.c:1494`) and the tier's literals.

`NVSHIM_DRM_SYNCOBJ_TRANSFER_FLAGS_TIMELINE` is `1u << 0`
(`src/include/nv_drm_uapi.h:299`), drm.h's only flag for this request.

## 3. What a served transfer means — and why it is not a copy

A record here is ONE point, not a chain head (`docs/nv-shim-sync.md` §3), so a
transfer cannot carry a Linux `dma_fence` across. What it can carry is the
identity of the completion event, and the pending table already holds exactly
that: `(record, channel, fence, point)`
(`struct shim_sigpend`, `src/kernel/nv_shim.c:70-75`).

So `op_drm_syncobj_transfer` (`src/kernel/nv_shim.c:3977-4099`):

1. calls `client_reap_locked()` first (`src/kernel/nv_shim.c:3334`) — the one
   completion path, exactly as a wait does, so a fence that retired while nobody
   was looking is visible to this decision;
2. checks the source point against the record's **submitted ledger**
   (`sobj_submitted_locked()`, `src/kernel/nv_shim.c:3327`);
3. if the source point is not yet reached, finds the pending entry that will
   land it and gives the **destination its own entry with its own point on the
   same (channel, fence)**;
4. writes the destination's `submitted` ledger at commit time, and moves the
   destination's reached `point` only when the fence retires.

That is what makes "the same retire satisfies src and dst" a property of the
code rather than of the test: there is one `fence_test()` pass per reconnect and
it lands both entries. It is also why a transfer answering 0 does **not** make
the destination's wait succeed — the suite asserts exactly that with the mock's
hold (`nv_rm_mock_hold(1)`) before releasing the kick.

If the source point is **already reached** (a `TIMELINE_SIGNAL` submits and
signals at once, or the retire happened before this call), no entry is created:
the destination reaches its point inside the transfer, which is Linux's
`drm_syncobj_add_point` of an already-signalled fence.

The row names both handles, both points, both object serials and whether it
chained or found the source already reached:

```
[NVSHIM] drm syncobj transfer src=1 dst=2 src_point=0x9 dst_point=0x9 obj=16->17 chained=1 src_at=0x0
```

## 4. What is refused, each by its own name

`nv_shim.h` gained reasons 15–19 (`src/include/nv_shim.h:432-436`), rendered by
`k_sync_reasons[]` (`src/kernel/nv_shim.c:3390-3394`):

| reason | errno | when | why its own name |
|---|---|---|---|
| `transfer_needs_timeline` (15) | `-EINVAL` | `flags` has no `TRANSFER_FLAGS_TIMELINE` | that is Linux's BINARY transfer — it moves a whole fence pointer, which a point-per-record node does not have |
| `transfer_point_unsubmitted` (16) | `-EINVAL` | `src_point > source.submitted` | nothing was ever handed that point; there is no fence to chain to (`drm_syncobj_find_fence`'s `-EINVAL`) |
| `transfer_point_not_pending` (17) | `-EINVAL` | submitted, not reached, and no live pending entry | different cause (the entry was dropped with its handle), same "would wait forever" outcome — so a different name |
| `transfer_handle_not_held` (18) | `-EINVAL` | a handle this client does not hold | a never-created number and a DESTROYED handle are the same absence here: the handle table recycles numbers, so no tombstone could separate them. The row names the absence instead of borrowing a sync_file name |
| `transfer_src_sync_file` (19) | `-EINVAL` | the source handle came from `IMPORT_SYNC_FILE` | that transfer carries a sync_file's fence, i.e. the sync_file leg of this row — still a separate rung |
| `timeline_point_zero` (5) | `-EINVAL` | `src_point == 0` or `dst_point == 0` | shared with the other timeline rows on purpose: 0 is not a point in this node anywhere |
| `bad_flags` (4) | `-EINVAL` | any other flag bit | the same rule as every other row |
| `bad_shape` (9) | `-EINVAL` | `pad != 0` | the same rule as every other row |
| `timeline_point_order` (6) | `-EINVAL` | `dst_point <= destination.submitted` | the destination may not go back in time — the rule `TIMELINE_SIGNAL` applies to its own point |
| `no_syncobj_slot` (8) | `-ENOSPC` | the pending table is full | the destination would never be signalled |

Every one of those is measured, including 17: the refusal suite submits point 7,
destroys the source handle while a syncobj fd keeps the record alive (which drops
the pending entry but keeps the ledger), re-imports the record and transfers
from it. Reason 17 is what prints.

## 5. The rows get their own budget

`r_synctransfer = 48` (`src/kernel/nv_shim.c:431`) is used by every row this rung
prints — the served leg, the already-reached leg and all ten refusal rows — via
`sync_refuse_rows()` (`src/kernel/nv_shim.c:3419`), a `sync_refuse()` that takes
the bucket. They run after five syncobj suites have spent most of `r_sync`, and
a green tier with no `transfer src=` row is the silent-budget failure this
project has already paid for twice. Both logs carry 17 such rows — 3 served
transfers and 14 refusals (`grep -c 'refuse transfer\|syncobj transfer src='
tier-*.log`); see §6.1.

## 6. Measured

Both tiers ran in this card's worktree, in one round, on the code commit named in
the completion result. No KVM: `/dev/kvm` is not readable by this uid, so both
tiers are TCG (`SKIP_KVM=1` on intel; ARM is TCG always).

| tier | invocation | baseline (tip `95bae60f`) | this rung | delta |
|---|---|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `Tests run: 602 / Tests failed: 0` | `Tests run: 604 / Tests failed: 0 / UNIT TESTS PASSED` | **+2** |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` | `Tests run: 481 / Tests failed: 0` | `Tests run: 483 / Tests failed: 0 / UNIT TESTS PASSED` | **+2** |

The baseline is the parent fold card's own attached logs
(`unit-tier-intel-merged.log:2971-2973` = 602/0, `unit-tier-arm-merged.log:2376-2378`
= 481/0, both at `95bae60f`). The delta is the two new suites —
`test_nvshim_drm_syncobj_transfer` (served) and
`test_nvshim_drm_syncobj_transfer_refusals` — one per arch each.

ARM reloc gate, quoted from the ARM log line 14:

```
hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

### 6.1 The rows that carry the acceptance (verbatim, intel log; selected rows —
the sync_file export/import rows of refusal case (g) sit between 2683 and 2686)

```
2658:[NVSHIM] drm syncobj transfer src=1 dst=2 src_point=0x9 dst_point=0x9 obj=16->17 chained=1 src_at=0x0
2661:[NVSHIM] drm syncobj transfer src=1 dst=2 src_point=0xc dst_point=0xc obj=16->17 src_reached=1 src_at=0xc
2671:[NVSHIM] drm syncobj refuse transfer flags=0x0 reason=transfer_needs_timeline
2672:[NVSHIM] drm syncobj refuse transfer flags=0x81 reason=bad_flags
2673:[NVSHIM] drm syncobj refuse transfer flags=0x80000001 reason=bad_flags
2674:[NVSHIM] drm syncobj refuse transfer src_point=0x1 reason=transfer_point_unsubmitted
2675:[NVSHIM] drm syncobj refuse transfer src_point=0x5 reason=transfer_point_unsubmitted
2676:[NVSHIM] drm syncobj transfer src=1 dst=2 src_point=0x4 dst_point=0x4 obj=18->19 src_reached=1 src_at=0x4
2677:[NVSHIM] drm syncobj refuse transfer handle=0x3f0 reason=transfer_handle_not_held
2678:[NVSHIM] drm syncobj refuse transfer handle=0x4 reason=transfer_handle_not_held
2679:[NVSHIM] drm syncobj refuse transfer handle=0x4 reason=transfer_handle_not_held
2680:[NVSHIM] drm syncobj refuse transfer point=0x0 reason=timeline_point_zero
2681:[NVSHIM] drm syncobj refuse transfer point=0x0 reason=timeline_point_zero
2682:[NVSHIM] drm syncobj refuse transfer dst_point=0x4 reason=timeline_point_order
2683:[NVSHIM] drm syncobj refuse transfer dst_point=0x3 reason=timeline_point_order
2686:[NVSHIM] drm syncobj refuse transfer src_handle=0x4 reason=transfer_src_sync_file
2690:[NVSHIM] drm syncobj refuse transfer src_point=0x7 reason=transfer_point_not_pending
```

`chained=1 src_at=0x0` on the first row is the whole point: the source had
submitted 9 and reached 0 when the transfer ran, so the destination got a
pending entry rather than a point. The assertions around it:

| acceptance bullet | how it is measured | where |
|---|---|---|
| transfer returns 0, row names both handles and the point | `sobj_transfer(src,dst,9,9,TIMELINE) == 0`, row quoted above | `test_nvshim_drm_syncobj_transfer`, `src/kernel/nv_shim_drm_test.c:3150` |
| dst `TIMELINE_WAIT` at N is `-ETIME` before the source retires | poll **and** a real ms deadline, while the mock holds the kick; `QUERY dst` reports reached 0 / submitted 9 | same suite |
| ... and 0 after, on the SAME retire | `nv_rm_mock_hold(0)` + one unsigned EXEC; then src wait 0 **and** dst wait 0; `QUERY dst` = 9 in both modes | same suite |
| an already-reached source needs no fence | `TIMELINE_SIGNAL src@12`, then `transfer(12,12)`; dst wait 0 immediately, `QUERY dst` 12 | second served row above |
| a point not yet submitted is refused | `transfer(src,dst,5,5)` after `src` submitted 4 | reason 16 rows |
| a destroyed handle is refused | create → destroy → transfer from it (source and destination) | reason 18 rows |
| a non-timeline transfer is refused | `flags = 0` (binary form) | reason 15 row |
| nothing a refusal touched landed | `QUERY dst` in both modes still 4; `wait dst@5` still `-ETIME` | refusal suite, `src/kernel/nv_shim_drm_test.c:3292` |

### 6.2 The stale expectations this rung had to change

| site | before | after |
|---|---|---|
| `test_nvshim_drm_layout` req-size | `TRANSFER` → `0` (refused) | `32` (served), plus `ANY(0xcc,31)` → `32` |
| `test_nvshim_drm_layout` refusal name | `TRANSFER` has a refusal name | none — EVENTFD is the last one |
| `test_nvshim_drm_refusals`, `test_nvshim_drm_prime_refusals` | `TRANSFER` → `-ENOSYS` | `-EINVAL` (a served row with no process) |
| `test_nvshim_drm_syncfile`, `test_nvshim_drm_syncobj_refusals` | `TRANSFER` → `-ENOSYS` through a live client | removed; the row has its own suites now |

## 7. Divergences from Linux, stated

1. **The binary form is refused, not served.** Linux's flagless transfer moves
   `dst->fence = src->fence` — a whole fence pointer. A record here holds one
   point and no chain, so there is nothing to move; the row answers by name
   instead of being half-served (that is exactly what
   `docs/nv-shim-syncfile.md` §5.1 declined to do for the row as a whole).
2. **The source point must be in the submitted ledger.** Linux's
   `drm_syncobj_find_fence` will also take a syncobj whose chain has a fence at
   or below the requested point; here the ledger is the authority, and a point
   above it is `transfer_point_unsubmitted` rather than a silent success.
3. **A transfer is a reward for a retire, not a fence object.** The destination
   names the same `(channel, fence)` event, so a *second* transfer of the same
   source point to a third timeline chains to the same entry — fine — but the
   destination does not keep an independent copy of the source's history. A
   destination that is `RESET` while a transferred point is pending still lands
   that point later: `client_reap_locked` writes the pending point whatever the
   record's current state. That limitation is not new (an EXEC sig pending across
   a `RESET` behaves the same way, and `RESET` has never been a cancel), and it
   is stated here so nobody reads the transfer as a cancel either.
4. **Destroying the destination handle drops its pending point.** `DESTROY`
   removes the client's pending entries for that record
   (`src/kernel/nv_shim.c:3509-3516`), so a destroyed destination never reaches
   its transferred point — with the last reference gone there is nothing left to
   reach. Linux keeps the fence alive on the remaining file reference; here the
   record is refcounted by handle entries and fds, and destroying the only
   handle frees it.
5. **No `-ENOENT`.** Linux answers `-ENOENT` for a handle it cannot find;
   this node has answered `-EINVAL` with a name for every handle-shaped refusal
   since WP-SYNC, and the transfer keeps that convention (reason 18).

## 8. Verification gaps

* no Mesa/nouveau client was linked or run: the row is driven through the real
  route table, the real encoded number `0xC02064CC`, the 32-byte struct and real
  processes, but the winsys use (`vk...Semaphore`, WSI timelines) is
  source-level;
* no GPU boot: the fence is the mock RM's, retired by `kick()`, so "retire" is a
  synchronous kick rather than an interrupt; the retire path exercised is the
  same `client_reap_locked()` the timeline suite has always driven;
* `TRANSFER`'s sync_file leg (an imported fence as the source) is refused by
  name (reason 19) and is the next rung, not a half-served row;
* a destination `RESET` while a transferred point is pending still lands that
  point (see §7.3) — pre-existing model behaviour, not introduced here;
* the pending table is per client (`SHIM_MAX_SIGPEND = 32`); a transfer costs it
  one slot, and a full table answers `-ENOSPC` (`no_syncobj_slot`) rather than
  silently dropping the destination.
