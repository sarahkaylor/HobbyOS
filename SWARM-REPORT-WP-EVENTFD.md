# SWARM-REPORT-WP-EVENTFD — a retired point writes the client's eventfd (t_d14cd97e)

Lane: `nv-card`. Card: `t_d14cd97e`. Base `nv/integ-swarm`: `bc857a3f` (the
WP-TRANSFERFILE fold). Branch: `nv/wp-eventfd`. Code commit: `c698bf13` (the
rung's first commit is `d19eabdc`; `c698bf13` makes point 0 the handle's current
fence and re-measures both tiers). No GPU boot, no VM boot, no `qm`, no KVM, no
`docs/nv-plan.md` edit.

## 1. What landed

`DRM_IOCTL_SYNCOBJ_EVENTFD` (`0xcf`) is **served**. It was the last named
refusal in `g_drm_refusals[]`, and it is now a row in `g_drm_rows[]` at drm.h's
own width:

```
src/kernel/nv_shim.c:1557   {0xcf, 24}, /* SYNCOBJ_EVENTFD  struct ..._eventfd */
```

with drm.h's struct, field for field, in `nv_drm_uapi.h`:

```c
struct nvshim_drm_syncobj_eventfd {   /* 24 bytes */
  uint32_t handle;   /* 0  */
  uint32_t flags;    /* 4  must be 0 -- drm.h defines no flag for this row */
  uint64_t point;    /* 8  0 = the binary form (point 1) */
  int32_t  fd;       /* 16 the eventfd, resolved at registration */
  uint32_t pad;      /* 20 must be 0 */
};
```

encoded `NVSHIM_DRM_IOC_SYNCOBJ_EVENTFD` = **`0xC01864CF`**
(`src/include/nv_drm_uapi.h:709-711`), pinned by `test_nvshim_drm_layout`.

The deliverable is the two small exports the parent's re-measurement named, plus
the shim's registration and retire hook — nothing vendor-supplied:

- **`file_eventfd_slot(struct process *, int fd, uint32_t *gen)`**
  (`src/kernel/fs.c:4576`, declared `src/include/fs.h:366`): the fd → counter-slot
  resolver, a read of the existing `f->eventfd.slot` field, plus the slot's
  allocation **generation** (see §3). `-EBADF` for an absent fd, `-EINVAL` for a
  live fd that is not an eventfd.
- **`l_eventfd_signal(int slot, uint32_t gen, uint64_t n)`** and
  **`l_eventfd_gen(int slot)`** (`src/kernel/linux_compat_fd.c:1430`, `:1408`):
  the kernel-side writer, the sibling of `write(2)`'s `l_eventfd_write()` with
  the value already in the kernel — add under `l_efd_lock`, take and drain the
  parked-reader `pid_mask`, `poll_notify_obj(&l_efd[slot])` on the same key
  `l_eventfd_key()` returns. No user pointer, no wake without a drain.
- **The shim** (`src/kernel/nv_shim.c`): `op_drm_syncobj_eventfd()` (`:4297`),
  `sobj_reach_locked()` (`:3405`) called from the one completion path
  `client_reap_locked()` (`:3433`) and from EXEC's already-retired branch,
  `TIMELINE_SIGNAL`, binary `SIGNAL` and TRANSFER's reached destination;
  `client_efd_flush()` (`:4442`) delivers the queued writes at the end of the
  retiring ioctl; reasons 21..24 in `k_sync_reasons[]` (`:3498-3502`); a
  dedicated row bucket `r_eventfd = 32`.

