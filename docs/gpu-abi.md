# `/dev/gpu` — the HobbyOS GPU ABI (M1)

Status: **M1 freeze, lane `rw-l68-virglm1`.** This is the userspace ABI the
virgl winsys (`M2`, `docs/rimworld-virgl-plan.md` §4.2) will be the first
consumer of. It exposes only what M1's kernel transport implements: capsets,
contexts, 3D resources with a user page list, fenced submits, transfers, and
resource teardown. Presentation (`SET_SCANOUT` of a 3D resource, cursorq) is
M5 and is deliberately absent.

Companion code: `src/include/gpu_abi.h` (the shared UAPI header),
`src/kernel/gpu_abi.c` (dispatch + client table + page resolution),
`src/kernel/virtio_gpu_3d.c` (the virtio-gpu 3D command surface),
`tools/gate/m1_virgl.py` (the M1 boot gate).

## 1. Shape

`/dev/gpu` is a character-device node in the same class as `/dev/fb0`
(`src/kernel/fs.c`). There are no new syscall rows: `SYS_MAX` is 104 and
frozen, and two of the calls carry variable-length blobs. A client does:

```
fd = open("/dev/gpu", O_RDWR);
ioctl(fd, GPU_IOC_GET_CAPSET_INFO, &info);      /* capset id/version/size */
ioctl(fd, GPU_IOC_GET_CAPSET,      &caps);      /* the blob */
ioctl(fd, GPU_IOC_CTX_CREATE,      &cc);        /* -> ctx_id */
ioctl(fd, GPU_IOC_RES_CREATE,      &rc);        /* -> res_id */
ioctl(fd, GPU_IOC_RES_ATTACH,      &ra);        /* {va,len} -> page list */
ioctl(fd, GPU_IOC_SUBMIT,          &sub);       /* -> fence_id */
ioctl(fd, GPU_IOC_FENCE_WAIT,      &wait);      /* fence_id */
ioctl(fd, GPU_IOC_TRANSFER,        &xfer);      /* TO/FROM host */
ioctl(fd, GPU_IOC_RES_UNREF,       &unref);
ioctl(fd, GPU_IOC_CTX_DESTROY,     &cd);
close(fd);                                      /* also tears the ctx down */
```

