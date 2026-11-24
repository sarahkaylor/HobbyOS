# SWARM-REPORT — WP-NVK: nvk built for HobbyOS, driven to its named wall

**Scope.** `tools/mesa` configured `-Dvulkan-drivers=` (empty), so the tree had no
Vulkan ICD for nvk. This lane builds one for the x86_64 HobbyOS **Linux-ABI compat
class** (the class a real Vulkan client is), attaches it to `/dev/nvshim`, and drives
it until it either enumerates `AD102 (NVK)` or refuses at a name. The build has since
been made permanent as `tools/mesa/build-nvk.sh` (which configures
`-Dvulkan-drivers=nouveau`, never an empty list); this report describes the lane that
produced it.

**Status: the wall is named and measured.** nvk loads in the guest, creates a Vulkan
instance, opens `/dev/nvshim`, speaks DRM (`VERSION`, `GETPARAM`, `VM_INIT`), creates
the `NV_DEVICE` object through NVIF, reads back **AD102 / chipset 0x192 / family 0xE /
4 GiB**, allocates a channel and its SCLASS list — and then stops because *the engine
class object it needs is an RM object that only GSP-RM can instantiate*:

```
nvk: copy engine class object 0xc7b5 refused: Function not implemented (-38)
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm \
         missing=rm_chan_object alloc=ce_alloc engine=copy1
```

Enumeration is **not** faked: `vkEnumeratePhysicalDevices` returns 0 devices, because
the physical device could not be created. Evidence is the guest's own serial log
(`nvk/serial.log`, row-for-row in §4).

Claims are tagged **MEASURED** (a log from this lane), **SOURCE** (read from a cited
file) or **INFERRED**.

---

## 1. What was built

| artifact | what |
|---|---|
| `libvulkan_nouveau.so` | Mesa's NVK ICD, 24,449,664 B, SONAME `libvulkan_nouveau.so`, `DT_NEEDED`: `libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 ld-linux-x86-64.so.2` — **nothing else** (MEASURED: `readelf -dW`) |
| `src/user/nvk_probe.c` | the smallest loader-equivalent probe: calls the ICD ABI directly (`vk_icdGetInstanceProcAddr` → `vkCreateInstance` → `vkEnumeratePhysicalDevices` → `vkGetPhysicalDeviceProperties`), no loader, no ICD JSON |
| `tools/mesa/build-nvk.sh` | the supported build (this lane's script, folded into the tree's Mesa configuration; `tools/nv/nvk/build.sh` is now a driver of it): sysroot → zlib → libelf → libcxx → libdrm → mesa → probe → audit |
| `tools/mesa/patches/0014-nvk-hobbyos-node-and-named-refusals.patch` | the one Mesa change (8 hunks, 3 files), §2 — with the other Mesa patches |
| `tools/nv/nvk/run_guest.sh` | stages glibc 2.31 + the ICD + the probe into a private `disk.img` copy, boots `MODE=test`, grades the serial log |

`tools/mesa/build-nvk.sh audit` (via `bash tools/nv/nvk/build.sh audit`) passes and is
what the claims in this table rest on (MEASURED):

```
DT_NEEDED: ld-linux-x86-64.so.2 libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0
undefined not in the 2.31 runtime/ld.so: _ITM_deregisterTMCloneTable
  _ITM_registerTMCloneTable __gmon_start__ pidfd_getpid pidfd_spawnp
  posix_spawn_file_actions_addchdir      (all WEAK — resolved as zero)
audit: PASS
```

`tools/mesa/build-hobbyos.sh` is untouched by this lane: it builds the GL DSO with
**0 `DT_NEEDED`** for the compat class, and a Vulkan ICD is a different artifact with a
different dependency closure. What changed when the build became permanent is *where the
Vulkan build lives*: `tools/mesa/build-nvk.sh` is now the tree's Vulkan configuration
(`-Dvulkan-drivers=nouveau`, never an empty list), the patch is
`tools/mesa/patches/0014-…` with the other Mesa patches, and `tools/nv/nvk/build.sh` is a
three-line driver of it. The GL lane keeps its invariants; the Vulkan lane keeps its
harness (`run_guest.sh`, `src/user/nvk_probe.c`).

### 1.1 Why Mesa 25.0.7 and not the tree's 26.2.6 pin

