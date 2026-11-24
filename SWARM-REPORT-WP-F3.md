# SWARM-REPORT — WP-F3: CPU-mapped BOs for `/dev/nvshim`

Branch `nv/wp-f3-gemmap`, cut from `756c8c9d` (`nv/integ-swarm`, round 2 merged,
**578 run / 0 failed**).
Scope: the *CPU-mapped BOs* item round 2's report named as a blocker for a stock
nvk (`SWARM-REPORT-WP-F2.md` §5). Rung **N2** (`PLAN.md` §4).

**Status: gate met, MEASURED.** A client creates a GEM object whose domain asks
for a CPU mapping, `mmap(2)`s it on the node, writes a pattern through the
mapping, and the pattern is then read back **through the kernel's view of the
object** — the RM's own frame address — and **through EXEC**, which fetches the
push buffer out of that same memory and digests it. No flush, no write-back
request, no `msync`: the mapping's pages *are* the object's frames.

Claims are tagged MEASURED (seen in a log from this branch), SOURCE (read from a
cited contract) or INFERRED. Evidence files: `evidence/nv/f3-unit-tier-580-cpumap.log`,
`evidence/nv/f3-red-wrong-frame.log`.

The honest caveat up front: **nothing userspace has ever mapped this.** The gate
runs in the kernel unit tier, where the client's "user space" is a real v2
address space built by the test harness and the writes go through the real page
tables (`vm_kwrite`), but no ELF binary has called `mmap(2)` on `/dev/nvshim`
yet. §6 says what that leaves unproven.

---

## 1. What changed