| file | Δ | what |
|---|---|---|
| `src/include/fs.h` | +8/−0 | `file_eventfd_slot()` declaration + contract |
| `src/include/nv_drm_uapi.h` | +58/−11 | drm.h's eventfd struct, the served request macro (same `0xC01864CF`), comments now say the syncobj surface has no numeric refusal left |
| `src/include/nv_shim.h` | +20/−0 | reasons 21 `not_an_eventfd`, 22 `eventfd_closed`, 23 `eventfd_registered`, 24 `eventfd_point_not_fence`, each with its meaning |
| `src/kernel/fs.c` | +20/−0 | `file_eventfd_slot()` |
| `src/kernel/linux_compat_fd.c` | +61/−0 | per-allocation `gen`, `l_eventfd_gen()`, `l_eventfd_signal()` |
| `src/kernel/nv_shim.c` | +389/−37 | `{0xcf,24}` into `g_drm_rows[]` (and out of `g_drm_refusals[]`), the registration table, `sobj_reach_locked()`, the handler, the flush, the destroy/clear hooks, `r_eventfd` |
| `src/kernel/nv_shim_drm_test.c` | +283/−20 | layout pins, the four stale refusal expectations, and the new `test_nvshim_drm_syncobj_eventfd` |
| `docs/nv-shim-eventfd.md` | +289/−0 | the rung's doc |

Source+tests: 7 files, **+839/−68**. With the doc: 8 files, **+1128/−68**.
This report adds its own size on top; `git diff --stat bc857a3f HEAD` is the
authoritative total for the branch.

## 2. The acceptance, measured

