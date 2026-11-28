# SWARM-REPORT: a VA space object, and a VA bound to real memory, from the mock RM — card t_b9c4df6d

Card: **t_b9c4df6d**, "Boot GB202, allocate a VA space and bind a VA to real VRAM".
Re-scoped by the owner on 2026-11-28: **the mock RM is acceptance for this
object; the firmware version of the claim stays parked on the GB202 ACR wall**
(`t_a769beaa`, revive condition R2). **Zero boots**: VM 300 / 313 / 314 / 315
were not started.

Tag on every claim: **MEASURED** = read out of a log or a live machine in this
run, with the file and line; **SOURCE** = read out of this tree's source (or,
where named, an external driver's source fetched in this run), with
`file:line`; **INFERRED** = a conclusion drawn from those two, labelled.

---

## 0. The hypothesis, answered

Original hypothesis: *"GSP-RM on GB202 allocates a VA space object for our
driver and a VA bound through it resolves to a real VRAM physical address
readable back from the mapping."*

**Falsified as written, and deliberately not attempted.** No boot was run
(owner instruction; the boot campaign was stopped at 06:00Z after seven GB202
boots left `MAILBOX0` at `0x0B`/`0x824` with `linked=0`, `initdone=0`), so there
is no GSP-RM peer to allocate anything. There is no VRAM physical address to
read back, because nothing on this path owns a card.

What this card delivers is the **mock** version of the same object life-cycle,
minted by `nv_rm_mock` through the existing `/dev/nvshim` VA rows:

1. a VA space object is allocated **by the RM**, and its handle comes back from
   the RM (`vm=1`), never from the client;
2. a VA is bound to a memory object that is backed by **real, physically
   contiguous pool frames** (`nv_dma_alloc`), and the RM's own walk of that
   binding answers the **physical address of the byte the VA names**;
3. the answer is a read-back through the mapping — the client sends a VA and
   has no field in which to send an address — and it is checked against the
   object's memory in the same boot (a store at the resolved physical address
   is read back through the RM's object handle at the same offset).

**It is not VRAM.** The object's frames are host pool RAM at a real CPU
physical address; GB202's VRAM is neither allocated nor addressed here. Every
row below carries `mock=1` in its own text, and **no result from this card may
be quoted as "GSP-RM allocated", a firmware handle, a VRAM address, or L3
evidence.**

---

## 1. Part identity — read-only, nothing booted

MEASURED on r1, 2026-11-28 06:37:46Z (read-only `qm config` / `lspci`, no VM
started):

```
cores: 8
hostpci0: 0000:41:00,pcie=1
memory: 16384
name: hobbyos-nv5090

41:00.0 VGA compatible controller [0300]: NVIDIA Corporation GB202 [GeForce RTX 5090] [10de:2b85] (rev a1)
```

VM 300, for contrast, still passes through `0000:01:00` (the AD102):
`hostpci0: 0000:01:00,pcie=1`. The mock run below never touched either card: it
ran on the unit-tier guests (x86-64 and AArch64 TCG, no PCI passthrough), which
is why the handle is a mock object and the address is pool RAM, not VRAM.

---

## 2. What the shim could not express, and the extension it got

The VA rows existed (`NVSHIM_IOC_VM_INIT` 6, `NVSHIM_IOC_VM_BIND` 7) but the
object was bookkeeping: `vm_create` handed back a handle, `vm_bind` recorded
`(va, range, handle, bo_offset)`, and **nothing ever answered the question a
binding exists to answer — what address does this VA name?** The rows said
`vm vm=1 size=…` and `bind op=0 va=…`, with `mock` nowhere in them. Both halves
of the card's evidence shape (an RM-assigned VA space handle; a physical
address read back through a binding) were inexpressible.

The shim was extended (SOURCE, four files, all additive):

* **RM seam** (`src/include/nv_rm.h:161-178`): `vm_resolve(dev, vm, va, *phys,
  *bus, *range)` — the RM's page-table walk over its *existing* binding records,
  returning the CPU physical address of the byte at `va`, the device address a
  GPU would dereference for it, and the extent of that one binding from `va`.
  The answer is built from what the RM stored at `vm_bind` plus the backing
  object's own addresses; the client supplies a VA and nothing else.
