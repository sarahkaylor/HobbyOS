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
| resources per client | 32 |
| pages per resource / system-wide pool | 16384 (64 MiB) |
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