The ioctls are reached through the Linux-ABI `ioctl(2)` handler
(`src/kernel/linux_compat_fd.c`, the same route `/dev/fb0` uses), because the
M2 consumer (Mesa's libGL) runs inside the game's Linux-ABI process. A native
HobbyOS syscall row is not added in M1; if the controller wants one later it
maps 1:1 onto rows above 104.

**One client = one `(process-group pid, fd)` pair, at most one context.**
A second `GPU_IOC_CTX_CREATE` on the same fd is `-EBUSY`. Resources belong to
the client's context; every id-taking request checks ownership, so one
process cannot submit against or free another's objects. Closing the fd
destroys the context and every resource still attached to it.

## 2. Requests

Numbers use the Linux `_IOC` encoding, defined locally in `gpu_abi.h`:
magic `'g'`, 2-bit direction, 14-bit size, 8-bit type, 8-bit number.

| request | arguments | emits (virtio-gpu) |
|---|---|---|
| `GPU_IOC_GET_CAPSET_INFO` | `struct gpu_capset_info` | `GET_CAPSET_INFO` (0x0108), cached at bring-up |
| `GPU_IOC_GET_CAPSET` | `struct gpu_capset` | served from the cached blob; no device round trip |
| `GPU_IOC_CTX_CREATE` | `struct gpu_ctx_create` | `CTX_CREATE` (0x0200) |
| `GPU_IOC_CTX_DESTROY` | `struct gpu_ctx_destroy` | `CTX_DESTROY` (0x0201) + unref of its resources |
| `GPU_IOC_RES_CREATE` | `struct gpu_res_create` | `RESOURCE_CREATE_3D` (0x0204) |
| `GPU_IOC_RES_ATTACH` | `struct gpu_res_attach` | `RESOURCE_ATTACH_BACKING` (0x0106) + `CTX_ATTACH_RESOURCE` (0x0202) |
| `GPU_IOC_RES_UNREF` | `struct gpu_res_unref` | `RESOURCE_UNREF` (0x0102) |
| `GPU_IOC_SUBMIT` | `struct gpu_submit` | `SUBMIT_3D` (0x0207), `VIRTIO_GPU_FLAG_FENCE` |
| `GPU_IOC_FENCE_WAIT` | `struct gpu_fence_wait` | no command in M1 (submits are synchronous) |
| `GPU_IOC_TRANSFER` | `struct gpu_transfer` | `TRANSFER_TO_HOST_3D` (0x0205) / `TRANSFER_FROM_HOST_3D` (0x0206) |

Every request copies its whole struct in with `vm_user_read` and writes the
result back with `vm_kwrite`; the kernel never keeps a user pointer. The
return value is `0` or a negative errno (the compat wrapper negates it, so
`ioctl()` sees `-1` and `errno`).

Error classes:

| value | meaning |
|---|---|
| `-ENOTTY` | request number is not a GPU request |
| `-ENODEV` | the GPU offered no `VIRTIO_GPU_F_VIRGL`, or ARM (no 3D transport) |
| `-EINVAL` | malformed struct, unknown ownership, bad geometry, bad fence |
| `-EFAULT` | the user range is not mapped/permitted |
| `-ENOSPC` | client/resource table or the attached-page pool is full |
| `-EBUSY` | `CTX_CREATE` on an fd that already owns a context |
| `-EIO` | the host rejected the command; the kernel's serial log carries the device response type |
| `-ETIMEDOUT` | `FENCE_WAIT` with a non-zero timeout on a fence that is not retired |

## 3. Structs and protocol values

The full layout is `src/include/gpu_abi.h`. Two conventions matter:

* `resource.target/format/bind` are **virgl protocol enums**, not Linux DRM
  fourccs: `target=2` (`VIRGL_TEXTURE_TARGET_2D`), `format=67`
  (`VIRGL_FORMAT_R8G8B8A8_UNORM`), `bind = RENDER_TARGET|SAMPLER_VIEW = 0xA`.
  The M2 winsys already has `pipe_to_virgl_format()` for this conversion.
* `struct gpu_transfer.box` is a `virtio_gpu_box` (`x,y,z,w,h,d`), and
  `to_host` is `0` = upload (guest→host), `1` = readback (host→guest).
  `stride == 0` means "host infers the row stride".

## 4. Memory rules (M1)

`GPU_IOC_RES_ATTACH` resolves `[va, va+len)` in the caller's address space:
`vm_touch` materialises/validates the range, then every page's leaf PTE is
read and sent as one `virtio_gpu_mem_entry` per page (short last entry when
`len` is not page-aligned). The page list is kept by the kernel for the
resource's lifetime.

Consequences the consumer must honour in M1:

* **v2 address spaces only.** A v1 process has no page table to walk and gets
  `-EFAULT`.
* The range must be readable and writable (`vm_touch` with write=1), because
  the host writes it during `TRANSFER_FROM_HOST_3D`.
* The pages must stay mapped and must not be `fork()`ed or `mprotect()`ed
  while attached. HobbyOS has no swap, so residency is not a concern; COW is.
  M2's winsys allocates its buffers from a private arena and never forks with
  a live GPU buffer, which satisfies this.
* The ABI copies the *command stream* (not the resources) into kernel memory:
  `SUBMIT_3D` needs one physically-contiguous range, so 128 KiB is bounced.

Limits, all enforced with `-EINVAL`/`-ENOSPC`:

| limit | value |
|---|---|
| concurrent clients | 8 |
| resources per client | **1024** (M3; was 32) |
| pages per resource | 16384 (64 MiB, unchanged: one `ATTACH_BACKING`) |
| system-wide pool | **131072 pages = 512 MiB** (M3; was 16384/64 MiB) |
| attached backing per client | **384 MiB** (M3, per-client accounting) |
| capset blob | 16384 bytes |
| command stream per submit | 128 KiB, dword-aligned |
| texture dimensions | 16384 × 16384, ≤ 64 MiB estimated bytes |
| debug name | 31 bytes + NUL |

## 5. Sync model

`SUBMIT_3D` is **fenced and synchronous in M1**: the kernel assigns a
monotonic `fence_id`, sets `VIRTIO_GPU_FLAG_FENCE`, and waits for the
command's used-ring completion, which QEMU defers until virglrenderer's fence
signals. On return the host has retired the fence, so a following
`TRANSFER_FROM_HOST_3D` observes the submit's effect — the read-after-clear
property the M1 gate asserts. `GPU_IOC_FENCE_WAIT` exists so the ABI does not
have to change when M4 makes submits asynchronous; today it validates the id
and returns 0 (a failed submit still retires its id, and its return code is
the error).

`RESOURCE_FLUSH` is not exposed as an ioctl in M1. The kernel keeps a fenced
flush path (`virtio_gpu_3d_flush`, exercised by the boot gate) for the M5
scanout mode.

## 6. The M1 gate

`tools/gate/m1_virgl.py` boots the real desktop kernel twice:

1. `-device virtio-gpu-gl-pci -vga none -display egl-headless,gl=on`, where
   the kernel negotiates `VIRTIO_GPU_F_VIRGL` and runs the kernel-only probe
   (`virtio_gpu_3d_probe`, no Mesa): `CTX_CREATE` → `RESOURCE_CREATE_3D`
   (64×64 RGBA8) → `ATTACH_BACKING` (4 pages) → `TRANSFER_TO_HOST_3D` (zero
   fill) → `SUBMIT_3D` of a virgl `CLEAR_TEXTURE` to `0xFF00FF00` → fence →
   `TRANSFER_FROM_HOST_3D` → assert every one of the 4096 pixels. Rows are
   prefixed `[VIRGL3D]`, each carrying the device's response type; the run
   ends with `[VIRGL3D] RESULT PASS`.
2. `-device virtio-gpu-pci -vga none -display none`, the 2D regression: the
   same kernel must print `VirtIO GPU: virtio-pci`, bring the desktop up and
   present (`RESOURCE_FLUSH` traffic), with **no** `[VIRGL3D]` rows at all.

Measured on this host (QEMU 10.2.1, libvirglrenderer package 1.2.0 /
`libvirglrenderer.so.1.10.0`, AMD Strix Halo render node), rows copied from
`obj/m1_virgl/gl.serial.log`:

```
[VIRGL3D] feat virgl=1 ctx_init=0
[VIRGL3D] capset_info id=1 maxver=1 maxsize=308 resp=0x0000000000001102
[VIRGL3D] capset id=1 ver=1 len=308 blob_maxver=1 resp=0x0000000000001103
[VIRGL3D] ctx_create ctx=1 name_len=8 resp=0x0000000000001100
[VIRGL3D] res_create res=4096 64x64 fmt=67 bind=0x000000000000000A resp=0x0000000000001100
[VIRGL3D] attach res=4096 pages=4 len=16384 resp=0x0000000000001100
[VIRGL3D] ctx_attach ctx=1 res=4096 resp=0x0000000000001100
[VIRGL3D] submit ctx=1 bytes=52 fence=1 echoed=1 resp=0x0000000000001100
[VIRGL3D] fence id=1 retired=1
[VIRGL3D] readback px00=0x00000000FF00FF00 px_mid=0x00000000FF00FF00 px_last=0x00000000FF00FF00 expect=0x00000000FF00FF00 match=4096/4096
[VIRGL3D] timing submits=64 total_ms=415 us_per_roundtrip=6484 ok=1
[VIRGL3D] timing_submit submits=64 total_ms=629 us_each=9828 ok=1
[VIRGL3D] timing_unfenced submits=64 total_ms=7 us_per_roundtrip=109 ok=1
[VIRGL3D] unfenced_readback px00=0x00000000FF0000FF expect=0x00000000FF0000FF
[VIRGL3D] timing_readback bytes=16384 total_ms=5 us_each=78 ok=1
[VIRGL3D] flush res=4096 fence=130 resp=0x0000000000001100
[VIRGL3D] unref res=4096 resp=0x0000000000001100
[VIRGL3D] ctx_destroy ctx=1 resp=0x0000000000001100
[VIRGL3D] RESULT PASS
```

The host accepted the whole sequence, including classic (non-blob)
`RESOURCE_CREATE_3D` + `ATTACH_BACKING`, so **risk R3 is resolved for M2**: a
virgl context, a 3D resource backed by guest pages, a fenced submit and a
readback all work on virglrenderer 1.10.0.

### The numbers that decide M2/M3

| row | value | reading |
|---|---|---|
| `timing_submit` (fenced, no readback) | **9.8 ms** per submit (7–10 ms across runs) | the fence dominates everything |
| `timing` (fenced submit + 64×64 readback) | **6.5 ms** (6.5–9.7 ms across runs) | equals the fence; readback is noise |
| `timing_unfenced` (unfenced submit + readback) | **109 µs** | the same work without waiting for the fence |
| `timing_readback` (16 KiB) | **62 µs** | ~260 MB/s guest-side, dominated by the control round trip |

The fenced path costs ~10 ms **regardless of the work**, which is the
signature of QEMU's virgl fence poll (a ~10 ms timer), not of GPU execution.
The order-of-magnitude difference is the actionable result:

* **Batch a frame into one submit.** 110 draws in one `SUBMIT_3D` pay one
  round trip, not 110.
* **Do not fence per frame if it can be avoided.** An unfenced submit is
  ~109 µs including a readback; a fenced one is ~10 ms. M3's present path
  (readback, then the existing 2D flush) already orders the readback after
  the submit — the `unfenced_readback` row proves the host honours that
  ordering (a red clear is observed by the immediately following readback).
  `GPU_SUBMIT_F_UNFENCED` exists in the ABI for exactly this; the fenced
  form stays the default and the correctness reference.
* At 30 fps (33.3 ms/frame), one fence per frame is survivable (10 ms) but
  leaves little room for fill, readback and the game's own CPU. One fence
  per frame *plus* per-draw submits would be fatal (110 × 10 ms).

The 2D regression rows from the same gate run (`obj/m1_virgl/2d.serial.log`):
`VirtIO GPU: virtio-pci`, `Desktop starting...`, 315
`virtio_gpu_cmd_res_flush` trace events, a 105-colour QMP screendump, and no
`[VIRGL3D]` row — the 3D code stayed behind the un-negotiated capability.

## 7. Not in M1 (deliberately)

* `mmap()` of a resource, blob resources, `RESOURCE_MAP_BLOB`, UUIDs.
* dmabuf import/export, `resource_create_from_handle`/`resource_get_handle`.
* Async submits and multiple in-flight fences.
* `SET_SCANOUT` of a 3D resource and the cursor queue (M5, mode A).
* `VIRTIO_GPU_F_CONTEXT_INIT` and capset 2 (`VIRGL2`): the device offers
  `CONTEXT_INIT`, but declining it keeps `CTX_CREATE` on the legacy layout
  and capset 1 for M1. Revisit only if the caps blob shows a feature the
  game needs.

## 8. M2 amendments (the first real consumer, lane `rw-l70-virglm2`)

M2's Mesa client exercised three paths M1's kernel-only probe did not. All
three are now fixed; they are recorded here because they change what this
document promises. Full evidence: `docs/rimworld-virgl-m2.md` §2.

1. **`GPU_IOC_RES_ATTACH` now also sends `CTX_ATTACH_RESOURCE` (0x0202)**, as
   §2 always claimed. M1's ioctl path sent only `RESOURCE_ATTACH_BACKING`; the
   kernel probe called `virtio_gpu_3d_ctx_attach()` itself, so the gap was
   invisible until Mesa. Without it the host accepts the submit and drops the
   whole batch (`vrend: Illegal resource <id>`), which reaches the guest as a
   black window with no error.
2. **`target == 0` is legal** — `PIPE_BUFFER` == `VIRGL_TEXTURE_TARGET_BUFFER`,
   the target of every vertex/index/constant buffer. Valid targets are 0..4
   (buffer, 1D, 2D, 3D, cube).
3. **For a buffer, `width` is a byte count**, not a texel count: gallium sets
   `width0` = size in bytes (a 1 MiB VBO is `width0 = 1048576`). The 16384
   dimension limit therefore applies only to textures; a buffer's ceiling is
   the 64 MiB backing cap, and its `bytes` estimate is `width`.

Two limits are now the likeliest non-performance wall for the game (M3/M4):
**32 resources per client** and **64 MiB of attached backing system-wide**.
The M2 probe uses 3 resources / 15.6 MB; a UI with texture atlases and several
render targets will not fit.

## 9. M3 amendments (the first game client, lane `rw-l71-virglm3`)

Both limits above were raised (1024 resources, 512 MiB pool, 384 MiB per
client) and two more **acceptance rules** turned out to be wrong -- each one a
value the ABI refused to accept and reported, two layers up, as
`GL_OUT_OF_MEMORY`:

1. **`bind == 0` is legal.** A buffer has `bind == 0` until it is first bound;
   Unity's `glBufferData` creates one exactly like that
   (`target=0 fmt=81 bind=0x0 64x1`). `gpu_abi_res_ok()` rejected it, and the
   game's renderer never came up. This is M2's `target == 0` bug in another
   field.
2. **Gallium array targets are not virgl targets.** `PIPE_TEXTURE_2D_ARRAY`
   (7) must arrive at the kernel as target 2 with `array_size = N`; the winsys
   passed 7 and the kernel's `target > 4` check answered `-EINVAL`.

Every refusal now prints the exact request on the serial console
(`[GPU] res_create refused: geometry ...`, `[GPU] res_attach refused: ...`,
`[VIRGL3D] res_create refused: table full ...`, `[VIRGL3D] attach refused ...
reason=no_contiguous_run`), and `ctx_destroy` prints a demand row
(`[VIRGL3D] stats peak_res=N peak_pages=M peak_mib=K caps=1024/131072`).


## 10. The Venus amendment (Cyberpunk F3, ABI version 2)

Status: **F3 freeze, lane C6-venus (`docs/cyberpunk.md` §6 P6, §7.3 F3).**
Written before the kernel side.  It adds what Mesa's Venus driver
(`vn_renderer_hobbyos.c`, `docs/cp-venus.md`) needs from the virtio-gpu
transport: context-init with a capset id, host-memory blob resources, a
mapping of a blob into the caller's address space through the device's
host-visible window, and asynchronous ring-indexed fences that the interrupt
path retires.  The name "M2" in `docs/cyberpunk.md` means this section, not
RimWorld's M2 (§8).

**Compatibility rule.** Every M1 request (numbers 1–10) keeps its number,
struct and behaviour.  A client that never issues a request numbered 11 or
higher sees exactly the M1 device: legacy `CTX_CREATE` (context_init = 0),
capset 1, synchronous fenced submits.  Negotiating `VIRTIO_GPU_F_CONTEXT_INIT`
and `VIRTIO_GPU_F_RESOURCE_BLOB` does not change any M1 command's encoding.

### 10.1 What the device must offer (host recipe)

`tools/cp/host-venus.sh`: `virtio-gpu-gl-pci,blob=true,hostmem=8G,venus=true`
on a memfd-backed guest.  Measured on strix (QEMU 10.2.1, virglrenderer 1.2.0,
`docs/cp-venus-evidence/step1-boot-check.txt`):

```
[VIRGL3D] shm id=1 bar=4 base=0x0000380800000000 len=0x0000000200000000
[VIRGL3D] feat offered lo=0x000000003000001B virgl=1 edid=1 uuid=0 blob=1 ctx_init=1
[VIRGL3D] capset index=0 id=1 (virgl) maxver=1 maxsize=308
[VIRGL3D] capset index=1 id=2 (virgl2) maxver=2 maxsize=1408
[VIRGL3D] capset index=2 id=4 (venus) maxver=0 maxsize=160
```

The kernel negotiates `RESOURCE_BLOB` (bit 3) and `CONTEXT_INIT` (bit 4)
whenever they are offered together with `VIRGL`.  The host-visible window is
the PCI capability with `cfg_type 8` (`struct virtio_pci_cap64`) and
`shmid 1` (`VIRTIO_GPU_SHM_ID_HOST_VISIBLE`); with `hostmem=` QEMU moves the
virtio registers to BAR 2 and puts the window in BAR 4.  The Venus capset is
**id 4, version 0**, 160 bytes (`struct virgl_renderer_capset_venus`).

### 10.2 New requests

Same `_IOC` scheme (§2), magic `'g'`.  Numbers 11–20.  Every struct is
naturally aligned, has explicit padding and is the same size in the kernel and
in glibc user space.

| nr | request | struct | emits (virtio-gpu) |
|---|---|---|---|
| 11 | `GPU_IOC_GET_PARAM` | `gpu_param` | none |
| 12 | `GPU_IOC_GET_CAPSET_INFO2` | `gpu_capset_info2` | none (table cached at bring-up) |
| 13 | `GPU_IOC_GET_CAPSET2` | `gpu_capset2` | `GET_CAPSET` (0x0109), any id, version 0 allowed |
| 14 | `GPU_IOC_CTX_CREATE2` | `gpu_ctx_create2` | `CTX_CREATE` (0x0200) with `context_init = capset_id` |
| 15 | `GPU_IOC_RES_CREATE_BLOB` | `gpu_res_create_blob` | optional `SUBMIT_3D` of `cmd`, then `RESOURCE_CREATE_BLOB` (0x010C), then `CTX_ATTACH_RESOURCE` (0x0202) |
| 16 | `GPU_IOC_RES_MAP_BLOB` | `gpu_res_map_blob` | `RESOURCE_MAP_BLOB` (0x0208) → `OK_MAP_INFO` (0x1106) |
| 17 | `GPU_IOC_RES_UNMAP_BLOB` | `gpu_res_unmap_blob` | `RESOURCE_UNMAP_BLOB` (0x0209) |
| 18 | `GPU_IOC_SUBMIT2` | `gpu_submit2` | `SUBMIT_3D` (0x0207), fenced with `FLAG_FENCE \| FLAG_INFO_RING_IDX` or unfenced |
| 19 | `GPU_IOC_FENCE_WAIT2` | `gpu_fence_wait2` | none (waits for the IRQ path) |
| 20 | `GPU_IOC_SCANOUT_BLOB` | reserved for P7 (AD-6) | answers `-ENOSYS` in v2 |

`GPU_IOC_RES_UNREF` (7) frees a blob too; it unmaps it first if it is mapped.
`GPU_IOC_CTX_DESTROY` (4) and closing the fd destroy every blob of the
context and unmap every mapping the client made (§10.5).

```c
struct gpu_param        { uint32_t param, padding; uint64_t value; };          /* 16 */
#define GPU_PARAM_ABI_VERSION       1  /* 2 */
#define GPU_PARAM_FEATURES          2  /* GPU_FEAT_* */
#define GPU_PARAM_HOST_VISIBLE_SIZE 3  /* bytes in the host-visible window, 0 = none */
#define GPU_PARAM_MAX_RINGS         4  /* rings a CTX_CREATE2 may ask for (64) */
#define GPU_PARAM_NUM_CAPSETS       5
#define GPU_PARAM_FENCE_SLOTS       6  /* async fenced submits in flight, device-wide */
#define GPU_FEAT_VIRGL         (1u << 0)
#define GPU_FEAT_RESOURCE_BLOB (1u << 1)
#define GPU_FEAT_CONTEXT_INIT  (1u << 2)
#define GPU_FEAT_HOST_VISIBLE  (1u << 3)
#define GPU_FEAT_FENCE_IRQ     (1u << 4)  /* fences are retired by the device interrupt */

struct gpu_capset_info2 { uint32_t index;      /* in: 0..NUM_CAPSETS-1 */
                          uint32_t capset_id, max_version, max_size; };        /* 16 */
struct gpu_capset2      { uint32_t capset_id, version;
                          uint32_t size;       /* in: capacity, out: bytes */
                          uint32_t padding; uint64_t blob; };                  /* 24 */
struct gpu_ctx_create2  { uint32_t capset_id;  /* 4 = Venus */
                          uint32_t num_rings;  /* 1..64; ring 0 is the CPU ring */
                          uint32_t flags;      /* 0 */
                          uint32_t ctx_id;     /* out */
                          char name[32]; };                                    /* 48 */
#define GPU_BLOB_MEM_HOST3D          2
#define GPU_BLOB_FLAG_MAPPABLE       1
#define GPU_BLOB_FLAG_SHAREABLE      2
#define GPU_BLOB_FLAG_CROSS_DEVICE   4
struct gpu_res_create_blob { uint32_t ctx_id, blob_mem, blob_flags;
                             uint32_t res_id;  /* out */
                             uint64_t blob_id, size;
                             uint64_t cmd;     /* optional command stream run first */
                             uint32_t cmd_size, padding; };                    /* 48 */
#define GPU_MAP_CACHE_CACHED   1   /* mapped write-back */
#define GPU_MAP_CACHE_UNCACHED 2   /* mapped UC (PAT 3) */
#define GPU_MAP_CACHE_WC       3   /* mapped write-combining (PAT 4) */
struct gpu_res_map_blob   { uint32_t res_id;
                            uint32_t map_info;   /* out: GPU_MAP_CACHE_* */
                            uint64_t va;         /* out */
                            uint64_t size; };    /* out: bytes mapped */            /* 24 */
struct gpu_res_unmap_blob { uint32_t res_id, padding; };                            /* 8 */
#define GPU_SUBMIT2_F_FENCE  (1u << 0)
struct gpu_submit2      { uint32_t ctx_id, flags, ring_idx, cs_size;
                          uint64_t cs;
                          uint64_t seqno; };   /* out: the fence, 0 when unfenced */  /* 32 */
struct gpu_fence_wait2  { uint32_t ctx_id, ring_idx;
                          uint64_t seqno;
                          uint64_t timeout_ns;   /* 0 = poll, ~0 = forever */
                          uint64_t deadline_ms; }; /* kernel scratch: 0 on entry */ /* 32 */
```

### 10.3 Contexts and capsets

`GET_CAPSET_INFO2` walks the table the kernel built at bring-up (one
`GET_CAPSET_INFO` per index until the device answers an error).
`GET_CAPSET2` asks the device for any listed id at `0 <= version <=
max_version` (Venus is version 0, which M1's `GET_CAPSET` refuses), and copies
at most `size` bytes.  `CTX_CREATE2` is one context per fd, exactly like
`CTX_CREATE` (`-EBUSY` for a second one); it requires `GPU_FEAT_CONTEXT_INIT`,
a capset id from the table and `1 <= num_rings <= 64`.  M1 requests that take
a context id (`RES_CREATE`, `SUBMIT`, `TRANSFER`) keep working on a CTX_CREATE2
context; they are what a virgl2 client would use.

### 10.4 Blobs and the host-visible window

Only `blob_mem = HOST3D` (2) exists in v2: Venus allocates every shmem
(`blob_id 0`, the render server's own shmem) and every `VkDeviceMemory`
(`blob_id` = the Venus memory object id) on the host.  `GUEST` and
`HOST3D_GUEST` answer `-EINVAL` (they need page lists; no client asks for
them).  `size` is rounded up to 4 KiB.  When `cmd_size != 0` the kernel first
submits `cmd` (≤ 128 KiB, dword-aligned) unfenced on the context, because the
blob's `blob_id` names an object that command creates (`vkAllocateMemory`);
this is Linux's `drm_virtgpu_resource_create_blob.cmd` contract.  The new
resource is then attached to the creating context.  Blobs share the client's
resource table with 3D resources (§4 limits), and their bytes count against
nothing: the memory is the host's.

`RES_MAP_BLOB` (MAPPABLE blobs only) allocates a range of the host-visible
window (first fit, 4 KiB granular, 2 MiB aligned for blobs ≥ 2 MiB), sends
`RESOURCE_MAP_BLOB(res, offset)`, and maps `[window + offset, +size)` into
the caller at a kernel-chosen address: `[0x700000000000, 0x7f0000000000)` for
a compat (Linux-ABI) address space, the native mmap window otherwise.  Each
blob maps at most once; a second `MAP_BLOB` returns the same `va`.  The pages
are `MAP_SHARED`, `PROT_READ|PROT_WRITE`, never executable, and are never
freed by teardown or `munmap` (they are the device's, like `/dev/fb0`'s).
A user `munmap` of the range is allowed and only drops the PTEs; the window
range stays allocated until `UNMAP_BLOB`/`UNREF`/close.

**Cache attributes.**  `map_info` is the host's: virglrenderer reports how
its own mapping of the `VkDeviceMemory` is cached (`CACHED` for
`HOST_CACHED` memory types, `WC` otherwise).  The guest must not be more
cacheable than the host.  On this host it is an AMD CPU, where KVM's nested
paging uses the **guest's** PAT memory type for the window, so a WB guest
mapping of a buffer the host maps WC would let the guest's caches hold lines
the GPU writes behind them (stale reads, lost writes).  The kernel therefore
honours `map_info`: `CACHED` → PAT 0 (WB), `WC` → PAT 4 (the kernel programs
`IA32_PAT` entry 4 to WC on every CPU at boot; entries 0–3 keep their
power-on WB/WT/UC-/UC, so no existing mapping changes), `UNCACHED`/`NONE` →
PCD|PWT (PAT 3, UC).  `mprotect` keeps the attribute.  Writes through a WC
mapping are posted; Venus orders them with its own release stores before it
notifies the host, and a `SUBMIT2` is a serialising trap, so no extra fence is
needed in the ABI.

`RES_UNMAP_BLOB` unmaps the caller's PTEs, sends `RESOURCE_UNMAP_BLOB` and
frees the window range.  QEMU ≥ 9.2 completes that command only after its
memory region is gone, so the next `MAP_BLOB` can reuse the range safely.

### 10.5 Fences

* **Rings.** A context has `num_rings` timelines.  Ring 0 is the CPU ring
  (the host signals it right after it has processed the command stream);
  Venus binds each `VkQueue` to one of rings 1..63, and the host signals that
  ring when the queue has finished the work submitted before the fence.
* **Seqnos.** `SUBMIT2` with `GPU_SUBMIT2_F_FENCE` takes the next value of a
  device-wide 64-bit counter that starts above 2^32 (QEMU retires an M1
  global fence by comparing ids across its whole fence queue without looking
  at the ring flag, so a ring fence numbered below an M1 fence id could be
  answered early), sends `SUBMIT_3D` with
  `hdr.flags = FENCE | INFO_RING_IDX`, `hdr.fence_id = seqno`,
  `hdr.ring_idx = ring_idx`, and **returns immediately** with `seqno`.  A
  fenced submit's command stream is at most 256 bytes (Venus sends 0–32: a
  `vkWaitRingSeqnoMESA`); a longer one is sent first as an unfenced submit and
  the fence follows it with an empty stream (one context's commands execute in
  order).  Without the flag, `SUBMIT2` is unfenced and synchronous up to the
  device's acceptance, like `GPU_SUBMIT_F_UNFENCED`.
* **Retirement.** QEMU withholds a fenced command's used-ring entry until its
  (context, ring) fence signals, so the used-ring entry *is* the retirement.
  The device interrupt (INTx, `GPU_FEAT_FENCE_IRQ`) drains the used ring,
  records `done[ctx][ring] = seqno` (per ring the order is monotonic: QEMU
  retires every pending fence of that ring up to the signalled id, in
  submission order) and wakes the waiters.  Every control transaction also
  drains the ring, so a completion is never lost if an interrupt is.
* **Waiting.** `FENCE_WAIT2` returns 0 when `seqno <= done[ctx][ring]`;
  `-EINVAL` for a ring or seqno the context never issued; `-ETIMEDOUT` when
  `timeout_ns` is 0 (a poll) or the deadline passed; `-EINTR` when a signal
  is deliverable.  Otherwise it parks the thread (the syscall restarts on
  wake, Linux-style).  The kernel writes the absolute deadline into
  `deadline_ms` on the first park so a restarted wait keeps the original
  deadline; a caller sets `deadline_ms = 0` before each new wait.
* **Ring 0 is slow on this host.** QEMU 10.2.1 answers CPU-ring (ring 0)
  fences from its 10 ms virgl fence poll: measured submit 14 µs p50, signal
  10.0 ms p50 (`docs/cp-venus-evidence/step3-transport-probe.txt`).  Venus
  fences VkQueue work on rings 1..63; ring 0 only serves
  `vn_renderer_submit_simple_sync`.
* **Limits.** 32 async fences in flight device-wide
  (`GPU_PARAM_FENCE_SLOTS`); a `SUBMIT2` that finds none free waits for the
  oldest to retire.  A context destroyed with fences in flight retires them as
  failed (`done` jumps to the last issued seqno) after `CTX_DESTROY`.

New error rows: `-ENOSYS` (`SCANOUT_BLOB`, reserved), `-ENOMEM` (the
host-visible window or the caller's address space has no room),
`-EOPNOTSUPP` (`MAP_BLOB` of a blob without `MAPPABLE`, or a device without a
host-visible window).

### 10.6 Not in v2

`SET_SCANOUT_BLOB` (P7, AD-6, F5), guest-memory blobs, dma-buf/sync-file
export and import, resource UUIDs, cross-device sharing, more than one context
per fd, and kernel `read()`/`write()` of a user buffer that lies in a mapped
blob (the kernel copies user memory through the direct map, which does not
cover the window; such a copy answers `-EFAULT` instead of faulting the
kernel — the P8 lane must lift this if the game reads files straight into
upload heaps).
