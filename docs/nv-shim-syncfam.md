# WP-SYNCFAM — `SYNCOBJ_QUERY` answers from a submitted-point ledger

Card: `t_0903a106`. Base: `nv/integ-swarm` @ `8efe5ae2` (the WP-FOLD tip, i.e.
WP-PRIME + WP-SYNC merged). Branch: `nv/wp-syncfam`. Shim only: no GPU boot, no
VM boot, no `docs/nv-plan.md` edit.

## 1. What this rung is, and what it is not

`t_8acf4a21` served four of the seven syncobj rows and left three named
refusals: `SYNCOBJ_QUERY`, `SYNCOBJ_TRANSFER`, `SYNCOBJ_EVENTFD`. This card
serves **QUERY** and leaves the other two refused, each for a reason measured
below (§5). Of the card's two scope items, one landed and one did not, and the
reason the second did not is a missing kernel seam rather than a decision:

| row | nr | before | after | why |
|---|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_QUERY` | `0xcb` | `-ENOSYS` by name | **served** | §3: a submitted-point ledger answers what the pending table cannot |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | `0xcf` | `-ENOSYS` by name | `-ENOSYS` by name | §5: no kernel-side eventfd counter writer exists to call from the retire path |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | `0xcc` | `-ENOSYS` by name | `-ENOSYS` by name | §5: needs a sync_file object (and the sync_file leg of the same object type) |

The card's own fallback clause is what authorises the EVENTFD half staying a
wall: *"If the eventfd object cannot be signalled from the shim's retire path
(check `src/kernel/linux_compat_fd.c` and `src/include/fs.h` for the writer), do
not improvise: report the missing seam with file:line, keep `syncobj_eventfd` a
named refusal, and land QUERY alone."* That check was run and it came back
negative; §5 is the report, with line numbers.

## 2. The row-table diff

Serving a DRM row is two table edits plus a dispatch case (the WP-SYNC
lesson: a row left in `g_drm_refusals[]` is answered `-ENOSYS` before any
argument is read, so it can never be served, and a row served with the wrong
encoded width makes every real client `-EINVAL`):

| what | where (post-change) |
|---|---|
| `g_drm_rows[]` entry `{0xcb, 24}` | `src/kernel/nv_shim.c:1466` (table starts `:1439`) |
| `g_drm_refusals[]` no longer lists `syncobj_query` | `src/kernel/nv_shim.c:1481` (table), reasons `:1493-1500`, rows `:1501-1502` |
| `case 0xcb: op_drm_syncobj_query` | `src/kernel/nv_shim.c:4799` |
| handler `op_drm_syncobj_query()` | `src/kernel/nv_shim.c:3757` |

`24` is `drm.h`'s own encoded size: `DRM_IOCTL_SYNCOBJ_QUERY` is
`DRM_IOWR(0xcb, struct drm_syncobj_timeline_array)` — the SAME struct
`SYNCOBJ_TIMELINE_SIGNAL` takes, so no new uAPI struct was invented and
`0xC01864CB` is the request number a real libdrm client sends. The header now
spells it that way (`src/include/nv_drm_uapi.h`, the served-row block) and
carries `NVSHIM_DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED`.

The refusal table after this rung, with its comment quoted (`:1493-1500`):

```
    /* WP-SYNC: 0xc1 / 0xc2 / 0xca / 0xcd are SERVED now (the row table above),
     * and WP-SYNCFAM added 0xcb (QUERY) to it.  What remains of the syncobj
     * surface is named here, and both rows are the SAME wall: a sync_file
     * object -- a file whose poll() answers POLLIN when a fence signals.
     * TRANSFER moves a point between timelines including through a sync_file
     * import, and EVENTFD registers an eventfd to be written when a point
     * signals; this node has neither object, and the eventfd half has no
     * kernel-side counter writer to reach (docs/nv-shim-syncfam.md §5). */
    {NVSHIM_DRM_NR_SYNCOBJ_TRANSFER, "syncobj_transfer"},
    {NVSHIM_DRM_NR_SYNCOBJ_EVENTFD, "syncobj_eventfd"},
