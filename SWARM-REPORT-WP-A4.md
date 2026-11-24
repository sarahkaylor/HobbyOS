# SWARM-REPORT-WP-A4 — the FRTS carve, seen through BAR1 (round 4)

Branch `nv/wp-a4-bar1`, cut from `nv/integ-swarm` @ `5cc95b9d`. Six deploys of an
eight-deploy budget (one of the six answered nothing; see r4c). Every row below
cites a log in `evidence/nv/r4*.log`.

## 1. What this round was asked, and what BAR1 is for

Rounds 1–3 closed every host-side falcon channel: the GSP window re-fences within
one iteration of START (1 event in 649,152 samples, 40,572 DMEM samples all
`0xBADF5620`), the engine store queue accepts four requests and retires none, and
`0x1438` carries no error code. The only verdict left was **halt time**: 27.6 ms
when the FRTS command is rejected, 105.8 ms when it completes.

BAR1 is a different decode — PFB's view of VRAM, with no falcon fence on it — and
the FRTS region (`0x5FE300000`, 1 MiB) physically lives in it. So a firmware write
there is readable by a channel the fence does not cover, *if* the aperture decodes
1:1 that far up. That was an open question in the contract
(`docs/nv-gsp-wpr-contract.md` §3.5 and §7 item 11: "which register re-bases the
BAR1 window on AD102 … without it we cannot write or read anything at 24 GiB").

**Answer, MEASURED, and it needed no re-basing register:** BAR1 decodes VRAM 1:1 at
23.7 GiB from the bare BAR. Evidence in all six runs:

| control | value |
|---|---|
| pattern probe at `0x01000000` (inside the 1 GiB window mapped at boot) | `wrote 0xA4B1C0DE → read 0xA4B1C0DE`, original restored |
| pattern probe at `0x5F0000000` (23.7 GiB, 134 MiB below `nonWprHeapOffset`) | `w0=0xFEEBC0DE w1000=0xFEEB3F21` — two pages 4 KiB apart hold *different* values, so the window is not aliasing one page |
| alias check | writing 23.7 GiB up the aperture leaves `0x01000000` unchanged (`NO-ALIAS`) |
| 1 MiB prefill at the FRTS region, read back | `words=0x40000 stuck=0x40000 other=0 sum=0x96940000` |

`0x40000` = 262,144 words read back in one pass: a write buffer cannot hold 1 MiB,
so the prefill is in VRAM, not in a posted-write queue.

## 2. The instrument (files changed)

| file | what |
|---|---|
| `src/kernel/nv_vram.c` (new, 445 lines) | the BAR1 channel: window open at `bar1_phys + vram_off`, 32-bit accessors, chunked region scan with a sentinel, a **read-kind classifier**, the behaviour probe, the page map, and the prefill+pre-scan entry point |
| `src/include/nv_vram.h` (new) | the same, with the address model and the 1:1 assumption stated |
| `src/kernel/nv_gsp.c` | prefill + probes before START (`[NV-BAR1]` rows), post-launch probes and the scan after the halt, and the metadata-copy read at `gspFwWprStart`; `nv_gsp_fwsec_launch()` takes `wpr_start` |
| `src/kernel/nv_test.c` | `test_nv_vram_scan_and_judge`: the sentinel/scan counts, the BLIND cases, the read-kind table, bounds, and "no BAR1 window ⇒ `-ENODEV`, never a window at 0" |

Three properties are instrumented rather than assumed, each because a run without
them would have produced a confident wrong answer:

1. **A prefill that did not read back cannot report a write** (`nv_vram_judge` →
   `BLIND`). "0 words changed" out of a region nobody could write is not a
   negative.
2. **An address that answers a sequence is not memory** (`nv_vram_reads_kind`).
   Eight reads of one word must return the same value; `+1` per read is a poison.
3. **The extent of a fence is a count, not a claim** (`nv_vram_page_map`, one
   character per 4 KiB page, nine pages at each edge).

## 3. Per-run table

| run | log | what it ran | halt time (verdict) | what BAR1 showed after the launch |
|---|---|---|---|---|
| **r4a** | `r4a-bar1-frts-scan.log` | first shape: prefill + 1 MiB scan, no read-kind probe | `0x64F0A80` = **105.843 ms** (FRTS completed) | scan: `untouched=0 changed=0x40000` → the block printed **"FWSEC WROTE VRAM"**. **That claim is retracted**; the same log refutes it (offset 0 read `0xBAD0AC0A`, then `0xBAD0AC2A` 32 reads later) |
| **r4b** | `r4b-bar1-poison-vs-memory.log` | adds the behaviour probe (8 reads, two addresses, write-back) | `0x6503C00` = **105.922 ms** | FRTS region: `rep 0xBAD0AC34…0xBAD0AC3B` = **ACCESS-COUNTER**, `ONE-COUNTER-FOR-BOTH` (the address is ignored), write-back not readable. bootbin / wpmeta / scratch: **MEMORY**, writable |
| **r4c** | `r4c-fault-injected-control.log` | intended to be the fault-injected control; **ran the r4b binary because the tree was not synced** | `0x64EA320` = 105.817 ms | identical to r4b, down to the counter seed `0xBAD0AC34`. **Answered nothing new** — a repeat, recorded as one |
| **r4d** | `r4d-fault-injected-control.log` | control: `init_cmd 0x15 → 0x99` (read back through `nv_fwsec_read_cmd`) | `+0x1A5DF60` = **27.648 ms**; `sentinel_changed=0`, `0x1438 = 0x5EC25A5A` | `untouched=0x40000 changed=0 sum` equal to pre → **NOTHING-WRITTEN**; every probe **MEMORY** |
| **r4e** | `r4e-region-edge-walk.log` | accepted case + page-granularity edge walk | `0x64F3B80` = **105.856 ms** | `below=MEMORY frts-first=POISON frts-last=POISON above=MEMORY bootbin=MEMORY wpmeta=MEMORY scratch=MEMORY`; edge lo `0x5FE2FC000`: **`MMMMPPPPP`**; edge hi `0x5FE3FB000`: **`PPPPPMMMM`** |
| **r4f** | `r4f-fault-control-repeat.log` | control repeat | `+0x1A5C800` = **27.642 ms**; `0x1438` sentinel intact | all **MEMORY**; edges `MMMMMMMMM` / `MMMMMMMMM`; **NOTHING-WRITTEN** again |

Before the launch, every run is identical: the prefill sticks word for word, the
FRTS region answers `MEMORY-SAME-VALUE-8x` with two independent lanes, and a
write into it reads back.

## 4. What BAR1 shows before and after a launch

**Before START.** The FRTS region is ordinary VRAM: the host's `0xA5A5A5A5`
pattern in all 262,144 words, eight reads of the same word agree, two addresses
4 KiB apart keep separate values, a write reads back.

**After an accepted FRTS (four observations: r4a, r4b, r4c, r4e).** The FRTS
region is no longer addressable memory:

```
probe tag=frts-post off=0x00000005FE300000
  rep: 0xBAD0AC34 0xBAD0AC35 0xBAD0AC36 0xBAD0AC37 0xBAD0AC38 0xBAD0AC39 0xBAD0AC3A 0xBAD0AC3B
  two-addr: 0xBAD0AC3C 0xBAD0AC3D … (one counter for both addresses)
  writeback wrote=0x4E563441 read: 0xBAD0AC45 0xBAD0AC46 0xBAD0AC47 0xBAD0AC48
```

…and the effect is exactly 1 MiB wide, at 4 KiB granularity:
`MMMMPPPPP` from `0x5FE2FC000` (the first `P` is `0x5FE300000`) and
`PPPPPMMMM` from `0x5FE3FB000` (the first `M` is `0x5FE400000`). Page below,
page above, the bootbin page *inside WPR2* (`0x5FE2F8000`), the metadata page at
`gspFwWprStart` (`0x5F8700000`) and a scratch page 134 MiB below the WPR all
answer memory and are writable.

**After a rejected FRTS (two observations: r4d, r4f).** The region is
byte-for-byte the host's prefill (262,144 words untouched, checksum equal,
`NOTHING-WRITTEN`), and both edges read `MMMMMMMMM`.

