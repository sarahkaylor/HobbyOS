# GB-070-V (round 2) — gate evidence (VERDICT: RED, clause 1 FAIL / clause 2 PASS / clause 3 PASS)

Ticket: GB-070-V (#176) — round 2: verify the W0 corpus gate names absent members and can fail
Graded: **PR #183**, branch `ticket/070-w0gate` @ `4eb225b` ("GB-070: wire the W0 corpus gate live;
emit output markers from CAT/LS/SH"), base `origin/main` @ `44a9875` (the sha this card names).
Method: the PR's files checked into a clean `44a9875` worktree, `make disk.img ARCH=intel`, a full intel
T3 wave, then the gate run over that wave's serial — the same log the wiring would hand it.
Verifier: DEEP farm session (DeepSeek v4.1 Flash, dgx1, swarm-farm-2). I authored neither GB-070 nor #183.
Run 2026-10-25 ~18:00–18:25Z. Not graded on `mergeable_state`.

## Clause table

| # | clause | result |
|---|---|---|
| 1 | the gate names the **absent** members on a real intel wave, quoted | **FAIL** — 0 members absent and the gate still goes RED (a **false** FAIL) |
| 2 | it can fail — remove one member from the healthy corpus → RED | **PASS** — quoted below |
| 3 | no silent widening — diff the gate predicate against `44a9875` | **PASS** — predicate byte-identical; the change only adds an invocation |

## The wave

```
$ make disk.img ARCH=intel          BUILD_EXIT=0
$ T3_LOG=obj/t3_intel_gb070v.log bash tools/t3_tier.sh intel
T3_intel: 17662 console lines, 100 FAIL lines, 11 user memory fault records across 9 processes,
          84 programs exited, halted=1
T3_intel EXIT=1
```
Arrivals of the whole W0 corpus, counted in that log:
```
HELLO  [ARRIVED] HELLO.BIN -> 2      CAT  [ARRIVED] CAT.BIN -> 4
SH     [ARRIVED] SH.BIN    -> 8      LS   [ARRIVED] LS.BIN  -> 2
```
So the corpus and the wave **agree** — the count of named-but-absent members is **0**.

## Clause 1 FAIL — the gate reports RED on a complete corpus

```
$ bash tools/gate/t4_w0.sh --serial obj/t3_intel_gb070v.log
=== t4_w0: W0 corpus gate over obj/t3_intel_gb070v.log ===
  PASS HELLO (arrived in guest)
  PASS HELLO output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS SH (arrived in guest)
  PASS SH output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
  PASS CAT (arrived in guest)
  FAIL CAT — guest output marker 'entered-main:main-returned\n" code=0 ' != Linux-leg 'entered-main:main-returned' (writes expected output: NO)
  PASS LS (arrived in guest)
  PASS LS output — guest 'entered-main:main-returned' == Linux-leg 'entered-main:main-returned'
t4_w0: FAIL — W0 corpus incomplete or output mismatch
W0_GATE_EXIT=1
```

Every member arrived and every member's output is right; the one FAIL is the gate's own extraction.
It takes the **first** line anywhere in the log that contains `[W0] CAT linux=`:

```
log:224  D cat got=56  out="probe-content\n[W0] CAT linux=entered-main:main-returned\n" code=0     <- picked
log:483  [W0] CAT linux=entered-main:main-returned                                                 <- CAT's own marker
```
`log:224` is `src/user/t4_probe.c:67` echoing CAT's stdout inside its `out="…"` row — it **contains** the
marker, so `grep '\[W0\] CAT linux=' | head -1 | sed 's/.*linux=//'` yields
`entered-main:main-returned\n" code=0 ` instead of the marker's value.

Anchored extraction (`grep '^\[W0\] CAT linux='`) gives the right answer:
```
guest=entered-main:main-returned
ref  =entered-main:main-returned
equal=YES
```

**Why this fails the card rather than merely blemishing it:** #183's whole point is to *wire this gate
live* into `tools/t3_tier.sh`. Wired live on a healthy corpus it turns the intel tier RED — a gate that
cries wolf on a complete wave is as unusable as one that cannot fail, and it is the same class of defect
the card was minted to kill (a verdict that does not correspond to the measurement).

## Clause 2 PASS — it can fail, proven twice

```
$ bash tools/gate/t4_w0.sh --self-test
[self-test] good wave (all members arrived):        OK — all W0 members PASS (matched Linux leg)
[self-test] missing CAT (must FAIL):                OK — absent member FAILED as required
[self-test] broken HELLO output (must FAIL):        OK — broken member FAILED as required
self-test result: fails=0                           SELFTEST_EXIT=0
```
and on the real wave, with LS's arrival+marker lines removed (17 651 lines vs 17 662):
```
  FAIL LS — not in the intel wave's exited-programs/arrival set (W0 corpus member never ran)
t4_w0: FAIL — W0 corpus incomplete or output mismatch
STRIPPED_GATE_EXIT=1
```

## Clause 3 PASS — no silent widening

```
$ git diff 44a9875 4eb225b -- tools/gate/t4_w0.sh | wc -l
0
```
The predicate is byte-identical to the base. The only tier change is additive and strictly stricter:

```
+# GB-070: W0 corpus gate, live. ...
+if [ "$rc" -eq 0 ] && [ "x${W0_GATE:-1}" != "x0" ] && [ -f "$ROOT/tools/gate/t4_w0.sh" ]; then
+  (cd "$ROOT" && bash tools/gate/t4_w0.sh --serial "$LOG") || { echo "T3_${ARCH}: FAIL - W0 corpus gate"; rc=1; }
+fi
```

Two things about that wiring, both stated because they change what "live" means:
1. It only fires when `rc -eq 0`, i.e. on an otherwise-green tier. Today's intel wave has 100 FAIL lines
   (the GB-093/095/057 family), so on `main` the W0 gate never runs at all — clause 1's measurement above
   is me running the gate over the wave the wiring would have handed it, not the wiring firing.
2. Because of the false CAT FAIL, the first green intel wave to reach that line would be flipped red.

## Verdict

**RED.** Clause 2 and clause 3 are met; clause 1 is not, and it fails on the exact property the card
exists to establish — on a real intel wave the gate's verdict does not match the measurement. To reach
GREEN: anchor the marker extraction (`grep -a '^\[W0\] '`, or strip `t4_probe`'s `out="…"` wrapper before
matching), re-run the intel wave, and show `t4_w0: PASS` **and** the absent-LS case still failing. The
`[W0]` markers added to `cat.c`/`ls.c`/`sh.c` and the three `.ref` fixtures are sound — HELLO/SH/LS all
compare equal, and CAT compares equal once the extraction is anchored.