```

## 3. QUERY: the ledger, and what the answer means

### 3.1 The uAPI shape (checked against the source, not against memory)

`include/uapi/drm/drm.h`:

```c
#define DRM_IOCTL_SYNCOBJ_QUERY  DRM_IOWR(0xCB, struct drm_syncobj_timeline_array)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0) /* last available point on timeline syncobj */
```

`struct drm_syncobj_timeline_array` is `handles`, `points`, `count_handles`,
`flags` — 24 bytes. **There is no input point**: `points` is the kernel's
OUTPUT array, one `u64` per handle. `drm_syncobj_query_ioctl()`
(`drivers/gpu/drm/drm_syncobj.c`) validates `flags & ~LAST_SUBMITTED` and a zero
`count_handles`, resolves every handle with `drm_syncobj_array_find()` (all or
nothing), and then per handle writes

* with `LAST_SUBMITTED`: the chain head's `seqno` — the newest point *submitted*;
* without it: the newest point *signalled* (walking the chain past unsignalled
  links);
* `0` when the fence is not a chain at all (no timeline, or a host-side stub).

That is the shape this node implements, with the chain replaced by two marks.

### 3.2 The ledger

`struct shim_sobj` (`src/kernel/nv_shim.c:245`) gained

```c
  uint64_t submitted;
