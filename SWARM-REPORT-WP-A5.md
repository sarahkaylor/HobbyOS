# WP-A5 — the SEC2 booter attempt, judged through BAR1

Branch `nv/wp-a5-booter`, cut from `b3edb2a2` (`nv/integ-swarm`, green at intel
584/0, ARM 468/0). Eight deploys, one named log each, all in
`evidence/nv/r5*.log`. Nothing pushed, nothing merged; `ada/`, `lane-*/` and the
other `wp-*` worktrees untouched.

**The answer in one line:** SEC2's window was never privilege-fenced — it was
sitting on the RISC-V core, the same state round 1's F3 cleared on the GSP — and
with that cleared SEC2 loads and executes the booter for the first time in this
project. The booter itself still produces nothing: the metadata's `verified`
word stays at its sentinel and the bootbin region is byte-for-byte the host's
prefill, in all eight runs.

---

## 1. The question, and the two judgments asked for

Round 4 made "FWSEC ran first" provable without a status register: the 1 MiB
FRTS region leaves the host's view exactly when the FRTS command is accepted,
and the halt time says which command ran. That was the sequencing gate, so the
attempt was made — `nv_gsp_sec2_start_booter_ex(..., force=1)` — and judged two
ways through BAR1, after the attempt:

| judgment | instrument | result |
|---|---|---|
| (i) metadata `verified` → `0xA0A0A0A0A0A0A0A0` | the 256-byte block in host memory, prefilled with `0x5EC2A0A0` | **never moved**: `0x5EC2A0A0` in every run, both attempts of the runs that make two |
| (ii) the bootbin region at `0x5FE2F8000` changing because a booter DMA wrote it | BAR1 prefill (`0xA5A5A5A5`, `0x8000` B) + word-for-word comparison against the bootloader's own data bytes | **never changed**: `0x2000/0x2000` words still the host's pattern, all eight runs |

