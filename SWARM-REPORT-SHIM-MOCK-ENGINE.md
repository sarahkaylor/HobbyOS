# SWARM-REPORT: one engine class object from the mock RM — card t_7cf2e538

Card: **t_7cf2e538**, "Boot GB202 and allocate one engine class object from GSP-RM".
Re-scoped by the owner on 2026-11-28: **the mock RM is acceptance for this
object; the firmware version of the claim stays parked on the GB202 ACR wall**
(`t_a769beaa`, revive condition R2). **Zero boots**: VM 300 / 313 / 314 / 315
were not started.

Tag on every claim: **MEASURED** = read out of a log or a live machine in this
run, with the file and line; **SOURCE** = read out of this tree's source, with
`file:line`; **INFERRED** = a conclusion drawn from those two, labelled as
such.

---

## 0. The hypothesis, answered

Original hypothesis: *"GSP-RM on GB202 allocates one engine class object for our
driver, with the handle coming from firmware."*

**Falsified as written, and deliberately not attempted.** No boot was run
(owner instruction, 2026-11-28), and there is no GSP-RM peer on GB202 to ask:
the wall card's ladder ends at `MAILBOX0 = 0x0B` with `linked=0`,
`initdone=0`, `statq_written=0` in every boot, unchanged by the later Tier A
boots (`t_97b8a05c`).

What this card delivers instead is a **mock** object: one engine class object
minted by `nv_rm_mock` through a new explicit request on `/dev/nvshim`. It is
**not**, and must not be quoted as, "GSP-RM allocated", a firmware handle, or
L3 evidence. Every row quoted below carries `mock=1` in its own text.

**Which class, and why that one.** The card asks for one engine class; the one
asked for is **`0xC7B5` (`NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B`,
`ampere_dma_copy_b`, engine instance `copy1`)**. That is the exact class a stock
nvk requests first: `nouveau_ws_context_find_class(classes, 0xb5)` takes the
highest class whose low byte matches and gets copy `0xC7B5`
(SOURCE: `docs/nv-shim-census.md` §7, `nouveau_context.c:167`), and on Ada it is
the CE allocation path (`rm->api->ce->alloc`, `NVC0B5_ALLOCATION_PARAMETERS`,
SOURCE: `src/kernel/nv_shim.c:5296-5298`). A second object of the 3-D class
`0xC797` is allocated in the same boot only to prove the handle counter moves —
it is not a second claim.

---

## 1. Part identity — read-only, nothing booted

MEASURED on r1, 2026-11-28 06:18:56Z (read-only `qm config` / `lspci`, no VM
started):

```
cores: 8
hostpci0: 0000:41:00,pcie=1
memory: 16384
name: hobbyos-nv5090
41:00.0 VGA compatible controller [0300]: NVIDIA Corporation GB202 [GeForce RTX 5090] [10de:2b85] (rev a1)
```

VM 300, for contrast, still passes through `0000:01:00` (the AD102):
`hostpci0: 0000:01:00,pcie=1`. **The mock run below never touched either card**
— it ran on the unit-tier guests (x86-64 and AArch64 TCG, no PCI passthrough),
which is why the handle is a mock object and not a GB202 one.

---

## 2. What the shim could not express, and the extension it got

Before this card the shim had no way to hold an engine class object at all: the
only engine-class numbers in the tree were SCLASS's report and an NVIF NEW
refusal (`need=gsp_rm`). There was no request that *allocated* one, so the
card's evidence shape (request row, reply row, RM handle) was inexpressible.
The shim was extended (SOURCE, one file pair):

* **RM seam** (`src/include/nv_rm.h`): `engine_class_create(dev, ch, oclass,
  &handle)` and `engine_class_destroy(dev, handle)` — the mock's stand-in for
  the Ada path's `nvkm_gsp_rm_alloc(chan, handle, class, ...)`. The handle is an
  **out** parameter because on the real path the RM stamps it.
* **Mock backend** (`src/kernel/nv_rm_mock.c`): an object table
  (`MOCK_MAX_ENGINE_CLASS 16`) whose handles come from a monotonic counter,
  `e->handle = ++g_mock.next_ec` (SOURCE: `nv_rm_mock.c:576`). The object is
  parented on the channel: `mock_channel_destroy` frees every class object of
  that channel, and `nv_rm_mock_live()` counts them, so a leak fails the tier.
* **ABI rows** (`src/include/nv_shim.h`, `src/kernel/nv_shim.c`):
  `NVSHIM_IOC_ENGINE_CLASS_ALLOC` / `NVSHIM_IOC_ENGINE_CLASS_FREE`, each with a
  fixed struct (`src/kernel/nv_shim.c:1445` is the alloc handler). The class id
  is validated against SCLASS's own table before the RM is called, so a class
  the chip does not have is `-EINVAL` at the lookup.
* **Lifetime**: the client's table records `(handle, parent ch)`. `CHANNEL_FREE`
  drops the client's entries with the RM's cascade, explicit FREE removes one,
  and `client_teardown` destroys them **before** the channels.