```

next to the existing `point`. The two marks answer two different questions:

| mark | written when | answers |
|---|---|---|
| `point` | a fence retires (`client_reap_locked`, `src/kernel/nv_shim.c:3247`), `TIMELINE_SIGNAL`, binary `SIGNAL`, `CREATE_SIGNALED`, `RESET` | "what point has this object reached?" — the field the wait rows already compare against |
| `submitted` | at SUBMIT time: an `EXEC`'s accepted sig (`src/kernel/nv_shim.c:4254`, before the fence can retire) and `TIMELINE_SIGNAL` (`:3722`) | "what point did you last submit?" — QUERY's `LAST_SUBMITTED` |

The reason the second mark has to exist is the one `docs/nv-shim-sync.md` §8
gave for refusing the row: the *pending table* drops its entry at retire
(`client_reap_locked` erases the entry as soon as the fence is done), and that
is exactly the moment a consumer asks what was submitted. A ledger written at
retire would forget precisely the points being queried, so it is written at
submit. The order check that used to scan the pending table
(`sobj_submitted_locked()`, `src/kernel/nv_shim.c:3240`) now reads the ledger,
because a pending point is already in it.

Two host-side rows deliberately do **not** write the ledger, because in Linux
they install a fence with no chain:

* `CREATE_SIGNALED` and binary `SYNCOBJ_SIGNAL` set `point = 1` and leave
  `submitted` alone (`src/kernel/nv_shim.c:3360`, `:3856-3867`);
* binary `SYNCOBJ_RESET` clears BOTH (`:3867`), because Linux's reset replaces
  the fence with NULL — the chain, and every point in it, is gone.

### 3.3 Answers and refusals

| case | answer |
|---|---|
| fresh record, either mode | `0` — the pre-submit value; a real value written into `points[]`, not an untouched buffer |
| after `TIMELINE_SIGNAL` 5 | both modes `5` (a timeline signal *adds* a point: `drm_syncobj_add_point` with a signalled stub) |
| `EXEC` signalling 11, fence held | `LAST_SUBMITTED` → `11`; no flag → `5` (the pre-submit signalled value) |
| … after that fence retires | both `11`, with the pending entry already gone |
| `RESET` | both `0` |
| `flags` other than `LAST_SUBMITTED` | `-EINVAL`, `reason=bad_flags` |
| `count_handles` 0 or > 64 | `-EINVAL`, `reason=bad_shape` |
| `points == 0` (nowhere to write) | `-EINVAL`, `reason=bad_shape` |
| a handle this client does not hold | `-EINVAL`, `reason=not_this_clients_syncobj`, **nothing written for the batch** |
| `LAST_SUBMITTED` on a point the ledger has no entry for | `-EINVAL`, `reason=point_beyond_ledger`, nothing written |

`point_beyond_ledger` (`src/include/nv_shim.h`, reason 12;
`src/kernel/nv_shim.c:3798`) is the card's "refuse a point beyond the ledger by
name, never return 0". It fires when the object has *reached* a point the
submitted ledger never recorded — which this node can produce honestly, because
`CREATE_SIGNALED` and binary `SIGNAL` are host-side stubs. Linux answers `0`
there (a stub fence carries no chain), and `0` is exactly what "nothing was ever
submitted" answers, so this node names the gap instead of emitting a value a
client cannot tell from an empty timeline. The signalled-mode answer for the
same object is the model's point `1`, and that is what the suite asserts.

Consequently, a batch is atomic in the same sense Linux's is: every entry is
resolved before the first byte is written, so a refusal leaves the caller's
`points[]` untouched (asserted with a sentinel in both refusal cases).

## 4. Two divergences from Linux, stated

1. **A host-side binary signal is point 1 here, and "no chain" there.** Linux's
   `SYNCOBJ_QUERY` on an object whose fence is the stub answers `0` in both
   modes. This node's one-record model (WP-SYNC §3) says the object is at point
   1, and every wait row already reads it that way; so the signalled mode
   answers `1`, and `LAST_SUBMITTED` refuses by name as above rather than
   answering `0`.
2. **A record with nothing submitted answers `0` in `LAST_SUBMITTED` mode.** That
   is Linux's answer and it is the truth ("no point submitted"); it is not a
   half-answer, and it is what "answer the pre-submit value before an EXEC"
   means in the card's own words.

Both are in the tier as measured values (see §6), not as comments.

## 5. What stayed refused, and the missing seam, with file:line

### 5.1 `SYNCOBJ_EVENTFD` — the eventfd object exists; the writer does not reach it

The acceptance for EVENTFD is *"an eventfd registered at point N reads 8 bytes
once N signals, reads nothing before that, and the lazy registration path is
asserted too"*. It cannot be met from a shim-only change, and here is the
measurement:

1. **The counter lives in a file-static table.** `src/kernel/linux_compat_fd.c:1353-1362`
   defines `static struct { int used; int sem; uint64_t count; … } l_efd[1024];`.
   Nothing outside that TU can touch `count`.
2. **The only writer takes a USER pointer.** `src/kernel/linux_compat_fd.c:1477`
   `int l_eventfd_write(struct file *f, const void *buf, int size)` adds to the
   counter, but at `:1482` it does
   `l_copy_from_user(current_process(), &v, (uint64_t)(uintptr_t)buf, 8)` — `buf`
   is a *user* address, validated by `vm_range_ok()` (`src/kernel/linux_compat.c:71-79`).
   The shim's retire path has a kernel value, not a user address, so calling it
   there would return `-EFAULT` at best and read the wrong address space at
   worst.
3. **The published knobs do not include the counter.** `src/include/fs.h:333`
   publishes exactly one eventfd function, `file_eventfd_create()`; the other
   exported names are `l_eventfd_key()` (`:1394`), `l_eventfd_release()`
   (`:1398`) and `l_eventfd_probe()` (`:1406`) — a read-only probe, plus
   `l_eventfd_new()` (`:1375`). None of them adds to the counter.
4. **There is no fd → `struct file *` resolver.** The dispatch into the writer is
   `src/kernel/fs.c:4346-4348` (`FILE_TYPE_EVENTFD` → `l_eventfd_write`),
   reachable only through `file_write(process *, int fd, const void *user_buf,
   int size, tf)` (`src/kernel/fs.c:4239`); the slot lives in the file
   description (`struct file`'s `eventfd.slot`, `src/include/fs.h:214-215`) whose
   table is addressed by `static inline struct file *gfile()` inside `fs.c:52`.
   A shim ioctl cannot reach `f->eventfd.slot`.

The missing seam is therefore two functions that do not exist — a kernel-side
`l_eventfd_signal(slot, value)` (add + wake readers/pollers without a user copy)
and a resolver such as `file_eventfd_slot(process *, fd, int *slot)` — both in
`linux_compat_fd.c`/`fs.c`, i.e. outside this card's shim-only scope. Faking it
was rejected: an eventfd the shim "signals" without touching the counter is the
§2.13 fake this project forbids (a client's `read(2)` would return `EAGAIN`
forever while the log claims a wake).

### 5.2 `SYNCOBJ_TRANSFER` and the sync_file trio — no sync_file object type

`EXPORT_SYNC_FILE`, `IMPORT_SYNC_FILE` and `TRANSFER`'s sync_file leg all need a
**sync_file object**: a file whose `poll()` answers `POLLIN` when the fence
signals. This node has no such object and the fd table has no poll seam for one;
a sync_file fd whose only observable is `close()` is the fake §2.13 forbids. All
three stay refused by name (`syncobj_transfer`, `syncobj_eventfd`, and the two
sync_file flags inside the fd rows), and that is unchanged from WP-SYNC.

## 6. Measured

Both tiers ran in the card's worktree on this commit's code, with the card's own
invocation. No KVM on this box (this uid is not in group `kvm`; `/dev/kvm` is
`root:kvm 660`), so both tiers ran TCG.

| tier | invocation | baseline (same tree, `8efe5ae2`) | this rung | delta |
|---|---|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `Tests run: 600 / Tests failed: 0` | `Tests run: 601 / Tests failed: 0 / UNIT TESTS PASSED` | +1 |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` (TCG) | `Tests run: 479 / Tests failed: 0` | `Tests run: 480 / Tests failed: 0 / UNIT TESTS PASSED` | +1 |

