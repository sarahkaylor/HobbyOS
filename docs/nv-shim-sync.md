# WP-SYNC — explicit fences on the `/dev/nvshim` shim

Card: `t_8acf4a21`. Base: `nv/wp-prime` @ `e46d257c` (itself `4a70f062`
`nv/integ-swarm` + the WP-PRIME rung). Branch: `nv/wp-sync`.

## 1. What this rung is, and what it is not

PRIME gave the shim the buffer half of a swapchain: a GEM object exports to a
real fd and imports back to the same pages. What it left was the *fence* half:
a consumer that receives a PRIME buffer also needs a fence **as an fd**, so it
can wait on our completion without sharing our address space. Before this card
all seven syncobj rows past the binary ones answered `-ENOSYS` by name
(`syncobj_handle_to_fd`, `syncobj_fd_to_handle`, `syncobj_timeline_wait`,
`syncobj_query`, `syncobj_transfer`, `syncobj_timeline_signal`,
`syncobj_eventfd`), and an `EXEC` carrying a timeline sync was refused with
`reason=timeline_syncobj` ("binary syncobjs only").

This card serves four of those seven — the two directions of the syncobj fd and
the two timeline rows — and makes `EXEC` accept a real `timeline_value`. It is
an **ABI rung, not an fps row**: no GPU was booted, the engine-class wall
(`need=gsp_rm`) is untouched, and a timeline point retiring on a mock fence is
not evidence that a frame was rendered.

## 2. The contract as implemented

`src/include/nv_drm_uapi.h` gained three structs, copied field for field from
`include/uapi/drm/drm.h`:

| struct | bytes | fields |
|---|---|---|
| `nvshim_drm_syncobj_timeline_array` | 24 | `handles`, `points`, `count_handles`, `flags` |
| `nvshim_drm_syncobj_timeline_wait` | 48 | + `timeout_nsec`, `first_signaled`, `pad`, `deadline_nsec` |
| `nvshim_drm_syncobj_handle` | 24 | `handle`, `flags`, `fd`, `pad`, **`point`** |

`struct drm_syncobj_handle`'s `point` field is why the encoded size is 24 and not
16; the old refusal-only macros used 24 for the same reason. The two timeline
rows' *refusal* macros were 32 bytes, which is not a size any client sends:
`drm_syncobj_timeline_array` is 24, so `SYNCOBJ_QUERY`/`SYNCOBJ_TIMELINE_SIGNAL`
were re-encoded at 24 (they are keyed on the number, so nothing regressed — but
now a probe built from this header is a request a real client could send).

| request | nr | encoded size | answer |
|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL` | `0xcd` | `0xC01864CD` (24) | **served** |
| `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT` | `0xca` | `0xC03064CA` (48) | **served** |
| `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | `0xc1` | `0xC01864C1` (24) | **served** |
| `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` | `0xc2` | `0xC01864C2` (24) | **served** |
| `DRM_IOCTL_SYNCOBJ_QUERY` | `0xcb` | `0xC01864CB` (24) | refused by name |
| `DRM_IOCTL_SYNCOBJ_TRANSFER` | `0xcc` | `0xC02064CC` (32) | refused by name |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | `0xcf` | `0xC01864CF` (24) | refused by name |

The four served rows left `nv_shim_drm_refusal_name()`; the three that stayed
kept theirs. Why those three stay is in §8.

## 3. One object, one point

A syncobj here is **one record with one 64-bit point** (`struct shim_sobj`,
`nv_shim.c`). That is not a simplification of Linux's model — it is that model
with the fence-pointer replaced by the point the fence would carry: in Linux a
"binary" syncobj is a syncobj whose fence is NULL or the stub, i.e. the same
object at point 0 or 1, and `DRM_SYNCOBJ_CREATE_SIGNALED` is exactly "point 1".

Consequences, all asserted in the tier:

* binary `SYNCOBJ_SIGNAL` writes point 1; binary `SYNCOBJ_RESET` writes point 0;
* a binary `SYNCOBJ_WAIT` is `point >= 1`, so a syncobj a timeline signal left
  at 5 satisfies it (as a non-NULL fence would in Linux);
* `SYNCOBJ_TIMELINE_SIGNAL` sets the point to `points[i]`;
* `SYNCOBJ_TIMELINE_WAIT` waits for `point >= points[i]`.

There is no second state machine to drift from the first, which is what makes
"a signal through the handle is visible through the fd" true by construction
rather than by a copy step.

## 4. Completion: the same mechanism `EXEC` uses, and why `-ETIME`

`rm()->kick()` retires the mock's fences; the shim learns about it only in
`client_reap_locked()`, which is the function `EXEC`'s submit path already
called for binary syncs. This card gave a pending entry the **point** it will
write, so nothing else about completion changed:

* a wait calls `client_reap_locked()` and then compares points — it never sets a
  point itself;
* a pending signal writes its own point when its fence retires, and a point
  never goes backwards (`max`);
