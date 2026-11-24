# SWARM-REPORT — WP-UNITS: `GETPARAM GRAPH_UNITS` answered, and the engine-class wall named

Branch `nv/wp-units`, cut from `eaf006a9` (`nv/integ-swarm`, round 5 merged).
Rung N2 → N5: the row round 5's own §9 named as "the next cheap, honest thing to
serve" and the row WP-NVIF §7.1 left open ("it needs a source for the counts —
the card, or a *declared* model of it — not a guess").

**Status: gate met, MEASURED — and both answers say where they come from.** A
stock winsys's device probe now gets past `GETPARAM GRAPH_UNITS` (the call Mesa
treats as fatal, `goto out_err`) and stops — by name — at the first engine class
object, which is copy, not 3D. Nothing is claimed as measured that was not: the
topology is a DECLARED part model and every row that carries it says
`src=declared`; the engine refusal now names the missing prerequisite
(`missing=rm_chan_object`) and the RM allocation that would have created the
object, instead of the blanket `need=gsp_rm`.

Claims are tagged MEASURED (a log line from this branch), SOURCE (read from the
cited upstream file) or INFERRED. Evidence:
`evidence/nv/el0-units-47of47.log` (the EL0 run, `[EL0] RESULT PASS 47/47`),
`evidence/nv/unit-tier-590-units.log` (the unit tier, trimmed to the new rows),
`evidence/nv/units-mutation-m1-rop-bits.log` and
`evidence/nv/units-mutation-m2-bulk-missing-reason.log` (two red runs).

---

## 1. What changed

| File | What |
|---|---|
| `src/include/nv_rm.h` | `struct nv_rm_gpu_topo` (`gpc_nr`, `tpc_total`, `rop_nr`, `source`) + `NV_RM_TOPO_NONE`/`DECLARED`; `nv_rm_device_info` gains `topo` and **`bar1_fb_size`**; the seam doc states where the numbers come from on Turing+ and why a declared model is the only honest alternative. Plus the mock-only `nv_rm_mock_set_topo()` seam |
| `src/kernel/nv_rm_mock.c` | the declared RTX 4090 / AD102-300 model (11 GPC / 64 TPC / 176 ROP) with its provenance and its falsification path; `bar1_fb_size = 0` (no aperture); `nv_rm_mock_set_topo()` |
| `src/kernel/nv_shim.c` | `nv_shim_gr_units_value()` (the Ada packing, and *only* the packing); `GRAPH_UNITS` served from the RM's topology or refused with `reason=mock_has_no_topo`; `VRAM_BAR_SIZE` answers `bar1_fb_size` instead of the VRAM amount; the `drm getparam` row for param 13 carries `gpc=/tpc=/src=`; the engine-class table gains `engine` + `alloc`, the refusal row prints them, and `nv_shim_nvif_engine_class()` exposes the same table to the tier; a dedicated `r_param` row budget |
| `src/kernel/nv_shim_drm_test.c` | one new suite (`test_nvshim_getparam_graph_units`): packing + field edges, both faces agreeing, the no-model refusal, `VRAM_BAR_SIZE != FB_SIZE`, and the engine-class table |
| `src/user/nvabi_probe.c` | row 30 rewritten to walk the client's real tail, rows 31-33 added (units decomposition, the five engine objects in nvk's order, the BAR decision); rows 34-47 renumbered; `find_cls()` mirrors Mesa's highest-low-byte rule |
| `tools/nv/el0_wave.sh` | `SKIP_KVM=1` / `EXTRA_QEMU_ARGS` honoured (this box has `/dev/kvm` but its user is not in group `kvm`, so the hard-coded `-enable-kvm` made the gate unrunnable here) |
| `src/include/nv_drm_uapi.h` | the device-create param path documented: which params are fatal, which are not, and what each number actually means |

