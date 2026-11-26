# GB202 (RTX 5090 `41:00.0`, VM 313) — the ACR wall is **parked**, and the honest ceiling

> **Coordinator note (2026-11-26).** This is the park record cited by
> `docs/nv-plan.md` §2.12 and §2.13. It was written by card `t_251d3c71`, whose
> §7 block is a *proposal*; that text has since been landed, edited, as plan
> §2.12. Read the plan for the authoritative wording; read this file for the
> row-by-row record. Copied into the repo unmodified apart from this note.

Card `t_251d3c71`, the pre-committed fallback for the GB202 ACR wall. Its parent,
`t_7178dca2`, closed **negative**: the vendor-computed FW heap reached the FSP and
the ACR still ended at `MAILBOX0 = 0x0B`. The card said that in that case this
document is written and the lane stops spending boots on the ACR. This is that
document.

Every row below is quoted from a log, and every log is named by path. Shorthand
used in this file:

```
ATT/<card>/<file>  =  /srv/hermes-kanban/boards/hobbyos-nv/attachments/<card>/<file>
WS/<card>/<file>   =  /srv/hermes-kanban/boards/hobbyos-nv/workspaces/<card>/<file>
```

**Part discipline.** Every `[NV-…]`, `mailbox`, `wpr2`, `census`, `fmcerr` and
`verdict=` row in §1–§4 is **GB202 in VM 313** (`[NV] state=… chip=GB202`, BAR1
`0x800000000` = 32 GiB, PFSP `0x8F0000`, ucode `0x44C`). The only AD102 numbers in
this file are in §6, labelled `AD102 (4090, VM 300)`. No GB202 number is reported
as AD102, or the reverse.

---

## 0. What "parked" means here

* No further GB202 boots are spent on the ACR. The lever list is exhausted as far
  as host-supplied state goes: every `GspFwWprMeta` field, the 88-byte boot-args
  page, both heap fields, `fbSize`, `frtsSize`, the FRTS object itself, the
  descriptor target, the signature/ELF/boot image, and the FMC generation have
  all been varied — each with a row, §1–§5.
* The GB202 lane's measured claim drops to **L1 + the measured part of L2**
  (§6). It does not claim L3, and it does not claim a frame.
* Nothing is deleted: the nine `t_a769beaa` workspaces, the eight `t_0936a17f`
  boots, the two `t_7178dca2` boots, their images and their logs stay where they
  are. `t_a769beaa` and `t_9bf9bc0c` stay blocked, `t_938ced62` stays `todo`, and
  each carries a comment pointing here.

---

## 1. The ACR ladder, as rows

Two instruments produced it. `t_a1c5bdd8` established which *descriptor* inputs
are refused and how; `t_a769beaa` then made one host-supplied RM artifact
unusable per boot and got a different fast code for each. Read together, the
ladder is:

```
descriptor unusable ................................. 0x1C  (t_a1c5bdd8)
   → descriptor accepted, FRTS-in-FB pair absent ...... 0x5   (t_a1c5bdd8, rm10)
      → FRTS pair present, FMC takes the mailboxes ... 0x0   (t_a1c5bdd8, rm36)
         → GSP-RM signature zeroed ................... 0x1D  (rm53, ≤150 ms)
            → radix3 ELF address unmapped ............ 0x02  (rm54, ≤150 ms)
               → boot image in the wrong container ... 0x1C  (rm55, ≤150 ms)
                  → everything accepted, attempt ends  0x0B  (every boot, 0.2–55 s)
                     — and WPR2 is torn down with it
```

### 1.1 The descriptor rungs (`t_a1c5bdd8`)

Report: `ATT/t_a1c5bdd8/REPORT-gb202-fmc.md`; logs `ATT/t_a1c5bdd8/5090-gb202fsp-rm10.log`,
`…-rm31.log`, `…-rm34.log`, `…-rm36.log`.

```
[NV-FSPCOT] cot gspFmcSysmemOffset=0x000000020C60E000 frtsSysmemOffset=0x000000020C63F000 gspBootArgsSysmemOffset=0x000000020C73F000 bytes=0x000000000000035C
[NV-FSPCOT] rm tl t_ms=200 hwcfg2=0x000000008187A7F7 m0=0x0000000000000005 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x00000000BADF5040
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
```

