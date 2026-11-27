# SWARM-REPORT-WP-SYNCOBJ-TRANSFER — a timeline point moves between syncobjs (t_b227267a)

Lane: `nv-card`. Card: `t_b227267a`. Base `nv/integ-swarm`: `95bae60f` (the
WP-SYNCFILE fold). Branch: `nv/wp-transfer`. Code commit: `622f90f0`.
No GPU boot, no VM boot, no `docs/nv-plan.md` edit.

## 1. What landed

`SYNCOBJ_TRANSFER` (`0xcc`) is served for its **timeline handle→handle** leg:
`src_handle@src_point` on one timeline becomes `dst_point` on another, and the
destination reaches it when the **fence the source submitted** retires — not
when the ioctl returns. Two timelines, no fd, no sync_file, which is why this
leg was servable after WP-SYNCFILE without touching the sync_file object.

The mechanism is not a copy. `op_drm_syncobj_transfer` reaps first (the one
completion path), checks `src_point` against the source record's **submitted
ledger**, and then gives the destination its own pending entry with its own
point on the **same `(channel, fence)`**. `client_reap_locked()` lands both
points in one pass — so "the same retire satisfies src" is a property of the
code, and a transfer that answered 0 cannot make the destination's wait succeed.

| file | Δ | what |
|---|---|---|
| `src/include/nv_drm_uapi.h` | +66/−14 | `struct nvshim_drm_syncobj_transfer` (32 B, drm.h field for field), `TRANSFER_FLAGS_TIMELINE`, the IOC macro derived from the struct (still `0xC02064CC`), header prose brought up to this tip (sync files served since WP-SYNCFILE) |
| `src/include/nv_shim.h` | +23/−0 | reasons 15–19, each documented as a different absence |
| `src/kernel/nv_shim.c` | +202/−9 | `op_drm_syncobj_transfer`, `{0xcc,32}` in `g_drm_rows[]`, 0xcc out of `g_drm_refusals[]`, the five reason names, `r_synctransfer = 48`, `sync_refuse_rows()` (a refusal that takes a bucket) |
| `src/kernel/nv_shim_drm_test.c` | +388/−23 | layout assertions (size, 6 offsets, encoding), `test_nvshim_drm_syncobj_transfer`, `test_nvshim_drm_syncobj_transfer_refusals`, and the five stale `-ENOSYS`/`req_size==0` expectations |

Total source: 4 files, **+679/−46** (`git show --numstat 622f90f0`). The two
documents are this file and `docs/nv-shim-transfer.md`.

## 2. The acceptance, measured

Both tiers ran in this card's worktree, in one round, TCG (this uid is not in
group `kvm`; `/dev/kvm` is not readable):

```
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests_intel.sh
TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=900 ./run_unit_tests.sh
```

| tier | baseline tip `95bae60f` | this rung | delta |
|---|---|---|---|
| intel (x86_64) | `Tests run: 602 / Tests failed: 0` | **`Tests run: 604 / Tests failed: 0 / UNIT TESTS PASSED`** (`tier-intel.log:2987-2989`) | **+2** |
| aarch64 | `Tests run: 481 / Tests failed: 0` | **`Tests run: 483 / Tests failed: 0 / UNIT TESTS PASSED`** (`tier-arm.log:2409-2411`) | **+2** |

The baseline is the parent fold card's own attached logs at this exact tip
(`unit-tier-intel-merged.log:2971-2973` = 602/0,
`unit-tier-arm-merged.log:2376-2378` = 481/0). The delta is
`test_nvshim_drm_syncobj_transfer` + `test_nvshim_drm_syncobj_transfer_refusals`,
one per arch each.

ARM reloc gate (`tier-arm.log:14`):

```
hobbyos.elf: reloc gate OK -- 429 kernel pointer word(s), every one applied at boot
```

Logs attached: `unit-tier-intel.log` (604/0), `unit-tier-arm.log` (483/0).

Each acceptance bullet, and where it is measured:

| acceptance | how it is measured | log evidence |
|---|---|---|
| two timeline syncobjs, src submitted to N, `TRANSFER src→dst at N` returns 0, row names both handles and the point | `sobj_create` ×2, EXEC timeline sig `src@9` with the mock **held**, `sobj_transfer(src,dst,9,9,TIMELINE) == 0` | `transfer src=1 dst=2 src_point=0x9 dst_point=0x9 obj=16->17 chained=1 src_at=0x0` |
| `TIMELINE_WAIT` on **dst** at N is `-ETIME` before src retires | poll and a real ms deadline, while held; `QUERY dst` = reached 0 / submitted 9 | the suite's `[NVSHIM] transfer: dst wait -ETIME …` line |
| ... and **0** after, on the same retire | `nv_rm_mock_hold(0)` + one unsigned EXEC; then src wait 0 **and** dst wait 0; `QUERY dst` = 9 in both modes | same suite, immediately after the row above |
| `SYNCOBJ_QUERY` on dst then reports the transferred point | `sobj_query(dst, 0)` and `(dst, LAST_SUBMITTED)` both 9 | same suite |
| an already-reached source needs no fence | `TIMELINE_SIGNAL src@12`, `transfer(12,12)`, dst wait 0 immediately | `transfer … src_point=0xc dst_point=0xc … src_reached=1 src_at=0xc` |
| a transfer from a point **not yet submitted** is refused by name | `transfer(src,dst,5,5)` when src's ledger is 4 | `refuse transfer src_point=0x5 reason=transfer_point_unsubmitted` |
| ... from a **destroyed** handle | create → destroy → transfer from it, as source and as destination | `refuse transfer handle=0x4 reason=transfer_handle_not_held` (×2) |
| ... with a **non-timeline** syncobj (binary form, `flags = 0`) | `sobj_transfer(..., flags 0)` | `refuse transfer flags=0x0 reason=transfer_needs_timeline` |
| nothing a refusal touched landed | after the batch: `QUERY dst` still 4 in both modes, `wait dst@5` still `-ETIME` | refusal suite tail |
| every named reason is exercised | including 17 (`transfer_point_not_pending`): submit 7, destroy the handle while a syncobj fd keeps the record, re-import, transfer | `refuse transfer src_point=0x7 reason=transfer_point_not_pending` |

The transfer rows have their own budget (`r_synctransfer = 48`), and both logs
carry all 17 of them (3 served, 14 refusals) — checked by grep, not by reading
the PASS line. `drm refuse syncobj_transfer errno=ENOSYS` is **gone from both
logs** (`grep -c` = 0); `drm refuse syncobj_eventfd errno=ENOSYS` still prints.

## 3. The 32-byte contract, and why it is the one way to be "served" and wrong

`struct nvshim_drm_syncobj_transfer` is drm.h's shape — `src_handle`,
`dst_handle`, `src_point`, `dst_point`, `flags`, `pad`, 32 bytes — and the tier
pins the size, all six offsets and the literal request `0xC02064CC`. The kernel
side pins the other end: the row is now size-checked (`drm_row_size()` equality
against `{0xcc, 32}`), where before it was answered `-ENOSYS` by number whatever
size it encoded. The old refusal macro and the new struct-derived macro produce
the same value, so a probe written against the previous header is still a
request a real client could send.

## 4. What stayed refused, stated with the reason

* **`SYNCOBJ_EVENTFD` (`0xcf`) — unchanged wall.** The eventfd counter table is
  file-static (`src/kernel/linux_compat_fd.c:1353-1362`) and its only writer
  takes a user pointer (`:1477`); the retire path has a kernel value and no
  fd→counter resolver. Not touched, not faked. `-ENOSYS` by name in both logs.
* **`TRANSFER`'s sync_file leg** — a transfer whose source handle came from
  `IMPORT_SYNC_FILE` — is refused by **name** (`transfer_src_sync_file`, reason
  19), not served as if it were a timeline point. That keeps the row honest: the
  part that needs a sync_file object is still a separate rung, and the part that
  does not is served and measured.
* **`TRANSFER`'s binary form** (no `TRANSFER_FLAGS_TIMELINE`) is refused by name
  (`transfer_needs_timeline`, reason 15): a whole fence-pointer move is not
  something a one-point-per-record node can do.

The create/destroy/wait/QUERY paths were not re-derived.

## 5. Stale statements left in other documents (for the coordinator)

These predate this rung and were **not** edited (out of this card's file scope;
`docs/nv-plan.md` is coordinator-owned):

| file:line | text |
|---|---|
| `docs/nv-shim-sync.md:49` | `DRM_IOCTL_SYNCOBJ_TRANSFER` … "refused by name" |
| `docs/nv-shim-sync.md:183` | `SYNCOBJ_TRANSFER` … "moves a point between objects including sync_file imports — needs the object type this node does not have" |
| `docs/nv-shim-syncfam.md:19`, `:212` | `TRANSFER` `-ENOSYS` by name |
| `docs/nv-shim-syncfile.md:16` | `TRANSFER` `-ENOSYS` by name (and §5.1 calls it "the next rung" — which this card is) |
| `SWARM-REPORT-WP-SYNCFILE.md` §4 | "`SYNCOBJ_TRANSFER` — next rung" |

## 6. Constraint check

* files touched: `src/kernel/nv_shim.c`, `src/kernel/nv_shim_drm_test.c`,
  `src/include/nv_drm_uapi.h`, `src/include/nv_shim.h` + the two documents —
  exactly the card's list;
* `docs/nv-plan.md` untouched (`git status` clean for it);
* unit tiers only: no GPU boot, no VM boot, no `qm`, no r1;
* both tiers in one round from this tip, `Tests failed: 0` in each;
* branch pushed without force; tip sha named in the completion result.
