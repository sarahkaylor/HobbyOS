# SWARM-REPORT-WP-TRANSFER-FOLD — WP-TRANSFER folded into `nv/integ-swarm` (t_e2620813)

Lane: `nv-card`. Card: `t_e2620813`. Base `nv/integ-swarm`: `95bae60f` (the WP-SYNCFILE
fold's report commit). Lane branch folded: `nv/wp-transfer` @ `dcaf09eb` (tooltip card
`t_b227267a`, commits `622f90f0` + `dcaf09eb`). Merge commit: **`089591ef`** (parents
`95bae60f` `dcaf09eb`). No rebase, no force-push, no `docs/nv-plan.md` edit, no GPU boot,
no VM boot — the shim is an ABI surface and the unit tiers are its only gate.

## 1. What the merge was

`git merge-base --is-ancestor origin/nv/integ-swarm origin/nv/wp-transfer` → YES, and
`git rev-list --left-right --count origin/nv/integ-swarm...origin/nv/wp-transfer` → `0 2`:
the lane is 2 commits ahead, 0 behind, so the content is a fast-forward. It was landed with
`--no-ff` so the fold is a named commit on the integration branch, matching every earlier
fold's shape (the last three folds are `--no-ff` merges).

| step | commit | result |
|---|---|---|
| worktree cut from `origin/nv/integ-swarm` | `95bae60f` | base |
| `git merge --no-ff -m "Merge WP-TRANSFER: SYNCOBJ_TRANSFER's timeline handle->handle leg is served" origin/nv/wp-transfer` | **`089591ef`** (parents `95bae60f` `dcaf09eb`) | **clean**, no conflicts, no rerere, nothing to resolve |
| docs commit carrying this report | see `git log` on the merged tip | report only |

`git show --no-patch --format=%P 089591ef` prints `95bae60f6c11… dcaf09ebf2fc…`, and
`git diff --stat dcaf09eb 089591ef` is **empty** — the merged tree is byte-identical to the
lane tip, as a content fast-forward must be.

Fold footprint — `git diff --numstat 95bae60f 089591ef`:

```
130     0       SWARM-REPORT-WP-SYNCOBJ-TRANSFER.md
247     0       docs/nv-shim-transfer.md
66      14      src/include/nv_drm_uapi.h
23      0       src/include/nv_shim.h
202     9       src/kernel/nv_shim.c
388     23      src/kernel/nv_shim_drm_test.c
```

6 files, **+1056 / −46** — byte-identical in stat to `git diff --stat 95bae60f origin/nv/wp-transfer`,
so the merge brought exactly the lane and nothing else. `git diff --stat 95bae60f 089591ef --
docs/nv-plan.md` is empty: the plan is untouched, as the card requires.

## 2. The acceptance measurement — merged tip, one round

Both tiers ran in the merged worktree (`HEAD = 089591ef`), one round, in this order:

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

`SKIP_KVM=1` is added because this uid is not in group `kvm` (`/dev/kvm` is `root:kvm 660`,
`id` → `uid=1001(nvcard) gid=1001(nvcard)`); the intel log's first line is
`Running unit tests with QEMU_ARGS:` with the list empty — TCG, no suite skipped. The ARM
tier is always TCG.

| tier | integration tip `95bae60f` | lane pre-fold `dcaf09eb` | **merged tip `089591ef`** | delta vs `95bae60f` |
|---|---|---|---|---|
| intel (x86_64) | `Tests run: 602` / `0` | `Tests run: 604` / `0` | **`604` / `0` / `UNIT TESTS PASSED`** | **+2** |
| ARM (aarch64) | `Tests run: 481` / `0` | `Tests run: 483` / `0` | **`483` / `0` / `UNIT TESTS PASSED`** | **+2** |

The `95bae60f` column is read from the previous fold's own attached logs
(`attachments/t_a0cadd5b/unit-tier-intel-merged.log:2971-2973`, `…-arm-merged.log:2376-2378`),
not from the lane's prose.

The actual lines, as printed on the merged tree:

```
unit-tier-intel-merged.log:3003  Tests run:    604
unit-tier-intel-merged.log:3004  Tests failed: 0
unit-tier-intel-merged.log:3005  UNIT TESTS PASSED
unit-tier-arm-merged.log:2409    Tests run:    483
unit-tier-arm-merged.log:2410    Tests failed: 0
unit-tier-arm-merged.log:2411    UNIT TESTS PASSED
```

The +2 per arch is the two new suites, named in both merged logs:

```
unit-tier-intel-merged.log:2664  Running test_nvshim_drm_syncobj_transfer...
unit-tier-intel-merged.log:2684  Running test_nvshim_drm_syncobj_transfer_refusals...
unit-tier-arm-merged.log:2099    Running test_nvshim_drm_syncobj_transfer...
unit-tier-arm-merged.log:2119    Running test_nvshim_drm_syncobj_transfer_refusals...
```

