# A 5090 host renderer on r1 — plan, blast radius, revert (PLAN ONLY)

Status: **PLAN ONLY. Nothing was installed, bound, unbound, started or stopped on r1.**
Card `t_aaf0ad86`. Decision recorded 2026-11-24 by the coordinator: **do not install a
proprietary NVIDIA driver on r1's host**; take the proprietary-driver numbers on `strix`
instead. This doc exists so the question is answered *before* anyone re-opens it, and so
that the answer is a measurement plan rather than a paragraph of caution.

**One line:** r1's host is a hypervisor that four card lanes depend on and it has no
driver, no `/dev/dri`, no ICD and a virglrenderer two releases behind — the install is a
4-package prerequisite chain, not one `.run`; the only GPU the driver could own is
`82:00`, which is the destructive lane's card; and the thing it would measure
(Venus on NVIDIA) is a **correctness** question with a documented-negative prior, not a
frame-rate question.

## 0. The four questions the card asked, answered

| Question | Answer | Where |
|---|---|---|
| Which driver version? | **580.178.04**, minimum branch 570.86.16. Delivery has to be DKMS against `proxmox-headers-7.0.14-23-pve` | §2 |
| What does it do to the existing vfio bindings? | `82:00.0/.1` must leave `vfio-pci` for the driver to own the card; the other four NVIDIA functions (`01:00.0/.1`, `41:00.0/.1`) survive only if the initramfs ordering is right — the failure mode is `nvidia` racing `vfio-pci` for `01:00` and VM 300/312 then failing to start | §3 |
| What does it do to VM 300/312? | Neither has to break, and both are two 30-second checks away from being proven safe. VM 314 (planned, `82:00`) cannot exist while the host driver owns that card — that is the real casualty | §4 |
| How to revert? | Pre-state capture, then unbind → rebind → purge → initramfs → reboot, with the exact commands and the one line that proves it worked (`lspci -nnk` = six functions on `vfio-pci`) | §5 |

## 1. What r1 is today (quoted measurements, not re-measured)

r1 was **not reachable from this VM at plan time**: `ssh r1` →
`Could not resolve hostname r1` (2026-11-24, this run). Every r1 row below is a past
measurement from this repo, with the doc and date it came from. Nothing here is inferred
from a different host.

| fact | value | source |
|---|---|---|
| host | Proxmox 9.2.21, kernel **7.0.14-23-pve**, QEMU 11.0.3 (pve), 48 cores, 188 GiB | `docs/nv-games-state.md` §1 (2026-11-23) |
| `/dev/dri` | **does not exist** — no render node for any GL/Vulkan backend | `docs/nv-games-state.md` §4 |
| `lsmod \| grep -c nvidia` | **0**; `nvidia-smi` absent | same |
| `/usr/share/vulkan/icd.d` | absent — no host Vulkan at all (so no Venus, no vkd3d) | same |
| `libGL.so* / libEGL*` | absent — not even llvmpipe can be created | same |
| `libvirglrenderer1` | **1.1.0-2** present, nothing to render with | same |
| `qm set 311 --vga virtio-gl; qm start 311` | **rc=255**, `missing libraries for 'virtio-gl' detected! Please install 'libgl1' and 'libegl1'` | same |
| NVIDIA functions | `01:00.0/.1` AD102 4090, `41:00.0/.1` + `82:00.0/.1` GB202 5090s — **all six on `vfio-pci`** | `docs/nv-vm201.md` §GPU inventory (2026-11-24) |
| IOMMU | groups 41/42 (4090), 58/59 and 22/23 (5090s); no group shared between lanes | same |

So the install is not "one driver": the same host also needs `libgl1`, `libegl1`,
`libgbm1`, a Vulkan loader + ICD, and virglrenderer ≥ 1.2.0 with Venus compiled in
(strix runs 1.2.0; r1 has 1.1.0-2). That is **five changes**, each with its own revert.

### The VMs that share that host

