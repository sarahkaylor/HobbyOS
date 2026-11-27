# WP-SYNCFILE — a sync_file is a real fd: `poll(2)` answers `POLLIN` at retire

Card: `t_1f7624e8`. Base: `nv/integ-swarm` @ `974519ed` (the WP-SYNCFAM fold,
i.e. WP-PRIME + WP-SYNC + SYNCOBJ_QUERY). Branch: `nv/wp-syncfile`. Shim + one
new fd type only: no GPU boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What this rung is, and what it is not

WP-SYNC served the syncobj fd and WP-SYNCFAM served QUERY, but both left the two
rows that need a *dma_fence as a file* as named refusals. This card serves them:

| row | flags | nr | before | after |
|---|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | `EXPORT_SYNC_FILE` | `0xc1` | `-ENOSYS`, reason `export_sync_file` | **served** — returns a real `FILE_TYPE_SYNCFILE` fd |
| `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` | `IMPORT_SYNC_FILE` | `0xc2` | `-ENOSYS`, reason `import_sync_file` | **served** — returns a handle whose `SYNCOBJ_WAIT` observes that file's point |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | — | `0xcc` | `-ENOSYS` by name | `-ENOSYS` by name (out of scope, §5) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | — | `0xcf` | `-ENOSYS` by name | `-ENOSYS` by name (out of scope, §5) |

The plain (no-flag) legs of `0xc1`/`0xc2` are unchanged: they still export and
import a **syncobj** fd. The flag picks which object comes out, exactly as in
Linux, and the two are different file types on purpose — the test asserts
`file_is_syncfile() == 1 && file_is_syncobj() == 0` on an exported sync_file fd.

### 1.1 The predecessor document was wrong in one measurable way

`docs/nv-shim-syncfam.md` §5.2 says:

> `EXPORT_SYNC_FILE`, `IMPORT_SYNC_FILE` and `TRANSFER`'s sync_file leg all need
> a **sync_file object** … This node has no such object **and the fd table has no
> poll seam for one** …

The second half is false in this tree. The seam exists; only the object type did
not:

* `src/kernel/fs.c:1830` `f1_probe_fd()` already dispatches readiness on
  `f->type` (TIMERFD … EVENTFD, DEVSND, SIGNALFD branches), and `file_poll_us` /
  `file_poll` (`src/include/fs.h:564`, `:634`) are the public surfaces over it.
* `FILE_TYPE_DMABUF` and `FILE_TYPE_SYNCOBJ` already prove a shim-owned fd can be
  an ordinary file description with no `poll` method of its own.

So the missing piece was exactly what the card said it was: a third file type.
No new poll engine, no private readiness flag, no `poll_notify` key.

## 2. The fd type: the whole `fs.c` diff

`src/kernel/fs.c` is **+77 lines, 0 deletions** (`git diff --numstat`), all of it
the new type. This is the first shim rung to touch `fs.c`; nothing was
restructured.

