# SWARM-REPORT — WP-NVIF: `/dev/nvshim` serves nouveau's object model

Branch `nv/wp-nvif-shim`, cut from `b3edb2a2` (`nv/integ-swarm`, round 4 merged,
intel **584 / 0**, ARM 468 / 0). Rung **N2 → N5** (`PLAN.md` §4): the row both
lane reports named as the single thing between this node and a winsys that can
enumerate — `DRM_IOCTL_NOUVEAU_NVIF`, nr `0x47`
(`SWARM-REPORT-WP-F2` §5.7, `SWARM-REPORT-WP-EL0` §9: "one table entry away").

**Status: gate met, MEASURED — with the wall named.** A caller can now create
the DEVICE object, read the chip's identity from it, ask a channel for its
engine classes and get the real AD102 list, and delete the object. Every
engine class object — the one call that needs a GPU — is refused `-ENOSYS` with
the class id and `need=gsp_rm` in a console row. Nothing is faked: no object
this node reports as created lacks a real backing, and no class it lists is
claimed to be instantiable.

Claims are tagged MEASURED (seen in a log from this branch), SOURCE (read from a
cited contract) or INFERRED. Evidence:
`evidence/nv/el0-nvif-44of44.log` (the green EL0 run, trimmed to `[EL0]` and
`[NVSHIM]` rows), `evidence/nv/nvif-mutation-m1-wrong-class.log`,
`evidence/nv/nvif-mutation-m2-size-check.log`, and
`evidence/nv/el0-red-nvif-del-buffer.log` (the one red EL0 run a row produced
before a client-side fix).

---

## 1. What changed

| File | What |
|---|---|
| `src/include/nv_nvif_uapi.h` | **new.** The NVIF contract: `nvshim_nvif_hdr` (24 B), `_new` (32 B), `_sclass` (8 B) + entry (8 B), `_mthd` (8 B), `device_v0` (16 B), `device_info_v0` (104 B), `device_time_v0` (16 B), the six type numbers, route/owner constants, the AD102 class ids, and the per-type size rules. Every number sourced in the header comment |
| `src/kernel/nv_shim.c` | the NVIF section (~390 lines): per-client object table keyed by the client's cookie, `NEW`/`MTHD`/`SCLASS`/`DEL`, the class table with names, named refusals, one `[NVSHIM] nvif …` row per operation. Plus: NVIF routed before the fixed-size check (its encoded size is a buffer length), and `CHANNEL_ALLOC` now accepts nouveau's `{~0, GR}` engine-selector spelling and normalises it to 0 in the reply |
| `src/kernel/nv_shim_drm_test.c` | 4 new suites (contract/offsets, device object + methods, SCLASS, engine refusals) ~450 lines; the old "NVIF is refused" assertions replaced with "NVIF is served, minimum size 24" |
| `src/user/nvabi_probe.c` | 12 new rows (20–31, total now 44): device object, INFO, method refusals, cookie rules, SCLASS + truncation, the engine wall, shape refusals, the **winsys walk**, DEL. NVIF sizes asserted on the client side too |
| `src/include/nv_drm_uapi.h` | NVIF moved out of the refusal table into the served rows; `NVSHIM_DRM_IOC_NVIF_LEN(len)` is how a client encodes a variable-length request, and the row's table entry is documented as a MINIMUM |
| `src/include/nv_rm.h`, `src/kernel/nv_rm_mock.c` | `nv_rm_device_info.revision` (PMC_BOOT_0 bits 0–7), filled `0xa1` by the mock — the identity a `NV_DEVICE_V0_INFO` reply carries belongs to the RM, not to the ioctl layer |

No driver code was copied. Struct layouts, type numbers, class ids and the
`argc`/route rules are quoted from `drivers/gpu/drm/nouveau/include/nvif/ioctl.h`,
`…/cl0080.h`, `…/class.h`, `nouveau_abi16.c` (the server that defines this ABI)
and Mesa's `src/nouveau/winsys/nouveau_{device,context}.c` (the client that
calls it).

