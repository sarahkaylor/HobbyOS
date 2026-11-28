# GB202 (RTX 5090 `41:00.0`, VM 313) — the three RM objects, as mock objects

Card **`t_c5453775`**, child of the wall card `t_a769beaa`. This file answers the
card's question for three objects — a **channel**, a **VA space**, an **engine
class** — in the form the owner re-scoped it to on 2026-11-28T05:44Z: all three
objects are minted by the **mock** RM behind `/dev/nvshim`, and every row and
every sentence about them says `mock`. **Zero GB202 boots were run**: the boot
campaign was stopped at 06:00Z, and VM 300 / 313 / 314 / 315 were not started by
any card in this spine.

**Part identity, stated once and true of every row in Part I: GB202, `41:00.0`,
VM 313.** NVIDIA GB202 [GeForce RTX 5090], `10de:2b85`, at PCI `41:00.0`, owned
by VM 313 (`hobbyos-nv5090`). Read-only, with nothing booted
(2026-11-28T06:54:55Z; commands and output exactly as `r1` printed them):

```
$ qm config 313 | grep -E "^(cores|memory|name|hostpci0):"
cores: 8
hostpci0: 0000:41:00,pcie=1
memory: 16384
name: hobbyos-nv5090
$ lspci -nn -s 41:00.0
41:00.0 VGA compatible controller [0300]: NVIDIA Corporation GB202 [GeForce RTX 5090] [10de:2b85] (rev a1)
```

VM 300, for contrast, still passes through `0000:01:00` — the AD102:
`hostpci0: 0000:01:00,pcie=1`. **No object in this document was created on
either card.** The runs that produced the rows are the unit-tier guests (x86-64
and AArch64, QEMU TCG, no PCI passthrough), which is why every handle below is a
mock handle and the one physical address is pool RAM.

**What this file is not.** Nothing here may be quoted as "GSP-RM allocated", a
"firmware handle", a VRAM address, or L3 evidence — the owner's re-scope forbids
each of those. The firmware version of this claim stays parked on the GB202 ACR
wall `t_a769beaa` (revive condition R2: a GSP-RM peer appears), which is Part II
of this file.

**Two parts, two numbering schemes.** **Part I** (§0–§10) is the three mock
objects. **Part II** is the wall card's FMC corpus record, kept unchanged with
its own section numbers (§0–§13); a bare "§13.5" below means the wall record's.

---

## 0. The result, in six lines

1. **All three objects allocated — as mock objects.** Channel `ch=1`, VA space
   `vm=1`, engine class `handle=1`. Each handle is quoted below from the log line
   that printed it, and each was stamped by the mock RM's own counter, not by
   the client.
2. **The firmware hypothesis is falsified as written and was not attempted.**
   There is no GSP-RM peer on GB202 to ask: the wall's ladder ends at
   `MAILBOX0=0x0B` / `0x824` with `linked=0 initdone=0 statq_written=0` (Part II,
   §0). Zero boots; no mock reply is offered as a firmware reply.
3. **No `NV_VGPU_MSG_*` message id is used anywhere.** The ids that are on the
   path are `NVSHIM_IOC_*` requests on the HobbyOS `'N'` face and their DRM-face
   sub-numbers, each named with `file:line` (§6).
4. **The VA space's physical address reads back, and it is pool RAM, not VRAM**:
   `phys=0x209686000` on intel TCG, `phys=0x8ef46000` on ARM — two platforms,
   two real addresses, which is what a read-back looks like and what a retyped
   constant would not.