No driver code was copied. Every shape and number is quoted from a cited file:
`nouveau_abi16.c`, `nvkm/engine/gr/base.c`, `nvkm/subdev/gsp/rm/{gr.c,engine.c}`,
`nvkm/subdev/gsp/rm/r535/{gr.c,gsp.c,ce.c}`, `nvkm/engine/gr/{ga102.c,gf100.c}`,
Mesa `src/nouveau/winsys/nouveau_{device,context}.c`,
`src/nouveau/vulkan/nvk_{device,physical_device}.c`, and NVIDIA's Ada whitepaper.

---

## 2. What `GETPARAM GRAPH_UNITS` actually asks (SOURCE)

The whole chain, on this card, is three calls long:

```
Mesa        nouveau_device.c: `nouveau_ws_param(fd, NOUVEAU_GETPARAM_GRAPH_UNITS, &value)`
            if it fails -> `goto out_err`            (fatal, unlike its neighbours)
DRM         nouveau_abi16.c:~308:
              case NOUVEAU_GETPARAM_GRAPH_UNITS:
                  getparam->value = gr ? nvkm_gr_units(gr) : 0;
nvkm        engine/gr/base.c: nvkm_gr_units() -> gr->func->units(gr)
Ada/GSP-RM  subdev/gsp/rm/r535/gr.c:
              r535_gr_units(): return (gsp->gr.tpcs << 8) | gsp->gr.gpcs;
```

and `gsp->gr.gpcs` / `gsp->gr.tpcs` are filled in exactly one place —
`subdev/gsp/rm/r535/gsp.c`, `r535_gsp_get_static_info()`, from the firmware's
reply to `NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO`:

```c
for (int gpc = 0; gpc < ARRAY_SIZE(rpc->tpcInfo); gpc++)
        if (rpc->gpcInfo.gpcMask & BIT(gpc)) {
                gsp->gr.tpcs += hweight32(rpc->tpcInfo[gpc].tpcMask);
                gsp->gr.gpcs++;
        }
```

Three consequences, each load-bearing for this lane:

1. **The packing is `(tpc_total << 8) | gpc_nr`, in 24 bits, with no ROP field** —
   `tpc_total` is the total over GPCs, and Mesa decodes it as
   `gpc = value & 0xff`, `tpc = (value >> 8) & 0xffff`. The pre-Turing
   `gf100_gr_units()` adds `rop_nr << 32`; on Ada those bits are never set, so a
   node that invents a ROP count in them is answering a different question.
2. **On this hardware the numbers are a GSP-RM message, not a fuse.** The
   host-side register path exists in `gf100_gr_oneinit()` (`0x409604 & 0x1f` for
   the GPC count, `GPC_UNIT(i, 0x2608)` per GPC for the TPC mask) — but
   `ga102_gr_new()` opens with `if (nvkm_gsp_rm(device->gsp)) return -ENODEV;`,
   so on Ada with GSP-RM that GR engine is never created and those reads never
   happen. Before `INIT_DONE` there is no reply, therefore no numbers.
3. **0 is not "zero units", it is "nothing to report".** `gr ? … : 0` means a
   nouveau without a GR engine answers 0 and Mesa accepts it. That is the
   spelling this node *could* have used — and rejected, because 0 read as a
   measurement is a lie of omission: nvk derives `shaderSMCount = tpc_count *
   mp_per_tpc` (a reported property) and sizes its shared-local-memory area as
   `bytes_per_tpc * tpc_count` (nvk_device.c), so a zero silently becomes a
   zero-sized SLM area and a device that claims no shader cores.

---

## 3. What this node answers, and why that is honest (MEASURED)

`drm_param_value()` now serves param 13 from the RM's topology block:

```
[NVSHIM] drm getparam param=13 value=0x400b gpc=11 tpc=64 src=declared
```

`0x400b` is `(64 << 8) | 11`. The values are the **declared configuration of the
part this mock stands for** — AD102-300, the RTX 4090 in PLAN.md §0/§1, which
NVIDIA's Ada whitepaper gives as **11 GPC / 64 TPC / 128 SM** (full die: 12 / 72
/ 144; ROPs 176 either way). Through Mesa's own arithmetic that is
`shaderSMCount = 64 * 2 = 128` SMs, i.e. an RTX 4090, not a hedge.

