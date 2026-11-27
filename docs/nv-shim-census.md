# WP-CENSUS — the shim's *client* surface: 0 numbers answered by absence, 0 served widths that disagree with the struct a stock nvk sends

Card: `t_d20b014f`. Base: `nv/integ-swarm` @ `e300fadf` (the WP-EVENTFD fold,
`g_drm_rows[]` = 25, `g_drm_refusals[]` = 11). Branch: `nv/wp-census`.
**Doc-only**: no code change, no tier rerun, no GPU boot, no VM boot, no
`docs/nv-plan.md` edit. Mesa 25.0.7 is read where it is
(`/home/sarah/cp/vk-build/src/mesa-25.0.7`) and never committed.

---

## 1. The gap this closes, and the two questions it asks

`docs/nv-shim-eventfd.md` §8 says the quiet part out loud:

> No real winsys drives `0xcf`: this is a **completeness row, not a
> client-path row**. It closes the syncobj surface to "served or refused by
> design", and no nvk progress may be claimed from it.

Every previous shim rung measured the **in-kernel** answer: a number is in
`g_drm_rows[]` (served, size-checked), in `g_drm_refusals[]` (refused by name,
`-ENOSYS`), or in neither (`-ENOTTY`, **no row on the console**). The last case
is the only silent one — a real client would fail with nothing in the log
naming what it asked for. This card measures that column from the *client* side
instead of the shim's:

1. **Is any number a stock nvk can issue ABSENT from both tables?** (target 0)
2. **Does any served row's pinned width disagree with the `sizeof()` of the
   struct Mesa 25.0.7's nouveau stack sends?** A wrong width is `-EINVAL` on a
   row that reads as served.

**Result: 0 ABSENT, and 25/25 served widths match the client's own encoded
sizes.** The first refusal on a real client path is the engine-class
`NVIF_IOCTL_V0_NEW`, `need=gsp_rm` — the `t_9bf9bc0c` wall, not shim work.
Sections 2–4 are the method (including one trap worth knowing), 5–7 the tables,
8 the first refusal, 9 what is *not* on the surface, 10 what remains open.

### Two corrections to the premise, both measured

- **An absent number is `-ENOTTY`, not `-ENOSYS`.** `nv_shim_drm_ioctl()`
  answers `drm_refusal_name()` first (`src/kernel/nv_shim.c:5493`), then
  `drm_row_size() < 0` → **`-ENOTTY`** (`:5501-5502`, `:5553`), and the tier
  pins it (`src/kernel/nv_shim_drm_test.c:283-285`). The count of absent
  numbers is unaffected; the errno a client would see is `ENOTTY` ("Inappropriate
  ioctl for device"), which is also what Linux's `drm_ioctl` gives an
  unimplemented number.
- **The card's "or `src/include/nv_drm_uapi.h`" is not a substitute for the
  client's header.** See §2: two `drm.h` copies live in this build and they
  disagree by 8 bytes on one struct. Checking the shim against the wrong one
  produces two *false* mismatches; checking against the right one produces none.

---

## 2. The header leg (why a naive width check fails)

Mesa 25.0.7 ships a vendored DRM uAPI header, and the build also has a libdrm.
They are not the same vintage:

| header | `struct drm_syncobj_handle` | `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` |
|---|---|---|
| Mesa tree, `include/drm-uapi/drm.h:903-909` | **16 bytes** (no `point`) | `0xC01064C1` (`:1217`) |
| libdrm 2.4.131, `libdrm/include/libdrm/drm.h:951-959` | **24 bytes** (`point`) | `0xC01864C1` (`:1274`) |

Which one is the *client leg*? The one libdrm was compiled with, because the
request is built inside libdrm, not in Mesa:

- nvk compiles with `-I../src/mesa-25.0.7/include` **and**
  `-I/home/sarah/cp/vk-build/libdrm/include/libdrm`
  (`/home/sarah/cp/vk-build/build-mesa-nvk25/compile_commands.json`, the
  `nvkmd_nouveau_ctx.c` entry).
- `libvulkan_nouveau.so` links **`/home/sarah/cp/vk-build/libdrm/lib/libdrm.a`**
  (the same build.ninja entry) — statically, so there is no second libdrm at
  runtime.
- The shipped object contains the request the client actually sends:

```
$ objdump -d build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so \
    | grep -n 'c01864c1'
1286906:  bf2e9d:  be c1 64 18 c0   mov $0xc01864c1,%esi   ; drmSyncobjHandleToFD   @bf2e70
1287059:  bf306d:  be c1 64 18 c0   mov $0xc01864c1,%esi   ; drmSyncobjExportSyncFile
```

`0xC01864C1` — the 24-byte struct. `vk_drm_syncobj.c:29` includes
`"drm-uapi/drm.h"`, but only for its **flags**; the struct and the number come
from libdrm 2.4.131's `xf86drm.c` (`:4997-5010` handle→fd, `:5039-5054`
export sync_file, `:5012-5025` fd→handle, `:5027-5037` import sync_file).

