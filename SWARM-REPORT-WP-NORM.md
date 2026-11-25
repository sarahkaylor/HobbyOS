# WP-NORM — the no-RM re-scope: what the matrix measured, and the one boot that decides it

Card `t_1044c642` ("BACKUP after the booter box: re-scope past GSP-RM"),
2026-11-24. Branch `nv/wp-no-rm` off `ab1381cf`. Parent: `t_7c890c1e` (the booter
wall: `0x840094 = 0x002B0009` on every reachable AD102 production image).

**Question.** With no NVIDIA-signed firmware executing, what can this AD102 do —
and if the answer is "nothing yet", which cheapest experiment moves it?

**Answer in one paragraph.** The re-scope is not "past GSP-RM", it is "past
SEC2's boot ROM": the reference driver's own host-side engine path
(`ga102_gr_load` → FECS/GPCCS via the ACR) is bootstrapped by an ACR whose
AHESASC application runs **on SEC2**, and its SEC2 bring-up loads a signed
container into the same falcon that rejected the GSP booter. And the ucode that
path needs is published for **GA102, not AD102**: linux-firmware's
`nvidia/ad102/` carries `gsp/` only, while `nvidia/ga102/` carries `sec2/`,
`acr/` and `gr/`. Cross-chip signatures are already measured to fail on this
part (`0x250009` for a GA102-signed booter). So the cheapest path to a real
submit is **one boot** with a non-booter container — the GA102 SEC2 HS
bootloader (`hs_bl_sig.bin`, identity triple `1/0x1/5`, which fits the part's
fuse table exactly as the booter's does) — and its first product is which
signature class that container falls into. That boot is prepared here and handed
to the card lane; it was not run, because this lane has no path to the 4090
(`ssh root@192.168.10.174` → `Permission denied (publickey,password)`). The
fallback is unchanged and already measured: Venus and virgl carry the three data
points (51.02 fps RimWorld, 29.19 fps Cyberpunk).

---

## 1. Deliverable 1: the feasibility matrix (`docs/nv-no-rm-path.md`)

Three rows, each with the experiment that would prove it and what the part
blocks, plus the measured evidence behind every cell:

* **VBIOS/PMU — closed.** The PMU is pre-loaded and idle (`UC_CTRL=0x20`
  SLEEPING, v6), no `PMC_ENABLE` bit lifts the SEC2 fence, the documented ucode
  command set has no "boot SEC2"/"load ucode" opcode, and this ROM carries no
  devinit script (three independent scans, 0 hits). The experiment (D6 dump +
  one command-word probe) would make the state measured; it cannot change the
  decision.
* **Host-side engine classes — plumbed, and gated at SEC2.** `ga102_gr_new()`
  refuses only when `nvkm_gsp_rm()` is true; without RM it builds the full
  `gf100_gr` path, including netlists and FECS/GPCCS **through the ACR**
  (`nvkm_acr_lsfw_load_bl_sig_net`). The ACR's AHESASC is `NVKM_ACR_HSF_SEC2`,
  the WPR bootstrap owner is `NVKM_ACR_LSF_SEC2`, and `ga102_sec2_new()` without
  RM loads `sec2/{desc,image,sig,hs_bl_sig}`. Every road runs through SEC2.
* **Honest fallback — met.** Native claim capped at L1 + measured L2; games rows
  keep their fps targets and now carry `renderer=`.

New measurements this card added (all reproducible, `tools/nv/no-rm/`):

| measurement | result |
|---|---|
| this VBIOS's falcon-ucode table | 7 applications (`01 07 08 45 85 49 89`), **no FECS/GPCCS**; table only resolves with the `+0x14E00` EFI-strip rule |
| descriptor plaintext in the 1 MiB ROM | **none** (0 valid `FALCON_UCODE_DESC` anywhere) → descriptors/payloads are inside the `0xE0` container |
| fence map, both committed vendor dumps | PRI `0x111000` is 1024/1024 fenced pre-init; PDISP answers unreadable-by-design in **both** states; SEC2 is 16 fenced pre-init and still 11 fenced with RM up |
| container identity triples | booter `1/0x1/3`, SEC2 hs_bl `1/0x1/5`, ACR `2/*/1` (fuse version 2 has no precedent on this part) |
| container extent rule | the published AD102 booter ends at `data_offset+data_size` (57 720 B) while `bin_size` says 57 856 — the data region is authoritative |
| **non-GSP ucode ownership** | linux-firmware `nvidia/ad102/` = `gsp/` only (booter/bootloader/scrubber); `sec2/`, `acr/`, `gr/` exist for **ga102** only — so the non-GSP chain has no AD102-signed ucode to run |

**What this settles that was open in the plan:** §2.7's "one card deploy could
turn the declared topology into a measured one" now has an implementation and a
known register pair (`0x409604`, `0x500000+i*0x8000+0x2608` — the `GPC_UNIT`
base is `0x500000`, which the plan's shorthand omitted). Row 2's "what of it is
still reachable" is answered by code: all of it, behind SEC2.

---

## 2. Deliverable 2: the cheapest path to a real submit, implemented as far as it goes

`src/kernel/nv_engine.c` (+ `struct nv_eng_report` in `nv.h`, called from
`nv_bringup`, 4 unit cases) — a **read-only** GR-window probe:

* seven named PGRAPH registers classified against the three sentinels;
* if the window answers, the topology is decoded and published only when a
  control register in the same block answered data **and** the count is in 1..32
  (so "zero GPCs" cannot masquerade as a measurement);
* it cannot fail the bring-up, and it writes nothing.

This is N1n of the re-pointed ladder: four reads that decide whether anything
above it can be attempted, and that replace the DECLARED 11/64 topology with a
measured one if the window answers.

`tools/nv/no-rm/hs_gate.py` — the static gate a container must pass before a boot
is spent on it: parses `nvfw_bin_hdr`/`nvfw_hs_header_v2`, prints the identity
triple, applies the one fuse reading this project has measured, and refuses to
call a container compatible when its declared fuse version has no precedent. It
also states, in its own output, what a static gate cannot see (the signature
class), because "identity compatible" was one boot away from being read as "will
run". It carries a 7-container selftest (booter AD102/GA102/TU102, sec2 hs_bl,
three ACR images) which reproduces the booter sweep's static drops:
`selftest: 7 containers checked, 0 mismatched`.

**One pre-existing test bug fixed on the way.** `test_nv_gsp_rpc_stage8_...`
read `r.built` from an uninitialised `struct nv_gsp_rpc`; `nv_gsp_rpc_stage8_into`
uses that field as its "already set up" latch, so the test only passed while the
stack happened to hold 0 there. Growing `struct nv_dev` (the new `eng` report)
moved the stack and the tier went red with `r.built == 1937251808`. The fix is
`t_zero(&r, …)` in the test — the field is the function's input, so a test that
reads it must initialise it.

**The boot itself is not run here.** It is one attempt on the card lane, staged
and specified in `docs/nv-no-rm-path.md` §6, with the decision table for each
possible `0x840094`. Handing that to the card lane is the correct move: this
profile has no route to the 4090, and a boot nobody ran is not evidence.

---

## 3. Deliverable 3: the ladder, re-pointed

`docs/nv-games-ladder.md` is created (the file `nv-plan.md` §4 names but which
never existed) and `docs/nvidia-driver.md` §7 points at it.

* Native rows: N0 (closed), N1n (implemented, not run), N2 (done), **N3n** (the
  SEC2 container gate, one boot), N4n→N9n blocked behind it, and **N3r** (GSP-RM
  boots) **parked, not cancelled**, as the "when a signed booter exists" branch.
* Fallback rows: N1v (51.02 fps), N3v (M1 virgl gate), N6v (vkcube on Venus),
  N8v (29.19 fps), each with `renderer=`.
* Withdrawn: the "same as NVIDIA's driver" comparison. Kept: every fps target,
  and the three data points.

---

## 4. What this card did not establish

* `[NV-ENG]` has not printed a row on the card — the probe is in the tree and
  unit-tested, not measured.
* The SEC2 HS bootloader boot is not run (§2), so **whether this AD102 verifies
  GA102-signed SEC2/ACR ucode** is predicted from the booter-class measurement
  (`0x250009`), not measured for that class.
* The container `fuse_ver` ↔ fuse-word mapping is established only for the
  booter's case (`1` ↔ `0x1`); the ACR images' `2` is a candidate, not a drop.
* No local 3-D run was possible: this host has no DRM render node (`egl: no drm
  render node available`), so `virtio-gpu-gl` cannot be negotiated here and the
  fallback's numbers stay r1's.

## 5. State and reproduction

GPU: untouched by this card — no boot, no VM, no register write from this profile.
The only hardware interaction attempted was `ssh root@192.168.10.174`, which
refused (`Permission denied (publickey,password)`), which is why every hardware
step above is specified and prepared rather than executed.

Tier: **594 tests, 0 failed, `UNIT TESTS PASSED`** (`docs/nv-no-rm-path-evidence/unit-tier-nv-engine.txt`), run
as `make ARCH=intel MODE=unit_tests TOOLS_SKIP=1 run` under TCG (`SKIP_KVM=1`;
this account is not in group `kvm`). Two environment facts, so a reader does not
re-derive them: the tree's **tools stage does not build on this host**
(`bin/clang-21` link: `undefined symbol: pthread_setname_np`, in
`llvm/lib/Support/Threading.cpp`, unrelated to this change), which is why
`TOOLS_SKIP=1` is on every build here; and the repo's `run_unit_tests_intel.sh`
passes neither `TOOLS_SKIP` nor a long enough timeout for TCG, so the three
commands below are the reproducible form.

```sh
# the matrix's numbers
python3 tools/nv/no-rm/vbios_ucode.py <path>/vbios.bin          # §3.2
python3 tools/nv/no-rm/vdesc_scan.py <path>/vbios.bin           # 0 descriptors
python3 tools/nv/no-rm/fence_map.py                             # §2
HS_CORPUS=<fw-dir> python3 tools/nv/no-rm/hs_gate.py --selftest # §4.2
# the probe, in the tier and (when a card slot exists) on the card
make ARCH=intel MODE=unit_tests TOOLS_SKIP=1 run QEMU_ARGS="-display none"
tools/nv/deploy.sh --mode test --gpu                            # prints the [NV-ENG] rows
```

Raw outputs: `docs/nv-no-rm-path-evidence/`.