Why this is honest and not an invented number:

- **It is a declaration with a provenance and a way to be wrong.** The values are
  a public part specification, they are tagged `NV_RM_TOPO_DECLARED`, the row
  says `src=declared` on every answer, and the header says what will contradict
  them: when GSP-RM boots, `GET_GSP_STATIC_INFO`'s masks must produce 11/64 or
  this table is wrong and gets fixed. A measured value would be indistinguishable
  in the ABI; a declared one is auditable.
- **It cannot be confused with a measurement.** `src=` is in the same row as the
  value. There is no path in the code where a `NONE` topology produces a number:
  `nv_shim_gr_units_value()` returns 0 for `NV_RM_TOPO_NONE` and the param table
  refuses before it gets there.
- **The refusal branch still exists and is proved.** `nv_rm_mock_set_topo(NONE)`
  makes the next device answer `-ENOSYS` with `reason=mock_has_no_topo` — a
  *different* name from the generic `mock_has_no_card`, because "the RM never
  received a topology" and "there is no card" are different absences. The tier
  opens a device in each state and asserts both (MEASURED, both rows in the
  unit-tier log).
- **The declared numbers are rejected at the seam if they cannot be packed.**
  `nv_rm_mock_set_topo()` refuses `gpc_nr == 0`, `gpc_nr > 0xff`,
  `tpc_total == 0`, `tpc_total > 0xffff` with `-EINVAL` rather than truncating —
  a model that cannot be expressed in nouveau's field widths is a bug, not a
  rounding.

What it does **not** claim: that this card has 11 GPCs. Nothing on this node has
ever received `GET_GSP_STATIC_INFO`; §8 names the one unrun experiment that could
turn the declaration into a measurement.

---

## 4. The other getparams on that path, audited (SOURCE + MEASURED)

`nouveau_ws_device_new()` asks four params after it has a version and a device
object, and only one of them can stop it:

| param | asked for | nouveau's answer | this node | fatal to nvk? |
|---|---|---|---|---|
| `PCI_DEVICE` | 4 | `pdev->device` | `0x2684` | **yes** — served since round 5 |
| `EXEC_PUSH_MAX` | 17 | derived from the channel's push limit (`nouveau_exec_push_max_from_ib_max`) | `64` = `NVSHIM_DRM_MAX_PUSH`, i.e. what this node's EXEC actually accepts | no — falls back to `NOUVEAU_GEM_MAX_PUSH` |
| `VRAM_BAR_SIZE` | 18 | `resource_size(NVKM_BAR1_FB)` — an **aperture** | **`0`** (was `fb_size`, corrected here) | no — `bar_size_B` stays 0 |
| `GRAPH_UNITS` | 13 | `(gsp->gr.tpcs << 8) \| gsp->gr.gpcs` | `0x400b`, `src=declared` | **yes** — this lane |

Two corrections fall out of that table:

- **`VRAM_BAR_SIZE` was answering the wrong quantity.** It returned the 4 GiB
  VRAM pool, i.e. `bar_size == vram_size` — which is precisely the predicate nvk
  uses to switch to its full-BAR CPU-upload path (`nvk_device.c`:
  `if (pdev->info.bar_size_B >= pdev->info.vram_size_B) shader_map_flags =
  NVKMD_MEM_MAP_W`), and to add an extra device-local heap when
  `0 < bar_size < vram` (`nvk_physical_device.c`). This node has no aperture at
  all, so the honest answer is 0; nvk then takes the upload-queue path, which
  needs the copy engine that §5 refuses by name. The old answer was not fatal —
  it was worse: quietly true-looking.
- **`EXEC_PUSH_MAX` is a promise, not a hardware fact**, and it is the one
  served param whose honest value is defined by this node: the largest
  `push_count` `DRM_NOUVEAU_EXEC` accepts here (`EXEC` refuses
  `push_count > NVSHIM_DRM_MAX_PUSH`). Serving 64 keeps the promise; serving
  nouveau's number would not.