**Why the stock NVIF path is deliberately untouched.** NVIF NEW of an engine
class still answers `-ENOSYS` with `need=gsp_rm`; a fake class object handed to
a real client would let nvk build a command stream nothing reads, which is the
refusal's whole point. This is proven still true in the same boot, below. The
new rows are HobbyOS-only (`'N'` face) and the header records the 2026-11-28
exception and its reason (SOURCE: `src/include/nv_shim.h:54-61`).

No change to any existing object's behaviour: same RM calls for device, memory,
VM and channel; 376 insertions / 5 deletions across five files, all additive to
the seam and the test/ABI tables.

---

## 3. The rows, quoted verbatim

Both tiers of this checkout ran the change. The quoted pair is from the intel
tier's console log; the ARM tier printed the same rows in the same order, at
the lines cited.

### 3.1 The engine class request row

`unit-tier-intel-ec.log:2349` (MEASURED), in `test_nvshim_probe`, one line after
the channel reply that created `ch=1` (`:2348`):

```
[NVSHIM] engine_class req mock=1 ioc=engine_class_alloc ch=1 class=0xc7b5 name=ampere_dma_copy_b engine=copy1 alloc=ce_alloc
```

ARM, same row shape: `unit-tier-arm-ec.log:1639` (MEASURED)

```
[NVSHIM] engine_class req mock=1 ioc=engine_class_alloc ch=1 class=0xc7b5 name=ampere_dma_copy_b engine=copy1 alloc=ce_alloc
```

### 3.2 The engine class response row — the object handle

`unit-tier-intel-ec.log:2350` (MEASURED):

```
[NVSHIM] engine_class rsp mock=1 ch=1 class=0xc7b5 handle=1
```

ARM, `unit-tier-arm-ec.log:1640` (MEASURED):

```
[NVSHIM] engine_class rsp mock=1 ch=1 class=0xc7b5 handle=1
```

**The handle is `handle=1`.** It is the mock RM's first class object of the
boot, stamped by `++g_mock.next_ec`, not chosen by the client — the client's
request struct leaves `handle` untouched and only receives it.

A second allocation in the same boot proves the counter is the RM's
(`unit-tier-intel-ec.log:2351-2352`, ARM `:1641-1642`, MEASURED):

```
[NVSHIM] engine_class req mock=1 ioc=engine_class_alloc ch=1 class=0xc797 name=ampere_b_gr engine=eng3d alloc=rm_alloc
[NVSHIM] engine_class rsp mock=1 ch=1 class=0xc797 handle=2
```

The explicit free (`unit-tier-intel-ec.log:2355`, ARM `:1645`, MEASURED):

```
[NVSHIM] engine_class free mock=1 handle=2
```

The first object (`handle=1`) is deliberately left to its channel: it is freed
by `CHANNEL_FREE`'s cascade later in the same test, and `nv_rm_mock_live()`
returning 0 after teardown is what proves the cascade ran.

### 3.3 The refusals, also named

`unit-tier-intel-ec.log:2353-2354` (ARM `:1643-1644`, MEASURED):

```
[NVSHIM] engine_class refused class=0x1234 reason=no_such_class
[NVSHIM] engine_class refused reason=not_this_clients_channel
```

### 3.4 The wall this card did NOT move

`unit-tier-intel-ec.log:2961` (ARM `:2251`, MEASURED) — the stock client path in
the same boot, still refused by name:

```
[NVSHIM] nvif new refused class=0xc7b5 name=ampere_dma_copy_b need=gsp_rm missing=rm_chan_object alloc=ce_alloc engine=copy1
```

---

## 4. The ids used — quoted as they appear

There is **no GSP-RM message id** on this path, and saying otherwise would be
the fabricated reply the card forbids: `NV_VGPU_MSG_*` appears in this tree only
in comments (`src/include/nv_rm.h:70`, `src/kernel/nv_gsp.c:1148`) and nothing
sends one (SOURCE). The mock has no message protocol — no queue, no packet, no
console.

The ids that *are* on the path, quoted from the headers as written:

* `src/include/nv_shim.h:312`
  ```
  #define NVSHIM_IOC_ENGINE_CLASS_ALLOC \
    NVSHIM_IOWR(NVSHIM_IOC_MAGIC, 18, struct nvshim_engine_class)
  ```
  with `#define NVSHIM_IOC_MAGIC 'N'` (`nv_shim.h:106`). This is the request the
  row above names as `ioc=engine_class_alloc`.
* `src/include/nv_shim.h:314`
  ```
  #define NVSHIM_IOC_ENGINE_CLASS_FREE \
    NVSHIM_IOW(NVSHIM_IOC_MAGIC, 19, struct nvshim_engine_class_free)
  ```
* The class asked for, `src/include/nv_nvif_uapi.h:221`:
  ```
  #define NVSHIM_NVIF_CLASS_AMPERE_DMA_COPY_B       0x0000c7b5 /* copy */
  ```
* The object id itself: the `handle=` field of the reply row, which the RM
  assigned — not the client (SOURCE: `src/kernel/nv_rm_mock.c:576`).