5. **No GPU-visible layout number is consumed by these objects, and no GB202
   layout number is claimed.** `docs/nv-plan.md` §2.7's `gpc=11 tpc=64
   src=register` is an **AD102** measurement (`t_78a50be7`, `vmid=300`); no GB202
   boot log in this tree carries that row, so this document does not copy it to
   GB202 (§8).
6. **Clean closing state for every run named here**: VM 313 `stopped`, VM 300
   `stopped`, `pgrep -c qemu-system-x86` = 0, re-measured at 06:54:55Z; the GB202
   guest's files still carry the previous boot's mtime (05:39:01Z), so nothing in
   this spine wrote them (§9).

---

## 1. The three hypotheses, and their verdicts

Each object card was written as a **firmware** hypothesis ("Boot GB202 and
allocate … from GSP-RM"). The owner re-scoped all three on 2026-11-28T05:44Z
(mock RM is acceptance; the firmware claim stays parked) and stopped the boot
campaign at 06:00Z. So each hypothesis is recorded here as it was written, and
the verdict is the same shape for all three:

| object | named hypothesis (as the card was written) | verdict | what was delivered instead |
|---|---|---|---|
| channel | "GSP-RM on GB202 allocates a channel object for our driver, and the handle comes from firmware, not the mock RM." | **falsified as written; not attempted** (0 boots, no peer) | mock channel object `ch=1` through `NVSHIM_IOC_CHANNEL_ALLOC` |
| VA space | "GSP-RM on GB202 allocates a VA space object for our driver and a VA bound through it resolves to a real VRAM physical address readable back from the mapping." | **falsified as written; not attempted** (0 boots, no VRAM) | mock VA space `vm=1` + a binding whose address the RM's own walk answers |
| engine class | "GSP-RM on GB202 allocates one engine class object for our driver, with the handle coming from firmware." | **falsified as written; not attempted** (0 boots, no peer) | mock engine class `handle=1` on `ch=1`, class `0xC7B5` |

**Step 6 of this card's body is superseded, and this file obeys the newer
instruction.** The body said: "explicitly state that every object came from
firmware and that no mock/synthetic object is being claimed as acceptance." The
owner's 05:44Z comment inverts it: the mock *is* acceptance for these objects,
every row and sentence must say `mock`, and no result here may be quoted as
"GSP-RM allocated" or as L3 evidence. This document says so once here, says
`mock` on every row, and does not claim a firmware allocation anywhere.

---

## 2. The invocation for each object — zero boots, and what ran instead

The card body's "boot invocation" line is the wall harness's GB202 boot, which
**was not run**. For the record, that invocation looks like this (quoted from the
wall's own log header, `ATT/t_a769beaa/5090-gb202fsp-rm50.log:1-3`):

```
# [NV-GB202FSP] boot 2026-11-26T07:22:49Z
# image=/home/nvcard/.hermes/kanban/boards/hobbyos-nv/workspaces/t_a769beaa/rm50.img sha256=e5163852fb…
# tag=rm50 vm=313 hostpci0=0000:41:00
```

The invocation that actually produced every row in this document is the **unit
tier** — the kernel's own test image booted under QEMU, TCG, no passthrough. The
scripts print their own banner, quoted from the top of each log
(`ATT/…/unit-tier-*.log:1`): `Running unit tests with QEMU_ARGS: ` — the argument
list is empty, so no host device was handed to the guest.

| object | tested tree (source sha) | intel (x86-64, TCG) | ARM (AArch64, TCG) | verdict rows | log |
|---|---|---|---|---|---|
| channel | `fd69e681` on `f7c2a714` | `TOOLS_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh` | `TOOLS_SKIP=1 ./run_unit_tests.sh` | intel **621/0**, ARM **486/0** | `unit-tier-intel-chan.log:3205-3207`, `unit-tier-arm-chan.log:2463-2465` |
| VA space | `7faed719` on `ef591e93` (merge `8c22fe17`) | `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1 UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` | `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests.sh` | intel **623/0**, ARM **486/0** | `unit-tier-intel-vm.log:3242-3244`, `unit-tier-arm-vm.log:2482-2484` |
| engine class | `5e0887a7` on `63a44fab` (merge `1c0c3c0f`) | same as VA row | same as VA row | lane 621/0 · 486/0; merged tip intel **623/0** | `unit-tier-intel-ec.log:3210-3212`, `unit-tier-arm-ec.log:2470-2472`, `unit-tier-intel-merged.log:3226-3228` |

Where a row was re-verified on the merged tip, the cite is given with the quote.
The tier runs are guest boots of the *unit-test* image; **none of them is a GB202
boot**, and none of them touched `41:00.0`.

---

## 3. Object 1 — the channel object (`ch=1`)

### 3.1 The rows, quoted verbatim

Request — `ATT/t_17ca89e5/unit-tier-intel-chan.log:2350` (MEASURED):

```
[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x209686000 size=0x10000
```

Response, and the handle — same log `:2351` (MEASURED):

```
[NVSHIM] channel rsp mock=1 ch=1 engine=1
```

**The handle is `ch=1`**, quoted from the reply row, not from a summary. ARM
printed the same pair at `unit-tier-arm-chan.log:1637-1638`, with that tier's own
push-buffer address:

```
[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x8ef46000 size=0x10000
[NVSHIM] channel rsp mock=1 ch=1 engine=1
```

The `mock=1` on the reply is not decoration — the line above the pair is the
node's provenance (`:2341`, `:2342`, MEASURED):

```
[NVSHIM] device dev=1 chipset=0x192 pci=0x2684 mock=1 feats=0x1ff
[NVSHIM] getparam refused param=14 reason=mock_has_no_card
```

`chipset=0x192 pci=0x2684` is the mock's **declared** AD102 identity; it read no
card, and says so on the next line.

A second allocation in the same boot proves the handle is the RM's counter
(`:2391-2392`, MEASURED):

```
[NVSHIM] channel req mock=1 ioc=channel_alloc vm=1 pushbuf=0x209685000 size=0x10000
[NVSHIM] channel rsp mock=1 ch=2 engine=1
```

The stock client's face mints the same kind of object (`:2426-2427`, `:2440`,
MEASURED):

```
[NVSHIM] drm channel req mock=1 ioc=drm_channel_alloc vm=1 pushbuf=0x209696000 size=0x10000
[NVSHIM] drm channel rsp mock=1 ch=1 pushbuf=0x209696000 size=0x10000
[NVSHIM] drm channel_free mock=1 ch=1
```

ARM: `unit-tier-arm-chan.log:1713-1714`, `:1727`.

### 3.2 The ids used

* `NVSHIM_IOC_CHANNEL_ALLOC` — `src/include/nv_shim.h:308` in the merged tree
  (`:277` in the tree this card tested, before the VA and engine-class rows
  shifted the header):
  `NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 8, struct nvshim_channel)`, with
  `#define NVSHIM_IOC_MAGIC 'N'` (`nv_shim.h:106`). This is the request the row
  above names `ioc=channel_alloc`.
* DRM face: `NVSHIM_DRM_NOUVEAU_CHANNEL_ALLOC = 0x02` (`src/include/nv_drm_uapi.h:545`),
  encoded as `NVSHIM_DRM_IOC_CHANNEL_ALLOC` (`:573-575`) and named in the row
  as `ioc=drm_channel_alloc`.
* **No `NV_VGPU_MSG_*` id.** The mock has no message protocol — no queue, no
  packet, no console — and nothing on this path sends one (§6).

### 3.3 Where the handle comes from

SOURCE, `src/kernel/nv_rm_mock.c:514-546` (`mock_channel_create`): the request is
validated against the mock's own tables, the first free slot is taken, and
`c->id = (uint32_t)++g_mock.next_ch;` (`:539`). The interface is named `"mock"`
(`nv_rm_mock.c:833`) and the node is bound to it wholesale
(`nv_shim.c`, `static const struct nv_rm_if *rm(void) { return nv_rm_mock_if(); }`).
`ch=1` is an index in `g_mock.ch[]`, not a GSP-RM object.

---

## 4. Object 2 — the VA space object (`vm=1`) and the address it resolves

### 4.1 The rows, quoted verbatim

Request and response — `ATT/t_b9c4df6d/unit-tier-intel-vm.log:2361-2362`
(MEASURED):

```
[NVSHIM] vm req mock=1 ioc=vm_init size=0x100000000
[NVSHIM] vm rsp mock=1 vm=1 size=0x100000000
```

**The VA space handle is `vm=1`.** ARM: `unit-tier-arm-vm.log:1630-1631`;
merged tip intel: `unit-tier-intel-merged.log:2356-2357`.

The two bind rows — request, then the reply that carries the address the RM's
walk answered (`:2363-2364`, MEASURED):

```
[NVSHIM] vm_bind req mock=1 ioc=vm_bind vm=1 op=0 va=0x10000000 range=0x10000 handle=1 bo_offset=0x0
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x10000000 range=0x10000 phys=0x209686000 bus=0x209686000 mapped=0x10000
```

**The physical address read back is `phys=0x209686000`** (intel TCG). ARM, same
request, its own frame (`unit-tier-arm-vm.log:1632-1633`, MEASURED):

```
[NVSHIM] vm_bind req mock=1 ioc=vm_bind vm=1 op=0 va=0x10000000 range=0x10000 handle=1 bo_offset=0x0
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x10000000 range=0x10000 phys=0x8ef46000 bus=0x8ef46000 mapped=0x10000
```

Merged tip intel reproduces the intel pair byte for byte at
`unit-tier-intel-merged.log:2358-2359`. `op=0` is `NVSHIM_VM_BIND_OP_MAP`,
`handle=1` is the 64 KiB memory object the probe allocated, `bo_offset=0x0`,
`range=0x10000` covers it whole; `mapped=0x10000` is the walk's extent from `va`.

**Why this is a measurement and not a constant.** The two tiers differ — that is
the point: each address is the first frame of the run `nv_dma_alloc` handed that
tier's pool. It is answered by the RM's own walk of the binding, not echoed from
the request: `struct nvshim_vm_bind` has **no address field of any kind**
(`src/include/nv_shim.h:228`), so the client cannot even offer one.

The walk translates rather than repeating. The argument suite binds one object at
two VAs and each resolves through its own binding (`:2417`, `:2420`, MEASURED):

```
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x1000 range=0x1000 phys=0x209685000 bus=0x209685000 mapped=0x1000
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x3ffff000 range=0x1000 phys=0x209685000 bus=0x209685000 mapped=0x1000
```

and the tier holds it to values, not to prose (SOURCE,
`src/kernel/nv_shim_test.c:313-334`, `:449`): `vm_resolve(0x10000000)` returns
`phys != 0`, `mapped == 0x10000`, `bus == phys`; `vm_resolve(0x10002000)` returns
`phys + 0x2000` with `mapped == 0x10000 - 0x2000`; a hole and a VA at the space's
size are `-EINVAL`; and a store of `0x5a5aa5a5` **at the resolved physical
address** is read back through the RM's object handle at the same offset
(`nv_rm_mock_read_bo`), so the answer names the object's bytes.

The same `vm=1` appears again for the second and third probe in that boot
(`:2400-2401`, `:2414-2415`, MEASURED, both with `size=0x40000000`) because each
probe is a separate device lifetime: the last `device_close` calls `mock_reset()`
(SOURCE: `src/kernel/nv_rm_mock.c:175-185`, called at `:265`), which zeroes the
counters.
They are sequential first-spaces, not three concurrent spaces sharing a handle.

Refusals are named, not silent (`:2366`, `:2422`, `:2423`, `:2424`, MEASURED):

```
[NVSHIM] vm_bind refused mock=1 va=0x10000000 range=0x10000 op=0 rm=-22
[NVSHIM] vm_bind refused mock=1 va=0x40000000 range=0x1000 op=0 rm=-22
[NVSHIM] vm_bind refused mock=1 handle=1 reason=not_this_clients_object
[NVSHIM] vm_bind refused mock=1 va=0x30000 range=0x1000 op=7 reason=geometry
```

An UNMAP has nothing left to name, so its reply carries no address (`:2383`,
MEASURED): `[NVSHIM] vm_bind rsp mock=1 op=1 va=0x10000000 range=0x10000`.

The stock client's face carries the same read-back (`:2460-2461`, MEASURED):

```
[NVSHIM] drm bind vm=1 ops=2 first_addr=0x10000 first_range=0x10000 phys=0x209685000 bus=0x209685000 mapped=0x10000
[NVSHIM] drm bind vm=1 ops=1 first_addr=0x20000 first_range=0x1000
```

ARM: `unit-tier-arm-vm.log:1729-1730`. A batch whose first op is an UNMAP omits
the field rather than answering zero.

### 4.2 "VA → VRAM": the one sentence this document cannot write

**These are not VA→VRAM bind rows.** The object's frames are `nv_dma_alloc`'s
host pool RAM; GB202's VRAM is never allocated or addressed on this path. The
honest label is **VA → physical address of a pool-RAM object**, and the tree's
own header for the new seam says the same thing in the code
(SOURCE, `src/include/nv_rm.h:174-176`):

```
   * This is the mock's stand-in for "a VA bound to VRAM resolves to a VRAM
   * physical address": the object's frames are real pool RAM, not card VRAM,
   * and no result here is evidence about the card. */
```

So the card body's "VA→VRAM bind rows" cannot be satisfied by this run, and this
document says so rather than rephrasing the row to look like it did.

### 4.3 The ids used

* `NVSHIM_IOC_VM_INIT` — `src/include/nv_shim.h:306`:
  `NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 6, struct nvshim_vm_init)`; the row names it
  `ioc=vm_init`.
* `NVSHIM_IOC_VM_BIND` — `src/include/nv_shim.h:307`:
  `NVSHIM_IOW(NVSHIM_IOC_MAGIC, 7, struct nvshim_vm_bind)`; the row names it
  `ioc=vm_bind`.
* Structures: `struct nvshim_vm_init` (`nv_shim.h:219`, `vm_id` is **out**) and
  `struct nvshim_vm_bind` (`:228`, and no address field).
* DRM face: `NVSHIM_DRM_NOUVEAU_VM_INIT = 0x10`, `NVSHIM_DRM_NOUVEAU_VM_BIND = 0x11`
  (`src/include/nv_drm_uapi.h:553-554`).
* **No `NV_VGPU_MSG_*` id.** For the parked path only (SOURCE, fetched —
  `nouveau-r535-vmm.c`, attached): nouveau's `r535_mmu_vaspace_new()` makes the
  VA space an RM allocation of class `FERMI_VASPACE_A` whose handle is
  `vmm->rm.object.handle`, and programs the page directory with
  `NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY` / `NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES`
  — control commands, not `NV_VGPU_MSG_FUNCTION_*` ids. None of that was sent.

### 4.4 Where the handle and the address come from

SOURCE, `src/kernel/nv_rm_mock.c`: `mock_vm_create()` (`:393-421`) stamps
`v->id = (uint32_t)++g_mock.next_vm` (`:405`); `mock_vm_resolve()` (`:480-510`)
finds the range covering `va`, resolves its object and returns
`a->phys + r->bo_offset + (va - r->va)` — where `a->phys` is the first byte of
the run `mock_mem_alloc()` got from `nv_dma_alloc` (`:281`, stored at `:322`:
`a->phys = m.phys;`). The seam is registered at `nv_rm_mock.c:842`
(`g_rm_if.vm_resolve = mock_vm_resolve`). `vm=1` is an index in `g_mock.vm[]`,
not a GSP-RM object.

---

## 5. Object 3 — the engine class object (`handle=1`)

### 5.1 The rows, quoted verbatim

Request — `ATT/t_7cf2e538/unit-tier-intel-ec.log:2349` (MEASURED):

```
[NVSHIM] engine_class req mock=1 ioc=engine_class_alloc ch=1 class=0xc7b5 name=ampere_dma_copy_b engine=copy1 alloc=ce_alloc
```

Response, and the handle — `:2350` (MEASURED):

```
[NVSHIM] engine_class rsp mock=1 ch=1 class=0xc7b5 handle=1
```

**The handle is `handle=1`.** ARM printed the same pair at
`unit-tier-arm-ec.log:1639-1640`; the merged tip intel log at
`unit-tier-intel-merged.log:2366-2367`, one line after the channel reply that
created `ch=1` (`:2364-2365`).

A second object proves the counter is the RM's, and the explicit free names the
handle (`unit-tier-intel-ec.log:2351-2352`, `:2355`; ARM `:1641-1642`, `:1645`,
MEASURED):

```
[NVSHIM] engine_class req mock=1 ioc=engine_class_alloc ch=1 class=0xc797 name=ampere_b_gr engine=eng3d alloc=rm_alloc
[NVSHIM] engine_class rsp mock=1 ch=1 class=0xc797 handle=2
[NVSHIM] engine_class free mock=1 handle=2
```

`handle=1` is deliberately left to its channel: `CHANNEL_FREE`'s cascade frees
it, and `nv_rm_mock_live() == 0` after teardown is what proves the cascade ran.

Refusals, named (`:2353-2354`, ARM `:1643-1644`, MEASURED):

```
[NVSHIM] engine_class refused class=0x1234 reason=no_such_class
[NVSHIM] engine_class refused reason=not_this_clients_channel
```

### 5.2 The class asked for, and the wall this did not move

`0xC7B5` is `NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B`
(`src/include/nv_nvif_uapi.h:221`), `ampere_dma_copy_b`, engine instance
`copy1`, CE `ce_alloc` shape — the **first** engine class a stock nvk asks for
(`nouveau_ws_context_find_class(classes, 0xb5)` takes the highest class whose low
byte matches). The `0xC797` object exists only to show the counter moves.

**The stock client is still refused, by name, in the same boot**
(`unit-tier-intel-ec.log:2961`; merged tip `:2978`; ARM `:2251`, MEASURED):

```
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm missing=rm_chan_object alloc=ce_alloc engine=copy1
```

So nothing here says nvk gets past the engine-class object. The new rows are
HobbyOS-only (`'N'` face); the NVIF path deliberately keeps answering
`need=gsp_rm`.

### 5.3 The ids used

* `NVSHIM_IOC_ENGINE_CLASS_ALLOC` — `src/include/nv_shim.h:312`:
  `NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 18, struct nvshim_engine_class)`; row name
  `ioc=engine_class_alloc`.
* `NVSHIM_IOC_ENGINE_CLASS_FREE` — `src/include/nv_shim.h:314`:
  `NVSHIM_IOW(NVSHIM_IOC_MAGIC, 19, struct nvshim_engine_class_free)`.
* The class id: `NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B 0x0000c7b5`
  (`src/include/nv_nvif_uapi.h:221`); structure `struct nvshim_engine_class`
  (`nv_shim.h:265`).
* **No `NV_VGPU_MSG_*` id**, for the same reason as the other two objects: on the
  real Ada path the object is an RM allocation under the channel's RM object
  (`nvkm_gsp_rm_alloc`, CE through `rm->api->ce->alloc`), and no message is sent
  here, so none is named.

### 5.4 Where the handle comes from

SOURCE, `src/kernel/nv_rm_mock.c:580-617` (`mock_engine_class_create`): the
object must be requested on a live channel of the same device, the first free
slot is taken, and `e->handle = ++g_mock.next_ec;` (`:608`). The object is
parented on the channel; `mock_channel_destroy` frees every class object of that
channel and `nv_rm_mock_live()` counts them, so a leak fails the tier.
`handle=1` is an index in `g_mock.ec[]`, not a GSP-RM object.

---

## 6. The message ids: there are none, and naming one would be the fabrication the re-scope forbids

**Zero `NV_VGPU_MSG_*` message ids were used by any of the three objects.** The
mock has no message protocol: no queue, no packet, no console, and the shim's own
comments say so at the three object entry points
(`src/kernel/nv_shim.c:1231`, `:1317`, `:1527`).

The whole tree contains **seven** occurrences of the string `NV_VGPU_MSG`
(`grep -r NV_VGPU_MSG src/ | wc -l` = 7): six are comments
(`src/include/nv_rm.h:70`, `src/kernel/nv_gsp.c:1148`,
`src/kernel/nv_fsp_cot.c:1253`, `src/kernel/nv_shim.c:1231`, `:1317`, `:1527`)
and one is an event name printed when the GSP-RM init event arrives
(`src/kernel/nv_gsp.c:978`, `NV_VGPU_MSG_EVENT_GSP_INIT_DONE, 0x1001`).
**None of them is a request any of these three objects makes**, and none is a
reply. The parked firmware path's
object ids, where they are known at all, are RM allocations and control commands
(§4.3), not `NV_VGPU_MSG_FUNCTION_*` ids.

The ids that *are* on the path, per object:

| object | `'N'` face request | DRM face | message ids |
|---|---|---|---|
| channel | `NVSHIM_IOC_CHANNEL_ALLOC` (ioctl 8) | `NVSHIM_DRM_NOUVEAU_CHANNEL_ALLOC = 0x02` | none |
| VA space | `NVSHIM_IOC_VM_INIT` (6), `NVSHIM_IOC_VM_BIND` (7) | `NVSHIM_DRM_NOUVEAU_VM_INIT = 0x10`, `…_VM_BIND = 0x11` | none |
| engine class | `NVSHIM_IOC_ENGINE_CLASS_ALLOC` (18), `…_FREE` (19) | — (HobbyOS-only `'N'` face by design) | none |

---

## 7. GPU-visible layout numbers: none consumed, and none borrowed

The card asks for "the measured GPU-visible layout number and how it was
measured (explicitly note it was measured on GB202, not copied from AD102 per
`docs/nv-plan.md` §2.7)". It gets three separate answers, because the honest
answer is not one number.

### 7.1 What these objects consume: no layout number at all

A mock channel, a mock VA space and a mock engine class **consume no GPU-visible
layout number**. Every number in the quoted rows is this run's own value — the
granted space (`0x100000000`), the VA (`0x10000000`), the range (`0x10000`), the
physical addresses, the class id, the handles. Nothing here was read from GB202,
so nothing here can be a GB202 layout number.

The only layout-ish constants on the path belong to the mock and are labelled as
its bookkeeping (SOURCE, `src/kernel/nv_rm_mock.c`):

```
#define MOCK_PAGE 4096ull                    /* :46  — the mock's granularity */
#define MOCK_VRAM_SIZE (4ull << 30)          /* :52  — the mock's arena, what FB_SIZE reports */
#define MOCK_VA_LIMIT (1ull << 40)           /* :57  — 1 TiB, what a stock client asks for */
```

These are the mock's, not GB202's: `MOCK_VA_LIMIT`'s own comment says 1 TiB is
the total a **stock nouveau client** asks for (`drm_nouveau_vm_init {1<<39, 1<<39}`),
and "the objects mapped into it still get their bus addresses from the 4 GiB
arena above". No GB202 boot measured them, and this document does not present
them as GB202's page size or VA width.

### 7.2 §2.7's number was measured — on AD102, and this document will not move it to GB202

The register-measured layout number this project actually has is GR topology
`gpc=11 tpc=64 src=register`. **It was measured on AD102, not GB202.** The boot
rows (MEASURED, `ATT/t_78a50be7/tpcba.log:167-168`):

```
[NV-ENG] topology verdict=MEASURED (gpc/tpc are register fields; this replaces the declared 11/64 row)
[NV-ENG] measured gpc=11 tpc=64 src=register decode=units.tpc_count[4:0] xcheck_ppc=64
```

How it was measured: `0x409604` bits [4:0] give the GPC count; each GPC's
`GPC_UNIT(i,0x2608)` `TPC_COUNT` bits [4:0] are summed for the TPC count and
cross-checked against the mask-encoded `ppc_tpc_mask`; a control register in the
same block must answer data or the read is declared `IMPLAUSIBLE` rather than
"zero GPCs". The rows it was read from are
`[NV-ENG] gr topology 0x409604=0x000000000C06000B gpc_nr=11 rop_nr=6 … control_ok=1`
and the eleven per-GPC mask words.

The report that owns that number states its part in its first lines
(`ATT/t_78a50be7/nv-topology-measured.md:1-4`):

```
# AD102 topology, measured and decoded (11 GPC / 64 TPC)

**Part:** AD102, `10de:2684` (RTX 4090), VM 300 on r1. **This is not a GB202
number and no GB202 number is used here.**
```

and the boot's own record says `vmid=300` (`ATT/t_78a50be7/tpcba.record`).

**The correction, stated plainly.** `docs/nv-plan.md:608` groups that row under
"measured on both parts (AD102 and GB202)". For the topology clause that sentence
is **not backed by any GB202 log in this tree**: no GB202 boot log carries an
`[NV-ENG] gr topology` or `measured gpc=` row (the GB202 wall boots are FSP boots
and print none; the `measured gpc=11 tpc=64` rows that appear in the unit-tier
logs come from the test's own fake register block, which is set to "the 4090's
published shape: 11 GPC, 64 TPC" — `src/kernel/nv_test.c:530-550`, not a card
read), and the report that owns the measurement says AD102 in its own first
paragraph. So:

* this document does **not** present `11/64` as GB202's number;
* it does **not** copy AD102's `11/64` into a GB202 row, which is exactly what
  `docs/nv-plan.md` §2.7's "declared model" caveat exists to prevent;
* **GB202's GR topology has not been measured**, and the one read-only boot that
  would measure it (read `0x409604` and `GPC_UNIT(i,0x2608)` with a control
  register) is not authorized while the boot campaign is stopped.

The mock does not quietly change that: its device identity still *declares* AD102
— `chipset 0x192`, `device_id 0x2684`, name `"AD102"`, revision `0xa1`
(SOURCE: `src/kernel/nv_rm_mock.c:239-249`) — while a real GB202 boot reads
`chipset=0x1b2 gpu=GB202 stepping=161`
(`ATT/t_a769beaa/5090-gb202fsp-rm50.log:132`, MEASURED). A document that wanted a
GB202 number could not get one from this run, and this one does not pretend
otherwise.

### 7.3 What GB202 *has* measured in the layout register sense — and why it is not a VA number

The wall card's boots read the WPR2 band **on this card** and printed it twice, in
two unit systems, in the same boot
(`ATT/t_a769beaa/KEY-ROWS-gb202-rm-objects.txt:6`, `:40`; MEASURED):

```
[NV-FSPCOT] rm tl t_ms=27000 hwcfg2=0x8187A7F7 m0=0x0 m1=0x0 os=0 rm=0 rvctl=0xBADF5620 wpr2_lo=0x07E61200 wpr2_hi=0x07F1EE00
[NV-FSPCOT] wpr2c t15s lo=0x7E6120000 hi=0x7F1EE0000
```

`0x07E61200 << 8 = 0x7E6120000` and `0x07F1EE00 << 8 = 0x7F1EE0000`, so the
`rm tl` row is in 256-byte units and the `wpr2c` row in bytes (a 189.75 MiB
span). That is a GB202 measurement, not a constant. **None of it is a VA-space,
page-table or engine-layout number**, and this document does not stretch it into
one; it is quoted only to show what "measured on GB202" looks like when it is
true.

---

## 8. Closing state, for every run named in this document

Read-only checks only — no boot, no VM start. Each row is the state immediately
after that run; the last row was taken for this document.

| after | measured (UTC) | `qm status 313` | `qm status 300` | `pgrep -c qemu-system-x86` | GB202 guest files |
|---|---|---|---|---|---|
| channel tier run (intel + ARM) | 2026-11-28T05:49:14Z | stopped | stopped | 0 | mtime 05:39:01Z (previous boot) |
| engine-class tier runs (lane) | 2026-11-28T06:18:56Z | stopped | stopped | 0 | mtime 05:39:01Z |
| engine-class merged-tip reruns | 2026-11-28T06:29:37Z | stopped | stopped | 0 | mtime 05:39:01Z |
| VA-space tier runs (lane + merged) | 2026-11-28T06:47:40Z (+06:50Z recheck) | stopped | stopped | 0 | mtime 05:39:01Z |
| **re-measured for this document** | **2026-11-28T06:54:55Z** | **stopped** | **stopped** | **0** | **mtime 2026-11-27 23:39:01.54 -0600 = 05:39:01Z** |

The check behind the last row, quoted as `r1` printed it:

```
$ qm status 313                  -> status: stopped
$ qm status 300                  -> status: stopped
$ pgrep -c qemu-system-x86       -> 0     (exit 1 when nothing matches, which is the measured state)
$ stat -c "%n %y %s" /root/hobbyos5090/disk.img /root/hobbyos5090/serial.log
/root/hobbyos5090/disk.img    2026-11-27 23:39:01.541392178 -0600 1073741824
/root/hobbyos5090/serial.log  2026-11-27 23:39:01.536392179 -0600 74995
```

Those mtimes are the `rm68` boot of `t_97b8a05c`; nothing in this spine wrote
them, which is what makes "zero GB202 boots" a measurement rather than a claim.

---

## 9. What this proves, what it does not, and the acceptance line

**Proves (all MEASURED or SOURCE as cited above):** the `/dev/nvshim` ABI can mint
a channel, a VA space and an engine class object against the mock RM; each handle
is assigned by the RM's counter and returned to the client (`ch=1`, `vm=1`,
`handle=1`); a VA bound to a memory object resolves, through the RM's own walk, to
a real physical address with a real extent, and that address names the object's
bytes; holes, out-of-range VAs, unowned handles and bad ops are named refusals
with visible rows; the stock DRM face shows the same channel object and the same
bind read-back; both unit tiers are green on both architectures; every quoted row
carries `mock=1` and was re-checked at its cited line.