* an `EXEC` that signals point 11 therefore leaves a wait for point 12
  outstanding **after** the fence retires. The tier drives exactly that: 11
  returns 0, 12 returns `-ETIME` in the same breath.

**The error is `-ETIME`, not `-EAGAIN`.** `-ETIME` is the uAPI's own answer for
a wait that did not complete (Linux's `drm_syncobj_wait` returns it for a poll
and for an expired deadline), it is what this node's binary row already
returned, and one condition should not have two errnos. `-EAGAIN` is `poll(2)`'s
and `read(2)`'s convention for "would block"; DRM's wait never uses it.

**No spin, no host-clock fake.** A wait re-reads the completion path on every
pass; what ends it early is a fence retiring. The clock only ends it when
nothing retired, at which point the answer is `-ETIME` and the row says
`retired=0 timeout=poll|ms clamped=…`. The wait is bounded
(`SHIM_WAIT_SPINS`, `SHIM_FENCE_WAIT_MAX_MS`), and a `~0` ("forever") timeout is
clamped and reported as clamped — a kernel that may not park inside a syscall
says so instead of lying. `WAIT_FOR_SUBMIT`, `WAIT_AVAILABLE` and `WAIT_DEADLINE`
are refused with `-ENOSYS` and `reason=wait_flag_unsupported` on both wait rows:
each of them needs fence registration or deadline propagation, neither of which
is a poll.

## 5. `EXEC` with a timeline sync

`NVSHIM_DRM_SYNC_TIMELINE_SYNCOBJ` with a real `timeline_value` is accepted.
The point is validated **before any push is enqueued** and then written when the
fence retires:

* `timeline_value == 0` → `-EINVAL`, `reason=timeline_point_zero` (Linux WARNs
  and treats it as 1, which is a kernel bug report this node cannot make);
* a value that is not greater than every point already **submitted** for that
  syncobj (signalled, or pending a retire) → `-EINVAL`,
  `reason=timeline_point_order`. Linux clamps that at a `WARN_ON`; this node
  refuses it and names it, because a signal that silently lands on an older
  point is a fence a consumer cannot tell from the one it waits on;
* the timeline type with any extra flag bit, or a binary sync carrying a
  `timeline_value`, → `-EINVAL`, `reason=unknown_sync_type` (unchanged);
* a handle this client does not hold → `-EINVAL` (unchanged).

The row `reason=timeline_syncobj` is **gone**: the case it named is served, and
the malformed ones say which field was wrong instead.

## 6. The syncobj fd

`SYNCOBJ_HANDLE_TO_FD` installs an ordinary file description of type
`FILE_TYPE_SYNCOBJ` (`fs.h`) whose payload **is the syncobj record**. Because it
is a file description, `dup(2)`, `fork`, `close(2)` and `SCM_RIGHTS` need no new
code — the same machinery the dma-buf fd uses, and the reason
`nv_shim_syncobj_release()` runs exactly once, at the last close.

The record is refcounted by **handle entries + exported fds**:

```
refs = one per client handle-table entry + one per exported syncobj fd
```

so it outlives the client that created it. The tier walks three close orders:

1. a second process receives the fd over `SCM_RIGHTS`, imports a handle, closes
   the handle, then its fd: nothing is freed (the description is still open in
   the exporter) and the record still reads its point;
2. importer handle → exporter handle → fd: refs 3 → 2 → 1 → freed, and the point
   is readable at refs 1;
3. the **node fd** closes while the syncobj fd is open: the client and every
   handle entry go, the record stays (refs 1) with its point, and the last close
   of the fd frees it.

`SYNCOBJ_FD_TO_HANDLE` from that fd gives a **new** handle on the same object
(a new handle entry, so a new reference) — not an echo of the number that
exported it. A signal through either handle, in either process, is visible
through the other; the tier measures that in both directions and reads the point
through the kernel seam to prove there is one field, not two copies.

What the fd is **not** is a `sync_file`. `EXPORT_SYNC_FILE` /
`IMPORT_SYNC_FILE` ask for a `dma_fence` as a file — a different object type
this node does not have — and both are refused with `-ENOSYS`,
`reason=export_sync_file` / `reason=import_sync_file`. That is the honest half
of the PRIME/fence story that a Vulkan WSI will eventually need
(`drmSyncobjExportSyncFile`), and it is deliberately not faked by handing back a
syncobj fd under another name.

As in `drm_syncobj.c`, the plain forms refuse a non-zero `point` (it belongs to
the sync-file form), and `pad` must be zero.

## 7. GET_CAP and the feature mask

* `DRM_CAP_SYNCOBJ_TIMELINE` now answers **1**. That is licensed by items 1–2 of
  the card (signal/wait in both directions, `EXEC` accepting a timeline sig) and
  by the cap's own meaning ("timeline operations on sync objects") — not by the
  id existing. The three rows still refused are not what this cap promises.
* the deprecated `'N'` face's `DEVICE_ALLOC` mask gained
  `NVSHIM_FEAT_SYNCOBJ_TIMELINE` for the same reason.

