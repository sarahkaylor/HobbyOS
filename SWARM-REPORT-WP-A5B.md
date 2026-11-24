# WP-A5B — SEC2's START→STOPPED is microseconds, and it is not a halt at all

Branch `nv/wp-a5b-halttime`, cut from `eaf006a9` (`nv/integ-swarm`) — it also
carries `2120a60e` (another lane's `docs/nv-booter-appcode.md`, committed into
this checkout while the tree was on this branch; see §8). Four boots, one named
log each (`evidence/nv/r6a..r6d.log`), one attempt per signature slot per boot =
**8 measurements**. Card lane owned r1 for the round; VM 300 and VM 312 are
stopped and no qemu is left behind (section 7).

**The answer in one line:** `ctl` (`0x840100`) carries STOPPED **before** the
START write and carried it in **every one of ~335,000 samples** taken at ~590 ns
intervals for 200 ms after it — `running_samples=0`, `transitions=0`, 8 for 8.
The trigger never took the core out of STOPPED, so the interval from START to
STOPPED is the latency of the first sample: **2.272–2.336 µs**. That is the
card's microsecond branch, in its strongest form: not "the ROM rejected the
image", but "the core was never seen to run at all".

---

## 1. The question, and why the PC channel could not answer it

Round 5's last deploy (r5h) sampled PC (`0x840FF0`) before, during and after
START and got `0xBADF5040` every time — the constant `nv_flcn.h` documents as
"looks like the fence word and is not it". It could not separate "the boot ROM
rejected the image" from "START never reached the core", which is the whole
question, because both leave no write behind.

The replacement is halt timing, which round 3 already used to judge a launch on
the GSP window: the core leaves STOPPED while it runs and sets it again when it
halts, so the interval START→STOPPED says which of the two worlds you are in.
27.6 ms was a rejected FRTS command, 105.8 ms an accepted one, on the same part.

## 2. The instrument (`sec2_attempt`, `src/kernel/nv_gsp.c`)

Four properties, each of which removes one way the number could lie:

| property | why |
|---|---|
| `ctl` is read **before** the START write (`pre=`) | "already stopped" cannot be reported as "stopped after the start" |
| the cost of one `ctl` read through this window is measured first (64 timed reads, `read_cost_ns=`) | a halt time at or below the floor is reported as a **bound**, not a measurement |
| sampling continues for the **whole 200 ms budget** even after STOPPED is seen | a core still running at 1 ms and halting at 27 ms cannot be reported as stopped at 1 µs |
| every non-STOPPED sample is counted (`running_samples=`), and every change of the word is timestamped (`transitions=`, `ev+<ns>:<value>`) | "never left STOPPED" and "left and came back inside one sample" are different verdicts, and only one of them is a measurement |

The budget is 200 ms because FWSEC's own accepted halt takes 105.8 ms; a ROM
that runs and rejects late must still be inside it.

## 3. The measurement

`at_ns` / `stop_ns` are ptimer nanoseconds after the START write; `readns` is the
measured cost of one `ctl` read; `polls` is the number of samples taken across
the 200 ms budget (spacing = 200 ms / polls).

| boot | att | readns | pre | first | at_ns | stop_ns | polls | spacing | running | trans | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|
| r6a | A | 670 | 0x10 | 0x10 | **2336** | 2336 | 335,234 | 596 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6a | B | 589 | 0x10 | 0x10 | **2304** | 2304 | 334,978 | 597 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6b | A | 595 | 0x10 | 0x10 | **2304** | 2304 | 334,594 | 597 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6b | B | 590 | 0x10 | 0x10 | **2272** | 2272 | 335,362 | 596 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6c | A | 589 | 0x10 | 0x10 | **2304** | 2304 | 335,426 | 596 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6c | B | 589 | 0x10 | 0x10 | **2304** | 2304 | 335,298 | 596 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6d | A | 582 | 0x10 | 0x10 | **2272** | 2272 | 339,266 | 589 ns | 0 | 0 | NEVER-LEFT-STOPPED |
| r6d | B | 582 | 0x10 | 0x10 | **2304** | 2304 | 338,882 | 590 ns | 0 | 0 | NEVER-LEFT-STOPPED |

**Distribution: min 2272 ns, median 2304 ns, max 2336 ns, spread 64 ns (8/8),**
all of it inside the first-sample latency (a write, a read and a three-read
ptimer read), so the spread is the instrument's quantisation, not the card's.
The honest statement of the interval is therefore a **bound**: START → first
observation of STOPPED ≤ 2.34 µs, and the bit never moved again in 200 ms.

`transitions=0` is the stronger fact than the number: a halt that occurs is a
0x0 → 0x10 transition with a timestamp, and there is none. The value 0x10 was
already there when the code looked at `ctl` right after clearing the RISC-V
core select — before FALCON_RM, before the DMA load, before the BROM parameters,
before the mailboxes — and the re-halt before attempt B reads it in its first
poll too (`[NV-SEC2] re-halt before B: ctl=0x10`).

