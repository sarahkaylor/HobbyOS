# SWARM-REPORT-WP-FOLD — the shim pair folded into `nv/integ-swarm` (t_f18e519e)

Lane: `nv-card`. Card: `t_f18e519e`. Merged tip on `nv/integ-swarm`: `b149b81d`.
Both rungs were measured **on their own branches** before this card; this report is
the first measurement of the **merged tree**. No GPU, no VM boot, no `docs/nv-plan.md`
edit — the shim is an ABI surface and both parent rungs said so.

## 1. What the merge was

| step | commit | result |
|---|---|---|
| worktree cut from `origin/nv/integ-swarm` | `bd8d10a3` | base measured here first |
| `git merge --no-ff origin/nv/wp-prime` | `de9966e8` (parents `bd8d10a3` `e46d257c`) | **clean**, 10 files, +6782/−79 |
| `git merge --no-ff origin/nv/wp-sync` | `b149b81d` (parents `de9966e8` `cb40bbf9`) | **clean**, 8 files, +2100/−111 |

`wp-sync` is based on `wp-prime` (`git merge-base e46d257c cb40bbf9` = `e46d257c`),
so the order was load-bearing and was followed. No conflict appeared, so the
union rule for `unit_test.{c,h}` never had to be exercised: neither branch touches
`src/kernel/unit_test.c` (the one registration call `nv_shim_test_suite()` at
`src/kernel/unit_test.c:73` is already in the base), and each rung's new suites are
called from inside that function in `src/kernel/nv_shim_drm_test.c`. Both rungs'
suites are therefore live by construction, and §2 shows all six running.

Fold footprint vs the base tip (`git diff --stat bd8d10a3..HEAD -- src/`):

```
 src/include/fs.h              |   52 +-
 src/include/nv_drm_uapi.h     |  216 ++++-
 src/include/nv_shim.h         |   84 ++
 src/kernel/fs.c               |  117 +++
 src/kernel/nv_shim.c          | 1835 ++++++++++++++++++++++++++++++++++++-----
 src/kernel/nv_shim_drm_test.c | 1333 ++++++++++++++++++++++++++++-
 src/kernel/vm.c               |    8 +
 7 files changed, 3426 insertions(+), 219 deletions(-)
```

## 2. The acceptance measurement

Both tiers were run in the same worktree, on the merged tree, with the card's
invocation line. No KVM: this uid is not in group `kvm` (`/dev/kvm` is `root:kvm
660`), so both ran under TCG and `SKIP_KVM=1` was added — it only selects the
accelerator, it does not skip a suite.

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

**Baseline measured first, in the same worktree** (`bd8d10a3`, before the merges):

| tier | baseline `bd8d10a3` | merged `b149b81d` | delta |
|---|---|---|---|
| intel | `Tests run: 594` / `Tests failed: 0` / `UNIT TESTS PASSED` | `Tests run: 600` / `Tests failed: 0` / `UNIT TESTS PASSED` | **+6** |
| ARM | `Tests run: 473` / `Tests failed: 0` / `UNIT TESTS PASSED` | `Tests run: 479` / `Tests failed: 0` / `UNIT TESTS PASSED` | **+6** |

The measured baseline is the one the plan quotes (594/473 at `93cfa49d`); the tip
between `93cfa49d` and `bd8d10a3` touches `docs/`, `tools/` and
`.github/workflows/` only — `git diff --stat 93cfa49d..bd8d10a3 -- src/` is empty —
which is why the number reproduced exactly. The +6 is the six new suites, and each
one is named in both merged logs:

```
  Running test_nvshim_drm_prime_export...
  Running test_nvshim_drm_prime_import...
  Running test_nvshim_drm_prime_refusals...
  Running test_nvshim_drm_syncobj_timeline...
  Running test_nvshim_drm_syncobj_fd...
  Running test_nvshim_drm_syncobj_refusals...
```

Three suites from WP-PRIME (`t_78fce566`) + three from WP-SYNC (`t_8acf4a21`) = 6,
so the arithmetic closes with no unexplained test on either arch.

One pre-existing note, not a regression: the ARM tier prints
`[LOCKHEALTH-IRQOFF] ... max_us=23951 worst_lock=vm_lock`, and the **baseline** ARM
run prints the same row with `max_us=23749`, same lock. It predates the fold.

## 3. Proof the code itself came across (not just the counts)

`git grep -n timeline_point_order -- src/kernel/nv_shim.c` on the merged tree:

```
src/kernel/nv_shim.c:3263:    "timeline_point_order",
```

That is the WP-SYNC refusal reason inside `k_sync_reasons[]`, which only exists
because WP-SYNC's record/point machinery is in this tree. The rest of the surface
the two rungs added, located in the merged file:

| what | merged-tree location |
|---|---|
| `PRIME_HANDLE_TO_FD` (0x2d) handler | `src/kernel/nv_shim.c:2272`, dispatched at `:4634` |
| `PRIME_FD_TO_HANDLE` (0x2e) handler | `src/kernel/nv_shim.c:2390`, dispatched at `:4635` |
| served-row table (`g_drm_rows[]`) | `src/kernel/nv_shim.c:1412` — 22 rows, 0x2d/0x2e and 0xc1/0xc2/0xca/0xcd among them |
| DRM refusal table (`g_drm_refusals[]`) | `src/kernel/nv_shim.c:1462–1470` — 14 rows; `syncobj_query`, `syncobj_transfer`, `syncobj_eventfd` kept by name |
| syncobj record + retire signalling | `client_reap_locked()` sets `o->point` from the retired fence (`src/kernel/nv_shim.c:3217–3234`) |
| `drm_syncobj` occurrences | 35 in `src/kernel/nv_shim.c` vs 19 on the base tip |

DRM surface after the fold: **22 real `DRM_IOCTL_NOUVEAU_*`/_DRM_ rows in the size
table + NVIF routed on its encoded buffer length = 23 served requests**, and
**14 named refusals**. Before the fold the plan's row counted 16 served rows.

## 4. What this report is not

- **No winsys/Mesa client was run.** The ioctl paths are exercised; the claim that
  a stock client uses them stays source-level, exactly as both parent cards said.
- **No sync_file.** `EXPORT_SYNC_FILE` / `IMPORT_SYNC_FILE` are still `-ENOSYS` by
  name, so a Vulkan WSI path that needs a `poll()`-able fence fd still hits a wall.
  That wall is the next shim rung, not this one.
- **No CI run was created.** This is a merge + local unit tier; the quoted counts
  are local TCG runs at `b149b81d` on this box, recorded in the attached logs.
- The report commit itself adds one markdown file at the repo root; no source is
  touched by it, so the measured tree and the pushed tree are identical in `src/`.

## 5. Evidence

| file | what |
|---|---|
| `intel-baseline-bd8d10a3.log` | base tip intel, 594/0 |
| `arm-baseline-bd8d10a3.log` | base tip ARM, 473/0 |
| `intel-merged-b149b81d.log` | merged intel, 600/0 |
| `arm-merged-b149b81d.log` | merged ARM, 479/0 |