Both tiers ran in this card's worktree, one round each, on the code commit
`c698bf13`, TCG (`SKIP_KVM=1`; this uid is not in group `kvm`):

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900  ./run_unit_tests.sh
```

| tier | fold baseline `bc857a3f` | this rung | delta |
|---|---|---|---|
| intel (x86_64) | `Tests run: 605 / Tests failed: 0` (`t_c2079388/unit-tier-intel-merged.log:3027-3028`) | **`Tests run: 606 / Tests failed: 0 / UNIT TESTS PASSED`** (`unit-tier-intel.log:3032-3034`) | **+1** |
| aarch64 | `Tests run: 484 / Tests failed: 0` (`t_c2079388/unit-tier-arm-merged.log:2432-2433`) | **`Tests run: 485 / Tests failed: 0 / UNIT TESTS PASSED`** (`unit-tier-arm.log:2453-2455`) | **+1** |

The delta is `test_nvshim_drm_syncobj_eventfd`, one per arch; the refusal shapes
are asserted inside it, so they spend no extra test count. Both meet the card's
floor (`>= 606 / 0`, `>= 485 / 0`).

ARM reloc gate (`unit-tier-arm.log:346`):

```
[RELOC] entries=0x00000000000001AD applied=0x00000000000001AD slide=0x00000003FA200000 outside=0x0000000000000000 status=0x0000000000000000 ok
```

Transport, quoted: intel `unit-tier-intel.log:1` reads `Running unit tests with
QEMU_ARGS: ` (empty — the TCG path `SKIP_KVM=1` selects); ARM
`unit-tier-arm.log:341` is `qemu-system-aarch64 ... -accel tcg,thread=multi ...`.

### 2.1 The suite's rows (intel line [ARM line])

```
2718 [2168]   Running test_nvshim_drm_syncobj_eventfd...
2721 [2171] [NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x4 slot=0 gen=1 regs=1
2722 [2172] [NVSHIM] drm syncobj refuse eventfd dup point=0x4 reason=eventfd_registered
2723 [2173] [NVSHIM] drm syncobj refuse eventfd fd=0x3 reason=not_an_eventfd
2724 [2174] [NVSHIM] drm syncobj refuse eventfd fd=0xffffffffffffffff reason=not_an_eventfd
2725 [2175] [NVSHIM] drm syncobj refuse eventfd flags=0x1 reason=bad_flags
2726 [2176] [NVSHIM] drm syncobj refuse eventfd point=0x68 reason=point_beyond_ledger
2727 [2177] [NVSHIM] drm syncobj refuse eventfd handle=0x1f5 reason=not_this_clients_syncobj
2728 [2178] [NVSHIM] drm syncobj eventfd signal slot=0 gen=1 point=0x4 handle=1 n=1
2729 [2179]   [NVSHIM] eventfd: read -EAGAIN before the retire, 8 bytes == 1 after the SAME unsigned EXEC, -EAGAIN on the second read; poll 0 -> POLLIN
2730 [2180] [NVSHIM] drm syncobj eventfd reg handle=1 obj=29 point=0x9 slot=1 gen=2 regs=1
2731 [2181] [NVSHIM] drm syncobj refuse eventfd slot=0x1 reason=eventfd_closed
2732 [2182]   [NVSHIM] eventfd closed: the retire wrote NOTHING -- an eventfd reusing the registered slot still reads -EAGAIN; row reason=eventfd_closed
2733 [2183] [NVSHIM] drm syncobj eventfd reg=now handle=2 point=0x1 slot=2 gen=4 n=1
2734 [2184] [NVSHIM] drm syncobj eventfd reg handle=3 obj=31 point=0x7 slot=3 gen=5 regs=1
2735 [2185] [NVSHIM] drm syncobj eventfd signal slot=3 gen=5 point=0x7 handle=3 n=1
2736 [2186]   [NVSHIM] eventfd binary form: CREATE_SIGNALED signals inside the registration; a pending head fence (7) is waited for, not the landed 3
```

The `eventfd signal` row at 2728 is the retire itself: slot 0, the generation
the registration pinned, point 4, `n=1`. The 2730/2731 pair is the closed-fd
case: the registration is on slot 1, the fd was closed, a **new** eventfd reused
slot 1 (gen 2 against the pinned gen 1), so the retire wrote nothing and named
it. The last three rows are the binary form: `reg=now` on a `CREATE_SIGNALED`
handle signals inside the registration at its stub point 1, while the timeline
handle whose point 3 had landed and whose point 7 was still pending registers at
**7** and fires only when 7 retires — the row that proves point 0 is not a
hard-wired 1.

| acceptance bullet | how it is measured |
|---|---|
| a **real** eventfd fd and a **real** `read(2)`, through the real route table and `0xC01864CF` | `file_eventfd_create(pr.p, 0, EFD_NONBLOCK)` makes the fd `eventfd2` makes; `file_read_user(pr.p, efd, user_va, 8, 0)` is the path `read(2)` takes (the same function the syscall handler calls); the request goes through `drm_ioc(&pr, NVSHIM_DRM_IOC_SYNCOBJ_EVENTFD, ...)` at the header's own encoding |
| before the retire: non-blocking read `-EAGAIN` | `unit-tier-intel.log:2721` registration, then `efd_read()` → `-EAGAIN` before the kick |
| after the single unsigned EXEC: 8 bytes == 1; a second read `-EAGAIN` | one `sync_exec_with(&pr, ch, 0, 0)` after `nv_rm_mock_hold(0)`; `efd_read()` returns 8 and the value 1, the next call returns `-EAGAIN`; row `:2728` is the write, `:2729` the suite's summary |
| `poll(2)` wakes at retire | `file_poll()` — the engine `poll(2)` runs — returns 0 / revents 0 before the kick and 1 / `K_POLLIN` after it |
| closed before the retire: refused by its own name, nothing written | the slot is learned from the resolver, the fd is closed, and fresh eventfds are allocated until one **reuses that exact slot**; after the retire the new fd still reads `-EAGAIN` and the row is `reason=eventfd_closed` (`:2730-2732`) |
| the row moves to `g_drm_rows[]` as `{0xcf, 24}`; the refusal row disappears from both logs | `grep -c 'drm refuse syncobj_eventfd' unit-tier-intel.log unit-tier-arm.log` = **0 / 0**; the fold's logs carried 4 each (`t_c2079388/unit-tier-intel-merged.log:2258`, `unit-tier-arm-merged.log:1692` …) |
| the serve rows appear in both logs | the suite's 17 eventfd lines in each log, quoted above with both line numbers |
| both arches one round, ARM reloc gate ok | §2 table + the `[RELOC]` line above |

## 3. The generation, and why it is not optional

`l_efd[]` reuses slot numbers immediately (`l_eventfd_new()` takes the first free
slot). A registration that remembered only the slot would, after a `close(2)`,
write into whatever eventfd now owns that number: a wake for a stranger, and a
`read(2)` that returns 1 for a point the reader never registered — the §2.13
fake in its quiet form. So every allocation takes a monotonic `gen`
(`linux_compat_fd.c:1363,1393`), the registration pins `(slot, gen)`, and
`l_eventfd_signal()` moves the counter only when `used && gen == registered`;
anything else answers `-EBADF`, which the shim prints as `eventfd_closed` and
writes nothing.

The suite measures this the hard way: it closes the registered fd, then keeps
allocating eventfds until one lands on the **same slot** the registration pinned
(`reuse` at `nv_shim_drm_test.c:4052-4060`), and asserts that fd's read is still
`-EAGAIN` after the retire. Without the generation check that read is the one
that would flip to 1.

## 4. What is still refused, and why

| reason | when | note |
|---|---|---|
| `not_an_eventfd` (21) | the fd is absent/closed at registration, or is the node/a pipe/a syncobj fd | `-EBADF` / `-EINVAL`, Linux's `eventfd_ctx_fdget()` split |
| `eventfd_closed` (22) | the registered eventfd was closed (or its slot reused) before the retire | **nothing is written**; the named refusal the close-after-register case must produce |
| `eventfd_registered` (23) | a second registration for the same `(record, point)` | drm.h's 24 bytes carry no cancel form, so refusing beats replacing; a registration on an **already-reached** point stores nothing and signals at once, as Linux's add-callback on a signalled fence does |
| `eventfd_point_not_fence` (24) | a handle from `IMPORT_SYNC_FILE` asked for a point other than its floor | the handle names exactly one fence |
| `point_beyond_ledger` (12), `bad_flags` (4), `bad_shape` (9), `not_this_clients_syncobj` (11), `no_syncobj_slot` (8) | unchanged | measured in the same suite |

No syncobj row is refused by **number** any more: `0xc1/c2/c3/c4/c5/ca/cb/cc/cd/cf`
are all in `g_drm_rows[]`, and every remaining refusal inside them is a reason
name. That is the surface state this rung closes: **served or refused by
design**, never absent.

## 5. Stated divergences from Linux

- **One registration per `(record, point)`** where Linux allows several
  callbacks on one fence — because there is no cancel form in 24 bytes.
- **The point must be in this node's submitted ledger** (or be the binary form
  at 1); Linux walks the syncobj's chain. A point that was never submitted has
  no fence here, so it is reason 12 rather than a wait that cannot end.
- **Destroying the handle drops the registrations**, with the pending entries:
  in this node a destroyed handle has no fence left to land the point (an
  exported fd keeps the object but cannot be submitted through), so leaving the
  registration would wait forever.
- **`point == 0` is the handle's current fence** — the imported floor, else the
  record's highest submitted point (the chain head Linux attaches its callback
  to), else the binary stub at 1 — and it fires at once when that fence is
  already signalled (Linux's explicit stub-fence case).
- The counter always moves by **1** per retire: Linux's
  `syncobj_eventfd_signal()` calls `eventfd_signal(efd, 1)`.

## 6. What this card does NOT prove

- It is a **completeness row, not a client-path row**: no Mesa/nouveau winsys
  calls `0xcf` here, and **no nvk progress may be claimed from it**.
- No GPU boot, no VM boot, no KVM: the fence is the mock RM's, retired by one
  unsigned EXEC through the same `client_reap_locked()` path the timeline suite
  drives.
- `poll(2)` is the engine `file_poll()`, not a parked multi-fd `ppoll`; the
  parked-reader drain in `l_eventfd_signal()` is exercised by construction (the
  mask is taken and drained) but not by a sleeping thread in this tier.
- The engine-class wall (`NVIF NEW` `0xc7b5`) and the binary transfer (reason
  15) were not touched — both exonerated by the card.

## 7. Artifacts

- `unit-tier-intel.log` — intel, `Tests run: 606 / Tests failed: 0 / UNIT TESTS PASSED` (`:3032-3034`)
- `unit-tier-arm.log` — aarch64, `Tests run: 485 / Tests failed: 0 / UNIT TESTS PASSED` (`:2453-2455`), reloc gate ok (`:346`)
- this report; `docs/nv-shim-eventfd.md` in the tree
