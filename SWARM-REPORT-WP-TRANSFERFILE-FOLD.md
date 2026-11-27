# SWARM-REPORT-WP-TRANSFERFILE-FOLD — WP-TRANSFERFILE folded into `nv/integ-swarm` (t_c2079388)

Lane: `nv-card`. Card: `t_c2079388`. Base `nv/integ-swarm`: `8e33b361` (the WP-TRANSFER
fold's report commit). Lane branch folded: `nv/wp-transferfile` @ `0ee98a2f` (tooltip card
`t_9daa63c3`, code `6acc1bf2` + docs `0ee98a2f`). Merge commit: **`2ba48fd3`** (parents
`8e33b361` `0ee98a2f`). No rebase, no force-push, no `docs/nv-plan.md` edit, no GPU boot,
no VM boot, no `qm` — the shim is an ABI surface and the unit tiers are its only gate.

## 1. What the merge was

`git merge-base --is-ancestor origin/nv/integ-swarm origin/nv/wp-transferfile` → YES, the
merge base is `8e33b361` itself and the lane is 2 commits ahead, 0 behind, so the content is
a fast-forward. It was landed with `--no-ff` so the fold is a named commit on the integration
branch, matching every earlier fold's shape.

| step | commit | result |
|---|---|---|
| worktree cut from `origin/nv/integ-swarm` | `8e33b361` | base |
| `git merge --no-ff -m "Merge WP-TRANSFERFILE: SYNCOBJ_TRANSFER's sync_file source leg is served" origin/nv/wp-transferfile` | **`2ba48fd3`** (parents `8e33b361` `0ee98a2f`) | **clean** (`ort`), no conflicts, nothing to resolve |
| docs commit carrying this report | see `git log` on the merged tip | report only |

`git show --no-patch --format=%P 2ba48fd3` prints
`8e33b36191cb577c1b0991b1a82820f3393c440d 0ee98a2f8c0259f9c99df9575af39b49d7a7b233`, and
`git diff --stat 0ee98a2f 2ba48fd3` is **empty** — the merged tree is byte-identical to the
lane tip, as a content fast-forward must be.

Fold footprint — `git diff --numstat 8e33b361 2ba48fd3`:

```
154     0       SWARM-REPORT-WP-TRANSFERFILE.md
5       3       docs/nv-shim-transfer.md
201     0       docs/nv-shim-transferfile.md
10      3       src/include/nv_shim.h
49      11      src/kernel/nv_shim.c
225     12      src/kernel/nv_shim_drm_test.c
```

6 files, **+644 / −29** — byte-identical in stat to `git diff --stat 8e33b361
origin/nv/wp-transferfile`, so the merge brought exactly the lane and nothing else.
`git diff --stat 8e33b361 2ba48fd3 -- docs/nv-plan.md` is empty: the coordinator-owned plan is
untouched.

## 2. The acceptance measurement — merged tip, one round

Both tiers ran in the merged worktree (`HEAD = 2ba48fd3`), one round, in this order:

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

`SKIP_KVM=1` is passed because this uid is not in group `kvm` (`/dev/kvm` is `root:kvm 660`);
the intel log's first line is `Running unit tests with QEMU_ARGS:` with the list empty — TCG,
no suite skipped. The ARM run is TCG by construction (`unit-tier-arm-merged.log:341`
`qemu-system-aarch64 … -accel tcg,thread=multi …`). The logs are the scripts' **console**
output (`unit-tier-{intel,arm}-merged.log`), not `qemu.log`.

| tier | integration tip `8e33b361` | lane pre-fold `0ee98a2f` | **merged tip `2ba48fd3`** | delta vs `8e33b361` |
|---|---|---|---|---|
| intel (x86_64) | `Tests run: 604` / `0` | `Tests run: 605` / `0` | **`605` / `0` / `UNIT TESTS PASSED`** | **+1** |
| ARM (aarch64) | `Tests run: 483` / `0` | `Tests run: 484` / `0` | **`484` / `0` / `UNIT TESTS PASSED`** | **+1** |

The `8e33b361` column is read from the previous fold's own attached logs
(`attachments/t_e2620813/unit-tier-intel-merged.log:3003`, `…-arm-merged.log:2409`), not from
its prose. The lane column is read from `attachments/t_9daa63c3/unit-tier-{intel,arm}.log`
(`:3027`, `:2432`).

The actual lines, as printed on the merged tree:

```
unit-tier-intel-merged.log:3027  Tests run:    605
unit-tier-intel-merged.log:3028  Tests failed: 0
unit-tier-intel-merged.log:3029  UNIT TESTS PASSED
unit-tier-arm-merged.log:2432    Tests run:    484
unit-tier-arm-merged.log:2433    Tests failed: 0
unit-tier-arm-merged.log:2434    UNIT TESTS PASSED
```

The +1 per arch is the lane's new suite, named in both merged logs:

```
unit-tier-intel-merged.log:2685  Running test_nvshim_drm_syncobj_transfer_syncfile...
unit-tier-arm-merged.log:2119    Running test_nvshim_drm_syncobj_transfer_syncfile...
```

ARM's relocation gate also ran on the merged tree:

```
unit-tier-arm-merged.log:346  [RELOC] entries=0x00000000000001AD applied=0x00000000000001AD slide=0x00000003FA200000 outside=0x0000000000000000 status=0x0000000000000000 ok
```

