# The ladder to games, re-pointed after the booter wall

`docs/nv-plan.md` §4 names this file as the ladder's home. It did not exist until
now: the rungs lived only in that table, which is why "re-point the ladder" had
nowhere to land. This is the ladder **as of the backup-plan decision**
(`t_1044c642`, `docs/nv-no-rm-path.md`), i.e. with the native branch capped at
L1 + measured L2 and the games rows carried by the virtio/Venus path.

Two rules make this file honest, and both are mechanical:

1. **Every games row names its renderer** — `renderer=venus`, `renderer=virgl/llvmpipe`,
   or `renderer=nv/AD102`. A games row without one is a doc bug, not a data point.
2. **A native row is only "met" when the submit came from `nv`.** Anything that
   reached fps through the virtio path is labelled as such, with the same fps
   target and no "same as NVIDIA's driver" claim.

Effort ranges are ESTIMATES and marked as such; measured numbers are not.

---

## 1. Where the claim stands

| level | claim | status |
|---|---|---|
| L1 | HobbyOS owns the card, verifies NVIDIA's firmware, builds every structure GSP-RM will consume | **done, measured** |
| L2 | FWSEC runs → SEC2 unfences → booter runs → GSP-RM reports `INIT_DONE` | **measured through "SEC2 runs the booter"; the boot ROM rejects every reachable vendor image (`0x002B0009`)** — the run, not the completion, is measured |
| L3 | a kernel submit reaches the 3-D engine and a clear reads back | **not met natively.** The Venus path reaches submits and read-back (`[VIRGL3D] match=4096/4096`, M1 gate); that is L3-equivalent *for the fallback*, not for `nv` |
| L4 | nvk enumerates AD102; `vkcube` draws | nvk enumerates the *shim* to the first engine class object (refused by name, `need=gsp_rm`); `vkcube` draws **on Venus** |
| L5 | RimWorld / Cyberpunk ≥30 fps at 720p | **met on the fallback** (51.02 fps / 29.19 fps avg); not met natively |

The native driver's public claim is L1 + measured L2. That is a real claim: it
identifies the part, brings up the register window, reads and verifies 38 MB of
vendor firmware, lays out VRAM the way the booter expects, and executes a
signed container as far as that container's own verification allows.

---

## 2. The ladder

### 2.1 Native branch

| # | rung | gate | today | depends on |
|---|---|---|---|---|
| N0 | 1080p present gate | 8,294,400 B maps; pixel visible | **closed 2026-11-23** — `(1919,1079)=(255,0,0)` in a QMP screendump | — |
| N1n | PGRAPH reachability + measured topology | `[NV-ENG]` row: `verdict=ADDRESSABLE` and `topology verdict=MEASURED` | **done 2026-11-26, one read-only boot** (`t_b2131040`, `t_78a50be7`): `verdict=ADDRESSABLE` — 6 of 7 named registers returned data, none fenced or absent (`0x400000` = `0xBADF5040` unreadable) — and `topology verdict=MEASURED` **gpc=11 tpc=64 `src=register`** from `0x409604=0x0C06000B` plus `GPC_UNIT(i,0x2608)` TPC_COUNT bits 4:0 (5,5,6×9), cross-checked against the mask-encoded `ppc_tpc_mask`. The DECLARED 11/64 row is **confirmed, not replaced** (`docs/nv-topology-measured.md`); the first read's tpc=44 was a whole-word popcount of a word the reference treats as a count | — |
| N2 | `/dev/nv*` shim over a mock RM | probe + fence retire | **done** (16 real `DRM_IOCTL_NOUVEAU_*` rows, 21 named refusals, NVIF + `GRAPH_UNITS` served; client-surface census 2026-11-27: 0 ioctls absent from both tables, every served width matches Mesa) | — |
| **N3n** | **SEC2 accepts a published (GA102-signed) HS container** | `0x840094` is neither `0x250009` (signature class) nor `0x002B0009` (post-signature), with the core not exiting by `INTR` | **closed 2026-11-27 without spending the boot.** Two independent reasons: the class question was answered by `v4a` — a GA102-signed container (different triple) rejected **at** the signature with `0x00250009` at +4.212 ms, `EXIT 0x10` — and the named candidate `ga102/sec2/hs_bl_sig.bin` is an ls-hsbl **signature descriptor, not a bootable container** (`t_90ef924c`), so no vendor-shipped HS bootloader exists to stage. Both branches the decision table could take end at "native branch closed on this part" | — |
| N4n | ACR bootstraps FECS/GPCCS on SEC2 | ACR cmdq answers; FECS/GPCCS halt with mailbox 0 | blocked behind N3n | N3n + an **AD102-signed** ACR ucode set, which NVIDIA does not publish (linux-firmware `nvidia/ad102/` is `gsp/` only) |
| N5n | first GR channel + submit | 64×64 clear reads back | blocked behind N4n | N4n |
| N6n | nvk enumerates AD102 natively | `vulkaninfo` lists `AD102 (NVK)` | blocked behind N5n | N5n |
| N7n | RimWorld via Zink+nvk | menu ≥30 fps, world loads | blocked behind N6n | N6n |
| N8n | Cyberpunk via vkd3d+nvk | P9 script end to end | blocked behind N6n | N6n |
| N9n | "reasonable performance" natively | 720p ≥30 fps incl. present | blocked behind N7n/N8n | — |
| N3r | **GSP-RM boots** (the signed-booter branch) | mailbox moves, first RPC answered | **PARKED, not cancelled**: no reachable vendor `booter_load` authenticates (`0x2B0009` invariant over three AD102 generations, two cross-chip controls) | a signed booter for this part's fusion |