## 4. The control, in the same boot, with the same instrument

A window that never answers can look exactly like a core that never runs. So the
same read pattern is applied to the GSP falcon in every one of these boots, by
the round-3 sweep, and it answers:

| boot | GSP `ctl` at 1 µs after START | GSP `ctl` 0x0 → 0x10 | FWSEC halt (`[NV-BAR1] halt_ns`) | FRTS receipt |
|---|---|---|---|---|
| r6a | **0x0** (running) | +105,837,888 ns | +105,841,888 ns | ACCESS-COUNTER (carved) |
| r6b | **0x0** | +105,859,360 ns | +105,863,392 ns | ACCESS-COUNTER |
| r6c | **0x0** | +105,818,720 ns | +105,822,688 ns | ACCESS-COUNTER |
| r6d | **0x0** | +105,816,928 ns | +105,820,896 ns | ACCESS-COUNTER |

`pre=0x10`, `running=0x0` within 1 µs, `0x10` again at 105.8 ms: the falcon's
control word is observable, it moves when the core runs, and the margin between
"never left STOPPED" and "ran for 105 ms" is three orders of magnitude wider
than this window's 590 ns sampling floor. SEC2's `pre=0x10 / first=0x10 /
running_samples=0` in the same boot is therefore a fact about SEC2, not about
the instrument.

The premise the round hangs on also reproduced in all four boots: `frts-post`
reads `0xBAD0AC73+` (poison, +1 per read) while the pages below and above stay
MEMORY and writable, so FWSEC ran first every time.

## 5. What this establishes, and what it kills

**MEASURED — the µs branch, in its strongest form**

- The STOPPED bit was set before the START write in 8/8 attempts across 4
  boots, and did not change in ~2.68 million samples in total (335k per attempt,
  ~590 ns apart) spanning 200 ms per attempt.
- No sample in any attempt showed the core out of STOPPED.
- The write path is not the doubt: `MAILBOX0` reads back the argument the driver
  wrote (`0x094E8000`), and the BROM words read back as written, so writes to
  this window land — the START write landed and did not move the core.
- Nothing was written anywhere: metadata `verified` still `0x5EC2A0A0`, the
  bootbin region `0x2000/0x2000` words still the host's prefill, DMEM
  `0/0x1800` words changed, the mailbox still holding its own argument.

**REFUTED — "SEC2 executed for the first time"** (plan §2.5, report WP-A5 line 8)

`halted=1` in r5c was read from a `ctl` that already said 0x10 *before the
image was loaded*. Round 5's own logs contain the refutation: r5b's row "`ctl`
became `0x10`" is printed by the core-select clear, which runs before the load.
"Loaded" stands (DMEM 8/8 LANDED, IMEM secure-marker, BROM words echo); "and
executed" does not. The correct sentence for round 5 is: **SEC2 loaded the
booter and the START trigger never took its core out of STOPPED.**

**NOT established (no claim)**

- Whether the boot ROM runs at all, or would accept this image: it never got the
  chance. Nothing here says the image is good or bad.
- Whether the core ran for less than one sample (≤ 590 ns). If it did, it did no
  work: DMEM, the mailbox and the metadata are all untouched.
- Why the trigger does not take. `ctl` bit 1 is a write-only trigger; a refused
  or unheard START and an accepted-then-instantly-halted one both leave 0x10.
- Whether an earlier write of our own is what refuses the trigger. Every START in
  this project has been preceded by the halt-gate reset (`UC_CTRL` bits 0/2/3) on
  a window that was still fenced, and SEC2 has never had the `0x048`/`0x014`/
  `0x3C0` reset the GSP path measured as required (section 6). The jump-to-µs
  branch is a measurement of the outcome, not of the cause.

## 6. Where this points: the reset SEC2 never got

The plan's own branch for the µs answer: *"the ROM is not running at all; the
lever is the boot state (secure-boot enable / BROM mode words around `0x840000`),
not the image."* The timing rules out the whole image-and-signature space at
once — signature slot, ucode id, engine mask, entry point, `modsel`: none of them
can matter to a core that never leaves STOPPED. The file-encoding question WP-A5
§6 left open is also already closed on this branch: `docs/nv-booter-appcode.md`
(dropped here by another lane, §8) measures the `0x7600` bytes as AES-128-ECB
ciphertext of the fused ucode key, so loading them verbatim is *correct* and the
ROM is supposed to decrypt them.

The cheapest lever is not the boot state either, because this tree already
contains a measured statement about what makes a falcon accept STARTCPU — on
the GSP:

> `nv_gsp_fwsec_reset`: *"The reset is REQUIRED for STARTCPU to be accepted --
> measured: without it the core answers START with 'still halted'."*