---

## 5. The engine-class refusal, named exactly (MEASURED + SOURCE)

Round 5 stopped at `need=gsp_rm`. That names the subsystem but not the missing
*object*, so the next step for a reader was ambiguous. The real path for one
engine class object on Ada is:

```
NVIF NEW <oclass> route 0xff, token = channel
  -> nvkm_rm_gr_obj_ctor / nvkm_rm_engine_obj_ctor   (rm/gr.c, rm/engine.c)
  -> nvkm_rm_engine_obj_new(&chan->rm.object, chan->id, oclass)
       GR / 2D / M2MF:  nvkm_gsp_rm_alloc(chan, handle, class, 0, &obj->rm)
       CE:              rm->api->ce->alloc() -> nvkm_gsp_rm_alloc_get/wr with
                        NVC0B5_ALLOCATION_PARAMETERS{engineType = COPY0 + inst}
```

So every engine object is **one RM allocation parented on the channel's RM
object**. The two missing pieces are therefore: GSP-RM itself, and an RM channel
object for the allocation to hang under (this node's channel is bookkeeping —
`nv_rm_mock.c` has no `chan->rm.object` because there is no RM to allocate it).

The row now says exactly that, and says which engine instance the class is:

```
[NVSHIM] nvif new refused class=0x902d name=fermi_twod_a      need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=eng2d
[NVSHIM] nvif new refused class=0xa140 name=kepler_inline_m2mf need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=m2mf
[NVSHIM] nvif new refused class=0xc797 name=ampere_b_gr       need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=eng3d
[NVSHIM] nvif new refused class=0xc7c0 name=ampere_compute_b   need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=compute
[NVSHIM] nvif new refused class=0xc6b5 name=ampere_dma_copy_a  need=gsp_rm missing=rm_chan_object alloc=ce_alloc engine=copy0
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b  need=gsp_rm missing=rm_chan_object alloc=ce_alloc engine=copy1
```

`nv_shim_nvif_engine_class()` is the same table the row prints, so the tier holds
the *claim* (not the prose) to a value: `0xc797 -> (ampere_b_gr, eng3d,
rm_alloc)`, `0xc7b5 -> (ampere_dma_copy_b, copy1, ce_alloc)`, `0x1234 -> -1`.
`-ENOSYS` is unchanged: a bookkeeping-only object would let nvk build a command
stream nothing reads, which is worse than a refusal (round 5 §7.2, still true).

---

## 6. The EL0 transcript (MEASURED)

`SKIP_KVM=1 tools/nv/el0_wave.sh --out evidence/nv/el0-units-47of47.log` on this
box (MODE=test, `/WAVE.CFG = NVABI.ELF`). Rows 1-29 are unchanged; 30-33 are this
lane's; 34-47 are the round-3/4/5 rows, renumbered.

```
[EL0] 30 nvif_winsys_walk getparam(4)=0 nvif_new=0 info=0 pushmax=0 bar=0 units=0 sclass=0 stop=nvif_new(copy 0xc7b5)=38 PASS
[NVSHIM] drm getparam param=4 value=0x2684
[NVSHIM] nvif new class=0x80 cookie=0x17fffff8c0 dev=1 served=device
[NVSHIM] nvif info chip=AD102 chipset=0x192 fam=0xe ram=0x100000000
[NVSHIM] drm getparam param=17 value=0x40
[NVSHIM] drm getparam param=18 value=0x0
[NVSHIM] drm getparam param=13 value=0x400b gpc=11 tpc=64 src=declared
[NVSHIM] nvif sclass ch=1 count=6 first=0x902d 3d=0xc797
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm missing=rm_chan_object alloc=ce_alloc engine=copy1
[EL0] 31 getparam_graph_units gpc=11 tpc=64 sm=128 rop_bits=0x0 PASS
[EL0] 32 nvif_engine_order copy=0xc7b5/38 2d=0x902d/38 3d=0xc797/38 m2mf=0xa140/38 compute=0xc7c0/38 refused=5/5 PASS
[NVSHIM] nvif new refused class=0x902d name=fermi_twod_a need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=eng2d
[NVSHIM] nvif new refused class=0xc797 name=ampere_b_gr need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=eng3d
[NVSHIM] nvif new refused class=0xa140 name=kepler_inline_m2mf need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=m2mf
[NVSHIM] nvif new refused class=0xc7c0 name=ampere_compute_b need=gsp_rm missing=rm_chan_object alloc=rm_alloc engine=compute
[EL0] 33 getparam_bar_size bar=0x0 vram=0x100000000 full_bar=0 PASS
[NVSHIM] drm getparam param=18 value=0x0
[NVSHIM] drm getparam param=8 value=0x100000000
...
[EL0] 47 fd_lifetime new_fd=3 old_handle=1 errno=22 PASS
[EL0] RESULT PASS 47/47
```