The new suite is `test_nvshim_drm_syncobj_query` — one per arch, which is the
whole delta. Logs: `unit-tier-intel-syncfam-601.log`,
`unit-tier-arm-syncfam-480.log` (attached to the card).

The baseline was measured **in the same worktree before any edit** (intel, at
`8efe5ae2`) and in a detached worktree of the same commit (ARM, at `8efe5ae2`);
the only source difference between those trees and the measured one is this
rung's own diff (`git diff --stat 8efe5ae2 -- src/` lists exactly the four shim
files below).

The rows that carry the acceptance, verbatim from the intel log:

```
[NVSHIM] drm syncobj create handle=1 point=0x0 obj=10
[NVSHIM] drm syncobj query n=1 point=0x0 last_submitted=1      <- pre-submit value
[NVSHIM] drm syncobj query n=1 point=0x0 last_submitted=0
[NVSHIM] drm syncobj timeline_signal n=1 point=0x5
[NVSHIM] drm syncobj query n=1 point=0x5 last_submitted=1
[NVSHIM] drm syncobj query n=1 point=0x5 last_submitted=0
[NVSHIM] drm exec ch=1 pushes=1 sigs=1 bytes=64 stream=0xdfde6ac5 fence=0x1   <- held
[NVSHIM] drm syncobj query n=1 point=0xb last_submitted=1      <- submitted, not reached
[NVSHIM] drm syncobj query n=1 point=0x5 last_submitted=0      <- pre-submit signalled
[NVSHIM] drm exec ch=1 pushes=1 sigs=0 bytes=64 stream=0xdfde6ac5 fence=0x2   <- kick: retire
[NVSHIM] drm syncobj query n=1 point=0xb last_submitted=1
[NVSHIM] drm syncobj query n=1 point=0xb last_submitted=0      <- reached; entry gone
[NVSHIM] drm syncobj query n=1 point=0xc last_submitted=1
[NVSHIM] drm syncobj query n=2 point=0xc last_submitted=1      <- two handles, in order
[NVSHIM] drm syncobj refuse query handle=0x385 reason=not_this_clients_syncobj
[NVSHIM] drm syncobj refuse query handle=0x2 reason=point_beyond_ledger
[NVSHIM] drm syncobj query n=1 point=0x1 last_submitted=0      <- CREATE_SIGNALED, no-flag
[NVSHIM] drm syncobj refuse query handle=0x2 reason=point_beyond_ledger
[NVSHIM] drm syncobj query n=1 point=0x0 last_submitted=1      <- after RESET
[NVSHIM] drm syncobj query n=1 point=0x0 last_submitted=0
[NVSHIM] drm syncobj refuse query flags=0x80 reason=bad_flags
[NVSHIM] drm syncobj refuse query count=0x0 reason=bad_shape
[NVSHIM] drm syncobj refuse query points=0x0 reason=bad_shape
```

