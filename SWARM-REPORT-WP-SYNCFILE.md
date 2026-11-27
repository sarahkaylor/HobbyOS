# SWARM-REPORT-WP-SYNCFILE — a sync_file is a real fd (t_1f7624e8)

Lane: `nv-card`. Card: `t_1f7624e8`. Base `nv/integ-swarm`: `974519ed` (the
WP-SYNCFAM fold). Branch: `nv/wp-syncfile`. Code commit: `0ee91bd8`.
No GPU boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What landed

`EXPORT_SYNC_FILE` and `IMPORT_SYNC_FILE` are served. The missing object was a
third shim-owned file type, `FILE_TYPE_SYNCFILE`, whose payload is (a syncobj
record, the timeline point the file names); `f1_probe_fd()` asks the shim
whether that record has reached that point, so `poll(2)` reports `POLLIN` when
the fence retires — the same field every `SYNCOBJ_WAIT` compares, never a
private readable bit.

| file | Δ | what |
|---|---|---|
| `src/include/fs.h` | +39/−1 | `FILE_TYPE_SYNCFILE`, the `{obj, point}` payload, the three helper declarations |
| `src/kernel/fs.c` | **+77/−0** | type slot, one probe branch, install/resolve/is, two close hooks. The first shim rung to touch `fs.c`; additive only, nothing restructured |
| `src/kernel/nv_shim.c` | +213/−38 | export/import legs, the wait floor, the readiness + release seams, reason names, its own row bucket |
| `src/include/nv_shim.h` | +28/−2 | the two seams, the two new refusal reasons |
| `src/include/nv_drm_uapi.h` | +5/−0 | the flags are documented as served |
| `src/kernel/nv_shim_drm_test.c` | +397/−7 | `test_nvshim_drm_syncfile` + the two stale `-ENOSYS` expectations |

Total: 6 files, **+759/−48**. `git diff --numstat` is quoted in
`docs/nv-shim-syncfile.md` §2.

## 2. The acceptance, measured

Both tiers ran in this card's worktree, TCG (this uid is not in group `kvm`):

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

| tier | baseline `974519ed` | this rung | delta |
|---|---|---|---|
| intel (x86_64) | `601 / 0` | **`Tests run: 602 / Tests failed: 0 / UNIT TESTS PASSED`** | **+1** |
| aarch64 | `480 / 0` | **`Tests run: 481 / Tests failed: 0 / UNIT TESTS PASSED`** | **+1** |

The delta is `test_nvshim_drm_syncfile`, one per arch. The ARM reloc gate ran on
this tree: `hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s)`, the same
429 as the merged tip.

Logs attached: `unit-tier-intel.log` (602/0), `unit-tier-arm.log` (481/0).

Each acceptance bullet, and where it is asserted:

| acceptance | how it is measured | log evidence |
|---|---|---|
| export on an unsignalled point returns a valid fd | `sobj_export(EXPORT_SYNC_FILE, point 0)` → fd, `file_is_syncfile()==1`, `file_is_syncobj()==0`, payload `==` the record a plain syncobj fd names, `point==1` | `handle_to_fd sync_file=1 … point=0x1 reached=0x0` |
| `poll(2)` not readable before retire, `POLLIN` after, via `file_poll_us`/`file_poll` | `file_poll(pr.p, &pf, 1, 0)` before, during a **held** EXEC, and after the releasing kick | `syncfile binary: file_poll 0/revents 0 while the fence was held, 1/POLLIN after the kick` |
| same for a timeline point | export at 7, held EXEC sig 7, kick | `syncfile timeline@7: …` line |
| `IMPORT_SYNC_FILE` waits on the same point | binary wait `-ETIME` before, `0` after; timeline wait at 3 (below the floor) `-ETIME`; at 7 `0`; the import handle waits 0 only after the kick | `fd_to_handle sync_file=1 … point=0x7` rows + the timeline PASS line |
| dup/close/fork unchanged | `file_dup`, `fs_reopen`/`fs_close_global` (what fork does), `fs_msg_ref`/`fs_msg_install` (SCM_RIGHTS): the record refcount does not move; each close of a copy is a no-op | `syncfile refs: … PASS` |
| last close drops the record reference exactly once | refcount walked 6→5→4→3→2→1→ freed, per step asserted | `syncfile release … refs=0 freed=1` |
| refused rows asserted `-ENOSYS` by name | `syncobj_transfer`, `syncobj_eventfd` through `nv_shim_ioctl` | `drm refuse syncobj_transfer errno=ENOSYS`, `…eventfd…` |

## 3. The correction to the predecessor document

`docs/nv-shim-syncfam.md` §5.2 claimed "the fd table has no poll seam". Measured
in this tree, that is false: `f1_probe_fd()` (`src/kernel/fs.c:1830`) already
dispatches readiness on `f->type`, and `file_poll_us`/`file_poll`
(`src/include/fs.h:564`, `:634`) publish it; `FILE_TYPE_DMABUF` and
`FILE_TYPE_SYNCOBJ` are already shim-owned fds with no poll method of their own.
The seam existed; only the object type did not. The new document
(`docs/nv-shim-syncfile.md` §1.1) states the correction with those file:line
references. The predecessor document itself was not edited (out of this card's
file scope).

## 4. What stayed refused, stated with the reason

* **`SYNCOBJ_TRANSFER` (`0xcc`) — next rung.** Its handle→handle leg needs **no
  sync_file at all** (two timelines, no fd), so it is a separate, smaller rung;
  the row is not half-served here. The sync_file leg of TRANSFER can be built on
  the object this rung adds, but not before the handle→handle leg exists.
* **`SYNCOBJ_EVENTFD` (`0xcf`) — unchanged wall.** The eventfd counter table is
  file-static (`src/kernel/linux_compat_fd.c:1353-1362`) and its only writer
  takes a user pointer (`:1477`); the retire path has a kernel value and no
  fd → counter resolver. Faking it is the §2.13 fake. `-ENOSYS` by name.

Both rows print `-ENOSYS` by name in both tier logs.

## 5. A row-budget fix this rung needed

The first green run hid most of its own evidence: all syncobj rows shared
`r_sync = 128` rows **per boot**, and the earlier syncobj suites spend most of
it. The sync_file legs now have `r_syncfile = 48`
(`src/kernel/nv_shim.c:416-423`), so the export/import/release rows this card
reports cannot be suppressed by an unrelated suite. Verified by grepping both
logs for every row quoted in §2, not by reading the PASS line.

## 6. Constraint check

* files touched: `nv_shim.c`, `nv_shim.h`, `nv_drm_uapi.h`, `fs.h`, `fs.c`, the
  test file — the five source files plus the test, as the card allows;
* `docs/nv-plan.md` untouched (`git diff 974519ed -- docs/nv-plan.md` empty);
* no GPU boot, no VM boot;
* `fs.c` is +77/−0: the type, one probe branch, install/resolve/is, two close
  hooks — no restructuring.
