# Cyberpunk track — Vulkan on the CPU (lavapipe), lane C5-vk

P5 groundwork (docs/cyberpunk.md §6 P5, §7.2b). Every number below comes
from a real run on `linuxci` on 2026-11-15; what was not run says so.

## 1. Result

- `bash tools/cp/vk/build.sh all` builds a self-contained `/CP/VK` tree
  (59 MiB stripped) against the pinned glibc 2.31 sysroot. **Max symbol
  version GLIBC_2.29** (lavapipe; loader and probes GLIBC_2.17); the
  DT_NEEDED closure is the glibc set only.
- Inside HobbyOS (x86_64, `MODE=test` wave, KVM) the enumerator reports
  `deviceName = llvmpipe (LLVM 21.1.8, 128 bits)`, Vulkan 1.4.354,
  `driverInfo = Mesa 26.2.6 (LLVM 21.1.8)`, conformance 1.3.1.1. The loader
  found the ICD **with no environment at all** (compiled-in fallback dir).
- `vkcube` rendered a spinning cube offscreen with lavapipe at 640×480 and
  1280×720, read each frame back and blitted it into `/dev/fb0`; the QMP
  screendump is `evidence/cp-vk/vkcube-fb0-qemu64.png`. The 640×480 last
  frame's fnv1a64 is **byte-identical to the host run** (`0x4c1987840da44743`).
- **No kernel change was needed.** LLVM's MCJIT (anonymous RW pages, then
  `mprotect` → R+X) works in compat processes today.

## 2. Build (`tools/cp/vk/build.sh`)

Work dir `~/cp/vk-build` (outside the checkout, 5.6 GiB incl. the LLVM build
tree). Steps, each cached and runnable on its own:

| step | what | notes |
|---|---|---|
| `sysroot` | glibc 2.31 pin set (the five debs of `tools/cp/build-wine.sh`) | absolute symlinks re-pointed into the sysroot |
| `runtimes` | libc++/libc++abi/libunwind 21.1.8, static, PIC, hermetic, abi+unwinder merged into `libc++.a` | from the tree's LLVM tarball (sha256 = `third_party/llvm-21.1.8/container.sha256`) |
| `llvm` | LLVM 21.1.8 static libs, `X86` only, RTTI/EH off, no zlib/zstd/xml/terminfo, tools off except `llvm-config` | `LLVM_PARALLEL_LINK_JOBS=1`, `-j6`; ~20 min on linuxci; 628 MiB installed |
| `loader` | Vulkan-Headers + Vulkan-Loader `vulkan-sdk-1.4.363.0`, all WSI off, prefix `/CP/VK`, `FALLBACK_DATA_DIRS=/CP/VK/share:/usr/local/share:/usr/share` | pkg-config pointed at the sysroot |
| `mesa` | Mesa 26.2.6, **native** meson build with `clang --sysroot`: `-Dvulkan-drivers=swrast -Dgallium-drivers= -Dplatforms= -Dllvm=enabled -Dshared-llvm=disabled -Dcpp_rtti=false`, every optional dep off, `--wrap-mode=nodownload` | 60 s at `-j6` |
| `probes` | `src/user/cp/vk{enum,cube}.c`, shaders via `glslangValidator` | plain glibc PIEs, RUNPATH `/CP/VK/lib` |
| `stage` | strip, ICD JSON check, sha256 manifest | |
| `audit` | GLIBC max ≤ 2.31; DT_NEEDED ⊆ glibc set + libvulkan; no exported C++ runtime symbols; lavapipe exports ⊆ ICD interface + Mesa PUBLIC; every lavapipe undefined symbol provided by the pinned runtime | `$WORK/audit.txt` |

Why not `tools/mesa/build-hobbyos.sh`'s compile-line replay: that targets
`x86_64-none-elf` with HobbyOS headers, so configure probes cannot run. Here
the target *is* x86_64-linux-gnu: binaries linked against the 2.31 sysroot run
on the newer-glibc build host, so meson/cmake probes, tablegen and
`llvm-config` work unmodified. `clang --sysroot` keeps every `-L` inside the
sysroot (gcc does not — build-wine.sh header).

Traps found (each measured, each fixed in the script):

1. **libc++ bootstrap**: CMake's compiler sanity link asks for `-lstdc++`,
   which the sysroot lacks → the runtimes step links with `-nostdlib++`.
2. **libgcc_s**: clang++ adds a hard (not `--as-needed`) `-lgcc_s` → a
   `libgcc_s.so.1` DT_NEEDED for nothing → `-static-libgcc -Wl,--as-needed`.
3. **libunwind's HERMETIC option still exports `_Unwind_*`** → the C++
   runtime archives are named in `--exclude-libs`.