and the two rows that stay refused, in the same log:

```
[NVSHIM] drm refuse syncobj_transfer errno=ENOSYS
[NVSHIM] drm refuse syncobj_eventfd errno=ENOSYS
```

Row budget: QUERY rows use WP-SYNC's `r_sync` bucket (128 rows per boot). The
new suite prints ~20 of them and the bucket was not exhausted — checked by
grepping the log for every row above rather than by reading the PASS line (a
spent bucket greens a suite while hiding its evidence). The DRM face's *own*
refusal row had the same disease for a different reason and now has its own
bucket: `"drm refuse <name> errno=ENOSYS"` shared `r_refuse`, which is 8 rows
per boot and is spent by the `'N'` face before the syncobj suites run, so the
two rows this card is about (`syncobj_transfer`, `syncobj_eventfd`) never
printed. `r_drm_refuse` (`src/kernel/nv_shim.c:402-409`) is 24 and covers both
dispatch refusal shapes.

### 6.1 The stale expectations this rung had to change

The WP-SYNC lesson is that one stale `EXPECT_EQ` returns from the test function
(a failed assertion in this framework *returns*), skips teardown, and reads as a
cascade of unrelated failures. Five sites asserted QUERY was `-ENOSYS`; all five
were updated in the same commit, before the first run:

| suite | assertion before | after |
|---|---|---|
| `test_nvshim_drm_layout` | `req_size(SYNCOBJ_QUERY) == 0`, `refusal_name != 0`, no literal | `== 24`, `refusal_name == 0`, `0xC01864CB` (`src/kernel/nv_shim_drm_test.c:146, :170, :194`) |
| `test_nvshim_drm_refusals` | `nv_shim_ioctl(0,-1,QUERY,0) == -ENOSYS` | `== -EINVAL` (served; no process is the served-row answer) (`:245`) |
| `test_nvshim_drm_prime_refusals` | `QUERY == -ENOSYS`, `refusal_name != 0` | `QUERY` is not in that list; `SYNCOBJ_TRANSFER` is (`:1906`) |
| `test_nvshim_drm_syncobj_refusals` | `QUERY == -ENOSYS` by name, in the "still refused" block | block holds `TRANSFER` + `EVENTFD` only; `point_beyond_ledger` added to the name table |

## 7. Verification gaps

* no Mesa/nouveau client was linked or run: QUERY is exercised through the real
  route table, the real encoded request number and a real process, but the
  winsys use (`vkGetSemaphoreCounterValue` on a timeline semaphore) is
  source-level;
* no `sync_file` and no eventfd wake, so a Vulkan WSI poll path still has the
  wall §5 describes — named, not silent;
* no GPU boot: the fences are the mock RM's, retired by `kick()`;
* the record table stays 32 node-wide and QUERY refuses a batch wider than 64
  handles by name rather than splitting it;
* QUERY answers the *object's* two marks; it does not model per-point
  availability (`WAIT_AVAILABLE` is still `-ENOSYS` by name), which a
  `WAIT_FOR_SUBMIT`/`AVAILABLE` client would need.
