# What an AD102 can do with no NVIDIA-signed firmware executing

**The backup plan's first deliverable** (card `t_1044c642`, `docs/nv-plan.md`
§2.10 option 2). Written after `t_7c890c1e` declared the booter wall: no
reachable vendor `booter_load` gets past SEC2's boot ROM (`0x840094 =
0x002B0009` on three AD102 production builds, while a foreign-chip or
debug-signed image gives `0x250009`).

This document answers one question, per path, with evidence:

> what can this part do with **no NVIDIA-signed firmware executing**?

Provenance tags are the project's (§6 of the plan): **MEASURED** (a log line or a
committed dump from this card), **SOURCE** (a named function in public source),
**INFERRED** (my reading), **NOT ESTABLISHED** (do not guess).
Every measurement in this file is reproducible by a command named beside it,
and the raw outputs are in `docs/nv-no-rm-path-evidence/`.

---

## 0. The verdict, before the tables

**No path reaches a GPU submit without SEC2 executing a signed image, and the
two "native" paths converge on the same SEC2 boot ROM that closed option 1.**
That is not a restatement of the GSP-RM wall; it is a different, sharper fact:

| path | what it needs first | where it dies today |
|---|---|---|
| 1. VBIOS / PMU | a PMU that can be asked to boot something | the PMU is pre-loaded and idle, and no opcode in its documented command set boots SEC2 or loads engine ucode (MEASURED state + SOURCE opcode set) |
| 2. host-side engine classes | the ACR to bootstrap FECS/GPCCS | the ACR's AHESASC runs **on SEC2**, and the published ucode for that chain is **GA102-signed** while this part rejects cross-chip signatures (`0x250009`) |
| 3. honest fallback (virtio/Venus) | nothing from NVIDIA firmware | nothing — it already reaches submits and is already measured (51.02 fps RimWorld, 29.19 fps Cyberpunk) |

The **cheapest path that can reach a real native submit** is therefore not "past
GSP-RM"; it is *one container boot*: does SEC2's boot ROM accept a container that
is **not** the GSP booter — specifically the SEC2 HS bootloader that the
reference driver loads in its own non-GSP path (§4.3, §6)? That boot also
answers the question underneath it, which a static read of the firmware tree
answers first and less happily:

> **The ucode for the non-GSP path is published for GA102, and not for AD102.**
> linux-firmware's `nvidia/ad102/` carries `gsp/` only (booter_load,
> booter_unload, bootloader, scrubber). `nvidia/ga102/` carries `sec2/`, `acr/`
> and `gr/` — the containers the non-GSP chain loads. Cross-chip signatures are
> **measured** to be rejected on this part: a GA102-signed AD102 booter gives
> `0x250009`, the signature-class word, where the AD102 production signature
> gives `0x002B0009` (`t_7c890c1e`, three-way A/B).

So the native branch needs one of exactly two things, and neither is a matter of
effort: an **AD102-signed** non-GSP ucode set (NVIDIA does not publish one), or
a boot ROM that accepts a **GA102-signed** container (measured not to, for the
booter class). The §6 boot settles the second for the SEC2/ACR class; if it
answers `0x250009` the native branch is closed on this part, and row 3 is the
only honest answer. If it answers anything else, the signature class is not the
blocker and the FECS/GPCCS bring-up becomes a driver problem.

**What this changes in the plan:** the native claim stays at L1 + the measured
part of L2; the games rows keep their fps targets and are labelled with the
renderer that produced them; the GSP-RM work is **parked, not deleted**, as the
"when a signed booter exists" branch. `docs/nv-games-ladder.md` is the re-pointed
ladder.

---

## 1. What is measured right now

Each row is a measurement, not a claim; the command that reproduces it follows.

| # | fact | value | evidence |
|---|---|---|---|
| 1.1 | PMU firmware is resident and idle | `UC_CTRL = 0x20` = SLEEPING, `UC_CAPS2` v6 | `[NV] falcon pmu ... ctl=0x20 ... present`, `evidence/nv/*.log` |
| 1.2 | no `PMC_ENABLE` bit lifts the SEC2 fence | `pmc_enable=0x40000000`, 32-bit single-bit sweep lifts nothing | `[NV-SEC2] probe write-then-read ctl=0xBADF5620` (`nvidia-driver.md` §5.7) |
| 1.3 | SEC2 executes a signed container, then rejects it after the signature | `0x840094`: `0x1` → `0x3900FF` → `0x2B0009` at +4.16…4.21 ms, `INTR=0x10` EXIT, booter status header never written | `[NV-SEC2T] S tl ... u94=0x2B0009`, 6 kept boots (`t_7c890c1e`) |
| 1.4 | GSP falcon takes a real image (FWSEC lands in IMEM) but never halts | `0x114 = 0xF100`, `status1438 = 0xBEEF0000` (sentinel untouched) | `[NV-FWSEC] ... dma state post-imem`, `f1r-*` logs |
| 1.5 | this VBIOS carries **no devinit script** | script table entry 0, slots 7/12/15 all zero; 0 writes to `0x840000`/`0x840100`/`0x110600` in 1 MiB | `nv-devinit-contract.md` §5 |
| 1.6 | the VBIOS's falcon-ucode table names 7 applications and **no FECS/GPCCS** | app ids `01 07 08 45 85 49 89`; 16 entries, 9 empty | §3.2 below (`tools/vbios_ucode.py`) |
| 1.7 | none of those descriptors is plaintext in the 1 MiB image | 0 self-consistent `FALCON_UCODE_DESC` headers in the whole image | §3.2 (`tools/vdesc_scan.py`) |
| 1.8 | the part's per-ucode fuse words | `0x8241C0..0x8241FF` = 13 × `0x1`, then `0x7F`, `0x7FFF`, `0x7FFF` | `[NV-FUSE] reg 0x8241c0..1ff: ...` (`evidence/nv/f1r-dma-done-bit.log:177`) |
| 1.9 | the fuse check passes for the FWSEC image and picks **signature index 1** | `ucode=9 fuse=0x1 sigvers=0x3 and=0x1 sig_index=1` → `verdict=COMPATIBLE` | same log, line 178-179 |
| 1.10 | `0xBADF5040` is a **normal** answer on this part, not a fault | it persists at `PMC_INTR_0`/`PBUS_INTR_0` while the card renders Vulkan | `nvidia-driver.md` §5.5 |
| 1.11 | the fence map is per window, and it holds with GSP-RM running | see the table in §2 | `tools/fence_map.py` over the committed NVIDIA-580 dumps |
| 1.12 | **AD102 has no published non-GSP ucode** | `nvidia/ad102/` = `gsp/` only: `booter_load-{535.113.01,570.144}`, `booter_unload-*`, `bootloader-*`, `scrubber-570.144`. The `sec2/`, `acr/`, `gr/` sets exist under `nvidia/ga102/` | linux-firmware tree, `nvidia/ad102` and `nvidia/ga102` listings (2026-11-24) |
| 1.13 | cross-chip signatures do **not** verify on this part | GA102-signed AD102 booter → `0x250009`; GA100-signed → `0x250009`; AD102 debug-signed → `0x250009`; only AD102 production → `0x002B0009` | `t_7c890c1e` §2/§3 (six kept boots) |

---

## 2. The fence map: what the host can even address

Read from the two committed NVIDIA-580.178.04 BAR0 dumps
(`docs/nv-gsp-reference-evidence/`), classified against the driver's own three
sentinels (`src/kernel/nv_regs.h`). Reproduce: `python3 tools/nv/no-rm/fence_map.py <evidence-dir>`.

| window | range | pre-init (before any driver) | post-init (GSP-RM running / rendering) |
|---|---|---|---|
| PMC | `0x000000..0x000fff` | data=4, unreadable=2 | data=4, unreadable=2 |
| PBUS | `0x009400..0x0097ff` | data=2 | data=2 |
| PDISP / PBUS alias | `0x100000..0x100fff` | **unreadable=2** | **unreadable=2** |
| GSP falcon | `0x110000..0x1107ff` | absent=6, data=70, **fenced=436** | absent=38, data=262, exists-nr=28, unreadable=184 |
| GSP tail | `0x110800..0x110fff` | absent=65, data=96, fenced=192, unreadable=158 | absent=74, data=125, unreadable=312 |
| PRI (RISC-V) | `0x111000..0x111fff` | **fenced=1024 (all of it)** | data=111, exists-nr=79, unreadable=820 |
| PFB / WPR2 | `0x1FA800..0x1fa83f` | data=14, unreadable=2 | data=14, unreadable=2 |
| SEC2 | `0x840000..0x8407ff` | data=1, **fenced=16** | data=6, **fenced=11** |

Three readings that matter for every path below:

1. **The whole `0x111000` PRI window is fenced before init** (1024/1024), and with
   GSP-RM running most of it is *still* not host-readable (820 unreadable). A
   host-side bring-up that assumed it could program that block would be wrong.
2. **SEC2 is fenced pre-init and partly fenced even with the vendor stack up** —
   and yet the booter runs there in our own lane (§1.3). So "fenced" is a
   read-side privilege answer, not "the engine is not ours to use": judging SEC2
   by its read-back is exactly the mistake §6.1 of the plan warns about.
3. **PGRAPH (`0x400000..`) and PCE (`0x104000..`) are not in either dump.** The
   reachability of the 3-D window is therefore *unmeasured*, and that is the
   first experiment row 2 needs. It is implemented and read-only (§4.4).

---

## 3. Row 1 — VBIOS / PMU: what can the PMU be asked without ACR/RM?

### 3.1 The PMU is not a lever on this generation (MEASURED + SOURCE)

| question | answer | tag |
|---|---|---|
| Is the PMU running? | It is **loaded and idle**: `UC_CTRL=0x20` SLEEPING, `UC_CAPS2` v6 | MEASURED (§1.1) |
| Who loaded it? | The fused boot ROM, from the VBIOS, before the host is involved; PMU ucode is not something the host uploads on GP102→Ada (`gm200_pmu_nofw`) | SOURCE |
| Does either stack send it a command? | No: the vendor's `kgspBootstrap_TU102` path contains no PMU transaction; nouveau refuses the whole PMU subdev when GSP-RM is in use (`gp102_pmu_new()` → `-ENODEV`) | SOURCE |
| Can it be asked to boot SEC2? | Not through its documented command set: the ucode-cmd table is EEPROM/VBIOS/register access + license verify, and **no opcode boots an engine or loads ucode** | SOURCE (envytools `nvrm/pmu/ucode-cmds`, 390-era) |
| Would a hand-written command help if the interface were live? | No. A command interface is not an authentication bypass: SEC2's boot ROM verifies what it loads (§1.3), and the PMU cannot sign | INFERRED |

**What this path blocks:** the PMU is a pre-loaded light-secure falcon whose
firmware the host cannot replace. It can be *asked* for register/EEPROM reads
(if its command queue is live — unmeasured), and it cannot be asked to run new
code, boot SEC2, or load engine ucode. So the VBIOS/PMU path yields no submit
path and does not shortcut the boot ROM.

**The experiment that would prove it** (1 boot, no writes to the GPU core, ~13 min):

```
read 0x10A000..0x10A0FF and 0x10A4A0..0x10A4CF (96 dwords)   # D6 of nv-pmu-acr-contract
write 0x10A040 = 0x00000002 (EEPROM-read command), poll status for 2 ms, read back
```
Positive (`MAILBOX0` status moves, error word 0): the PMU command interface is
live — and it still cannot boot SEC2, which is the load-bearing half.
Negative (status never moves): the interface is inert on Ada and the row is
closed in one deploy. Either way the *decision* does not change; the experiment
exists to keep the row MEASURED rather than SOURCE-only.

### 3.2 What the ROM actually carries (MEASURED, this card's committed dump)

Parsed from `docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.gz`
(sha256 `7dfb3428…b863`, verified on decompression). Reproduce:
`python3 tools/nv/no-rm/vbios_ucode.py <vbios.bin>`.

| item | measurement |
|---|---|
| PCI ROM image | starts at 0 (`0xAA55` at 0, no IFR header on this image) |
| BIT header | one candidate at `0x1B0`: header 12 B, token stride 6, 19 tokens, header checksum = 0 mod 256, BCD `0x0100` |
| token list | `32 42 43 44 49 4d 4e 50 53 54 55 56 78 64 70 75 69 45 73` |
| falcon-data token | `0x70` at `0x41F`, payload `0x00080DE8`; resolves only as `+0x14E00` (EFI-stripped logical image) to **`0x95BE8`** |
| ucode table | `version=1 hdr=6 entry=6 count=16 desc_v=1 desc_size=48` |
| applications present | `0x01`(tgt 1), `0x07`(tgt 6), `0x08`(tgt 1), `0x45` FWSEC_DBG(tgt 7), `0x85` FWSEC_PROD(tgt 7), `0x49`(tgt 5), `0x89`(tgt 5); the other 9 entries are empty |
| FECS / GPCCS in the ROM | **absent** (no `0x20`/`0x21`-class application in the table) |
| descriptor plaintext | **none**: 0 self-consistent `FALCON_UCODE_DESC` (v2/60 or v3/44) anywhere in the 1 MiB |

Consequences, and they are the reason row 2 is not a VBIOS problem:

* the GR ucode **is not in the card's ROM** — so a host-side GR bring-up must get
  FECS/GPCCS from the driver package, not from the VBIOS (they are published; §4.2);
* the descriptors and payloads for the applications that *are* named are not
  plaintext in the image, i.e. they live in the `0xE0` container — which is why
  the devinit doc found no host-replayable script either (§1.5).

---

## 4. Row 2 — host-side engine classes: what is reachable without GSP-RM?

The plan's phrasing was "the `gf100`-style register programming that
`ga102_gr_new()` short-circuits *because* GSP-RM is present". Reading the
reference driver settles what that short-circuit is, and what it is not.

### 4.1 The short-circuit is one `if`, and the code behind it is complete (SOURCE)

```c
/* nvkm/engine/gr/ga102.c */
int ga102_gr_new(struct nvkm_device *device, ...)
{
        if (nvkm_gsp_rm(device->gsp))
                return -ENODEV;
        return gf100_gr_new_(ga102_gr_fwif, device, type, inst, pgr);
}
```

With GSP-RM absent, that function **does not refuse**; it builds the ordinary
`gf100_gr` object with the GA102 function table, whose firmware loader is:

```c
/* ga102_gr_load(), same file */
nvkm_acr_lsfw_load_bl_sig_net(subdev, &gr->fecs.falcon, NVKM_ACR_LSF_FECS,
                              "gr/fecs_", ver, fwif->fecs, ...);