So the one variable that separates the two states is whether the FRTS command was
the one the ucode accepted: the impaired run is the one that halts at 105.8 ms and
writes `0x00005A5A` into `0x1438`; the unaffected run halts at 27.6 ms with
`0x1438` untouched. `init_cmd` is the only difference between them.

## 5. What FWSEC actually produced

* **Not a completion word.** Nothing in the region is readable afterwards, and the
  rejected run shows that nothing is written when the command is refused. A
  completion record in the FRTS region is refuted for this firmware/launch.
* **Not a garbage write over the region.** In the accepted runs the region leaves
  the host's address space; a garbage *write* would have left readable garbage.
  In the rejected runs nothing is written at all.
* **A region-scoped exclusion of exactly the 1 MiB it was handed** — the carve
  itself, MEASURED at 4 KiB granularity, page-aligned to `frtsOffset` and ending
  exactly at `frtsOffset + frtsSize = gspFwWprEnd`. This is the first
  recipe-independent effect of the FRTS command that is not a status register, and
  it required no success claim from `0x1438`.

Tag: the *observation* is MEASURED. The *mechanism* — WPR2 carve/firewall versus a
BAR1 block remap — is INFERRED: this run cannot tell them apart, because the
region's content is unreadable from the host either way.

## 6. Is the privilege drop visible from BAR1?

**No — measured, three ways, and this is the round's second finding.**

