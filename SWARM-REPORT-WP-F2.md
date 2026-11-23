# SWARM-REPORT — WP-F2: `/dev/nvshim` speaks nouveau's real DRM surface

Branch `nv/wp-f2-nouveau`, cut from `d44659a2` (round 1 merged, 561/0).
Rung **N2** (`PLAN.md` §4) → the part that was missing for a stock winsys.
**Status: gate met, MEASURED.** The node now answers `DRM_IOCTL_NOUVEAU_*` on
DRM's magic `'d'`, with the uAPI's request numbers, struct layouts and
unsigned-long size validation, over the existing mock RM. A probe creates a
device, a VA space, a BO, a channel, executes a push and retires a fence — in
the kernel unit tier, through `nv_shim_ioctl()`'s magic-`'d'` route.

The honest caveat up front: **a stock nvk still cannot attach**, and not because
of a number or a struct. It needs NVIF, GEM CPU mapping and PRIME, which are
refused by name (§5). What this branch fixes is the ABI mismatch that made *any*
real client impossible; §5 is what remains.

Claims are tagged MEASURED (seen in a log from this branch), SOURCE (read from a
cited contract) or INFERRED.

---

## 1. What changed

New:

| File | What |
|---|---|
| `src/include/nv_drm_uapi.h` | The contract: DRM `'d'` encoding, all 18 structs byte-for-byte, every request number, the 21 named refusals, the limits. Path/line provenance in the header comment |
| `src/kernel/nv_shim_drm_test.c` | 5 unit-tier suites: contract/layout, size + refusal routing, the gate probe, ownership, limits |

Changed:

| File | What |
|---|---|
| `src/kernel/nv_shim.c` | The DRM face (~1,100 lines): row table, size validation, dispatch, implicit device, one-VA-space rule, kernel-allocated push buffer per channel, binary syncobjs, EXEC by GPU VA. One param table now shared by both faces. Per-client `alloc_domain[]`, `ch_pushbuf[]`, syncobj + pending tables |
| `src/include/nv_shim.h` | `'N'` face marked **DEPRECATED** with a row-by-row mapping onto the `'d'` face; `_IOC` field decoders; nouveau's CPU/MAPPABLE/COHERENT/NO_SHARE domain bits; the `nv_shim_drm_*` API |
| `src/include/nv_rm.h`, `src/kernel/nv_rm_mock.c` | `submit_va()`: the EXEC path names a stream by GPU VA and the RM refuses a range not fully covered by mappings. Mock VA limit 1 TiB (bookkeeping; the arena is still 4 GiB) |
| `src/include/errno.h` | `ETIME` = 62 (DRM's `drm_syncobj_wait` timeout answer) |
| `src/kernel/unit_test.c`, `unit_test.h` | one registration line + declaration |

No driver code was copied. Numbers and layouts are quoted from
`include/uapi/drm/nouveau_drm.h` and `include/uapi/drm/drm.h` (SOURCE).

---

## 2. The request surface (MEASURED — sizes and numbers asserted in `test_nvshim_drm_layout` against literal hex)

`_IOWR('d', nr, size)` = `0xC0000000 | size<<16 | 0x64<<8 | nr`.

### 2a. Served — 16 rows

| req (hex) | uAPI | nr | struct | size | what it does here |
|---|---|---|---|---|---|
| `0xC0406400` | `DRM_IOCTL_VERSION` | `0x00` | `drm_version` | 64 | `1.3.1`, name `"nouveau"`, desc `"HobbyOS nvshim mock RM"`. Mesa's nouveau winsys refuses anything below 1.3.1, so the version is part of the contract (SOURCE: `nouveau_device.c`) |
| `0x40086409` | `DRM_IOCTL_GEM_CLOSE` | `0x09` | `drm_gem_close` | 8 | frees a BO by handle. nouveau has no `GEM_CLOSE` of its own; libdrm sends this generic row |
| `0xC010640C` | `DRM_IOCTL_GET_CAP` | `0x0c` | `drm_get_cap` | 16 | `SYNCOBJ`=1, `SYNCOBJ_TIMELINE`=0, `DUMB_BUFFER`=0, else -EINVAL |
| `0xC0106440` | `NOUVEAU_GETPARAM` | `0x40` | `drm_nouveau_getparam` | 16 | 7 params answered (§2c), card-reading ones -ENOSYS |
| `0xC0586442` | `NOUVEAU_CHANNEL_ALLOC` | `0x42` | `drm_nouveau_channel_alloc` | 88 | the **kernel** allocates the 64 KiB push buffer (the struct carries domains, not a handle); returns `channel`, `pushbuf_domains`, `nr_subchan=0` |
| `0x40046443` | `NOUVEAU_CHANNEL_FREE` | `0x43` | `drm_nouveau_channel_free` | 4 | destroys the channel and its push buffer; double free -EINVAL |
| `0xC0106450` | `NOUVEAU_VM_INIT` | `0x50` | `drm_nouveau_vm_init` | 16 | one VA space per file; `addr+size ≤ 1 TiB`. **After a BO or channel → -ENOSYS**, the uAPI's own rule |
| `0xC0286451` | `NOUVEAU_VM_BIND` | `0x51` | `drm_nouveau_vm_bind` | 40 | `op_count` ops from `op_ptr`, each 40 B: MAP/UNMAP by handle+addr+range+bo_offset. Whole batch validated before any op is applied. `RUN_ASYNC`/SPARSE/wait/sig → -ENOSYS |
| `0xC0286452` | `NOUVEAU_EXEC` | `0x52` | `drm_nouveau_exec` | 40 | up to 64 pushes by GPU VA (16 B each) + up to 8 sig syncs; the RM resolves each VA through the channel's VM and refuses an unmapped run |
| `0xC0306480` | `NOUVEAU_GEM_NEW` | `0x80` | `drm_nouveau_gem_new` | 48 | `info.size`/`info.domain` in; `handle`, granted `size`, `offset` (**the bus address**), `map_handle=0` out |
| `0xC0286484` | `NOUVEAU_GEM_INFO` | `0x84` | `drm_nouveau_gem_info` | 40 | size, offset, domain for a handle this client owns |
| `0xC00864BF` | `DRM_IOCTL_SYNCOBJ_CREATE` | `0xbf` | `drm_syncobj_create` | 8 | binary syncobj, optional `CREATE_SIGNALED` |
| `0xC00864C0` | `DRM_IOCTL_SYNCOBJ_DESTROY` | `0xc0` | `drm_syncobj_destroy` | 8 | destroys; pendings that referred to it are dropped |
| `0xC02864C3` | `DRM_IOCTL_SYNCOBJ_WAIT` | `0xc3` | `drm_syncobj_wait` | 40 | 0 / **-ETIME**; `timeout_nsec` absolute monotonic, 0 = poll, ~0 clamped to 1 s and said so in the row |
| `0xC01064C4` | `DRM_IOCTL_SYNCOBJ_RESET` | `0xc4` | `drm_syncobj_array` | 16 | client-forced unsignal |
| `0xC01064C5` | `DRM_IOCTL_SYNCOBJ_SIGNAL` | `0xc5` | `drm_syncobj_array` | 16 | client-forced signal |

Why syncobjs are in this branch: `drm_nouveau_exec` has **no fence field**. A
nouveau client learns a submission completed by the syncobjs it asked EXEC to
signal. Without `SYNCOBJ_WAIT` an EXEC's completion is unobservable, so the gate
("retiring a fence") cannot be met on the real surface at all.

### 2b. Refused by name — 21 rows, `-ENOSYS`, one `[NVSHIM] drm refuse <name>` row each

`SETPARAM` `0x41`, `GROBJ_ALLOC` `0x44`, `NOTIFIEROBJ_ALLOC` `0x45`,
`GPUOBJ_FREE` `0x46`, **`NVIF` `0x47`**, `SVM_INIT` `0x48`, `SVM_BIND` `0x49`,
`GET_ZCULL_INFO` `0x53`, `GEM_PUSHBUF` `0x81`, `GEM_CPU_PREP` `0x82`,
`GEM_CPU_FINI` `0x83`, `GEM_FLINK` `0x0a`, `PRIME_HANDLE_TO_FD` `0x2d`,
`PRIME_FD_TO_HANDLE` `0x2e`, `SYNCOBJ_HANDLE_TO_FD` `0xc1`,
`SYNCOBJ_FD_TO_HANDLE` `0xc2`, `SYNCOBJ_TIMELINE_WAIT` `0xca`,
`SYNCOBJ_QUERY` `0xcb`, `SYNCOBJ_TRANSFER` `0xcc`,
`SYNCOBJ_TIMELINE_SIGNAL` `0xcd`, `SYNCOBJ_EVENTFD` `0xcf`.

Refusals are keyed on the **number**, because several have no struct in the
public uAPI at all (the deprecated rows' structs are gone; NVIF's lives in
nouveau's private `include/nvif/ioctl.h`). Anything else with magic `'d'` is
`-ENOTTY`. A known number carrying the wrong struct width is `-EINVAL` — three
mistakes, three answers (MEASURED, §3.4).

### 2c. GEM domains and params

Domains: `VRAM` (0x2) and `GART` (0x4) served; `CPU` (0x1), `MAPPABLE` (0x8),
`COHERENT` (0x10), `NO_SHARE` (0x20) refused — there is no CPU mapping of an
allocation in this subset, and saying otherwise would be a lie a client builds
on.

Params answered: `PCI_VENDOR`(3)=`0x10de`, `PCI_DEVICE`(4)=`0x2684`,
`FB_SIZE`(8)=4 GiB, `CHIPSET_ID`(11)=`0x192`, `VRAM_BAR_SIZE`(18)=4 GiB,
`EXEC_PUSH_MAX`(17)=64, `HAS_VMA_TILEMODE`(20)=0.
Refused: `BUS_TYPE`(5), `AGP_SIZE`(9), `VM_VRAM_BASE`(12), `GRAPH_UNITS`(13),
`PTIMER_TIME`(14), `HAS_BO_USAGE`(15), `HAS_PAGEFLIP`(16), `VRAM_USED`(19).

---

## 3. Gates

### 3.1 Intel build

`nvswarm build wp-f2` — green, no warnings from the new files (the only warnings
in the tree are the pre-existing `-Wcomment` ones from `vm.h`/`file_obj.h`).
MEASURED.

### 3.2 Unit tier

```
  Running test_nvshim_drm_layout...
  Running test_nvshim_drm_refusals...
  Running test_nvshim_drm_probe...
  Running test_nvshim_drm_ownership...
  Running test_nvshim_drm_limits...
Tests run:    566
Tests failed: 0
UNIT TESTS PASSED
```

Baseline on the same box, same harness, before this branch: **561 / 0**
(MEASURED, seen in its own run). **566 = 561 + the 5 new suites, 0 failed**
(MEASURED). Round 1's five `test_nvshim_*` suites still pass unchanged.

### 3.3 ARM per-TU (the full ARM link is still broken by another lane)

```
$ nvswarm sh wp-f2 'TOOLS_SKIP=1 BROWSER_SKIP=1 make -j12 ARCH=arm MODE=unit_tests \
    LO_ON_DISK= obj/arm/nv_shim.o obj/arm/nv_shim_drm_test.o obj/arm/nv_shim_test.o \
    obj/arm/nv_rm_mock.o obj/arm/unit_test.o'
... 2 warnings generated.   (all -Wcomment, from vm.h)
42232 obj/arm/nv_rm_mock.o
243448 obj/arm/nv_shim.o
147912 obj/arm/nv_shim_drm_test.o
```
MEASURED. The DRM face is arch-neutral by design (no `__x86_64__` guard; the mock
needs no hardware), so this is the real code being compiled for AArch64.

### 3.4 Mutation check — the size validation (MEASURED, red then green)

Mutation applied to `src/kernel/nv_shim.c` `nv_shim_drm_ioctl()`: the size
comparison disabled (`if (0 && NVSHIM_DRM_SIZE(r) != (uint32_t)want)`), i.e. any
encoded size accepted for a known request number. Rebuild + full tier:

```
  Running test_nvshim_drm_probe...
EXPECT_EQ FAILED: drm_ioc(&pr, NVSHIM_DRM_IOC_ANY(0x50, 8), &vmi, (int)sizeof vmi) == -EINVAL
                 at src/kernel/nv_shim_drm_test.c:371
  Running test_nvshim_drm_ownership...
EXPECT_EQ FAILED: nv_rm_mock_live() == 0 at src/kernel/nv_shim_drm_test.c:703
  Running test_nvshim_drm_limits...
EXPECT_EQ FAILED: nv_rm_mock_live() == 0 at src/kernel/nv_shim_drm_test.c:755
Tests run:    566
Tests failed: 3
UNIT TESTS FAILED
```

Restored, rebuilt, re-ran: `Tests run: 566 / Tests failed: 0 / UNIT TESTS
PASSED` (MEASURED). The first failure is the discriminating one: with the check
gone the engine reads the full 16-byte struct anyway and *succeeds*, creating a
VM the next line then finds already taken; the two leak assertions are the
downstream damage. Note that the same assertions made with `p == 0` would not
have caught this — they answer `-EINVAL` for the missing process either way.
That is why the decisive check lives inside the probe, with a live client.

### 3.5 The probe's transcript (MEASURED, `qemu.log`, `[NVSHIM]` rows)

```
[NVSHIM] drm device dev=1 chipset=0x192 mock=1
[NVSHIM] drm getparam param=11 value=0x192
[NVSHIM] drm getparam param=4 value=0x2684
[NVSHIM] drm getparam param=3 value=0x10de
[NVSHIM] drm getparam param=17 value=0x40
[NVSHIM] drm getparam param=8 value=0x100000000
[NVSHIM] drm vm vm=1 umd_end=0x8000000000 total=0x10000000000
[NVSHIM] drm gem_new handle=1 size=0x10000 offset=0x100000000 domain=0x2
[NVSHIM] drm gem_info handle=1 offset=0x100000000 size=0x10000
[NVSHIM] drm bind vm=1 ops=2 first_addr=0x10000 first_range=0x10000
[NVSHIM] drm bind vm=1 ops=1 first_addr=0x20000 first_range=0x1000
[NVSHIM] drm channel ch=1 pushbuf=0x100010000 size=0x10000
[NVSHIM] drm syncobj create handle=1 signaled=0
[NVSHIM] drm syncobj create handle=2 signaled=0
[NVSHIM] drm exec ch=1 pushes=1 sigs=1 fence=0x1
[NVSHIM] drm syncobj wait n=1 retired=0 timeout=poll      <- kick held
[NVSHIM] drm syncobj wait n=1 retired=0 timeout=ms clamped=0
[NVSHIM] drm exec ch=1 pushes=1 sigs=0 fence=0x2
[NVSHIM] drm syncobj wait n=1 retired=1                   <- the fence retired
[NVSHIM] drm syncobj wait n=1 retired=0 timeout=poll      <- SIGNAL/RESET pair
[NVSHIM] drm syncobj wait n=1 retired=1
[NVSHIM] drm syncobj wait n=1 retired=0 timeout=poll
[NVSHIM] drm channel_free ch=1
[NVSHIM] drm gem_close handle=1
  [NVSHIM] drm probe version+cap+getparam+gem+vm+bind+channel+exec+syncobj: PASS
```

`umd_end=0x8000000000 total=0x10000000000` is Mesa's own VM_INIT argument
(`{1<<39, 1<<39}`, TOP = 1<<40), quoted from `nouveau_device.c` (SOURCE) and
replayed verbatim. The probe runs through the real path: `process_create()` →
real `addr_space` → real `file_open("/dev/nvshim")` → `nv_shim_ioctl()` with
every struct and every op/push/sync/handle array living in the process's own
user pages; out fields are prefilled with `0xFFFFFFFF` so "the kernel wrote
this" is distinguishable from "it was always zero".

Both directions of the fence are observed: with `nv_rm_mock_hold(1)`, the same
`SYNCOBJ_WAIT` that later returns 0 returns `-ETIME` (poll and a 1 ms absolute
deadline), `nv_rm_mock_pending()` stays 1; after the hold is released, the next
EXEC's kick retires both fences and the same wait returns 0 with
`first_signaled=0`. `SYNCOBJ_SIGNAL`/`RESET` are exercised too, so a syncobj
that is signalled by construction cannot pass.

The `drm bind ops=2` row matters: the test maps two ranges in **one**
`VM_BIND`, then proves `op[1]` was applied (re-mapping it is an overlap refusal,
unmapping it succeeds).

---

## 4. Round-1 surface: kept, marked deprecated, one behaviour difference

The `'N'` rows still work exactly as before — round 1's 5 suites and their
assertions pass untouched. What changed in the header is honesty, not behaviour:
it now says DEPRECATED, names the `'d'` row each `'N'` row maps onto, and states
that nothing new may be added there.

One difference, MEASURED and asserted: the `'N'` dispatcher keys on the **whole**
encoded request, so a request that differs only in its size field is `-ENOTTY`
as it always effectively was. The `'d'` dispatcher keys on **number then size**
(`-EINVAL` on a mismatch), because that is DRM's contract. Both are asserted side
by side in `test_nvshim_drm_refusals` so neither can drift.

---

## 5. What a real nvk still needs (the gap, in the order it will hit them)

nvkmd's nouveau backend (Mesa `src/nouveau/vulkan/nvkmd/nouveau/`) does this:

1. **Open `/dev/dri/renderD128`.** Not an ioctl; the node HobbyOS serves is
   `/dev/nvshim`. A render-node name is an fs-side change, not this branch's.
2. `DRM_IOCTL_VERSION` — **served** (1.3.1, so the version gate passes).
3. `DRM_IOCTL_GET_CAP` — **served** (syncobj yes, timeline no).
4. `GETPARAM` — **served** for identity and size; `PTIMER_TIME`, `BUS_TYPE`,
   `GRAPH_UNITS` are `-ENOSYS` (they read the card).
5. `VM_INIT` — **served**, 1 TiB.
6. `GEM_NEW`/`GEM_INFO`/`GEM_CLOSE` — **served**, but **no CPU mapping**: nvk
   writes shaders and command streams from userspace, so it needs
   `NOUVEAU_GEM_DOMAIN_CPU|MAPPABLE`, an `mmap` of the BO, and
   `GEM_CPU_PREP`/`CPU_FINI`. All three are refused today. **This is the next
   real blocker after the numbers.**
7. **`NVIF` (nr `0x47`)** — refused `-ENOSYS`, and it is where nvk creates the
   device object, the **channel** and every engine class. `CHANNEL_ALLOC` is
   served but it is the *legacy* path; a stock nvk will not call it. So
   "channel works" here means "the new-uAPI channel row works", not "nvk gets a
   channel".
8. `EXEC` — **served** for binary syncobjs, sync only. Async `VM_BIND`
   (`RUN_ASYNC` + syncobj waits/signals) is refused.
9. **PRIME/dmabuf** (`HANDLE_TO_FD`/`FD_TO_HANDLE`) — refused; nvk's WSI
   presents through dma-buf.
10. Timeline syncobjs — refused; nvk uses them for sparse bind and dependencies.

So: the ABI mismatch is fixed, the remaining work is feature surface, and it is
named rather than silent.

---

## 6. What is NOT verified

- **No userspace client.** Nothing has attached from userspace. The ioctl
  *route* (`linux_compat_fd.c` → `nv_shim_ioctl`) is still exercised only
  in-process: the tests call `nv_shim_ioctl()` directly and use
  `file_open`/`file_close` for the node and its release hook. `l_ret()`'s path is
  untested for this node (round 1's caveat, unchanged).
- **No card, no GSP, no real RM.** `mock=1` in every row. Nothing here says
  anything about AD102. `nv_rm_gsp.c` does not exist.
- **No engine.** `EXEC` validates each push's VA coverage against the mock's
  mapping table and enqueues a fence; no bytes are read, no bus address is
  dereferenced. A push inside a mapped range succeeds whether or not it is a
  valid command stream.
- **No mmap, no relocations, no async, no multi-channel ordering** (the mock's
  fences are per channel; nothing tests cross-channel ordering).
- **Concurrency.** The client table and the mock use process-context spinlocks;
  the reap/pending path was tested single-threaded. The claim pattern
  (RESERVED slot → RM call → re-check) is written for a racing close and was
  not raced.
- **ARM is compile-only** (per-TU), not run: no ARM unit-tier boot on this
  branch, and the full ARM link is broken by `wp-arm`.
- **Error-code divergences from a real DRM**, deliberate and documented: an
  unknown request number is `-ENOTTY` (Linux returns `-EINVAL`), and an unknown
  GEM handle is `-EINVAL` (Linux returns `-ENOENT`). Neither is branched on by
  libdrm/nouveau, but a client that did would see a difference.
- **One VA space per file** is enforced (`-EBUSY` on a second `VM_INIT`) because
  `drm_nouveau_vm_bind` carries no VM id; that is a property of the uAPI, not a
  shortcut, but the `-EBUSY` choice itself is ours, not nouveau's.

---

## 7. Next step

1. **GEM CPU mapping**: `GEM_NEW` accepting `CPU|MAPPABLE`, a page-backed
   allocation in the mock, `mmap` on the node, then `GEM_CPU_PREP/FINI` as named
   rows. Without it no userspace client can fill a push buffer, so it gates
   every later measurement.
2. **NVIF skeleton** (host classes only: device, channel, engine) — or, cheaper
   and less faithful, a winsys adapter in `src/user/` that drives `CHANNEL_ALLOC`
   + `EXEC` today. Pick one deliberately; do not let both grow.
3. Then a real userspace probe (`src/user/nvshim_drm_probe.c`) so the compat
   ioctl route and the layout are proved from the other side of the syscall
   boundary — the gap that makes §6's first bullet true.
4. When FWSEC unfences: `nv_rm_gsp.c` behind `struct nv_rm_if` (the seam did not
   need to change for this branch beyond `submit_va`), flip `mock` to 0, and
   re-point `ptimer`/`graph_units` from `-ENOSYS` to real reads.
