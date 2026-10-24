**THE COUNTER IS 96% FALSE POSITIVES AND THE REAL GLUE IS MOSTLY NOT A KERNEL BUG — measured on both trees. This changes what the card should ask for.**

I replicated `t4_glued.sh`'s two predicates and then checked whether each hit actually carries **two record tags**, requiring a real tag to look like a record start (`[A-Z][A-Z0-9_]{2,}` not preceded by an identifier character, so `tf->regs[0]` is a subscript and not a tag):

| log | counter says | subscript-only (false positive) | **real glue** |
|---|---|---|---|
| `obj/t3_arm.log` — `main`, pre-branch | 540 | 504 | **36** |
| `obj/t3_arm_fixed.log` — `ticket/082-wholeline` | 470 | 453 | **17** |

**96% of the count is `[DEBUG] Before eret, tf->regs[0] = …` and `[KERNEL] sys_fork: tf->regs[0] is now 7`** — one record tag plus a C array subscript, matched by the counter's P1 regex `\[…\].*\[…\]`. GB-092's 379/417 is the same finding; the true rate on the ARM tree is **453/470**.

**So the branch is better than the card's numbers suggest: real glue 36 → 17.** It halves the defect. And **17 is not 0**, so the DoD is still unmet — but for a reason nobody has written down yet, and it is not the one the card assumes:

**The remaining glue is what a line looks like when an application builds it from several `write()` calls.** The sample is unambiguous:

```
[HELLO] argc=[KERNEL] Process 35: CAT.BIN exited unexpectedly or gracefully.
[LFN][TEST] Sending 'cat SHTEST.TXT' command...
[LKSTEST] PASS [FPU_T] floating-point bring-up test
```

`[HELLO] argc=` is a prefix the program printed **before** the value and the newline — `print("[HELLO] argc=")` then `print_int(argc)` then a newline is three writes. `print()` is atomic per call (`libc.c:225`, one `write(1, …)` per call), and the kernel's exit notice is built whole and emitted under the same lock. **Neither side is broken: atomicity is per write(), and the program chose to emit one logical line as several writes.** No kernel lock can fix that, and the kernel is doing the right thing.

**Which means the DoD as written cannot be reached by kernel work.** Two honest options, and the choice is a design decision for this card:

1. **Line-buffer user output.** Accumulate in `print`/`printf` until `'\n'` and emit each line as one write. Then "0 glued" becomes achievable *and* meaningful — at the cost of a user-libc change with its own hazards (unflushed output on exit, prompts without newlines like `sh`'s `$ ` stalling until flush). If this is chosen, `exit()` must flush, and interactive prompts must be flushed explicitly.
2. **Re-derive the DoD to what the kernel can promise**: *no glue within a single write; a logical line split by the application across writes is not the console's defect.* Then the metric is "0 real glue attributable to a single write", and the current 17 are reclassified as app-side line composition — with a named follow-up list of the programs that do it (`HELLO`, `LFN`, `LKSTEST`, `GREPTEST` all appear).

**My recommendation: (2) now, (1) as its own card.** (2) is measurable today, it makes the counter's number meaningful, and it stops a kernel ticket from owning a user-application behaviour. (1) is a real improvement but it changes every program's output timing, and that should be graded on its own card with its own evidence — not smuggled into a console-atomicity fix.

**What this card must do first, either way: fix the counter.** Every number quoted about this defect — mine included, twice — has been ~96% noise. Until `t4_glued.sh` refuses to call `regs[0]` a glued line, no reading on this card or on GB-057 is a measurement.

Classifier: `evidence/gb-082/glued_true.py` (committed with this) — takes a log, prints counter/false-positive/real, and shows the real ones so the claim is checkable.
