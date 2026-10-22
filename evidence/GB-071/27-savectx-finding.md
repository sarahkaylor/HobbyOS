**GB-071 — save-time probe: candidate (a) is refuted as the dominant path (`c2f156e`).**

I instrumented `save_context()` to print whenever it is about to save a kernel-range `rdi` into `context[5]`, with the privilege level it came from — atomic single-write via `kline`, because the earlier probe lost ~43% of rows to the 8-CPU console interleave:

```
[SAVECTX-KBUF] … 4481 lines
cs=0 (kernel-mode trap):    62
cs=3 (user-mode trap):    4419
```

4419 of 4481 saves take a kernel-range `rdi` **from a user-mode frame** — the value is already in the user register when the CPU traps. The save is a faithful symptom, and "the C handler's `mov rdi,rsp` frame pointer is saved as the user rdi" (candidate (a)) explains 62 cases, not the family.

I also read the resume path end to end (`enter_user_space`, `trap.c:3149-3245`): the frame is copied with `rep movsq` (37 quadwords), the register block is restored by `mov rdi,[rsp+40]`, and the tail rearranges RIP/CS/RFLAGS/RSP/SS correctly for the `iretq` (`280`→RIP, `288`→CS, `296`→RFLAGS, `304`→RSP from `gs:[24]`, `312`→SS). Nothing there writes a kernel pointer into `[rsp+40]`; it restores what `context[5]` already holds.

So the cycle is closed and self-consistent: a user-mode trap carries a corrupt `rdi` → it is saved → it is restored → the process runs with it → the next trap carries it again. What is still unnamed is the **bootstrap**: which write put a kernel pointer into `context[5]` outside that cycle (a PCB slot initialised from stale data, or a fork/exec copying a context captured mid-syscall). That is a different site from every one measured so far, and it is the next instrument: print `context[5]` at `process_create`/`fork` and at the first `arch_set_user_sp`.

DoD 1 stands answered (register, instruction, and producer path). DoD 2/3/4 remain open; the ticket stays **In Progress**. Evidence: `24-savectx-lines.txt`, `25-savectx-cs-hist.txt`, `26-process-instrument.patch`, commit `c2f156e`.