**Does not prove:** anything about GB202, GSP-RM, firmware, WPR2, VRAM or L3. No
`NV_VGPU_MSG_*` is sent, no firmware handle exists, the physical address is host
pool RAM, and the stock client still stops at
`nvif new refused class=0xc7b5 … need=gsp_rm`. The firmware claim stays where the
wall card left it — `MAILBOX0=0x0B`, no peer — and this document is **not**
evidence for it.

**Acceptance, bullet by bullet, with the one gap named:**

| card's acceptance bullet | status |
|---|---|
| `docs/gb202-rm-objects.md` exists | yes — this file |
| all three handles quoted from logs | yes — `ch=1` (§3.1), `vm=1` (§4.1), `handle=1` (§5.1), each with log file + line |
| all message ids named | yes — `NVSHIM_IOC_*` + DRM sub-numbers with `file:line` (§6); `NV_VGPU_MSG_*` is named as **none**, which is the only honest answer |
| the VA physical address read back | yes — `phys=0x209686000` (intel) / `phys=0x8ef46000` (ARM), §4.1 |
| the measured layout number **with its measurement method**, measured on GB202 | **not satisfied for GB202, and stated rather than faked**: these objects consume no layout number (§7.1); the only register-measured layout number is AD102's `gpc=11 tpc=64 src=register` (§7.2, with its method), and no GB202 boot log in this tree carries that row. The GB202 read is one read-only boot away, and no boot is authorized today. |
| the part identity (GB202, 41:00.0, VM 313) | yes — stated once at the top, re-measured 06:54:55Z |
| clean closing state for each boot | yes — §8, all runs, plus the live re-check |
| if any of the three objects failed to allocate, say so and do not complete | **all three allocated** (as mock objects); nothing failed, so nothing is hidden |

Step 6 of the body ("every object came from firmware") is superseded by the
owner's 05:44Z re-scope and is answered in §1: every object is a mock object, and
no mock object is presented as a firmware allocation.

---

## 10. Evidence for Part I

Attached to card `t_c5453775`'s three siblings and to the wall card (paths are in
the board's attachments directory):

| what | where |
|---|---|
| channel rows + report | `t_17ca89e5/unit-tier-intel-chan.log`, `…/unit-tier-arm-chan.log`, `…/REPORT-shim-mock-chan.md`, `…/check-quotes-chan.py`, `…/nv_shim-channel-mock.patch` |
| VA-space rows + report | `t_b9c4df6d/unit-tier-intel-vm.log`, `…/unit-tier-arm-vm.log`, `…/unit-tier-intel-merged.log`, `…/unit-tier-arm-merged.log`, `…/SWARM-REPORT-SHIM-MOCK-VM.md`, `…/check-quotes-vm.py`, `…/check-output-vm.txt`, `…/nv_shim-vm-resolve.patch`, `…/nouveau-r535-vmm.c` |
| engine-class rows + report | `t_7cf2e538/unit-tier-intel-ec.log`, `…/unit-tier-arm-ec.log`, `…/unit-tier-intel-merged.log`, `…/unit-tier-arm-merged.log`, `…/SWARM-REPORT-SHIM-MOCK-ENGINE.md`, `…/check-quotes-engine.py`, `…/check-output.txt`, `…/nv_shim-engine-class.patch` |
| the only register-measured topology (AD102) | `t_78a50be7/tpcba.log`, `…/tpcba.record`, `…/nv-topology-measured.md` |
| GB202 part identity + closing state | read-only `qm` / `lspci` / `stat` at 2026-11-28T06:54:55Z, quoted in the header and §8 |
| the parked wall's GB202 rows (Part II) | `t_a769beaa/5090-gb202fsp-rm{50…65}.log`, `…/KEY-ROWS-gb202-rm-objects.txt` |

---

## Part II — the parked wall record: every FMC / boot-image / ACR blob reachable, and the two abort points

*Part II is the wall card's record (`t_a769beaa`), kept here unchanged so its
section numbers still resolve — `SWARM-REPORT-GB202-TIERA.md` cites §13.5, and
`docs/gb202-rm-parked.md` and `docs/nv-ladder-repoint.md` point at the same
record through the card's attachment copies. Its numbering is its own (§0–§13),
not Part I's.*

**Editorial note, added 2026-11-28 with Part I:** Part II's §0 closing sentence —
"No channel, VA space or engine class exists, because GSP-RM has still never
started" — remains true of **firmware** objects, and is superseded for **mock**
objects by Part I of this file. Nothing else in Part II was edited.

