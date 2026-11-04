# GL2 surface boundary (Program-2) — measured 2026-11-04

Status: **BLOCKED ON DEVICE on the graded farm host `swarm-farm-3`** — not blocked
on shim design. The host's QEMU does not offer a virgl device, so no guest can
negotiate a 3D capset today. The device is one apt package away, not a HobbyOS
gap (see "What is actually missing").

Probe: `tools/t4/virgl_probe.py` (GB-197). Repro:

```
python3 tools/t4/virgl_probe.py \
    --root . --out evidence/virgl
```

## 1. Host measurement (quoted)

Host `swarm-farm-3`, `QEMU emulator version 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2)`:

```
$ qemu-system-x86_64 -device virtio-gpu-gl-pci,help
qemu-system-x86_64: -device virtio-gpu-gl-pci,help: Device 'virtio-gpu-gl-pci' not found
$ qemu-system-x86_64 -device virtio-gpu-gl-device,help
qemu-system-x86_64: -device virtio-gpu-gl-device,help: Device 'virtio-gpu-gl-device' not found
$ qemu-system-x86_64 -device help | grep virtio-gpu
name "virtio-gpu-device", bus virtio-bus
name "virtio-gpu-pci", bus PCI, alias "virtio-gpu"
```

Only the 2D devices exist. This differs from `docs/graphics-accel.md` §1's
controller workstation, where the same QEMU version *did* have the GL variants
because `qemu-system-modules-opengl` was installed.

## 2. Negative control — the plain 2D device

Boot with the plain `virtio-gpu-pci` and no GL:

```
make run DESKTOP=hobby ARCH=intel \
  QEMU_ARGS="-display none -device virtio-gpu-pci -vga none"
```

Serial (quoted; full log in `evidence/virgl/<ts>/control-serial.log`):

```
VirtIO GPU: virtio-pci
VirtIO GPU successfully initialized.
Desktop starting...
[MENU] count=186
```

0 `FATAL` lines, 0 `VIRGL`/`capset`/`3D` markers in the serial. The guest
driver negotiates `VIRTIO_F_VERSION_1` only (`src/kernel/virtio_gpu_x64.c`
writes 0 to the driver-feature page) and has no `VIRTGPU_GET_CAPSET` /
`VIRTGPU_GETPARAM` path, so a plain-device boot **cannot** expose a 3D capset.
That is the control the epic's entry criterion must fail.

## 3. What is actually missing (provisionable)

```
$ dpkg -l | grep qemu-system-modules-opengl
(nothing)
$ apt-cache policy qemu-system-modules-opengl
  Installed: (none)
  Candidate: 1:10.2.1+ds-1ubuntu3.3
```

`qemu-system-modules-opengl` provides `hw-display-virtio-gpu-gl.so` /
`virtio-gpu-pci-gl`; on the controller workstation the same package supplied
both arches' GL variants. Installing it (host provisioning, not this card) is
the first step to a present-device measurement. The guest side already boots
the GL device with the 2D driver (docs/graphics-accel.md §1), so the display
path is not the unknown.

## 4. The GL2 boundary the RTS/DSP renderers need (when a device exists)

The Spring and Dyson-Sphere renderers need a **GLES2/GL2 client surface with an
EGL- or GLX-shaped entry point**. On HobbyOS that means, in order:

1. **Guest virtio-gpu 3D ABI** — negotiate `VIRTIO_GPU_F_VIRGL`; implement
   `VIRTGPU_GETPARAM` (`3D_FEATURES`), `VIRTGPU_GET_CAPSET` (the `VIRGL`
   capset, version 2), `VIRTGPU_CONTEXT_INIT` and `SUBMIT_3D` on the control
   queue. Today the x64 driver negotiates no device features and only issues
   the 2D commands (`GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, …).
2. **A Mesa virgl driver + EGL platform** in userspace — the virgl winsys
   talks the context/submit ioctls from (1); EGL must be able to create a
   window/pbuffer surface on the virtio-gpu scanout. This is the actual shim
   surface: `eglGetDisplay`/`eglCreateWindowSurface` (or GLX) backed by virgl,
   not a new HobbyOS graphics API.
3. **Host** — QEMU with `--enable-virglrenderer` (or the distro's
   `qemu-system-modules-opengl`) plus a GL-capable display backend; headless
   `-display none` is explicitly rejected for the GL device
   (docs/graphics-accel.md §1: "display backend does not have OpenGL support
   enabled").

Nothing in (1)–(3) is designed here; this document fixes the *entry criterion*
and the boundary so the RTS/DSP epics state their cost rather than a hope.

## 5. Entry criterion for the RTS/DSP epics

The epics may unblock when **both** hold on a graded host:

1. `qemu-system-x86_64 -device virtio-gpu-gl-pci,help` prints options (not
   `not found`); and
2. a guest boot negotiates `VIRTIO_GPU_F_VIRGL` and reports a non-zero capset
   (the probe's present-device path, which is the next card).

Until then the honest status is **blocked on device**, and no amount of shim
design changes that.

## 6. Falsifier

A report that prints `3D supported` without the host device line is RED. The
probe enforces it:

```
$ python3 tools/t4/virgl_probe.py --force-3d
[VIRGL] 3D supported
[VIRGL] RED: '3D supported' with no host device line (the falsifier must fail)
$ echo $?
1
```

The plain-device boot above is the negative control: `VirtIO GPU: virtio-pci`,
0 3D markers.