| VM | card | lane | what it is |
|---|---|---|---|
| 300 | `01:00` | `nv-card` | the 4090 driver VM — the campaign's main target |
| 301 | `01:00` | reference | `linux-ref` guest, Ubuntu 24.04, **NVIDIA 580.178.04 in the guest**, GSP-RM running (`docs/nv-gsp-reference.md` §1) |
| 311 | — (no `hostpci`) | games | 2D-only `hobbyos-games` (`docs/nv-games-state.md` §1) |
| 312 | `01:00` | ROM work | VBIOS/ROM dump VM (`docs/nv-plan.md` §Hardware, `docs/nv-vbios-map.md`) |
| 313 | `41:00` | `nv-card2` | 5090 bring-up, behind a 7-verb forced-command wrapper (`docs/nv-vm201.md` §) |
| 314 | `82:00` | `nv-card2` (destructive) | **planned, not created** — `t_fb8b31ff`'s lane (`tools/nv/provision-5090b.sh`) |

VM 301 is the useful precedent here: **580.178.04 already drives a passed-through NVIDIA
card on this host today — inside a guest.** See §7.

## 2. The driver, if it were ever approved

**Version: 580.178.04.** Three reasons, in order of weight:

1. **It is the only RM image this project has measured to sign GB202.** The 5090's
   bring-up found the GB202 RM image *is* `firmware/gsp_ga10x.bin` from the 580.178.04
   package (75 028 464 B, sha256 `3b89a63f…`, `.fwversion 580.178.04`,
   `.fwsignature_gb20x` present) — that is `t_deb5ab29`'s handoff, measured on the card.
   A driver that carries a GB202-signed RM image is the one whose bring-up path is
   already partly characterised here.
2. **It is the version already measured running GSP-RM on AD102** (`docs/nv-gsp-reference.md`
   §1: Driver 580.178.04, GSP Firmware 580.178.04, CUDA 13.0, `nvidia-smi` populated), so
   an r1 number would be comparable with the project's only working reference.
3. Minimum for the silicon is **570.86.16** (first release with RTX 5090 support,
   2025-01-30, NVIDIA release notes / Phoronix) — 580 is three branches past it, and
   570/575 carry no GB202 evidence in this repo.

Delivery on PVE, per the Proxmox vGPU/passthrough wiki: DKMS against the running PVE
kernel, i.e. `proxmox-headers-7.0.14-23-pve` (PVE ships no prebuilt `nvidia` module, and
an opt-in kernel needs its own headers or DKMS does not build). NVIDIA's `.run` installer
with `--dkms`, or a distro `nvidia-dkms-*` package if the trixie archive carries 580 — the
`.run` is the route PVE documents and the one that cannot silently pull a different branch
than the one measured above.

Prerequisites the host does not have (each is a separate host change):

| piece | why | today |
|---|---|---|
| `libgl1`, `libegl1`, `libgbm1` | QEMU refuses `virtio-gl` without them (rc=255) | absent |
| `libvulkan1` + NVIDIA's ICD (`nvidia_icd.json`) | Venus forwards Vulkan; no ICD = no Venus | absent (`/usr/share/vulkan/icd.d` missing) |
| virglrenderer **≥ 1.2.0** with `-Dvenus=true` | Venus capset; strix runs 1.2.0 | 1.1.0-2 |
| `nvidia-drm.modeset=1` | creates `/dev/dri/renderD*` | no `/dev/dri` |
| `fbdev=0` | 570+ defaults `nvidia-drm fbdev=1`, which takes the console over; on a headless hypervisor that is all downside | n/a |
| NVIDIA's GBM backend (`libnvidia-egl-gbm`) | QEMU's `-display egl-headless,rendernode=` goes through **gbm** on the render node; this is the least-proven piece of the chain (SOURCE: 570 release notes add 32-bit GBM-backend support; headless use INFERRED, untested anywhere in this repo) | absent |
| secure boot state | DKMS modules must be signed/MOK-enrolled if enabled | **UNKNOWN** — `mokutil --sb-state` on the host answers it |