## 8. What stayed refused, and why

| row | name | why it is still a refusal |
|---|---|---|
| `SYNCOBJ_QUERY` | `syncobj_query` | a correct answer needs the *submitted* point (the `LAST_SUBMITTED` flag), and this node's pending ledger is dropped at retire; a half-answer would be the fake this card forbids |
| `SYNCOBJ_TRANSFER` | `syncobj_transfer` | moves a point between objects including `sync_file` imports — needs the object type this node does not have |
| `SYNCOBJ_EVENTFD` | `syncobj_eventfd` | registers an eventfd to be signalled; needs the eventfd object wired into the completion path |
| `GEM_PUSHBUF`, `GEM_CPU_PREP/FINI`, `GEM_FLINK`, `SETPARAM`, the three legacy object rows, `SVM_INIT/BIND`, `GET_ZCULL_INFO` | unchanged | untouched by this card |

Nothing that answered `-ENOSYS` starts answering 0 without doing the work; the
tier asserts the three remaining syncobj rows are still `-ENOSYS` **by name**.

## 9. Refusal-name table

`nv_shim_sync_refusal_name()` (`nv_shim.h`) is the sync analogue of the PRIME
one, and is copied into a caller buffer rather than returned as a pointer — a
pointer-returning literal selector becomes a `.rodata` table of pointers, which
the AArch64 relocation audit gates on (the lesson WP-PRIME paid for).

| name | when |
|---|---|
| `not_a_syncobj` | the fd is not a syncobj fd |
| `not_this_clients_syncobj` | a handle this client does not hold |
| `export_sync_file` / `import_sync_file` | the sync-file forms |
| `bad_flags` | an undefined flag bit |
| `bad_shape` | `pad != 0`, `count_handles == 0`, or a bad width |
| `timeline_point_zero` | a point of 0 |
| `timeline_point_order` | not greater than a submitted point |
| `point_without_sync_file` | `point != 0` on the plain fd form |
| `wait_flag_unsupported` | `WAIT_FOR_SUBMIT` / `WAIT_AVAILABLE` / `WAIT_DEADLINE` |
| `no_syncobj_slot` | the record table (32 node-wide) or a handle table is full |

## 10. Measured

Both tiers ran on the code commit **`8a346157`** (`nv/wp-sync`, based on
`nv/wp-prime` @ `e46d257c`). Logs are attached to the card:
`unit-tier-intel-sync-600.log`, `unit-tier-arm-sync-479.log`.

| tier | invocation | result |
|---|---|---|
| intel (x86_64) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh` | `Tests run: 600 / Tests failed: 0 / UNIT TESTS PASSED` |
| aarch64 | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh` (TCG) | `Tests run: 479 / Tests failed: 0 / UNIT TESTS PASSED` |

Baseline was intel 597 / ARM 476 (WP-PRIME, `46603a46`); the delta is **+3** on
both, one per new suite (`test_nvshim_drm_syncobj_timeline`, `..._fd`,
`..._refusals`).

The rows that carry the acceptance are in both logs:

```
[NVSHIM] drm syncobj timeline_signal n=1 point=0x5
[NVSHIM] drm syncobj timeline_wait n=1 point=0x5 retired=1
[NVSHIM] drm syncobj timeline_wait n=1 point=0x6 retired=0 clamped=0
...
[NVSHIM] drm syncobj timeline_wait n=1 point=0xb retired=0 clamped=0   <- held
[NVSHIM] drm syncobj timeline_wait n=1 point=0xb retired=1             <- after retire
[NVSHIM] drm syncobj timeline_wait n=1 point=0xc retired=0 clamped=0   <- still outstanding
[NVSHIM] drm exec refused timeline_signal handle=0x2 reason=timeline_point_zero
[NVSHIM] drm exec refused timeline_signal point=0xc reason=timeline_point_order
[NVSHIM] drm syncobj handle_to_fd handle=1 fd=4 obj=7 point=0x0
[NVSHIM] drm syncobj fd_to_handle fd=4 handle=2 obj=7
[NVSHIM] syncobj release obj=7 fd=4 point=0xc refs=0 freed=1
```

## 11. Verification gaps (what this card does not prove)

* no Mesa/nouveau client was linked or run: the ioctl path is exercised through
  the real route table and a real process/address space, but the winsys claim is
  source-level;
* no `sync_file` either way, so a Vulkan WSI's `drmSyncobjExportSyncFile` still
  gets `-ENOSYS` by name — this rung hands a consumer a **syncobj** fd, not a
  fence fd;
* no GPU boot: the fence is the mock RM's, retired by `kick()`;
* `SYNCOBJ_QUERY` cannot be used to observe a point, so the tests read it
  through the kernel seam (`nv_shim_test_syncobj_point`) and through waits;
* the record table is a fixed 32 node-wide; a client that needs more gets
  `-ENOSPC` by name (`no_syncobj_slot`) rather than a silent reuse.
