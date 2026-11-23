# SWARM-REPORT — WP-F: `/dev/nvshim`, a nouveau-subset device ABI over a mock RM

Branch `nv/wp-f-shim`, cut from `ce4c44af`. Rung **N2** in `PLAN.md` §4.
**Status: gate met.** A probe creates a device, a memory object, a GPU VM, a
channel, submits a command stream and a fence retires — all in the kernel unit
tier, with no card and no GSP. Claims below are tagged MEASURED (seen in a log
from this branch) or INFERRED.

## 1. What changed

New:

| File | What |
|---|---|
| `src/include/nv_shim.h` | The whole user ABI: 11 implemented requests + 6 named refusals, structs, limits, provenance of every number |
| `src/include/nv_rm.h` | The RM seam (`struct nv_rm_if`) the ABI is served by, plus the mock's test seams |
| `src/kernel/nv_shim.c` | `/dev/nvshim` ioctl dispatcher: client table keyed on (process-group pid, fd), ownership checks, copy-in/out, per-stage `[NVSHIM]` rows |
| `src/kernel/nv_rm_mock.c` | The mock RM: objects, software bus arena, VA intervals, per-channel fence sequence, explicit `kick()` retirement |
| `src/kernel/nv_shim_test.c` | 5 unit-tier suites (interface, refusals, probe, ownership, limits) |

Wiring (existing files, small):

| File | What |
|---|---|
| `src/kernel/fs.c` | `/dev/nvshim` open path, fstat (char 7), read = EOF, write = -EINVAL, lseek = ESPIPE, close hook → `nv_shim_release_fd()` |
| `src/include/fs.h` | `FILE_TYPE_DEVNVSHIM`, appended last |
| `src/include/file_obj.h` | `file_is_devnvshim()` |
| `src/kernel/linux_compat_fd.c` | the ioctl(2) route to `nv_shim_ioctl()` (not arch-gated) |
| `src/kernel/unit_test.c`, `unit_test.h` | one line each to run the suite |

No driver code was copied. The structs and constants follow nouveau's public
uAPI (`include/uapi/drm/nouveau_drm.h`) as a contract; the ioctls themselves are
HobbyOS's own (`_IOC` magic `'N'`), because HobbyOS has no DRM and serves a
subset with smaller structs. Every row in `nv_shim.h` names the nouveau request
it corresponds to.

## 2. The ioctl surface (MEASURED — sizes asserted in `test_nvshim_layout`)