4. **`--exclude-libs,ALL` is wrong for Mesa**: lavapipe is linked from Mesa's
   own static archives, so ALL also hid `vk_icdGetInstanceProcAddr`; with
   `--gc-sections` the DSO shrank to 8.8 MB with zero exports.
5. **Mesa's lavapipe target has no version script**: without excluding LLVM's
   archives it exported 29,787 symbols (all of LLVM). The script names
   `libc++.a:libc++abi.a:libunwind.a:libdrm.a:libLLVM*.a` explicitly; result:
   7 exports (`vk_icd*` ×3 + Mesa's own PUBLIC `ddebug_/noop_/trace_screen_create`,
   `util_run_tests`, the same set upstream exports).
6. **libdrm is mandatory**: on Linux every Mesa Vulkan driver requires it
   (`with_dri2 = with_any_vk and dri platform 'drm'`). Mesa's tarball pins it
   in `subprojects/libdrm.wrap` (2.4.133, sha256 `fc68f9d0…77b4f5`); the script
   fetches exactly that file from dri.freedesktop.org, checks the wrap's
   sha256, and builds it as a **static** subproject (hidden), so there is no
   `libdrm.so.2` DT_NEEDED and HobbyOS needs no DRM.
7. The Vulkan loader REQUIREs pkg-config on Linux (pointed at the sysroot).
8. Mesa's swrast BVH shaders need `glslangValidator` ≥ 12.2 at build time:
   apt `glslang-tools` 16.2.0 installed on linuxci (the only host package added).