| what | where |
|---|---|
| `FILE_TYPE_SYNCFILE` (appended last: no values shift) | `src/include/fs.h:147` |
| slot payload `{ void *obj; uint64_t point; }` — the record **and** the point it names | `src/include/fs.h:270-276` |
| `file_syncfile_install / _obj / _is` | `src/include/fs.h:706-708`, `src/kernel/fs.c:5103-5142` |
| **one** probe branch in `f1_probe_fd()` | `src/kernel/fs.c:1951-1961` |
| close hook (description's last close) | `src/kernel/fs.c:3272-3276`, release at `:3297` |
| fd-table teardown hook (no process context: `fd = -1`) | `src/kernel/fs.c:3461-3465`, release at `:3477` |

The probe branch is the readiness answer and nothing else:

```c
  if (f->type == FILE_TYPE_SYNCFILE) {
    if (!f->syncfile.obj) return -1;
    if (nv_shim_syncfile_ready(f->syncfile.obj, f->syncfile.point)) *r = 1;
    return 0;
  }
```

`nv_shim_syncfile_ready()` (`src/kernel/nv_shim.c:2806`) takes `g_shim_lock` and
compares the record's **`point`** — the same field `sobj_wait_ready()` compares
for every wait row. Retirement is still the shim's single completion path
(`client_reap_locked()`, `src/kernel/nv_shim.c:3320`), driven by the mock RM's
`kick()`; the probe only observes it. That is the line between "a poll seam over
the real state" and the §2.13 fake: there is no separate readable bit to set, so
poll and `SYNCOBJ_WAIT` cannot disagree — and the suite asserts that agreement
while the fence is held.

Because the slot is an ordinary file description, `dup(2)`, `fork` (via
`fs_reopen()`) and `SCM_RIGHTS` need no per-type code: they all bump
`struct file::ref_count`, and the record reference is dropped once, at the last
close (`nv_shim_syncfile_release()`, `src/kernel/nv_shim.c:2820`).

## 3. The two legs

### 3.1 `EXPORT_SYNC_FILE` names a (record, point)

| flags | point accepted | fd names | `poll` readable when |
|---|---|---|---|
| `EXPORT_SYNC_FILE` (binary) | must be 0 | point **1** — a binary syncobj's fence is that record at 1 | record `point >= 1` |
| `EXPORT_SYNC_FILE\|TIMELINE` | must be `>= 1` | the caller's point | record `point >= point` |

Implementation: `src/kernel/nv_shim.c:3995-4066`. The record gains one reference
before the fd exists and loses it if installation fails or the write-back faults
(`file_close()` semantics), exactly like the existing syncobj-fd leg.

Refusals (all `-EINVAL`, each named): a point with no `TIMELINE` flag
(`point_without_timeline`, new reason 14), a timeline point of 0
(`timeline_point_zero`), a handle this client does not hold
(`not_this_clients_syncobj`), a spent fd slot (`-EMFILE`, row
`reason=export_sync_file`).

### 3.2 `IMPORT_SYNC_FILE` turns that fd back into a handle

The fd must be a **sync_file** fd (`src/kernel/nv_shim.c:4124-4200`). The import
installs a new handle entry on the same record with a per-entry **floor**: the
point the file named (binary form), or the point the caller stated with
`TIMELINE`. `sobj_wait_ready()` (`src/kernel/nv_shim.c:3518-3545`) compares
`record.point >= max(caller_point, floor)`, so:

* a **binary** `SYNCOBJ_WAIT` (point 1) on a file exported at 7 still waits for
  **7** — this is what makes the imported fence the one the wait observes;
* a timeline wait **below** the floor (3) is not satisfied by reaching 3;
* reaching 7 satisfies both imported handles and the poll on the same retire.

Refusals: a non-sync_file fd (`not_a_sync_file`, new reason 13 — the node, a
plain syncobj fd, a dma-buf), point without `TIMELINE`
(`point_without_timeline`), timeline point 0 (`timeline_point_zero`), a spent
handle table (`-ENOSPC`, `no_syncobj_slot`).

## 4. Divergences from Linux, stated

1. **Binary export of an unsignalled syncobj is a valid fd here; Linux answers
   `-EINVAL`.** Linux's binary syncobj with no fence has nothing to export
   (`drm_syncobj_handle_to_fd` returns `-EINVAL` when `drm_syncobj_fence_get()`
   is NULL). This node's one-record model (WP-SYNC §3) says an unsignalled
   record is point 0 and a binary fence is point 1, so the fd exists and simply
   is not readable yet. That is the card's own acceptance ("returns a valid fd;
   `poll` reports not readable before the fence retires"), and it is what a
   binary `SIGNAL` / `EXEC` sig later moves.
2. **An imported handle aliases the exporter's timeline.** Linux's import
   creates a *new* syncobj whose only fence is the one in the sync_file, so a
   timeline wait past the imported point can never be satisfied. Here the handle
   names the exporter's record, so if the exporter later signals a higher point
   than the one imported, a wait for it succeeds. Imported points at or below
   the floor behave identically in both models (the observed fence is the
   imported one), which is what the acceptance measures.
3. **No `POLLHUP`/error channel.** A sync_file poll is readable-only here; a
   fence that can never retire (a destroyed channel) simply never reads
   readable, rather than reporting `POLLERR`. Linux's sync_file has no error
   path either, so this is a mock limitation, not a divergence from the uAPI.

## 5. What stayed refused, and why (with file:line)

### 5.1 `SYNCOBJ_TRANSFER` — the next rung, and it needs no sync_file

`TRANSFER` moves a timeline point from one syncobj to another. Its
**handle→handle leg needs no sync_file at all** (two timelines, no fd), so it is
a separate, smaller rung — the next one — not a half-served row in this card.
The sync_file leg of `TRANSFER` (a transfer through an imported fence) can be
built on the object this rung adds, but the row cannot be served until the
handle→handle leg is, so the row stays `-ENOSYS` with its name
(`src/kernel/nv_shim.c:1518`, `:1519`; `row reason=syncobj_transfer` in both
tier logs).

### 5.2 `SYNCOBJ_EVENTFD` — the missing kernel writer is unchanged

The wall is exactly where `docs/nv-shim-syncfam.md` §5.1 measured it:
`src/kernel/linux_compat_fd.c:1353-1362` keeps the eventfd counter table
file-static (`static struct { … } l_efd[1024]`), and the only writer,
`l_eventfd_write()` (`src/kernel/linux_compat_fd.c:1477`), takes a **user**
pointer. The shim's retire path has a kernel value and no fd → counter
resolver, so there is nothing to call. Faking it would leave a client's
`read(2)` returning `EAGAIN` forever while the log claims a wake — the §2.13
fake. `-ENOSYS` by name, unchanged.

### 5.3 Why the refusal comment changed, and the reason names

`g_drm_refusals[]` no longer claims the two rows are the same sync_file wall
(`src/kernel/nv_shim.c:1508-1519`). The reason table gained
`not_a_sync_file` (13) and `point_without_timeline` (14)
(`src/kernel/nv_shim.c:3373-3374`; `src/include/nv_shim.h:410-413`). The
historical `export_sync_file` / `import_sync_file` names (2, 3) stay in the
table: they now name a **refused** sync_file leg — the export row uses
`export_sync_file` when the fd slot cannot be installed, and the names remain
the contract the tier asserts.

## 6. Measured

Both tiers ran in this card's worktree on the code commit below, with the card's
own invocations. No KVM (this uid is not in group `kvm`; `/dev/kvm` is
`root:kvm 660`), so both tiers ran TCG.

| tier | invocation | baseline (merged tip `974519ed`) | this rung | delta |
|---|---|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `601 / 0` | `Tests run: 602 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` | `480 / 0` | `Tests run: 481 / Tests failed: 0 / UNIT TESTS PASSED` | **+1** |

The delta is `test_nvshim_drm_syncfile` — one per arch. The ARM reloc gate ran
and passed on this tree: `hobbyos.elf: reloc gate OK -- 429 kernel pointer
word(s)`, the same 429 the merged tip reported.

Logs: `unit-tier-intel.log`, `unit-tier-arm.log` (attached to the card).

### 6.1 The rows that carry the acceptance (intel, verbatim)

```
[NVSHIM] drm syncobj handle_to_fd sync_file=1 handle=1 fd=5 obj=14 point=0x1 reached=0x0
  [NVSHIM] syncfile binary: file_poll 0/revents 0 while the fence was held, 1/POLLIN after the kick retired it
[NVSHIM] drm syncobj handle_to_fd sync_file=1 handle=2 fd=7 obj=15 point=0x7 reached=0x0
[NVSHIM] drm syncobj fd_to_handle sync_file=1 fd=7 handle=3 obj=15 point=0x7
[NVSHIM] drm syncobj fd_to_handle sync_file=1 fd=7 handle=4 obj=15 point=0x7
  [NVSHIM] syncfile timeline@7: file_poll 0/revents 0 before the kick, 1/POLLIN after; imported SYNCOBJ_WAIT -ETIME -> 0 on the same retire
[NVSHIM] drm syncobj fd_to_handle sync_file=1 fd=4 handle=1 obj=15 point=0x7   <- the SCM_RIGHTS receiver's table
[NVSHIM] drm refuse syncobj_transfer errno=ENOSYS
[NVSHIM] drm refuse syncobj_eventfd errno=ENOSYS
[NVSHIM] syncfile release obj=14 fd=5 point=0x1 reached=0x1 refs=0 freed=1
[NVSHIM] syncfile release obj=15 fd=7 point=0x7 reached=0x7 refs=0 freed=1
  [NVSHIM] syncfile refs: dup/fork/SCM_RIGHTS kept ONE description and ONE record reference; the last close freed each record once: PASS
```

`reached=0x0` on both export rows is the whole point: the file was exported
before anything retired, and the poll said "not readable" until the kick.

### 6.2 The row budget this rung had to fix

The first green run hid most of the evidence: every syncobj row shares
`r_sync = 128` rows **per boot**, and the timeline + fd + refusals suites spend
most of it before this suite runs. That is the same disease WP-SYNCFAM fixed for
`drm refuse`. The sync_file legs now have their own bucket,
`r_syncfile = 48` (`src/kernel/nv_shim.c:416-423`), used by the two export
rows, the two import rows, the export-failure refusal row and the release rows —
so a green suite cannot suppress the rows it is evidence for. Checked by
grepping both logs for each row above, not by reading the PASS line.

### 6.3 The stale expectations this rung had to change

Three sites asserted the two flags were `-ENOSYS`; all three were updated in the
same commit before the first run:

| suite | before | after |
|---|---|---|
| `test_nvshim_drm_syncobj_refusals` (HANDLE_TO_FD) | `EXPORT_SYNC_FILE` → `-ENOSYS` | served; malformed point forms `-EINVAL` |
| `test_nvshim_drm_syncobj_refusals` (FD_TO_HANDLE) | `IMPORT_SYNC_FILE` on a plain syncobj fd → `-ENOSYS` | `-EINVAL` (`not_a_sync_file`) |
| `test_nvshim_drm_syncobj_refusals` (names) | — | `not_a_sync_file`, `point_without_timeline` added to the name table |

## 7. Verification gaps

* no Mesa/nouveau client was linked or run: the rows are driven through the real
  route table, the real encoded request numbers (`0xC01864C1` / `0xC01864C2`) and
  real processes, but the winsys use (`vkGetSemaphoreFdKHR` /
  `vkImportSemaphoreFdKHR`, WSI) is source-level;
* `select(2)` uses the same `f1_probe_fd()` branch but the suite drives
  `file_poll()` only; `poll(2)` is the measured surface;
* no GPU boot: the fences are the mock RM's, retired by `kick()`, so "retire" is
  a synchronous kick rather than an interrupt;
* a parked `poll()` on a sync_file re-scans on the generic poll slice (10 ms)
  rather than being woken by a poll key — correct, but not woken early;
* the import aliases the exporter's record (§4.2), so it does not model a
  frozen per-import fence chain;
* `TRANSFER`'s sync_file leg is buildable on this object but still refused
  (§5.1); the handle→handle leg is the next rung.