* **Mock backend** (`src/kernel/nv_rm_mock.c:480-510`): `mock_vm_resolve()`
  walks `MOCK_MAX_RANGE` intervals, finds the one covering `va`, finds the
  object it bound, and returns `a->phys + bo_offset + (va - r->va)`. A hole, a
  VA past the space, and a binding whose object has no memory are `-EINVAL`;
  nothing is clamped. `mock_vm_bind` (`:423`) is **unchanged**, so the walk
  observes the bindings the existing tests already create instead of recording
  a second, parallel truth.
* **Rows** (`src/kernel/nv_shim.c:1234-1275`, `:1318-1381`): `vm req`/`vm rsp`
  for VM_INIT and `vm_bind req`/`vm_bind rsp` for BIND, in the same shape as the
  channel and engine-class rows and every one carrying `mock=1`. The reply rows
  carry `phys=`, `bus=` and `mapped=` **as the walk returned them**. The DRM
  face's existing `drm bind` row carries the same three fields for its first MAP
  op, so the stock-client path shows the read-back too (`:3460-3475`).
* **Row budget** (`src/kernel/nv_shim.c:421-430`): `r_vmbind = 16`. This is not
  cosmetic — the first run of this change returned `-EINVAL` for a VA past the
  space with **no row at all**, because `r_refuse` is 8 rows per boot and the
  `'N'` face's `-ENOSYS` rows spend it before the VM suites run. That is the
  invisible-refusal failure this file already documents twice (`r_param`,
  `r_drm`); the refusals now have their own bucket and all of them print.

No behaviour change for any existing request: same RM calls, same struct sizes
(`nvshim_vm_init` 16, `nvshim_vm_bind` 40 — unchanged and still asserted), same
refusals. 200 insertions / 14 deletions across four files.

---

## 3. The rows, quoted verbatim

Both tiers of this checkout ran the change. The quoted pairs are from the intel
tier's console log; the ARM tier printed the same rows in the same order at the
lines cited, with its own physical addresses (§3.5). Verified byte-for-byte by
`check-quotes-vm.py` (attached; output in `check-output-vm.txt`).

### 3.1 The VA space request row

`unit-tier-intel-vm.log:2361` (MEASURED), in `test_nvshim_probe`:

```
[NVSHIM] vm req mock=1 ioc=vm_init size=0x100000000
```

ARM, same row shape: `unit-tier-arm-vm.log:1630` (MEASURED).

### 3.2 The VA space response row — the object handle

`unit-tier-intel-vm.log:2362` (MEASURED):

```
[NVSHIM] vm rsp mock=1 vm=1 size=0x100000000
```

ARM, `unit-tier-arm-vm.log:1631` (MEASURED):

```
[NVSHIM] vm rsp mock=1 vm=1 size=0x100000000
```

**The VA space object handle is `vm=1`.** It is the mock RM's first VA space of
that device lifetime, stamped by `++g_mock.next_vm` inside `mock_vm_create`
(SOURCE: `src/kernel/nv_rm_mock.c:405`); the client's `struct nvshim_vm_init`
leaves `vm_id` untouched and only receives it.

The same `vm=1` appears again later in the boot for the *second* and *third*
probe (`unit-tier-intel-vm.log:2400-2401`, `:2414-2415`), because each probe is
a separate device lifetime: the last `device_close` calls `mock_reset()`
(SOURCE: `src/kernel/nv_rm_mock.c:260-269`), which zeroes the counters. Those
are three sequential first-spaces, not three concurrent spaces sharing a
handle — each one is torn down before the next row pair, and the tier's
`nv_rm_mock_live() == 0` after teardown is what proves it.

### 3.3 The bind request row

`unit-tier-intel-vm.log:2363` (MEASURED):

```
[NVSHIM] vm_bind req mock=1 ioc=vm_bind vm=1 op=0 va=0x10000000 range=0x10000 handle=1 bo_offset=0x0
```

ARM, `unit-tier-arm-vm.log:1632` (MEASURED) — identical but for nothing:

```
[NVSHIM] vm_bind req mock=1 ioc=vm_bind vm=1 op=0 va=0x10000000 range=0x10000 handle=1 bo_offset=0x0
```

`op=0` is `NVSHIM_VM_BIND_OP_MAP`, `handle=1` is the 64 KiB memory object the
probe allocated in the `NVSHIM_DOMAIN_VRAM` domain, `range=0x10000` covers it
whole, `bo_offset=0x0` starts at its first byte.

### 3.4 The bind response row — the physical address, read back

`unit-tier-intel-vm.log:2364` (MEASURED):

```
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x10000000 range=0x10000 phys=0x209686000 bus=0x209686000 mapped=0x10000
```

ARM, `unit-tier-arm-vm.log:1633` (MEASURED):

```
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x10000000 range=0x10000 phys=0x8ef46000 bus=0x8ef46000 mapped=0x10000
```

**The physical address read back is `phys=0x209686000` on the intel tier and
`phys=0x8ef46000` on the ARM tier.** The two differ because each is the first
frame of the run `nv_dma_alloc` handed that tier's own pool — a real physical
address, not a typed constant (and the strongest single proof that the field is
read out rather than echoed: the same source tree, the same request, two
platforms, two addresses).

`bus=` is what a device dereferences for the same byte. On both tiers it equals
`phys`, which is a property of this tree's measured DMA state, not of the mock:
`nv_dma_resolve_bus()` returns `phys` only while `GSTS.TES` is clear
(SOURCE: `src/kernel/nv_dma.c:75-80`, and `src/kernel/nv_dma.h:1-30` for the
measurement that translation is off), and `nv_dma_bus()` refuses with
`NV_DMA_BUS_INVALID` rather than publishing a plausible number once it is on.
`mapped=0x10000` is the walk's extent from `va` — the whole binding, here.

**The walk translates; it does not repeat.** The argument suite binds the same
object at two VAs, and each resolves through *its own* binding
(`unit-tier-intel-vm.log:2417`, `:2420`, MEASURED):

```
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x1000 range=0x1000 phys=0x209685000 bus=0x209685000 mapped=0x1000
[NVSHIM] vm_bind rsp mock=1 op=0 va=0x3ffff000 range=0x1000 phys=0x209685000 bus=0x209685000 mapped=0x1000
```

Same object, same first byte; the VA at `0x3ffff000` (one page inside a 1 GiB
space) names it too. In the probe, where the binding is 64 KiB, the tier also
asserts `va+0x2000 → phys+0x2000` with `mapped=0x10000-0x2000` (§5).

### 3.5 The refusals are named (and visible)

`unit-tier-intel-vm.log:2366` (a double map of the same range — the RM's own
interval bookkeeping), `:2422` (a VA at the space's size, i.e. the first byte
outside), `:2423` (a handle this client does not own) and `:2424` (an op that is
neither MAP nor UNMAP), MEASURED:

```
[NVSHIM] vm_bind refused mock=1 va=0x10000000 range=0x10000 op=0 rm=-22
[NVSHIM] vm_bind refused mock=1 va=0x40000000 range=0x1000 op=0 rm=-22
[NVSHIM] vm_bind refused mock=1 handle=1 reason=not_this_clients_object
[NVSHIM] vm_bind refused mock=1 va=0x30000 range=0x1000 op=7 reason=geometry
```

The first of these is the shape that used to be invisible (§2, `r_vmbind`): the
same boot before the budget fix returned `-EINVAL` with no row at all.

An UNMAP has nothing left to name, so its reply carries no address
(`unit-tier-intel-vm.log:2383`, MEASURED):

```
[NVSHIM] vm_bind rsp mock=1 op=1 va=0x10000000 range=0x10000
```

### 3.6 The same read-back on the stock client's face

A stock nouveau client does not speak the `'N'` face; it speaks the DRM one.
The existing `drm bind` row now carries the walk's answer for its first MAP op
(`unit-tier-intel-vm.log:2460`, MEASURED):

```
[NVSHIM] drm bind vm=1 ops=2 first_addr=0x10000 first_range=0x10000 phys=0x209685000 bus=0x209685000 mapped=0x10000
```

