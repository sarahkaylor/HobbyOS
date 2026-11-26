# WP-PRIME — dma-buf/PRIME on the `/dev/nv*` shim

Card: `t_78fce566`. Base: `nv/integ-swarm` @ `4a70f062`. Branch: `nv/wp-prime`.

## 1. What this rung is, and what it is not

`/dev/nvshim` served nouveau's real `DRM_IOCTL_NOUVEAU_*` surface over a mock RM
with exactly one named gap left: both PRIME ioctls answered `-ENOSYS` by name
(`prime_handle_to_fd`, `prime_fd_to_handle`). That is the ABI a winsys needs to
*hand a buffer to someone else* — a compositor, a second process, the scanout
side — and without it a native nvk could enumerate, allocate, and still have no
way to present a frame.

This card closes that gap: an export produces a **real fd**, mmap of that fd is
**the same pool frames** as the GEM object (not a copy), an import gives a GEM
handle that maps those same pages, and the object's frames live until the last
reference (handle entry or exported fd) closes, in every close order tried.

It is an **ABI rung, not an fps row**. Nothing here says the driver can render:
the engine-class wall (`need=gsp_rm`, copy `0xc7b5`) is untouched, no GPU was
booted, and a PRIME round trip on a CPU-mapped BO is not evidence of 3D work.

## 2. The contract as implemented

