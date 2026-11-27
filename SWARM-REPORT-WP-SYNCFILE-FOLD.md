# SWARM-REPORT-WP-SYNCFILE-FOLD — WP-SYNCFILE folded into `nv/integ-swarm` (t_a0cadd5b)

Lane: `nv-card`. Card: `t_a0cadd5b`. Base `nv/integ-swarm`: `974519ed` (the WP-SYNCFAM fold).
Lane branch folded: `nv/wp-syncfile` @ `0c42cc9d` (tooltip card `t_1f7624e8`).
Merge commit: `4d614cca` (parents `974519ed` `0c42cc9d`).
No rebase, no force-push, no `docs/nv-plan.md` edit, no GPU boot, no VM boot — the shim is an
ABI surface, and the unit tiers are its only gate.

## 1. What the merge was

The lane branch was **2 commits ahead, 0 behind** the integration tip
(`git rev-list --left-right --count 974519ed...origin/nv/wp-syncfile` → `0	2`), and
`git merge-base --is-ancestor origin/nv/integ-swarm origin/nv/wp-syncfile` → YES, so the
content is a fast-forward. It was landed with `--no-ff` so the fold is a named commit on the
integration branch, matching every earlier fold's shape.

| step | commit | result |
|---|---|---|
| worktree cut from `origin/nv/integ-swarm` | `974519ed` | base |
| `git merge --no-ff -m "Merge WP-SYNCFILE: sync_file is a real fd …" origin/nv/wp-syncfile` | `4d614cca` (parents `974519ed` `0c42cc9d`) | **clean**, no conflicts, no rerere |
| docs commit carrying this report | see `git log` on the merged tip | report only |

The commit message names both parents, and `git show --no-patch --format=%P 4d614cca` prints
`974519ed 0c42cc9d`.

Fold footprint — `git diff --stat 974519ed HEAD` on the merged tree:

```
 SWARM-REPORT-WP-SYNCFILE.md   | 101 ++++++++++++
 docs/nv-shim-syncfile.md      | 251 ++++++++++++++++++++++++++++++++++++++++++
 src/include/fs.h              |  40 ++++-
 src/include/nv_drm_uapi.h     |   5 +
 src/include/nv_shim.h         |  30 +++-
 src/kernel/fs.c               |  77 ++++++++
 src/kernel/nv_shim.c          | 251 +++++++++++++++++++++++++++++++----
 src/kernel/nv_shim_drm_test.c | 404 +++++++++++++++++++++++++++++++++++++++++-
 8 files changed, 1111 insertions(+), 48 deletions(-)
```

That is byte-identical in stat to `git diff --stat 974519ed origin/nv/wp-syncfile` — the merge
brought exactly the lane and nothing else. `git diff --stat 974519ed HEAD -- docs/nv-plan.md`
is empty.

## 2. The acceptance measurement — merged tip, one round

Both tiers ran in the same worktree, on the merged tree (`HEAD = 4d614cca` before the report
commit), in the card's invocation:

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

`SKIP_KVM=1` is on the intel line because this uid is not in group `kvm`
(`/dev/kvm` is `root:kvm 660`); the intel log's first line is
`Running unit tests with QEMU_ARGS:` (empty) — TCG, as intended, and it skips no suite. The
ARM tier is always TCG.

| tier | integration tip `974519ed` | lane pre-fold `0c42cc9d` | **merged tip `4d614cca`** | delta vs `974519ed` |
|---|---|---|---|---|
| intel (x86_64) | `Tests run: 601` / `0` | `Tests run: 602` / `0` | **`Tests run: 602` / `Tests failed: 0` / `UNIT TESTS PASSED`** | **+1** |
| ARM (aarch64) | `Tests run: 480` / `0` | `Tests run: 481` / `0` | **`Tests run: 481` / `Tests failed: 0` / `UNIT TESTS PASSED`** | **+1** |

The actual lines, as printed:

```
unit-tier-intel-merged.log:2971  Tests run:    602
unit-tier-intel-merged.log:2972  Tests failed: 0
unit-tier-intel-merged.log:2973  UNIT TESTS PASSED
unit-tier-arm-merged.log:2376    Tests run:    481
unit-tier-arm-merged.log:2377    Tests failed: 0
unit-tier-arm-merged.log:2378    UNIT TESTS PASSED
```

The merged counts reproduce the lane's pre-fold 602/481 exactly, and the +1 against each
arch's `974519ed` baseline is the one new suite, `test_nvshim_drm_syncfile`, named in both
merged logs:

```
unit-tier-intel-merged.log:2626  Running test_nvshim_drm_syncfile...
unit-tier-arm-merged.log:2060    Running test_nvshim_drm_syncfile...
```

ARM's relocation gate also ran on the merged tree:

```
unit-tier-arm-merged.log:14  hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

`429` is the same word count as the tip — the new `FILE_TYPE_SYNCFILE` adds no relocated
static data.

Logs attached to this card: `unit-tier-intel-merged.log` (602/0) and
`unit-tier-arm-merged.log` (481/0).

## 3. `fs.c` / `fs.h` landed clean — the `f1_probe_fd()` dispatch audited

This is the first fold to touch the fd table, so the probe dispatch was checked line by line,
not just by the green tier.

`src/kernel/fs.c` is **+77/−0** (`git diff --numstat 974519ed HEAD -- src/kernel/fs.c` →
`77	0`): no existing line was removed or rewritten, so no branch could have been dropped by
the merge. The dispatch itself:

- base `f1_probe_fd()` (`src/kernel/fs.c:1830`) had **nine** `f->type` branches: `TIMERFD`,
  `PIPE`, `UNIXSOCK`, `SOCKET`, `FAT16 || NFS`, `TMPFS || TMPFSDIR || MEMFD`, `EVENTFD`,
  `DEVSND`, `SIGNALFD`;
- merged `f1_probe_fd()` has the **same nine, byte-identical**, plus one new branch
  (`src/kernel/fs.c:1951-1961`) inserted immediately before the function's final
  `return -1`:

```c
  if (f->type == FILE_TYPE_SYNCFILE) {
    if (!f->syncfile.obj) return -1;
    if (nv_shim_syncfile_ready(f->syncfile.obj, f->syncfile.point)) *r = 1;
    return 0;
  }
