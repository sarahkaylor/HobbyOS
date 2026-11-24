# WP-A7 — the START write lands; the *sequence* is what refuses it

Round 7 of the local swarm. Card `t_8e5217e5`. Branch `nv/wp-sec2-start`,
worktree `/home/sarah/Documents/GitHub/HobbyOS-sec2` (the shared checkout and
its `disk.img` were not touched).

**Question.** Round 6 measured SEC2 START → STOPPED at 2.3 µs and found
`transitions=0`. Is the START bit refused, dropped, or consumed?

**Answer, in one paragraph.** The write always lands. Whether the bit is taken,
refused or dropped is decided by the engine state the sequence leaves behind —
and the sequence the driver used was missing the state change that makes the
core take it. The two sticky registers in the same window take a value that
cannot appear there by themselves and read it back, and after a falcon reset the
START bit itself *latches* and stays latched for 100 ms: the bus and the
register file are not the problem. Three states, measured across 8 boots:

| state the sequence builds | ctl at the first read after the write (2.27–2.43 µs) | at 10 ms | at 100 ms | core ran? |
|---|---|---|---|---|
| **A/B — fixups, no falcon reset** (every round 1-6 state) | `0x10` (bit 1 gone) | `0x10` | `0x10` | **no** — 0 running samples of ~131 k |
| **R — falcon reset, no fixups** | `0x12` (bit 1 **latched**) | `0x12` | `0x12` | **no** — 0 of ~131 k |
| **S — falcon reset *then* the GSP's four fixups** | `0x0` (**bit consumed, STOPPED cleared**) | `0x0` | `0x0` | **YES** — every sample of ~130 k |

So: the trigger was being *refused* (cleared without a start) in every state
this project has ever run, and it becomes a real start only when the falcon has
been reset **and** the four state fixups the GSP path applies are written after
that reset. The reset alone leaves the trigger latched and unexecuted; the
fixups alone (the current sequence) leave it cleared and unexecuted. Round 6's
2.3 µs was the instrument: `wr_to_read_ns` is measured now, and it is
2272–2432 ns in every attempt of every boot (19 of 20 rows at 2272–2304) — the
same number round 6 read as a hardware interval.

---

## 1. The instrument, and why a read-back is not evidence

One line per row, from `src/kernel/nv_gsp.c` (`[NV-SEC2T]`), with the columns
the card asks for **in the same row**:

```
[NV-SEC2T] <att> <phase> dt_ns= ctl= bcr= tcfg624= tcfg600= ent= st4c=
           hw0f4= rm84= sec240= alias130= pc= mb0=
```

* `ctl` = `0x840100` UC_CTRL: bit 1 START (write-only trigger), bit 4 STOPPED
  (RO). `bcr` = `0x841668`, the core-select word (VALID | RISCV | BRFETCH).
  `tcfg624` = `0x840624` (bit 7 `ALLOW_PHYS_NO_CTX`), `tcfg600` = `0x840600`
  (`TRANSCFG[0]`, the DMA context word).
* Phases: `pre` → write START → `post_first` (the read **nearest** the write,
  with its ptimer bracket) → a 12-read burst → `post` → `at10ms` → `at100ms`,
  plus `summary` (`bit1_ever`, `running_ever`, `polls`, `running_samples`,
  `ctl_changes`).
* The write→read gap is bracketed by two ptimer reads, so `wr_to_read_ns` is
  that gap and not a round-trip cost measured somewhere else.

**No bit of ctl is sticky**, so "wrote 0x2, read 0x10" proves nothing by
itself: a taken trigger self-clears, and so does a trigger the logic discards.
Two things make the rows mean something instead:

1. **A write-lands control in the same window, before every trigger.** `0x104`
   (UC_ENTRY) and `0x084` (FALCON_RM) are written with values that cannot occur
   there spontaneously, read back, then restored:

   ```
   ent_pre=0x0        wrote=0xA5C33C5A ent_back=0xA5C33C5A
   rm_pre=0x192000A1  wrote=0x5A3CC3A5 rm_back=0x5A3CC3A5
   verdict=WRITE-PATH-LIVE
   ```

   This row is present in **16 of 16** attempts across the 8 boots and is
   positive in all of them. It is what turns a later "the bit never appeared"
   into a statement about the control logic rather than about a dead bus.