So: **core DRM rows are checked against libdrm 2.4.131's `drm.h`**, and
**`DRM_NOUVEAU_*` rows against Mesa's own
`include/drm-uapi/nouveau_drm.h`** (what `nouveau_*.c` includes as
`"drm-uapi/nouveau_drm.h"`, command indices at `:435-452`, ioctl macros at
`:500-515`).

A consequence worth recording, not fixing: the served `0xc1`/`0xc2` contract
(24 bytes, with `point`) is the contract of **libdrm ≥ 2.4.13x**. A client
linked against an older libdrm would encode 16 and get `-EINVAL` from a served
row — exactly the failure mode this census exists to detect. The pinned build
(`libdrm-2.4.131`, `libdrm.a` inside `libvulkan_nouveau.so`) is on the right
side of that line.

---

## 3. What counts as "the client surface"

**A. The authoritative command set** — `driver_ioctls[]` in
`src/nouveau/drm-shim/nouveau_noop.c:396-411`: 15 nouveau commands. This is the
list Mesa's own DRM shim implements, i.e. the set the nouveau stack needs to run
without a kernel.

**B. What the real paths add** — every `drmCommandWrite*` /
`drmSyncobj*` / `drmPrime*` / `drmCloseBufferHandle` / `drmGetVersion` /
`drmGetCap` call in `src/nouveau/{winsys,vulkan}` and in the generic
`src/vulkan/runtime/vk_drm_syncobj.c` that nvk uses: **15 more numbers** (core
DRM rows, not nouveau commands).

**A ∪ B = 30 numbers.** Of those, **24 are served** and **6 are refused by
name**; **0 are absent**. Nothing else in the tree issues an ioctl on the
nouveau fd: no C file in `src/nouveau` calls `drmIoctl()` directly (grep returns
only the two call sites in `compiler/nak/hw_runner.rs`, a standalone Rust
developer tool that issues **EXEC** and **SYNCOBJ_TIMELINE_WAIT** — both already
in the set, and neither linked into the driver). `drmGetDevices2()`
(`src/vulkan/runtime/vk_instance.c:406`) opens `/dev/dri` nodes that do not exist
here — the hook at `src/nouveau/vulkan/nvk_instance.c:50-103` exists precisely
because of that, and it runs the same per-DRM path (GETPARAM first, `:68`).

Row markers used in the tables:

- **†** — the call site is reached only after physical-device creation
  succeeds. The row is still part of the surface a served device issues, but on
  the current shim that point is behind §8's first refusal.
- **refused** — `g_drm_refusals[]`, keyed on the number only: the answer is
  `-ENOSYS` **before** any size check, so no width is validated (and none needs
  to be).

---

## 4. Table 1 — every DRM ioctl the nouveau stack in Mesa 25.0.7 can issue

The `shim` column is the `g_drm_rows[]` literal (`src/kernel/nv_shim.c:1519-1557`)
or the `g_drm_refusals[]` entry (`:1571-1582`); the handler column is where the
number is answered.

### 4.1 The 15 commands in Mesa's own DRM shim (`nouveau_noop.c:396-411`)

