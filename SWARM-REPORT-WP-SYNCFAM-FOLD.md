# SWARM-REPORT-WP-SYNCFAM-FOLD — WP-SYNCFAM folded into `nv/integ-swarm` (t_d55b8831)

Lane: `nv-card`. Card: `t_d55b8831`. Base `nv/integ-swarm`: `8efe5ae2`.
Lane branch folded: `nv/wp-syncfam` @ `67bf3b05` (the tooltip card `t_0903a106`).
Merge commit: `2050bad4`. No other branch folded, no rebase, no force-push, no
`docs/nv-plan.md` edit, no GPU/VM boot — the shim is an ABI surface.

## 1. What the merge was

The lane branch was **3 commits ahead, 0 behind** the integration tip
(`git rev-list --left-right --count 8efe5ae2...origin/nv/wp-syncfam` → `0	3`), so the
content is a fast-forward. It was landed with `--no-ff` so the fold is a named commit on
the integration branch, matching the previous fold's shape (`b149b81d`).

| step | commit | result |
|---|---|---|
| worktree cut from `origin/nv/integ-swarm` | `8efe5ae2` | base |
| `git merge --no-ff -m "Merge WP-SYNCFAM: SYNCOBJ_QUERY answers the submitted point" origin/nv/wp-syncfam` | `2050bad4` (parents `8efe5ae2` `67bf3b05`) | **clean**, no conflicts |
| docs commit carrying this report | see `git log` on the merged tip | report only |

Fold footprint — `git diff --stat 8efe5ae2..HEAD` on the merged tree:

```
 SWARM-REPORT-WP-SYNCFAM.md    | 137 ++++++++++++++++++++++++++++++
 docs/nv-shim-syncfam.md       | 309 ++++++++++++++++++++++++++++++++++++++++++
 src/include/nv_drm_uapi.h     |  31 ++++-
 src/include/nv_shim.h         |   5 +
 src/kernel/nv_shim.c          | 200 +++++++++++++++++++++++----
 src/kernel/nv_shim_drm_test.c | 278 +++++++++++++++++++++++++++++++++++++++--
 6 files changed, 915 insertions(+), 45 deletions(-)
```

That is byte-identical in stat to `git diff --stat 8efe5ae2 origin/nv/wp-syncfam`, and
`git diff 8efe5ae2 HEAD -- docs/nv-plan.md` is empty: the merge brought exactly the lane
and nothing else.

## 2. The acceptance measurement — merged tip, one round

Both tiers ran in the same worktree, on the merged tree, in the card's invocation:

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

`SKIP_KVM=1` is on the intel line only because this uid is not in group `kvm`
(`/dev/kvm` is `root:kvm 660`); it selects the TCG accelerator and skips no suite. The
intel log prints `Running unit tests with QEMU_ARGS:` (empty), i.e. TCG as intended.

| tier | integration tip `8efe5ae2` | lane pre-fold `67bf3b05` | **merged tip `2050bad4`** | delta vs `8efe5ae2` |
|---|---|---|---|---|
| intel (x86_64) | `Tests run: 600` / `Tests failed: 0` | `Tests run: 601` / `Tests failed: 0` | **`Tests run: 601` / `Tests failed: 0` / `UNIT TESTS PASSED`** | **+1** |
| ARM (aarch64) | `Tests run: 479` / `Tests failed: 0` | `Tests run: 480` / `Tests failed: 0` | **`Tests run: 480` / `Tests failed: 0` / `UNIT TESTS PASSED`** | **+1** |

The merged counts **reproduce the lane's pre-fold 601/480 exactly** (delta 0 against the
lane), and the +1 against each arch's `8efe5ae2` baseline is the one new suite,
`test_nvshim_drm_syncobj_query` — named in both merged logs:

```
  Running test_nvshim_drm_syncobj_query...        (unit-tier-intel-merged.log:2573)
  Running test_nvshim_drm_syncobj_query...        (unit-tier-arm-merged.log:2007)
```

ARM's relocation gate also ran on the merged tree:
`hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot`
(`unit-tier-arm-merged.log:14`).

Logs attached to this card: `unit-tier-intel-merged.log` (601/0) and
`unit-tier-arm-merged.log` (480/0).

## 3. The merged tree serves QUERY and refuses the other two by name

Served row, merged `src/kernel/nv_shim.c` / `nv_drm_uapi.h`:

| fact | merged line |
|---|---|
| `#define NVSHIM_DRM_IOC_SYNCOBJ_QUERY … /* 0xC01864CB */` | `src/include/nv_drm_uapi.h:534-536` |
| `#define NVSHIM_DRM_NR_SYNCOBJ_QUERY 0xcb` | `src/include/nv_drm_uapi.h:579` |
| row `{0xcb, 24} /* SYNCOBJ_QUERY struct ..._timeline_array */` | `src/kernel/nv_shim.c:1466` |
| handler `static int op_drm_syncobj_query(...)` | `src/kernel/nv_shim.c:3757` |
| dispatch `case 0xcb: return op_drm_syncobj_query(p, fd, arg);` | `src/kernel/nv_shim.c:4799` |

Refusals, **by name**, in the merged refusal table (`g_drm_refusals`, names are
fixed-size char arrays — the ARM reloc hazard the table documents):

```
1501:    {NVSHIM_DRM_NR_SYNCOBJ_TRANSFER, "syncobj_transfer"},
1502:    {NVSHIM_DRM_NR_SYNCOBJ_EVENTFD, "syncobj_eventfd"},
```

`drm_refusal_name()` is consulted before the size table and the row returns `-ENOSYS`
(`src/kernel/nv_shim.c:4744-4749`), printing `drm refuse <name> errno=ENOSYS`. Both
merged logs show both rows, e.g. intel:

```
[NVSHIM] drm refuse syncobj_transfer errno=ENOSYS
[NVSHIM] drm refuse syncobj_eventfd errno=ENOSYS
```

and the served path answering through the uAPI's own `points[]` window, e.g.:

```
[NVSHIM] drm syncobj query n=1 point=0x0 last_submitted=1
[NVSHIM] drm syncobj query n=1 point=0x5 last_submitted=0
[NVSHIM] drm syncobj query n=1 point=0xb last_submitted=1
[NVSHIM] drm syncobj query n=2 point=0xc last_submitted=1
[NVSHIM] drm syncobj refuse query handle=0x385 reason=not_this_clients_syncobj
```

(NB: the predecessor's handoff cited the refusal rows at `:1481`; on the merged tree the
same two rows are at `:1501-1502` — the numbers above are the ones that were read this
run.)

## 4. `docs/nv-shim-syncfam.md` on the merged tip

It did not exist on `nv/integ-swarm` before the fold (`git show
origin/nv/integ-swarm:docs/nv-shim-syncfam.md` → `fatal: path … does not exist`) and is
now present at 309 lines. Its §6 measured rows are the lane's own pre-fold measurement
(`8efe5ae2` → `e6fb0233`): intel `600 → 601`, ARM `479 → 480`, +1 each,
`test_nvshim_drm_syncobj_query`. Those rows **match this merged run**: the merged tip
prints the same 601/0 and 480/0, with the same deltas and the same suite name. No doc
edit was needed, and none was made.

## 5. `docs/nv-plan.md` status rows — verbatim, not edited (coordinator-owned)

The plan still carries the pre-fold text. Two rows describe the shim:

- §3 table, row **F. `/dev/nv*` shim** (`docs/nv-plan.md:677`):
  > **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows + 21 named refusals, CPU-mapped BOs + `mmap` (583/0 on intel, 465/0 on ARM); NVIF served (§2.5) and, since round 6, `GETPARAM GRAPH_UNITS` too — a stock nvk now stops at the first engine class object (copy `0xc7b5`), refused by name; the ICD that drives it is built by the tree's own `tools/mesa/build-nvk.sh` (§2.8); PRIME still missing

- §4 ladder, row **N2** (`docs/nv-plan.md:693`):
  > **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows (magic `'d'`, real struct sizes), 21 named refusals, CPU-mapped BOs with `mmap`; NVIF (round 5) and `GETPARAM GRAPH_UNITS` (round 6) served, so a stock winsys's device probe now ends at the copy-class object by name; PRIME still missing

Stale in both: the counts (583/465 and "PRIME still missing") predate the folded
PRIME/sync/syncfam rungs. On the merged tip the shim tier is 601/0 intel and 480/0 ARM,
PRIME is served (`t_78fce566`), the syncobj fd/timeline rows are served (`t_8acf4a21`),
and QUERY is served (`t_0903a106`); the named syncobj refusals left are
`syncobj_transfer` and `syncobj_eventfd`.

## 6. Push

Pushed without force: local `nv/wp-syncfam-fold` → `refs/heads/nv/integ-swarm`, verified
by `git ls-remote`. Only `nv/integ-swarm` was written; `nv/wp-syncfam` was read, not
moved.

## 7. What this fold does not claim

`syncobj_transfer` and `syncobj_eventfd` remain refused with named reasons
(no sync_file object; no kernel-side eventfd counter writer). The next rung,
`t_1f7624e8` (`SWARM-REPORT-WP-SYNCFILE.md`), builds on this merged tip — it is parented
on this card, not on the lane branch.
