# SWARM-REPORT-WP-SYNCFAM — QUERY served, EVENTFD left a named wall (t_0903a106)

Lane: `nv-card`. Card: `t_0903a106`. Base: `nv/integ-swarm` @ `8efe5ae2` (the
WP-FOLD tip). Branch: `nv/wp-syncfam`. Shim only — **no GPU boot, no VM boot, no
`docs/nv-plan.md` edit**; `src/kernel/nv_shim.c` was the only open shim card's
file this round and nothing else touched it.

## 1. What landed, and what did not

| scope item | outcome |
|---|---|
| `SYNCOBJ_QUERY` (`0xcb`) answers the submitted point | **landed** — served from a per-record submitted/signalled ledger; driven through the uAPI's own `points[]` output array |
| `SYNCOBJ_EVENTFD` (`0xcf`) wakes a real eventfd | **not landed, deliberately** — the eventfd counter has no kernel-side writer the shim can call from the retire path; named refusal kept, seam reported with file:line (§4) |

The card authorises exactly this fallback: *"If the eventfd object cannot be
signalled from the shim's retire path ... do not improvise: report the missing
seam with file:line, keep `syncobj_eventfd` a named refusal, and land QUERY
alone — that is an honest partial, and this card closes on QUERY's
measurement."* The seam check was run; §4 is the report.

## 2. The measurement

Both tiers, in the card's worktree, with the card's invocation line, on the
code commit `e6fb0233` on `nv/wp-syncfam` (the doc commit after it is .md only). No KVM: this
uid is not in group `kvm` (`/dev/kvm` is `root:kvm 660`), so both ran TCG.

| tier | invocation | baseline, same tree | this rung | delta |
|---|---|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `Tests run: 600 / Tests failed: 0` (`8efe5ae2`, measured before the first edit in this worktree) | `Tests run: 601 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` (TCG) | `Tests run: 479 / Tests failed: 0` (`8efe5ae2`, detached worktree of the same commit) | `Tests run: 480 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |

The delta is one suite per arch: **`test_nvshim_drm_syncobj_query`**. Logs, all
four, attached to the card:

* `unit-tier-intel-syncfam-601.log` (this rung, intel)
* `unit-tier-arm-syncfam-480.log` (this rung, ARM)
* `unit-tier-intel-baseline-600.log` (baseline, intel, same worktree)
* `unit-tier-arm-baseline-479.log` (baseline, ARM, detached worktree at the same commit)

The ARM `LOCKHEALTH-IRQOFF` row (a pre-existing, unrelated stress row) moved
`24226 → 23912 us` between baseline and this rung, i.e. down; it is not a
regression and was the same class of row WP-FOLD had to explain.

## 3. QUERY: what the tier actually asserts

`docs/nv-shim-syncfam.md` is the contract; this is the list the acceptance asked
for, with where each is measured (`test_nvshim_drm_syncobj_query`,
`src/kernel/nv_shim_drm_test.c:2545-2730`):

| acceptance | measurement |
|---|---|
| after an EXEC signalling N retires, QUERY returns N with `LAST_SUBMITTED` | `nv_rm_mock_pending()` 1 → held EXEC signals 11 → `query point=0xb last_submitted=1` **before** the retire, and the same after the retire that drops the pending entry |
| before the EXEC it returns the pre-submit value | fresh record: `query point=0x0 last_submitted=1`; after a timeline signal to 5 the pre-submit signalled value reads `0x5` while the held EXEC's point reads `0xb` |
| a point past the ledger is refused by name | a foreign handle → `reason=not_this_clients_syncobj`, nothing written (`points[]` sentinel intact, batch of 2); a host-side stub point with no ledger entry → `reason=point_beyond_ledger`, nothing written |
| read through the uAPI, not the kernel seam | the suite never calls `nv_shim_test_syncobj_point()`; it reads the answer out of its own `points[]` user window after `DRM_IOCTL_SYNCOBJ_QUERY` (`0xC01864CB`) |
| the rows that stay refused are asserted `-ENOSYS` by name | `syncobj_transfer` and `syncobj_eventfd`: `nv_shim_drm_refusal_name() != 0` and `nv_shim_ioctl(...) == -ENOSYS`, plus the console rows `[NVSHIM] drm refuse syncobj_transfer errno=ENOSYS` / `…syncobj_eventfd errno=ENOSYS` |

Refusal table line numbers, post-change, as the card asks
(`src/kernel/nv_shim.c`): `g_drm_rows[]` at **1430**, the QUERY row `{0xcb, 24}`
at **1457**, `g_drm_refusals[]` at **1472** (comment 1484-1491, the two rows
1492-1493), handler `op_drm_syncobj_query()` at **3748**, dispatch `case 0xcb`
at **4790**. The header's refusal-probe comment and the served-row block were
updated too, so a probe built from `src/include/nv_drm_uapi.h` sends the real
`0xC01864CB`.

## 4. EVENTFD: the missing seam, with file:line

The eventfd object exists and is real (a counter, readers parked on pid masks,
a poll key). What does not exist is any way for the shim to add to that counter
from a fence retire:

1. `src/kernel/linux_compat_fd.c:1353-1362` — the counter table
   `static struct { … uint64_t count; … } l_efd[1024];` is file-static.
2. `src/kernel/linux_compat_fd.c:1477` — the only writer,
   `int l_eventfd_write(struct file *f, const void *buf, int size)`; at `:1482`
   it does `l_copy_from_user(current_process(), &v, (uint64_t)(uintptr_t)buf, 8)`,
   i.e. **`buf` is a user address**, range-checked by `vm_range_ok()`
   (`src/kernel/linux_compat.c:71-79`). A kernel-side value from the retire path
   cannot be passed through it.
3. `src/include/fs.h:333` — the only published eventfd entry point is
   `file_eventfd_create()`; `l_eventfd_key()` (`:1394`), `l_eventfd_release()`
   (`:1398`) and `l_eventfd_probe()` (`:1406`, read-only) add nothing, and
   `l_eventfd_new()` (`:1375`) makes a new counter.
4. `src/kernel/fs.c:4346-4348` — the only dispatch into the writer is
   `FILE_TYPE_EVENTFD` inside `file_write(process *, int fd, const void
   *user_buf, int size, tf)` (`fs.c:4239`); the slot is `f->eventfd.slot`
   (`src/include/fs.h:214-215`) and the file table is addressed by
   `static inline struct file *gfile()` at `fs.c:52`, i.e. inside `fs.c`.

So signalling an eventfd from `client_reap_locked()` needs two functions that do
not exist — a kernel-side `l_eventfd_signal(slot, value)` and an
fd → slot resolver — both in files outside this card's shim-only scope.
Faking it (writing nothing to the counter while logging a wake) is the §2.13
fake this project forbids. `syncobj_eventfd` therefore stays `-ENOSYS` by name,
and `SYNCOBJ_TRANSFER` stays with it on the sync_file wall
(`docs/nv-shim-syncfam.md` §5).

## 5. Footprint

```
 src/include/nv_drm_uapi.h     |  31 +++--
 src/include/nv_shim.h         |   5 +
 src/kernel/nv_shim.c          | 200 ++++++++++++++++++++++++++----
 src/kernel/nv_shim_drm_test.c | 278 ++++++++++++++++++++++++++++++++++++++++--
 docs/nv-shim-syncfam.md       | new
 4 files changed, 469 insertions(+), 45 deletions(-)   (+ the doc)