and ARM (`unit-tier-arm-vm.log:1729`, MEASURED):

```
[NVSHIM] drm bind vm=1 ops=2 first_addr=0x10000 first_range=0x10000 phys=0x8ef46000 bus=0x8ef46000 mapped=0x10000
```

A batch whose first op is an UNMAP omits the field rather than answering zero
(`unit-tier-intel-vm.log:2461`, MEASURED):

```
[NVSHIM] drm bind vm=1 ops=1 first_addr=0x20000 first_range=0x1000
```

---

## 4. The ids used — quoted as they appear

There is **no GSP-RM message id** on this path, and quoting one would be the
fabricated reply the card forbids. The mock has no message protocol: no queue,
no packet, no console, and nothing in this tree sends an `NV_VGPU_MSG_*`
(SOURCE: the only occurrences are comments, `src/include/nv_rm.h:70-71` and
`src/kernel/nv_gsp.c:1148`). **Zero message ids were used.**

The ids that *are* on the path, quoted from the header as written:

* `src/include/nv_shim.h:306`
  ```
  #define NVSHIM_IOC_VM_INIT       NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 6, struct nvshim_vm_init)
  ```
  with `#define NVSHIM_IOC_MAGIC 'N'` (`nv_shim.h:106`) — the request the
  §3.1 row names `ioc=vm_init`.
* `src/include/nv_shim.h:307`
  ```
  #define NVSHIM_IOC_VM_BIND       NVSHIM_IOW(NVSHIM_IOC_MAGIC, 7, struct nvshim_vm_bind)
  ```
  the request the §3.3 row names `ioc=vm_bind`.
* The two structures those requests encode: `struct nvshim_vm_init`
  (`nv_shim.h:219`, 16 bytes, `vm_id` is **out**) and `struct nvshim_vm_bind`
  (`nv_shim.h:228`, 40 bytes, and there is **no address field of any kind** —
  the client cannot even offer a physical address).