(rm10 `ATT/t_a1c5bdd8/5090-gb202fsp-rm10.log` gives `0x5` with the COT's
`frtsVidmemOffset/Size = 0/0`; rm36 `…-rm36.log` gives the second row with an
FRTS pair and `m0 = 0` at 150 ms — the report's rm35 quotes the same shape for
the 64 MiB COT.) Note the different `fmcerr` value: the `0x5` state leaves
`0xBADF5040` (the unreadable-window sentinel), the running state reads `0`. The
`0x1C` set was measured field by field in rm14/rm22/rm24: block magic, `revision
≠ 1`, `gspRmDescSize ∉ {0x100}`, `gspRmDescTarget = 0`, `gspRmDescOffset = 0`,
whole block zeroed — each `0x1C`; the same block copied to a different page is
still `0x5`. That is why `0x1C` is *not* a refusal of the block.

### 1.2 Signature, ELF, boot image (`t_a769beaa`)

Logs: `ATT/t_a769beaa/5090-gb202fsp-rm53.log`, `…-rm54.log`, `…-rm55.log`,
`…-rm56.log`.

```
rm53  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x000000000000001D m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm54  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000002 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm55  bootimg bytes=200812[NV-FSPCOT]  file=BLB.BIN d0=0x00000000000010DE d1=0x0000000000000001 d2=0x0000000000031100 d3=0x0000000000000018 code_off=0x000000000000B200 data_off=0x0000000000000A00
rm55  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x000000000000001C m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm56  [NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
rm56  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
```

Two facts fall out and they are the ones that matter:

1. **`0x0B` is none of those three.** Signature, ELF and boot image each have
   their own fast code (≤150 ms). `0x0B` only appears when all three are
   accepted.
2. **The boot image's *address* is not the discriminator** (rm56: address
   unmapped, bytes unchanged → same `0x0B` at the same time as baseline), while
   the boot image's *container form* is rejected early (rm55 → `0x1C`). The
   honest reading: `0x1C` is one code for "an artifact the FMC will not accept",
   measured for descriptor-field violations and for a boot image in the wrong
   container; the rows do not distinguish which check wrote it. What they do
   establish is that it is not the `0x0B` step.

---

## 2. The nine `t_a769beaa` boots — one host change each

Report: `ATT/t_a769beaa/REPORT-gb202-rm-objects.md`; per-boot logs
`ATT/t_a769beaa/5090-gb202fsp-rm{50,51,53,54,55,56,57,59,59b}.log`; condensed rows
in `ATT/t_a769beaa/KEY-ROWS-gb202-rm-objects.txt`. Nine boots, one COT each, one
image per boot (`/RM.CFG` = mode), `STOP_AFTER_VERDICT=1`.

| boot | mode | the one change | `MAILBOX0` | first seen | WPR2 |
|---|---|---|---|---|---|
| `rm50` | 50 | `gspRmDescTarget=2`, args `+56=2` | `0` (quiet) | 27 s | held |
| `rm51` | 51 | `GspFwWprMeta.fbSize = 0x800000000` | `0` (quiet) | 27 s | held |
| `rm53` | 53 | GSP-RM **signature** zeroed (4096 B) | **`0x1D`** | 150 ms | torn down |
| `rm54` | 54 | radix3 ELF address → unmapped `0x1000000000` | **`0x02`** | 150 ms | torn down |
| `rm55` | 55 | boot image = `BLB.BIN` (nvfw container) | **`0x1C`** | 150 ms | torn down |
| `rm56` | 56 | boot-image **address** → unmapped, bytes unchanged | `0x0B` | 40→55 s | torn down at 55 s |
| `rm57` | 57 | `GspFwWprMeta.frtsSize = 1 MiB` | `0x0B` | 40→55 s | held to 40 s |
| `rm59` | 59 | fine census, 12 s sample | `0x0B` (already down) | ≤12 s | torn down |
| `rm59b` | 59 | fine census, 15 s sample, measured extent | `0x0B` | ≤15 s | torn down |

Rows for the two "quiet" boots (the change reached the FMC and nothing moved):

```
rm50  [NV-FSPCOT] boot_args dwords[NV-FSPCOT]  d0=0x0x0000000000000000[NV-FSPCOT]  d1=0x0x0000000000000000[NV-FSPCOT]  d2=0x0x0000000000000002[NV-FSPCOT]  d3=0x0x0000000000000100[NV-FSPCOT]  d4=0x0x000000000C741000[NV-FSPCOT]  d5=0x0x0000000000000002[NV-FSPCOT]  d6=0x0x0000000000000000[NV-FSPCOT]  d7=0x0x0000000000000000[NV-FSPCOT]  d8=0x0x0000000000000000[NV-FSPCOT]  d9=0x0x0000000000000001[NV-FSPCOT]  d10=0x0x0000000000000001[NV-FSPCOT]  d11=0x0x0000000000000000[NV-FSPCOT]  d12=0x0x000000000C743000[NV-FSPCOT]  d13=0x0x0000000000000002[NV-FSPCOT]  d14=0x0x0000000000000002[NV-FSPCOT]  d15=0x0x0000000000000000[NV-FSPCOT]  d16=0x0x0000000000000000[NV-FSPCOT]  d17=0x0x0000000000000000[NV-FSPCOT]  d18=0x0x0000000000000000[NV-FSPCOT]  d19=0x0x0000000000000000[NV-FSPCOT]  d20=0x0x0000000000000000[NV-FSPCOT]  d21=0x0x0000000000000000
rm50  [NV-FSPCOT] rm tl t_ms=27000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
rm51  [NV-FSPCOT] wprm sizes heap=0x0000000008000000 nonwpr_heap=0x0000000000230000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000800000000 verified=0x0000000000000000
rm51  [NV-FSPCOT] rm tl t_ms=27000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
```