## 3. What it does to the vfio bindings

### 3.1 The mechanism

A driver can only own a PCI function if `vfio-pci` is not holding it. Today all six
NVIDIA functions are on `vfio-pci` (PVE does this by default for devices a VM claims, and
`docs/nv-plan.md` §Hardware records it). To render on `82:00`:

```
# host, root, VM 314 NOT running
echo 0000:82:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind
echo 0000:82:00.0 > /sys/bus/pci/drivers/nvidia/bind     # then nvidia_drm creates renderD*
```

That is the runtime, no-reboot form — which is the right form for an experiment, because
it is undone by the same two echoes in reverse (§5). The persistent forms are worse:
removing `10de:2b85` from `/etc/modprobe.d/vfio.conf` + `update-initramfs` + reboot, or
letting `nvidia` load before `vfio-pci` and race-claiming unbound NVIDIA functions.

`82:00.1` (the HDMI audio function) must **stay** on `vfio-pci`: `nvidia.ko` has no HDA
driver, so the moment `.1` is free `snd_hda_intel` can take it, and VM 314 needs the pair.
Function-level binding makes that possible; IOMMU group 22/23 (INFERRED to be `82:00`'s
pair from `docs/nv-vm201.md`'s ordering — confirm with
`find /sys/kernel/iommu_groups -name '*82:00*'`) is not itself a constraint as long as the
group is not assigned to a running VM.

### 3.2 The four functions that must not move

`01:00.0/.1` (VM 300/301/312) and `41:00.0/.1` (VM 313) must stay on `vfio-pci`. The
guarantees, in order of strength:

1. `options vfio-pci ids=10de:2684,10de:2b85` in `/etc/modprobe.d/*.conf` — keeps all six
   on `vfio-pci` at boot. **Already in place**, since that is how they are bound today.
2. `softdep nvidia pre: vfio-pci` — PVE's own remedy for "the GPU module got there first".
   Without it, `nvidia` can claim a function before `vfio-pci` does, and VM 300 then fails
   with `cannot bind 0000:01:00.0 to vfio-pci`, which reads as a broken GPU, not a broken
   boot order.
3. `blacklist nouveau` — already implied: the host has no `/dev/dri` at all today, so
   nothing DRM-ish is loading for these cards.

### 3.3 Failure modes, ranked, each with the check that catches it

| # | failure | why it bites | 30-second check after install, **before any VM starts** |
|---|---|---|---|
| 1 | `nvidia` claims `01:00` (or `41:00`) | initramfs/softdep ordering; VM 300/313 then cannot start | `lspci -nnk -s 01:00,41:00,82:00 \| grep -E 'Kernel driver'` → `vfio-pci` on the four that must not move |
| 2 | vgaarb decode change on `01:00` | `nv`/`nvidia` does `vgaarb: VGA decodes changed` on the cards it binds — measured in the guest dmesg (`docs/nv-gsp-reference.md` §2). If it ever does that on `01:00`, the 4090's passthrough can degrade | `dmesg \| grep -i vgaarb` → any line naming `0000:01:00` is a stop sign |
| 3 | console takeover (`fbdev=1`) | 570+ default; headless host, so it can only make the console unusable mid-experiment | `dmesg \| grep -i -E 'fbdev\|nvidia-drm'` |
| 4 | `snd_hda_intel` takes `82:00.1` | audio function of the card the lane wants | `lspci -nnk -s 82:00.1` |
| 5 | frozen module vs. opt-in kernel | a PVE kernel update without headers leaves DKMS unbuilt; on reboot there is no driver and `virtio-gl` returns rc=255 again (which is *not* damage — it is the §1 state) | `dkms status` |

Nothing in this list is silent-but-harmless: #1 and #2 are the two that would take a card
lane down, and both are visible in `lspci -nnk` + `dmesg` before the first VM start. The
residual risk is not the install, it is **an unattended reboot into a host whose initramfs
formats changed** — which is why the coordinator's bar for touching this host is high.