| # | ioctl | nr | encoded | client anchor(s) in Mesa 25.0.7 | shim answer (nv_shim.c) | width | verdict |
|---|---|---|---|---|---|---|---|
| 1 | `DRM_IOCTL_NOUVEAU_GETPARAM` | `0x40` | `0xC0106440` | `nouveau_noop.c:397`; `nvk_instance.c:68`; `winsys/nouveau_device.c:166` (issued `:303`, `:337`, `:343`, `:346`, `:407`, `:422`, `:432`) | served `:1525`, `op_drm_getparam` `:1811` | 16 | match |
| 2 | `DRM_IOCTL_NOUVEAU_NVIF` | `0x47` | length-encoded (min `0xC0186447`) | `nouveau_noop.c:398`; `nouveau_device.c:202`, `:229`; `nouveau_context.c:30`, `:55`, `:111` | served as a **length** row `:1608-1610`, `op_drm_nvif` `:5449` | min 24 | match (§6) |
| 3 | `DRM_IOCTL_NOUVEAU_CHANNEL_ALLOC` | `0x42` | `0xC0586442` | `nouveau_noop.c:399`; `nouveau_context.c:156` | served `:1526`, `op_drm_channel_alloc` `:3197` | 88 | match |
| 4 | `DRM_IOCTL_NOUVEAU_CHANNEL_FREE` | `0x43` | `0x40046443` | `nouveau_noop.c:400`; `nouveau_context.c:139` | served `:1527`, `op_drm_channel_free` `:3316` | 4 | match |
| 5 | `DRM_IOCTL_NOUVEAU_GROBJ_ALLOC` | `0x44` | — (no struct, no ioctl macro) | `nouveau_noop.c:401` | refused `:1574` (`grobj_alloc`) | n/a | refused |
| 6 | `DRM_IOCTL_NOUVEAU_NOTIFIEROBJ_ALLOC` | `0x45` | — (no ioctl macro) | `nouveau_noop.c:402`; gallium `winsys/nouveau/drm/nouveau.c:139` | refused `:1575` (`notifier_obj`) | n/a | refused |
| 7 | `DRM_IOCTL_NOUVEAU_GPUOBJ_FREE` | `0x46` | — (no ioctl macro) | `nouveau_noop.c:403`; gallium `nouveau.c:255` | refused `:1576` (`gpuobj_free`) | n/a | refused |
| 8 | `DRM_IOCTL_NOUVEAU_GEM_NEW` | `0x80` | `0xC0306480` | `nouveau_noop.c:404`; `nouveau_bo.c:123` † | served `:1531`, `op_drm_gem_new` `:1918` | 48 | match |
| 9 | `DRM_IOCTL_NOUVEAU_GEM_PUSHBUF` | `0x81` | `0xC0406481` | `nouveau_noop.c:405`; `nouveau_context.c:247` (no nvk caller; gallium `nouveau.c:1187`, `:1566`) | refused `:1580` (`gem_pushbuf`) | n/a | refused |
| 10 | `DRM_IOCTL_NOUVEAU_GEM_CPU_PREP` | `0x82` | `0x40086482` | `nouveau_noop.c:406`; `nouveau_bo.c:306` (no nvk caller; gallium `nouveau.c:932`) | refused `:1581` (`gem_cpu_prep`) | n/a | refused |
| 11 | `DRM_IOCTL_NOUVEAU_GEM_INFO` | `0x84` | `0xC0286484` | `nouveau_noop.c:407`; `nouveau_bo.c:188` † | served `:1532`, `op_drm_gem_info` `:2024` | 40 | match |
| 12 | `DRM_IOCTL_NOUVEAU_GEM_CPU_FINI` | `0x83` | `0x40046483` | `nouveau_noop.c:408` (**no caller anywhere in the tree**) | refused `:1582` (`gem_cpu_fini`) | n/a | refused |
| 13 | `DRM_IOCTL_NOUVEAU_VM_INIT` | `0x50` | `0xC0106450` | `nouveau_noop.c:409`; `nouveau_device.c:296` | served `:1528`, `op_drm_vm_init` `:3006` | 16 | match |
| 14 | `DRM_IOCTL_NOUVEAU_VM_BIND` | `0x51` | `0xC0286451` | `nouveau_noop.c:410`; `nouveau_bo.c:31`, `:58`; `nvkmd/nouveau/nvkmd_nouveau_ctx.c:337`; `nvkmd_nouveau_va.c:106` † | served `:1529`, `op_drm_vm_bind` `:3090` | 40 | match |
| 15 | `DRM_IOCTL_NOUVEAU_EXEC` | `0x52` | `0xC0286452` | `nouveau_noop.c:411`; `nvkmd_nouveau_ctx.c:143`, `:248` † | served `:1530`, `op_drm_exec` `:4804` | 40 | match |

### 4.2 The 15 core DRM numbers the same stack issues on the same fd