`rm51` matters twice: the descriptor really carried `fbSize=0x800000000`
(`wprm sizes … fb_size=0x800000000`), and the FSP's WPR2 pair did not move
(`0x07E61200/0x07F1EE00`, same as every baseline), so **FB size is not what the
FSP sizes WPR2 from** and is not what the ACR waits on.

`rm53`/`rm54`/`rm55` — the three fast rejections, full rows:

```
rm53  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x000000000000001D m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm53  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000001D m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm54  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000002 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm54  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000002 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm55  [NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x000000000000001C m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm55  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000001C m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
```

`rm56`/`rm57` — the `0x0B` transition, at 55 s, with the teardown in the same row:

```
rm56  [NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
rm56  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm57  [NV-FSPCOT] wprm sizes heap=0x0000000008000000 nonwpr_heap=0x0000000000230000 frts=0x0000000000100000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
rm57  [NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
rm57  [NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
```

Instrument caveat, stated because it changes how a table is read: in `rm56`/`rm57`
the `[NV-FSPRM] verdict=` row is the **27 s** sample and reads `m0_last=0`
(logs: `…-rm56.log`, `…-rm57.log`); the `0x0B` is in the 55 s `rm tl` row above,
not in the verdict row. Judge those boots by the timeline, not the verdict.

The census boot — the one that proves the ACR writes nothing into the WPR2 it was
given (256-byte stride over the first 8 MiB, 64 KiB over the whole band):

```
rm59b [NV-FSPCOT] wpr2c t15s lo=0x00000007E6120000 hi=0x00000007F1EE0000
rm59b [NV-FSPCOT] wpr2c done nz_head=0[NV-FSPCOT]  first_nz_head=0x0x0000000000000000[NV-FSPCOT]  meta=0[NV-FSPCOT]  nz_all=0[NV-FSPCOT]  first_nz_all=0x0x0000000000000000[NV-FSPCOT]  last_nz_all=0x0x0000000000000000[NV-FSPCOT]  last_val=0x0x0000000000000000
rm59b [NV-FSPCOT] rm post-fine t_ms=15000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm59  [NV-FSPCOT] rm pre-fine t_ms=12000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
rm59  [NV-FSPCOT] wpr2c t12s[NV-FSPCOT]  skipped (no live pair)
```

And the queue that would have carried any RM object (all nine boots, quoted from
rm50; identical in the other eight):

```
[NV-RPC] peer tl t_ms=150 v=0x0000000000000000 size=0x0000000000000000 msgsize=0x0000000000000000 count=0x0000000000000000 wp=0x0000000000000000 flags=0x0000000000000000 rxhdroff=0x0000000000000000 entryoff=0x0000000000000000 rp=0x0000000000000000 (header still zero)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=50 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000000 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000000000000 radix3=1
```

Nine boots, nine times `statq_written=0 linked=0 initdone=0`. No channel, no VA
space, no engine class, and no VA bound to VRAM — not because the shim refused,
but because there was never a peer to ask.

---

## 3. The eight `t_0936a17f` boots — the prologue after the FRTS-in-FB gate

Report: `ATT/t_0936a17f/REPORT-gb202-acr-prologue.md`; logs
`ATT/t_0936a17f/5090-gb202fsp-rm41.log`, `…-rm43.log`, `…-rm47.log` (the report's
table lists all eight: rm40/m30, rm41/m31, rm42/m33, rm43/m34, rm44/m35, rm45/m36,
rm46/m37, rm47/m38).