```

- the three probe call sites (`file_select`/`file_poll` family at `src/kernel/fs.c:2102`,
  `:2178`, `:4447`) are unchanged and now see one extra type;
- the enum value is **appended last** (`src/include/fs.h:147`), so no existing
  `FILE_TYPE_*` number shifts — the comment says so and the diff shows
  `FILE_TYPE_SYNCOBJ,` → `FILE_TYPE_SYNCOBJ, … FILE_TYPE_SYNCFILE`;
- both close paths carry the new hook: per-description `file_close()` calls
  `nv_shim_syncfile_release()` at `src/kernel/fs.c:3297`, and the no-process-context
  `fs_close_global()` calls it with `fd = -1` at `:3477` — the same two-hook shape
  `FILE_TYPE_SYNCOBJ` already uses;
- `src/include/fs.h` is +39/−1: the type, the `{obj, point}` slot, and the three helper
  declarations (`file_syncfile_install`, `file_syncfile_obj`, `file_is_syncfile`).

## 4. The merged tree serves the sync_file legs, and the log rows are present

Every row quoted below was grepped out of the **merged** logs, not inferred from the PASS
line. The rung's own bucket `r_syncfile = 48` (`src/kernel/nv_shim.c:423`) is what keeps
them from being suppressed by the earlier syncobj suites (`r_sync = 128`/boot).

| fact | intel log | ARM log |
|---|---|---|
| `handle_to_fd sync_file=1 … point=0x1 reached=0x0` | `:2543` | `:1977` |
| `fd_to_handle sync_file=1 fd=4 handle=2 obj=9 point=0x1` | `:2544` | `:1978` |
| `syncfile binary: file_poll 0/revents 0 while the fence was held, 1/POLLIN after the kick retired it` | `:2643` | `:2077` |
| `syncfile timeline@7: file_poll 0/revents 0 before the kick, 1/POLLIN after; imported SYNCOBJ_WAIT -ETIME -> 0 on the same retire` | `:2653` | `:2087` |
| `syncfile refs: dup/fork/SCM_RIGHTS kept ONE description and ONE record reference; the last close freed each record once: PASS` | `:2659` | `:2093` |
| `drm refuse syncobj_transfer errno=ENOSYS` | `:2574` | `:2008` |
| `drm refuse syncobj_eventfd errno=ENOSYS` | `:2575` | `:2009` |

Served code on the merged tree: `op_drm_syncobj_handle_to_fd` at
`src/kernel/nv_shim.c:3975`, `op_drm_syncobj_fd_to_handle` at `:4106`, dispatched from
`case 0xc1`/`0xc2` at `:4968-4969`; the readiness/release seams are
`nv_shim_syncfile_ready()` at `:2806` and `nv_shim_syncfile_release()` at `:2820`. The two
flags are documented as served in `src/include/nv_drm_uapi.h:253-256`.

## 5. `docs/nv-shim-syncfile.md` is on the branch now

It did not exist on the integration tip before the fold — `git show
974519ed:docs/nv-shim-syncfile.md` → `fatal: path 'docs/nv-shim-syncfile.md' … not in
'974519ed'` — and is now present at 251 lines. Its §2/§6 rows are the lane's own pre-fold
measurement (`974519ed` → `0c42cc9d`): intel 601 → 602, ARM 480 → 481, +1 each,
`test_nvshim_drm_syncfile`. Those rows **match this merged run**; no doc edit was made or
needed.

## 6. `docs/nv-plan.md` status rows — verbatim, not edited (coordinator-owned)

The plan still carries its pre-fold text; the file is untouched on the merged tip
(`git diff 974519ed HEAD -- docs/nv-plan.md` is empty). Both rows that describe the shim
follow, quoted verbatim:

- §3 table, row **F. `/dev/nv*` shim** (`docs/nv-plan.md:677`):
  > **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows + 21 named refusals, CPU-mapped BOs + `mmap` (583/0 on intel, 465/0 on ARM); NVIF served (§2.5) and, since round 6, `GETPARAM GRAPH_UNITS` too — a stock nvk now stops at the first engine class object (copy `0xc7b5`), refused by name; the ICD that drives it is built by the tree's own `tools/mesa/build-nvk.sh` (§2.8); PRIME still missing

- §4 ladder, row **N2** (`docs/nv-plan.md:693`):
  > **done**: 16 real `DRM_IOCTL_NOUVEAU_*` rows (magic `'d'`, real struct sizes), 21 named refusals, CPU-mapped BOs with `mmap`; NVIF (round 5) and `GETPARAM GRAPH_UNITS` (round 6) served, so a stock winsys's device probe now ends at the copy-class object by name; PRIME still missing

Stale in both, for the coordinator: the counts (583/0 and 465/0) predate the PRIME, sync,
syncfam and syncfile rungs — on this merged tip the shim tier is **602/0 intel and 481/0
ARM**; PRIME is served (`t_78fce566`), the syncobj fd/timeline rows are served
(`t_8acf4a21`), QUERY is served (`t_0903a106`), and EXPORT/IMPORT_SYNC_FILE are served
(`t_1f7624e8`). The named syncobj refusals that remain are `syncobj_transfer` and
`syncobj_eventfd`.

## 7. Push

Pushed without force: local `nv/wp-syncfile-fold` → `refs/heads/nv/integ-swarm`, verified
with `git ls-remote origin refs/heads/nv/integ-swarm` (the remote ref equals local `HEAD` at
push time; `nv/wp-syncfile` was read, not moved).

## 8. What this fold does not claim

`syncobj_transfer` (next rung) and `syncobj_eventfd` stay `-ENOSYS` **by name** in both
merged logs — no half-served transfer leg, no faked eventfd counter. The child rung
`SYNCOBJ_TRANSFER` has a handle→handle leg that this tip can carry: it builds on a tree that
already carries `FILE_TYPE_SYNCFILE`, which is now `nv/integ-swarm`'s tip, not the lane
branch.