9. One transient `glslangValidator` failure on an unchanged shader; a re-run
   built clean (linuxci's CIFS-backed disk; see §4 on the later read-only FS).
10. `__morestack` is a weak undefined in lavapipe (resolves to 0); the audit
    allows it by name.

Audit (raw tail, `$WORK/audit.txt`):

```
/CP/VK/bin/vkcube                                GLIBC_max=2.17   NEEDED: libdl.so.2 libm.so.6 libc.so.6
/CP/VK/bin/vkenum                                GLIBC_max=2.17   NEEDED: libdl.so.2 libc.so.6
/CP/VK/lib/libvulkan.so.1.4.363                  GLIBC_max=2.17   NEEDED: libdl.so.2 libm.so.6 libpthread.so.0 libc.so.6
/CP/VK/lib/libvulkan_lvp.so                      GLIBC_max=2.29   NEEDED: libpthread.so.0 libdl.so.2 libm.so.6 libc.so.6 ld-linux-x86-64.so.2
# GLIBC max over the tree: GLIBC_2.29 (allowed <= 2.31)
# lavapipe exports (7): ddebug_screen_create noop_screen_create trace_screen_create util_run_tests vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion
# lavapipe undefined symbols: 305; not provided by the 2.31 runtime: none
cp-vk: audit: PASS
```

ICD JSON: `{"ICD":{"api_version":"1.4.354","library_arch":"64","library_path":"/CP/VK/lib/libvulkan_lvp.so"},"file_format_version":"1.0.1"}`.
Sizes: `libvulkan_lvp.so` 60,661,824 B stripped (142,961,352 B with debug info),
`libvulkan.so.1.4.363` 612,128 B.

## 3. Kernel: JIT memory in compat processes

Question: does LLVM's JIT get anonymous executable memory and RW→RX
`mprotect` in a compat process? **Yes, on the unchanged base kernel.**

- `bash tools/gate/t3_linux_probe.sh glibc` (lane checkout = base, no kernel
  edits): `[LP] mprotect RW -> R|X ok`, `[LP] call JIT page returns 42 ok
  detail=42`, `[LP] done rc=0`, PASS.
- lavapipe itself (MCJIT, `GALLIVM_USE_ORCJIT=0`) compiled and ran its
  shaders in-guest: the cube frames are JIT output.

No kernel source was modified by this lane, so no parity runs were needed.

## 4. In-OS probes and gates (`tools/cp/vk/gate.sh enum|cube`)

The gate builds the `MODE=test` kernel + disk and stages onto the FAT
`disk.img`: glibc 2.31 into `/lib64` + `/lib/x86_64-linux-gnu` (as the M1
gate), the tree into `/CP/VK/{lib,bin,share/vulkan/icd.d}` (FAT has no
symlinks: `libvulkan.so.1` is a copy of the versioned file), and the probe at
the root named in `WAVE.CFG`. It boots under `flock /tmp/cp-qemu.lock` with a
QMP socket and a short deadline, screendumps while `vkcube` holds its last
frame, and grades the serial log. Default QEMU CPU (no `-cpu`: qemu64 under
KVM → no AVX → LLVM picks 128-bit SSE), `-smp 8`, 12 GiB.

### enum — PASS (boot 22 s)

```
[VK] loader libvulkan.so.1 loaded in 629.0 ms
[VK] Vulkan Instance Version: 1.4.363
[VK] vkCreateInstance ok in 717.0 ms
[VK] vkEnumeratePhysicalDevices: 1 device(s) in 779.0 ms
[VK] GPU0:
[VK]     apiVersion         = 1.4.354
[VK]     driverVersion      = 26.2.6
[VK]     vendorID           = 0x10005
[VK]     deviceType         = PHYSICAL_DEVICE_TYPE_CPU
[VK]     deviceName         = llvmpipe (LLVM 21.1.8, 128 bits)
[VK]     driverName         = llvmpipe
[VK]     driverInfo         = Mesa 26.2.6 (LLVM 21.1.8)
[VK]     conformanceVersion = 1.3.1.1
[VK]     memoryHeap[0] size = 5376 MiB flags = 0x1
[VK]     queueFamily[0] count = 1 flags = 0xf timestampValidBits = 64
[VK]     Device Extensions: count = 178
[VK]     1.2: timelineSemaphore=1 bufferDeviceAddress=1 descriptorIndexing=1 ... shaderFloat16=0 vulkanMemoryModel=1
[VK]     1.3: dynamicRendering=1 synchronization2=1 maintenance4=1 shaderDemoteToHelperInvocation=1 robustImageAccess=1
[VK] done rc=0
cp-vk-gate: PASS (enum)
```

The heap is 5376 MiB = the x64 frame pool (5.25 GiB, cyberpunk.md §0.2 item
6), not `-m`. Host (AVX2) for comparison: `llvmpipe (LLVM 21.1.8, 256 bits)`,
179 device extensions, `shaderFloat16=1` — the guest differences follow from
qemu64's missing AVX/F16C. Extension rows vkd3d-proton/DXVK care about,
in-guest: **yes** for push_descriptor, robustness2, demote_to_helper,
dynamic_rendering, extended_dynamic_state 1/2/3, EXT_mutable_descriptor_type,
descriptor_buffer, mesh_shader, transform_feedback, vertex_attribute_divisor,
maintenance5/6, memory_budget, external_memory_host/fd, conditional_rendering,
depth_clip_enable, custom_border_color, shader_float16_int8,
shader_stencil_export, image_view_min_lod, graphics_pipeline_library,
shader_object, ray_tracing_pipeline; **no** for KHR_swapchain (no WSI built),
VALVE_mutable_descriptor_type, KHR_fragment_shading_rate. (The probe's
`VK_EXT_headless_surface` row checks the *device* list, where it can never
appear — it is an instance extension; the instance list has no surface
extensions at all in this no-WSI build.)

### cube — PASS (boot 87 s), screendump `evidence/cp-vk/vkcube-fb0-qemu64.png`

1280×720 frame at (0,0), 640×480 frame at (1280,0), grey backdrop elsewhere.

```
[VK] device: llvmpipe (LLVM 21.1.8, 128 bits) (api 1.4.354) instance+enumerate 11150.0 ms
[VK] vkCreateDevice ok in 25779.0 ms (queue family 0)
[VK] fb: /dev/fb0 1920x1080 bpp=32 stride=7680 smem=8294400 (R@16 G@8 B@0)
[VK] size 640x480: setup 1305.0 ms (pipeline create 1152.0 ms)
[VK] size 640x480 frame 0: record 148.00 ms, render+readback 11963.00 ms, blit+present 5.00 ms, total 12116.00 ms
[VK] size 640x480 frame 1: record 14.00 ms, render+readback 242.00 ms, blit+present 5.00 ms, total 261.00 ms
[VK] size 640x480 RESULT frames=90 wall=13508.0 ms fps=6.66 | per-frame total ms min/median/p90/max 4.00/13.00/19.00/12116.00 | render+readback median 11.00 | blit+present median 2.00 | record mean 1.833
[VK] size 640x480 last frame fnv1a64=0x4c1987840da44743 covered=62854/307200 px (20.5%) placed at (1280,0)
[VK] size 1280x720: setup 22.0 ms (pipeline create 1.0 ms)
[VK] size 1280x720 RESULT frames=90 wall=4902.0 ms fps=18.36 | per-frame total ms min/median/p90/max 5.00/14.00/17.00/3594.00 | render+readback median 11.00 | blit+present median 2.00 | record mean 0.022
[VK] size 1280x720 last frame fnv1a64=0xd8883985a3fff39a covered=141412/921600 px (15.3%) placed at (0,0)
[VK] teardown: clean
[VK] done rc=0
cp-vk-gate: PASS (cube)
```

Per-frame time (guest, qemu64 CPU, smp 8):

| size | median total | min | steady mean (frames 2–89) | render+readback median | blit+present median |
|---|---|---|---|---|---|
| 640×480 | 13 ms | 4 ms | ≈ 12.9 ms | 11 ms | 2 ms |
| 1280×720 | 14 ms | 5 ms | ≈ 13.5 ms (one 3,594 ms stall excluded) | 11 ms | 2 ms |

Host (linuxci, AVX2, same binaries under the pinned 2.31 ld.so, no fb):
640×480 median 0.85 ms, 1280×720 median 1.30 ms per frame.

Reading these honestly:

- **The guest clock is 1 ms granular** (every value is an integer ms), so the
  per-frame columns are coarse.
- Both sizes share an **11 ms render+readback median** while the minimum is
  3–5 ms: the steady cost is dominated by something size-independent — most
  likely wake-up latency of the fence wait / llvmpipe's worker threads (5
  compat threads via `clone(0x3D0F00)`), i.e. scheduler granularity, not
  rasterisation. Not measured further.