| boot | mode | the question | result |
|---|---|---|---|
| `rm40` | 30 | parent state + full-FB dumps/scans, samples to 60 s | `0x0B` at 35 s |
| `rm41` | 31 | timing: no BAR1, 7 register reads at 1 Hz for 90 s | `0x0B` between 27 and 28 s |
| `rm42` | 33 | park-then-start sweep of 16 fields | a second COT does not rebuild WPR2; every step terminal |
| `rm43` | 34 | whole-FB census (32 GiB at 128 KiB), at +3 s | three classes, no meta, no GFW |
| `rm44` | 35 | `frtsVidmemOffset` 64 M/256 M/512 M/1 G/2 G | every value lands in fence or poison |
| `rm45` | 36 | **176 MiB** (legal, writable FB address) as the first COT | same nothing; FRTS address still reads 0 |
| `rm46` | 37 | same, host waits 200 ms and looks | `0x0B` before 200 ms |
| `rm47` | 38 | same, then host places the FRTS blob in FB and points PBUS at it | `0x0B` ≈1 s later, unchanged |

The timing row (`rm41`) is the one that killed the "instrument effect" theory:

```
[NV-FSPCOT] ttl t_s=27 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 wpr2_lo=0x0000000007E61200 wpr2_hi=0x0000000007F1EE00 statq_d0=0x0000000000000000
[NV-FSPCOT] ttl t_s=28 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 statq_d0=0x0000000000000000
```

The census (`rm43`, no host activity before it):

```
[NV-FSPCOT] census done zero=259337[NV-FSPCOT]  dead=1519[NV-FSPCOT]  first_dead=0x0x00000007E6120000[NV-FSPCOT]  fence=1288[NV-FSPCOT]  first_fence=0x0x00000007F5F00000[NV-FSPCOT]  other=0[NV-FSPCOT]  first_other=0x0x0000000000000000[NV-FSPCOT]  last_other=0x0x0000000000000000[NV-FSPCOT]  last_val=0x0x0000000000000000[NV-FSPCOT]  meta=0[NV-FSPCOT]  gfw=0
```

1,519 × 128 KiB = 189.9 MiB of `0xDEADDEAD` starting exactly at `0x1FA824 × 256`,
1,288 × 128 KiB = 161 MiB of `0xBAD0AC00` at the top, `other=0`, `meta=0`,
`gfw=0`. The ACR neither copied the 256-byte meta into FB nor copied any of the
63.5 MB ELF.

The hand-placed FRTS (`rm47`) — the object the COT asks for, produced by the host,
handed to the ACR inside its live window:

```
[NV-FSPCOT] m37 pre m0=0x0000000000000000 hwcfg2=0x000000008187A7F7 wpr2_lo=0x0000000007DF1200 wpr2_hi=0x0000000007EAEE00 src_d0=0x0000000049574647
[NV-FSPCOT] m37 copy nz_dwords=74160 rb_d0=0x0000000049574647 rb_d1=0x0000000000000001 rb_d2=0x0000000000000028 rb_last=0x0000000000000000
[NV-FSPCOT] m37 pbus_after lo=0x0x00000000F5000000 hi=0x0000000000000007 cfg=0x0000000000000100
[NV-FSPCOT] m37 tl i=0 m0=0x0000000000000000 hwcfg2=0x000000008187A7F7 os=0x0000000000000000 wpr2_lo=0x0000000007DF1200 wpr2_hi=0x0000000007EAEE00 statq_d0=0x0000000000000000
[NV-FSPCOT] m37 tl i=1 m0=0x000000000000000B hwcfg2=0x000000008187A7F7 os=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 statq_d0=0x0000000000000000
```

74,160 non-zero dwords landed, the PBUS write latched with FB media type, and the
attempt still ended `0x0B` with WPR2 torn down within one second. **The FRTS rung
is closed by handing it the object, not by argument.**

---

## 4. The parent's two boots — the heap lever, measured and inert

Report: `ATT/t_7178dca2/gb202-fwheap.md`; logs
`ATT/t_7178dca2/5090-gb202fsp-fh58.log`, `…-fh620.log`.

`fh58` (mode 58) put the vendor's own `_kgspCalculateFwHeapSize` pair — 135 MiB /
`0x220000` — into the descriptor, one change vs baseline:

```
[NV-FSPCOT] wprm sizes heap=0x0000000008700000 nonwpr_heap=0x0000000000220000 frts=0x0000000000000000 vga_ws=0x0000000000020000 fb_size=0x0000000000000000 verified=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=40000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000000 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x0000000007E5A200 wpr2_hi=0x0000000007F1EE00 fmcerr=0x0000000000000000
[NV-FSPCOT] rm tl t_ms=55000 hwcfg2=0x000000008187A7F7 m0=0x000000000000000B m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000000000000
[NV-FSPCOT] wpr2c t58 lo=0x00000007E5A20000 hi=0x00000007F1EE0000
[NV-FSPCOT] wpr2c done nz_head=0[NV-FSPCOT]  first_nz_head=0x0x0000000000000000[NV-FSPCOT]  meta=0[NV-FSPCOT]  nz_all=0[NV-FSPCOT]  first_nz_all=0x0x0000000000000000[NV-FSPCOT]  last_nz_all=0x0x0000000000000000[NV-FSPCOT]  last_val=0x0x0000000000000000
[NV-FSPCOT] wprm-after verified=0x0000000000000000 wpr_start=0x0000000000000000 wpr_end=0x0000000000000000 heap_off=0x0000000000000000 heap_size=0x0000000008700000 nonwpr_off=0x0000000000000000 image_off=0x0000000000000000 bootbin_off=0x0000000000000000 frts_off=0x0000000000000000 frts_size=0x0000000000000000
[NV-FSPCOT] wprm-after untouched=yes (no field the ACR owns was written)
[NV-FSPRM] verdict=fmc-or-rm-window-open mode=58 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000000 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000000000000 radix3=1
```

The one counter that moved is the **size** of WPR2: `0x7E5A20000–0x7F1EE0000` =
196.750 MiB vs the baseline `0x7E6120000–0x7F1EE0000` = 189.750 MiB — `wpr2_lo`
down exactly 7.000 MiB, which is exactly the heap delta (7,340,032 B). The FSP
sized WPR2 from the descriptor. `MAILBOX0` still ended `0x0B`, the pair was torn
down, the band was empty, and the descriptor's own copy came back untouched.

`fh620` (mode 60) substituted the 620.70.10 GB202 FMC (287,304 B,
`hash == SHA-384(image)`, same prod key) to get the formatted error word:

```
fmc image bytes=287304[NV-FSPCOT]  file=FMC620.BIN fmc img_d0=0x0000000000480403 img_d1=0x0000000000046200
[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
[NV-FSPCOT] rm tl t_ms=150 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
[NV-FSPCOT] fmcerr verdict reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
```

The 570-generation FSP authenticates the 620 FMC (`errorCode=0`), the FMC writes
`0x14DC = 0x12000824` — variant 1, partition 1, **error code `0x824`**, info 0,
mirrored into `MAILBOX0` as `0x824` — and it aborts **before any live WPR2**
(`wpr2_lo=0x7FFFFE00` at the first sample, `wpr2c … skipped (bad pair)`, and `0x0B`
appears nowhere in that log). So `0x824` names the 620-FMC-on-570-inputs abort,
not the `0x0B` step; attaching it to `0x0B` would need a 620-generation boot image
or a 620-generation FSP, neither of which is obtainable on this box.

---

## 5. Ruled out, each with the row that rules it out

