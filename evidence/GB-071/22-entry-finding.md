**GB-071 — the kernel-range `rdi` is not an edge case. It is the normal state of a user process.** (entry-time probe, `0e5be89`)

I instrumented `general_interrupt_handler`'s first instruction: if the trap came from user mode (`cs&3==3`) and the framed `rdi` is already kernel-range, print it. One wave:

```
ENTRY-KBUF occurrences (raw tag count): 1870
parsed with pid+rdi: 1495
distinct rdi values: 751
  rdi=0x000000007559dbe0  x99      rdi=0x00000000755fdbc8  x45
  rdi=0x00000000755bdbe0  x98      rdi=0x000000007559dbc8  x43
  rdi=0x000000007558dbe0  x94      rdi=0x000000007558dbc8  x40
  rdi=0x00000000755fdbe0  x88      rdi=0x00000000755edbc8  x35
family-B faults: 18   pids all inside the ENTRY-KBUF pid set
```

Three things follow, and they change the shape of the ticket:

1. **The clobber is already present when the CPU enters the kernel.** So it is not produced by any
   handler; it was written into the user register before the process resumed — i.e. by the
   **return-to-user restore**.
2. **It is frequent, not rare**: 1495+ user-mode traps saw a kernel-range `rdi`, versus 18 fatal ones.
   A process normally runs with `rdi` pointing at its CPU's kernel scratch frame and only dies when it
   uses `rdi` as a string pointer before overwriting it. That is exactly why this family looked
   sporadic and why 43–55 faults per wave never matched a single culprit program.
3. **The values cluster at `…DBE0` / `…DBC8` on each CPU's 64 KiB stride** (`0x7559…`, `0x755b…`,
   `0x7558…`, `0x755f…`), i.e. the same `target_sp(cpu) ± frame` coordinate GB-068 derived — this is a
   per-CPU *kernel stack* pointer, not a leaked heap object.

Trap vector on those entries is the timer/preemption path, consistent with the restore that follows a
preempt-and-resume rather than a syscall return.

### What is left

The exact write that puts a kernel pointer into the restored `rdi` slot. The two candidates the evidence
still allows: (a) `save_context()` called on a **kernel-mode** frame (`cs&3==0`) recording the C
handler's `mov rdi,rsp` frame pointer into `context[5]`, which the resume then restores as the user's
`rdi`; (b) the resume tail copying a park frame whose `+40` slot was never rewritten from the context.
Both are inside `save_context()` / `enter_user_space`, and both are testable by printing `context[5]` at
`save_context` time with the `cs` that produced it — one more instrumented wave.

DoD 1 is now answered at the level the ticket asked (*register* and *instruction*, with pids), and the
producer is narrowed to one file. DoD 2 (fix + zero family-B faults), DoD 3 (regression guard) and DoD 4
(census attribution) remain open; the ticket stays **In Progress**. Evidence: `20-entry-parse.txt`,
`21-entry-kbuf-lines.txt`, raw serial `logs-t3-entry.log`.