Two further receipts, both negative, both free: no metadata copy at
`gspFwWprStart` (`0x40/0x40` words still the host's pattern), and no WPR2 over
the WPR (the bootbin page map reads `MMMMMMMMMP` before **and** after the
attempt, i.e. nothing was carved beyond FWSEC's own 1 MiB).

The FWSEC receipt reproduced in all eight runs, so the premise held every time:
`frts-post` = `ACCESS-COUNTER 0xBAD0AC34+n`, halt **105.73–105.92 ms**
(spread 0.18 ms), edges `MMMMPPPPP` at `0x5FE2FC000` and `PPPPPMMMM` at
`0x5FE3FB000`.

---

## 2. Per-run table

| run | log | what it changed or asked | BAR1 before / after | `verified` moved? | bootbin changed? | verdict |
|---|---|---|---|---|---|---|
| **r5a** | `r5a-sec2-attempt-image-fixed.log` | the attempt, with the image corrected: data-region-relative offsets (`imem@0x378 len 0x7700`, `dmem@0x7A78 len 0x6000`, ends `0xDA78` = file size), advancing DMA destination, fuse-selected signature slot 1 | prefill stuck `0x2000/0x2000`; post `untouched=0x2000 changed=0` | no (`0x5EC2A0A0`) | **no** | attempt made behind a still-fenced window; null |
| **r5b** | `r5b-core-select-clear.log` | clear `0x841668` — the GSP's F3 write, one window over | same | no | **no** | **`0x841668` read `0x110` (RISCV\|BRFETCH, the exact GSP pre-clear value) → cleared to `0x1`; `ctl` became `0x10`, `transcfg=0x5`: the window answers.** Load then stalled at IMEM block 19 (`0x118=0x617` FULL stuck, `0x114`/`0x11C` still block 18's values) |
| **r5c** | `r5c-dma-cfg-and-attempt.log` | the GSP's three pre-load DMA writes on SEC2 (`0x624` `0x110→0x190`, `0x10C=0`, `0x600=5`) | same | no | **no** | **full load by DMA, all BROM words read back as written (`para=0x10 engid=1 ucode=3 modsel=1 bootvec=0x100`), `ctl=0x10 halted=1` — SEC2 executed.** Wrote nothing: `mailbox0` still the argument, `0x118=0x602`, `0x114=0x5F00`, `0x11C=0xD978` |
| **r5d** | `r5d-both-signature-slots.log` | both signature slots in one boot (A = fuse-derived 1, B = 0), identical image and transport | same | no, either attempt | **no** | signature slot **ruled out** |
| **r5e** | `r5e-imem-layout-app-at-0.log` | app code at IMEM 0, entry 0 (the contract's own reading) + landing map | same | no | **no** | layout/entry **ruled out** as stated; the landing map answered **nothing** (8/8 read errors — my bug: `flcn_pio` refuses a falcon with `present=0`) |
| **r5f** | `r5f-landing-and-dmem-diff.log` | force `present`, so the port answers; diff all DMEM after the attempt | same | no | **no** | **DMEM landing 8/8 LANDED both attempts** (signature patch at `+0x10` included); IMEM `0xDEAD5EC1` = secure-page marker (no claim); **DMEM after: `0/0x1800` words changed** |
| **r5g** | `r5g-falcon-rm.log` | the last GSP fixup SEC2 never got: `0x084` ← PMC_BOOT_0 (`was=0 → 0x192000A1`) | same | no | **no** | `FALCON_RM` **ruled out** as the blocker |
| **r5h** | `r5h-pc-samples.log` | sample PC (`0xFF0`) before/after START | same | no | **no** | **answered nothing**: PC reads `0xBADF5040` in every sample, before and after — the documented non-PC constant, so this channel cannot say whether anything executed |

---

## 3. What the eight runs actually establish

**PROVEN (MEASURED)**

1. **SEC2 was never privilege-fenced.** `0x841668` read `0x110` — RISCV|BRFETCH,
   bit for bit the value the GSP's `0x111668` read before round 1's F3 cleared
   it — and clearing it made the whole window answer (`ctl=0x10`,
   `transcfg=0x5`, `0x110/114/118/11C/120` real). PLAN §2's "SEC2's fence is
   privilege-gated (measured twice)" is **wrong for SEC2**: the `0xBADF5620`
   wall is the core select. It cost rounds 1–4 their reading of the failure.
2. **SEC2 executes ucode.** With the GSP's pre-load DMA configuration the image
   loads and `START` runs the core to a halt (`ctl` bit 4, `halted=1`), with the
   engine drained (`0x118=0x602`, `0x114=0x5F00` = one past the last IMEM block,
   `0x11C=0xD978` = one past the last DMEM source). First time in this project.
3. **The data image the core holds is the image on disk.** 8/8 DMEM probes read
   back as staged, including the 384-byte signature at `+0x10`.
4. **The booter writes nothing, anywhere**: no `verified`, no bootbin DMA, no
   meta copy in VRAM, no WPR2, no DMEM status word (`0/0x1800`), no return code
   in MAILBOX0 (it still holds the address we passed, which is the argument
   being its own sentinel — the code used to call that "a non-zero return code"
   and no longer does).

**REFUTED (MEASURED)**

- "SEC2's fence is privilege state" (see 1 above).
- The signature slot as the blocker: both slots null in one boot (r5d).
- The IMEM layout/entry as the blocker: `os_code+app @IMEM0 / entry 0x100`
  (r5c/r5d) and `app @IMEM0 / entry 0` (r5e) are both null.
- `FALCON_RM (0x084)` as the missing fixup (r5g).
- Three transport defects, each measured and fixed: the load header's offsets
  read as file rather than data-region offsets (`0x378` displacement, which put
  signature bytes at the head of IMEM); the DMA destination never advancing
  (round 1's defect, never applied to this path); and the missing pre-load DMA
  configuration that stalled the queue at block 19.

**UNVERIFIED (no claim)**

- **IMEM content.** Secure pages answer `0xDEAD5EC1`; the bytes the core fetches
  cannot be read back by the host. DMEM is verified, IMEM is not.
- **Whether the boot ROM ran at all.** No halt-time measurement was taken for
  SEC2, and the PC channel is a constant on this part.
- **Whether FWSEC completed its own job.** Its DMEM read-back has never been
  verified in any round; `0x1438` hi16 = 0 is "no error code", not "done".
- The `(engid, ucode_id, modsel)` triple beyond the contract's `MEASURED†`
  values (1, 3, 1).

---

## 4. Runs that answered nothing (said out loud)

- **r5e**: the landing map returned 8/8 read errors. The cause was mine —
  `flcn_pio` refuses any falcon with `present=0`, and SEC2's flag was set while
  the window still answered the fence word. Worse, the row then called a read
  error "NOT-LANDED". Fixed in r5f (flag forced, and `DMEM-UNREADABLE` is now a
  separate, claim-free verdict).
- **r5h**: the PC register reads `0xBADF5040` before, during and after START —
  the constant `nv_flcn.h` already documents as "looks like the fence word and
  is not it". It answered nothing, and the last deploy of the round was spent on
  it. The instrument that should have taken its place is **halt timing**, which
  is now measurable because the window answers (see next step 1).

---

## 5. Gates

| gate | result |
|---|---|
| `nvswarm build wp-a5` (intel) | green (`exit=0`), no new warnings from `nv_gsp.c`, `nv_vram.c`, `nv_test.c` |
| unit tier (final tree) | **585 tests, 0 failed** (`UNIT TESTS PASSED`) |
| the new suite fails before the fix | mutation `match_words >= post->words` → `>`, rebuild, tier returns **585 run / 1 failed**: `EXPECT_EQ FAILED: nv_bootbin_verdict(&pre, &post, 0x2000) == NV_BOOTBIN_MATCHED at src/kernel/nv_test.c:847`; reverted, green again |
| ARM per-TU | `obj/arm/nv_gsp.o`, `obj/arm/nv_vram.o`, `obj/arm/nv_test.o` all compile (`OK`); every change is inside `#ifdef __x86_64__` |
| card left clean | VM 300 and VM 312 **stopped**, no qemu process for either (`ps -eo args \| grep -c -- '-id 300'` → 0) |

---

## 6. Next step, in order of leverage

1. **Time the SEC2 halt (one deploy, no writes).** `ctl` answers now, so
   `START → STOPPED` can be measured with PTIMER, exactly as FWSEC's was.
   µs-scale ⇒ the START never reached the core (or the ROM faulted on entry);
   ms-scale ⇒ the ROM ran and rejected the image. This is the observation r5h
   tried to make with the PC and could not, and it splits the remaining space in
   two.
2. **If ms-scale (the image is being rejected):** the only unverified input is
   IMEM. Check offline whether the file's `0x7600` app-code bytes are ciphertext
   or plaintext RISC-V; if they are plaintext, loading them as-is guarantees the
   ROM decrypts garbage, and the fix is a different load, not a different
   signature.
3. **If µs-scale:** the ROM is not running at all; the lever is the boot state
   (secure-boot enable / BROM mode words around `0x840000`), not the image.
4. **Cheap variants once the halt time is known:** swap the two meta dwords
   (`engid=3, ucode=1`), and `ucode=9` as FWSEC uses.
5. **FWSEC's own completion:** with the port answering, read the FWSEC-written
   DMEM words *before* START on the GSP side. FWSEC is the only stage whose
   progress has never been observable, and everything above assumes it finished.
