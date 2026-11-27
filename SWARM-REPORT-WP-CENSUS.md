# SWARM-REPORT-WP-CENSUS — the shim's client surface is whole: 0 absent, 25/25 widths match (t_d20b014f)

Lane: `nv-card`. Card: `t_d20b014f`. Base `nv/integ-swarm`: `e300fadf` (the
WP-EVENTFD fold). Branch: `nv/wp-census`. **Doc-only**: one new doc, no code
change, no tier rerun, no GPU boot, no VM boot, no `docs/nv-plan.md` edit. Mesa
25.0.7 is read in place at `/home/sarah/cp/vk-build/src/mesa-25.0.7` and never
committed.

Deliverable: `docs/nv-shim-census.md` (this report is its summary).

## 1. The three numbers the card asked for

| claim | measured |
|---|---|
| **ABSENT** ioctls (in neither `g_drm_rows[]` nor `g_drm_refusals[]`) | **0 of 30** |
| served rows whose pinned width disagrees with the struct the client sends | **0 of 25** (25 match) |
| first refusal on a real client path | **`NVIF_IOCTL_V0_NEW` engine class `0xC7B5`**, `need=gsp_rm` (`src/kernel/nv_shim.c:5235`) |

The surface was derived, not copied: `driver_ioctls[]` in Mesa's own DRM shim
(`src/nouveau/drm-shim/nouveau_noop.c:396-411`, **15** nouveau commands) ∪ every
`drmCommandWrite*` / `drmSyncobj*` / `drmPrime*` / `drmCloseBufferHandle` /
`drmGetVersion` / `drmGetCap` site in `src/nouveau/{winsys,vulkan}` and
`src/vulkan/runtime/vk_drm_syncobj.c` (**15** core DRM numbers) = **30**. Of
those: 24 served, 6 refused by name, 0 absent. The six refused are all
gallium/legacy rows — **none is on nvk's path** (`GROBJ_ALLOC`,
`NOTIFIEROBJ_ALLOC`, `GPUOBJ_FREE`, `GEM_CPU_FINI` have no caller at all;
`GEM_PUSHBUF`/`GEM_CPU_PREP` only gallium + uncalled winsys helpers).

## 2. The first refusal, named

`nouveau_ws_device_new()` builds a temporary context with `engines = ~0`
(`src/nouveau/winsys/nouveau_device.c:359`). Inside it, after CHANNEL_ALLOC
(`nouveau_context.c:156`) and NVIF SCLASS (`:55`), the **copy** branch asks for
the highest class whose low byte is `0xb5` (`:167`) → **`0xC7B5`
(`AMPERE_DMA_COPY_B`)** → `drmCommandWrite(DRM_NOUVEAU_NVIF)` at
`nouveau_context.c:111`. The shim answers `-ENOSYS` with the row at
`src/kernel/nv_shim.c:5230-5240`:

```
nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm \
  missing=rm_chan_object alloc=ce_alloc engine=copy1
```