Both cheap native gates are now settled, and neither was a pause: **N1n measured**
(PGRAPH addressable, topology `src=register` and equal to the declared 11/64) and
**N3n closed in class** (no GA102-signed container is verified by this part, and no
AD102-signed non-GSP ucode is published). What is left on the native branch above them
is gated on an AD102-signed ucode set that NVIDIA does not ship — see `docs/nv-plan.md`
§2.12–§2.13 for why that ends the branch as a declared ceiling rather than an open
question.

### 2.2 Fallback branch (the one that reaches the data points today)

| # | rung | gate | measured | renderer |
|---|---|---|---|---|
| N1v | RimWorld's own menu on virgl 720p | ≥30 fps median, input | **51.02 fps median**, input fixed, audio not reachable | `renderer=virgl/llvmpipe` (r1 has no host 3-D driver for the 4090) |
| N3v | virgl 3-D submit + read-after-clear (kernel only, no Mesa) | `[VIRGL3D] match=4096/4096` | M1 gate green in-tree | `renderer=virgl` |
| N6v | `vkcube` draws + presents | two screendumps, frames > 0 | Gate P5/P6 harness in `tools/cp/venus/` | `renderer=venus` |
| N8v | Cyberpunk 2077 benchmark, 720p | P9 script end to end | **29.19 avg / 29.7 median / 18.2 1%-low**; host native control 139.41 avg | `renderer=venus` |
| N9v | "reasonable performance" on the fallback | 720p ≥30 fps incl. present | RimWorld met. Cyberpunk **not met, and not a tail fix away**: avg 29.19 / median 29.66 / 1 %-low 18.18, with the gate line at the run's own *median* (51.9 % of frames over it) — the frame cost has to move ~1.65×. Analysis and the named wall: `docs/nv-n9v-tail.md` | `renderer=venus` |

The fallback's remaining lever **for r1's rows** is the **r1 host renderer**
(`docs/nv-r1-host-renderer.md`): r1 has no `/dev/dri` at all, so its games rows are
llvmpipe-class; installing a host driver for the 4090 would move them to GPU numbers,
and it is blocked on owner approval because it changes the host the card lanes depend
on. It is **not** the lever for N8v's 29.19: that number was measured on strix, whose
host already renders on the 9070 XT through
`egl-headless,rendernode=/dev/dri/renderD129`, so a host renderer is present in that
datum (`docs/nv-n9v-tail.md` §6).

---

## 3. What is withdrawn, and what is not

Withdrawn on this branch:

* "the games rows are the same as NVIDIA's driver" — the reference behaviour is
  no longer NVIDIA's stack; the fps targets stay, the comparison does not;
* `GRAPH_UNITS` as a *declared* topology — the rung N1n produced the measured row on
  2026-11-26 (11 GPC / 64 TPC, `src=register`), so the number is no longer a guess; what
  the shim still answers (`src=declared`, `mock_has_no_topo` on a node with no model) is
  the *shim's* honesty about having no card, not a measurement gap;
* treating "GSP-RM boots" as the only route to a submit — N3r is parked, and the other
  native gate, N3n, closed in class on 2026-11-27 (see §2.1).

Not withdrawn:

* the fps targets at 720p (≥30 fps) on every games rung;
* vkcube, RimWorld and the Cyberpunk benchmark as the three data points;
* the GSP-RM work itself: `nv_gsp.c`, the WPR2 contract, the FWSEC launch and the
  booter harness stay in the tree and green, because N3r runs the day a signed
  booter exists.

---

## 4. Critical path, honestly

* **No cheap native gate is left.** Both were spent and settled in 2026-11-26/27: N1n
  measured the topology and N3n closed in class (§2.1). What remains on the native
  branch — N4n onward — needs an **AD102-signed** ACR/FECS/GPCCS ucode set that NVIDIA
  does not publish, so it is not a gate that can be bought with a boot or a week; it is
  the declared ceiling (`docs/nv-plan.md` §2.13, revive condition R1/R3).
* The fallback critical path is a **harness/fps** question, not a bring-up question: it
  already has all three data points, and its remaining gap is N9v's 1.65× frame cost,
  whose levers are all outside the card (`docs/nv-n9v-tail.md` §6 — native 3-D in the
  guest, a cheaper translation chain, or re-scoping the 1 %-low bar, which is an owner
  decision).

---

## 5. Reproduce a row

| row | command | the line to look for |
|---|---|---|
| N0 | `tools/gate/` present gate (see `nvidia-driver.md` §5) | `(1919,1079)=(255,0,0)` |
| N1n | `tools/nv/deploy.sh --mode test --gpu` | `[NV-ENG] pgraph window ... verdict=` and `[NV-ENG] measured gpc=` |
| N3n | stage `ga102/sec2/hs_bl_sig.bin`, one boot (§6 of `nv-no-rm-path.md`) | `[NV-SEC2T] S tl ... u94=` |
| N3r | `tools/nv/boot-n.sh 1 r` with a vendor `booter_load` | `[NV-GSP]` stage-5 row / `0x1430` block |
| N1v | `tools/rw/run-rimworld-desktop.sh` on r1 | fps median row + `renderer=` |
| N3v | `tools/gate/m1_virgl.py` | `[VIRGL3D] ... match=4096/4096` |
| N8v | `tools/cp/venus/run_vkcubex_venus.py` / P9 harness | benchmark summary + `Venus` device row |
