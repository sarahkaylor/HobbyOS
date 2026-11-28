# Cyberpunk 2077's own benchmark on the 4090 and the 5090 — platform control

Status: **measured 2026-11-28** on Proxmox `r1`, in a Linux guest with each card passed
through. This is **not** a HobbyOS row and must never be quoted as one: `renderer=` for
every row here is **`nvidia-proprietary-open-guest`**. What it answers is a narrower
question — *does Cyberpunk's own benchmark complete on this silicon, with NVIDIA's own
driver?* — which the project had never had for either NVIDIA part.

## 1. The rows

| card | BDF | guest driver | preset / resolution | average | min | max | frames / time |
|---|---|---|---|---|---|---|---|
| **RTX 5090** (GB202 `10de:2b85`) | `41:00` | 580.178.04 **open** module; the game reports `Nvidia v 58099 [r62115_v0.9.3]` | Low, 1280×720, VSync off, FSR off, RT off | **171.61 fps** | 125.03 | 231.21 | 11,028 / 64.26 s |
| **RTX 4090** (AD102 `10de:2684`) | `01:00` | 580.178.04; game reports the same driver string | Low, 1280×720, VSync off, FSR off, RT off | **172.05 fps** | 126.98 | 228.89 | 11,056 / 64.26 s |

Guest: Ubuntu 24.04, kernel 6.8.0-147, VM 315 (a clone of the 580.178.04 reference guest),
16 GiB, 8 vCPU. The guest reports `osName = Windows 10 Pro` because Proton presents that;
the host is Linux and the game runs under **GE-Proton 11-14**.

Evidence: `summary.json` for each run (attached to card `t_4f0b2c11`), plus the results-screen
screenshots.

## 2. What the two numbers say — and do not

- **The benchmark completes on both cards.** That is the claim, and it is now measured
  rather than assumed.
- The two are **within 0.3 % of each other** at identical settings and the same CPU. That is
  an observation about *the instrument*, not about either card's ceiling: at 720p/Low this
  benchmark is not GPU-limited, so it cannot rank a 4090 against a 5090. Do not quote these
  as "the 5090 is as fast as the 4090".
- It is **not** comparable to the Venus/virgl rows (29.19 avg on an AMD host) or to the
  host-native RADV control (139.41 avg): different renderer, different stack, different part.
  It sits beside them as a vendor-driver reference on NVIDIA silicon.

## 3. How it was run (reproduce)

1. `qm clone 301 315 --full 1` (a 4090 guest with NVIDIA 580.178.04 already in it), then
   `qm set 315 --hostpci0 0000:<bdf>,pcie=1`. A 5090 **requires the open kernel module**
   (`nvidia-driver-580-open`); the closed module refuses GB202 by design
   (`docs/nv-r1-host-renderer.md` §1.1).
2. Xorg on `:99` with `Option "AllowEmptyInitialConfiguration" "true"` and `Virtual 1280 720`,
   plus openbox (no window manager means no input focus for the game).
3. The game comes from the harness's own copy — `/srv/nfs/cp-game` on r1, 156 files /
   66,820,176,214 bytes, byte-identical to the census the earlier waves used — exported
   read-only to the guest subnet and mounted at `/mnt/cp`.
4. **Transplant the harness guest's Proton prefix** (`~/compat/1091500`, 315 MB, extracted
   from `guest.qcow2` with `qemu-nbd`). It carries the answered first-run state *and* the P0
   settings (Low preset, 1280×720, FSR2 Auto, RT off, VSync off, borderless). Nothing else
   got past the first-run consent dialog.
5. Drive it with **QMP `input-send-event`** from the hypervisor — an absolute move plus a
   button press, i.e. real HID into the guest — **not** XTest. The game ignores synthetic X
   clicks on its menus entirely; the `at()`-style relative-motion recipe works for `xdotool`
   only where the guest has a relative pointer, and even then not on the consent screen.
   Sequence: menu → SETTINGS (159,364) → GRAPHICS tab (497,36) → `b` → Return.
6. The game writes `benchmarkResults/<stamp>/summary.json` under
   `Documents/CD Projekt Red/Cyberpunk 2077/` in the prefix.

## 4. What this unlocks, and what it does not

- It gives the Cyberpunk work an **NVIDIA-side reference**: the same title, the same
  benchmark, the same resolution and preset, on both parts the rack actually has.
- It does **not** move the native ladder. `nv` still has no 3-D engine; these rows are the
  vendor driver in a guest, and the plan's `renderer=` discipline exists precisely so this
  row cannot be mistaken for the driver's.