| # | ioctl | nr | encoded | client anchor(s) in Mesa 25.0.7 | shim answer (nv_shim.c) | width | verdict |
|---|---|---|---|---|---|---|---|
| 16 | `DRM_IOCTL_VERSION` | `0x00` | `0xC0406400` | `winsys/nouveau_device.c:272` (`drmGetVersion`); `nvkmd_nouveau_pdev.c:24` | served `:1520`, `op_drm_version` `:1774` | 64 | match |
| 17 | `DRM_IOCTL_GEM_CLOSE` | `0x09` | `0x40086409` | `winsys/nouveau_bo.c:220`, `:265` (`drmCloseBufferHandle`) † | served `:1521`, `op_drm_gem_close` `:2950` | 8 | match |
| 18 | `DRM_IOCTL_GET_CAP` | `0x0c` | `0xC010640C` | `src/vulkan/runtime/vk_drm_syncobj.c:439` (`DRM_CAP_SYNCOBJ_TIMELINE`), from `nvkmd_nouveau_pdev.c:128` † | served `:1522`, `op_drm_get_cap` `:1791` | 16 | match |
| 19 | `DRM_IOCTL_PRIME_HANDLE_TO_FD` | `0x2d` | `0xC00C642D` | `winsys/nouveau_bo.c:312` (`drmPrimeHandleToFD`), from `nvkmd_nouveau_mem.c:248` † | served `:1523`, `op_drm_prime_handle_to_fd` `:2397` | 12 | match |
| 20 | `DRM_IOCTL_PRIME_FD_TO_HANDLE` | `0x2e` | `0xC00C642E` | `winsys/nouveau_bo.c:168` (`drmPrimeFDToHandle`) † | served `:1524`, `op_drm_prime_fd_to_handle` `:2515` | 12 | match |
| 21 | `DRM_IOCTL_SYNCOBJ_CREATE` | `0xbf` | `0xC00864BF` | `vk_drm_syncobj.c:56`, `:407`; `nvkmd_nouveau_ctx.c:75` † | served `:1533`, `op_drm_syncobj_create` `:3542` | 8 | match |
| 22 | `DRM_IOCTL_SYNCOBJ_DESTROY` | `0xc0` | `0xC00864C0` | `vk_drm_syncobj.c:82`, `:308`, `:445`; `nvkmd_nouveau_ctx.c:104` † | served `:1534`, `op_drm_syncobj_destroy` `:3599` | 8 | match |
| 23 | `DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD` | `0xc1` | `0xC01864C1` | `vk_drm_syncobj.c:324`, `:146`, `:358` † | served `:1538`, `op_drm_syncobj_handle_to_fd` `:4547` | 24 | match (§2) |
| 24 | `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE` | `0xc2` | `0xC01864C2` | `vk_drm_syncobj.c:302`, `:341` † | served `:1539`, `op_drm_syncobj_fd_to_handle` `:4678` | 24 | match (§2) |
| 25 | `DRM_IOCTL_SYNCOBJ_WAIT` | `0xc3` | `0xC02864C3` | `vk_drm_syncobj.c:156`, `:274`, `:429`; `nvkmd_nouveau_ctx.c:235` † | served `:1540`, `op_drm_syncobj_wait` `:3748` | 40 | match |
| 26 | `DRM_IOCTL_SYNCOBJ_RESET` | `0xc4` | `0xC01064C4` | `vk_drm_syncobj.c:131` † | served `:1541`, `op_drm_syncobj_set(…,0)` `:4480` | 16 | match |
| 27 | `DRM_IOCTL_SYNCOBJ_SIGNAL` | `0xc5` | `0xC01064C5` | `vk_drm_syncobj.c:98` † | served `:1542`, `op_drm_syncobj_set(…,1)` `:4480` | 16 | match |
| 28 | `DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT` | `0xca` | `0xC03064CA` | `vk_drm_syncobj.c:263`, `:269` † | served `:1543`, `op_drm_syncobj_timeline_wait` `:3816` | 48 | match |
| 29 | `DRM_IOCTL_SYNCOBJ_QUERY` | `0xcb` | `0xC01864CB` | `vk_drm_syncobj.c:115` † | served `:1546`, `op_drm_syncobj_query` `:3992` | 24 | match |
| 30 | `DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL` | `0xcd` | `0xC01864CD` | `vk_drm_syncobj.c:63`, `:96` † | served `:1552`, `op_drm_syncobj_timeline_signal` `:3890` | 24 | match |

**ABSENT count: 0 / 30.** Every number in A ∪ B is answered by a row or a
named refusal; no client request lands in the silent `-ENOTTY` bucket.

Reading the refused six: **none of them is on nvk's path.** `GROBJ_ALLOC`,
`NOTIFIEROBJ_ALLOC`, `GPUOBJ_FREE` and `GEM_CPU_FINI` have no caller in Mesa
25.0.7 at all (the first three are marked `/* deprecated */` at
`include/drm-uapi/nouveau_drm.h:439-441`, `GEM_CPU_FINI` has a macro at
`:510` but nothing issues it); `GEM_PUSHBUF`'s only callers are
`nouveau_ws_context_killed()` (`nouveau_context.c:237-249`, itself uncalled) and
gallium; `GEM_CPU_PREP`'s only callers are `nouveau_ws_bo_wait()`
(`nouveau_bo.c:298-307`, uncalled) and gallium. So the six cost nvk nothing —
and they are the reason "refused by name" is not the same claim as "on the
client path".

---

## 5. Table 2 — width check on every served row (26 numbers)

Method: two compiled probes, one per header leg (§2), plus a probe against the
shim's own structs; then a mechanical `join` of the shim's `(nr,width)` against
the client's encoded `(nr,size)`:

```
shim rows: 25
client-encoded numbers: 28
--- rows in shim table with no matching client number:   (none)
--- same number, different width (shim vs client):       (none)
--- identical: 25
```

`client-encoded` is 28 because it also carries the three **refused** nouveau rows
whose header defines a width (`0x81` 64, `0x82` 8, `0x83` 4). A refusal is
answered before the size check, so there is no served width to compare — but
their encodings are printed all the same, which is why they are in the list and
not in the table.

