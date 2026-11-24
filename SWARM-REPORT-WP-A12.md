# WP-A12 — `0x002B0009` is the payload check, not the launch: it is invariant over every host input and moves only with the signature block (or with the key the ROM is asked for)

Card **t_c724a763** (SEC2 booter: the ROM rejects our signed image). Builds on
rounds 9–11 (`SWARM-REPORT-WP-A9.md`, `-A10`, and the input bisect in
`t_e2bd20b3`). Work tree: a copy of
`/srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_0384c646/sec2`
(`e8948293` + `nv-sec2-launch-path.patch` + `nv-sec2-halt-probe.patch` +
`nv-sec2-auth-bisect.patch`) in
`/srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_c724a763/sec2`.
Harness: `tools/nv/sec2-boot-lane.sh <n> <tag>`. Six boots are cited: `bv1a`,
`ul1a`, `ul2a`, `ul3a`, `oc2a`, `fz1a`, `eg1a` (all in `evidence/nv/`).

**Question.** Which authentication input rejects our image, or prove that it
cannot be satisfied on this part.

**Answer in one paragraph.** The rejection is not an input the host supplies.
Measured this round: the code word is `0x002B0009` for **two different
vendor-signed images** (`booter_load` 55 928 B and `booter_unload` 39 800 B,
different IMEM/DMEM geometry, different signatures) in the same launch; it is
`0x002B0009` for **both** 384-byte signature slots; for **both** IMEM coverage
layouts (`app@IMEM0` and `os_code+app@IMEM0`); for **both** boot vectors (0 and
`0x100`, the reference's value); and for the four BROM words at the reference's
values. It changes in exactly two ways, and both are measured: (i) disturb a
byte of the 384-byte block → `0x00250009`, same timestamp; (ii) ask the ROM for
a **different engine's key** (`BROM_ENGIDMASK` 1 → `0x400`, the GSP bit) →
`0x0004000B` **at +31.7 µs instead of +281.7 µs**. So the SEC2 key exists and
the ROM gets ~250 µs further with it — the RSA and the long hash run — and then
rejects the payload. Three defects were found and fixed in *our* path along the
way (the fuse register was the wrong one, the slot rule was the wrong one, and
two container guards refused the `booter_unload` image); none of them changes
the verdict, and the fuse the part reports turns out to be satisfiable. The
input that cannot be satisfied is the **signed identity/digest inside the
384-byte block**, which the host can neither write (it needs the fused private
key) nor read (the ROM exposes neither its key store nor the digest it expects).

---

## 1. The reference, and the three defects it exposed in our path

Two independent, working sources were read this round. Both are more specific
than `docs/nv-gsp-wpr-contract.md §4.2`:

| question | NVIDIA driver (`src/nvidia/src/kernel/gpu/gsp/kernel_gsp_booter.c`) | nova-core (`drivers/gpu/nova-core/…`) |
|---|---|---|
| patch site | `hsSigDmemAddr = patchLoc - dataOffset` (0x7710−0x7700 = **0x10**) | `pkc_data_offset = patch_loc - os_data_offset` |
| signature source | `pSignatures + (numSigs-1-fuseVer)*sigSize` | `idx = fuse_ver - reg_fuse_version` |
| fuse register | `ksec2ReadUcodeFuseVersion()` → `HIGHESTBITIDX(fuse)+1`, address from `dev_fuse.h` | `signature_reg_fuse_version_ga102()`: `engine_id_mask&1` → `NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION` |
| BROM words | `ENGIDMASK ← meta[1]`, `CURR_UCODE_ID ← meta[2]`, `MOD_SEL ← Rsa3k` | `program_brom_ga102()` writes the same four, in the `0x841xxx` window |
| load | app (secure) → IMEM 0, DMEM → 0, `BOOTVEC = app[0].offset` | same |

`NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION = 0x00824140` — from NVIDIA's own
`dev_fuse.h` (`ampere/ga100`), and the independent working driverless AD102
loader `gpu-unlock` uses the same address (`GU_FUSE_SEC2_UCODE1 0x00824140`).

Defects this exposed here, each fixed in this tree:

1. **The fuse register was the wrong one.** `nv_gsp.c` read
   `NV_PFUSED_FW_VERSION0 + (id-1)*4` = `0x8241C8`. That is the generic FPF
   version block, not the SEC2 ucode-version block. The SEC2 block is
   `0x824140`. Every earlier statement that "the fuse cannot distinguish the
   ucode id" was a statement about the wrong register. Measured now
   (`fz1a`): the SEC2 block reads
   `1 ×13, then 0x7F, 0x7FFF, 0x7FFF` for ucode ids 1..16.
2. **The slot rule was the wrong one.** `nv_fwsec_sig_index()` selects "the
   version bit above the fuse word's top set bit" (a FWSEC rule), which picks
   slot **1** here. Both NVIDIA's driver and nova-core select
   `numSigs-1-fuseVer` = `2-1-1` = **slot 0**. Measured consequence: none —
   slot 0 and slot 1 give the same code word (round 11, re-measured below).
3. **Our own parser refused the `booter_unload` image.** `ul1a`:
   `[NV-GSP] fw BOOTER.BIN parse FAILED`; `ul2a`: `stage5 result=-22` with no
   SEC2 row. Two causes, both ours: the load-header convergence check rejected
   a data region whose images cover 0x9700 of 0x9800 with 0x100 **zero**
   padding (now accepted only if the padding is zero), and
   `nv_gsp_sec2_start_booter_ex()` had a `len < 0x7700+0x6000` guard that only
   the 55 928-byte load image satisfies (now guarded on the header only; every
   offset is already bounds-checked by the parse).

## 2. The measurement matrix (six boots)

Every row below is a launch of the same SEC2 path from a reset + the four GSP
state fixups. "control(S)" is the reference-configured launch in the *same
boot* as each variant (`S` vs `W`), so reset, image, transport, mailbox and
timing are shared.

| boot | what was varied | code word `0x840094` | timing after START | other channels |
|---|---|---|---|---|
| `fx1a` (shipped build) | nothing (control) | `0x002B0009` | +279.6 µs | `INTR=0x10`, `0x240=0x3002`, `verified=0x5EC2A0A0` |
| `bv1a` | BOOTVEC 0 → `0x100` (reference value) | `0x002B0009` both | +281.7 / +280.3 µs | same |
| `oc2a` | IMEM = `os_code+app` (0x7700 B at IMEM 0) | `0x002B0009` both | +279.4 / +282.1 µs | same |
| `ul3a` | **image = `booter_unload`** (IMEM 0x4800, DMEM 0x4E00) | `0x002B0009` | **+270.4 µs** | same, `MBOX0=0x94E0000` (its own meta) |
| `fz1a` | read the SEC2 fuse block at `0x824140` | `0x002B0009` | +279.6 µs | `reg_fuse_version=1`, NVIDIA idx 0 **valid** |
| `eg1a` | `BROM_ENGIDMASK` 1 → `0x400` (GSP key) | **`0x0004000B`** | **+31.7 µs** | same `INTR`/`SEC_MODE`/sentinel |
| `ctl0a` (r11) | slot 1 → 0 | `0x002B0009` both | +281 µs | same |
| r12/r13 (r10) | one byte of the 384-byte block | **`0x00250009`** | +280 µs | same |

The timings are each launch's own `post_first` START (`since_t0_ns − wr_to_read_ns`) subtracted from the `tl` row that first carries the word, so they are comparable across boots.

Three facts fall out of the matrix:

* **The verdict is image-independent, and the payload is what is being timed.**
  `ul3a` is a different container (`data_size 0x9800`, IMEM `0x4800` at file
  `0x478`, DMEM `0x4E00` at `0x4C78`, patch site `0x4910` → DMEM `0x10`, its own
  two 384-byte slots) and it produces the same word — but **11 µs earlier**
  (`+270.4` vs `+279.6`–`+281.7 µs`), with 0x4000 fewer image bytes to process.
  Two different payloads do not normally produce the identical code; a stage
  that *reads the payload* and then judges it does, and its length shows in the
  timestamp. (The `ul3a` row's `DOES-NOT-TILE` wording is the old message: its
  arithmetic is right — `0x478+0x4800 = 0x4C78`, `+0x4E00 = 0x9A78`, file
  `0x9B78` — and the 0x100 tail is the zero padding of §1.3.)
* **The block is read, and read for real.** One flipped byte inside it moves
  the word to `0x00250009` at the same timestamp; the marker `0x003900FF` is
  written 226 µs earlier (and is *absent* for the `booter_unload` image while
  the final word is identical — so it is progress, not the verdict).
* **The key selection is live and the SEC2 key exists.** Asking for the GSP key
  (`0x400`) produces a different word **250 µs earlier**. A key that is present
  and usable is exactly what lets the ROM spend 226 µs on the hash/RSA with the
  SEC2 key and only then reject.

`fz1a` in full:

```
[NV-SEC2] fuse SEC2 ucode block @0x824140: 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x7F 0x7FFF 0x7FFF (one dword per ucode id 1..16)
[NV-SEC2] fuse SEC2 ucode=3 word=0x1 -> reg_fuse_version=1 | NVIDIA slot rule idx=num_sigs-1-reg=0 (valid)
[NV-SEC2] sig block@0x3C slots=2 bytes=0x180 fuse@0x8241C8=0x1 patchsite=0x7710 -> dmem+0x10 fuse_rule_slot=1
```

The container's own descriptors, parsed with the reference's structures
(`nvfw_bin_hdr`, `nvfw_hs_header_v2`, `nvfw_hs_load_header_v2`), agree with the
reference on every field: `sig_prod_off 0x3C`, `sig_prod_size 0x300`,
`num_sig 2` (slot 0x180), `patch_sig 0` (⇒ block at 0x3C), `patch_loc` field
holding `0x7710`, `meta = {fuse_ver 1, engine_id 1, ucode_id 3}`,
`load header = os_code 0/0x100, os_data 0x7700/0x6000, app 0x100/0x7600`.

## 3. The named wall (acceptance b)

**The input that cannot be satisfied is the signed identity inside the 384-byte
block — what the ROM authenticates the payload against — not any input the host
supplies.**

The rows that show it, and what each one eliminates:

1. *It is not the image, but it is about the image* — `ul3a`: a second vendor
   image, different geometry, different signatures, same word, **11 µs
   earlier** in proportion to its smaller payload. The ROM reads the payload
   and then judges it; the judgement is wrong for both.
2. *It is not the signature slot or the version rule* — `ctl0a` (slot 1) and
   the round-11 slot-0 run: identical; `fz1a`: the SEC2 fuse at the correct
   register is `0x1` for ucode 3, i.e. version 1, which the container carries
   (NVIDIA's rule selects index 0, "valid"). The fuse is therefore
   **satisfiable**, and both slots fail anyway.
3. *It is not the coverage we can vary* — `oc2a` (`os_code+app`, 0x7700 B) and
   `bv1a` (BOOTVEC `0x100`): identical. DMEM is verified byte-for-byte against
   the container before START (`dmem landing match=8/8`, full-window diff
   `0/0x1800`).
4. *It is not the BROM parameter set* — the four words are written and read
   back at the reference's values (para `0x10`, engid `1`, ucode `3`, modsel
   `1`); varying engid to `3`, ucode to `2`, modsel to `0` (round 11) changes
   nothing, and the one variation that changes the *key* (`0x400`) proves the
   parameter is consumed.
5. *It is read and it is genuine-sensitive* — one flipped byte → `0x250009`.

What is left is the content of the block itself: the digest/identity the fused
SEC2 key decrypts it to and the ROM compares. The host cannot write that
(it would need the fused private key) and cannot read it (the ROM exposes
neither its key store nor the expected digest; `0x840094` is the only word it
leaves, and it has no readable PC on this part — round 9, four independent
sources). **This is the wall, and it is named as a wall: no host-supplied input
remains that changes the verdict.**

The honest caveat, stated because it is the one thing the rows cannot separate:
`0x002B0009` is reached only with a genuine signature, so it is *after* the RSA
work; whether the specific comparison that fails is the payload digest or an
identity field inside the decrypted block cannot be told apart from the host
side, because both are inside the same 384 bytes. What the rows do settle is
that the failing comparison is not one the host feeds.

## 4. What would break the wall

In order of leverage, all outside this card's budget:

1. **A container signed for a different fuse/key generation.** The 535.113.01
   images are the only SEC2 images in `third_party/nvfw`; a 570/6xx
   `booter_load` extracted from `open-gpu-kernel-modules` bindata (the
   `gpu-unlock` pipeline, `nouveau/extract-firmware-nouveau.py`) would be a
   second *generation* rather than a second *image*, and one boot would say
   whether the wall is the generation. **Not fabricated here: no such file is
   in the tree, and the download was outside this card's time budget.**
2. **A way to read the expected digest.** None on this part: `0x840094` is the
   only post-mortem word, and `SP`/`PC`/`BREAKPOINT` are unreadable in every
   state (round 9).
3. If (1) shows a newer generation authenticates, the fix here is a data
   change (the staged `BOOTER.BIN`), not code.

## 5. Reproduce

```sh
cd /srv/hermes-kanban/boards/hobbyos-nv/workspaces/t_c724a763/sec2
make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 -j8 disk.img
tools/nv/sec2-boot-lane.sh 1 fz1                     # control + fuse rows
NV_BOOTER_FILE=third_party/nvfw/ad102/gsp/booter_unload-535.113.01.bin \
  tools/nv/sec2-boot-lane.sh 1 ul3                   # the second image
# the in-boot variants: SEC2_AUTH_BISECT 5 = BOOTVEC 0x100, 6 = engid 0x400;
# SEC2_IMEM_OSCODE 1 = os_code+app at IMEM 0
grep -E 'X auth-bisect|fuse SEC2|image data_off' evidence/nv/eg1a.log
```

Images: `bv1a` `f23f2abc…`, `ul3a` `0c5416ed…`, `oc2a` `…`, `fz1a` `…`, `eg1a`
`37b3fbab…` (each boot prints its own sha256). The `booter_unload` run uses the
lane's `NV_BOOTER_FILE` override (added this round, default unchanged).

## 6. Gates

| gate | result |
|---|---|
| `make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 -j8 disk.img` | exit 0, no `nv_gsp.c` / `nv_fw.c` warning |
| unit tier | see the card completion (`run_unit_tests_intel.sh`) |
| boots | six, each with `stage5 result` present and ≥ 290 `[NV-SEC2T]` rows except the two parse-failure boots, which are kept as the evidence for defect 3 |