| hypothesis | ruled out by | row |
|---|---|---|
| `gspRmDescTarget / args+56 = 2` (NONCOHERENT) | `rm50` | `d2=0x2`, `d14=0x2` (full row in §2), `m0=0` to 27 s, `0x0B` later |
| `GspFwWprMeta.fbSize` | `rm51` | `fb_size=0x800000000`, WPR2 pair unmoved, `m0=0` to 27 s |
| descriptor `frtsSize` (1 MiB, to agree with the COT) | `rm57` | `frts=0x100000`, WPR2 unmoved, `0x0B` at 55 s |
| GSP-RM **signature** identity | `rm53` | `0x1D` at 150 ms with the pair already down |
| radix3 **ELF** address/identity | `rm54` | `0x02` at 150 ms |
| **boot image** identity / address | `rm56` | address unmapped, bytes unchanged → same `0x0B`, same time |
| boot image **container** form | `rm55` | BLB.BIN → `0x1C` at 150 ms |
| the ACR writes a meta copy / copies the ELF anywhere in FB | `rm59b` (+ `rm43`) | `nz_head=0 meta=0 nz_all=0`; census `other=0 meta=0 gfw=0` |
| FB-side FRTS object (the COT's own ask) | `rm47` | 74,160 dwords placed, PBUS latched FB media, `0x0B` 1 s later |
| every `frtsVidmemOffset` value | `rm44`/`rm45` | 64 M/256 M/512 M/1 G/2 G all fence-or-poison; 176 MiB legal → same nothing |
| the FSP places the FB-side FRTS itself | `rm44`/`rm41` | PBUS stays `0x0C63F000 / 0x2 / 0x10100` in every boot |
| vendor-computed **FW heap** (`_kgspCalculateFwHeapSize`) | `fh58` | WPR2 +7.000 MiB, `m0` still `0x0B`, band empty |
| a 6xx FMC names the `0x0B` step | `fh620` | `0x824` is an earlier abort; no live WPR2, `0x0B` absent |
| "`0x0B` is an instrument effect of polling" | `rm41`/`rm43`/`rm46` | 1 Hz register-only 27→28 s; no host activity <3 s; <200 ms |
| a second COT in the running state is a fresh attempt | `rm42` | every step `cot_error=0`, `m0` stays `0x0B` |
| `0x5` is a descriptor refusal | `t_a1c5bdd8` | `0x1C` is the refusal set; `0x5` is "descriptor read, later gate" |
| `0x14DC` can be read from a 570-generation FMC | `fh58`, all eight `t_0936a17f` boots | `reg=0x14dc=0x0` in every `fmcerr` row |
| GB202 has a host-loadable SEC2 / a vendor `booter_load` | `t_67f09feb` (AD102-independent finding, GB202) | `0x840000` = `0xBADF4100` in 7/7 fields; `0xAD00BC` = `0xFF` SUCCESS; no `gb202/sec2` in linux-firmware, no GB100/GB202 `BooterLoadUcode` in the vendor bindata |

Not in this table, because it is *not* ruled out: **the meaning of `0x0B`**. It
is not in `flcnretval.h` (which defines nothing below `0x10`), it is not an FSP
`kern_fsp_retval.h` code (those start at `0x9E`), and the FMC that produces it is
encrypted (8.00 bits/byte), so the instruction is not available to us. The park is
honest precisely because it says "we cannot name this step from the host side",
not "we know it".

---

## 6. The honest ceiling — plan §2.11, option 2

This is the landing the board already named on `t_67f09feb` ("plan option 2/3")
and the one this card is the fallback for. Stated plainly, with what is measured
and what is not.

### 6.1 What the native driver claims

| level | claim | status |
|---|---|---|
| **L1** | HobbyOS owns the card, brings up the register window, identifies the part, verifies NVIDIA's firmware byte-for-byte, and builds every structure GSP-RM consumes (WPR metadata, radix3, WPR2 layout, COT, publish block) | **done, measured on both parts** (AD102 and GB202) |
| **L2 (measured part, GB202 only)** | the part's own security processor runs NVIDIA's signed boot firmware: the FSP reports `SUCCESS` at its published gate, authenticates our COT (`errorCode=0`) and runs NVIDIA's FMC | **measured on GB202** |
| **L2 (the rest)** | our code runs on SEC2; the booter runs; GSP-RM reports `INIT_DONE`; a status queue is published | **not met on either part** |
| **L3–L5** | a submit reaches the 3-D engine and reads back; nvk enumerates; a game renders | **not started on either part** |

Read the L2 row exactly as written. The card's shorthand is "SEC2 runs the vendor
booter on GB202, FSP executes vendor firmware"; the measurement is narrower than
that sentence and worth keeping narrow:

* On **GB202**, the FSP is the security processor's firmware. Its published gate
  `NV_THERM_I2CS_SCRATCH.FSP_BOOT_COMPLETE` at `0x00AD00BC` reads `0xFF` =
  SUCCESS at the first sample (+2 ms), proved live by a write sentinel that
  landed and was restored (`ATT/t_67f09feb/5090-gb202sp-gate-REPORT.md`).
  *Source, not measured here:* that NVIDIA's `kern_fsp_gb202.c` has the BootFSM
  start the FSP from chip reset. `t_67f09feb` records the matching caveat — one
  boot cannot exclude a stale `0xFF`, and the reset-boundary test belongs to the
  reset/FLR card `t_72dc36b2`. So "the FSP is running and reports success" is
  measured; "it was started *by this boot*" is vendor source plus an untested
  reset boundary.
* There is **no host-loadable SEC2 on GB202** and no GB202 `booter_load`:
  `0x840000` answers `0xBADF4100` in 7/7 fields, the AD102 gate address
  `0x840094` is dead there, and neither linux-firmware (`nvidia/gb202/gsp/{bootloader,fmc,gsp}`
  only) nor the vendor bindata (`BooterLoadUcode` exists only for
  TU102/TU116/GA100/GA102/AD102) ships one. So **no HobbyOS-authored code has
  ever executed on GB202's security processor** — the vendor's firmware does, and
  our contribution is that we built and signed the frame it accepted.
* On **AD102 (4090, VM 300)** the `[AD102]` side of the same wall: three distinct
  AD102 vendor booters all return `0x002B0009` at +4.2 ms with `EXIT 0x10`, while
  a GA102 image returns `0x00250009` — the gate discriminates by part, and the
  rejection is authorisation, not a malformed container (`t_67f09feb`, quoted in
  its body; those are AD102 numbers and stay AD102).

So the honest one-line claim is: **"HobbyOS verifies and feeds NVIDIA's firmware,
and on GB202 NVIDIA's own FSP boots and authenticates it."** Not "we boot the
GPU".

### 6.2 Every games/perf data point stays on the fallback

No number below was produced by the `nv` driver. Each carries its renderer label
and the part it actually ran on.

| title | number | renderer | what it ran on | source |
|---|---|---|---|---|
| RimWorld (main menu, 720p) | **51.02 fps median** (7,600 presents / 149,848 guest ms) | `renderer=virgl` (host **llvmpipe**) | r1, VM 311 `hobbyos-games`, `virtio-vga`, **no passthrough** — the 4090 stays on `vfio-pci` | `docs/nv-plan.md` §2.5/N1 |
| Cyberpunk 2077 (game's own benchmark, 1280×720, Custom/Low, scaling **off**) | **29.19 fps avg / 29.66 median / 18.18 1 %-low** (1,874 frame rows in `frames.csv`, 1,873 after the warm-up frame 0; 64.20 s) | `renderer=venus` (virtio-gpu-gl-pci, `venus=true`) | the AMD host: `summary.json` reports `gpuName "Virtio-GPU Venus (AMD Radeon RX 9070 XT (RADV GFX1…#"`, `cpuName "AMD RYZEN AI MAX+ 395"`; qemu cmdline `-display egl-headless,rendernode=/dev/dri/renderD129`, `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json` — **not** either NVIDIA part | `docs/cp-bench.md`; `docs/cp-bench-evidence/hobbyos-venus-720p/{summary.json,frames.csv,qemu-cmdline.txt}` |

The Cyberpunk digits are re-derivable from the run's own frames, and were checked
for this document: `summary.json` `averageFps = 29.189037322998048`;
`frames.csv` gives median frame time 33.72 ms → `1000/33.72 = 29.656` fps and p99
55.0008 ms → `1000/55.0008 = 18.182` fps (the `fps.py` definitions in
`docs/cp-bench.md`; that doc rounds them to 29.7 / 18.2). No new run was made for
this card.

### 6.3 What the fallback costs

* **No native L3+ on any part currently in the rack that this lane has measured**
  — AD102 (`10de:2684`, r1 `01:00.0`, VM 300) and GB202 (`41:00.0`, VM 313).
  Neither has ever had a kernel submit reach the 3-D engine, so neither has a
  native frame, a native `vkcube`, or a native game number.
* **The `nv` driver's engine path is unproven end to end.** Submit → engine class
  → fence → read-back has never run against real hardware; the `/dev/nv*` shim,
  the DRM ABI and the 129-page RPC transport are tested against mocks and
  synthetic queues only. `t_9bf9bc0c` (first hardware round trip) stays blocked
  for the same reason it was blocked before: no GSP-RM peer.
* **The games story is the platform's, not the driver's.** Both titles' numbers
  come from Wine/vkd3d-proton + Venus or virgl on a virtio GPU. That is a real
  achievement and a real demo, but it is not evidence about `nv`, and it must
  keep the `renderer=venus` / `renderer=virgl` label wherever it is quoted.
* **The two stacks remain mutually exclusive on one card.** Venus needs the GPU on
  the host; `nv` needs it in the guest, and `r1` has one of each part. A GB202
  native path and a GB202 Venus path cannot be demonstrated in the same boot.
* **What survives is L1 plus the vendor-firmware part of L2.** That is not
  nothing: part identification, BAR windows, 38 MB/63.5 MB firmware read and
  digest, WPR layout, COT construction and authentication, the publish block and
  the RPC transport are all measured work, and they are the parts a future
  generation or a future firmware generation would reuse. They do not draw a
  frame today.

---

## 7. Proposal for `docs/nv-plan.md` §2.12

**This is a proposal only.** `docs/nv-plan.md` is coordinator-owned and was **not**
edited. The copy in this tree (`docs/nv-plan.md`, 30,726 B) ends at §2.6; the
§2.11/§2.12 numbering is the coordinator's newer draft, as cited by cards
`t_67f09feb` and this one. Paste, renumber, or discard — but do not read the
proposal as landed text.

> **§2.12 GB202 ACR: parked, and what that means for the driver's claim.**
> The GB202 (`41:00.0`, VM 313) ACR wall is **parked** after 19 boots on the three
> cards named here (`t_0936a17f` 8, `t_a769beaa` 9, `t_7178dca2` 2), on top of
> `t_a1c5bdd8`'s 21 (rm10…rm35). The ladder
> is measured and stable: descriptor unusable → `0x1C`; descriptor accepted,
> FRTS pair absent → `0x5`; running → `0x0`; signature zeroed → `0x1D`;
> radix3 ELF unreachable → `0x02`; boot image in the wrong container → `0x1C`;
> everything accepted → `0x0B` in 0.2–55 s **with the PFB WPR2 pair torn down in
> the same row** (`wpr2_lo=0x7FFFFE00 wpr2_hi=0`). Every host-supplied value has
> been varied with a row: `gspRmDescTarget`/args `+56=2` (`rm50`), `fbSize`
> (`rm51`), descriptor `frtsSize` (`rm57`), the FB-side FRTS object handed to the
> ACR inside its window (`rm47`), the whole WPR2 band at a 256-byte stride
> (`rm59b`: `nz_head=0 meta=0 nz_all=0`), and the vendor's own heap pair
> (`fh58`: WPR2 +7.000 MiB exactly, `m0` still `0x0B`). A 620-generation FMC is
> accepted (`cot errorCode=0`) but aborts earlier with `0x14DC=0x12000824`
> (`0x824`, variant 1, partition 1), so it names a 620-on-570-inputs abort, not
> `0x0B`. The meaning of `0x0B` is not obtainable from the host: the FMC is
> encrypted and no published table defines it. **Do not re-spend boots on:** the
> heap pair, `fbSize`, `frtsSize`, `gspRmDescTarget`, the FRTS object or any
> `frtsVidmemOffset`, the signature/ELF/boot-image identity, the WPR2 band
> census, a "6xx FMC will name `0x0B`" reading, or a second COT as a sweep.
> **Consequence for the claim:** the native driver stands at **L1 plus the
> measured part of L2** — on GB202 NVIDIA's FSP (gate `0xFF` SUCCESS, reset
> boundary not yet isolated) authenticates and runs NVIDIA's FMC; on AD102 SEC2 rejects
> every vendor booter with `0x002B0009`. No HobbyOS-authored code runs on either
> security processor, and no native L3+ exists on any part the rack exposes to
> this lane. Every
> games number in this plan is the virtio/venus fallback and stays labelled:
> RimWorld 51.02 fps median `renderer=virgl` (host llvmpipe, no passthrough);
> Cyberpunk 29.19 avg / 29.66 median / 18.18 1 %-low `renderer=venus` (AMD
> `RX 9070 XT` host). The `nv` engine path is unproven; `t_9bf9bc0c` and
> `t_938ced62` stay blocked until a GSP-RM peer exists. Record of every row:
> `docs/gb202-rm-parked.md`.

---

## 8. Where everything is

| what | path |
|---|---|
| this document | `docs/gb202-rm-parked.md` (this tree: `WS/t_251d3c71/tree/docs/`) |
| heap report + logs | `ATT/t_7178dca2/gb202-fwheap.md`, `…/5090-gb202fsp-fh58.log`, `…/5090-gb202fsp-fh620.log`, `…/gb202-fwheap-t_7178dca2.tar.gz` |
| RM-objects report + logs | `ATT/t_a769beaa/REPORT-gb202-rm-objects.md`, `…/KEY-ROWS-gb202-rm-objects.txt`, `…/5090-gb202fsp-rm{50,51,53,54,55,56,57,59,59b}.log`, `…/gb202-rm-objects-t_a769beaa.tar.gz` |
| prologue report + logs | `ATT/t_0936a17f/REPORT-gb202-acr-prologue.md`, `…/5090-gb202fsp-rm{41,43,47}.log`, `…/gb202-acr-prologue-t_0936a17f.tar.gz` |
| FMC/descriptor report + logs | `ATT/t_a1c5bdd8/REPORT-gb202-fmc.md`, `…/5090-gb202fsp-rm{10,31,34,36}.log` |
| SEC2/FSP gate report + log | `ATT/t_67f09feb/5090-gb202sp-gate-REPORT.md`, `…/5090-gb202sp-gate-20261125T0548Z.log` |
| workspaces (images, harness state) | `WS/t_7178dca2/`, `WS/t_a769beaa/`, `WS/t_0936a17f/`, `WS/t_9bf9bc0c/` |
| games evidence | `docs/cp-bench.md`; `docs/cp-bench-evidence/hobbyos-venus-720p/{summary.json,frames.csv,qemu-cmdline.txt}`; `docs/nv-plan.md` §2.5/N1 |

Nothing was deleted, no branch or workspace was touched, no VM was started, and no
`docs/nv-plan.md` byte was changed by this card. Live hardware state was not
queried for this document and this card booted nothing; the closing state recorded
by the parent, `t_7178dca2`, is `313 stopped / 300 stopped / pgrep qemu = 0`.