| nr | ioctl | shim width | `nv_shim.c` | client `sizeof()` | client header anchor | client encoded | verdict |
|---|---|---|---|---|---|---|---|
| `0x00` | VERSION | 64 | `:1520` | 64 | libdrm `drm.h:133` (`struct drm_version`) | `0xC0406400` | **match** |
| `0x09` | GEM_CLOSE | 8 | `:1521` | 8 | libdrm `drm.h:601` (`drm_gem_close`) | `0x40086409` | **match** |
| `0x0c` | GET_CAP | 16 | `:1522` | 16 | libdrm `drm.h:811` (`drm_get_cap`) | `0xC010640C` | **match** |
| `0x2d` | PRIME_HANDLE_TO_FD | 12 | `:1523` | 12 | libdrm `drm.h:926` (`drm_prime_handle`) | `0xC00C642D` | **match** |
| `0x2e` | PRIME_FD_TO_HANDLE | 12 | `:1524` | 12 | libdrm `drm.h:926` | `0xC00C642E` | **match** |
| `0x40` | NOUVEAU_GETPARAM | 16 | `:1525` | 16 | Mesa `drm-uapi/nouveau_drm.h:78` | `0xC0106440` | **match** |
| `0x42` | NOUVEAU_CHANNEL_ALLOC | 88 | `:1526` | 88 | Mesa `drm-uapi/nouveau_drm.h:93` | `0xC0586442` | **match** |
| `0x43` | NOUVEAU_CHANNEL_FREE | 4 | `:1527` | 4 | Mesa `drm-uapi/nouveau_drm.h:111` | `0x40046443` | **match** |
| `0x47` | NOUVEAU_NVIF | min 24 | `:1608-1610` | 24 (`nvif_ioctl_v0`) | Mesa `src/nouveau/drm/nvif/ioctl.h:5-32` | length-encoded | **match** (§6) |
| `0x50` | NOUVEAU_VM_INIT | 16 | `:1528` | 16 | Mesa `drm-uapi/nouveau_drm.h:262` | `0xC0106450` | **match** |
| `0x51` | NOUVEAU_VM_BIND | 40 | `:1529` | 40 | Mesa `drm-uapi/nouveau_drm.h:338` | `0xC0286451` | **match** |
| `0x52` | NOUVEAU_EXEC | 40 | `:1530` | 40 | Mesa `drm-uapi/nouveau_drm.h:404` | `0xC0286452` | **match** |
| `0x80` | NOUVEAU_GEM_NEW | 48 | `:1531` | 48 | Mesa `drm-uapi/nouveau_drm.h:152` | `0xC0306480` | **match** |
| `0x84` | NOUVEAU_GEM_INFO | 40 | `:1532` | 40 | Mesa `drm-uapi/nouveau_drm.h:142` | `0xC0286484` | **match** |
| `0xbf` | SYNCOBJ_CREATE | 8 | `:1533` | 8 | libdrm `drm.h:936` | `0xC00864BF` | **match** |
| `0xc0` | SYNCOBJ_DESTROY | 8 | `:1534` | 8 | libdrm `drm.h:942` | `0xC00864C0` | **match** |
| `0xc1` | SYNCOBJ_HANDLE_TO_FD | 24 | `:1538` | 24 | libdrm `drm.h:951-959` | `0xC01864C1` | **match** (see §2) |
| `0xc2` | SYNCOBJ_FD_TO_HANDLE | 24 | `:1539` | 24 | libdrm `drm.h:951-959` | `0xC01864C2` | **match** (see §2) |
| `0xc3` | SYNCOBJ_WAIT | 40 | `:1540` | 40 | libdrm `drm.h:974` | `0xC02864C3` | **match** |
| `0xc4` | SYNCOBJ_RESET | 16 | `:1541` | 16 | libdrm `drm.h:1034` (`drm_syncobj_array`) | `0xC01064C4` | **match** |
| `0xc5` | SYNCOBJ_SIGNAL | 16 | `:1542` | 16 | libdrm `drm.h:1034` | `0xC01064C5` | **match** |
| `0xca` | SYNCOBJ_TIMELINE_WAIT | 48 | `:1543` | 48 | libdrm `drm.h:992` | `0xC03064CA` | **match** |
| `0xcb` | SYNCOBJ_QUERY | 24 | `:1546` | 24 | libdrm `drm.h:1041` (`timeline_array`) | `0xC01864CB` | **match** |
| `0xcc` | SYNCOBJ_TRANSFER | 32 | `:1551` | 32 | libdrm `drm.h:961` | `0xC02064CC` | **match** |
| `0xcd` | SYNCOBJ_TIMELINE_SIGNAL | 24 | `:1552` | 24 | libdrm `drm.h:1041` | `0xC01864CD` | **match** |
| `0xcf` | SYNCOBJ_EVENTFD | 24 | `:1557` | 24 | libdrm `drm.h:1025` | `0xC01864CF` | **match** |

**25/25 rows match; 0 mismatches.** The second, independent check — that each
literal equals `sizeof()` of the struct the shim itself declares for it — also
passes for all 25 (probe below), so the table cannot be internally inconsistent
either:

```
nvshim_drm_version 64   gem_close 8   cap 16   prime_handle 12   getparam 16
channel_alloc 88   channel_free 4   vm_init 16   vm_bind 40   exec 40
gem_new 48   gem_info 40   syncobj_create 8   syncobj_destroy 8   syncobj_handle 24
syncobj_wait 40   syncobj_array 16   syncobj_timeline_wait 48   timeline_array 24
syncobj_transfer 32   syncobj_eventfd 24
nvif: hdr 24  new 32  mthd 8  sclass 8  oclass 8  info 104  v0 16  MIN=24
```