Row 30 is the point of the lane: it runs the tail of `nouveau_ws_device_new()` in
the order that function runs it and then the first step of
`nouveau_ws_3d_context_init(ALL_3D_ENGINES)`. Two things a stock nvk does that
round 5's walk did not model:

- **The first engine object it asks for is COPY, not 3D.** The engine order in
  `nouveau_ws_3d_context_init` is copy → 2D → 3D → M2MF → compute, and the class
  for each is chosen by `nouveau_ws_context_find_class()`, which takes the
  **highest** class whose low byte matches — so of the two copy classes SCLASS
  reports it asks for `0xc7b5` (`ampere_dma_copy_b`, CE instance 1), not
  `0xc6b5`. Round 5's row 30 asked for 3D, which is the class nvk *prints* as
  `cls_eng3d` but not the class it *requests* first.
- **The failure is fatal for the whole context**, so the CB/CE objects never get
  created at all and `nouveau_ws_device_new()` returns NULL.

Rows 31-33 pin the numbers the client derives: `gpc=11 tpc=64 sm=128 rop_bits=0`
(row 31), all five engine objects refused with the ids above (row 32), and
`bar=0x0 vram=0x100000000 full_bar=0` (row 33).

---

## 7. What a real nvk does next, in each case

**A. `GRAPH_UNITS` answers (this node, this lane).** `nouveau_ws_device_new()`
continues to `nouveau_ws_context_create(ALL_3D_ENGINES)`: `CHANNEL_ALLOC`
(accepted — the `{~0, GR}` spelling), `SCLASS` (6 classes), then `NVIF NEW` for
the copy class `0xc7b5`, which is refused `-ENOSYS` with
`missing=rm_chan_object alloc=ce_alloc engine=copy1`. The context create fails,
so the device create fails, and `vkEnumeratePhysicalDevices` returns zero devices
from this node — a named failure rather than a crash, a hang, or a device that
mis-describes itself. Because the row names the missing RM channel object and the
allocation, the next engineering step is unambiguous: make `CHANNEL_ALLOC` create
a real RM channel object (rung N3), after which the same row's `alloc=` is the
call to implement.

**B. An engine class object (unchanged behaviour, now named).** A real stack
would issue an RM allocation on the channel's RM object and program the engine
through the RM methods that come back. Nothing here can stand in for it: the
object *is* the RM allocation. This is GSP-RM (N3) plus a channel object — the
row says both.

**C. `GRAPH_UNITS` with no topology (the retained branch).** `goto out_err`
before NVIF is ever reached: nvk enumerates nothing, exactly as it did against
the pre-lane node. This is what an RM that has no model must answer, and it is
still the honest answer for any node that has not been given one.

**D. The real card, once GSP-RM is up.** `GET_GSP_STATIC_INFO` fills
`gsp->gr.gpcs/tpcs` from the enabled masks and `r535_gr_units()` returns
`0x400b` for an RTX 4090. That is the same word §3 declares, so the declaration
is a testable prediction: if the card reports something else, the table is wrong.

---

## 8. What is NOT verified