* The objects' ids: `vm=` in the reply row (the RM assigned it,
  `nv_rm_mock.c:405`) and `handle=` in the bind rows (the memory object, also
  the RM's, `nv_rm_mock.c:320`).

**For the parked firmware claim only (SOURCE, fetched, not sent).** I fetched
nouveau's current GSP-RM VA-space path to name what the mock stands in for.
`drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/vmm.c` (torvalds/linux master,
fetched 2026-11-28 06:33Z, `sha256 2ef7ee195914…`, kept as
`nouveau-r535-vmm.c` in the evidence directory):

* the VA space is an **RM allocation**, not a message with a hand-written id:
  `nvkm_gsp_rm_alloc_get(&vmm->rm.device.object, handle, FERMI_VASPACE_A, …)`
  (`:63`), written with `nvkm_gsp_rm_alloc_wr`, and the object's handle is
  `vmm->rm.object.handle` (`:37`, `:144`) — the same "the RM stamps it"
  property this card's seam keeps;
* the page directory is programmed with **control commands**:
  `NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY` for the external (client-owned)
  space (`:136`, with `hVASpace = vmm->rm.object.handle` at `:144`) and
  `NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES` for the internal one
  (`:105`);
* the server-reserved PDE copy uses `page_shift = 29; /* 512MiB */` (`:78`) —
  a **nouveau layout choice**, not a GB202 measurement. It is recorded here as
  what the mock does not model, and is **not** a number this card claims.

For the map half I did not find (and therefore do not name) a message id in the
fetched source, so this report names none. `NV_VGPU_MSG_FUNCTION_VMM_*` is not
asserted here: unverified names would be worse than silence.

---

## 5. Where the handle and the address come from

SOURCE, `src/kernel/nv_rm_mock.c`:

* **Handle** — `mock_vm_create()` (`:393-421`) checks the size, takes the first
  free slot and stamps `v->id = (uint32_t)++g_mock.next_vm` (`:405`). The ids
  in §3.2 are indices in `g_mock.vm[]`, not GSP-RM objects.
* **Address** — `mock_vm_resolve()` (`:480-510`) finds the range covering `va`,
  resolves its `handle` to the object, and returns
  `a->phys + r->bo_offset + (va - r->va)` / `a->bus + …`. `a->phys` is the
  first byte of the run `nv_dma_alloc()` returned in `mock_mem_alloc()`
  (`:292-322`, stored at `:322`), i.e. real pool frames; `a->bus` is the address
  `nv_dma_bus()` published for that run.
* **Interface binding** — `g_rm_if.vm_resolve = mock_vm_resolve` (`:842`), in the
  runtime-built table (the `.rodata`/slide hazard that table exists for is
  unchanged and the ARM tier's relocation gate still passes: §7).

**Held to values in the tier, not to a paragraph** (SOURCE:
`src/kernel/nv_shim_test.c:306-337`, `:446-450`). After the probe binds 64 KiB
at `0x10000000`, `test_nvshim_probe` asserts:

* `vm_resolve(va=0x10000000)` = 0, `phys != 0`, `mapped == 0x10000`, `bus == phys`;
* `vm_resolve(va=0x10002000)` = 0 and `phys2 == phys + 0x2000`, `mapped ==
  0x10000 - 0x2000` — the walk translates and reports its extent;
* `vm_resolve(va=0x10010000)` (a hole) and `vm_resolve(va=0x100000000)` (the
  space's size) are `-EINVAL` — never clamped;
* a store of `0x5a5aa5a5` **at the resolved physical address** (`phys_to_virt`)
  is read back through the RM's object handle at the same offset with
  `nv_rm_mock_read_bo()` — two views of one memory, so the address the walk
  answered is the object's bytes and not an arithmetic coincidence;
* after UNMAP, the same `va` resolves to `-EINVAL` — the walk follows the
  mapping's lifetime.

---

## 6. GPU-visible layout numbers: none are needed, and none were retyped

The card's step 5 is conditional ("if a GPU-visible layout number is
required"). **It is not required for this object**, and the report states that
plainly rather than borrowing a number to look complete:

* The VA space's granularity (`MOCK_PAGE` 4 KiB, `nv_rm_mock.c:46`) and its
  limit (`MOCK_VA_LIMIT` 1 TiB, `:57`) are the **mock's own bookkeeping
  constants**, and they are labelled as such here. They are not presented as
  GB202's page size or GB202's VA width, and no GB202 boot measured them.
* Every number in the quoted rows is **measured in this run, on this tree** —
  the granted space (`0x100000000` requested, granted whole), the VA
  (`0x10000000`), the range (`0x10000`), and the physical addresses
  (`0x209686000` intel / `0x8ef46000` ARM). The two platforms disagree on the
  physical address, which is what a real read-back looks like; a retyped
  constant would not.
* **No AD102 number was retyped.** The mock device still *declares* the AD102
  identity it has always declared — `chipset 0x192`, `device_id 0x2684`,
  `name "AD102"`, `revision 0xa1`, topology 11 GPC / 64 TPC
  (SOURCE: `src/kernel/nv_rm_mock.c:239-255`) — because this card changed no
  identity path; that declaration is the node's, not this card's, and this card
  claims nothing about GB202's layout on the strength of it.
* For provenance only, and **not used as evidence here**: `docs/nv-plan.md`
  §2.7's GR topology is no longer only a model on GB202 — `t_b2131040` /
  `t_78a50be7` measured `gpc=11 tpc=64 src=register` from `0x409604` and
  `GPC_UNIT(i,0x2608)` (`docs/nv-games-ladder.md:45`,
  `docs/nv-topology-measured.md`). The VA object in this report does not consume
  that number, and nothing here re-derives it.

---

## 7. The tiers that produced the rows

Tested tree: branch `nv/shim-mock-vm`, code commit `7faed719`, cut from
`nv/integ-swarm` `ef591e93` (the engine-class card's tip). One source commit,
four files.

* intel (x86-64, TCG): `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1
  UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` —
  **`Tests run: 623` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`intel-vm-lane.log:3242-3244`, MEASURED).
* ARM (AArch64, TCG): `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1200
  ./run_unit_tests.sh` — **`Tests run: 486` / `Tests failed: 0` /
  `UNIT TESTS PASSED`** (`unit-tier-arm-vm.log:2482-2484`, MEASURED), with the
  relocation gate green in the same run:
  `hobbyos.elf: reloc gate OK -- 443 kernel pointer word(s), every one applied
  at boot` (`:10`, MEASURED).

Test counts are unchanged from the tip (623 / 486): this card adds assertions
inside existing test functions, not new ones. Both tiers print the same rows in
the same order, so the object is not an x86-only path; the tier is where the
"two views of one memory" check (§5) actually runs, on both architectures.

A note on the log files so the line numbers can be re-checked: each file is a
verbatim console capture from the run named, but they are not all the same
capture. `unit-tier-intel-vm.log` is the intel test script's own stdout (its status
lines, then the `qemu.log` it cats on success); the other three are the
`qemu.log` files those runs wrote, which begin with the `make run` recipe echo
before the guest's first line. Line numbers are therefore per file, and
`check-quotes-vm.py` verifies every citation in the file it names.

### 7.1 The merged tip (both lanes)

The code was merged onto `origin/nv/integ-swarm`, which had moved to
`44753b8a` (a docs-only ladder commit) while this card was running; the merge
commit is `8c22fe17` (parents `44753b8a` + `7faed719`). Both tiers were re-run
on that merged tree:

* intel: **`Tests run: 623` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-intel-merged.log:3237-3239`, MEASURED); the same rows at
  `:2356-2361`, `:2412-2419`, `:2455-2456`.
* ARM: **`Tests run: 486` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-arm-merged.log:2482-2484`, MEASURED); the same rows at
  `:1630-1635`, `:1686-1692`, `:1729`, relocation gate at `:10`.

`check-quotes-vm.py` verifies every quoted row at its cited line in **all four
logs** (lane and merged, both tiers), all four verdict blocks, the row counts
that prove the budget did not swallow a row, and every `file:line` source
citation in this tree. This report itself arrives on the tip in `a31ff263`
(parents `8c22fe17` + `970a9227`, the report commit); later docs-only commits
refine its prose and cited line numbers, never its source. The tree the rows
were read on and the tree carrying this text therefore differ by markdown only —
no source change.

---

## 8. Closing state — and the proof that nothing booted

MEASURED on r1 at `2026-11-28T06:47:40Z`, read-only, after both tiers (lane and
merged tip):

```
status: stopped          # qm status 313
status: stopped          # qm status 300
qemu_count=0 rc=1        # pgrep -c qemu-system-x86
```

`pgrep -c` prints `0` and exits 1 when nothing matches, which is exactly the
measured state: no QEMU process exists. The guests' files were not touched by
this run — their mtimes are still the previous card's boot:

```
/root/hobbyos5090/disk.img    2026-11-27 23:39:01.541392178 -0600
/root/hobbyos5090/serial.log  2026-11-27 23:39:01.536392179 -0600
```

i.e. 2026-11-28 05:39:01Z, the `rm68` boot of card `t_97b8a05c` (started
05:38:44Z). Nothing in this card wrote them, so **zero boots** is a measurement,
not a claim.

---

## 9. What this does and does not prove

**Does:** the mock RM can allocate a VA space object and hand back a handle it
assigned; a VA bound to a memory object resolves, through the RM's own walk of
the binding, to a real physical address with a real extent; the address names
the object's bytes (a store there is visible through the object handle at the
same offset); holes, out-of-range VAs, unowned handles and bad ops are named
refusals with visible rows; the stock DRM face shows the same read-back; both
unit tiers are green on both architectures; every row and every sentence here
says `mock`.

**Does not:** it says nothing about GB202, GSP-RM, firmware, VRAM or L3. The
physical address is host pool RAM, not VRAM; the handle is an index in
`g_mock.vm[]`, not an RM object; no `NV_VGPU_MSG_*` is sent. The stock client's
engine-class wall is untouched (`need=gsp_rm`), so nvk still stops where the
wall card measured it. The firmware claim stays where `t_a769beaa` left it —
`MAILBOX0 = 0x0B`, no peer — and this card is **not** evidence for it. The next
card in the spine (the doc card `t_c5453775`) inherits the same rule: every row
and every sentence says `mock`.