Card `t_a769beaa`, reopened by the owner on 2026-11-27 (**R4**, `docs/nv-plan.md`
§2.13.1). The reopening's first step was a **zero-boot survey** (§1–§10); the
round after it spent **2 boots** on the one variable that survey named (§11:
`615.78.08`, then the same state with the FSP's CMS2 window added), and the
round after **that** spent **1 boot** on the last one-field difference from the
vendor's 615 stock (§12: `nonWprHeapSize`). Every hardware row is quoted from a
log and labelled with its boot; every static fact names its source.

**Part discipline.** Every hardware row is **GB202, `41:00.0`, VM 313**. No AD102
number is used as a GB202 number. Closing state of this run: VM 313 `stopped`,
VM 300 `stopped`, `pgrep -c qemu-system-x86` = **0** on r1 and on this worker.

---

## 0. The answer, in six lines

1. **Nothing newer than the 620.70.10 set exists.** `620.73.04` is the newest
   public tag by version (`open-gpu-kernel-modules`, commit `a4b95f6a2d`; its tag
   commit, 2026-11-18, is even older than `620.70.10`'s 2026-11-23, which does
   not matter — see the next sentence). Its GB202 FMC and boot-image bindata are
   **byte-identical** to `620.70.10`'s: file sha256 `f36554a8…` / `1053c2dc…`,
   decoded FMC image sha256 `55042efa…`, boot image `fc42d268…`. So does
   `620.70.06`. A boot spent hunting an FMC newer than 620 buys nothing.
2. **The whole reachable corpus is 11 distinct GB202 FMC images and 11 distinct
   boot images**, across every public tag from `570.86.16` to `620.73.04` (§2).
   Only **four FMC images have ever been on the card**: `570.144` (`e4d17650…`,
   199,240 B — the `0x0B` ladder), `580.178.04` (`cea2190c…`, 215,624 B — `0x5`
   once on the pre-FRTS COT, then **`0x0B`** on the `0x0B` COT, §13),
   `615.78.08` (`5b74fefd…`, 287,304 B — `0x824`, §11) and `620.70.10`
   (`55042efa…`, 287,304 B — `0x824`).
3. **The abort point follows the FMC image, not the artifact set.** With the same
   host state (COT shape, boot-args page, GSP-RM image/signature and every
   descriptor field except the bootloader geometry that the paired image
   carries), the 570 FMC ends `0x0B` and the 620 FMC ends `0x824` at 150 ms
   (§5); **the 615.78.08 FMC, with its own boot image, also ends `0x824` at
   150 ms** (§11) — so `0x824` belongs to the 287 KiB FMC class, not to 620. The
   boot image, the signature/ELF and the ACR cannot move either code; the only
   host-substitutable input that changes the code is the FMC itself. **The
   `580.178.04` FMC — the 215 KiB class, and the image NVIDIA's own
   `nvidia-open-580.178.04` binds for this part — ends `0x0B` with WPR2 armed at
   the exact pair the 570 FMC produces** (§13). The class line is therefore
   bracketed from both sides: 199/215 KiB → `0x0B` with WPR2, 287 KiB → `0x824`
   with WPR2 never armed; the one class member in between is 590's 286 KiB
   (§6 rank 1).
4. **There is no GB202 ACR blob to enumerate.** No `BooterLoadUcode`,
   `BooterUnloadUcode` or `Sec2Bl` for GB100/GB202 exists in **any** of the 15
   tags' generated trees, in linux-firmware, or in either `.run`; GB202 exposes
   no SEC2 window (`0x840000` = `0xBADF4100` 7/7, measured `t_67f09feb`), and its
   FSP is started by BootFSM out of chip reset and **cannot be staged**.
5. **The one boot that was left has been spent, and it answered:** the
   `615.78.08` FMC (`5b74fefd…`) with its own boot image (`37159b4b…`), on the
   COT that yields `0x0B`, ends **`0x824` / `0x14DC = 0x12000824` at 150 ms**
   (§11) — identical to the 620 pair. `0x824` is therefore not a 620 regression;
   it is the 287 KiB FMC class's entry-path abort. Both `WprMeta` fields in
   which our block differed from the vendor's 615 stock are now answered — one
   by the corpus (decorative), one by measurement (§12: inert) — so the abort
   sits upstream of descriptor content. A third boot then spent the survey's
   rank-2 pair — NVIDIA's own `580.178.04` FMC with the boot image every wall
   boot already used — and answered `0x0B` with WPR2 armed (§13), which is why
   the boundary is now 580→590 and no longer 615→620.
6. **The comparison that decides whether the FMC or our host structures is the
   wall needs NVIDIA's own path on this part**: VM 301 (Ubuntu 24.04) with
   **`nvidia-open-580.178.04`** — the closed module refuses a GB202 (measured,
   `docs/nv-r1-host-renderer.md` §1.1) — with `41:00` passed through. That is the
   only success path on this silicon, and its FMC is statically known to be
   `cea2190c…` (§7).

**Acceptance is not claimed and not moved by this document.** No channel, VA
space or engine class exists, because GSP-RM has still never started.

---

## 1. What was searched, and what is reachable on 2026-11-27

The survey treated "reachable" as *every path this lane can read without a GPU
boot*: the worker's disks, r1 as root, the public NVIDIA source tags, the driver
packages (`.run`), linux-firmware, and the guest images.

| source | what is there | what it yielded |
|---|---|---|
| this worker, `/srv/nv5090/work/dl/` | `NVIDIA-Linux-x86_64-570.144.run` (375,742,470 B), `NVIDIA-Linux-x86_64-580.178.04.run` (397,214,534 B), extracted trees `x/` (570), `y/` (580) | the two `.run`s ship **only** `firmware/gsp_ga10x.bin` (570: 63,571,696 B `a8c3ebee…`; 580: 75,028,464 B `3b89a63f…`) and `gsp_tu10x.bin`; no separate FMC, boot or ACR blob |
| this worker, workspace `fw/` | containers `FMC.BIN` 199,820 (`cb59a35c…`, linux-firmware 570.144), `FMCV.BIN` 216,204 (`6adbd92d…`, 580.178.04), `FMC595.BIN` 287,884, `FMC620.BIN` 287,884; decoded images `FMC580.BIN` 215,624 (`cea2190c…`); staged images `BOOTIMG.BIN` 217,088 (`ac068832…`), `BLB.BIN` 200,812 (`d40b48e4…`), `BOOT620.BIN` 290,816 (`fc42d268…`); bindata for 580 and 595 | the exact artifacts the wall's boots staged (§2.2); the 580/595 bindata |
| this worker, `cache/scratch/t620/` | 620.70.10 GB202 bindata (Boot, Fmc, CcFmc, ConcatenatedFMC + descs) | 620.70.10 row of the table |
| r1, as root | `/lib/firmware/nvidia/gb202/gsp/` = `bootloader-570.144.bin` 200,812, `fmc-570.144.bin` 199,820, `gsp-570.144.bin` → `../../ga102/gsp/gsp-570.144.bin`; `pve-firmware` 3.18-7; no `.run`, no `nvidia-open` source tree, no DKMS module | linux-firmware's GB202 directory is the **570.144 set only** — the same bytes already on the card; nothing newer |
| r1, VMs | 313 disk `/root/hobbyos5090/disk.img` (the staged `FMC/BLB/BOOTIMG` set), 301 disk `local-lvm:vm-301-disk-0` (Ubuntu 24.04 + 580.178.04) | 313's staged set is §2.2; 301's 580 firmware is the same 75 MB `gsp_ga10x.bin` already extracted (no need to mount) |
| GitHub `NVIDIA/open-gpu-kernel-modules` | tags back past 550 (two API pages); every tag ≥ `570.86.16` carries `g_bindata_kgspGetBinArchiveGspRm{Boot,FmcGfwProdSigned}_GB202.c`; newest `620.73.04` | the 15-tag matrix in §2.1–§2.2 (files fetched by `raw.githubusercontent`, no API budget spent on blobs) |
| linux-firmware | `nvidia/gb202/gsp/{bootloader,fmc,gsp}-570.144.bin` (r1, above) | 570.144 only |
| GB202 VBIOS / ACR ucode | **no dump exists in the repo** (`docs/nv-vbios-evidence/` holds the 4090's AD102 VBIOS only); the 5090's VBIOS would need a VM 312 ROM-dump boot | not enumerated; and moot — the ACR row in §5.2 and §3 |
| strix / NAS | documented as unreachable from this lane (`docs/nv-r1-host-renderer.md` §1.1: strix answers ping but not a key; NAS copy is `0770 sarah:sarah`) | nothing |

Method note: file-identity comparisons use the **git blob sha** GitHub returns
for `src/nvidia/generated/` at each tag (one API call per tag), so "unchanged
between tags" is a property of the repository, not of my download. Artifact
hashes below are computed locally from the decoded arrays.

---

## 2. The GB202 artifact corpus, decoded

Every row was decoded from the tag's generated bindata with
`decode_bindata.py` (this card's tool, attached): it parses the `_data[]` array
(either `NvU8` byte literals or `NvU64` `0x…ULL` literals, including the
`NV_DECLARE_ALIGNED` form), trims to the declared `DATA SIZE`, inflates a
`DESC` with raw deflate, and parses `RM_RISCV_UCODE_DESC`.

### 2.1 FMC images (`g_gsp_gb20x_dyn_prd_gfw_image.bin`, in `GspRmFmcGfwProdSigned_GB202`)

| tag(s) | image B | image sha256 | header `+4` / `+0x0C` | `hash==SHA384(image)` | ever booted on GB202? |
|---|---|---|---|---|---|
| 570.86.16 | 198,984 | `eccbd503ae2f803cf2b523a15c98b14e97c5c1fe3928ed9b096dc8ce4178d3b2` | `0x30A00` / `0x25700` | true | no |
| 570.144 | 199,240 | `e4d176500d82d824c1a915202aca89e093c27febf8be1cf1a0705b544c2d6ddb` | `0x30A00` / `0x25800` | true | **yes — 19+ boots, 0x0B** |
| 570.211.01 | 199,240 | `52fb09e72c0d8300c294fa74e9be6d68e0bd2fcea9fa9d8b6deceefcde6d6515` | `0x30A00` / `0x25800` | true | no |
| 575.64.05 | 199,240 | `2e8cd01deb31b5238866da97c5bb26452407247ee17ed0b2db5cd9df7c657a0f` | `0x30A00` / `0x25800` | true | no |
| 580.65.06 | 215,624 | `c62856e9787222fc68c1c41e44e4ba7c022f9aaff3a56622f3437e3382af50ee` | `0x34A00` / `0x25800` | true | no |
| 580.178.04 = 580.196.11 | 215,624 | `cea2190cc8d7f74f65c0bd3dbf1c87377b2360417d866dd24345f9419b76703e` | `0x34A00` / `0x25800` | true | **twice: `rm23`/mode 13, pre-FRTS COT → `0x5`; `rm65`/mode 65, the `0x0B` COT → `0x0B` (§13)** |
| 590.48.01 | 286,792 | `6252aafcdd1a6e45dcce8beb6f7f7475ed706d470e57dca56728f9f41efaf0bf` | `0x46000` / `0x36E00` | true | no |
| 595.104.02 = 595.112.13 | 287,304 | `d7174df2c31d9e7f732fcc6176de233e8716dbc6e002be99bfcab497a986ed6a` | `0x46200` / `0x37000` | true | no |
| 610.57.04 | 287,304 | `17a15ce8f0e697f83d6d59ee48dc7beb9f8574ad6206d6956564c33fe5584ce0` | `0x46200` / `0x37000` | true | no |
| 615.78.08 | 287,304 | `5b74fefd42fc7b5f2e30c03e7558631d76899d9c6237ee9115c4e0829c63548b` | `0x46200` / `0x37000` | true | no |
| 620.70.06 = 620.70.10 = 620.73.04 | 287,304 | `55042efa5e02ded619105c80ae8b110d54ab5c3af22702419c8b0057bc19eef6` | `0x46200` / `0x37000` | true | **yes — 3 boots (`fh620`, `fh60b`, `fh62`), 0x824** |

All eleven carry the **same production public key**, `048bf118bbeb9c0f…`, and
every `UCODE_HASH` equals `SHA-384(image)` (the FMC is the only GB202 archive
that has a published hash). The header's `+0x0C` word is the **monitor code
size** and `+4` the image's total extent; they must agree with the paired boot
image's `RM_RISCV_UCODE_DESC` (§2.4), which is a free pairing check.

### 2.2 Boot images (`g_gsp_gb20x_dyn_prd_image.bin`, in `GspRmBoot_GB202`)

| tag(s) | image B | image sha256 | desc | code / data / manifest | ever staged on GB202? |
|---|---|---|---|---|---|
| 570.86.16 | 200,704 | `c3016bcff080784d1567029ef15128c2c41a7db4b1701e729685cf9a9a4a459b` | v5 (84/28) | `0xB200` / `0x25700`, data `0xA00` | no |
| 570.144 | 200,704 | `8b8cf1a1067c6f9d0cd966ca9553f9a6894a71b69607171a4c2c98e62e5ead4b` | v5 (84/28) | `0xB200` / `0x25800`, data `0xA00` | no (this is the **bootloader** geometry; `BLB.BIN` wraps it) |
| 570.211.01 | 200,704 | `df2ed9060e1ea253812afb183504a1088d728b3c81e67d44d0bd764e4174ba67` | v5 | `0xB200` / `0x25800`, data `0xA00` | no |
| 575.64.05 | 200,704 | `ec081adc7fef0db76f28e3a7dada13b60db542429187896ccb0970ee163ddc38` | v5 | `0xB200` / `0x25800`, data `0xA00` | no |
| 580.65.06 | 217,088 | `2fe1c6c232a014293cd92ccd6d43bc100b95a8cec36481924aa9097d84285e6c` | v5 | `0xF200` / `0x25800`, data `0xA00` | no |
| 580.178.04 = 580.196.11 | 217,088 | `ac0688326c1ea8aae2da26d785e7091bedb9a45565b03543a479ed05db0ba367` | v5 | `0xF200` / `0x25800`, data `0xA00` | **yes — every wall boot staged this file as `BOOTIMG.BIN`, and `rm65` staged it with the 580 FMC it belongs to** |
| 590.48.01 | 286,720 | `e1265e10912cc87a77e91944d162dd43294576e1ec0c545a03851831607652de` | v6 (92/28) | `0xF200` / `0x36E00`, data `0xA00` | no |
| 595.104.02 = 595.112.13 | 290,816 | `eb8d7c1f5ccbcff006a3f68031e973e2cca0853eeba5c4cbcfbc89dba8f36cfe` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 610.57.04 | 290,816 | `146a49168cae2b385d8ae7a79fd11c503004c070550dbca651967bddee0ed0c2` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 615.78.08 | 290,816 | `37159b4b7ce28066bbf83d11d50abff8e26722eb6bc2825290c14944a7c6aef0` | v6 | `0xF200` / `0x37000`, data `0xA00` | no |
| 620.70.06 = 620.70.10 = 620.73.04 | 290,816 | `fc42d2683f131df161c3afb1fcf9895b9fca8661e86234f3283fdcaacea3c4c1` | v7 (96/29) | `0xF200` / `0x37000`, data `0xA00` | **yes — `fh62` staged it as `BOOT620.BIN`** |

Two facts that correct a loose reading of the earlier rounds:

* **`BLB.BIN` is the 570.144 bootloader container**: its `nvfw` header is
  `magic 0x10de, ver 1, size 0x31100, hdr 0x18, data 0x6c + 0x31000`, and the
  200,704-byte image at `0x6c` is exactly the 570.144 `GspRmBoot_GB202`
  payload (`8b8cf1a1…`, full-image match checked). Its inner desc is the one
  the kernel quotes (`code@0xB200 size 0x25800`), so the 570-generation archive
  is the **bootloader image**, while 580+ ships the **GSP-RM boot image** at
  `code@0xF200`.
* `rm55`'s `0x1C` is therefore *consistent with* the 570 bootloader container
  being offered where a GSP-RM boot image was expected; the row does not by
  itself prove which check wrote `0x1C` (the same code answers descriptor-field
  violations), so this is an inference, not a second measurement.

### 2.3 Provenance correction: which driver ships which boot image

This is new and it changes a label, not a row. **The load-bearing check is the
staged file's own sha256, not a fingerprint inside the module:** `fw/BOOTIMG.BIN`,
the file every wall boot handed the FSP, is `ac0688326c1ea8aa…`, which is the
**580.178.04** boot image (§10). A module search alone cannot establish which
image a driver "ships": a 16-byte search finds the **container-layout** copy
whose first `0xA00` bytes are the manifest and whose remainder is *not* the
image, and it is easy to mistake that header match for body identity.

Measured on both closed modules (`nv-kernel.o_binary`, 570.144 = 98,164,824 B,
580.178.04 = 112,677,456 B), each image searched by its first 16 bytes, every
hit then compared **whole**:

| image | module | container-layout copy (header only) | byte-identical copy |
|---|---|---|---|
| boot `8b8cf1a1…` 570.144 (200,704, code `0xB200`) | 570.144 | @`0x2dbaea8` — manifest `0…0xA00` equal, diverges at `+0xA00` (module zeros) | **@`0x2f6f0e0`**, 200,704/200,704 B equal |
| boot `ac068832…` 580.178.04 (217,088, code `0xF200`) | 580.178.04 | @`0x36df1c8` — manifest equal, diverges at `+0xA00` | **@`0x3a17840`**, 217,088/217,088 B equal |
| FMC `e4d17650…` 570.144 | 570.144 | @`0x2e14800` — diverges at `+0x48` | **@`0x2d8ae60`** |
| FMC `cea2190c…` 580.178.04 | 580.178.04 | @`0x379cd60` — diverges at `+0x48` | **@`0x36ab180`** |

and neither module contains the other generation's image at all (0 hits). So the
**217,088-byte `BOOTIMG.BIN` that every wall boot staged is the 580.178.04
image** — a byte-identical copy of it is in the 580 module and none of it is in
the 570 one — and the 570.144 driver's own boot image (200,704, `8b8cf1a1…`,
`code@0xB200`) **has never been on the card**. The kernel comment at
`src/kernel/nv_fsp_cot.c` ("the vendor's dyn_prd image (0x34A00 content,
code@0xF200) the 570 driver ships") is **not supported by the bytes**: the 570
module embeds the 0x30A00 image, the 580 module the 0x34A00 one. No measured row
changes — the rows already name the size and the container — but the earlier
reports' phrase "the 570 boot image" for the 217,088-byte file is a
mislabelling, and it means the **true 570.144 pair has never been run** (§6,
rank 5).

### 2.4 The static pairing rule (new, zero-boot)

The FMC image header's `+0x0C` code size equals the paired boot image's
`monitorCodeSize` in **every** generation:

| class | FMC `+0x0C` | boot desc `monitorCodeSize` | tags |
|---|---|---|---|
| 199 KiB | `0x25700` / `0x25800` | `0x25700` / `0x25800` | 570.86 / 570.144–575 |
| 215 KiB | `0x25800` | `0x25800`, code at `0xF200` | 580 |
| 286 KiB | `0x36E00` | `0x36E00` | 590 |
| 287 KiB | `0x37000` | `0x37000` | 595, 610, 615, 620 |

So a *legitimate* one-variable FMC swap also stages that FMC's own boot image
(the `fh62` pair already did this for 620 and still got `0x824`; the geometry was
right, which is why the negative is informative).

---

## 3. "Newer than 570 and the 620.70.10 set": the proof of nothing new

The public tag list (`/tags?per_page=100`, two pages, down to `550.40.85` —
older than any GB202-capable tag) was read whole.
The GB202-relevant tags, their commit dates, and the git blob shas of the two
GB202 bindata:

| tag | commit date | `GspRmBoot_GB202` blob | `GspRmFmcGfwProdSigned_GB202` blob |
|---|---|---|---|
| 570.86.16 | 2025-01-30 | `631d347748a4` | `7206a8bdb750` |
| 570.144 | — | `a58b9899ef21` | `af135475fd87` |
| 570.211.01 | 2026-01-13 | `0c6f6c44655a` | `be18c302217c` |
| 575.64.05 | 2025-07-22 | `f0e397e7c9ab` | `7d904fa5b3a7` |
| 580.65.06 | — | `73158c175249` | `1d00cc123f67` |
| 580.178.04 | 2026-08-03 | `2cb97d9fe513` | `f0bc104f4c4d` |
| 580.196.11 | — | `2cb97d9fe513` | `f0bc104f4c4d` (same) |
| 590.48.01 | 2025-12-18 | `99e7e8facb41` | `9d4d3c4078b5` |
| 595.104.02 | 2026-09-22 | `3ec9002e1cf0` | `af6cedb07d44` |
| 595.112.13 | — | `3ec9002e1cf0` | `af6cedb07d44` (same) |
| 610.57.04 | 2026-08-03 | `20c3a76fbe04` | `03b00601bb8d` |
| 615.78.08 | 2026-10-07 | `2b09b1e7eb9f` | `8d978b7e4a64` |
| 620.70.06 | 2026-11-05 | `e64a54452c90` | `5ae49fb706e9` |
| 620.70.10 | 2026-11-23 | `e64a54452c90` | `5ae49fb706e9` (same) |
| **620.73.04** | 2026-11-18 (tag commit) | **`e64a54452c90`** | **`5ae49fb706e9`** (same) |

**The newest tag moves no GB202 byte.** `620.70.06`, `620.70.10` and `620.73.04`
share both blobs; the decoded images are identical (`55042efa…`, `fc42d268…`),
and the local 620.70.10 files are byte-identical to the 620.73.04 downloads
(sha256 `f36554a84612654e…` and `1053c2dc1127ff2a…`). Two other pairs are also
identical and cannot be re-booted as if new: `580.178.04 == 580.196.11`,
`595.104.02 == 595.112.13`.

Also checked at `620.73.04` (`src/nvidia/generated/`, 546 entries, one API read):

* **No `BooterLoadUcode` / `BooterUnloadUcode` / `Sec2Bl` for GB100 or GB202.**
  The booter set is `{TU102, TU116, GA100, GA102, AD102}`; `ksec2GetBinArchiveBlUcode`
  is TU102-only. The same is true of the 14 other tags' listings (`grep` over all
  15 `gen/*.json`: **0** GB-class booter files in every one).
* **No GB202 FSP image.** The only `fsp` entries are `g_kern_fsp_nvoc.c`,
  `g_fsp_events_nvoc.c` and `g_bindata_spdmGetBinArchiveFspL1Certificate_IMPL.c`.
  `kern_fsp_gb202.c` (620.73.04, and byte-identical in 620.70.10) still says:
  *"In GB202, Bootfsm triggers FSP execution out of chip reset."*

That is the complete answer to "anything newer": **the FMC/boot/ACR corpus is
closed at 620.70.10 for GB202**. What remains unmeasured is a *class*, not a
"newer" artifact: `590.48.01` (the only image class between the two measured
ends), its 287 KiB neighbours `595`/`610`, and the true 570.144 same-tag pair
(§6).

---

## 4. The 620 `UCODE_DESC_PROD`, re-decoded (confirming `t_320e84e5`)

The 620.70.10 boot image's desc is **not** a 32-byte tag: it is 4 `NvU64`
literals holding a **29-byte raw-deflate stream** of a **96-byte
`RM_RISCV_UCODE_DESC` version 7**. This run's decoder reproduces the parent's
result from both 620 tags, byte for byte:

```
stored 96 B (compressed size 29), inflated 96
+0x00 07 00 00 00   version 7
+0x20 00 0A 00 00   manifestOffset 0      (manifestSize 0xA00 at +0x24)
+0x28 00 0A 00 00   monitorDataOffset 0xA00
+0x2C E8 00 00 00   monitorDataSize 0xE800 (59,392)
+0x30 F2 00 00 00   monitorCodeOffset 0xF200
+0x34 70 03 00 00   monitorCodeSize 0x37000 (225,280)
+0x38 01 00 00 00   bIsMonitorEnabled 1
```

Image 290,816 B (`0x47000`): manifest `0 / 0xA00`, monitor data `0xA00 /
0xE800`, monitor code `0xF200 / 0x37000`, then 3,584 B of zeros — the three
regions tile the front of the file (quoted from `t_320e84e5` §1.2, re-checked
against the image here). The **570 desc is the same three offsets** with
`monitorCodeSize 0x25800`; what changes at 620 is the *size* of the image and the
struct's trailing fields (`bIsSmp`, `bIsPlicEnabled`, `numDebugBuffers`), not the
placement the FMC consumes. `kgspPopulateWprMeta_GH100` reads exactly
`monitorCode/Data/manifestOffset`, which is why the earlier "unpublished offsets"
worry was a decoder problem, not a missing field.

---

## 5. The two abort points, and which artifacts can move each

### 5.1 What the two points are, quoted

**`0x0B` — the 570.144 FMC's terminal code** (`t_0936a17f` `rm41`, GB202 VM 313):

```
[NV-FSPCOT] ttl t_s=27 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 statq_d0=0x0000000000000000
[NV-FSPCOT] ttl t_s=28 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 statq_d0=0x0000000000000000
```

**`0x824` — the 620.70.10 FMC's code** (`t_320e84e5` `fh60b`, GB202 VM 313):

```
fmc image bytes=287304  file=FMC620.BIN fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000012000824 … variant=1 partition=1 err=0x0000000000000824 info=0
```

`fh60b` staged the 217,088-byte boot image the wall always used (the 580.178.04
image, `ac068832…`; `t_320e84e5` calls it the 570 image — see §2.3) with the 620
FMC; `fh62` staged the 620 boot image with it. Both: `0x824` at 150 ms,
`0x14DC = 0x12000824`. So the boot image does not select this code either.

### 5.2 The rule the corpus establishes

> **With the host state equal in shape and in every field the host writes, the
> FMC image selects the abort point. The boot image, the GSP-RM signature, the
> radix3 ELF and the ACR are all downstream of, or absent from, that choice.**

"Equal in shape" is the honest form of this document's earlier "byte-identical",
which was false for the rows it cited and is withdrawn. Two things necessarily
move between any two boots and are not inputs:

* **the sysmem addresses.** The COT's three buffers and the boot image are
  allocated by size, so a different image size (or just a different boot) returns
  different addresses. Measured on GB202 VM 313:

  | FMC on the card | image B | `gspFmcSysmemOffset` / `frtsSysmemOffset` / `gspBootArgsSysmemOffset` |
  |---|---|---|
  | 570.144 (`fh58`, `rm41`) | 199,240 | `0x20C60E000` / `0x20C63F000` / `0x20C73F000` |
  | 580.178.04 (`rm65`) | 215,624 | `0x20C616000` / `0x20C64B000` / `0x20C74B000` |
  | 615.78.08 (`rm63`/`rm64`) | 287,304 | `0x20C65E000` / `0x20C6A5000` / `0x20C7A5000` |
  | 620.70.10 (`fh60b`) | 287,304 | `0x20C63A000` / `0x20C681000` / `0x20C781000` |

  The 615 and 620 rows make the point: same image size and same boot-image size,
  **different addresses in all three fields**, and the same `0x824`;
* **the bootloader geometry**, when the FMC changes *class*: the FMC header's
  `+0x0C` code size must equal the paired boot desc's `monitorCodeSize` (§2.4),
  so 570→615 also moves `sizeOfBootloader` `0x35000`→`0x47000`, the descriptor's
  `bootimg` bus address and the COT addresses (§11.1). The boot-image axis is
  controlled separately: `fh62` (620 FMC + 620 image) is counter-identical to
  `fh60b` (620 FMC + the 580 image), and `rm56` (570 image address unmapped)
  still ends `0x0B`.

What is genuinely held fixed across a pair is what the host *writes*: the COT
shape (`frtsVidmemOffset` `0x4000000` / size `0x100000`), the `GSP_FMC_BOOT_PARAMS`
page, the `WprMeta` size/heap fields, and the GSP-RM image/signature.

Evidence for each clause:

| artifact | can it move `0x0B`? | can it move `0x824`? | the row |
|---|---|---|---|
| **FMC image** | **yes** — 570.144 and 580.178.04 are `0x0B`; 615.78.08 and 620.70.10 are `0x824` | **yes** | `rm41`, `fh58`, `rm65`, `rm63`, `fh60b` |
| **boot image** **content/address** | no | no | `rm56` (address unmapped → same `0x0B`); `fh62` (620 image → same `0x824`) |
| **boot image** **container form** | no — it changes a *different* code | no | `rm55` (`BLB.BIN` → `0x1C`) |
| GSP-RM **signature** | no | no | `rm53` (`0x1D`) |
| radix3 **ELF** | no | no | `rm54` (`0x02`) |
| **ACR / SEC2** | no — no blob exists, no window accepts one | no | `t_67f09feb`: `0x840000` = `0xBADF4100` 7/7, gate `0xAD00BC` = `0xFF` |
| **FSP** | no — in-chip, BootFSM-started, not stageable | no | `kern_fsp_gb202.c`, no FSP image in any tag |
| descriptor / COT **fields** | no (each measured) | no (each measured) | `docs/gb202-rm-parked.md` §5, 20 rows |

### 5.3 The gap that is left in the corpus

**Four** of the eleven FMC images have been on the card:

* `570.144` → `0x0B` (19+ boots, the whole `t_a769beaa`/`t_0936a17f` ladder);
* `580.178.04` → `0x5` once (`rm23`, mode 13, vendor FMC + vendor boot image,
  before `rm29` found that the COT needs a non-zero FRTS pair), and **`0x0B`**
  once on the corrected COT (`rm65`, mode 65, §13);
* `615.78.08` → `0x824` (3 boots: `rm63`, `rm63b`, `rm64`);
* `620.70.10` → `0x824` (3 boots: `fh620`, `fh60b`, `fh62`).

Measured, the class rule is: **199–215 KiB → `0x0B`, with WPR2 armed at
`0x7E5A200/0x7F1EE00` under the vendor heap pair; 287 KiB → `0x824` at 150 ms,
with WPR2 never armed.** What is left unmeasured:

* **`590.48.01` (286,792 B, `0x36E00`) — the only class member between the two
  measured ends** (§6 rank 1);
* `595.104.02` and `610.57.04` — 287 KiB, the same class as 615/620;
* `580.65.06` — the same 215 KiB class **and the same `0x25800` code size** as
  the measured `580.178.04`, so it cannot answer anything `rm65` did not;
* `570.86.16`, `570.211.01`, `575.64.05` — the 199 KiB class, same geometry as
  `570.144` (`0x86.16` differs only in its `0x25700` code size);
* the **true 570.144 same-tag pair** (§6 rank 3) — a boot-image control, not a
  class question.

---

## 6. Ranked candidates, with the boot each costs

A candidate is worth a boot only if it can change one of the two codes at fixed
host inputs. Ranking is by *new information per boot*, not by version. **Ranks 1
and 2 of the survey version are spent** — `615.78.08` (§11, `0x824`) and
`580.178.04` (§13, `0x0B`) — so the table below is the reconciled ranking after
both, and §13 spends rank 1.

| # | candidate | pair to stage | boots | which abort it can move | why it is ranked here |
|---|---|---|---|---|---|
| 1 | **590.48.01** | FMC `6252aafc…` (286,792 B) + its own `GspRmBoot` `e1265e10…` (286,720 B, `0xF200`/`0x36E00`) | **1** | `0x824` or `0x0B` | **the only class member between the two measured ends**: 199/215 KiB answers `0x0B` with WPR2 armed (`fh58`, `rm65`), 287 KiB answers `0x824` with WPR2 never armed (`rm63`, `fh60b`). A 286 KiB build decides whether the abort arrived with the first `0x36E00` image class or only with the later 287 KiB builds |
| 2 | **610.57.04** | FMC `17a15ce8…` + `BOOT610` `146a4916…` (290,816 B, `0xF200`/`0x37000`) | 1 | re-measures `0x824` | an earlier 287 KiB build (2026-08-03); informative **only if 590 answers `0x0B`**, where it would place the 286→287 transition inside the class |
| 3 | **570.144 same-tag pair** | FMC `e4d17650…` + the **570.144** boot `8b8cf1a1…`, `code@0xB200` | 1 | `0x0B` (control) | the true 570 driver pair has **never** been on the card (§2.3): every wall boot used the 580 boot image at `0xF200`. Expected to reproduce `0x0B` (`rm56` says the boot image is read after that point), so it is a control, not a lever; it needs the boot image and the `bootloaderCodeOffset` changed together |
| 4 | **580.65.06** | FMC `c62856e9…` (215,624 B) + `BOOT580.65` `2fe1c6c2…` (217,088 B, `0xF200`/`0x25800`) | 1 | nothing new | same 215 KiB class **and the same `0x25800` code size** as the measured `580.178.04`; it can only re-measure that class. **Do not spend** — this is the pair §12.5 used to name, and the naming was wrong on three counts (see §12.5) |
| 5 | 570.86.16, 570.211.01, 575.64.05, 595.104.02 | — | — | — | same class and geometry as a measured member (199 KiB with `0x25800`, or 287 KiB with `0x37000`); no question left that rows 1–2 do not answer |

**Already spent (do not re-run):** 615.78.08 (§11, `0x824`), 580.178.04
(§13, `0x0B`), 620.70.10 (`t_320e84e5`), 570.144 (the `0x0B` ladder).

**Do not spend:** 620.70.06 / 620.70.10 / 620.73.04 (identical to the FMC already
booted), 580.196.11 (= 580.178.04), 595.112.13 (= 595.104.02), any "newer" FMC
(none exists), any booter/ACR blob (none exists), any FSP (not stageable), and
every host-field variation in `docs/gb202-rm-parked.md` §5.

---

## 7. The single comparison that would distinguish them

### 7.1 Primary: one variable at a time, and the one class point left

Hold the **host state** — the COT shape (FRTS-in-FB 64 MiB/1 MiB, `frts_size`
0), the boot-args page (`GSP_FMC_BOOT_PARAMS`), the descriptor's size/heap
fields and the GSP-RM image/signature — equal to the state that yields `0x0B`
(mode 58/59's vendor heap pair), and vary **only the FMC generation**. A
generation is an FMC image **plus its paired boot image**: the FMC header's
`+0x0C` code size must equal the boot desc's `monitorCodeSize` (§2.4), so the
pair moves together and some descriptor bytes necessarily move with it — for
570→615 that is `sizeOfBootloader` `0x35000`→`0x47000`, the descriptor's
`bootimg` bus address, and all three COT sysmem addresses (§11.1). The
boot-image axis is controlled separately and does not need to ride along:
`fh62` (620 FMC with the 620 boot image) is counter-identical to `fh60b` (620
FMC with the 580 image), and `rm56` (570 image address unmapped) still ends
`0x0B`. The corpus now has **three measured class points**:

* 570.144 (199,240 B) → `0x0B`, WPR2 armed (19+ boots, `fh58` the clean control);
* 580.178.04 (215,624 B) → `0x0B`, WPR2 armed at the same pair (`rm65`, §13);
* 615.78.08 / 620.70.10 (287,304 B) → `0x824` at 150 ms, WPR2 never armed
  (`rm63`, `fh60b`).

The missing point is **590.48.01** — 286,792 B, code size `0x36E00`, the only
image class between the `0x0B` end and the `0x824` end, with its own
`GspRmBoot` (`e1265e10…`, 286,720 B, `0xF200`/`0x36E00`). One boot decides:

| 590 outcome | what it proves |
|---|---|
| `0x824`, `0x14DC` formatted | the abort arrived with the **first `0x36E00` class** (590), not with the 287 KiB builds; the class line is the FMC image size/geometry, and 595/610 need no boot |
| `0x0B`, WPR2 armed | the abort arrived **inside** the 286→287 KiB step; 610.57.04 (§6 rank 2) then places the transition, since 610 and 615 share the 287 KiB class |
| anything else (a new code, or `0x0`/`INIT_DONE`) | the class story is incomplete or the wall moved — an **R2** event if `statq_written=1 linked=1 initdone=1` |

**What it needs:** the HobbyOS 5090 image in **VM 313**, booted with
`tools/nv/gb202fsp-boot.sh <img> <tag>` (one boot, `STOP_AFTER_VERDICT=1`,
`STOP_PATTERN='^\[NV-FSPRM\] verdict='`), the 590 FMC container staged as
`/NVIDIA/GSP/FMC590.BIN` with its own boot image as `/NVIDIA/GSP/BOOT590.BIN`
(the kernel's mode 65 pattern: one new `fmc_name` + the default `BOOTIMG.BIN`
geometry replaced by `0xF200`/`0x36E00`; `FMC.BIN`/`BOOTIMG.BIN` stay staged for
the older modes). **No kernel module is involved** — our kernel is the driver.
The §8 readbacks are already in the sampler (§13), so nothing else is needed
before the boot.

**Guest/module flavour for the control (7.2):** the Linux guest is **VM 301**
(Ubuntu 24.04, DKMS), and the module must be **`nvidia-open-580.178.04`** — the
closed `nvidia.ko` **refuses a GB202 by design**, measured 2026-11-27:
`NVRM: … requires use of the NVIDIA open kernel modules`, `RmInitAdapter failed!
(0x22:0x56:897)` (`docs/nv-r1-host-renderer.md` §1.1). A 5090 baseline built on
the closed module fails before it can say anything about the FSP.

### 7.2 The control only NVIDIA can give: its own path on the same part

The primary comparison can tell you *where* the abort changes; it cannot tell you
whether **any** of our FMCs is the one this part expects. The only success path
on GB202 is NVIDIA's own driver, and the comparison is a single guest boot:

* **guest:** VM 301, Ubuntu 24.04, `41:00` passed through (`qm set 301 --hostpci0
  0000:41:00,pcie=1`), `nvidia-open-580.178.04` (not the closed module);
* **capture, at the same offsets this kernel reads:** `NV_PGSP_FALCON_MAILBOX0/1`,
  `NV_PBUS_ZB_SW_SCRATCH_GSP_FMC_ERROR` (`0x14DC`), `NV_THERM_I2CS_SCRATCH`
  (`0xAD00BC`), and `NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0..3)`, on the success
  path and on any failure;
* **the known answer to "which FMC":** the 580 open module binds
  `GspRmFmcGfwProdSigned_GB202` = **`cea2190c…` (215,624 B)** — the image §13
  staged and measured (`0x0B`, WPR2 armed). So the "is a 215 KiB FMC able to run
  on this part at all" half is already answered **yes** (§13.3): the FMC executes
  and the FSP lays out WPR2. What the control still decides is the other half —
  whether **NVIDIA's own host structures** get past `0x0B` with that image. If
  they do, the remaining difference is in the COT/descriptor/args our kernel
  writes; if they do not, the wall is the in-chip FSP's generation and no host
  artifact can move it.

That is the distinguishing experiment, and it is one guest boot plus one HobbyOS
boot against a **known FMC identity**, not another sweep of host fields.

---

## 8. The instrument gap (done, 0 boots)

**Status: closed in §11 and reused in §13.** The six readbacks below are in the
sampler from mode 63 on, so every mode-58/63/64/65 boot prints
`SCRATCH_GROUP_2(0..3)`, `PGSP_MAILBOX(1..3)` and `0x14DC`: they read
`0x9F,0,0,0xA` / `0,0,0` / `0` in `rm65` and were frozen through the `0x0B`
transition (§13.2). The history of the gap is kept because it says *why* the
loop reads what it reads.

NVIDIA's own failure path, `kfspDumpDebugState_GB100` (620.73.04
`src/nvidia/src/kernel/gpu/fsp/arch/blackwell/kern_fsp_gb100.c`), reads **more
registers than this kernel's sampler**:

```
NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0) … (3)
NV_PGSP_FALCON_MAILBOX0 / MAILBOX1
NV_PBUS_SW_SCRATCH_GSP_FMC_ERROR                     (0x14DC)
NV_PGSP_MAILBOX(0) … NV_PGSP_MAILBOX(NV_PGSP_MAILBOX__SIZE_1-1)
FSP microcode version (GFW FSP_UCODE_VERSION), then the CMS2 and DMEM logs
```

Our sampler prints `m0`, `m1`, `0x14DC`, `HWCFG2`, `FALCON_OS/RM`, `rvctl` — but
**not** `SCRATCH_GROUP_2[0..3]` and **not** `NV_PGSP_MAILBOX(1..)`: the kernel
reads `NV_GSP_ENGINE_MAILBOX0` (`0x804`, index 0) but never the rest of that
array. `t_67f09feb` measured the FSP's own scratch at +2 ms
(`scratch2 s0=0x9F s3=0xA`, `ucode_ver 0x44C`) — those are live FSP progress
words, and the `0x0B` transition has never been sampled against them. Adding six
readbacks to the 1 Hz loop costs **0 boots** and is the cheapest chance to give
`0x0B` a name (the FMC's code is encrypted; the FSP's scratch is not).

---

## 9. Statically ruled out — do not spend a boot on it

Everything in `docs/gb202-rm-parked.md` §5 still holds (descTarget, `fbSize`,
`frtsSize`, signature/ELF/boot-image identity and address, the boot-image
container, FRTS-in-FB, the heap pair, the 620-pair result, `0x14DC` from a 570
FMC). This survey adds:

| hypothesis | ruled out by |
|---|---|
| an FMC **newer than 620.70.10** exists | `620.73.04` is the newest tag; its two GB202 blobs are identical to `620.70.10`'s (§3) |
| `620.70.06` is a different GB202 artifact | same two blob shas as 620.70.10/620.73.04 |
| `580.196.11` / `595.112.13` are new artifacts | same blob shas as 580.178.04 / 595.104.02 |
| a GB202 **SEC2 booter / ACR ucode** is reachable | absent from 15/15 `generated/` listings (442→546 entries), absent from linux-firmware (`nvidia/gb202/` has `gsp/` only), absent from both `.run`s |
| "the 217,088-byte boot image is the 570.144 one" | the 570.144 `nv-kernel.o_binary` embeds `8b8cf1a1…` (200,704, code `0xB200`); `ac068832…` is in the 580.178.04 module (§2.3) |
| a GB202 **FSP** image is stageable | absent from every tag; `kern_fsp_gb202.c`: BootFSM starts it out of chip reset; the gate `0xAD00BC` is the host's only lever |
| the **boot image** can move `0x824` | `fh62` (620 image, right geometry) == `fh60b` (570 image) == `0x824` |
| the **boot image** can move `0x0B` | `rm56` (address unmapped) == baseline `0x0B` at the same time |
| **`0x14DC`** is readable from a 570 FMC | 0 in all 8 `t_0936a17f` boots; the formatted-error reader/format is 610+/620 |
| a **cross-generation FMC pair** is invalid by geometry | the FMC header `+0x0C` matches the boot desc's code size for every class (§2.4); 570 FMC + 580 boot is code-size-compatible (`0x25800`) |
| the **ACR writes/copies anything** into FB | `rm43`/`rm59b` census: `other=0 meta=0 gfw=0`; 256-byte stride over the WPR2 band, `nz_head=0 nz_all=0` |
| "**`0x824` is 620-specific**" | `rm63`/`rm63b`: 615.78.08 + its own boot image, `cot_error=0`, `m0=0x824`, `0x14DC=0x12000824` at 150 ms (§11.3) |
| the **615 boot image** can move `0x824` | the 615 FMC with the 615 image == the 620 FMC with the 620 image == `0x824`; the boot image is not the variable in either |
| the FSP's **SCRATCH_GROUP_2 / PGSP_MAILBOX(1..3) / CMS2 log** names the `0x824` | `rm63b`: scratch frozen at `0x9F,0,0,0xA`; `PGSP_MAILBOX(1..3)=0`; CMS2 window 48/48 identical pre/post, no text (§11.4) |
| the vendor's 615-stock **`nonWprHeapSize = 0x300000`** moves `0x824` | `rm64`: same pair, same COT, `nonwpr_heap=0x300000` read back from the block, `m0=0x824` / `0x14DC=0x12000824` unchanged at all 13 samples (§12) |
| **`WprMeta.frtsSize = 0x100000`** moves it | not booted: decorative on this path per the corpus the `WprMeta` diff comes from (05.12 "the COT value wins") and our COT already carries 1 MiB (§11.6) |
| the **580.178.04 FMC** moves the code off `0x0B` | `rm65`: same host state, same boot image, WPR2 armed at `0x7E5A200/0x7F1EE00` (**identical** to `fh58`'s 570-FMC pair), `m0=0x0B` with the pair torn down (§13) |
| `0x824` is a property of "the FMC NVIDIA ships for this driver" | the 580.178.04 image is the one `nvidia-open-580.178.04` binds for GB202 (§2.3, §7.2) and it answers `0x0B`, not `0x824` (§13) |
| the **215 KiB class** can reach the wall | `rm65`: WPR2 is armed exactly as with the 570 FMC and the attempt still terminates `0x0B`; `wprm-after untouched=yes` (§13) |

**Not** ruled out: the **590.48.01 class point** (§6 rank 1 — the only image
class between the two measured ends); **610.57.04** as its follow-up; the **true
570.144 same-tag pair** (§6 rank 3, a control); the **meaning of `0x0B`** (not in
`flcnretval.h`, not an FSP `kern_fsp_retval.h` code, and the producing FMC is
encrypted); and the only in-band namer, the **FSP DMEM log over the MCTP
download-log RPC** (`_kfspGatherDmemLog_GB100`). `0x824`'s meaning is likewise
not in the public map; §11.5 places it in the `WprMeta`-parser block. Both
`WprMeta` fields in which our block differed from the vendor's 615 stock are
answered — `nonWprHeapSize` by measurement (§12), `frtsSize` by the corpus
(§11.6).

---

## 10. Method and evidence

Static-only commands, reproducible from this workspace:

```sh
# the tag matrix (one API read per tag, blobs by raw.githubusercontent)
curl -s "https://api.github.com/repos/NVIDIA/open-gpu-kernel-modules/contents/src/nvidia/generated?ref=<tag>"
# the decode (no third-party deps; raw deflate for DESC, hashlib for identity)
python3 decode_bindata.py matrix/*_Boot.c matrix/*_FmcGfwProdSigned.c … > survey.json
# the FMC header geometry check
python3 fmchdr.py    # +4 = image extent, +0x0C = monitor code size
```

Attached to the card: `REPORT-gb202-rm-objects.md` (this file),
`gb202-rm-survey-evidence.tar.gz` (the 15 `gen/*.json` listings, `survey.json`,
the decoded FMC/boot images, `decode_bindata.py`, `fmchdr.py`, `mktable.py`,
`prov23.py`), the full `survey.json`, and for the three boot rounds
`gb202-rm-615-evidence.tar.gz` (§11: `rm63`/`rm63b` logs, `KEY-ROWS-615.txt`,
the mode-63 source diff), `gb202-rm-mode64-evidence.tar.gz` (§12: the `rm64`
log, `KEY-ROWS-64.txt`, the mode-64 source diff, `cmp6364.py`, `keyrows64.py`,
`check_rows12.py`, `prov23.py`, `SHA256SUMS`) and
`gb202-rm-mode65-evidence.tar.gz` (§13: the `rm65` log, the `fh58` control,
`KEY-ROWS-65.txt`, the mode-65 source diff and the pre-65 snapshot,
`keyrows65.py`, `build65.sh`, `SHA256SUMS`).

Files staged by the wall (for the boots in §11–§13), with this run's hashes:

```
FMC.BIN     199,820  cb59a35c1d4bd1274d7267fd…  (linux-firmware 570.144 container; image e4d17650…)
FMCV.BIN    216,204  6adbd92d63c375aa3dc40ea1…  (580.178.04 container; image cea2190c…)
FMC580.BIN  215,624  cea2190cc8d7f74f65c0bd3d…  (the 580.178.04 decoded image, not a container)
FMC595.BIN  287,884  17057676d36400fdeb7dda5e…  (595.104.02 bindata; image d7174df2…)
FMC620.BIN  287,884  ec87ba3509186302ab194f7a…  (620.70.10 container; image 55042efa…)
BOOTIMG.BIN 217,088  ac0688326c1ea8aae2da26d7…  (580.178.04 boot image — the one field's pair in §13)
BLB.BIN     200,812  d40b48e431d1707dc77af360…  (570.144 bootloader, nvfw container)
BOOT620.BIN 290,816  fc42d2683f131df161c3afb1…  (620.70.10 boot image)
```

**No boot was spent by the survey pass. Nothing on r1 was started, bound, or
installed by it.** Closing state then: VM 313 `stopped`, VM 300 `stopped`,
`pgrep -c qemu-system-x86` = 0 on r1 and on this worker.

---

## 11. The 615.78.08 boot — the one variable, measured (2 boots, 2026-11-27)

This is the boot §7.1 named, run after the review that asked for it. Part in every
row: **GB202, `41:00.0`, VM 313**. Harness `tools/nv/gb202fsp-boot.sh`, one boot
per named hypothesis, `STOP_AFTER_VERDICT=1`, `STOP_PATTERN='^\[NV-FSPRM\] verdict='`.
Two boots were spent: **`rm63`** (the named one-variable boot) and **`rm63b`**
(the same state, plus the FSP's own CMS2 window — hypothesis H2 in §11.4).

### 11.1 What was staged, and what necessarily moved with it

Both artifacts were decoded from the **`615.78.08`** tag's generated bindata
(`GspRmFmcGfwProdSigned_GB202`, `GspRmBoot_GB202`) by the survey's own decoder —
the same sources the §2 corpus table came from, not a re-download of anything new.

| staged file | bytes | sha256 | geometry |
|---|---|---|---|
| `/NVIDIA/GSP/FMC615.BIN` | 287,884 | `ae4e70559f14d9becf7f0c4014a990d0873d174f3af0cc4913484e2cfb3238d5` | container: hash@0x150 == SHA-384(image) ✔, pkey@0x1E0 = `048bf118bbeb9c0f…`, image 287,304 = `5b74fefd…` |
| `/NVIDIA/GSP/BOOT615.BIN` | 290,816 | `37159b4b7ce28066bbf83d11d50abff8e26722eb6bc2825290c14944a7c6aef0` | desc v6: manifest 0/0xA00, data 0xA00/0xE800, code 0xF200/0x37000 |

`FMC615.BIN` was built from the bindata's four blocks with the same
`mkfmc.py` shape the 620 container used (hash 48 / signature 96 / publickey 97 /
image at 0x244), and the container's hash section **equals `SHA-384(image)`** —
checked, not assumed. FMC image `+0x0C` = `0x37000`, the boot desc's
`monitorCodeSize` = `0x37000` (§2.4's pairing rule holds), so the FMC and its
own boot image are one *generation* and move together.

**What is held equal, and what did not have to move.** The GSP-RM image
(63,541,248 B), its 4,096-byte signature, the radix3 tree (33 pages), the boot
args page (`GSP_FMC_BOOT_PARAMS`), the FRTS sysmem buffer, the descriptor's heap
pair (`0x8700000` / `0x220000`), `frtsSize = 0` and the COT's
`frtsVidmemOffset = 0x4000000` / `frtsVidmemSize = 0x100000` are unchanged from
the `0x0B` state (mode 58/59). The FMC generation swap necessarily moved three
things, and the logs show them:

| field | `fh58` (570 pair) | `rm63`/`rm64` (615 pair) |
|---|---|---|
| `sizeOfBootloader` | `0x35000` | `0x47000` |
| descriptor `bootimg` bus address | `0x20C7C6000` | `0x20C82C000` |
| COT `gspFmcSysmemOffset` / `frtsSysmemOffset` / `gspBootArgsSysmemOffset` | `0x20C60E000` / `0x20C63F000` / `0x20C73F000` | `0x20C65E000` / `0x20C6A5000` / `0x20C7A5000` |

Those are consequences of a different pair of files (different sizes, different
allocation), not extra variables: the boot-image axis is controlled separately by
`fh62` vs `fh60b` (identical counters) and `rm56` (unmapped address, same
`0x0B`), and the descriptor's own fields (`code 0xF200`, `data 0xA00`,
`manifest 0`) are identical in both columns.

The earlier sentence here ("the COT, the WPR descriptor, the boot args page …
byte-identical") was wrong and is withdrawn: the descriptor's `bootimg` address
and size and the COT's three sysmem addresses changed with the pair.

### 11.2 The rows

The block was consumed by the FSP — the COT round trip is clean:

```
[NV-FSPRM] start gsp=rm mode=63[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63: FMC615.BIN,BOOT615.BIN)
fmc image bytes=287304 fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
bootimg bytes=290816[NV-FSPCOT]  file=BOOT615.BIN d0=0x000000001156E500 d1=0x000000003996718D d2=0x0000000045AF2B00 d3=0x00000000C74FBC4D code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] cot class=1[NV-FSPCOT] cot cmd_nvdm=20[NV-FSPCOT] cot errorCode=0
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
```

and the terminal state is already there at the first sample and never moves
(`rm63b`; `rm63` is identical — the CMS2 window exists only in `rm63b`):

```
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=63 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000824 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000012000824 radix3=1
[NV-FSPCOT] gsp-shape post off=0x0000000000110040 v=0x0000000000000824
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
```

All eleven samples (`t_ms` 150, 250, 400, 600, 1000, 3000, 6000, 10000, 15000,
21000, 27000) and the two extra ones (40000, 55000) carry the same
`m0=0x824`, `fmcerr=0x12000824`, `wpr2_lo=0x7FFFFE00`, `wpr2_hi=0` — the verdict
row is not a last sample, the state is constant for 55 s.

### 11.3 What this settles

`0x824` is **not 620-specific**. The immediate predecessor of the 620 branch, run
with **its own** boot image and a COT the FSP accepts (`errorCode=0`), emits the
identical `0x14DC = 0x12000824` (variant 1, partition 1, err `0x824`, info 0) and
dies before `t=150 ms`.

The measured corpus now has two families, and the boundary is a property of the
FMC image, not of the boot image, the COT, or the host structures:

| FMC class | boots | `MAILBOX0` | WPR2 at 150 ms | status queue |
|---|---|---|---|---|
| 570.144, 199,240 B | 19+ | `0x0B`, 0.2–28 s | **up** (`0x07E61200/0x07F1EE00` at 27 s, `rm41`) then torn down | never written |
| 615.78.08, 287,304 B | `rm63`, `rm63b` | **`0x824`** at 150 ms | **already down** (`0x7FFFFE00/0`) | never written |
| 620.70.10, 287,304 B | `fh620`, `fh60b`, `fh62` | **`0x824`** at 150 ms | already down | never written |

So the 287 KiB FMCs do not reach the state where the FSP arms WPR2 at all, while
the 570 FMC arms it, runs (the ACR reads nothing — `rm59b`), and then aborts with
`0x0B`. The wall is on the newer FMC's *entry* path.

### 11.4 The §8 instrument, closed — and negative (`rm63b`)

The readbacks §8 named were added before `rm63` (so both boots carry them) and
are in every sample above:
`NV_PFSP_FALCON_COMMON_SCRATCH_GROUP_2(0..3)` (`0x8F0320+4i`),
`NV_PGSP_MAILBOX(1..3)` (`0x110808/80C/810`; GB100 `__SIZE_1` = 4, so `gmb0` at
`0x804` was the only word this kernel had read), plus the FSP boot-complete gate
`NV_THERM_I2CS_SCRATCH` (`0xAD00BC`) — eight words where the kernel had read one.

* The FSP scratch is **frozen** at its boot-time values — `0x9F, 0, 0, 0xA` — the
  same four words `t_67f09feb` measured at +2 ms and the same the `[NV-PFSP]`
  probe prints before any COT. Neither the COT nor the `0x824` moves them.
* `i2cs = 0xFF` (FSP_BOOT_COMPLETE.SUCCESS) throughout; `PGSP_MAILBOX(1..3) = 0`.
* **H2: the FSP's CMS2 log.** The vendor's own `kfspDumpDebugState_GB100` reads
  it through `NV_POOBHUB_RCV_INDIRECT_CMS2_MEM_RD_ADDR` (`0x8AA374`) /
  `…_RD_DATA` (`0x8AA378`), words `0x50…0x7F`. Read pre-COT and again after 55 s:
  **48/48 dwords identical**, and the whole window only ever contains
  `{0, 0x7, 0xA, 0x65, 0x9F, 0xFF, 0x140000}` — a repeated 7-word status record
  (the same `0x9F`/`0xA` the scratch holds), **no text, and no new record for the
  abort**. The FSP does not write this failure anywhere the host can read.

(The CMS2 rows are printed with the `[NV-FSPCOT]` prefix repeated inside the row
because the helper prints one prefix per call; cosmetic, and the 48 values were
parsed and compared programmatically, not by eye.)

### 11.5 What `0x824` is, and is not

* **Not an FSP RPC error.** `kern_fsp_retval.h` (620.73.04) defines exactly
  `FSP_OK 0x00`, `FSP_ERR_IFS_ERR_INVALID_STATE 0x9E`, `…FILE_NOT_FOUND 0x9F`,
  `…NOT_SUPPORTED 0xA0`, `…INVALID_DATA 0xA1`, `…INVALID_KNOB_ID 0x1E3`.
  `0x824` is outside that set; `kfspErrorCode2NvStatusMap_GH100` would map it to
  `NV_ERR_GENERIC`. It is an FMC/ACR-side code reported in the *formatted*
  register (variant 1); the same value also lands in `MAILBOX0` (`0x824`), which
  the vendor calls an "effectively 8-bit" channel for older FMCs.
* **Not in the public map.** The GB202 reverse-engineering corpus
  (`shuttie/bf16-unlock-blackwell-research`, `specs/08_errors-and-logging.md`)
  maps seventeen boot codes — `0x827` (`WprMeta.magic`), `0x828` (`revision`),
  `0x82e` (`sizeOfBootloader` alignment), `0x836` (256 MiB bootloader),
  `0x83d/0x83e/0x841/0x885/0xa97/0xaaa/0xa0f/0xaff`, and the FSP responses
  `0xd6/0xd9/0x33/0x377/0x1db` — and **does not contain `0x824`**. Its numeric
  neighbours place it in the `WprMeta`-parser block, immediately before `magic`.
* **Our `WprMeta` differs from the vendor's 615 stock in two fields** (same
  corpus, GB202 PRO, 615.71.09, `specs/01_boot-chain.md#wprmeta`):
  `frtsSize` **0x100000** there vs **0** here, and `nonWprHeapSize` **0x300000**
  there vs **0x220000** here (`RM_NONHEAP_BYTES_GB202` = 2,228,224; the row
  `wprm sizes … nonwpr_heap=0x0000000000220000` is what was staged). `gspFwHeapSize`
  matches (`0x8700000`); `fbSize = 0` is documented as ignored on the 615 class
  (an `0xffffffff` value still boots), and `sizeOfBootloader = 0x47000`,
  `bootloader{Code,Data,Manifest}` `0xf200/0xa00/0`, `sizeOfSignature = 0x1000`
  all match our staged pair.

### 11.6 The named next boot — `nonWprHeapSize`, not `frtsSize`

The first version of this paragraph named `WprMeta.frtsSize = 0x100000` first.
**That is withdrawn**: the same corpus this document cites for the `WprMeta`
diff measured the field as decorative on this path —
`experiments/05_fmc-fsp-boot-inputs.md` §05.12 ("no cross-check …
`WprMeta.frtsSize` is decorative on this path. The COT value is authoritative")
and `specs/01_boot-chain.md` line 179 ("Disagreement with the COT is ignored;
the COT value wins") — and our COT already carries `frtsVidmemSize = 0x100000`,
so making the struct agree cannot change the layout under that rule. Nothing in
the `0x824` state suggests the 287 KiB class reverses a rule the corpus measured
on the same part, so no boot is spent on it.

The one measured difference left is **`nonWprHeapSize 0x220000 → 0x300000`**
(the corpus's "honoured 1:1" field). §12 spends that boot. After it, the next
boot is the survey's rank-2 pair — **580.178.04** (FMC `cea2190c…`, 215,624 B,
header `+0x0C` = `0x25800`; its own boot image is the `ac068832…` `BOOTIMG.BIN`
already staged) — which §13 spends; and then the **590.48.01 class point**
(§6 rank 1). The FSP's DMEM log over the MCTP download-log RPC
(`_kfspGatherDmemLog_GB100`) remains the only in-band namer, and it needs that
packet protocol implemented — not a COT or `WprMeta` field.

The VM 301 / `nvidia-open-580.178.04` control still needs coordinator sign-off
(§7.2) and was not run.

### 11.7 Acceptance

**Unmet, and no mock or shim value is used or claimed.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0` in both boots, and the status queue's producer header is 0 at every
sample (`[NV-RPC] peer … (header still zero)`). The part is GB202, `41:00.0`, VM
313 in every row.

---

## 12. The mode-64 boot — the last one-field difference, measured (1 boot, 2026-11-27)

§11.6 promoted one field and this section spends it: **`WprMeta.nonWprHeapSize`
`0x220000` → `0x300000`** (the vendor's 615 stock value), 615.78.08 pair, mode 58
host state, COT unchanged. One VM 313 boot, `tools/nv/gb202fsp-boot.sh`, tag
`rm64`, `STOP_AFTER_VERDICT=1 STOP_PATTERN='^\[NV-FSPRM\] verdict='`. The
kernel mode is `64`; its source diff is attached (`nv_fsp_cot.c.mode64.diff`).
No CMS2 window in this boot — §11.4 measured that instrument static and
textless, so it would only have added rows.

### 12.1 What moved, and what did not — the two logs diffed

Mode 64 is mode 63 plus one descriptor dword. The proof is mechanical: the two
serial logs are **586 lines each, 13 samples each**, every sampled register is
single-valued in each boot and **identical between them**
(`m0=0x824`, `fmcerr=0x12000824`, `wpr2_lo=0x7FFFFE00`, `wpr2_hi=0`,
`s2=0x9F,0,0,0xA`, `mb1..3=0`, `i2cs=0xFF`, `hwcfg2=0x8187A7F7`), and the
row-by-row diff of the whole GSP/FSP path is **one** row (`rm63`, then `rm64`):

```
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000220000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000300000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
```

plus the mode number in the two rows that print it, and boot-time noise that is
not an input: the boot timestamp, the image sha, the ptimer pair, the guest's
NVMe / `[BLKQ]` / `[NFSIO]` rows (their values are per-boot *timing* of the
block driver's own init — one boot shows `asqt=0 acqh=0` and `ms=7265`, the
other `asqt=1 acqh=1` and `ms=32806`, all before the GPU probe), the kernel's
LAPIC-calibration digits, `[BLKTOKW]` counters, and **one channel-probe word**
in its own EMEM window (`[NV-GB202FSP] shape off=0x8F2A48 v=0x40` in `rm63` vs
`0x42` in `rm64`) — that block is what the probe itself wrote before the COT,
not a COT input, and the FSP's own scratch (`0x8F0320..0x8F032C`) is identical
in both. The COT's three sysmem addresses, the descriptor's `bootimg`
address/size and the whole `wprm dwords`/`wprm-after dwords` block are
**identical** between the two boots.

The format fix from §11's review is visible here: `wprm dwords`/`wprm-after
dwords` now print `d0=0x…` once (`uart_print_hex` already emits `0x`). The
`rm63`/`rm63b` logs keep the old `0x0x` text — those are historical records and
are quoted as they are; only the two `wprm` rows were changed, so the rest of the
dword rows (`rmargs`, `boot_args`, `frts`) still carry the double prefix.

### 12.2 The rows

```
[NV-FSPRM] start gsp=rm mode=64[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63/64: FMC615.BIN,BOOT615.BIN)
fmc image bytes=287304 fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
bootimg bytes=290816[NV-FSPCOT]  file=BOOT615.BIN d0=0x000000001156E500 d1=0x000000003996718D d2=0x0000000045AF2B00 d3=0x00000000C74FBC4D code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000300000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] gsp-shape post words=64 not-locked=64 transitions=11 first=0x00000000BADF5620 verdict=window-decodes
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=64 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000824 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000012000824 radix3=1
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
```

(The `t_ms=55000` row is abridged with `…` in the middle — the elided fields are
identical to the `t_ms=150` row above it; all 13 rows were parsed and compared
programmatically, and the full row is in the log. Every other quoted row is
verbatim, prefix quirks included.)

Boot bookkeeping from the harness log header and the closing state:

```
# [NV-GB202FSP] boot 2026-11-27T18:04:20Z
# image=/srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_a769beaa/rm64.img sha256=0c84359fae405eacfe7056ba4f28536ece61732aceaa23f12d883a9650784fb4
# tag=rm64 vm=313 hostpci0=0000:41:00
```

Closing state, read on r1 after the boot: `qm status 313` = `stopped`,
`qm status 300` = `stopped`, `pgrep -c qemu-system-x86` = **0**; the guest's
`serial.log` was last written 2026-11-27 12:04:36 -0600 (= 18:04:36Z) and no
boot has touched it since. Staged kernel sha256
`3970903eff220231a8aec61e7894292765827689afc073bf99c9e4dcb28418dc`; the two
firmware files inside the image were re-read with `mtype | sha256sum` and match
§11.1 byte for byte (`ae4e7055…`, `37159b4b…`).

### 12.3 What this settles

**The vendor's own 615-stock `nonWprHeapSize` is inert for the `0x824` abort.**
With every other input identical — same FMC image, same boot image, same COT,
same boot-args page, same heap size, same sampled registers — 1 MiB more
non-WPR heap moves `MAILBOX0` not at all (`0x824` at 150 ms, constant to 55 s),
moves the formatted code not at all (`0x14DC = 0x12000824`), and does not even
arm WPR2 (`0x7FFFFE00/0`). The ACR never laid out WPR2, so a heap *offset* it
would place inside WPR2 cannot be what it validated.

With `frtsSize` demoted on the corpus's own measurement (§11.6) and
`nonWprHeapSize` now measured inert, **every `WprMeta` field in which our block
differs from the vendor's 615 stock has been answered**: the `0x824` abort is
upstream of `WprMeta` *content* on the 287 KiB FMC class. That is consistent
with §11.3's shape (WPR2 never armed; the 570 class arms it and aborts later at
`0x0B`) and with §9's ruled-out table: what is left is either the host↔FMC
input at a layer below the descriptor (the RPC/`GSP_FMC_BOOT_PARAMS` path the
corpus does not publish) or something the FMC reads that only a *different
generation* supplies.

### 12.4 Acceptance

**Unmet, and nothing here is a mock or a shim value.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0`, `[NV-RPC] peer … (header still zero)` at all 13 samples of this
boot, and the status queue's producer header 0. The message id exercised in this
boot is still only the boot channel's: NVDM COT `class=1`, `cmd_nvdm=20 (0x14)`,
answered `errorCode=0` — no `NV_VGPU_MSG_*` request exists to send, because no
RM task has started. Part: **GB202, `41:00.0`, VM 313** in every row.

### 12.5 The named next step (superseded by §13)

The version of this paragraph that shipped in the mode-64 handoff named
**580.65.06** as the next boot, described as "FMC `c62856e9…` (215,624 B,
`code 0xF200`) … the last 215 KiB / first 0xF200-code generation". That naming
was wrong three ways and is withdrawn:

1. it **contradicted this document's own ranking** — the pre-§13 §6 listed
   580.65.06 under "no question left" while rank 2 named the staged `580.178.04`
   pair, which is the same 215 KiB class with the same `0x25800` code size;
2. `code 0xF200` is the **boot image's** `monitorCodeOffset`; the FMC's own
   header field (`+0x0C`) is `0x25800`, and its total extent (`+4`) is `0x34A00`;
3. "the last 215 KiB generation" is false — `580.178.04` is **later** than
   `580.65.06` and the same size, and it is the image the 580 open module binds
   for this part.

The pair the reconciled text names is therefore **`580.178.04`** (FMC
`cea2190c…` container `FMCV.BIN` 216,204 B, with `BOOTIMG.BIN` `ac068832…`,
217,088 B — already staged, and its own same-tag boot image). §13 spends it.

After §13 the next named step is **590.48.01** (FMC `6252aafc…`, 286,792 B,
`0x36E00`, with its own `GspRmBoot` `e1265e10…`), the only image class between
the two measured ends — §6 rank 1, §7.1.

The VM 301 / `nvidia-open-580.178.04` control still needs coordinator sign-off
(§7.2) and was **not** run under this card.

---

## 13. The 580.178.04 boot — NVIDIA's own FMC for this driver, measured (1 boot, 2026-11-27)

This is the boot §12.5's reconciled text names (§6 rank 2 of the survey
version): **the FMC image is the only variable.** Mode 65 = mode 58's host state
(vendor heap pair `gspFwHeapSize 0x8700000` / `nonWprHeapSize 0x220000`, COT
`frtsVidmemOffset 0x4000000` / `frtsVidmemSize 0x100000`, descriptor
`frts_size 0`), with the FMC container swapped from linux-firmware's 570.144
(`FMC.BIN`, image 199,240 B) to 580.178.04's own (`FMCV.BIN`, image 215,624 B).
The **boot image is the same file** (`BOOTIMG.BIN`, 217,088 B `ac068832…`), the
descriptor's `bootloader*` offsets are the same **and correct for a 580 FMC**
(`0xF200`/`0xA00`/`0`, code size `0x25800` = the FMC header's `+0x0C`, §2.4), and
the boot-args page, `WprMeta` field values, GSP-RM image and signature are
identical. So this pair differs from the wall by exactly one artifact: the FMC.

One VM 313 boot, `tools/nv/gb202fsp-boot.sh`, tag `rm65`,
`STOP_AFTER_VERDICT=1 STOP_PATTERN='^\[NV-FSPRM\] verdict='`. Kernel mode 65;
source diff attached (`nv_fsp_cot.c.mode65.diff`).

### 13.1 What was staged

| file | bytes | sha256 | identity |
|---|---|---|---|
| `FMC580C.BIN` (staged from `fw/FMCV.BIN`) | 216,204 | `6adbd92d63c375aa3dc40ea11af35cbc4a03957e3b2c9ae3e778cc5d90ab3cbc` | 580.178.04 container; `image` = **215,624 B, sha256 `cea2190c…`**; header `+4` = `0x34A00`, `+0x0C` = `0x25800` |
| `BOOTIMG.BIN` | 217,088 | `ac0688326c1ea8aae2da26d785e7091bedb9a45565b03543a479ed05db0ba367` | the 580.178.04 `GspRmBoot_GB202` image — **the same file every wall boot staged** |
| `GSPRM.BIN` / `GSPRM.SIG` / `FMC.BIN` / `FMCV.BIN` | 63,541,248 / 4,096 / 199,820 / 216,204 | unchanged from `base.img` | the same GSP-RM image, radix3 source and signature as the `0x0B` boots |

The staged bytes were verified inside the image (`mtype | sha256sum`) before the
boot and the log re-reads them (`fmc image bytes=215624`,
`img_d1=0x0000000000034A00`; `bootimg bytes=217088 … file=BOOTIMG.BIN
code_off=0x000000000000F200`).

### 13.2 The rows

```
[NV-FSPRM] start gsp=rm mode=65[NV-FSPCOT]  files=/NVIDIA/GSP/{FMC.BIN,BOOTIMG.BIN,GSPRM.BIN,GSPRM.SIG} (mode 63/64: FMC615.BIN,BOOT615.BIN; mode 65: FMC580C.BIN,BOOTIMG.BIN)
fmc image bytes=215624 fmc img_d0=0x0000000000480403 img_d1=0x0000000000034A00
bootimg bytes=217088[NV-FSPCOT]  file=BOOTIMG.BIN d0=0x000000004DC21E2B d1=0x000000007D12A87C d2=0x00000000499B881A d3=0x00000000117369FA code_off=0x000000000000F200 data_off=0x0000000000000A00
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000220000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] cot gspFmcSysmemOffset=0x000000020C616000 frtsSysmemOffset=0x000000020C64B000 gspBootArgsSysmemOffset=0x000000020C74B000 bytes=0x000000000000035C
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000000000000 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000000 partition=0x000000000000000F sk_err=0x0000000000000000 sk_phase=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=27000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 … wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B … wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
[NV-FSPCOT] gsp-shape post words=64 not-locked=64 transitions=11 first=0x00000000BADF5620 verdict=window-decodes
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=65 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x000000000000000B m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000000000000 radix3=1
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000000000000 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000000 partition=0x000000000000000F sk_err=0x0000000000000000 sk_phase=0x0000000000000000
```

The `rm tl` rows are abridged with `…` in the middle; all 13 samples were parsed
programmatically and every elided field is single-valued in the boot (see
`KEY-ROWS-65.txt`, generated by `keyrows65.py` from the log). The full rows are
in `5090-gb202fsp-rm65.log`.

Boot bookkeeping from the harness header and the closing state:

```
# [NV-GB202FSP] boot 2026-11-27T18:17:54Z
# image=/home/nvcard/.hermes/kanban/boards/hobbyos-nv/workspaces/t_a769beaa/rm65.img sha256=33fac03c922c3dc083a96e549bee9f4f9589097e3df88616bf825e0a9200b8b0
# tag=rm65 vm=313 hostpci0=0000:41:00
```

Closing state, read on r1 after the boot: `qm status 313` = `stopped`,
`qm status 300` = `stopped`, `pgrep -c qemu-system-x86` = **0**; the guest's
`serial.log` was last written 2026-11-27 12:18:10 -0600 (= 18:18:10Z) and no
boot has touched it since. No `disk.img.pre-*` backup was taken for this tag, so
the r1 footprint is one 1 GiB image copy (53 GB free before and after).

### 13.3 What this settles: `0x824` is the 286/287 KiB class; `0x0B` is what a 215 KiB FMC does here

**NVIDIA's own FMC for the 580 driver — the image `nvidia-open-580.178.04`
binds for GB202 — arms WPR2 and then terminates exactly like the 570.144 FMC.**
Three things are identical to the mode-58 control (`fh58`, 570.144 FMC, same
host state) and are the reason this is a one-variable result rather than a
coincidence:

| | `fh58` (570.144 FMC) | `rm65` (580.178.04 FMC) |
|---|---|---|
| `fmc image bytes` | 199,240 | **215,624** |
| `wprm sizes` | heap `0x8700000`, nonwpr `0x220000`, frts `0` | **identical** |
| PFB WPR2 while silent | `0x7E5A200` / `0x7F1EE00` | **identical** |
| terminal `MAILBOX0` | `0x0B` | **`0x0B`** |
| WPR2 at that sample | `0x7FFFFE00` / `0` | **identical** |
| `0x14DC` | `0` | **`0`** |
| `statq_written`/`linked`/`initdone` | `0/0/0` | **`0/0/0`** |

The only differences are the FMC image and the time the attempt ends
(`fh58`: `0x0B` by the 55 s sample, silent at 40 s; `rm65`: `0x0B` by 40 s,
silent at 27 s) — both inside the `0.2–55 s` window `t_0936a17f` measured for
this terminal code.

What follows:

1. **`0x824` is not "the FMC NVIDIA ships".** The 580 image is shipped by
   NVIDIA for this part and answers `0x0B`; `0x824` is confined to the
   286/287 KiB images (590, 595, 610, 615, 620 — 615 and 620 measured).
2. **The wall is not a 570-linux-firmware artifact.** It survives the swap to
   NVIDIA's own prod FMC of a later driver generation with the same host state,
   the same boot image and the same descriptor fields.
3. **The 215 KiB class still arms WPR2 and still cannot finish.** The FSP sizes
   WPR2 from the descriptor exactly as `t_7178dca2` measured (the pair is
   identical to `fh58`'s, so the descriptor reached the FSP), and the ACR still
   writes nothing into the WprMeta block (`untouched=yes`) before ending.
4. **The class boundary is now 580→590**, not 615→620: the only unmeasured image
   class between a measured `0x0B` and a measured `0x824` is `590.48.01`
   (286,792 B, `0x36E00`) — §6 rank 1, §7.1.

### 13.4 Acceptance

**Unmet, and nothing here is a mock or a shim value.** No channel handle, no VA
space, no engine class object, no VA→VRAM binding: `statq_written=0 linked=0
initdone=0` in this boot, `[NV-RPC] peer link linked=0 peer=none (status header
still zero: no RM task has started)` at every sample, and `wprm-after
untouched=yes` — no ACR-owned field was ever written, so no allocation object
exists to quote. The message id exercised in this boot is still only the boot
channel's: NVDM COT `class=1`, `cmd_nvdm=20 (0x14)`, answered `errorCode=0` — no
`NV_VGPU_MSG_*` request exists to send, because no RM task has started. Part:
**GB202, `41:00.0`, VM 313** in every row.

### 13.5 The named next step (not spent)

**`590.48.01`** — FMC `6252aafc…` (286,792 B, header `+4` `0x46000`, `+0x0C`
`0x36E00`) with its own `GspRmBoot` `e1265e10…` (286,720 B, `0xF200`/`0x36E00`),
staged as `/NVIDIA/GSP/FMC590.BIN` + `BOOT590.BIN` with a mode that repeats mode
65 and changes only `bl_name`/`bl_code`/`bl_want`. It is the one remaining class
point: `0x824` there puts the abort at the first `0x36E00` image, `0x0B` there
puts it inside the 286→287 KiB step and hands the question to 610.57.04. One
boot, the same harness, the same COT.

The VM 301 / `nvidia-open-580.178.04` control (§7.2) still needs coordinator
sign-off and was **not** run under this card.