MEASURED, in this environment, and corrected against the first draft of this section
(which said 25.0.7's NVK is "plain C" — it is not; both series use the Rust NAK):

1. **26.2.6 cannot be configured with `-Dllvm=disabled -Dvulkan-drivers=nouveau`.** meson
   stops at
   `src/mesa-26.2.6/meson.build:977:3: ERROR: Feature llvm cannot be disabled: CLC requires LLVM`
   (`~/cp/vk-build/log-mesa-setup.txt`): that series compiles OpenCL kernels into the
   nouveau driver with `mesa-clc`, so the ICD would carry a static LLVM — the same cost
   `tools/cp/vk/build.sh` pays for lavapipe, and the opposite of this artifact's point
   (a 24 MB ICD whose whole closure is the glibc five).
2. **Both series need Rust.** 25.0.7's `meson.build:794` adds the `rust` language for
   `with_nouveau_vk` and `src/nouveau/compiler/meson.build` uses `rust.bindgen` +
   `rust.proc_macro`; this build needs `rustc`, `bindgen` and `cbindgen` (they ship in
   `~/.cargo/bin`, which `build-nvk.sh` puts on PATH). Rust is therefore not what
   separates 25.0.7 from 26.2.6 — LLVM/CLC is.

25.0.7 is the last series where `-Dllvm=disabled` is allowed with
`vulkan-drivers=nouveau`, and it speaks the same Vulkan ICD interface (version 5,
negotiated in the guest — MEASURED). Recommendation: stay on 25.0.7 for the ICD until
somebody wants to pay for a static LLVM in this DSO, or until the tree moves its NVK
build onto `tools/cp/vk/build.sh`'s LLVM lane.

---

## 2. The port patch — two changes, both about *naming* what is missing

### 2.1 Device discovery: HobbyOS has no sysfs and no `/dev/dri`

`nvk` finds a device through `drmGetDevices2()` (`src/vulkan/runtime/vk_instance.c:418`),
which is libdrm walking `/sys/class/drm` + `/dev/dri`. HobbyOS has neither: the node is
`/dev/nvshim`. A stock nvk therefore enumerates **zero** devices and says nothing —
indistinguishable from "no NVIDIA card installed".

The patch adds `nvk_hobbyos_enumerate()`: when `/dev/nvshim` (or `$NVK_HOBBYOS_NODE`)
opens, it builds the one-element `_drmDevice` that node describes (PCI bus, vendor
`0x10de`, **device id read back from the node** with `GETPARAM(PCI_DEVICE)`) and calls
the normal `nvk_create_drm_physical_device()`. Everything after the hook — `drv_fd_is_nouveau`,
VERSION, GETPARAM, VM_INIT, NVIF — is stock nvk, unmodified. Absent node ⇒
`VK_ERROR_INCOMPATIBLE_DRIVER` ⇒ libdrm scan, so the patch is a no-op on Linux.

This is the ABI gap **G1**: *a HobbyOS DRM node has no render-node name*. The cheap
alternative (add `renderD128` to the guest devfs and a minimal `/sys/class/drm` entry)
would keep the driver unpatched; it is kernel work, not taken here.

### 2.2 Named refusals instead of silence

Upstream's `nouveau_ws_device_new()` and `nouveau_ws_context_create()` `goto out_err`
on the first failing step, so the only reason a driver disappears is a `VkResult`. The
patch prints the failing step and errno:

* `nvk: GETPARAM(GRAPH_UNITS=%d) failed: %s (%d)` (did not fire — see G3),
* `nvk: %s engine class object 0x%04x refused: %s (%d)` (fired: `copy 0xc7b5 -38`).

Without it the guest log would have shown `count=0` and nothing else.

---

## 3. Build-side gap: zlib was a hard `DT_NEEDED`

MEASURED before the fix: the ICD carried `DT_NEEDED libz.so.1` (Mesa's disk shader
cache compresses) and the compat class stages no libz, so `RTLD_NOW` in the guest
would have failed on `inflate`/`deflate`/`crc32`. Two honest fixes; both are in
`tools/mesa/build-nvk.sh`:

* `-Dshader-cache=disabled` (removes Mesa's use), **and**
* a static PIC `libz.a` in the sysroot, so the DSO links zlib in rather than adding a
  guest library.

`DT_NEEDED` is now exactly the five 2.31 names above (MEASURED, §1).

---

## 4. The run — row by row (MEASURED, `nvk/serial.log`)

Guest: `make ARCH=intel MODE=test hobbyos.elf disk.img`, private image copy,
`qemu-system-x86_64 -M q35 … -display none -serial stdio`, KVM.

```
v2 loader: PID=2 entry=0x1008001100 … native=0 interp=0x1008000000   ← compat class, glibc ld.so
[NVK] probe start (ICD used directly, no loader)
[NVK] icd negotiate: result=0 version=5
[NVK] instanceExtensionProperties: result=0 count=18
[NVK] createInstance: VK_SUCCESS (0)

[NVSHIM] drm device dev=1 chipset=0x192 mock=1                        ← GETPARAM(CHIPSET_ID)
[NVSHIM] drm getparam param=4 value=0x2684                            ← PCI device (read by the hook)
[NVSHIM] drm vm vm=1 umd_end=0x8000000000 total=0x10000000000         ← VM_INIT {1<<39,1<<39}
[NVSHIM] nvif new class=0x80 cookie=0x1040005dd0 dev=1 served=device   ← NV_DEVICE object
[NVSHIM] nvif info chip=AD102 chipset=0x192 fam=0xe ram=0x100000000    ← NV_DEVICE_V0_INFO
[NVSHIM] drm getparam param=13 value=0x400b gpc=11 tpc=64 src=declared ← GRAPH_UNITS
[NVSHIM] drm channel ch=1 pushbuf=0x2091e2000 size=0x10000
[NVSHIM] nvif sclass ch=1 count=6 first=0x902d 3d=0xc797
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm \
         missing=rm_chan_object alloc=ce_alloc engine=copy1
nvk: copy engine class object 0xc7b5 refused: Function not implemented (-38)
nvk: /dev/nvshim: physical device create failed: VkResult -9
[NVK] enumeratePhysicalDevices: count=0 VK_SUCCESS (0)
[NVK] wall: nvk enumerated 0 physical devices (see the nvk:/[NVSHIM] rows above)
[NVK] done rc=1
```

Two runs (07:41 and 07:48) produced the same sequence; `nvk/serial.log` is the second.

**Reading of the wall.** nvk's `nouveau_ws_device_new()` does not merely identify the
card: it creates a *temporary context* to learn the engine classes
(`nouveau_ws_context_create`), because `nvk_physical_device` needs
`cls_eng3d/cls_copy/cls_m2mf`. Every engine class in nouveau is an RM object
instantiated with `NVIF NEW` on the channel. `/dev/nvshim` reports 6 classes via
`SCLASS` and refuses all of their instantiation with `need=gsp_rm,
missing=rm_chan_object` — deliberately, and documented in `src/include/nv_nvif_uapi.h`
("WHY SCLASS REPORTS CLASSES THIS NODE THEN REFUSES").

So the missing piece is not a number, a struct or an errno mapping. It is
**`NVIF NEW` of an engine class (e.g. `AMPERE_DMA_COPY_B` 0xC7B5), which is a GSP-RM
channel object**. That is rung N3 (GSP-RM boots), not a userspace defect — nvk cannot
be pushed past this point without either GSP-RM or an ICD that lies about having an
engine, which this lane refuses to do.

---

## 5. Every ABI gap found, in the order nvk hits it

| # | gap | state after this lane |
|---|---|---|
| G1 | **no render node**: nvk/libdrm discover devices through `/sys/class/drm` + `/dev/dri`; HobbyOS serves `/dev/nvshim` | closed in the driver (node hook, §2.1). Kernel alternative: expose `/dev/dri/renderD128` + a minimal sysfs |
| G2 | **zlib**: ICD carried `DT_NEEDED libz.so.1`; compat class stages none | closed (static PIC zlib + `shader-cache=disabled`) |
| G3 | **`GRAPH_UNITS` (param 13)** was `-ENOSYS` in WP-F2 and was predicted as nvk's first stop | **no longer a gap**: the shim now answers `0x400b` (`gpc=11 tpc=64 src=declared`) and nvk continues (MEASURED) |
| G4 | **`VRAM_BAR_SIZE` (param 18)** answers `0x0` | nvk accepts it (`bar_size_B = 0`); only a VRAM-bar/WSI consumer would care. Observation, not a blocker |
| G5 | **engine class objects**: `NVIF NEW` of every class `SCLASS` advertises returns `-ENOSYS` (`need=gsp_rm missing=rm_chan_object`) | **the wall**. Needs GSP-RM (N3). Not fixable above the kernel without faking an engine |
| G6 | **next walls, not reached**: nvk's memory path wants `GEM_NEW` with `CPU\|MAPPABLE` + `mmap` (served since WP-F3, domain `0x19` accepted — MEASURED in the unit rows), then PRIME/dma-buf for WSI and timeline syncobjs (both still refused) | INFERRED from `nvk_device_memory.c` + `src/nouveau/winsys`; they move behind G5 |

Also worth recording (not an ABI gap): an earlier boot in this lane used a kernel whose
objects came from a mixed-mode build (a concurrent lane had left `MODE=unit_tests`
objects in `obj/intel`), so it ran the kernel unit suite and reported
`Tests run: 590 / Tests failed: 1` — `EXPECT_EQ FAILED` at
`src/kernel/nv_shim_drm_test.c:1812`, a line that does not exist in the current file
(1,715 lines). That failure belongs to a stale object, not to this lane; the final run
uses a kernel rebuilt from a clean object set in `MODE=test` (which does not run the
unit suite) and grades only rows attributable to `PROBE.ELF`.

---

## 6. What is NOT verified

* **No Vulkan loader.** The probe speaks the ICD ABI directly (interface version 5).
  A real loader adds ICD discovery (`VK_DRIVER_FILES`/`ICD JSON`), dispatch-table
  trampolines and layer enumeration — none of which changes the nvk path below it, but
  none of which is measured here.
* **No enumeration**, therefore no `vulkaninfo` output. By construction: enumeration is
  what the named refusal prevents.
* **Headless WSI only** (`-Dplatforms=`). No X11/XCB surface, no swapchain, no PRIME.
* **Mesa 25.0.7, not the tree's 26.2.6 pin** (§1.1). The ICD ABI is identical; the
  driver source is one minor series older.
* **Determinism**: two boots, same rows. Not a soak; the boot harness has no
  cross-run scheduling control.
* **Not run on the 4090 / real RM.** `mock=1` in every row; nothing here says anything
  about AD102 hardware beyond what the mock declares.

---

## 7. Reproduce

```sh
# 1. build the ICD + probe (needs clang, meson, cmake, rustc, bindgen, cbindgen, patch)
bash tools/mesa/build-nvk.sh all        # cold ~15 min; warm ~10 s; ends with "audit: PASS"
#    (tools/nv/nvk/build.sh is the same command through a thin driver)

# 2. boot the guest and run the probe against /dev/nvshim
NVK_SKIP_KERNEL_BUILD=1 bash tools/nv/nvk/run_guest.sh
#    exit 0 = enumerated; exit 1 = the probe completed without enumerating
#    (this lane's measured state: exit 1, wall = copy engine class 0xc7b5, need=gsp_rm)
#    SKIP_KVM=1 forces TCG on a host whose /dev/kvm is not usable by this user.
```

The whole guest run is ~20 s of boot plus the probe; `run_guest.sh` prints the
`[NVK]`/`nvk:`/`[NVSHIM]` rows and grades `[NVK] dev[0]`.

---

## 8. Next step, in order

1. **Nothing in userspace can move G5.** The next real step is rung N3 (GSP-RM); until
   then nvk's own wall is the honest ceiling.
2. Cheap and independent: expose `/dev/dri/renderD128` in the guest devfs and drop the
   node hook from the patch (G1), so the ICD needs no HobbyOS-specific code.
3. Build the pinned Vulkan-Loader for the compat class and replace the direct-ICD probe
   with `vulkaninfo --summary`; the same wall will appear, one layer up, and the
   deliverable becomes a stock `vulkaninfo` transcript.
4. Keep `GRAPH_UNITS`/`VRAM_BAR_SIZE` honest: `src=declared` is fine for GRAPH_UNITS
   (AD102 GPC/TPC counts are chipset constants), but `VRAM_BAR_SIZE=0` will be read as
   "no BAR" by anything that maps through BAR1 — worth naming in the shim the way
   `src=declared` is.