That is the same set the unit tier pins (`src/kernel/nv_shim_drm_test.c:67-94`,
`:184-213`), which is exactly why the shim's numbers were consistent *with
itself* before this card — and why nothing had ever compared them to a client
header.

---

## 6. Table 3 — the NVIF surface a stock nvk actually uses

`DRM_IOCTL_NOUVEAU_NVIF` is the one row whose encoded `_IOC` size is a **buffer
length**, not a struct width (`drmCommandWrite` encodes the caller's `size`:
libdrm `xf86drm.c:3143-3155`), so the shim pins the minimum (24) and each type
validates its own shape (`nv_shim.c:1602-1610`, `op_drm_nvif` `:5449-5484`).

| request | owner / route / type | oclass / method / token | client length | client anchor | shim handler → answer |
|---|---|---|---|---|---|
| NEW **device** | `OWNER_ANY (0xff)` / `route 0x00` / `NVIF_IOCTL_V0_NEW (0x02)` | `oclass NV_DEVICE (0x80)`, `token`=`object`= the ws device struct | 72 (24+32+16) | `winsys/nouveau_device.c:174-203` | `nvif_new` `:5160` → row `:5202-5209` **served=device** |
| MTHD **NV_DEVICE_V0_INFO** | `OWNER_ANY` / `route 0x00` / `MTHD (0x04)` | `method 0x00` (`NV_DEVICE_V0_INFO`), object = the device cookie | 136 (24+8+104) | `winsys/nouveau_device.c:206-232` | `nvif_mthd` `:5349` → row `:5434-5443` **served** (`nvif info`) |
| SCLASS on a channel | `owner 0` / `route 0xff` / `SCLASS (0x01)` | `token` = channel, `count = 16` | 160 (24+8+16×8) | `winsys/nouveau_context.c:35-57` | `nvif_sclass` `:5289` → row `:5332-5343` **served**, `count=6` |
| NEW **engine class** | `owner 0` / `route 0xff` / `NEW (0x02)` | `oclass` from SCLASS, `token` = channel | 56 (24+32) | `winsys/nouveau_context.c:81-113` | `nvif_new` `:5228-5241` → row `:5230-5239` **refused** `need=gsp_rm` |
| DEL | `owner 0` / `route 0x00` / `DEL (0x03)` | object = the cookie (unknown is not an error) | 24 | `nouveau_context.c:12-33` (subchan), teardown | `nvif_del` `:5253` → row `:5278-5284` **served** |

The engine-class ids SCLASS reports, and which of them the client asks for, are
`g_nvif_engines[]` (`nv_shim.c:5100-5106`) and the class header
(`src/include/nv_nvif_uapi.h:214-220`):

```
0x0000902d fermi_twod_a     0x0000a140 kepler_inline_m2mf   0x0000c797 ampere_b
0x0000c7c0 ampere_compute_b 0x0000c6b5 ampere_dma_copy_a    0x0000c7b5 ampere_dma_copy_b
```

Refused-but-not-issued: `MTHD NV_DEVICE_V0_TIME` (`nv_shim.c:5372-5377`,
`need=ptimer`) and `MTHD GCX_READY` (`:5378-5383`, `need=gsp_rm`) are refused
because a client *may* ask, but no Mesa 25.0.7 caller does — only
`NV_DEVICE_V0_INFO` appears in the tree (`nouveau_device.c:221`;
`nouveau_noop.c:198`).

---

## 7. The first refusal on a real client path

**It is the engine-class NVIF NEW, and there is no earlier non-RM gate.** The
ordered walk of everything a stock nvk issues from `open()` to the wall:

| # | step | client anchor | shim answer |
|---|---|---|---|
| 1 | GETPARAM `PCI_DEVICE` (the HobbyOS enumerate hook, its own fd) | `nvk_instance.c:58` (open), `:68` (ioctl) | served |
| 2 | `drmGetVersion` → VERSION `0x00` (`drm_device_is_nouveau`) | `nvkmd_nouveau_pdev.c:24`, called `:71` | served, name `nouveau` |
| 3 | `drmGetVersion` → VERSION `0x00` (ws device) + the `>= 0x01000301` gate | `nouveau_device.c:272`, gate `:290` | served, **1.3.1** (`op_drm_version` `:1774-1789`) — gate clears |
| 4 | VM_INIT `0x50` | `nouveau_device.c:296` | served → `has_vm_bind = true` |
| 5 | NVIF NEW `NV_DEVICE` | `nouveau_device.c:202` | served |
| 6 | GETPARAM `PCI_DEVICE` | `nouveau_device.c:303` | served |
| 7 | NVIF MTHD `NV_DEVICE_V0_INFO` | `nouveau_device.c:229` | served |
| 8 | GETPARAM `EXEC_PUSH_MAX` (17), `VRAM_BAR_SIZE` (18) | `nouveau_device.c:337`, `:343` | served (`drm_param_value` `:1728-1759`) |
| 9 | GETPARAM `GRAPH_UNITS` (13) — **hard gate**: failure ⇒ `goto out_err` | `nouveau_device.c:346-353` | served from the mock's declared 11 GPC / 64 TPC (`nv_rm_mock.c:81`), or `-ENOSYS need=mock_has_no_topo` if a test clears it |
| 10 | CHANNEL_ALLOC `0x42` | `nouveau_context.c:156` | served (88) |
| 11 | NVIF SCLASS | `nouveau_context.c:55` | served, 6 classes |
| 12 | **NVIF NEW engine class** | `nouveau_context.c:111`, from `:168` (the copy branch), with `engines = ~0` at `nouveau_device.c:359` | **`-ENOSYS`, `nv_shim.c:5235`** |