| File | What |
|---|---|
| `src/include/nv_rm.h` | `NV_RM_DOM_*` (nouveau's domain bits, one spelling); `mem_phys()` — the CPU view of an object, `-ENOTSUP` when the domain never offered one; `read_va()` — a GPU VA resolved to memory, the only way EXEC can see a push buffer; `nv_rm_mock_read_bo()`, the kernel-tier seam that reads an object through its own frame run |
| `src/kernel/nv_rm_mock.c` | an object is now a **contiguous run of pool frames** (`nv_dma_alloc`: zeroed, page-aligned, bus via `nv_dma_bus`), freed at `mem_free`; `mappable` recorded from the domain; bindings record `bo_offset`; `read_va()` page-wise with `-EFAULT` for a binding with no memory |
| `src/include/vm.h`, `src/kernel/vm.c` | `VM_OBJ_NVBO` (an object whose pages are a device allocation, which it does **not** own and which are never zero-filled); `vm_object_install_foreign()`; the two free paths skip an NVBO's frames; `vm_mmap()` routes an `/dev/nvshim` fd to `nv_shim_mmap()` |
| `src/include/nv_shim.h` | `nv_shim_mmap()`; the two kernel-tier seams (`nv_shim_test_last_exec`, `nv_shim_test_sum`) |
| `src/include/nv_drm_uapi.h` | the **mmap contract** written out (offset is `map_handle`, MAP_SHARED only, length ≤ object, READ ± WRITE); `map_handle` documented as `handle << 12` or 0 |
| `src/kernel/nv_shim.c` | domain table + `drm_domain_ok()` / `drm_domain_cpu_mappable()` / `drm_map_handle()`; `map_handle` out of GEM_NEW and GEM_INFO; `nv_shim_mmap()`; a `g_maps[]` table so GEM_CLOSE / fd close unmap before the frames are freed; EXEC fetches every push through `read_va()` **before** enqueuing any, digests it, and reports it in the `drm exec` row |
| `src/kernel/nv_shim_drm_test.c` | two suites: the gate probe and the refusal matrix; one round-2 limit expectation updated (§4) |
| `evidence/nv/f3-*.log` | the green gate section and the red mutation section, trimmed from the full runs |

No driver code was copied. `map_handle`/`drm_vma_node_offset_addr` and the domain
bit values come from `include/uapi/drm/nouveau_drm.h` and DRM's VMA-node
convention (SOURCE).

---

## 2. The request table this branch adds: none, and why

DRM has no mmap request row — a GEM object is mapped with `mmap(2)` on the node,
and the object is named by the **offset**. Round 2's uAPI already had the field
for it (`drm_nouveau_gem_info.map_handle`) and always answered 0. So the surface
this branch adds is a *contract* on two existing rows plus the file's `mmap`
op, not a new number; the request-number discipline is untouched (still 16 served
rows, still 21 named refusals).

### 2a. `GEM_NEW` domain table (MEASURED: `test_nvshim_drm_cpu_map_refusals`)

| domain bits | answer | why |
|---|---|---|
| `VRAM`, `GART` | `0`; frames allocated, `map_handle = 0` | memory for the device; this port maps no BAR, so no CPU mapping is promised |
| `CPU` | `0`; frames, `map_handle = 0` | system memory, and the client did not say it will map it |
| `CPU \| MAPPABLE` | `0`; frames, `map_handle = handle << 12` | the mappable shape |
| `... \| COHERENT` | allowed alongside `MAPPABLE` | the frames are Normal-NC pool RAM the device already sees, so "coherent" is a fact here, not a flag to honour |
| `MAPPABLE` without `CPU` | `-EINVAL`, row `reason=geometry` | there is no BAR to map VRAM through |
| `COHERENT` without `MAPPABLE` | `-EINVAL` | a coherence promise with no mapping |
| `NO_SHARE` | `-EINVAL` | the bit exists to tell PRIME not to export the object, and PRIME is refused by name — accepting it would be a silent no-op |
| no domain bit at all | `-EINVAL` | nothing to allocate from |

### 2b. The mmap contract (MEASURED: the gate probe, and 8 named refusals)

| mmap argument | accepted | refused (errno, row reason) |
|---|---|---|
| `offset` | `map_handle` of an object this client owns | `0` → `-EINVAL no_such_object`; unaligned → `-EINVAL no_such_object`; a handle it does not own → `-EINVAL not_this_clients_object` |
| `length` | page-aligned, `≤` object size (shorter is legal: the first *length* bytes) | `0`/unaligned → `-EINVAL length`; past the object → `-EINVAL beyond_object` |
| `prot` | `READ`, optionally `WRITE` | no `READ` → `-EINVAL protection`; `EXEC` → `-EINVAL protection` |
| `flags` | `MAP_SHARED` only | `MAP_PRIVATE`/`MAP_ANONYMOUS`/`MAP_FIXED` → `-EINVAL needs_shared` |
| object | domain offered a CPU mapping **and** the RM has pool memory | `-EINVAL domain_never_offered_a_cpu_mapping`; `-ENOTSUP`/`-EINVAL no_cpu_memory` |

Every refusal is one `[NVSHIM] drm mmap refused … reason=<name>` row
(MEASURED, log lines 2270-2277 of the green run).

---

## 3. Gates

### 3.1 Intel build (MEASURED)

```
$ nvswarm build wp-f3
exit 0, 0 errors   (warnings in vm.c/nv_dma.c are pre-existing)
```

### 3.2 Unit tier (MEASURED)

```
$ nvswarm sh wp-f3 'TOOLS_SKIP=1 BROWSER_SKIP=1 ./run_unit_tests_intel.sh'
==================================
Tests run:    580
Tests failed: 0
UNIT TESTS PASSED
==================================
```

**578 → 580** (+2 suites: `test_nvshim_drm_cpu_map`, `test_nvshim_drm_cpu_map_refusals`).
Every pre-existing suite still passes, including the round-1 `'N'` face.

### 3.3 ARM per-TU (MEASURED; the full ARM link is still another lane's)

```
$ nvswarm sh wp-f3 'TOOLS_SKIP=1 BROWSER_SKIP=1 MODE=unit_tests LO_ON_DISK= \
    make ARCH=arm obj/arm/nv_shim.o obj/arm/nv_rm_mock.o obj/arm/vm.o \
    obj/arm/nv_shim_drm_test.o'
arm=0   (all four objects built)
```

`nv_dma_alloc`'s VT-d query is `#ifdef __x86_64__`, so the mock links on AArch64
with translation reported off; nothing in the new path is x86-only.

### 3.4 The gate probe — what it actually observes (MEASURED, `f3-unit-tier-580-cpumap.log`)

```
[NVSHIM] drm gem_new handle=1 size=0x2000 offset=0x209500000 domain=0x9 map=0x1000
[NVSHIM] drm mmap handle=1 va=0x1080004000 len=0x2000 phys=0x209500000 domain=0x9
[NVSHIM] drm exec ch=1 pushes=1 sigs=0 bytes=64 stream=0x6957b385 fence=0x1
[NVSHIM] drm exec ch=1 pushes=1 sigs=0 bytes=64 stream=0x392d7385 fence=0x2
[NVSHIM] drm mmap handle=1 va=0x1080006000 len=0x1000 phys=0x209500000 domain=0x9
  [NVSHIM] cpu-mapped BO: fault+write+kernel-view+exec digest: PASS
```

* `domain=0x9` = CPU|MAPPABLE; `map=0x1000` = handle 1 << 12.
* The mapping landed at `0x1080004000`, 0x2000 bytes, over
  `phys=0x209500000` — the object's run, not a copy.
* `stream=0x6957b385` is **FNV-1a 32 of the test's first 64 pattern bytes**,
  recomputed independently from the pattern definition in Python:
  `pat[i] = 0xA5 ^ ((7i+1) & 0xFF)` → `0x6957b385`. `stream=0x392d7385` is the
  same for the second pattern (`0x3C+i`) written at **object offset 64** — so
  the digest is not a constant and the submit read the VA it was given.
* (Cross-check on an untouched object: the round-2 probe's VRAM BO, never
  written, digests to `0xdfde6ac5` = FNV-1a of 64 zero bytes — the frames really
  are zeroed at allocation.)

What the probe asserts, and the fake each assertion rules out:

| assertion | what it excludes |
|---|---|
| no leaf present before the first store, present after it (`vm_arch_walk`) | an *eager* mapping — present/present would pass a mapping that never faults |
| `nv_rm_mock_read_bo()` equals the pattern | a mapping that is a copy, or that points at a different frame |
| 64 bytes at object offset 256 still zero (prefilled 0xFF before the read) | a splatter write, or a read at the wrong offset that "happens" to match |
| a second, shorter mapping of the same object sees the first one's store | per-mapping private copies |
| `GEM_CLOSE` → both VAs answer `-EFAULT` and their regions are gone | a leaf outliving its frames (use-after-free, not a leak) |
| `nv_shim_test_last_exec()` is `-ENOENT` before any EXEC | a digest that is true by construction |

### 3.5 Mutation — the mapping pointed one page up (MEASURED, red then green)

Mutation applied to `nv_shim_mmap()`: `vm_object_install_foreign(obj, off,
phys + off + SHIM_PAGE)` — every mapping page names the frame one page above the
one the object holds.

```
EXPECT_EQ FAILED: diff == 0 at src/kernel/nv_shim_drm_test.c:1062
  Left:  255
  Right: 0
...
Tests run:    580
Tests failed: 2
UNIT TESTS FAILED
```

255 of 256 bytes differ between the mapping's store and the kernel's view of the
object: the gate is the assertion that saw it. The second failure
(`nv_rm_mock_live() == 0`, `Left: 4`) is collateral — `EXPECT_EQ` returns from
the test function on the first failure, so that test's client is never torn down
and the *next* test sees four live objects. Then reverted, rebuilt, re-run:
**580 / 0** again (`evidence/nv/f3-red-wrong-frame.log`,
`f3-unit-tier-580-cpumap.log`).

---

## 4. One round-2 expectation changed, on purpose

`test_nvshim_drm_limits` asserted `GEM_NEW {domain = CPU}` → `-EINVAL` (round 2
refused every CPU bit). Under nouveau's uAPI a domain is a preference set and
`CPU` alone means "system memory", with `MAPPABLE` the separate "I will map it"
hint — so the honest answer now is an object with memory and `map_handle = 0`.
The assertion was updated in place with the reason beside it; the test's other
seven geometry cases are unchanged. `NVSHIM_DOMAIN_CPU|MAPPABLE|COHERENT` with
`map_handle != 0` is a new case in the refusal suite.

The `'N'` face is untouched: `nv_shim_alloc_ok()` still accepts VRAM/GART only,
so round 1's probe and its five suites keep their behaviour.

---

## 5. What a real nvk still needs (after this)

Ordered by when a stock client hits it:

1. **NVIF** — every RM class object (`DRM_IOCTL_NOUVEAU_NVIF`, refused by name).
   CHANNEL_ALLOC is the legacy path; nvk's nouveau winsys asks for
   `NVIF_CLASS_*` objects (device, engine, channel) and drives the channel
   through them. This is the next blocker and it needs a class model the mock RM
   does not have.
2. **PRIME** — `HANDLE_TO_FD` / `FD_TO_HANDLE` and dma-buf. Refused by name; it
   also needs `NO_SHARE` to mean something (§2a).
3. **`GEM_PUSHBUF` / relocations** and the legacy push path, refused by name.
   EXEC-by-VA is what nvk uses, so this is lower priority than NVIF.
4. **The real GSP-RM behind the same `struct nv_rm_if`** — the seam is unchanged
   by this branch (`mem_alloc`/`mem_phys`/`read_va`/`submit_va`), which is what
   it was designed for.

Sharp edges of what this branch *does* serve, worth knowing before nvk tries it:

* A mapping is **MAP_SHARED only**. Fine for nvk's memory; a client that asks
  for `MAP_PRIVATE` gets `-EINVAL` rather than a silent copy.
* **`GEM_CLOSE` unmaps.** Linux keeps the object alive while a VMA refers to it;
  this node has no object refcount, so the order is inverted (documented in
  `nv_drm_uapi.h`). A client that writes to a mapping after closing the handle
  sees `-EFAULT`, not stale memory — safe, but not Linux's lifetime rule.
* The device address is the physical address while VT-d translation is off
  (`nv_dma_bus()`), and with translation **on** every allocation is `-ENOTSUP`,
  so `GEM_NEW` fails rather than publishing a bus address that means nothing.

---

## 6. What is NOT verified

* **No userspace program has mmap'd the node.** The gate is the kernel tier: the
  pages, the fault path, the page tables and the RM frames are all real, but the
  `mmap(2)` came from `vm_mmap()` via the test harness, not from a libc in EL0.
  No `.bin` probe exists yet.
* **No stock nvk, no `vulkaninfo`.** NVIF is still refused (§5); this branch
  removes one of three blockers, not all.
* **The GPU never reads the bytes.** The mock has no engine: `read_va()` proves
  the kernel can fetch the stream from the object's frames, not that a 3D engine
  can. Nothing here says anything about the card.
* **Contiguity.** Every object is one `nv_dma_alloc` run, so a large BO can fail
  `-ENOMEM` where round 2's software arena always succeeded. Measured sizes here
  are 4 KiB – 64 KiB; the per-client cap is still 512 MiB but a contiguous run
  that size is not attempted (INFERRED: `frame_alloc_contig` would fail long
  before).
* **No fork, no concurrency, no big-object test.** The NVBO path is reached by
  the shared-object branch of the clone and fault paths (both pre-existing), but
  no test forks a process that has a BO mapped, and no test races two threads
  faulting the same BO page.
* **ARM is compiled, not run.** The tier gate is intel only (as for every WP).
* **`EXEC`'s "validation" is a fetch plus a digest, not command decoding.** The
  shim now proves the bytes exist in memory and hands a digest to the caller;
  it does not parse a push stream (there is no engine to accept one).

---

## 7. Next step

The highest-leverage move for rung N2 is now **NVIF**: a table of RM classes over
the same mock objects, driven by `DRM_IOCTL_NOUVEAU_NVIF`'s ioctl-shaped payload
(`NVIF_IOCTL_*`), which is what turns CHANNEL_ALLOC's legacy path into the object
model a stock nvk actually uses. After that, PRIME.

If the next round prefers to stay on this branch's surface: an EL0 probe binary
that `open`s `/dev/nvshim`, GEM_NEWs a CPU|MAPPABLE BO, `mmap`s it, writes a
pattern, EXECs it and prints the digest would close the "no userspace program has
ever mapped it" gap — the kernel half is ready for it.