- **No real nvk ran.** Nothing in this branch links against Mesa. The call
  order and the decoding are transcribed from `nouveau_device.c`,
  `nouveau_context.c`, `nvk_device.c`, `nvk_physical_device.c` (SOURCE); the EL0
  rows prove this node's answers to that sequence, not that nvk reaches them.
- **The topology is DECLARED, not measured.** 11 GPC / 64 TPC is the RTX 4090's
  published configuration; this node has never read a `gpcMask`. Every row says
  `src=declared`.
- **The one experiment that could make it measured** has not been run: read BAR0
  `0x409604 & 0x1f` and `GPC_UNIT(i, 0x2608)` for the registered GPCs — naive
  nouveau's pre-Turing recipe. It is worth exactly one deploy, and it may come
  out negative: on Ada PGRAPH is owned by GSP-RM, and reads outside the readable
  blocks answer `0xBADF5620` (project rule 1), so "no GPCs" and "unreadable"
  would look alike unless the read is validated against a known-good register in
  the same block.
- **`rop_nr = 176` is carried but never sent**, because the Ada packing has no
  ROP field; it is recorded for the record only.
- **`bar1_fb_size = 0`** is the honest answer for a node with no aperture, but it
  has not been compared against a real `resource_size(NVKM_BAR1_FB)` on this
  card (PLAN.md §1 measures BAR1 as 32 GiB, of which 1 GiB is mapped).
- **SCLASS membership** is still the kernel's engine-table reconstruction from
  round 5 (§3b there); this lane did not change it, it changed the *order* in
  which a client requests the members and the naming of the refusal.
- **ARM is compile-only here.** The gates below are intel; no ARM unit-tier boot
  was run on this branch (the ARM link is another lane's `.bss` issue, unchanged).

---

## 9. Gates (MEASURED on this box)

| Gate | Command | Line |
|---|---|---|
| intel unit tier | `SKIP_KVM=1 TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1800 ./run_unit_tests_intel.sh` | `Tests run: 590` / `Tests failed: 0` / `UNIT TESTS PASSED` |
| EL0 wave, end to end | `SKIP_KVM=1 tools/nv/el0_wave.sh --out evidence/nv/el0-units-47of47.log` | `[EL0] RESULT PASS 47/47` |
| mutation M1 | `nv_shim_gr_units_value()` adds `rop_nr << 32` (the pre-Turing packing) | red: `EXPECT_EQ FAILED: … >> 32) == 0` (Left 176, Right 0), 590 run / 1 failed |
| mutation M2 | `nv_shim_param_refusal_reason()` returns `mock_has_no_card` for every param | red: `EXPECT_EQ FAILED: … "mock_has_no_topo"`, 590 run / 1 failed |

Baseline: round 5 reported 588 / 0 on its own tip. This lane adds one suite and
does not remove any; the tip it was cut from reports 590 / 0 with it applied.

> **Build-tree note.** This box had a second lane building in the same checkout
> (`OBJ_DIR=obj/nvktest`) while these gates ran, and a shared `obj/` produced two
> links that failed on *unrelated* symbols (`sched_wfi_test_op`, `tests_run`), and
> a `make clean` here raced that lane's ICU configure. The gates above were taken
> in an isolated worktree (`HobbyOS-units`, branch `nv/wp-units`) with its own
> `obj/`. `/dev/kvm` exists but this user is not in group `kvm`, so both runs are
> TCG (`SKIP_KVM=1`, added to `el0_wave.sh` for exactly this reason).

---

## 10. Next step

1. **A real RM channel object** — `CHANNEL_ALLOC` is where the engine-class row's
   `missing=rm_chan_object` is cured (rung N3). Everything else on this node's
   NVIF surface is already waiting for it.
2. **The `0x409604` read** (§8): one deploy on the card, and a chance to replace
   the declared model with a measured one before GSP-RM exists.
3. **`GETPARAM PTIMER_TIME`** remains refused (`need=ptimer`, `-ENOSYS`): nvk's
   `get_gpu_timestamp()` falls back to 0, so it does not stop enumeration, but
   any timestamp-based query is still wrong.