`src/include/nv_drm_uapi.h` gained a `struct nvshim_drm_prime_handle` (12 bytes,
the uAPI's own `struct drm_prime_handle`) and the two generic DRM numbers:

| request | nr | encoded size | answer |
|---|---|---|---|
| `DRM_IOCTL_PRIME_HANDLE_TO_FD` | `0x2d` | `0xC00C642D` (12) | served |
| `DRM_IOCTL_PRIME_FD_TO_HANDLE` | `0x2e` | `0xC00C642E` (12) | served |

Both were **removed from the named-refusal table** (see §4) and added to the
row/size table with their exact 12-byte width, so a client built against
`include/uapi/drm/drm.h` passes the size check.

### Export (`HANDLE_TO_FD`)

* handle must be a GEM object **this client** owns (`-EINVAL` otherwise);
* flags may carry `DRM_CLOEXEC` / `DRM_RDWR` only (`-EINVAL` otherwise);
* the result is an ordinary fd in the caller's file table
  (`FILE_TYPE_DMABUF`, `fs.h`): `dup(2)`, `fork`, `close(2)` and `SCM_RIGHTS`
  all work through the machinery that already exists, because the fd **is** a
  file description;
* each export is its own fd and its own reference on the object.

### mmap of the exported fd

`mmap(2)` on the fd, offset 0, length ≤ object size, `MAP_SHARED`,
`PROT_READ[|WRITE]`. The pages are the **same pool frames** the exporting
handle maps: one `vm_object` of kind `VM_OBJ_NVBO` built by the same helper
(`shim_region_build`) the node-fd path uses, so "same pages" is one code path,
not two that agree by luck.

A VRAM-only object (no CPU mapping) **exports fine** — a device-visible buffer
is exactly what PRIME shares — and mmap of its fd refuses **by name**
(`domain_never_offered_a_cpu_mapping`).

### Import (`FD_TO_HANDLE`)

* `fd` must be a dma-buf fd this process holds, **including one received over
  `SCM_RIGHTS`** — `fs_msg_ref_gfd` + `fs_msg_install_gfd` (the calls
  `unix.c`'s sendmsg/recvmsg use) put the message's reference straight into the
  receiver's fd table; no new machinery was needed;
* `flags` must be 0 (`-EINVAL` otherwise, as in Linux);
* the returned handle is in the importing client's GEM table, maps the same
  pages, answers `GEM_INFO` truthfully, and can be bound to a VA space;
* an object exported by a **different device** is refused by name
  (`cross_device`) and never imported silently.

Documented divergence: handles on this node are RM-global, so an import returns
the object's own handle number. Importing the same fd twice in one client adds a
second table entry under the same number, and each `GEM_CLOSE` drops one.

### Lifetime

    refs = (client GEM-handle table entries) + (exported dma-buf fds)

The frames are freed exactly once, by whoever drops the last reference:
handle-then-fd, fd-then-handle, or a client teardown that takes every handle
while an fd is still open. A mapping is **not** itself a reference (this kernel
has no munmap hook to learn that a VMA went away), so a mapping is dropped by
its owner going away — the node fd's handle, or the dma-buf fd — or by the
object reaching `refs == 0`, whichever comes first, and never left pointing at
recycled frames.

## 3. Where the code is

| file | change |
|---|---|
| `src/include/nv_drm_uapi.h` | `struct nvshim_drm_prime_handle`, the two `NVSHIM_DRM_IOC_PRIME_*` macros, the "PRIME contract" block, the `NO_SHARE` paragraph moved from "refused at allocation" to "refused at export" |
| `src/include/nv_shim.h` | `nv_shim_dmabuf_mmap` / `nv_shim_dmabuf_release`, the `NVSHIM_PRIME_*` refusal codes, two kernel-tier seams, `NVSHIM_FEAT_PRIME` |
| `src/include/fs.h` | `FILE_TYPE_DMABUF`, `f->dmabuf.obj`, `file_dmabuf_install/_obj/_is_dmabuf` |
| `src/kernel/nv_shim.c` | the refcounted object table (`g_bos`, `bo_add/bo_ref/bo_unref`), the dma-buf table (`g_dmabufs`), both ioctl handlers, `nv_shim_dmabuf_mmap`, `nv_shim_dmabuf_release`, the shared `shim_region_build`, the map-owner sweep, and every free path (`op_free`, `op_drm_gem_close`, `client_teardown`) turned into a refcount drop |
| `src/kernel/fs.c` | install / resolve / predicate for the dma-buf fd type; the last-close hook in `file_close` **and** `fs_close_global` (a received fd discarded with no process context) |
| `src/kernel/vm.c` | one branch: `file_is_dmabuf(fd)` → `nv_shim_dmabuf_mmap` |
| `src/kernel/nv_shim_drm_test.c` | three new suites (§5) plus the two refusal-row updates |

`SHIM_MAX_BOS` is 64 (the mock RM's own table size); `SHIM_MAX_DMABUF` is 16.

## 4. Refusal-table diff

`nv_shim_drm_refusal_name()` (the `g_drm_refusals` table) **lost exactly two
rows**, the two this card serves:

    -        {NVSHIM_DRM_NR_PRIME_HANDLE_TO_FD, "prime_handle_to_fd"},
    -        {NVSHIM_DRM_NR_PRIME_FD_TO_HANDLE, "prime_fd_to_handle"},

and gained none: PRIME's refusals are per-object/per-call and name themselves on
the console through `nv_shim_prime_refusal_name()` — `no_share`,
`cross_device`, `not_a_dmabuf`, `domain_never_offered_a_cpu_mapping`, `flags`.
Every other named row is unchanged (the tier re-asserts `gem_flink`,
`setparam`, `gem_pushbuf`, `syncobj_handle_to_fd`, `syncobj_timeline_wait`…).

One **non-table** row changed and is called out for review: `NO_SHARE`.

* before: `GEM_NEW` refused `NOUVEAU_GEM_DOMAIN_NO_SHARE` with `-EINVAL`
  (`reason=geometry`), because PRIME did not exist and accepting the bit would
  have been the silent drop the project forbids;
* now: the object is **made** (and maps locally for its owner) and **export**
  refuses it by name (`no_share`). That is what the bit is for, and the card's
  acceptance asks for exactly that: "a `NO_SHARE` object must refuse export by
  name". The old expectation (`-EINVAL` at allocation) is now asserted the other
  way in `test_nvshim_drm_cpu_map_refusals`, with a comment saying why.

Nothing that returned `-ENOSYS` now returns 0: the two rows that changed
qualify return 0 only after doing the work (a real fd, a real handle), and the
only `-ENOSYS` rows that exist are the ones still in the table.

## 5. The suites (what passing means)

Three new tests in `nv_shim_drm_test.c`, all driven through the real route
(`nv_shim_ioctl` with the struct in the probe's user pages):

1. `test_nvshim_drm_prime_export` — export returns a real fd (file-table
   query, `O_RDWR`, `file_is_dmabuf`); **both directions** of "same pages":
   store through the fd's mapping → read through the handle's mapping and
   through `nv_rm_mock_read_bo()` (the RM's own frame view), then store through
   the handle's mapping at offset 64 → read through the fd's mapping and the RM;
   untouched pages still zero (one allocation, not two buffers); a second export
   is a second fd and a second reference; closing the fd takes its mapping down
   while the handle's mapping still reads the pattern.
2. `test_nvshim_drm_prime_import` — import into the same client (handle, GEM_INFO,
   mapping, same pages); **`SCM_RIGHTS`** hand-off to a second process
   (`fs_msg_ref_gfd` + `fs_msg_install_gfd`) with an import and a mapping there,
   and a write in process 2 read back in process 1 and in the RM; three close
   orders — importer-first, exporter's-fd-first, and client-teardown-with-an-fd-
   out — each asserting the live refcount (`nv_shim_test_prime_refs`) and that
   the frames are freed only at the last close (`nv_rm_mock_read_bo` → `-EINVAL`,
   mappings gone).
3. `test_nvshim_drm_prime_refusals` — the `no_share` export refusal (the object
   still maps locally); a VRAM object exports and imports but its fd refuses
   mmap by name; offset ≠ 0 and non-`MAP_SHARED` shapes; foreign/absent handles;
   the node fd and a bad fd are `not_a_dmabuf`; `flags` on both directions; the
   `cross_device` refusal (the one branch userspace cannot reach — the mock has
   one device, so a kernel-tier seam makes a live buffer claim another device;
   the refusal is `-EINVAL`, no reference leaks, and with its real device
   restored the same fd imports, so the refusal was about the device and nothing
   else); and the "every other named refusal keeps its row" sweep.

## 6. Measurements

Both tiers were run on the committed tree (`46603a46`), full tier, nothing
skipped but the two local-environment switches named below.

| gate | sha | result | log |
|---|---|---|---|
| `run_unit_tests_intel.sh` | `46603a46` | `Tests run:    597` / `Tests failed: 0` / **`UNIT TESTS PASSED`** | `evidence/nv/unit-tier-intel-prime-597.log` |
| `run_unit_tests.sh` (aarch64) | `46603a46` | `Tests run:    476` / `Tests failed: 0` / **`UNIT TESTS PASSED`** | `evidence/nv/unit-tier-arm-prime-476.log` |

Baseline at the parent sha `4a70f062` was **intel 594 / ARM 473** (plan §2.5 and
the `nv/wp-units` fold), so both runs are baseline **+3** — the three new
suites, one `tests_run++` each. `UNIT_TESTS_TIMEOUT=900` was not needed: both
runs finished inside the script's own deadline.

Local environment, stated because it is not CI's: this uid is not in group
`kvm`, so the intel tier ran TCG (`SKIP_KVM=1`, the same mode the parent's
`590/0` and this lane's `597/0` were measured in), and both runs used
`TOOLS_SKIP=1` because the in-OS LLVM dist-staging is a CI prefetch artefact
that does not exist in this checkout (`/TOOLS` is absent from the disk image;
the kernel unit tier does not use it). The ARM boot needed
`~/.local/share/AAVMF/AAVMF_CODE.fd`, which the Makefile expects and this host
ships under `/usr/share/AAVMF` — symlinked, no tree change.

Rows from the intel log (one per stage, quoted as they print):

    [NVSHIM] prime export handle=1 fd=4 size=0x2000 domain=0x9 buf=1
    [NVSHIM] prime mmap fd=4 buf=1 handle=1 va=0x1080006000 phys=0x209500000
    [NVSHIM] prime import buf=3 fd=4 handle=1 size=0x2000
    [NVSHIM] drm gem_close handle=1 kept refs=1 reason=prime_fd_open
    [NVSHIM] prime release buf=3 fd=4 last=1
    [NVSHIM] prime refuse dir=export handle=1 reason=no_share
    [NVSHIM] prime refuse dir=import buf=4 reason=cross_device bufdev=99 dev=1
    [NVSHIM] prime mmap refused fd=4 buf=6 domain=0x2 reason=domain_never_offered_a_cpu_mapping

CI: the branch `nv/wp-prime` was pushed at `c9b1fb2c` (the WP-PRIME code
commit, parent of this one) and `linux-ci` created run **45894370800** for that
sha; it sat `queued` for the whole window because the self-hosted Linux runner
`linuxci-hobbyos` reported **`offline`**, so no job ever started. The gate
evidence for this card is therefore the two local runs above — both arches,
full tier, on `46603a46` — and the run id is recorded here and on the card so
the next dispatch can quote whichever run the runner eventually executes. (For
context, the same runner's run for the parent sha `4a70f062` is red — job
`139348443154`, `Tests run: 594 / Tests failed: 1`, and the failure is
`[LOCKHEALTH-IRQOFF] cpu=0 max_us=117540`, the interrupt-masked budget, not a
test function; it did not reproduce here.)

## 7. Verification gaps (stated, not hidden)

* **No Mesa/nouveau client was linked or run against this.** The exporter side
  (`nouveau_bo_export` → `DRM_IOCTL_PRIME_HANDLE_TO_FD`) is exercised through the
  identical ioctl path, but "a stock winsys did it" is a source-level claim here,
  not a measurement.
* **No real second device.** The mock RM has one device (`MOCK_DEV_ID`), so
  `cross_device` is proven with a seam that lies about the exporter's device. When
  a real RM backend lands, that branch becomes reachable and the seam becomes
  unnecessary.
* **dma-buf is not a shared subsystem here.** What exists is a first-class fd
  whose mmap maps the exporter's frames and whose lifetime is refcounted. There is
  no `dma_buf_attach`/`sg_table`/importer-ops layer, because the one RM cannot
  express a second attachment; the refusal is named rather than faked.
* **ARM has no GPU boot in this card** (and needs none): the tier is the same
  kernel suite on the same mock.
* A mapping is not a reference (§2) — Linux would keep the object alive while a
  VMA exists; this kernel has no munmap hook, so the mapping dies with its owner
  or with the object instead of pinning it. The tests pin that behaviour.

## 8. Next shim gap (not this card)

`t_8acf4a21` — explicit fences: a syncobj exports to a fd and a timeline point
waits/signals. Sync files and the timeline half of syncobj remain the named
refusals this card deliberately did not touch.