ARM's relocation gate also ran on the merged tree:

```
unit-tier-arm-merged.log:14  hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

`429` is the same word count as the pre-fold tip — this rung adds no relocated static data
(the uAPI struct, the five reason strings and the test bodies are all plain data).

## 3. The merged tree serves the transfer leg, and the rows are present

Every row below was grepped out of the **merged** logs, not inferred from the PASS line. Row
bucket `r_synctransfer = 48` (`src/kernel/nv_shim.c:431`) keeps the transfer rows from being
suppressed by the earlier syncobj suites — the silent-budget failure this project has paid
for twice.

| fact | intel log | ARM log |
|---|---|---|
| `transfer src=1 dst=2 src_point=0x9 dst_point=0x9 obj=16->17 chained=1 src_at=0x0` (pending: destination chained onto the source's entry) | `:2674` | `:2109` |
| `transfer src=1 dst=2 src_point=0xc dst_point=0xc obj=16->17 src_reached=1 src_at=0xc` (already reached: lands on return) | `:2677` | `:2112` |
| `transfer src=1 dst=2 src_point=0x4 dst_point=0x4 obj=18->19 src_reached=1 src_at=0x4` | `:2692` | `:2127` |
| `refuse transfer src_handle=0x4 reason=transfer_src_sync_file` | `:2702` | `:2137` |
| `refuse transfer src_point=0x7 reason=transfer_point_not_pending` | `:2706` | `:2141` |
| `transfer refusals: binary form, unknown flag, unsubmitted point, destroyed handle, zero point, dst point order, sync_file source -- all named` | `:2709` | `:2144` |

Seventeen transfer rows print in each log (3 served + 14 refused), the same count the lane
reported pre-fold:

```
$ grep -c "drm syncobj refuse transfer\|drm syncobj transfer " unit-tier-intel-merged.log   # 17
$ grep -c "^\[NVSHIM\] drm syncobj transfer "                unit-tier-intel-merged.log   # 3
$ grep -c "drm syncobj refuse transfer"                       unit-tier-intel-merged.log   # 14
```
(the ARM log prints the identical 17 / 3 / 14).

Served code on the merged tree: `op_drm_syncobj_transfer` at `src/kernel/nv_shim.c:3977`,
dispatched from `case 0xcc` at `:5167`; the uAPI struct is `src/include/nv_drm_uapi.h:290`
(32 bytes: `src/dst_handle` @0/4, `src/dst_point` @8/16, `flags` @24, `pad` @28) with the
encoded number `0xC02064CC` pinned at `:592`. The five distinct reasons are
`NVSHIM_SYNC_TRANSFER_*` 15…19 at `src/include/nv_shim.h:432-436`, rendered by
`k_sync_reasons[]` at `src/kernel/nv_shim.c:3390-3394`.

## 4. The doc surface is on the branch now

`docs/nv-shim-transfer.md` and `SWARM-REPORT-WP-SYNCOBJ-TRANSFER.md` did **not** exist on the
integration tip:

```
$ git show 95bae60f:docs/nv-shim-transfer.md
fatal: path 'docs/nv-shim-transfer.md' exists on disk, but not in '95bae60f'
$ git show 95bae60f:SWARM-REPORT-WP-SYNCOBJ-TRANSFER.md
fatal: path 'SWARM-REPORT-WP-SYNCOBJ-TRANSFER.md' exists on disk, but not in '95bae60f'
```

They are now present at 247 and 130 lines respectively. The doc's §2 (the uAPI, 32 bytes
pinned) and §4 (what is refused, each by its own name) match the merged tree's
`src/include/nv_drm_uapi.h` and `nv_shim.c` anchors quoted above; no doc edit was made or
needed by this card.

## 5. What this fold does not claim

- **`SYNCOBJ_EVENTFD` (`0xcf`) stays refused by name** in both merged logs —
  `drm refuse syncobj_eventfd errno=ENOSYS` at intel `:2257 :2456 :2572 :2652` and ARM
  `:1692 :1891 :2007 :2087`. Its wall is `linux_compat_fd.c`'s file-static eventfd counter
  table (`docs/nv-shim-syncfile.md` §5.2) and is not part of this card.
- **The transfer's sync_file source leg stays refused** (`transfer_src_sync_file`, reason 19,
  rows above). The child rung `t_9daa63c3` (`nv/wp-transferfile`) owns that leg and builds on
  the tree this fold lands.
- No binary (non-TIMELINE) transfer: `transfer_needs_timeline` (reason 15) still answers it.

## 6. Push

Pushed without force: local `nv/wp-transfer-fold` → `refs/heads/nv/integ-swarm`; verified
with `git ls-remote origin refs/heads/nv/integ-swarm` (remote ref equals local `HEAD` at push
time). `nv/wp-transfer` was read, not moved, and `nv/wp-syncfile`/`nv/wp-syncfam` are
untouched.