---

## 2. The ABI, as the server that defines it actually behaves (SOURCE)

Five findings that shaped the implementation, each read from
`nouveau_abi16.c`'s NVIF dispatch:

1. **`argc` is the whole buffer, not a struct width.** `argc = size;
   argc -= sizeof(*ioctl)` and every type then applies its own rule. So NVIF
   is the *only* row on this node that cannot be validated by an equality
   check — the test tier asserts `nv_shim_drm_req_size(NVIF) == 24` is the
   MINIMUM, and the handler applies: `NEW` ≥ 56, `SCLASS` = 32 + count·8
   exactly, `MTHD` = 32 + method payload exactly, `DEL` = 24 exactly.
2. **Objects are keyed by the CLIENT's cookie** (`new.object`, later
   `ioctl->object`), and the reply never replaces it. That is why Mesa passes
   `(uintptr_t)dev`, `new.handle = 0`, and later names the object with a bare
   `object` field and `token = 0`. This node stores exactly that cookie and
   nothing else (no kernel address is stored or returned).
3. **A modern kernel's NVIF is two object types only** — DEVICE and engine
   objects ("the remainder … has been removed", `nouveau_abi16.c`'s own
   comment) — and **one method**, `NV_DEVICE_V0_INFO`. `TIME` and `GCX_READY`
   exist in `cl0080.h` but the ioctl surface answers `-EINVAL` for anything but
   INFO. So "the object tree" is device → per-channel engine objects; there is
   no deeper tree to serve.
4. **`SCLASS` reports the number the channel HAS**, filling only as many
   entries as the caller's array holds (`args->count = ret` after copying
   `min(count, ret)`). Implemented verbatim, and asserted both ways (16-slot and
   2-slot calls).
5. **`DEL` of an unknown cookie is not an error** (0), and a duplicate cookie on
   `NEW` is `-EEXIST`. Both mirrored; the row prints `found=0|1` so the negative
   case is visible anyway.

Where this node deliberately differs from nouveau, each with a row: `MTHD TIME`
and `GCX_READY`, and `MAP`/`UNMAP`, answer `-ENOSYS` with a name (`need=ptimer`,
`need=gsp_rm`) where nouveau answers `-EINVAL`; `ioctl->owner` is ignored
(nouveau ignores it too) rather than validated.

---

## 3. The class table

### 3a. Served

| oclass | name | what it is | what the object IS here |
|---|---|---|---|
| `0x00000080` | `NV_DEVICE` | the device object (`cl0080.h`) | a table entry keyed by the client's cookie; `MTHD INFO` answers from the mock RM's identity |

Methods on it: `NV_DEVICE_V0_INFO` (`0x00`) served; `TIME` (`0x01`) and
`GCX_READY` (`0x02`) refused by name.

### 3b. Reported by SCLASS, then refused by name (`-ENOSYS`, `need=gsp_rm`)

These are the six classes an **AD102** channel's runlist carries, in the order
nouveau's engine tables declare them (SOURCE: `gr/ga102.c` `.sclass` then
`ce/ga102.c` `.sclass`; `nv192_chipset` dispatches AD102 to both). The ORDER is
ours (the RM's order is its runlist's); the MEMBERSHIP is the kernel's tables.
Serving one would mean creating a real GR/CE context on a real channel.

| oclass | row name | engine | nvk's low-byte match | refused with |
|---|---|---|---|---|
| `0x0000902d` | `fermi_twod_a` | 2D | `0x2d` | `-ENOSYS`, `need=gsp_rm` |
| `0x0000a140` | `kepler_inline_m2mf` | M2MF | `0x40` | `-ENOSYS`, `need=gsp_rm` |
| `0x0000c797` | `ampere_b_gr` | 3D (GR) | `0x97` | `-ENOSYS`, `need=gsp_rm` |
| `0x0000c7c0` | `ampere_compute_b` | compute | `0xc0` | `-ENOSYS`, `need=gsp_rm` |
| `0x0000c6b5` | `ampere_dma_copy_a` | copy (CE 0) | `0xb5` | `-ENOSYS`, `need=gsp_rm` |
| `0x0000c7b5` | `ampere_dma_copy_b` | copy (CE 1) | `0xb5` | `-ENOSYS`, `need=gsp_rm` |