- **Start-up is very slow**: instance+enumerate 11.2 s and `vkCreateDevice`
  25.8 s in this boot (the enum boot did instance+enumerate in 0.78 s), and
  the first frame 12 s (shader JIT). The 14× swing between two boots of the
  same binary points at host contention (linuxci was running other lanes'
  builds and boots at the time) and/or first-touch cost of the 60 MiB DSO on
  FAT; not isolated.
- The 640×480 hash is byte-identical to the host's; the 1280×720 hash differs
  (host `0xfded9b56a4459c16`) with identical coverage (141,412 px) — the runs
  used different SIMD widths (128 vs 256 bits); cause not verified.

**Not run:** the planned `-cpu host` repeat (256-bit path in-guest). Its gate
started, but linuxci's root filesystem went **emergency read-only**
(`/dev/sda1 … errors=remount-ro,emergency_ro`; journald "Read-only file
system") while it waited for the QEMU lock, and the gate died before booting.

## 5. Staging proposal for F4: `/CP/VK`

```
/CP/VK/lib/libvulkan.so.1                         loader (real file; FAT has no symlinks)
/CP/VK/lib/libvulkan_lvp.so                       lavapipe (LLVM + libc++ + libdrm inside)
/CP/VK/share/vulkan/icd.d/lvp_icd.x86_64.json     library_path=/CP/VK/lib/libvulkan_lvp.so
/CP/VK/bin/{vkenum,vkcube}                        probes
```

Dependencies outside the tree: only the glibc 2.31 runtime (`libc`, `libm`,
`libpthread`, `libdl`, `ld-linux`); **no libgcc_s, libstdc++, libdrm, zlib,
expat, X11**. Self-contained: the loader's compiled-in fallback finds the ICD
under `/CP/VK/share` with no env var.

## 6. What winevulkan (P5) and Venus (P6) need from this

- winevulkan `dlopen`s **`libvulkan.so.1`** by soname: Wine's processes must
  have `/CP/VK/lib` on their search path (`LD_LIBRARY_PATH=/CP/VK/lib` in the
  launcher, or the loader staged beside Wine's libs). The probes use RUNPATH
  for the same effect.
- ICD discovery: nothing to set for lavapipe (fallback dir). To select a
  driver explicitly use **`VK_DRIVER_FILES`** or the legacy
  **`VK_ICD_FILENAMES`** — both override the fallback. Venus later becomes a
  second JSON in the same `icd.d` (`virtio_icd.x86_64.json`);
  `VK_DRIVER_FILES` picks one.
- The loader is built **without any WSI**; winex11's Vulkan path needs
  `VK_KHR_surface` + `VK_KHR_xlib_surface`/`xcb_surface` from the loader *and*
  the ICD, so the X11 rebuild must switch on `BUILD_WSI_XLIB_SUPPORT` /
  `BUILD_WSI_XCB_SUPPORT` and Mesa `-Dplatforms=x11` against C4-x's libs.
- Mesa needs libdrm even for lavapipe (trap 6); the Venus build
  (`-Dvulkan-drivers=swrast,virtio`) can reuse the static-libdrm recipe.
- `-Dshader-cache=disabled` here; vkd3d-proton/DXVK runs will want Mesa's disk
  cache (needs zlib or zstd in the sysroot) — decide in P5 proper.

## 7. Not done

- **X11 WSI** (P5 item 3): C4-x has landed real libX11/libxcb on `cp-x`
  (`ca82e870`), but Mesa's X11 WSI also needs xcb-dri3/present/sync/shm/
  xfixes/randr, x11-xcb and xshmfence, and XHOBBY.BIN must answer them; not
  attempted in this lane.
- The `-cpu host` (AVX2) in-guest timing (linuxci FS went read-only).
- Explaining the start-up time and the size-independent ~11 ms/frame floor.
- `vkcube` runs 90 frames per size (≈ 18 s after start-up), not P5's ≥ 60 s soak.