| nr | request | struct / size | semantics | nouveau counterpart |
|---|---|---|---|---|
| 1 | `DEVICE_ALLOC` | `nvshim_device` 40 | in `abi=1, flags=0`; out `dev_id`, `chipset=0x192`, `device_id=0x2684`, `mock=1`, `gsp_required=1`, `features=0x7f`, `name="AD102"`. Second call on one fd → `-EBUSY` | `DRM_IOCTL_NOUVEAU_GETPARAM` (identity) |
| 2 | `DEVICE_FREE` | `nvshim_device_free` 8 | tears the client down (channels → VMs → allocs → device) | teardown side |
| 3 | `GET_PARAM` | `nvshim_param` 16 | serves `PCI_VENDOR`(3) `PCI_DEVICE`(4) `FB_SIZE`(8) `CHIPSET_ID`(11) `VRAM_BAR_SIZE`(18); the rest `-ENOSYS`, unknown `-EINVAL` | `DRM_IOCTL_NOUVEAU_GETPARAM`, **nouveau's own param numbers** |
| 4 | `ALLOC` | `nvshim_alloc` 32 | in `domain` (nouveau GEM domain bits: VRAM `0x2`, GART `0x4`), `size`; out `handle`, `bus`, granted `size`. Rounded to 4 KiB; ≤512 MiB per client, ≤4 GiB each | `DRM_IOCTL_NOUVEAU_GEM_NEW` / `drm_nouveau_gem_info` |
| 5 | `FREE` | `nvshim_free` 8 | frees one memory object; double free → `-EINVAL` | `DRM_IOCTL_NOUVEAU_GPUOBJ_FREE` |
| 6 | `VM_INIT` | `nvshim_vm_init` 16 | one GPU VA space per call (≤4 per client), 4 KiB-aligned size ≤4 GiB | `DRM_IOCTL_NOUVEAU_VM_INIT` |
| 7 | `VM_BIND` | `nvshim_vm_bind` 40 | `op` 0=map / 1=unmap, `va`, `range`, `bo_offset`; overlap, out-of-range and double-unmap are `-EINVAL` | `DRM_IOCTL_NOUVEAU_VM_BIND` (`drm_nouveau_vm_bind_op`) |
| 8 | `CHANNEL_ALLOC` | `nvshim_channel` 32 | in `vm_id`, push-buffer `handle` (≥4 KiB); out `ch_id`, `pushbuf` bus address, `engine=NVSHIM_ENGINE_GR(1)` | `DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC` |
| 9 | `CHANNEL_FREE` | `nvshim_channel_free` 8 | destroys one channel; double free → `-EINVAL` | `DRM_IOCTL_NOUVEAU_CHANNEL_FREE` |
| 10 | `SUBMIT` | `nvshim_submit` 32 | in user `va` + `len` (≤64 KiB, dword multiple); copied into a kernel bounce buffer; out `fence` (per-channel, monotonic, nonzero) | `DRM_IOCTL_NOUVEAU_EXEC`, one push per call |
| 11 | `FENCE_WAIT` | `nvshim_fence_wait` 16 | 0 = retired, `-ETIMEDOUT` on a poll or after `timeout_ms` (clamped to 1 s), `-EINVAL` for a fence this channel never issued | none (nouveau uses syncobjs); shape follows `docs/gpu-abi.md` §5/§10.5 |

Deliberately refused, **routed and answered `-ENOSYS` with a named
`[NVSHIM] refuse <name> errno=ENOSYS` row** (never a silent no-op, never
`-ENOTTY`):

| nr | request | why |
|---|---|---|
| 12 | `GROBJ` | graphics object classes are GSP-RM's |
| 13 | `NOTIFIER` | needs the real RM's notifier channel |
| 14 | `NVIF` | the object interface; every class is GSP-RM's |
| 15 | `SVM_INIT` | shared VA needs MMU notifiers |
| 16 | `GEM_PUSHBUF` | relocations + a real push parser |
| 17 | `SETPARAM` | nouveau itself answers `-EINVAL` (`drm_invalid_op`); we name it |

Error classes: `-ENOTTY` not a request, `-ENOSYS` a refused row, `-ENODEV` no
device on this fd, `-EINVAL` bad struct/foreign id/bad geometry, `-EFAULT`
unmapped user range, `-EBUSY` second device on one fd, `-ENOSPC` table full,
`-ENOMEM` arena/client cap, `-ETIMEDOUT` fence not retired.

## 3. Gates

**Intel build** (`nvswarm build wp-f`): green, no warnings from the new files
(only pre-existing `-Wcomment` noise from `vm.h` / `file_obj.h`).

**Unit tier** (`nvswarm sh wp-f 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'`):

```
  Running test_nvshim_layout...
  Running test_nvshim_refusals...
  Running test_nvshim_probe...
  Running test_nvshim_ownership...
  Running test_nvshim_limits...
Tests run:    518
Tests failed: 0
UNIT TESTS PASSED
```

Baseline was 513; **518 = the 5 new suites, 0 failed** (MEASURED).

**ARM compile gate** — the full ARM *link* is broken at `ce4c44af` for an
unrelated pre-existing reason (`ld.lld: error: kernel .bss exceeds the limine
placement window`; reproduces on an untouched tree, both hosts). Per-TU
compilation of every added/changed source is used instead, and all six build:

```
$ nvswarm sh wp-f 'TOOLS_SKIP=1 BROWSER_SKIP=1 make -j12 ARCH=arm MODE=unit_tests \
    LO_ON_DISK= obj/arm/nv_shim.o obj/arm/nv_rm_mock.o obj/arm/nv_shim_test.o \
    obj/arm/fs.o obj/arm/linux_compat_fd.o obj/arm/unit_test.o'
... 2 warnings generated.  (all -Wcomment, from vm.h / file_obj.h)
```

Object sizes from that build: `nv_shim.o` text 20189 / bss 74032,
`nv_rm_mock.o` text 7213 / bss 20360. The node and the mock are arch-neutral by
design (`nv_shim.c` has no `__x86_64__` guard: the mock needs no hardware), so
this is the real thing being compiled for AArch64, not a stub (MEASURED).

## 4. The probe's transcript (MEASURED, `qemu.log`, `[NVSHIM]` rows)

```
[NVSHIM] refuse grobj_alloc errno=ENOSYS
[NVSHIM] refuse notifier_obj errno=ENOSYS
[NVSHIM] refuse nvif errno=ENOSYS
[NVSHIM] refuse svm errno=ENOSYS
[NVSHIM] refuse gem_pushbuf errno=ENOSYS
[NVSHIM] refuse setparam errno=ENOSYS
  Running test_nvshim_probe...
[NVSHIM] device dev=1 chipset=0x192 pci=0x2684 mock=1 feats=0x7f
[NVSHIM] getparam refused param=14 reason=mock_has_no_card
[NVSHIM] alloc handle=1 bus=0x100000000 size=0x10000 domain=0x2
[NVSHIM] alloc handle=2 bus=0x100010000 size=0x1000 domain=0x4
[NVSHIM] vm vm=1 size=0x100000000
[NVSHIM] bind op=0 va=0x10000000 range=0x10000 handle=1
[NVSHIM] vm_bind refused rm=-22          <- the same range mapped twice
[NVSHIM] channel ch=1 engine=1 pushbuf=0x100000000
[NVSHIM] submit ch=1 len=64 fence=0x1
[NVSHIM] fence ch=1 fence=0x1 retired=0 timeout=poll     <- kick held
[NVSHIM] fence ch=1 fence=0x1 retired=0 timeout=ms clamped=0
[NVSHIM] submit ch=1 len=64 fence=0x2                    <- kick released
[NVSHIM] fence ch=1 fence=0x1 retired=1
[NVSHIM] fence ch=1 fence=0x2 retired=1
[NVSHIM] bind op=1 va=0x10000000 range=0x10000 handle=0
[NVSHIM] device_free dev=1
[NVSHIM] device dev=1 chipset=0x192 pci=0x2684 mock=1 feats=0x7f
[NVSHIM] alloc handle=1 bus=0x100000000 size=0x1000 domain=0x2
  [NVSHIM] probe device+vm+channel+submit+fence: PASS
```

The probe runs through the real path: `process_create()` → real
`addr_space` → real `file_open("/dev/nvshim")` → `nv_shim_ioctl()` with the
request structs and the 64-byte command stream living in the process's own user
pages (`vm_kwrite` in, `vm_user_read` back out). Out fields are prefilled with
`0xFFFFFFFF` before each call, so "the kernel wrote this" is distinguishable
from "it was always zero".

Both directions of the fence are observed: with `nv_rm_mock_hold(1)` the same
`FENCE_WAIT` that later returns 0 returns `-ETIMEDOUT` (poll and 20 ms), and
`nv_rm_mock_pending()` stays 1. The observation could have been negative, and
was.

## 5. Mutation check — the fence-retire path

Mutation applied to `src/kernel/nv_rm_mock.c` `mock_kick()`: replace
`if (c) c->done = p->fence;` with a comment, so the pending list drains without
advancing any channel's retired sequence. Rebuild + full unit tier:

```
  Running test_nvshim_probe...
EXPECT_EQ FAILED: probe_ioc(&pr, NVSHIM_IOC_FENCE_WAIT, &fw, (int)sizeof fw) == 0
                 at src/kernel/nv_shim_test.c:346
EXPECT_EQ FAILED: nv_rm_mock_live() == 0 at src/kernel/nv_shim_test.c:503
EXPECT_EQ FAILED: nv_rm_mock_live() == 0 at src/kernel/nv_shim_test.c:640
Tests run:    518
Tests failed: 3
UNIT TESTS FAILED
```

Restored, rebuilt, re-ran: `Tests run: 518 / Tests failed: 0 / UNIT TESTS
PASSED`. The retire path is covered by a test that was watched failing
(MEASURED).

## 6. Two bugs the gate caught (recorded so the next agent does not repeat them)

1. **A leaked spinlock in `op_alloc`.** The completion path returned `-ENOSPC`
   / `-ENODEV` *after* re-acquiring `g_shim_lock` and never released it. First
   allocation succeeded, second call self-deadlocked: `[LOCKFOREVER] cpu=0
   lock=g_shim_lock owner=1`, symbolized with `llvm-addr2line` to
   `spinlock_acquire` ← `op_alloc`. Fixed by releasing before the error
   returns; all ops now release on every path.
2. **Rows printed their payload but not their head.** The per-stage row budgets
   were zero-initialised, so `row_begin()` suppressed the `[NVSHIM] stage`
   prefix while the payload calls still printed — the log read
   `device dev=1 …` with no stage name. Budgets now start at 8 and the whole
   row is composed in a per-CPU buffer and emitted with one `uart_puts`.

## 7. What is NOT verified

- **No real Vulkan client.** Nothing has attached to `/dev/nvshim` from
  userspace. The ioctl *route* is exercised only in-process (kernel tests call
  `nv_shim_ioctl` directly and `file_open`/`file_close` for the node and its
  release hook); `linux_compat_fd.c`'s `l_ret()` path is untested for this node.
  nvk speaks `DRM_IOCTL_NOUVEAU_*` on `/dev/dri/renderD128`, so a winsys
  adapter is required — this ABI is a documented subset, not DRM.
- **No GSP, no card, no real RM.** `mock=1` in every device row. Nothing here
  says anything about AD102's behaviour; the `nv_rm_gsp.c` backend does not
  exist.
- **No engine.** `SUBMIT` copies, validates and drops the command stream; the
  fence retires because `kick()` says so. No command is parsed and no bus
  address is ever dereferenced.
- **No `mmap`** of an allocation (there are no pages behind the mock's bus
  addresses), no async fences (a wait polls; it never parks), no relocations,
  no multi-channel ordering.
- **Concurrency**: the client table and the mock both use process-context
  spinlocks and were tested single-threaded. The claim-pattern (RESERVED slot →
  RM call → re-check) is written for a racing close but was not raced.
- **ARM is compile-only** (per-TU), not run: no ARM unit-tier boot on this
  branch.

## 8. Next step

1. A userspace probe (`src/user/nvshim_probe.c`) that opens `/dev/nvshim` and
   replays §4's sequence through a real `ioctl(2)`, so the compat route and the
   struct layout are proved from the other side of the syscall boundary.
2. Then a thin winsys shim mapping `DRM_IOCTL_NOUVEAU_{GETPARAM,VM_INIT,
   VM_BIND,CHANNEL_ALLOC,EXEC}` onto these 11 rows, so nvk's probe gets past
   device creation against the mock.
3. When FWSEC unfences: implement `nv_rm_gsp.c` against `struct nv_rm_if`
   (device/channel/fence come from GSP-RM; `allocate` becomes real VRAM; the
   fence moves to the interrupt path) and flip `mock` to 0. Nothing in
   `nv_shim.c` should need to change.