Reported as `{oclass, minver = -1, maxver = -1}` — the spelling nouveau's engine
tables use for "any version". A class **not** in the list is `-EINVAL`
(`reason=no_such_class`), so the class lookup can come out negative and the test
tier pins that (`0x1234`, and `NV_DEVICE` under a channel).

Two honest notes about the list:

- Its membership comes from the kernel's engine tables, not from a capture of a
  running GA102 RM (INFERRED that a live channel lists exactly these and no
  others; the ids themselves are SOURCE). A real `nvkm_uchan_sclass` also walks
  every engine on the runlist, so a live list may additionally carry the `dma`
  engine's `NV_DMA_FROM/TO/IN_MEMORY` (`0x0002`/`0x0003`/`0x003d`). nvk matches
  none of those, so their absence cannot misroute it — recorded rather than
  guessed at.
- `SCLASS` is a question about the CHIP; it is not a capability claim by the
  node, and the header says so. The alternative — an empty list — makes a stock
  winsys call `nouveau_ws_subchan_alloc(fd, ch, handle, 0, …)` and hit its own
  `assert(!"called with invalid oclass")`: an unnamed death in the client
  instead of a named refusal from the kernel.

---

## 4. Gates (all MEASURED on `sarah@192.168.0.250`)

| Gate | Command | Line |
|---|---|---|
| intel build | `nvswarm build wp-nvif` / `make -j12 ARCH=intel MODE=unit_tests …` | `rc=0`; **no warning or error from any file this branch touches** (the 102 warnings a rebuild emits are pre-existing ones in `vm.h`, `file_obj.h`, `dhcp.c`, `trap.c` …) |
| intel unit tier | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` | `Tests run: 588` / `Tests failed: 0` / `UNIT TESTS PASSED` |
| EL0 wave, end to end | `tools/nv/el0_wave.sh` | `[el0] PASS`, `[EL0] RESULT PASS 44/44` |
| per-TU ARM (files touched) | `make ARCH=arm obj/arm/{nv_shim,nv_shim_drm_test,nv_rm_mock,unit_test}.o` | `rc=0`, 0 errors, 3 × pre-existing `-Wcomment` (`vm.h`) |
| ARM guard, client file | `clang --target=aarch64-linux-gnu -ffreestanding -c src/user/nvabi_probe.c` | `rc=0`, empty TU (x86_64-only by construction) |

Baseline on the same box before this branch: **584 / 0**. `588 = 584 + 4`
suites. The 16 previously-served rows still pass unchanged, and the round-1 `'N'`
face (which still refuses its own NVIF number, 14) is untouched.

---

## 5. The EL0 transcript (MEASURED, verbatim)

`tools/nv/el0_wave.sh` on the c4 box, MODE=test, `/WAVE.CFG = NVABI.ELF`.
Rows 1–19 are the round-3 ones and still pass; the new block is 20–31. Kernel
rows interleave exactly as emitted. Full file:
`evidence/nv/el0-nvif-44of44.log`.

```
[EL0] 20 nvif_new_device class=0x80 cookie=0x17fffffaa8 len=72 errno=0 PASS
[NVSHIM] nvif new class=0x80 cookie=0x17fffffaa8 dev=1 served=device
[EL0] 21 nvif_info chip=AD102 chipset=0x192 fam=0xe ram=0x100000000 PASS
[NVSHIM] nvif info chip=AD102 chipset=0x192 fam=0xe ram=0x100000000
[EL0] 22 nvif_mthd_refusals time_errno=38 unknown_object=22 short_payload=22 PASS
[NVSHIM] nvif mthd refused method=1 name=device_time need=ptimer
[NVSHIM] nvif mthd refused reason=object
[NVSHIM] nvif mthd refused reason=size
[EL0] 23 nvif_new_cookie duplicate_errno=17 zero_cookie=22 PASS
[NVSHIM] nvif new class=0x80 refused reason=cookie_exists
[NVSHIM] nvif new refused reason=cookie
[EL0] 24 nvif_sclass_controls no_parent=22 route0=22 PASS
[NVSHIM] nvif sclass refused reason=channel
[NVSHIM] nvif sclass refused reason=route
[EL0] 25 nvif_sclass ch=1 count=6 2d=0x902d 3d=0xc797 copy=0xc6b5 PASS
[NVSHIM] nvif sclass ch=1 count=6 first=0x902d 3d=0xc797
[EL0] 26 nvif_sclass_truncated slots=2 reported=6 e0=0x902d e1=0xa140 PASS
[NVSHIM] nvif sclass ch=1 count=6 first=0x902d 3d=0xc797
[EL0] 27 nvif_new_engine class=0xc797 errno=38 need=gsp_rm PASS
[NVSHIM] nvif new refused class=0xc797 name=ampere_b_gr need=gsp_rm
[EL0] 28 nvif_new_classes refused=6/6 no_such_class=22 PASS
[NVSHIM] nvif new refused class=0x902d name=fermi_twod_a need=gsp_rm
[NVSHIM] nvif new refused class=0xa140 name=kepler_inline_m2mf need=gsp_rm
[NVSHIM] nvif new refused class=0xc797 name=ampere_b_gr need=gsp_rm
[NVSHIM] nvif new refused class=0xc7c0 name=ampere_compute_b need=gsp_rm
[NVSHIM] nvif new refused class=0xc6b5 name=ampere_dma_copy_a need=gsp_rm
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm
[NVSHIM] nvif new refused class=0x1234 reason=no_such_class
[EL0] 29 nvif_new_shapes foreign_ch=22 no_channel=22 bad_version=22 bad_route=22 short=22 PASS
[NVSHIM] nvif new refused reason=channel
[NVSHIM] nvif new refused reason=channel
[NVSHIM] nvif refused reason=version
[NVSHIM] nvif refused reason=route
[NVSHIM] nvif refused reason=size
[EL0] 30 nvif_winsys_walk stop1=getparam(13)=38 then_sclass=0 stop2=nvif_new(3d)=38 PASS
[NVSHIM] drm getparam param=4 value=0x2684
[NVSHIM] nvif new class=0x80 cookie=0x17fffff920 dev=1 served=device
[NVSHIM] nvif info chip=AD102 chipset=0x192 fam=0xe ram=0x100000000
[NVSHIM] drm getparam param=17 value=0x40
[NVSHIM] drm getparam param=18 value=0x100000000
[NVSHIM] drm getparam refused param=13 reason=mock_has_no_card
[NVSHIM] nvif sclass ch=1 count=6 first=0x902d 3d=0xc797
[NVSHIM] nvif new refused class=0xc797 name=ampere_b_gr need=gsp_rm
[EL0] 31 nvif_del del=0 info_after=22 del_again=0 PASS
[NVSHIM] nvif del object=0x17fffff920 found=1
[NVSHIM] nvif mthd refused reason=object
[NVSHIM] nvif del object=0x17fffff920 found=0
...
[EL0] RESULT PASS 44/44
```

**Row 30 is the point of the lane.** It replays the tail of
`nouveau_ws_device_new` in the order that function runs it (SOURCE:
`nouveau_device.c`: `GETPARAM PCI_DEVICE` → `NVIF NEW` → `NVIF MTHD INFO` →
`GETPARAM EXEC_PUSH_MAX` → `GETPARAM VRAM_BAR_SIZE` → `GETPARAM GRAPH_UNITS` →
`nouveau_ws_context_create` → `SCLASS` → one `NVIF NEW` per engine), and reports
the two walls **in the order a client meets them**:

- `getparam(13) = GRAPH_UNITS = -ENOSYS` (`reason=mock_has_no_card`). In Mesa
  this call is fatal (`goto out_err`), so this is the *first* thing that stops a
  stock nvk, and it is a `GETPARAM` wall, not an NVIF one.
- Continuing as if it had answered: `SCLASS` succeeds with 6 classes, and the
  class nvk would pick from that list (`0xc797`) is refused `-ENOSYS`
  (`need=gsp_rm`).

Two client-side numbers agree with two kernel-side numbers throughout: the
cookie the client chose (`0x17fffff920`, an address in its own stack) is the
cookie the kernel's rows name, and the class ids the client parses out of its
own buffer are the ids the kernel says it wrote.

One defect found by a row and fixed (`evidence/nv/el0-red-nvif-del-buffer.log`,
red at 43/44, then `el0-nvif-44of44.log` green): row 31's second `DEL`
initially answered `-EINVAL` (`del_again=22`) because the shared 192-byte
request buffer had been rebuilt as an `MTHD` by the call in between — a
client-side bug in the probe (a real winsys uses one buffer per call), caught
because the row demanded `0` and got `22`.

---

## 6. The mutation runs (a test that never failed is not evidence)

Both applied to `src/kernel/nv_shim.c`, run, recorded, reverted.

### M1 — wrong class id (`evidence/nv/nvif-mutation-m1-wrong-class.log`)

`SCLASS` reports `0x0000c597` (Turing 3D) where AD102's class is `0x0000c797`:

```
EXPECT_EQ FAILED: (int)list[2].oclass == NVSHIM_NVIF_CLASS_AMPERE_B (nv_shim_drm_test.c:1577)
EXPECT_EQ FAILED: nv_rm_mock_live() == 0 (nv_shim_drm_test.c:1698)
Tests run: 588 / Tests failed: 2 / UNIT TESTS FAILED
[EL0] 25 nvif_sclass ch=1 count=6 2d=0x902d 3d=0xc597 copy=0xc6b5 FAIL
[EL0] 30 nvif_winsys_walk stop1=getparam(13)=38 then_sclass=0 stop2=nvif_new(3d)=22 FAIL
[EL0] RESULT FAIL 42/44 failed=2
```

The downstream row is the interesting one: with the wrong id, the winsys's
class-selection asks for a class the node does not know, and the answer becomes
`-EINVAL` (a *wrong request*) instead of the named `-ENOSYS` that says "this
needs GSP-RM". A plausible-looking number turns a wall into a mystery — exactly
the failure mode this project keeps paying for.

### M2 — the exact-length check disabled (`evidence/nv/nvif-mutation-m2-size-check.log`)

`MTHD` accepts any payload length (`if (0 && argc != …)`):

```
EXPECT_EQ FAILED: nvif_mthd(&pr, cookie, NVSHIM_DEVICE_V0_INFO, 135, 0) == -EINVAL (…:1465)
Tests run: 588 / Tests failed: 1 / UNIT TESTS FAILED
[EL0] 22 nvif_mthd_refusals time_errno=38 unknown_object=22 short_payload=0 FAIL
[EL0] RESULT FAIL 43/44 failed=1
```

`short_payload=0` is the shape of the bug: the call *succeeds* while the kernel
reads a 104-byte struct out of a 103-byte buffer — one byte past what the caller
handed over.

Reverted; rebuilt; re-ran both gates: **588 / 0** and **44/44** (MEASURED, the
transcripts in `evidence/nv/`).

---

## 7. What a stock nvk still cannot do, and why (ordered, as it will hit them)

1. **It never reaches NVIF at all on this node today**, because of
   `GETPARAM GRAPH_UNITS` (row 30). nvk needs GPC/TPC counts at device-create
   time and treats the failure as fatal; the mock has no topology to report, and
   inventing two numbers would be a lie a client builds heaps and dispatch
   geometry on. **This is the next cheap, honest thing to serve** — but it needs
   a source for the counts (the card, or a declared model of it), not a guess.
2. **An engine class object** (`NVIF NEW`, route `0xff`) — `-ENOSYS`,
   `need=gsp_rm`. This is a real HW class on a real channel: a GR context with
   its own VMM/instance allocations, programmed via RM methods. Without GSP-RM
   there is no way to build one, and a bookkeeping-only object would let nvk
   build a command stream that no engine ever reads — worse than a refusal,
   because the failure would surface as a hang or a corrupt frame later.
3. **`NV_DEVICE_V0_TIME`** (`need=ptimer`) and **`GCX_READY`/`MAP`/`UNMAP`**
   (`need=gsp_rm`). Also `GETPARAM PTIMER_TIME`: nvk's `get_gpu_timestamp()`
   falls back to 0, so it does not stop enumeration, but any timestamp-based
   query would be wrong.
4. **Past that**, the already-named walls from earlier lanes, unchanged:
   `GEM_PUSHBUF` (nvk's channel-killed probe), the three PRIME rows (WSI),
   timeline syncobjs (`RUN_ASYNC` VM_BIND), `SVM`, and the fs-side fact that a
   stock nvk opens `/dev/dri/renderD128`, not `/dev/nvshim`.

So the honest one-line answer to "does a stock nvk get past object creation?":
**the device object and its identity are served and proved from ring 3; the
winsys's own device probe still dies at `GETPARAM GRAPH_UNITS` before it asks
for a channel object, and if that were answered it would die at the first
engine class object, by name.**

---

## 8. What is NOT verified

- **No real nvk ran.** Nothing in this branch links against Mesa. The call
  order in row 30 is transcribed from `nouveau_device.c`/`nouveau_context.c`
  (SOURCE), not observed from a running binary; the row proves the *node's
  answers* to that sequence, not that nvk would reach them.
- **The SCLASS membership is a reconstruction** from the kernel's engine tables
  (see §3b). No GA102 RM was interrogated.
- **No engine, no GSP, no card.** `mock=1` in every row; no object this branch
  creates has RM memory or a GPU address behind it.
- **`revision = 0xa1`** is the value PLAN.md §1 measured from the card's
  `PMC_BOOT_0`; the mock reports it because the mock *is* the AD102 stand-in
  (chipset `0x192`, device `0x2684`), not because anything read it this round.
- **Padding and `owner`** fields are accepted and ignored, as nouveau accepts
  and ignores them; no test asserts they are *rejected*.
- **Multiple contexts per client**: the table holds 24 objects and is
  per-client; a winsys that keeps many contexts alive at once is untested
  (`nvk` makes one probe context plus one per engine set, so 24 was chosen with
  headroom, not measured against a long-running client).
- **ARM is compile-only** (per-TU). The full ARM link is still broken by another
  lane's `.bss` placement; no ARM unit-tier boot was run on this branch.
- **`'N'` face**: still refuses its own NVIF number (14). Deliberate — it is
  deprecated and carries nothing the `'d'` face does not.

---

## 9. Next step

1. **`GETPARAM GRAPH_UNITS`** — the only thing between this node and a winsys
   that reaches `SCLASS` on its own. It needs a truthful source: either the
   card, or a *declared* model of AD102 with its provenance written down beside
   it (PLAN.md §1 has the measured `PMC_BOOT_0`, not a topology). Until one of
   those exists, `-ENOSYS` with `reason=mock_has_no_card` is the honest answer.
2. Then the engine object. That is GSP-RM (rung N3), and this lane's job is
   finished at naming it: `nvif new refused class=0xc797 name=ampere_b_gr
   need=gsp_rm`.