## 4. VM 300 / 312 (and 313 / 314)

| VM | effect of a host `nvidia` bound to `82:00` only | effect if the binding is wrong (#1/#2 above) |
|---|---|---|
| 300 (4090 driver) | **none** — its card stays on `vfio-pci` | refuses to start (`cannot bind`), or starts with a degraded 4090 |
| 301 (linux-ref, 4090) | none — guest's own 580 driver is independent of the host's | same |
| 312 (ROM work, 4090) | none | same |
| 313 (5090 `41:00`) | none | same |
| **314 (5090 `82:00`, planned)** | **impossible** — `provision-5090b.sh` refuses a card "held by a host driver", by design | n/a |

So "what it does to VM 300/312" has an exact answer: **nothing, if and only if
`vfio-pci` keeps `01:00`, which is a property of the initramfs and not of the driver**. The
card that is actually consumed is `82:00`, and `82:00` is the card the one permitted
destructive lane is built on (`t_fb8b31ff`: VM 314, `hostpci0 0000:82:00,pcie=1`, already
written and green in staging, blocked only on host-side provisioning). A host renderer on
`82:00` and that lane cannot both exist — that is an allocation decision, and it is the
coordinator's, not a worker's.

## 5. Revert

Capture this **before** touching anything; it is the yardstick revert is judged against:

```bash
lspci -nnk | grep -A3 -E '10de:2684|10de:2b85' > /root/pre-hostdrv.lspci
dkms status > /root/pre-hostdrv.dkms; lsmod | grep -E 'nvidia|nouveau' >> /root/pre-hostdrv.dkms
for v in 300 312 313; do qm config $v | grep -E 'hostpci|^name' ; done >> /root/pre-hostdrv.vms
mokutil --sb-state >> /root/pre-hostdrv.vms
```

Revert, in this order (the order is the whole plan — rebinding before unloading freezes
the pair):

```bash
# 0. nothing of ours may be running
qm status 314 ; qm status 313 ; qm status 300        # all stopped, or stop them first
qm stop 314                                          # never started by this plan
# 1. give 82:00.0 back to vfio-pci
echo 0000:82:00.0 > /sys/bus/pci/drivers/nvidia/unbind
echo 0000:82:00.0 > /sys/bus/pci/drivers/vfio-pci/bind
lspci -nnk -s 82:00                                  # must read "Kernel driver in use: vfio-pci"
# 2. drop the modules (this order; nvidia_drm holds references)
modprobe -r nvidia_drm nvidia_modeset nvidia_uvm nvidia
# 3. remove the packages / the .run
./NVIDIA-Linux-x86_64-580.178.04.run --uninstall     # or: apt purge '^nvidia-'
rm -f /etc/modprobe.d/nvidia*.conf                   # only the files the installer wrote
# 4. back to the pre-state initramfs
update-initramfs -u -k all
# 5. and the 2D host stays the host it was
reboot
```

Proof of a complete revert is not "the purge exited 0": it is the §1 table reproducing —

```bash
ls /dev/dri                                   # must fail: no such file or directory
ls /usr/share/vulkan/icd.d                    # must fail
lspci -nnk | grep -c 'vfio-pci'               # the six NVIDIA functions
qm start 313 && ssh r1 status                  # or the nv-card path for 300
```

`/dev/dri` disappearing and `qm set 311 --vga virtio-gl` returning to **rc=255
`missing libraries for 'virtio-gl'`** is the definition of "host restored" — it is exactly
the state `docs/nv-games-state.md` §4 measured.

Steps 1–2 are reversible live. Step 3 is reversible by reinstalling. Step 4 is the one
that can fail a reboot, so it is done with a root console in front of the machine, not
over SSH — and never on a night the nightly soak needs r1.

## 6. What the measurement would actually buy — and why it is the wrong question

The ask behind the card was "a 5090 renders Venus/virgl on r1 so host-rendered numbers
stop being llvmpipe". What is measured today:

| stack | number | source |
|---|---|---|
| strix, Cyberpunk host-native (Proton, RX 9070 XT) | **388–399 fps median @720p**, 393.5 @1080p; benchmark 139.41 avg | `docs/cyberpunk.md` §0.2 table |
| strix, Cyberpunk **on Venus** (Linux guest) | 142.8 / 144.4 fps median @720p; benchmark 25.86 avg | same |
| strix, Cyberpunk on **HobbyOS** via Venus | 29.19 avg / 29.7 median / 18.2 1 %-low @720p | `docs/nv-plan.md` §N8 |
| r1 today, RimWorld on virgl | **llvmpipe** host renderer, 51.02 fps median @720p | `docs/nv-plan.md` §N1 |
| a GPU host (strix), RimWorld-shaped scene through virgl | 170.9 fps mean / 187.7 median @720p present | `docs/nv-games-state.md` §5 |

A 5090 host renderer would produce a **Venus-on-NVIDIA** row. Two things are known about
that stack, and both are about correctness rather than speed:

1. **virglrenderer's own tracker**: issue **#524, "vkMapMemory fails with NVIDIA GPU"** —
   `failed to mmap 30 of size 16777216 rw: Permission denied` → `VK_ERROR_MEMORY_MAP_FAILED`
   → `VK_ERROR_OUT_OF_HOST_MEMORY` → segfault (NVIDIA 545.29.06, Mesa 23.3.3, virglrenderer
   1.0.1). Venus maps exported dma-bufs read-write; NVIDIA's proprietary Vulkan export
   does not permit that.
2. **Collabora's state-of-virglrenderer (2025-01-15)**: "We still have some issues with the
   NVIDIA proprietary driver (#524), which are expected to be addressed by NVIDIA in future
   versions of their driver." Venus also requires `VK_EXT_image_drm_format_modifier` from
   the host driver; NVIDIA's modifier import has a documented stride/distortion defect in
   VirtIO-GPU Venus (NVIDIA developer-forum thread, EGL dma-buf import with modifiers
   ignoring explicit stride).

This project already runs a workaround for the #524 class: `tools/cp/venus/venus-host-shim.c`
adds `DRM_RDWR` to the render server's dma-buf imports, and without it **every guest
`vkMapMemory` fails** (`docs/cyberpunk.md` §0.2 item 14). On AMD that is sufficient. On
NVIDIA the export side is the driver's, so whether the shim is enough is **INFERRED, not
measured** — and it is exactly the kind of thing that produces a *plausible wrong number*
rather than a failure: if the modifier/stride defect surfaces, the frames render distorted
and the fps evidence is quietly invalid.

And the comparison itself is not controlled: 9070 XT + RADV + Mesa 26.2.6 versus 5090 +
NVIDIA proprietary are different vendors, different drivers and a different Mesa path. A
"Venus fps on a 5090" row answers *5090 vs 9070 XT*, not *Venus vs native* — the latter
needs same-GPU rows, and strix already provides them (§6 table, first two rows). The
honest framing of the 5090 ask is therefore: **"does the NVIDIA proprietary Vulkan path
satisfy Venus' dma-buf assumptions on GB202"** — a driver-correctness question with a
documented-negative prior, which is why the coordinator's alternative (§8) is the better
buy.

## 7. The version of this that has a small blast radius (Option B)

VM 301 already proves the pattern: **580.178.04 driving a passed-through NVIDIA card on
r1, with GSP-RM running, inside a guest** (`docs/nv-gsp-reference.md` §1). No host driver,
no vfio changes, no initramfs, no reboot of the hypervisor. A host renderer could be built
the same way: a Linux VM owning `82:00`, the proprietary driver *in that guest*, QEMU in
the guest serving Venus to a nested HobbyOS guest.

- Blast radius: **one VM**. Revert: `qm stop`. Cannot touch `01:00`, cannot touch the
  host's initramfs, cannot affect VM 300/312/313.
- Cost: the card allocation is still `82:00` (same conflict with `t_fb8b31ff`, but now it
  is a *VM* allocation the coordinator can arbitrate without a host change), plus
  **nested KVM** on r1 must be on (`cat /sys/module/kvm_intel/parameters/nested`;
  **UNKNOWN** — not measured anywhere in this repo). If nested is off, the inner guest
  runs TCG and every fps row from it is meaningless — that check is the first step, and it
  is a read.

## 8. What was done instead (the coordinator's call, and it is the right one)

Take the proprietary-driver baselines on **strix, 192.168.0.123** — the machine every
Venus number in this repo already comes from, and the one that produced the 388–399 fps
menu rows.

```bash
# on strix, in the cp checkout
tools/cp/host-venus.sh check                      # render node 1002:7550 on amdgpu, versions, shim
tools/cp/host-venus.sh boot DISK WORK             # QEMU 10.2.1 virtio-gpu-gl-pci, venus=true,blob=true
```

What that gives: the same-GPU native/Venus pair (388–399 native vs 142.8/144.4 Venus
@720p), which is the comparison the project actually cites, at zero risk to r1.

What strix **cannot** give, stated plainly: any NVIDIA row. If someone needs NVIDIA-host
numbers specifically, the recipe is AMD-pinned in four places and would have to be
re-parameterised first — `VKDEV=1002:7550` (`tools/cp/host-venus.sh:53`), the
`render_check()` that refuses any node that is not `1002:7550` on amdgpu (`:70`), the
hardcoded `VK_ICD_FILENAMES=…/radeon_icd.json` (`:171`), and the render-node fencing in
`venus-host-shim.c`. That is a day of work on a machine nobody depends on — and it still
lands on the #524/#6-with-NVIDIA class above.

## 9. What would reopen this

In order of what would have to change first:

1. **virglrenderer #524 closed** by an NVIDIA release, *and* the project acquires a
   same-GPU native/Venus pair that matters. Until then the 5090 row cannot be interpreted.
2. **A named gate that needs NVIDIA-host rows.** The card asks for "the measurement it
   unblocks" — today there is none; the Venus evidence base is AMD and the only NVIDIA
   device in the project is the DUT (VM 300/313), which is not a host renderer.
3. **The destructive lane retires** and `82:00` is genuinely free. Even then: **Option B
   (driver in a VM, §7) before a host install**, because it answers the same question
   with a `qm stop` for a revert.

## 10. Evidence and reproduction

- No r1 access was attempted beyond a name-resolution probe (`ssh r1` → could not resolve,
  2026-11-24), because this lane has no r1 channel since the card-lane isolation
  (`t_89f24e1f`) and the card is PLAN ONLY.
- Nothing was installed, purged, bound, unbound, started or stopped on any host. No
  `82:00` access. No VM 300/312/313 access.
- r1 facts: `docs/nv-games-state.md` §1/§4, `docs/nv-vm201.md` §GPU inventory,
  `docs/nv-gsp-reference.md` §1/§2/§7, `docs/nv-plan.md` §Hardware.
- Driver version/firmware: `t_deb5ab29` handoff (GB202 RM image signature in 580.178.04);
  NVIDIA 570.86.16 release notes (first RTX 5090 support).
- Venus-on-NVIDIA: virglrenderer issue #524; Collabora, "The state of GFX virtualization
  using virglrenderer", 2025-01-15; Mesa's Venus requirements page
  (`VK_EXT_external_memory_dma_buf`); NVIDIA developer-forum thread on EGL dma-buf import
  with modifiers ignoring explicit stride in VirtIO-GPU Venus.
- PVE mechanics: Proxmox wiki, "PCI(e) Passthrough" (`softdep … pre: vfio-pci`,
  `options vfio-pci ids=`) and "NVIDIA vGPU on Proxmox VE" (DKMS + `proxmox-headers-X.Y`,
  `.run --dkms`).