**For the parked firmware claim only (SOURCE, not sent):** on the real Ada path
the object is an RM allocation under the channel's RM object
(`nvkm_gsp_rm_alloc`; CE through `rm->api->ce->alloc`), and an RM handle comes
back in that call. Quoting a `NV_VGPU_MSG_FUNCTION_*` id for it here would imply
a message was sent; none was, so this report names none.

---

## 5. Where the handle comes from

SOURCE, `src/kernel/nv_rm_mock.c:548-595`: `mock_engine_class_create()` checks
the object is requested on a live channel of the same device, takes the first
free slot, and stamps `e->handle = ++g_mock.next_ec` (`:576`). The interface is
named `"mock"` (`nv_rm_mock.c:801`) and `nv_shim.c:513` binds the whole node to
it (`static const struct nv_rm_if *rm(void) { return nv_rm_mock_if(); }`). So
`handle=1` in §3.2 is a **mock** handle: an index in `g_mock.ec[]`, not a
GSP-RM object.

---

## 6. The tiers that produced the rows

Tested tree: branch `nv/shim-mock-engine`, code commit `5e0887a7`, against
`nv/integ-swarm` `63a44fab` (the channel card's merge). One source commit,
five files.

* intel (x86-64, TCG): `TOOLS_SKIP=1 BROWSER_SKIP=1 SKIP_KVM=1
  UNIT_TESTS_TIMEOUT=1200 ./run_unit_tests_intel.sh` —
  **`Tests run: 621` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-intel-ec.log:3210-3212`, MEASURED).
* ARM (AArch64, TCG): `TOOLS_SKIP=1 BROWSER_SKIP=1 UNIT_TESTS_TIMEOUT=1200
  ./run_unit_tests.sh` — **`Tests run: 486` / `Tests failed: 0` /
  `UNIT TESTS PASSED`** (`unit-tier-arm-ec.log:2470-2472`, MEASURED).

Both tiers are the scripts' own console output, kept verbatim in the evidence
directory. The stage runs in both, so the object is not an x86-only path
(`nv_shim.c` and `nv_rm_mock.c` have no `__x86_64__` guard). The ARM tier also
links with the kernel relocation audit, so the runtime-built RM table
(`nv_rm_mock.c`'s `mock_if_fill`) still carries its slide with two more entries.

### 6.1 The merged tip (both lanes)

The code was merged onto `origin/nv/integ-swarm`, which had moved to the
census-gate lane's `6cd6239f`: merge commit `1c0c3c0f` (parents `930a8a5d` +
`6cd6239f`; `930a8a5d` is the engine-class merge, parents `63a44fab` +
`f63b5776`). Both tiers were re-run on that merged tree:

* intel: **`Tests run: 623` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-intel-merged.log:3226-3228`, MEASURED), engine-class rows at
  `:2366-2372`, the NVIF wall still refused at `:2978`.
* ARM: **`Tests run: 486` / `Tests failed: 0` / `UNIT TESTS PASSED`**
  (`unit-tier-arm-merged.log:2470-2472`, MEASURED), engine-class rows at
  `:1639-1645`, the NVIF wall still refused at `:2251`.

The two extra intel tests are the census lane's; this card adds assertions to
existing tests, not new test functions. The merged tip differs from `1c0c3c0f`
by this report only.

---

## 7. Closing state — and the proof that nothing booted

MEASURED on r1 at 06:18:56Z, read-only, after both tiers:

```
status: stopped          # qm status 313
status: stopped          # qm status 300
qemu_count=0 rc=1        # pgrep -c qemu-system-x86
```

`pgrep -c` prints `0` and exits 1 when nothing matches, which is exactly the
measured state: no QEMU process exists. A second read-only check after the
merged-tip reruns, at 06:29:37Z, returned the same three answers and the same
mtimes below. The guests' files were not touched by this run — their mtimes
are still the previous card's boot:

```
/root/hobbyos5090/disk.img    2026-11-27 23:39:01.541392178 -0600
/root/hobbyos5090/serial.log  2026-11-27 23:39:01.536392179 -0600
```

i.e. 2026-11-28 05:39:01Z, the `rm68` boot of card `t_97b8a05c` (started
05:38:44Z). Nothing in this card wrote them, so **zero boots** is a
measurement, not a claim.

---

## 8. What this does and does not prove

**Does:** the mock RM can allocate an engine class object on a channel and hand
back a handle it assigned; the shim expresses the request and the reply as rows
that each say `mock=1`; the object is parented on the channel and dies with it;
the class id comes from SCLASS's table, so a class the chip does not have is a
named refusal; both unit tiers are green; the rows are byte-quoted from the
logs.

**Does not:** it says nothing about GB202, GSP-RM, firmware, WPR2 or L3. The
stock client's path is unchanged — `NVIF NEW class=0xc7b5` is still refused
`need=gsp_rm` (§3.4), so nvk still stops exactly where the wall card measured it.
The firmware claim stays where the wall card left it — `MAILBOX0 = 0x0B`, no
peer — and this card is **not** evidence for it. The next card in this
serialised spine (VA space) builds on the same shim and inherits the same rule:
every row and every sentence says `mock`.