| question | answer | source |
|---|---|---|
| does the metadata copy at `gspFwWprStart` show acceptance (`verified == 0xA0A0…`)? | **no.** `magic=0 verified=0 nonzero_words=0` in all six runs. The page answers memory (so the read is good), and the copy is absent: the booter never wrote it | MEASURED |
| does the bootbin region show the booter's DMA? | unchanged: `0x5FE2F8000` reads `0` and is writable after both accepted and rejected launches | MEASURED |
| does SEC2 unfence? | `0x840100 = 0xBADF5620` in all six runs, including the three accepted ones | MEASURED |

There is no *direct* VRAM-side marker of the privilege level: the privilege drop is
a state of SEC2's register window, and the only VRAM-side trace of its consequence
is the booter's `verified` word — which requires SEC2 to run. So the honest
statement is: **the FRTS carve happened and the privilege drop's only observable
consequence did not.** Per the contract the same command does both
(`docs/nv-pmu-acr-contract.md` §3.2: "carves WPR2, drops privileges"), so either
the drop needs a host step that has not been performed (contract §3.3 step 11's
second GSP reset + RISC-V switch), or SEC2's read fence is not an oracle for the
privilege state — round 3 already declined to treat it as one.

## 7. Proven / refuted / unverified

**Proven (MEASURED, ≥2 runs or a one-variable control):**

* BAR1 decodes VRAM 1:1 at 23.7 GiB from the bare BAR; no window re-basing is
  needed. Closes the contract's §3.5 / §7-item-11 blocker for `[0, fbSize)`.
* The FRTS region is host-readable and host-writable **before** START, and the
  host's 1 MiB prefill survives there word for word.
* After a launch the FRTS command was accepted for, `[0x5FE300000, 0x5FE400000)`
  answers an access counter (`0xBAD0AC0A+`, +1 per access, address ignored) and
  nothing else in VRAM does.
* When the FRTS command is rejected (`init_cmd 0x99`, halt 27.6 ms), the region
  keeps the host's bytes: FWSEC wrote nothing into it.
* The exclusion is page-exact: `frtsOffset` … `frtsOffset+frtsSize`.
* No metadata copy, no booter landing, SEC2 still `0xBADF5620`, in every run.

**Refuted:**

* "A post-launch change of the 1 MiB region means FWSEC wrote 1 MiB" (r4a's first
  reading). The region answers a counter; the change is the fence, not content.
* "BAR1 is entirely unfenced after START." True for VRAM generally, false for the
  one region the FRTS command was handed.

**Unverified / open:**

* Whether the FRTS data bytes exist behind the exclusion, and what they are.
* Mechanism of the exclusion (firewall vs aperture remap) — see §5.
* Whether the exclusion is reversible without a VM restart (every run starts from a
  fresh VM, so its per-boot lifetime is MEASURED; within-boot reversal is not).
* Whether SEC2's `0xBADF5620` is still privilege state now that a carve is proven.

## 8. Next step (one experiment, now cheap)

The sequencing rule in `PLAN.md` §2 was "no SEC2 work until FWSEC's channel says it
ran". That condition is now satisfiable without a status register: the carve is a
physical effect of the accepted command. The next deploy should therefore **attempt
the booter once, with the bootbin region prefilled** (`0x5FE2F8000`, `0x8000` bytes,
through the instrument that already exists) and judge it by two things that are not
the fence: whether those bytes are replaced by the booter image (visible through
BAR1, since that page reads as memory), and whether `verified` at
`gspFwWprStart` moves off 0. A booter that runs but is rejected will still show the
copy; a booter that never runs leaves the sentinel.

Also worth one row on the next run: read `0x1FA824/828` again *after* the carve —
the contract says they are read-protected, so an unchanged `0xBADF5620` there is
expected, but a change would name the mechanism.

## 9. Gates

| gate | result |
|---|---|
| `nvswarm build wp-a4` (intel) | green, no new warnings in `nv_vram.c` / `nv_gsp.c` |
| `./run_unit_tests_intel.sh` | **584 tests, 0 failed** (was 583; the new case is `test_nv_vram_scan_and_judge`) |
| the new case can fail | watched RED: with the BLIND check mutilated, `EXPECT_EQ FAILED: nv_vram_judge(&blind, &blind) == NV_VRAM_EFFECT_BLIND at src/kernel/nv_test.c:766`, `Tests failed!` |
| ARM per-TU | `obj/arm/nv_vram.o`, `obj/arm/nv_gsp.o`, `obj/arm/nv_test.o` compile clean (`#ifdef __x86_64__` empty TUs) |
| card hygiene | VM 300 stopped, VM 312 untouched and stopped, no qemu left behind on r1 |

Not verified by me: the full ARM link (broken pre-existing at `ce4c44af`,
`.bss exceeds the limine placement window`).

## 10. Budget

8 deploys allowed, **6 used**, 2 held back. r4c answered nothing (stale binary —
`nvswarm sh` does not sync; every later deploy was preceded by an explicit
`nvswarm sync` plus a remote `grep` of the flag under test).