`0x1AD` = 429 kernel pointer words, entries == applied, `status=0x0` — the same count as the
pre-fold tip (this rung adds no relocated static data: the uAPI field shape, the reason string
and the test bodies are plain data).

## 3. The merged tree serves the sync_file source leg, and the rows are present

Every row below was grepped out of the **merged** logs. The transfer bucket `r_synctransfer = 48`
(`src/kernel/nv_shim.c:431`) is sized for the whole boot, so the earlier syncobj suites cannot
suppress these rows — the silent-budget failure this project has paid for twice.

| fact | intel log | ARM log |
|---|---|---|
| `transfer src=1 dst=2 src_point=0x9 dst_point=0x9 obj=16->17 chained=1 src_at=0x0` (handle->handle, pending: destination chained onto the source's entry) | `:2675` | `:2109` |
| `transfer src=1 dst=2 src_point=0xc dst_point=0xc obj=16->17 src_reached=1 src_at=0xc` (handle->handle, already reached) | `:2678` | `:2112` |
| `transfer src=3 dst=2 src_point=0x9 dst_point=0x9 obj=18->19 chained=1 src_at=0x0` (**sync_file source at its floor, pending**) | `:2692` | `:2126` |
| `transfer src=3 dst=4 src_point=0x9 dst_point=0xd obj=18->20 src_reached=1 src_at=0x9` (**sync_file source at its floor, reached, point remapped**) | `:2693` | `:2127` |
| `transfer src=6 dst=5 src_point=0xc dst_point=0xc obj=21->22 src_reached=1 src_at=0xc` (**sync_file source, reached**) | `:2697` | `:2131` |
| `refuse transfer src_point=0x1 reason=transfer_point_below_import` (**the new refusal, reason 20**) | `:2720` | `:2154` |
| `refuse transfer src_handle=0x5 reason=transfer_src_sync_file` (reason 19, narrowed to "imported source above the floor") | `:2721` | `:2155` |
| `refuse transfer src_point=0x7 reason=transfer_point_not_pending` | `:2727` | `:2161` |

The row totals are the same 23 per log the lane reported pre-fold (6 served + 17 refused):

```
$ grep -c "drm syncobj transfer \|drm syncobj refuse transfer" unit-tier-intel-merged.log   # 23
$ grep -c "^\[NVSHIM\] drm syncobj transfer "                  unit-tier-intel-merged.log   #  6
$ grep -c "drm syncobj refuse transfer"                        unit-tier-intel-merged.log   # 17
```

(the ARM log prints the identical 23 / 6 / 17, and `reason=transfer_point_below_import` is
present in both — a green tier without that row would be the silent-budget failure the card
names.)

Served code on the merged tree: `op_drm_syncobj_transfer` at `src/kernel/nv_shim.c:3991`,
dispatched from `case 0xcc` at `:5205`; the uAPI row is `NVSHIM_DRM_IOC_SYNCOBJ_TRANSFER` at
`src/include/nv_drm_uapi.h:590` (32 bytes, `0xC02064CC`). The reason space is
`NVSHIM_SYNC_TRANSFER_*` 15…20 at `src/include/nv_shim.h:438-443`, with 20 =
`NVSHIM_SYNC_TRANSFER_POINT_BELOW_IMPORT` rendered as `transfer_point_below_import` by
`k_sync_reasons[]` (`src/kernel/nv_shim.c:3396`).

## 4. The doc surface is on the branch now

`docs/nv-shim-transferfile.md` and `SWARM-REPORT-WP-TRANSFERFILE.md` did **not** exist on the
integration tip:

```
$ git cat-file -e origin/nv/integ-swarm:docs/nv-shim-transferfile.md
fatal: path 'docs/nv-shim-transferfile.md' does not exist in 'origin/nv/integ-swarm'
$ git cat-file -e origin/nv/integ-swarm:SWARM-REPORT-WP-TRANSFERFILE.md
fatal: path 'SWARM-REPORT-WP-TRANSFERFILE.md' does not exist in 'origin/nv/integ-swarm'
```

They are now present at 201 and 154 lines on the merged tree, and `docs/nv-shim-transfer.md`
grew by 5/3 lines to 249. No doc edit was made or needed by this card.

## 5. What this fold does not claim

- **`SYNCOBJ_EVENTFD` (`0xcf`) stays refused by name** in both merged logs
  (`drm refuse syncobj_eventfd errno=ENOSYS` at intel `:2258 :2457 :2573 :2653`); it is the
  one remaining entry in `g_drm_refusals[]` (`src/kernel/nv_shim.c:1533`). The child rung
  `t_d14cd97e` owns that row and builds on this tree.
- **The binary (non-TIMELINE) transfer is still refused**: `transfer_needs_timeline`
  (reason 15) at intel `:2705` / ARM `:2139`.
- **Pending-transfer visibility is per client**: the served pending shape requires the import
  on the same `/dev/nvshim` description (`docs/nv-shim-transferfile.md` §4). Nothing here
  widens that.

## 6. Push

Pushed without force: local `nv/wp-transferfile-fold` → `refs/heads/nv/integ-swarm`; verified
with `git ls-remote origin refs/heads/nv/integ-swarm` equal to local `HEAD` at push time.
`nv/wp-transferfile` was read, not moved, and `nv/wp-transfer`, `nv/wp-syncfile` and
`nv/wp-syncfam` are untouched.