and the client prints `nvk: copy engine class object 0xc7b5 refused: Function
not implemented (-38)`, then `physical device create failed: VkResult -3` and
zero devices. **No earlier non-RM gate exists**: the enumerate GETPARAM, the
`>= 1.3.1` VERSION gate (shim reports 1.3.1), VM_INIT, NVIF NEW/MTHD, the
`EXEC_PUSH_MAX`/`VRAM_BAR_SIZE` GETPARAMs and the hard `GRAPH_UNITS` gate (the
mock's declared 11 GPC / 64 TPC) are all served. That is the `t_9bf9bc0c` wall,
not a shim row.

## 3. Two corrections to the card's premise (both measured)

1. **An absent number is `-ENOTTY`, not `-ENOSYS`.** `nv_shim_drm_ioctl()`
   checks the refusal table first (`nv_shim.c:5493`), then
   `drm_row_size() < 0` → `-ENOTTY` (`:5501-5502`); the tier pins it
   (`src/kernel/nv_shim_drm_test.c:283-285`). Count unaffected; a client would
   see `ENOTTY`, not a named refusal.
2. **Two `drm.h` copies live in this build and they disagree.** Mesa's vendored
   `include/drm-uapi/drm.h:903` has **no** `point` in `struct
   drm_syncobj_handle` (16 B, `0xC01064C1`); libdrm 2.4.131's
   `libdrm/include/libdrm/drm.h:951` has it (24 B, `0xC01864C1`). The client leg
   is libdrm's: the shipped `libvulkan_nouveau.so` contains
   `mov $0xc01864c1,%esi` (objdump `bf2e9d`, `bf306d`) and links
   `/home/sarah/cp/vk-build/libdrm/lib/libdrm.a` statically. Checking the shim
   against Mesa's vendored copy would have produced **two false mismatches** on
   `0xc1`/`0xc2`; checking against the leg that sends gives 25/25. Recorded as a
   served-contract dependency: `0xc1`/`0xc2` are 24 bytes **because** the pinned
   build uses libdrm ≥ 2.4.13x.

## 4. The surface beyond the client path (honesty rows)

- `SYNCOBJ_EVENTFD 0xcf` is served and **no Mesa 25.0.7 caller exists**
  (`grep -rn "SYNCOBJ_EVENTFD\|drmSyncobjEventfd" src/` → 0 hits): §8 of
  `docs/nv-shim-eventfd.md` stands, now measured tree-wide.
- `SYNCOBJ_TRANSFER 0xcc` is served and issued only by generic WSI
  (`src/vulkan/wsi/wsi_common_drm.c:372`, from `wsi_common.c:1190`/`:1226` when
  a swapchain image has `explicit_sync`) — not an nvk queue path, and this node
  has no window system.
- Five refused numbers (`0x0a`, `0x41`, `0x48`, `0x49`, `0x53`) are gallium-only
  or not defined in either header.

Both directions of the §8 gap are therefore closed *as measurements*: no number
a client issues is answered by absence, and no served/refused row is missing a
documented issuer.

## 5. The rung this card recommends next

**None, until R1/R2 in `docs/nv-plan.md` §2.13.** The surface is whole; the
first refusal is the engine-class NVIF NEW (`need=gsp_rm`), i.e. the declared
native ceiling. Every remaining reachable refusal (`gem_cpu_prep`,
`gem_pushbuf`, GETPARAM `PTIMER_TIME`/`VRAM_USED`) is issued **after** physical
device creation, so serving any of them today would change nothing observable —
the census records that ordering so a future "serve PTIMER_TIME for timestamps"
card is not written against a path the client cannot reach.

## 6. What this branch contains

| file | Δ | what |
|---|---|---|
| `docs/nv-shim-census.md` | +477 | the census: 30-row client-surface table, 26-row width table, NVIF surface table, the ordered first-refusal walk, the off-path rows, and the reproduction appendix |
| `SWARM-REPORT-WP-CENSUS.md` | +117 | this report |

No `src/` change, no test change, no plan change. `git diff --stat e300fadf
HEAD` on this branch is exactly those two files.

## 7. Method (so the numbers can be re-derived)

Three small C probes in the worker's scratch dir (not committed):
`width_libdrm` (core rows against libdrm 2.4.131), `width_mesa` (nouveau rows
against Mesa's vendored `drm-uapi/nouveau_drm.h`), `shim_widths` (the shim's own
structs), plus `nvif_len` (NVIF request lengths). The comparison is mechanical:
`awk` extracts `(nr,width)` from `g_drm_rows[]`, the probes' `_IOC` encodings are
reduced to `(nr,size)`, and a `join` reports any row without a matching number or
with a different width — both empty, 25 identical. Command lines are in the
doc's appendix. Every `path:line` in the doc was re-resolved against the file it
names before the commit (24 search roots, 0 unresolved).