**The exact request:** `nouveau_ws_context_find_class(classes, 0xb5)`
(`nouveau_context.c:167`, `:108-118`) takes `MAX2` over the classes whose low
byte is `0xb5` — both copy classes match, so it asks for **`0xC7B5`
(`AMPERE_DMA_COPY_B`)**. The shim's row is
`nv_shim.c:5230-5240`:

```
nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm \
  missing=rm_chan_object alloc=ce_alloc engine=copy1
```

and the client's own report, one layer up (`nouveau_context.c:118-128`):

```
nvk: copy engine class object 0xc7b5 refused: Function not implemented (-38)
```

then `nouveau_ws_device_new()` returns `NULL`
(`nouveau_device.c:359-360` → `nvkmd_nouveau_pdev.c:74-76`), so
`nvk_hobbyos_enumerate()` prints `physical device create failed: VkResult -3`
(`nvk_instance.c:95-98`) and enumerates zero devices.

**Why nothing earlier bites.** The three client-side gates before it are all
cleared by served rows, and the one *hard* GETPARAM gate (GRAPH_UNITS) is
served by the mock's declared topology — the shim's own `-ENOSYS` for it is a
test-only state (`nv_rm_mock_set_topo(NV_RM_TOPO_NONE,…)`,
`nv_rm_mock.c:83-96`), not the default. GETPARAM `VRAM_USED` (19) and
`PTIMER_TIME` (14) are refused (`nv_shim.c:1748-1755`) but are non-fatal by
construction (`nouveau_device.c:403-426`: `return 0` on error), and both are
reached *after* `nouveau_ws_device_new()` succeeds — i.e. **behind** row 12 (§9).

So: the refusal is the wall `t_9bf9bc0c` names, and the census found no
servable rung in front of it.

---

## 8. Served rows that are *not* on a client path (and refused rows that are not either)

The complement matters as much as the surface — a "served" row that nothing
issues is a completeness claim, and `docs/nv-shim-eventfd.md` §8 says so for
`0xcf`. The full shim face is 26 served numbers + 11 refused; 30 of those 37
are on the surface above. The other seven:

| row | status | who issues it |
|---|---|---|
| `SYNCOBJ_TRANSFER 0xcc` (`:1551`) | served | generic WSI only: `wsi_create_sync_for_image_syncobj()` (`src/vulkan/wsi/wsi_common_drm.c:342`) calls `drmSyncobjTransfer` at `:372`, from `wsi_common.c:1190`, `:1226` when the swapchain image has `explicit_sync`. Not an nvk queue path, and this node has no window system. |
| `SYNCOBJ_EVENTFD 0xcf` (`:1557`) | served | **nothing**: `grep -rn "SYNCOBJ_EVENTFD\|drmSyncobjEventfd" src/` in Mesa 25.0.7 returns 0 hits. §8's sentence stands, now measured tree-wide. |
| `GEM_FLINK 0x0a` (`:1572`) | refused | nothing in the nouveau stack; the number is a core DRM row (`DRM_IOCTL_GEM_FLINK`), kept for legacy clients. |
| `SETPARAM 0x41` (`:1573`) | refused | `/* deprecated */` in `drm-uapi/nouveau_drm.h:436`; no ioctl macro, no caller. |
| `SVM_INIT 0x48` / `SVM_BIND 0x49` (`:1577-1578`) | refused | gallium only: `gallium/drivers/nouveau/nouveau_screen.c:364`, `nvc0/nvc0_context.c:72`. |
| `GET_ZCULL_INFO 0x53` (`:1579`) | refused | nothing: no definition in either header, no caller. |

That leaves **no** number the shim serves or refuses without a documented
issuer, and **no** number a client issues without a row. Both directions of the
§8 gap are now closed as *measurements*.

---

## 9. What is behind the wall (do not spend a card on it)

Every remaining named refusal on a *reachable* client path is issued after a
physical device exists, which the first refusal prevents:

| refusal | client anchor | why it cannot be reached today |
|---|---|---|
| `gem_cpu_prep` `0x82` | `nouveau_bo.c:306` (via `nouveau_ws_bo_wait`, itself uncalled) | needs a BO, i.e. a created device |
| `gem_pushbuf` `0x81` | `nouveau_context.c:247` (`nouveau_ws_context_killed`, uncalled) | needs a live ws context |
| `GETPARAM VRAM_USED 19`, `PTIMER_TIME 14` | `nouveau_device.c:407`, `:422` (from `nvkmd_nouveau_pdev.c:104-105`, `nvkmd_nouveau_dev.c:79`) | `nvkmd_nouveau_pdev.c:74` — `nouveau_ws_device_new()` must return first |
| `GETPARAM BUS_TYPE/AGP_SIZE/VM_VRAM_BASE/HAS_BO_USAGE/HAS_PAGEFLIP` | none of them are issued by nvk | no caller |

So a rung that served `PTIMER_TIME` (a plausible-looking next step: timestamps
would make `vkGetCalibratedTimestamps` work) or `GEM_CPU_PREP` would change
**nothing observable** while `0xc7b5` refuses: the client never gets far enough
to ask. Recording that here is the point of the table — the shim's refusal
ordering is not the client's issue ordering.

---

## 10. What this closes, and what it does not

**Closes (as a measurement, not a claim of progress):** `docs/nv-shim-eventfd.md`
§8's gap, in the only form it can be closed by a doc-only card. Every one of the
30 numbers a stock nvk in Mesa 25.0.7 can issue is served or refused by name
(**0 ABSENT**), and all 25 served widths equal the client's own encoded size
(**0 mismatches**), against the headers the client actually compiles and links
with.

**Does not close:**

- **No client ran.** Nothing here is evidence that nvk enumerated, created a
  device, or submitted anything; it is a source-level census. The `†` rows are
  issued by this code path on a served device, not by a client on this node.
- **No tier rerun.** Doc-only card; the 606/0 intel and 485/0 ARM rows of
  `e300fadf` remain the code's own gates.
- **The libdrm floor is now a served-contract dependency.** `0xc1`/`0xc2` are
  24 bytes because the pinned build links libdrm 2.4.131. A client built against
  an older libdrm (16-byte `struct drm_syncobj_handle`) would get `-EINVAL` from
  two rows that read as served. Recorded in §2; no action, because the client
  leg is pinned by `tools/mesa/build-nvk.sh`'s libdrm.

**The next rung: none, until R1/R2 in `docs/nv-plan.md` §2.13.** The surface is
whole; the first refusal is the engine-class NVIF NEW (`need=gsp_rm`), which is
the declared native ceiling, not a shim row. Serving anything behind it is
unobservable until that wall moves.

---

## Appendix — reproduction

All of this is read-only against the two trees; the probes are three small C
files compiled in the worker's scratch dir (not committed).

**The client set** (from `src/nouveau` + the generic syncobj runtime):

```
grep -rn "DRM_IOCTL_\|drmIoctl\|drmCommandWrite\|drmSyncobj\|drmPrime\|drmCloseBufferHandle\|drmGetVersion\|drmGetCap" \
  $MESA/src/nouveau $MESA/src/vulkan/runtime/vk_drm_syncobj.c
sed -n '396,411p' $MESA/src/nouveau/drm-shim/nouveau_noop.c      # the authoritative set
```

**Width probe, core rows** (libdrm 2.4.131 — the client leg):

```
gcc -I$LIBDRM/include -I$LIBDRM/include/libdrm -o width_libdrm width_libdrm.c
```

**Width probe, nouveau rows** (Mesa's own vendored uAPI):

```
gcc -I$MESA/include -o width_mesa width_mesa.c    # includes "drm-uapi/nouveau_drm.h"
```

**Width probe, shim-side structs:**

```
gcc -I<repo>/src/include -o shim_widths shim_widths.c    # includes "nv_drm_uapi.h"
```

**NVIF lengths** (Mesa's `src/nouveau/drm/nvif` headers):

```
gcc -I$MESA/src/nouveau/drm -o nvif_len nvif_len.c
# hdr 24 / new 32 / mthd 8 / sclass 8 / oclass 8 / nv_device_v0 16 / info 104
# device NEW 72, MTHD INFO 136, SCLASS×16 160, engine NEW 56, DEL 24
```

**The mechanical 25/25 comparison:** `awk` extracts `(nr,width)` from
`g_drm_rows[]` (`nv_shim.c:1519-1557`), the two probes' `_IOC` encodings are
reduced to `(nr,size)`, and a `join` reports any row without a matching number
or with a different width — both lists empty, 25 identical (output quoted in
§5).

**The decisive header-leg check:**

```
objdump -d build-mesa-nvk25/src/nouveau/vulkan/libvulkan_nouveau.so | grep c01864c1
# bf2e9d / bf306d: mov $0xc01864c1,%esi   (24-byte struct drm_syncobj_handle)
```
