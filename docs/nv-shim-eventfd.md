# WP-EVENTFD — a retired point writes the client's eventfd and a real read(2) returns 1

Card: `t_d14cd97e`. Base: `nv/integ-swarm` @ `bc857a3f` (the WP-TRANSFERFILE
fold). Branch: `nv/wp-eventfd`. Shim + its DRM test suite + docs only: no GPU
boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What this rung serves

`DRM_IOCTL_SYNCOBJ_EVENTFD` (`0xcf`) was the **last named refusal in
`g_drm_refusals[]`**. It is now a served row at drm.h's own shape and width, and
`g_drm_refusals[]` no longer names any syncobj row.

| row | form | nr | before | after |
|---|---|---|---|---|
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | timeline handle, `point` in the submitted ledger | `0xcf` | `-ENOSYS` by name (`syncobj_eventfd`) | **served** — the retire writes the counter |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | `point == 0` (the binary form: the handle's one fence, point 1) | `0xcf` | `-ENOSYS` by name | **served** — same registration at point 1; signals at once if the handle is already signalled |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | `fd` is not an eventfd | `0xcf` | `-ENOSYS` by name | `-EINVAL`/`-EBADF`, reason `not_an_eventfd` (21) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | the registered eventfd was **closed before the retire** | `0xcf` | `-ENOSYS` by name | retire writes **nothing**, reason `eventfd_closed` (22) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | a second registration for the same `(handle, point)` | `0xcf` | `-ENOSYS` by name | `-EINVAL`, reason `eventfd_registered` (23) — never a silent replace |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | `point` a handle from `IMPORT_SYNC_FILE` does not carry | `0xcf` | `-ENOSYS` by name | `-EINVAL`, reason `eventfd_point_not_fence` (24) |
| `DRM_IOCTL_SYNCOBJ_EVENTFD` | non-zero `flags`, non-zero `pad`, a point above the submitted ledger | `0xcf` | `-ENOSYS` by name | `bad_flags` / `bad_shape` / `point_beyond_ledger` |

The row moved from `g_drm_refusals[]` to `g_drm_rows[]` in the same edit:

```
src/kernel/nv_shim.c:1557   {0xcf, 24}, /* SYNCOBJ_EVENTFD  struct ..._eventfd */
```

The number had to leave the refusal table for the same reason every earlier row
did: `nv_shim_drm_ioctl()` answers a refusal **before** the size check, so a
number present in both tables would look served in the contract test and never
run. The encoded request is unchanged in value — the old refusal spelling was
`ANY(0xcf, 24)` and drm.h's own encoding is `DRM_IOWR(0xcf, struct
drm_syncobj_eventfd)` = **`0xC01864CF`** (`src/include/nv_drm_uapi.h:706-708`),
pinned by `test_nvshim_drm_layout` (`src/kernel/nv_shim_drm_test.c:90,131-133,
170-172`).

## 2. What "served" means

### 2.1 The struct, from drm.h's shape

```c
struct nvshim_drm_syncobj_eventfd {   /* src/include/nv_drm_uapi.h:333 */
  uint32_t handle;   /* 0  */
  uint32_t flags;    /* 4  must be 0 */
  uint64_t point;    /* 8  timeline point; 0 = the binary form */
  int32_t  fd;       /* 16 the eventfd, resolved at registration */
  uint32_t pad;      /* 20 must be 0 */
};                   /* 24 bytes */
```

`flags` is MBZ: drm.h defines **no** flag bit for this row and Linux's
`drm_syncobj_eventfd_ioctl()` answers `-EINVAL` for any non-zero value, so this
node does the same and names it `bad_flags`. The same field shape says there is
**no cancel form**: 24 bytes carry no "remove this registration" operation, and
no such ioctl exists upstream either (the fence callback dies with the fence).
That absence is answered by name rather than approximated — see §3.

### 2.2 Registration

Per client, in the table model the pending entries use: `(record, point, slot,
generation)`. `op_drm_syncobj_eventfd()` (`src/kernel/nv_shim.c:4297`):

1. reads the 24 bytes, refuses `pad`/`flags`/shape;
2. resolves the **fd** to `(counter slot, slot generation)` *before* taking the
   shim lock (`file_eventfd_slot()`, `src/kernel/fs.c:4576`) — a read of the
   existing `f->eventfd.slot`, not a new table;
3. takes `g_shim_lock`, runs `client_reap_locked()` first (a retire nobody
   observed must be visible to this decision), resolves the handle to its
   record (a handle this client does not hold is reason 11);
4. decides the **effective point**:
   - `point != 0`: it must be `<= o->submitted` for an ordinary handle (a point
     that was never handed to the timeline has no fence, so nothing could ever
     land it — reason 12), and exactly the floor for a handle from
     `IMPORT_SYNC_FILE` (that handle carries one fence — reason 24);
   - `point == 0`: drm.h's **binary form**, i.e. the handle's *current* fence:
     the floor of an `IMPORT_SYNC_FILE` handle, else the record's highest
     submitted point (the chain head Linux attaches its callback to), else the
     binary stub at **1** (`CREATE_SIGNALED`/binary `SIGNAL`). Resolving it to 1
     unconditionally would fire on an earlier landed point while the head fence
     is still pending;
5. refuses a duplicate `(record, effective point)` by name (23) — replacing
   would silently drop a signal the client already asked for;
6. if the record has **already reached** the point, signals inside this request
   (Linux's `dma_fence_add_callback()` on an already-signalled fence, and its
   explicit `fence == dma_fence_get_stub()` case); otherwise stores the
   registration and waits for the retire.

### 2.3 The retire writes 1

A point is landed in exactly one place per signal path, and the eventfd fires
from the **same** helper the timeline/transfer rows use:
`sobj_reach_locked()` (`src/kernel/nv_shim.c:3405`) is called by
`client_reap_locked()` (`:3433`), the EXEC that retires its fence inside the
ioctl, `TIMELINE_SIGNAL`, a binary `SIGNAL`, and TRANSFER's already-reached
destination. It moves every registration at or below the new point into the
client's fire queue and drops it from the table, so each registration fires
once.

On retire the counter is incremented by exactly **1** — Linux's
`syncobj_eventfd_signal()` calls `eventfd_signal(efd, 1)`. A non-blocking
`read(2)` then returns one 8-byte u64 `== 1`; a second read is `-EAGAIN`;
`poll(2)` wakes on the same key `l_eventfd_key()` already returns for that slot.

The write itself is `l_eventfd_signal()` (`src/kernel/linux_compat_fd.c:1430`),
the kernel-side sibling of `l_eventfd_write()`: add under `l_efd_lock`, take
the parked-reader mask, drain it, `poll_notify_obj(&l_efd[slot])`. No user
pointer, no `copy_from_user`, no fake "wake" without a `pid_mask` drain.

Delivery happens at the **end of the ioctl** that retired the point
(`client_efd_flush()`, `src/kernel/nv_shim.c:4434`, called from
`nv_shim_drm_ioctl()`): still inside the request, so a `read(2)` issued after it
returns observes the 1, but after `g_shim_lock` is released, because the signal
wakes processes and notifies pollers. Rows are emitted there too (this file's
convention: no console output under the shim lock).

### 2.4 The generation: why a slot alone is not honest

`l_efd[]` reuses slot numbers immediately (`l_eventfd_new()` scans for the first
free slot). A registration that remembered only `slot` would, after a
`close(2)`, write into whatever eventfd now owns that number — a wake for a
stranger, the §2.13 fake in its quiet form. So every allocation gets a
monotonic `gen` (`src/kernel/linux_compat_fd.c:1363,1393`), the registration
pins `(slot, gen)`, and `l_eventfd_signal()` writes only when `used && gen ==
registered`. Anything else returns `-EBADF`, which the shim prints as reason 22
and nothing is written.

## 3. The refusal names

`nv_shim.h` gains reasons 21..24 (`src/include/nv_shim.h:460-463`), rendered by
`k_sync_reasons[]` (`src/kernel/nv_shim.c:3498-3502`):

| # | name | meaning |
|---|---|---|
| 21 | `not_an_eventfd` | the fd is absent/closed at registration, or is the node/a pipe/a syncobj fd |
| 22 | `eventfd_closed` | the eventfd the registration resolved was closed (or its slot reused) before the retire; **nothing is written** |
| 23 | `eventfd_registered` | a second registration for the same (record, point); there is no cancel form to express in 24 bytes, so refusing beats replacing |
| 24 | `eventfd_point_not_fence` | a handle from `IMPORT_SYNC_FILE` names exactly one point (its floor); another point is not its fence |

Rows from the suite (intel tier, verbatim):

```
[NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x4 slot=0 gen=1 regs=1
[NVSHIM] drm syncobj refuse eventfd dup point=0x4 reason=eventfd_registered
[NVSHIM] drm syncobj refuse eventfd fd=0x3 reason=not_an_eventfd
[NVSHIM] drm syncobj refuse eventfd fd=0xffffffffffffffff reason=not_an_eventfd
[NVSHIM] drm syncobj refuse eventfd flags=0x1 reason=bad_flags
[NVSHIM] drm syncobj refuse eventfd point=0x68 reason=point_beyond_ledger
[NVSHIM] drm syncobj refuse eventfd handle=0x1f5 reason=not_this_clients_syncobj
[NVSHIM] drm syncobj eventfd signal slot=0 gen=1 point=0x4 handle=1 n=1
[NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x9 slot=1 gen=2 regs=1
[NVSHIM] drm syncobj refuse eventfd slot=0x1 reason=eventfd_closed
[NVSHIM] drm syncobj eventfd reg=now handle=2 point=0x1 slot=2 gen=4 n=1
[NVSHIM] drm syncobj eventfd reg handle=3 obj=31 point=0x7 slot=3 gen=5 regs=1
[NVSHIM] drm syncobj eventfd signal slot=3 gen=5 point=0x7 handle=3 n=1
```

The `eventfd signal` row is the retire: slot 0, the generation the registration
pinned, point 4, `n=1`. The `reg`/`refuse` pair after it is the closed-fd case:
the registration is at point 9 on slot 1; the fd was closed and a **new** eventfd
reused slot 1 (gen 2 against the pinned gen 1), so the retire wrote nothing and
named it. The last three rows are the binary form: `reg=now` on a
`CREATE_SIGNALED` handle signals inside the registration at the stub point 1,
while the timeline handle whose point 3 had landed and whose point 7 was still
pending registers at **7** and fires only when 7 retires — the row that proves
`point == 0` is the current fence, not a hard-wired 1.

## 4. Why the two exports are in `fs.c` / `linux_compat_fd.c`

Neither is a shim-owned object; both live where the state lives:

- `file_eventfd_slot(struct process *, int fd, uint32_t *gen)`
  (`src/include/fs.h:366`, `src/kernel/fs.c:4576`) — the fd → slot resolver,
  next to the other `file_*_obj()` resolvers. `-EBADF` for an absent fd,
  `-EINVAL` for a live fd that is not an eventfd.
- `l_eventfd_signal(int slot, uint32_t gen, uint64_t n)`
  (`src/kernel/linux_compat_fd.c:1430`) plus `l_eventfd_gen(int slot)`
  (`:1408`). `l_eventfd_write()` remains the `write(2)` path with its user
  pointer; the signal is the same body with the value already in the kernel.

Nothing vendor-supplied, no new table, no change to `write(2)`/`read(2)`
semantics.

## 5. The rows get their own budget

The registration and retire rows are what this card reports, and they run after
six syncobj suites. They get `r_eventfd = 32` (`src/kernel/nv_shim.c:470-477`)
rather than sharing `r_sync` — the WP-SYNCFILE/WP-TRANSFER lesson: a green tier
whose log has no `eventfd signal slot=` row is exactly the silent-budget failure
this project has paid for twice.

## 6. Measured (both arches, one round)

Tree: `nv/wp-eventfd`, code commit (see §7). Both tiers on this box, one round
each, `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1`, TCG (no `/dev/kvm` for uid
1001), `UNIT_TESTS_TIMEOUT=1200` (intel) / `900` (ARM).

| arch | tests run | failed | verdict | log |
|---|---|---|---|---|
| intel | **606** | **0** | `UNIT TESTS PASSED` | `unit-tier-intel.log:3032-3034` |
| ARM | **485** | **0** | `UNIT TESTS PASSED` | `unit-tier-arm.log:2453-2455` |

Baseline from the fold (`t_c2079388`, `nv/integ-swarm` @ `bc857a3f`): 605 / 484.
Delta: **+1 per arch**, `test_nvshim_drm_syncobj_eventfd`.

The lines that prove the tier's transport:

```
unit-tier-intel.log:1    Running unit tests with QEMU_ARGS:          (empty: TCG path, SKIP_KVM=1)
unit-tier-arm.log:341    qemu-system-aarch64 ... -accel tcg,thread=multi ...
unit-tier-arm.log:346    [RELOC] entries=0x1AD applied=0x1AD slide=0x3FA200000 outside=0x0 status=0x0 ok
```

### 6.1 The suite's rows (intel)

```
unit-tier-intel.log:2718  Running test_nvshim_drm_syncobj_eventfd...
unit-tier-intel.log:2721  [NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x4 slot=0 gen=1 regs=1
unit-tier-intel.log:2728  [NVSHIM] drm syncobj eventfd signal slot=0 gen=1 point=0x4 handle=1 n=1
unit-tier-intel.log:2729    [NVSHIM] eventfd: read -EAGAIN before the retire, 8 bytes == 1 after the SAME unsigned EXEC, -EAGAIN on the second read; poll 0 -> POLLIN
unit-tier-intel.log:2731  [NVSHIM] drm syncobj refuse eventfd slot=0x1 reason=eventfd_closed
unit-tier-intel.log:2732    [NVSHIM] eventfd closed: the retire wrote NOTHING -- an eventfd reusing the registered slot still reads -EAGAIN; row reason=eventfd_closed
unit-tier-intel.log:2733  [NVSHIM] drm syncobj eventfd reg=now handle=2 point=0x1 slot=2 gen=4 n=1
unit-tier-intel.log:2734  [NVSHIM] drm syncobj eventfd reg handle=3 obj=31 point=0x7 slot=3 gen=5 regs=1
unit-tier-intel.log:2735  [NVSHIM] drm syncobj eventfd signal slot=3 gen=5 point=0x7 handle=3 n=1
unit-tier-intel.log:2736    [NVSHIM] eventfd binary form: CREATE_SIGNALED signals inside the registration; a pending head fence (7) is waited for, not the landed 3
```

The `syncobj_eventfd` **refusal** row is gone from the log
(`grep -c 'drm refuse syncobj_eventfd' unit-tier-intel.log` = 0); the served
row's own rows above replace it.

### 6.2 The suite's rows (ARM)

```
unit-tier-arm.log:2168  Running test_nvshim_drm_syncobj_eventfd...
unit-tier-arm.log:2171  [NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x4 slot=0 gen=1 regs=1
unit-tier-arm.log:2178  [NVSHIM] drm syncobj eventfd signal slot=0 gen=1 point=0x4 handle=1 n=1
unit-tier-arm.log:2179    [NVSHIM] eventfd: read -EAGAIN before the retire, 8 bytes == 1 after the SAME unsigned EXEC, -EAGAIN on the second read; poll 0 -> POLLIN
unit-tier-arm.log:2181  [NVSHIM] drm syncobj refuse eventfd slot=0x1 reason=eventfd_closed
unit-tier-arm.log:2182    [NVSHIM] eventfd closed: the retire wrote NOTHING -- an eventfd reusing the registered slot still reads -EAGAIN; row reason=eventfd_closed
unit-tier-arm.log:2183  [NVSHIM] drm syncobj eventfd reg=now handle=2 point=0x1 slot=2 gen=4 n=1
unit-tier-arm.log:2184  [NVSHIM] drm syncobj eventfd reg handle=3 obj=31 point=0x7 slot=3 gen=5 regs=1
unit-tier-arm.log:2185  [NVSHIM] drm syncobj eventfd signal slot=3 gen=5 point=0x7 handle=3 n=1
unit-tier-arm.log:2186    [NVSHIM] eventfd binary form: CREATE_SIGNALED signals inside the registration; a pending head fence (7) is waited for, not the landed 3
```

Same rows, same values (slot 0 / gen 1 / point 4, then slot 1 / gen 2 refused),
on the slid AArch64 image: `grep -c 'drm refuse syncobj_eventfd'
unit-tier-arm.log` = 0.

## 7. Divergences from Linux, stated

- **One registration per (record, point).** Linux allows several eventfds on
  one fence; this node's 24-byte struct has no cancel form, so a second
  registration on the same target is refused (23) instead of being stored next
  to the first. A client that wants two counters registers two points or two
  handles.
- **A second registration on an already-reached point signals again.** Nothing
  is stored in that case, so there is nothing to replace: each request is
  Linux's "add a callback to a signalled fence", which fires at once. The
  duplicate refusal (23) is about a **pending** registration.
- **The registered point must be in this node's ledger.** Linux finds the fence
  in the syncobj's chain; this node has one point plus a submitted ledger, so
  `point > submitted` is reason 12 rather than a later wait that cannot end.
- **Destroying the handle drops the registrations** (`op_drm_syncobj_destroy`),
  with the pending entries: in this node a destroyed handle has no fence left
  to retire, so a registration would wait forever. Linux's callback lives on
  the fence, which an exported fd would keep; this node says so here instead of
  pretending.
- **`point == 0` is the handle's CURRENT fence.** drm.h leaves the field
  meaning "binary syncobj" when 0, and Linux attaches the callback to
  `drm_syncobj_fence_get()` — the chain head. Here that is the imported floor
  for an `IMPORT_SYNC_FILE` handle, else the record's highest submitted point,
  else the binary stub at 1.
- **The counter is written once per retire, `n = 1`.** `eventfd(2)`'s
  `EFD_SEMAPHORE` shape does not change this: Linux's syncobj signal passes 1.

## 8. Verification gaps

- Kernel unit tier only: the shim's DRM face is not reachable from the EL0
  probe without a client path, and this card may not boot a VM or the GPU.
- The `poll(2)` check is `file_poll()` — the engine `poll(2)` runs — not a
  multi-fd `ppoll` with a parked waiter; the parked-reader drain in
  `l_eventfd_signal()` is exercised by construction (the mask is taken and
  drained) but not by a sleeping thread in this tier.
- No real winsys drives `0xcf`: this is a **completeness row, not a client-path
  row**. It closes the syncobj surface to "served or refused by design", and no
  nvk progress may be claimed from it.
