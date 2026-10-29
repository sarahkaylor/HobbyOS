**GB-129 — the bisect narrowed the window from "since 2026-10-26" to eight hours on 2026-10-23/24, and it refuted the card's own premise: the 26 Oct head is **BAD**, not good.**

Predicate fixed before the first run, unchanged throughout:

```
GOOD = halted=1 AND FATAL==0 AND programs>=55
BAD  = otherwise   (a wedge is its own column, not a retry)
```

Six intel T3 runs, one per step, this VM, `evidence/gb-129/`.

| sha | date (UTC) | console lines | FAIL | exited | FATAL | halted | verdict |
|---|---|---|---|---|---|---|---|
| `18bb542` | 2026-10-20 23:54 | 1 377 | 5 | 62 | **0** | **1** | **GOOD** |
| `d4e180a` | 2026-10-23 19:26 | 34 624 | 86 | 82 | **0** | **1** | **GOOD** |
| `8fccec7` | 2026-10-24 03:21 | 108 667 | 140 | 84 | **1** | 0 | **BAD** |
| `ef2dabc` | 2026-10-25 03:53 | ~10 978 (frozen) | — | — | 0 | 0 | **BAD** (wedge: log stopped growing for ~9 min, monitor sampled the CPUs, no `System halt`) |
| `6bc3235` | 2026-10-25 06:29 | 26 821 | 165 | 86 | **1** | 1 | **BAD** |
| `0633743` | 2026-10-26 23:48 | 46 623 | 240 | 87 | **1** | 0 | **BAD** |

## The two findings that matter

**1. The card's assumed good endpoint does not exist.** It proposed testing "whatever `main` was when GB-057 closed (2026-10-26)" and treating `215b79b`'s neighbours as the suspect set. `0633743` is that head, and it is **BAD by my own run**: 1 FATAL, `halted=0`, 46 623 lines, monitor sampling. So the regulator is not from tonight's three merges — it predates them by days.

**2. The narrowed interval is (d4e180a .. 8fccec7] — 62 commits, 2026-10-23 19:26 → 2026-10-24 03:21, about eight hours.** GOOD on the left, BAD on the right, and the false candidate in the middle (`ef2dabc`, "Revert GB-057 resume-page materialisation") is already BAD, so the GB-057 resume-page commits at 25 Oct 05:07/06:27 are **not** the first bad — my own hypothesis, refuted by a run rather than argued away.

**Bonus signal, worth handing to GB-125:** the fatal at `8fccec7` is

```
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=6 rip=0x0000000000000001 cr2=0x00000017FFFFFCB8 pid=…
```

`cr2=0x17FFFFFCB8` — the **stack top page** — and `vector=6` with `rip≈0x1`. That is the same address class GB-122 measured as `pte=0` (`0x17FFFFFxxx`), so the defect I chased tonight is at least five days older than the cards that filed it, and the "late tear" and "absent resume page" are plausibly one story: a jump through a value read from the page the resume copy failed to write.

## Why I am stopping short of the first bad commit

62 commits is ~6 more bisect steps at 6–20 minutes each with wedges. That is past the card's ~75-minute budget (I used ~55). Per clause 5 this is the narrowed interval plus the table, not a guess at a commit.

## Continuation (next card, ready to run)

Bisect **(d4e180a .. 8fccec7]**, midpoint `git log --reverse d4e180a..8fccec7 | sed -n '31p'`. The GOOD/BAD predicate and the table format above transfer unchanged; `18bb542` and `d4e180a` are already established GOOD and `8fccec7`, `ef2dabc`, `6bc3235`, `0633743` already BAD, so steps 1–6 need not be repeated. Expect wedges — record them as BAD-by-predicate with the wedge column, as done here for `ef2dabc`.

Evidence: `evidence/gb-129/` — `t3_<sha>.tier.txt` and `t3_<sha>.log` per run (six), each with the tier's own summary lines. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-29T11:45Z