# SWARM-REPORT-WP-SYNC — explicit fences on the shim (t_8acf4a21)

Lane: `nv-card`. Card: `t_8acf4a21`. Branch: `nv/wp-sync`, based on `nv/wp-prime`
@ `e46d257c` (WP-PRIME's doc commit; its code commit is `46603a46`).
Contract + measurements: `docs/nv-shim-sync.md`. No GPU boot, no VM, no
r1/c3/c4 change — the card forbids one and this rung needs none.

## 1. What changed

| file | change |
|---|---|
| `src/include/nv_drm_uapi.h` | three uAPI structs (`syncobj_timeline_array` 24, `syncobj_timeline_wait` 48, `syncobj_handle` 24 with its `point`), the four flag bits, four **served** `_IOWR` rows; `SYNCOBJ_QUERY`/`TIMELINE_SIGNAL`'s encoded size corrected to drm.h's 24; the "WHAT IS NOT HERE" block rewritten |
| `src/include/fs.h` | `FILE_TYPE_SYNCOBJ` + `f->syncobj.obj` + `file_syncobj_install/_obj/_is` |
| `src/kernel/fs.c` | the three functions, and the release hook in **both** close paths (`file_close`, `fs_close_global`) |
| `src/include/nv_shim.h` | `NVSHIM_FEAT_SYNCOBJ_TIMELINE`, `nv_shim_syncobj_release()`, twelve named refusal reasons + the copy-out name function, two test seams |
| `src/kernel/nv_shim.c` | the refcounted syncobj record, the point as the single state, timeline signal/wait, `EXEC` timeline sig, both fd directions, `GET_CAP`/feature honesty, refusal-table diff, rows |
| `src/kernel/nv_shim_drm_test.c` | two existing assertions updated (the served case), three new suites |
| `docs/nv-shim-sync.md` | new: the contract, the errno choice, the close orders, the refusal diff, the gaps |

## 2. The five acceptance items, and how each was measured

1. **A point signal is a point wait.** `SYNCOBJ_TIMELINE_SIGNAL` at 5 makes
   waits for 5, 4 and 1 return 0 and waits for 6 return `-ETIME` (poll *and* a
   1 ms absolute deadline). The error is `-ETIME` because that is the uAPI's own
   answer for a wait that did not complete and what the binary row already
   returned; `-EAGAIN` is `poll(2)`'s errno and DRM's wait never uses it.
   *Same completion mechanism:* the wait's only source of state is
   `client_reap_locked()` → `rm()->fence_test()`, which is the function `EXEC`'s
   submit path calls. An `EXEC` signalling point 11 leaves a wait for 12
   `-ETIME` **after** the fence retires; the log has both rows.
2. **`EXEC` with a timeline sync is served.** `NVSHIM_DRM_SYNC_TIMELINE_SYNCOBJ`
   with `timeline_value = 11` is accepted, and 11 becomes signalled exactly when
   the fence retires (hold the kick → `-ETIME`; release + one kick → 0). The row
   `reason=timeline_syncobj` is gone. The malformed forms are refused by name:
   `timeline_point_zero` (value 0), `timeline_point_order` (12 and 5 when the
   object is at 12; also 6 twice in a row from the client-forced row),
   `unknown_sync_type` (a timeline type with an extra bit, or a binary sync
   carrying a value).
3. **The fd is real and names the syncobj.** `HANDLE_TO_FD` returns
   `FILE_TYPE_SYNCOBJ` with O_RDWR; `FD_TO_HANDLE` from it installs a *second*
   handle on the same record (refs 1 → 2 → 3), and a signal through either
   handle is visible through the other (both directions asserted, and the point
   is read through the kernel seam to show one field, not two copies).
   `SCM_RIGHTS` to a second process works through the existing fd machine
   (`fs_msg_ref_gfd` + `fs_msg_install_gfd` — **no new machinery was needed**,
   so nothing had to be refused here), and a signal in that process is seen in
   the first. Three close orders reach refs 1 → 0 and the record's point is
   readable up to the last close, including with the **node** fd closed first.
4. **`GET_CAP` is honest.** `SYNCOBJ_TIMELINE` now answers 1; the tier asserts it
   in two suites, and items 1–2 are what license it. The deprecated `'N'` face's
   feature mask gained the matching bit.
5. **Nothing unearned started returning 0.** The refusal table lost **exactly**
   the four rows this card serves; `SYNCOBJ_QUERY`, `SYNCOBJ_TRANSFER`,
   `SYNCOBJ_EVENTFD` kept their names and are asserted `-ENOSYS` by name. Every
   other refusal row is untouched. `docs/nv-shim-sync.md` §8 says why each of
   the three stays (a correct `QUERY` needs a submitted-point ledger this card's
   pending table drops at retire; `TRANSFER` needs a `sync_file`; `EVENTFD`
   needs an eventfd registration).
6. **Both unit tiers pass.** See §4.

## 3. Two findings worth keeping

* **`struct drm_syncobj_handle` is 24 bytes, not 16** — it carries a `point`
  field for the timeline forms. The old refusal-only macros happened to use 24,
  so nothing was wrong there; but `struct drm_syncobj_timeline_array` is **24**,
  and the refusal macros for `QUERY`/`TIMELINE_SIGNAL` said 32. That never hurt
  while they were refusals (a refusal is keyed on the number), but serving
  `TIMELINE_SIGNAL` at 32 would have made every real client fail the size check.
  Both are 24 now.
* **`EXPECT_EQ` returns from the test function on failure.** One stale
  expectation (`EXEC` timeline sig == `-ENOSYS`) therefore skipped that suite's
  teardown, leaked three mock objects, and turned every later suite's
  `nv_rm_mock_live() == 0` into a failure: 13 "failures" from one sentence. The
  lesson is in the skill, not just here: a single stale expectation reads as a
  cascade, and the first FAIL line is the only one that matters.

## 4. Gates

Both tiers ran on the code commit **`8a346157`** (`nv/wp-sync`, based on
`nv/wp-prime` @ `e46d257c`); the docs commit follows it and changes no code.

| gate | result |
|---|---|
| `run_unit_tests_intel.sh` (x86_64, TCG, `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900`) | **Tests run: 600 / Tests failed: 0 / UNIT TESTS PASSED** |
| `run_unit_tests.sh` (aarch64 under TCG, `UNIT_TESTS_TIMEOUT=900`) | **Tests run: 479 / Tests failed: 0 / UNIT TESTS PASSED** |
| sha (code / docs) | `8a346157` / the docs commit this file is in |

Baseline: intel 597 / ARM 476 at `46603a46` (WP-PRIME). Delta **+3** on both, one
per new suite (`test_nvshim_drm_syncobj_timeline`, `..._fd`, `..._refusals`).
Logs attached: `unit-tier-intel-sync-600.log`, `unit-tier-arm-sync-479.log`.

No `linux-ci` run was created: this card's constraint is "shim only, no GPU
boot", the two gates below are the ones the card names, and the previous card's
`linux-ci` runs were never started (self-hosted runner offline), so a queued run
would be a claim with no job behind it.

## 5. What this does and does not prove

Proves: the uAPI layout and sizes, the size/route checks, the timeline point as
one field, the completion path as the only source of a point, the fd's identity
with the syncobj, its refcount across handle entries and fds, SCM_RIGHTS, three
close orders, and named refusals for everything not served.

Does **not** prove: that Mesa/nvk drives it (no client was linked or run), that a
`sync_file` can be exported (it cannot — `EXPORT_SYNC_FILE` is `-ENOSYS` by
name, so a Vulkan WSI path still has a wall), or anything about frames: no GPU
was booted and the fence is the mock RM's.

## 6. Merge

`git merge --no-ff nv/wp-sync` (based on `nv/wp-prime`). If `nv/wp-prime` is not
in `nv/integ-swarm` yet, this branch carries it; the two are a pair (PRIME's
buffer + this card's fence) and were built to be merged in that order.