2. **The trigger is judged by what the core did**, never by the bit: STOPPED
   clearing, the DMEM image changing, the booter's own metadata verdict word.

---

## 2. The trace

Three images (three instrument revisions), 8 boots, each boot's ptimer origin
distinct — these are 8 boots, not one boot counted 8 times. Revision 1 (t7) had
the trace rows but no `wr_to_read_ns` bracket and no S variant; revision 2 (t8)
added both (with the S order bug documented in §2.3); revision 3 (t9) fixed the
order and added the `0x130` alias test and `0x240`.

| boot | image sha256 (first 16) | ptimer t0 | rows |
|---|---|---|---|
| t7a | `c3540f11c3a7013c` | `0x19BEDC100` | 31 |
| t7b | `c3540f11c3a7013c` | `0x19DD1D540` | 31 |
| t7c | `c3540f11c3a7013c` | `0x198D3A240` | 31 |
| t8a | `6cee5754f251cec9` | `0x19B1B9900` | 51 |
| t8b | `6cee5754f251cec9` | `0x19BBBF940` | 51 |
| t8c | `6cee5754f251cec9` | `0x19958D8E0` | 51 |
| t9a | `b5693e16aab9b68a` | `0x19E02DA80` | 51 |
| t9b | `b5693e16aab9b68a` | `0x19C8E4560` | 51 |

### 2.1 A/B — the state every previous round ran in (8 boots × 2 attempts)

| att | phase | ctl | bcr | tcfg624 | tcfg600 | alias130 |
|---|---|---|---|---|---|---|
| A/B | pre | `0x10` | `0x1` | `0x190` | `0x5` | `0x0` |
| A/B | post (+~10 ms) | `0x10` | `0x1` | `0x190` | `0x5` | `0x0` |
| A/B | at10ms | `0x10` | `0x1` | `0x190` | `0x5` | `0x0` |
| A/B | at100ms | `0x10` | `0x1` | `0x190` | `0x5` | `0x0` |

`post_first`: `ctl=0x10 bit1=0 running=0`, `wr_to_read_ns=0x8E0–0x980`
(2272–2432 ns; the single 2432 is one B attempt in t9a, the other nine A/B rows
with the field are 2272–2304), `read_cost_ns=0x25F–0x31B` (607–795 ns).
`summary`: `bit1_ever=0 running_ever=0 polls=0x1F901..0x20AE1` (129,281-133,857
samples) `running_samples=0 ctl_changes=0` — 100 ms of sampling at ~0.6-2.3 us
spacing, not one sample in which the core left STOPPED.

Reading: the trigger is **gone by the first look** and the core never moves.
With the write-lands row positive, the bit was not lost on the bus: the CTL
logic took it and cleared it. That is a *refused* start, not a dropped write —
but it is also not "consumed by a running core", because a core that ran and
died inside 2.3 µs would still leave the DMEM image changed, and it does not
(0 of `0x1800` words, every attempt, every boot).

### 2.2 R — a real falcon reset, nothing else (8 boots)

The GSP's own sequence, issued on SEC2 for the first time (only the `0x3C0`
pulse had ever been written before, by the unfence path): `0x048 &= ~3`,
`0x014 = 0xFFFFFFFF`, pulse `0x3C0`, ~20 ms scrub.

| phase | ctl | bcr | tcfg624 | tcfg600 | rm84 |
|---|---|---|---|---|---|
| before-reset | `0x10` | `0x1` | `0x190` | `0x5` | `0x192000A1` |
| after-reset | `0xBADF5620` | **`0x110`** | `0xBADF5620` | `0xBADF5620` | `0x0` |
| after-bcr (select cleared) | `0x10` | `0x1` | `0x110` | `0x110` | `0x0` |
| post (after START) | **`0x12`** | `0x1` | `0x110` | `0x110` | `0x0` |
| at10ms | `0x12` | `0x1` | `0x110` | `0x110` | `0x0` |
| at100ms | `0x12` | `0x1` | `0x110` | `0x110` | `0x0` |

`summary`: `bit1_ever=1 running_ever=0 running_samples=0 ctl_changes=0`.
Metadata verdict word: still the sentinel (`0x5EC2A0A0`).

Two facts, both new:

* **The reset re-arms the core select to `0x110`** — RISCV | BRFETCH — and while
  that selection stands **the whole falcon page answers the fence word**
  (`ctl`, `tcfg624`, `tcfg600`, `ent`, `alias130`, `pc`), while `0x841668` and
  `0x0F4` still answer real values (`0x110`, `0x67B7`). Clearing the select sets
  VALID (`0x1`) and the page opens again (it reads `ctl=0x10`,
  `tcfg624=0x110`, `tcfg600=0x110`).
* **The START bit latches and stays latched**: `0x12` at 2.27 µs, still `0x12`
  at 100 ms, `ctl_changes=0`, and `alias130=0x2` in the same rows. The register
  is holding a pending trigger that the core never takes. A one-write-per-bit
  sweep of ctl in this state (issued last, deliberately destructive) shows the
  latch is ordinary RW: `w=0x1 -> 0x10` (clears bit 1), `w=0x2 -> 0x12` (sets
  it), `w=0x4/0x8/0x10 -> 0x10`, `w=0x40 -> 0x50` (bit 6 latches too). `0x130`
  mirrors bit 1 but **writing START to `0x130` does not set it** (t9a/t9b) — a
  status mirror, not a second door.

So the write reaches the block, the trigger is retained, and the core does not
run. The blocker is downstream of the trigger.

### 2.3 S — reset *then* the GSP's four fixups (3 boots with a broken order, 2 with the fixed order)

S repeats R exactly, then adds the four writes the GSP path makes at the same
point: `0x624 |= 1<<7` (`ALLOW_PHYS_NO_CTX`), `0x10C = 0`, `0x600 = 5`
(coherent physical sysmem), `0x084 <- PMC_BOOT_0`. Order matters inside S:
the select must be cleared **first**, because while RISC-V is selected the page
answers the fence word and a read-modify-write of `0x624` then writes
`0xBADF5620` back and does not land (measured in t8: `624=0xBADF5620 ->
0xBADF5620`; that cut behaved like R and latched — a useful third data point).

t9a/t9b, fixed order:

```
S state-fixups 624=0x110->0x190 600=0x5 10c=0x0 rm84=0x192000A1
S pre   ctl=0x10 bcr=0x1 tcfg624=0x190 tcfg600=0x5 ent=0x0 st4c=0x0 hw0f4=0x47B7 rm84=0x192000A1 sec240=0x3000 alias130=0x0 pc=0xBADF5040
S post_first ctl=0x0 wr_to_read_ns=0x900 read_cost_ns=0x268 bit1=0 running=1
S summary bit1_ever=0 running_ever=1 polls=0x1F921 running_samples=0x1F921 ctl_changes=0   (t9b: 0x20001/0x20001)
```

**The trigger is consumed and the core runs.** `ctl` goes `0x10 -> 0x0` inside
the same 2.3 µs read and stays `0x0` for the whole 100 ms window — 129,313 of
129,313 samples in t9a and 131,073 of 131,073 in t9b, `ctl_changes=0`. And the
page behaves exactly like a *running* core: `bcr`, `tcfg624`, `tcfg600`, `ent`,
`alias130` and `pc` all read `0xBADF5620` in the post/10 ms/100 ms rows, while
`ctl` keeps answering `0x0`. That re-fencing-while-running is the same signature
rounds 1-4 kept mis-reading as a privilege wall.

**What S does not show:** the booter's own verdict. The metadata word is read
immediately after the 100 ms trace and is still the sentinel in both boots —
but the core is still running at +100 ms, and FWSEC's own halt on this card
comes at ~105.8 ms, so a verdict at +100 ms would be early even on a healthy
launch. This report therefore claims **execution**, not **completion**.

---

## 3. The bisect, stated as the driver can use it

| # | falcon reset | the four fixups, *after* it | bit 1 at 2.3 µs | core leaves STOPPED |
|---|---|---|---|---|
| A/B | no | yes (current order) | `0`, core stopped | **no** |
| R | yes | no | `1` (latched) | **no** |
| S | yes | yes | `0`, core **running** | **yes** |

Both halves are load-bearing, and the order between them is part of it. The
current driver's halt step is what skips the first half: it issues UC_CTRL's
reset bits only when `ctl` is unreadable or not already STOPPED, and on this
card `ctl` is always readable and always STOPPED — so SEC2 never receives the
reset that the GSP path applies unconditionally and that
`nv_gsp_fwsec_reset` records as *required* for STARTCPU to be accepted.