nvkm_acr_lsfw_load_bl_sig_net(subdev, &gr->gpccs.falcon, NVKM_ACR_LSF_GPCCS,
                              "gr/gpccs_", ver, fwif->gpccs, ...);
```

So the host-side path is **plumbed, not missing**: netlists, FECS, GPCCS, the
per-GPC topology reads (`0x409604`, `GPC_UNIT(i,0x2608)`), the 2-D class
(`FERMI_TWOD_A` is in `ga102_gr.sclass`, i.e. 2-D is served *by* GR on this
part, not by a separate block). The first thing it does *not* do itself is load
its ucode: `nvkm_acr_lsfw_*` is the ACR's loader.

### 4.2 Which ucode, signed by whom — and for **which chip** (MEASURED inventory)

NVIDIA signs all of it with a per-chip production key. The files are public, but
**not for this part**: `nvidia/ad102/` carries `gsp/` only, while the non-GSP
chain's containers exist under `nvidia/ga102/`:

| file (linux-firmware) | size | sha256 (16) | set |
|---|---|---|---|
| `nvidia/ga102/sec2/hs_bl_sig.bin` | 1280 | `1e24000c438e12a8` | **GA102 only** |
| `nvidia/ga102/sec2/desc.bin` / `image.bin` / `sig.bin` | 660 / 99 584 / 2248 | `1fd6fd67…` / `e103b573…` / `d2485ddb…` | **GA102 only** |
| `nvidia/ga102/acr/ucode_ahesasc.bin` | 57 088 | `ca69d947…` | **GA102 only** |
| `nvidia/ga102/acr/ucode_asb.bin` / `ucode_unload.bin` | 28 672 / 17 920 | `27ee1438…` / `ef413c92…` | **GA102 only** |
| `nvidia/ga102/gr/NET_img.bin` | 146 088 | `56b3b302…` | **GA102 only** |
| `nvidia/ga102/gr/fecs_bl.bin` / `fecs_sig.bin` | 288 / 2248 | `b1153ed2…` / `ecb36698…` | **GA102 only** |
| `nvidia/ga102/gr/gpccs_bl.bin` / `gpccs_sig.bin` | 288 / 2248 | `fa2ad555…` / `c298ede1…` | **GA102 only** |
| `nvidia/ad102/gsp/*` | — | — | **AD102**: booter/bootloader only |

Full hashes: `docs/nv-no-rm-path-evidence/firmware-sha256.txt`.

And data written by the card as machine facts: the fourth column matters more
than any table. A GA102-signed image on this AD102 fails **the signature itself**
(`0x250009`), where an AD102 production image gets through the signature and
fails afterwards (`0x002B0009`, §1.13). So "the ucode exists" and "the ucode can
run here" are two different statements, and only the second one matters.

Each secure container additionally declares an **identity triple** (`fuse_ver,
engine_id, ucode_id`) that the boot ROM compares against the part's fuses. Parsed
by `tools/nv/no-rm/hs_gate.py`:

| container | fuse_ver | engine_id | ucode_id | static verdict |
|---|---|---|---|---|
| AD102/GA102 `booter_load` 570.144 | 1 | `0x1` | **3** | IDENTITY-COMPATIBLE — and it still failed after its signature verified (`0x2B0009`) |
| TU102 `booter_load` 570.144 | 0 | `0x1` | 13 | UCODE-ID-NOT-FOR-THIS-PART (the static drop the sweep made) |
| GA102 `sec2/hs_bl_sig` | 1 | `0x1` | **5** | IDENTITY-COMPATIBLE |
| GA102 `acr/ucode_ahesasc` | **2** | `0x1` | 1 | FUSE-PRECEDENT-MISSING |
| GA102 `acr/ucode_asb` | **2** | `0x400` | 1 | FUSE-PRECEDENT-MISSING |
| GA102 `acr/ucode_unload` | **2** | `0x1` | 1 | FUSE-PRECEDENT-MISSING |

Read that table the way the evidence allows: the *booter's* triple is the only
one this project has measured to be compatible with the part (`sig_index=1`,
§1.9) — and identity compatibility is **necessary, not sufficient** (the booter
proves that). The SEC2 HS bootloader (`1/0x1/5`) fits the same measured reading
as the booter; the ACR containers declare fuse version 2, a class for which this
part's fuse words offer no precedent (all 13 are `0x1`). That makes the ACR a
*candidate to test*, not a drop and not a matched pair.

### 4.3 The convergence: everything ends at SEC2 (SOURCE)

This is the finding that re-scopes the plan, so it is stated as code, not as
inference:

| call | what it needs | runs where |
|---|---|---|
| `ga102_sec2_new()` with GSP-RM | `r535_sec2_new()` — RM's own SEC2 | — |
| `ga102_sec2_new()` **without** GSP-RM | `nvkm_sec2_new_()` → `ga102_sec2_load()` → `nvkm_acr_lsfw_load_sig_image_desc_v2(..., "sec2/")`: loads `desc.bin`+`image.bin`+`sig.bin`+`hs_bl_sig.bin` | **SEC2** |
| `ga102_sec2_acr_0.bootstrap_falcons` | `BIT(FECS) \| BIT(GPCCS) \| BIT(SEC2)` — the SEC2 ACR app bootstraps the engine falcons | **SEC2** |
| `ga102_acr_ahesasc_fwif` | `NVKM_ACR_HSF_SEC2` — the ACR's application runs on SEC2 | **SEC2** |
| `gp102_acr_wpr_build()` (used by GA102) | `bootstrap_owner = NVKM_ACR_LSF_SEC2` | **SEC2** |
| `ga102_gr_load()` | `nvkm_acr_lsfw_load_bl_sig_net` for FECS/GPCCS — an ACR LSFW load | ACR on **SEC2** |
| `ga102_acr_new()` / `ga102_gr_new()` | both refuse when `nvkm_gsp_rm()` — so the non-RM path is *only* reachable when RM is absent | — |

So the host-side engine path does not avoid the falcon that rejected the booter;
it *starts* at it. "Past GSP-RM" and "past SEC2's boot ROM" are the same pivot,
and the second one is where the evidence is.

### 4.4 What is still unmeasured, and the cheap experiment (implemented)

The `[NV-ENG]` probe now in the tree (`src/kernel/nv_engine.c`, called from
`nv_bringup`) answers, read-only, with no writes:

* is the PGRAPH window host-addressable at all (7 named registers, classified
  against the three sentinels);
* if it is, what the **measured** topology is: `0x409604 & 0x1f` for GPC count,
  `0x500000 + i*0x8000 + 0x2608` for each GPC's TPC mask — the same two reads the
  reference driver's `gf100_gr_oneinit` makes, which is what turns the plan's
  DECLARED 11/64 model (§2.7) into a measurement.

It deliberately refuses to publish a topology when the word reads 0 with no
control register answering in the same block, because "zero GPCs" and
"unreadable" have looked identical on this project before (§6 rule 1). Unit
coverage: 4 cases in the kernel tier (absent / fenced / measured 11-64 /
zero-topology-is-not-a-measurement).

One boot prints it: `tools/nv/deploy.sh --mode test --gpu` (or
`tools/nv/boot-n.sh 1 eng`). Nothing else in the tree changes because of it: the
probe cannot fail the bring-up.

---

## 5. Row 3 — the honest fallback, and what "labelled" means

| data point | number | renderer that produced it | where |
|---|---|---|---|
| RimWorld's own menu, 720p | **51.02 fps median**, input works, audio not reachable | virgl on r1 → host **llvmpipe** | `docs/nv-plan.md` §4 N1 |
| Cyberpunk 2077 benchmark, 720p | **29.19 avg / 29.7 median / 18.2 1%-low** | **Venus** (platform), host native control 139.41 avg | §4 N8 |
| vkcube | draws + presents (rung N6's data point) | Venus/`/dev/gpu` path | `tools/cp/venus/` |

What the fallback claims: the two games and vkcube are **already** driven
end-to-end on HobbyOS through the virtio path, and those numbers are the ladder's
data points. What it does **not** claim: that NVIDIA's driver produced them, or
that the native driver will match NVIDIA's driver on this part.

The mechanical part of "clearly labelled": every games row in
`docs/nv-games-ladder.md` carries a `renderer=` field naming *who* submitted the
work (`venus`, `virgl/llvmpipe`, or `nv/AD102`), and a row with no renderer is a
doc bug. The native driver's own claim is capped at **L1 + the measured part of
L2** until a native submit exists.

**What this path blocks:** the "native driver runs the games" claim, for as long
as the native branch is closed. It blocks nothing else — and it is the only path
that reaches a real submit today.

---

## 6. The one boot that decides the native branch (N3n)

**Question:** does SEC2's boot ROM accept a *non-booter* NVIDIA container — the
SEC2 HS bootloader `nvidia/ga102/sec2/hs_bl_sig.bin` (triple `1/0x1/5`) — and
does it accept a **GA102-signed** container at all on this AD102?

**Why this one:** the booter (`1/0x1/3`, AD102-signed) is rejected *after* its
signature verifies (§1.3), while a GA102-signed booter is rejected *at* the
signature (`0x250009`, §1.13). The published non-GSP chain exists only for
GA102, so the first fact this boot produces is which of those two classes the
SEC2/ACR images fall into. If it is the signature class, no published ucode can
run the non-GSP chain on this part and the native branch closes with a measured
word instead of an argument. If the signature verifies, the reference driver's
non-GSP chain (§4.1–4.3) becomes live enough to try.

**Preparation (done here, static):**

```
python3 tools/nv/no-rm/hs_gate.py <fw>/ga102/sec2/hs_bl_sig.bin
# -> IDENTITY-COMPATIBLE, triple 1/0x1/5, 1280 B, PIO image (no data region)
```

**The boot (card lane, one attempt, ~13 min):**

```
# stage the HS bootloader in place of the booter, same lane, same harness:
NV_BOOTER_FILE=<fw>/ga102/sec2/hs_bl_sig.bin tools/nv/sec2-boot-lane.sh 1 hsbl
grep -E '\[NV-SEC2T\] S tl ' evidence/nv/hsbl-a.log       # 0x094 progression
```

The harness must be told the container is a bootloader-only image: the existing
gate `len < 0x7700 + 0x6000` refuses it by design (the booter's geometry), so the
lane needs the bound taken from the container's own load header (`os_code_size`,
no data region) instead of a constant. That is a 5-line change in the lane tree,
not a driver change; it is listed as the card's first step so the boot cannot be
spent on a refusal to stage.

**Decision table:**

| `0x840094` after START | means | next |
|---|---|---|
| `0x250009` | **signature class**: this AD102 does not verify GA102-signed ucode, and no AD102-signed non-GSP ucode is published (§1.12) | native branch **closed** on this part; ladder points at row 3 for good, N3r stays parked |
| `0x002B0009` | the AD102 signature class verified (as it did for all three booters) and the post-signature comparison failed | native branch closed unless an AD102-signed container exists that NVIDIA does not publish; record and close |
| anything else, with `INTR` not EXIT | the HS bootloader ran and stopped somewhere of its own accord | next card: capture `0x840094` progression + `UC_CTRL`, then wire the ACR (`acr/ucode_ahesasc`) the way `ga102_sec2_acr_0` does — starting with the signature question for the ACR container |
| `0x0` with the booter's status header written | the non-GSP chain is live | N3n passes: next rung is the ACR bootstrap (FECS\|GPCCS\|SEC2) and the first GR channel |

Nothing weaker counts: a boot that merely lasts longer is not evidence, and no
synthetic status may stand in for the header (§6 of the plan).

---

## 7. What each path blocks, in one table

| # | path | can reach a submit? | what the part blocks | cost to settle |
|---|---|---|---|---|
| 1 | VBIOS / PMU | no | PMU firmware is pre-loaded and idle; its command set has no "boot SEC2" or "load ucode" opcode; the host cannot sign | 1 boot (D6 + one command) to make the row measured; decision does not depend on it |
| 2a | PGRAPH reachability + topology | **unknown — the first question** | unmeasured; PDISP answers unreadable-by-design in *both* states, the PRI window is 100% fenced pre-init | 1 boot, read-only, **implemented** (`[NV-ENG]`) |
| 2b | engine bring-up (FECS/GPCCS) | only if 2c passes | the published FECS/GPCCS/ACR/SEC2 sets are **GA102-signed**; this part's fuse rejects cross-chip signatures (`0x250009`) | weeks of driver work **after** 2c |
| 2c | SEC2 accepts a GA102-signed HS container | the gate for everything native | the signature class (cross-chip) and, behind it, the post-signature comparison | **1 boot** (§6) |
| 3 | virtio/Venus fallback | **yes, already** | nothing from NVIDIA firmware; the r1 renderer is llvmpipe until a host driver is approved | 0 — measured |

---

## 8. What changes in the ladder, and what does not

* `docs/nv-games-ladder.md` is created (the file `nv-plan.md` §4 already names)
  and re-points every row at the renderer that produced it. N3 splits into
  **N3n** (the SEC2 container gate, above) and **N3r** (GSP-RM boots — *parked*,
  not cancelled, behind "when a signed booter exists").
* The fps targets on the games rungs are unchanged. The "same as NVIDIA's
  driver" claim is **withdrawn** on this branch and replaced by "same fps target,
  different renderer, named".
* `GRAPH_UNITS` stays `src=declared` (11/64) until the `[NV-ENG]` row produces a
  measured one. The declared row is falsifiable by design; nothing else consumes
  it.
* The GSP-RM work (`nv_gsp.c`, the WPR contract, the FWSEC launch, the booter
  harness) stays in the tree and stays green: it is the branch that runs the day
  a signed booter exists, and N3r is its gate.

---

## 9. What this document does not establish

* **PGRAPH reachability** — not measured (no dump of `0x400000..` or `0x104000..`
  exists); the probe is implemented, the boot is not run.
* **Whether SEC2's BROM accepts a GA102-signed container on this AD102** — the one
  boot in §6. The booter-class answer is measured (`0x250009` for GA102-signed,
  §1.13); the SEC2/ACR class is predicted, not measured.
* **What the boot ROM's post-signature comparison compares** — the A13 report
  could not separate payload digest from identity field, and this document does
  not try.
* **The mapping between a container's declared `fuse_ver` and the per-ucode fuse
  word** — only the booter's case is measured (`1` ↔ `0x1`); the ACR containers'
  `2` has no precedent, which is why they are candidates and not drops.
* **The PMU command interface on Ada** — the 390-era opcode table is not an
  AD102 contract; the D6 probe would make the state measured, and cannot change
  the decision.
* **Whether upstream's device layer admits a GA102 with no GSP-RM at all** — this
  document only needed the code paths (§4.1–4.3), which exist and are what the §6
  boot would exercise.

---

## 10. Sources and reproductions

Local tools (this branch, `tools/nv/no-rm/`):

| tool | what it produces |
|---|---|
| `vbios_ucode.py` | §3.2: IFR/BIT walk, falcon-ucode table, application ids, descriptor resolution (including the `+0x14E00` EFI-strip rule) |
| `vdesc_scan.py` | §3.2: proof that no descriptor is plaintext in the 1 MiB image |
| `hs_gate.py` | §4.2: container identity triples, fuse check, and a 7-container selftest (`--selftest`) |
| `fence_map.py` | §2: per-window classification of the committed vendor dumps |

Committed evidence: `docs/nv-no-rm-path-evidence/` (parse JSON, fence map,
container dumps, firmware hashes).

Reference source read (tag-independent, `torvalds/linux` master; each claim names
its function):

* `nvkm/engine/gr/ga102.c` — `ga102_gr_new`, `ga102_gr_load`, `ga102_gr_fecs_reset`,
  `ga102_gr_gpccs_reset`, `ga102_gr` function table (`FERMI_TWOD_A` in `.sclass`,
  `MODULE_FIRMWARE("nvidia/ga102/gr/fecs_bl.bin")` …).
* `nvkm/engine/gr/gf100.c`, `gf100.h` — `gf100_gr_oneinit` (`0x409604`,
  `GPC_UNIT(i,0x2608)`), `GPC_UNIT(t,r) = 0x500000 + t*0x8000 + r`,
  `gf100_gr_units`.
* `nvkm/engine/sec2/ga102.c` — `ga102_sec2_new` (RM vs `nvkm_sec2_new_`),
  `ga102_sec2_load` (`"sec2/"`, `nvkm_acr_lsfw_load_sig_image_desc_v2`),
  `ga102_sec2_acr_0.bootstrap_falcons = FECS|GPCCS|SEC2`, `ga102_sec2_flcn`.
* `nvkm/subdev/acr/ga102.c` — `ga102_acr_new` (refuses on `nvkm_gsp_rm`),
  `ga102_acr_ahesasc_fwif` (`NVKM_ACR_HSF_SEC2`), `ga102_acr_asb_fwif`
  (`NVKM_ACR_HSF_GSP`), `ga102_acr_wpr_build`.
* `nvkm/subdev/acr/gp102.c` — `bootstrap_owner = NVKM_ACR_LSF_SEC2`.
* `nvkm/subdev/acr/lsfw.c` — `nvkm_acr_lsfw_load_sig_image_desc_v2` (the
  `desc/image/sig/hs_bl_sig` split and where the identity triple is read from).
* `nvkm/subdev/gsp/ga102.c` — `ga102_gsp_booter_ctor` (`meta[0..2]` =
  fuse_ver/engine_id/ucode_id), `ga102_gsp_fwsec_signature`.
* `include/nvfw/hs.h`, `include/nvfw/flcn.h` — container headers and
  `flcn_bl_dmem_desc_v2` layouts used by `hs_gate.py`.
* linux-firmware, `nvidia/ga102/{sec2,acr,gr}/` — the published, signed ucode
  enumerated in §4.2.

Project documents this stands on: `docs/nv-plan.md` (§0, §2.7, §2.10, §4, §6),
`docs/nv-pmu-acr-contract.md`, `docs/nv-devinit-contract.md`,
`docs/nv-gsp-wpr-contract.md`, `docs/nv-booter-candidates.md`,
`docs/nvidia-driver.md` §5.5/§5.7, and `SWARM-REPORT-WP-A13.md`.