```

No other file is touched: no `src/kernel/fs.c`, no `linux_compat_fd.c`, no
`docs/nv-plan.md`, no `unit_test.c`.

Two things this rung found and fixed on the way, both evidence bugs rather than
feature bugs:

* **five stale `EXPECT_EQ`s** asserted QUERY was `-ENOSYS` (layout size/name,
  the refusals suite, the PRIME refusals suite, the syncobj refusals suite). In
  this framework a failed assertion *returns* from the test function, skipping
  teardown, so a single stale one reads as a cascade. All five were updated in
  the same commit before the first run; §6 of the doc lists them;
* the DRM face's own refusal row (`drm refuse <name> errno=ENOSYS`) shared
  `r_refuse` (8 rows **per boot**), which the `'N'` face spends long before the
  syncobj suites run — so the two rows this card is about never printed even
  though the tier was green. They now have their own bucket `r_drm_refuse = 24`
  (`src/kernel/nv_shim.c:402-409`) and are in both logs. This is the WP-SYNC
  lesson ("a green tier with no evidence rows is a silent budget bug") caught
  by grepping for the rows instead of trusting the PASS line.

## 6. Verification gaps

* no Mesa/nouveau client linked or run — the ioctl path, request number and
  process are real, the `vkGetSemaphoreCounterValue` mapping is source-level;
* no eventfd wake and no sync_file either way (§4): a Vulkan WSI poll path still
  has that wall, named;
* no GPU boot: the fence is the mock RM's, retired by `kick()`;
* QUERY models two marks per object, not per-point availability, so
  `WAIT_AVAILABLE`/`WAIT_FOR_SUBMIT` remain `-ENOSYS` by name;
* the counts are local TCG runs on this box; no CI job was created.