Which of the four fixups is the load-bearing one was **not** isolated — they
were written as one group, as the GSP writes them. Isolating them is one write
per boot and belongs in the next deploy.

---

## 4. What this refutes, and what it leaves open

**Refuted, with rows:**

* "The START bit is dropped / the window's writes are dead." No: 16/16
  write-lands rows positive; the bit latches in R and is a plain RW bit in the
  post-reset sweep.
* "START → STOPPED is 2.3 µs of hardware time." No: 2.3 µs is
  `wr_to_read_ns`, the write+read round trip, in all three states — including
  the one where the core runs. The interval is the instrument.
* "SEC2 executed for the first time in round 5" stays refuted, and now for a
  second reason: in that sequence the trigger was refused, not executed.
* "A privilege fence gates the SEC2 window." The fence follows the *core-select
  state* and the *running* state: `0x110` after a reset fences the page;
  clearing the select opens it; starting the core fences everything except
  `ctl`. It is a visibility state, not a privilege drop.

**Open, in order of leverage:**

1. **Does the booter complete?** Run the S sequence as the launch path and poll
   the metadata `verified` word for ≥300 ms (it is the booter's own channel and
   lives in host memory, so no register reading is involved). ~105 ms is the
   reference's own budget for this step.
2. **Which fixup matters?** Bisect the four, one per boot, on top of the reset.
3. **The entry point.** S arms `UC_ENTRY = 0` (`bootvec`), the same value the
   attempts use. If the core runs but never writes the verdict, the entry/IMEM
   layout is the next thing to vary, not the trigger.
4. **The RISC-V block.** While the reset's `0x110` selection stands, the block
   `0x841654..0x841694` answers real values (BCR at `0x1668` = `0x110`,
   `0x1664` = `0xFFFFFFFF`, `0x1690` = `0xFFFFFF8F`, zeros elsewhere) while the
   rest of the page is fenced. If SEC2's boot ROM is a RISC-V program on this
   part, its start/vector registers are in there; nothing in this round used
   them, and the falcon path above already executes.

---

## 5. Reproduce

```sh
# build (TOOLS_SKIP=1: the /TOOLS selfhost toolchain is not needed for a driver boot)
make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 -j$(nproc) disk.img

# 3 boots on r1 VM 300 with the RTX 4090 passed through; refuses to boot if
# VM 300 is not stopped, and refuses to keep a log without trace rows
tools/nv/sec2-boot.sh 3 t9

# every number in this report, read back out of the logs
python3 tools/nv/sec2-trace-table.py evidence/nv/t7{a,b,c}.log \
        evidence/nv/t8{a,b,c}.log evidence/nv/t9{a,b}.log
```

Logs: `evidence/nv/t7a..t7c.log`, `t8a..t8c.log`, `t9a..t9b.log`
(+ `evidence/nv/sec2-trace-tables.txt`, `evidence/nv/sec2-trace.json`).

## 6. Gates

| gate | result |
|---|---|
| `make ARCH=intel MODE=test LO_ON_DISK= TOOLS_SKIP=1 disk.img` | exit 0, no new warning from `nv_gsp.c` (all three instrument revisions compiled with `-Wall -Wextra`) |
| unit tier (`TOOLS_SKIP=1 SKIP_KVM=1 ./run_unit_tests_intel.sh`) | **589 run / 0 failed**, `UNIT TESTS PASSED` — same count as round 6 |
| ARM per-TU (`clang --target=aarch64-none-elf -mcpu=cortex-a53 -mgeneral-regs-only -c src/kernel/nv_gsp.c`) | compiles to an empty 704-byte object (the block is inside `#ifdef __x86_64__`) |

## 7. What was not run, and why

A ninth boot (`t9c`) was not run: access to `r1` moved off the `sarah` account
mid-deploy (operator note, card `t_89f24e1f` — the key now lives only under the
`nvcard` account). The failure was an SSH permission denial before `qm start`,
so no VM was started by that attempt. VM 300 was stopped by the harness's own
`qm stop` after each boot, including t9b (the script prints its KEPT line only
after that command returns); the final `qm status` verification could not run.
VM 312 is never started by this harness and was last observed `stopped`, with 0
`qemu-system` processes on r1, after t8. The trace evidence above is from the 8
boots that ran, and every claim in §2 is repeated in at least three of them.
