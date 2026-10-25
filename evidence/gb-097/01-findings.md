# GB-097 — findings: the raw sink shreds records, measured today, and it is reachable from returning paths

## 1. A byte-interleaved record, verbatim (my own GB-096 intel run, `9f89a23`)

```
[KERNEL] FATAL: Exception in Kve2r nelloa Mdoerd:e!  Vimeagctoer : m14 aEprrpoerd C od(el: 0azx0y00)0, 00b00yt00e00s000=26
```

Two writers' bytes alternate run-for-run: one is
`[KERNEL] FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x0000000000000002`, the other is a
second fault report (its own `…Mode… Vector… Error Code… bytes=…`) being emitted through the raw sink
while the first CPU was still inside the locked path. Neither record survives; both are unreadable.

That run also contains 17 kernel-mode faults but **0 `[FREERACE]`/`[STALLBREAK]` records**, so it does
not satisfy the DoD's precondition ("a run that fires `[FREERACE]`/`[STALLBREAK]`"). It does prove the
defect class: the raw sink and the locked sink interleave byte-for-byte on a live system.

## 2. The raw sink is reachable from paths that RETURN (the DoD's second clause)

Callers of `uart_putc_raw` / `uart_puts_raw` / `uart_print_hex_raw` outside `uart.c`:

```
src/kernel/arch/x64/mmu.c:665   uart_puts_raw("[FREERACE] freeing root=")      <- inside v2 free, RETURNS
src/kernel/main.c:46,114,119    hex/decimal helpers                            <- return to their caller
src/kernel/process.c:125-145    "[KERNEL] resume call RETURNED (must never happen)"
```

`mmu.c`'s FREERACE block is the clearest case: it is a *diagnostic inside a normal, returning
function* (`v2 free`), chosen deliberately because "we may hold `vm_lock` here, and `uart_puts` can take
a lock". That is exactly the trade the card names — the diagnostic is made lock-free and therefore
becomes able to shred, and be shredded by, every other CPU's console traffic. Its comment says
"reports and changes no behaviour"; what it does change is the integrity of the console for every other
writer while it runs.

So the clause is currently **violated by construction**: the raw sink is used from returning code.

## 3. What the fix has to be (shape, not code — this is a design call)

Two separable requirements, and the DoD asks for both:

1. **Make the raw sink unusable from a returning path.** Either (a) give the console an owner: the
   first raw use claims it for that CPU; while claimed, other CPUs' console writes are dropped or
   buffered, and the owner only releases on a non-returning exit; or (b) mechanically forbid it — a
   gate that fails the build when a raw-sink call sits in a function that returns (the callers above
   are exactly the list it would have to whitelist, and the whitelist would be short).
2. **Prove it with a run that fires `[FREERACE]`/`[STALLBREAK]` and shows zero byte-interleaved
   lines.** The instrument for "byte-interleaved" is not `t4_glued.sh` as it stands — that counts
   record *fusions* (two tags on a line), while this defect produces *shredded* records with no second
   tag at all, like the line quoted above. A byte-level check (e.g. lines matching a known record
   prefix but failing that record's grammar) is needed before the zero can be claimed.

## Status

**In Progress, DoD unmet.** Evidence delivered: one verbatim shredded record from a real run today, and
the returning-path inventory that shows the raw sink is still reachable from code that returns. What is
missing is (a) a run with `[FREERACE]`/`[STALLBREAK]` under the current tree, and (b) the fix itself —
which is a design choice (console ownership vs build-time prohibition) I am flagging rather than
picking unilaterally.