That sentence describes SEC2's behaviour exactly, and SEC2 has never had that
reset. The GSP sequence writes **`0x048 &= ~3`**, **`0x014 = 0xffffffff`**,
pulses **`0x3C0 = 1…0`**, and waits ~20 ms for the scrub. SEC2 gets only the
`UC_CTRL` reset bits (`0x840100` = `RESET_TLB|RESET_UNK2|RESET_UNK3`) plus the
`0x8403C0` pulse inside `sec2_unfence` — where it is issued *before* the core
select is cleared, i.e. while the window still answers the fence word, and
`0x840048` / `0x840014` are never written at all.

So the single highest-value next deploy is one variable, three writes: repeat the
GSP's full reset **on SEC2, after `0x841668` is cleared** (so the window answers
and the pulse is observable rather than done blind), then load and START. The
halt-timing row already in place says in the same boot whether the trigger took.

Two orderings are worth one boot each after that:

1. **Double START, 1 ms apart** — a trigger that is consumed and one that is
   never seen both leave `0x10`, but a second trigger written while the first is
   still pending may answer differently.
2. **The select word after a refused START** — `0x841668` reads `0x1` (VALID,
   falcon selected) at the end of every attempt here; re-reading it immediately
   after a START that did not take says whether the select reverts, which is what
   "the trigger reached the wrong core" would look like.


## 7. Gates

| gate | result |
|---|---|
| build, intel `MODE=test` | `exit=0`, 727 compiles, no new warning from `nv_gsp.c` (`clang -O2 -Wall -Wextra`, `KERNEL_MODE_TEST`) |
| ARM per-TU | `obj/arm/nv_gsp.o` compiles (`aarch64-none-elf`, exit 0, no warning): the new block is inside the file's x86-only region |
| unit tier | **589 tests, 0 failed** (`UNIT TESTS PASSED`, `run_unit_tests_intel.sh`, TCG — `/dev/kvm` is present but not readable by this user, so `SKIP_KVM=1`; KVM mode aborts at `failed to initialize kvm: Permission denied` and says nothing about the tree) |
| image | `sha256=d55acc67cbf14f3ab11645ef933d834f39d072d3aabeb02e7b6e1f12ba9aa7ce`, 1 GiB, firmware staged (`BOOTER/LOADER/GSPRM/UNLOAD.BIN`) once; the same local file re-uploaded before each boot and never touched in between, so all four boots start from identical bytes. The file in the worktree has since been replaced by the unit tier's own `MODE=unit_tests` image (that is what `run_unit_tests_intel.sh` builds); the four logs are the evidence, and `boot-n.sh` reproduces the image from this commit |
| distinct boots | four different ptimer origins: `0x19BFDF960`, `0x199976C20`, `0x19811D4A0`, `0x19C96B400` |
| card left clean | VM 300 **stopped** and VM 312 **stopped** (`qm status`), `/var/run/qemu-server/300.pid` **absent**, `ps -eo comm \| grep -c '^qemu-system'` = **0** on r1 |

## 8. Runs that answered nothing (said out loud)

- **Two boots of a torn image.** The first harness attempt uploaded `disk.img`
  while a *different* lane's build was rewriting it in the shared checkout
  (`/home/sarah/Documents/GitHub/HobbyOS`); the image arrived without
  `/NVIDIA/GSP/BOOTER.BIN` and the boots produced no `[NV-SEC2]` rows at all
  (`fw BOOTER.BIN load FAILED rc=-2`). Both were discarded, and the fixture is
  now "stage the firmware, then *prove* `BOOTER.BIN` is in the image before
  spending a boot" (`tools/nv/boot-n.sh`). This round's four logs are from a
  private worktree (`HobbyOS-a5b`) built and booted out of that contention.
- **A commit that is not this lane's.** `2120a60e` (`docs/nv-booter-appcode.md`,
  the AES-ECB analysis) is the parent of this lane's first commit: it was made in
  the shared checkout at 07:29 while that checkout was on this branch, so it
  landed here rather than on its author's branch. It is kept, not dropped — it is
  real work and this branch is the only place it exists. A merge of
  `nv/wp-a5b-halttime` therefore carries three commits, two of them this lane's.
  **hotspot: `/home/sarah/Documents/GitHub/HobbyOS` (the shared checkout and its
  `disk.img`)** — two lanes building in the same tree at the same time is what
  produced the torn upload and the stray commit; this round moved to a private
  worktree (`HobbyOS-a5b`) after the first two boots were lost to it.
- **`tools/nv/deploy.sh` cannot be used per boot here.** It waits up to 450 s
  for the guest to self-terminate; this kernel does not halt QEMU in `MODE=test`,
  so four boots spend thirty minutes in `qm status` polling. `boot-n.sh` does the
  same steps (firmware, one upload, boot, poll for the stage-5 row, pull, stop)
  and costs one boot each.

## 9. Next step

1. The three start-path observations of section 6, cheapest first: the
   double-START control (no writes beyond the trigger), then `0x841668` after a
   refused start, then the SEC2 engine's clock/power gate.
2. Keep the halt-timing row in every future SEC2 attempt: it is one row, it
   costs 200 ms, and it is the only channel that distinguishes the two worlds
   the PC could not.
