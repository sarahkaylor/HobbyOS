# GB-082-V — verification evidence (DEEP Verifier, 2026-10-24)

Verdict **RED** on clause 1. Graded at `9f89a23` (the card's sha), fresh worktree, tree clean.
(`origin/main` is `7cc0ac5` now; this card names `9f89a23`, so that is what was run.)

| # | file | what |
|---|---|---|
| 01 | `01-glued-arm.log` | `t4_glued.sh obj/t3_arm.log` → **2** glued, exit 1 |
| 02 | `02-glued-intel.log` | `t4_glued.sh obj/t3_intel.log` → 2 glued, exit 1 |
| 03/04 | `03-tier-arm.log`, `04-tier-intel.log` | the two tier runs |
| 05/06 | `05-arm-glued-lines.txt`, `06-intel-glued-lines.txt` | the glued lines, verbatim |
| 07 | `07-selftest.txt` | counter self-test: injected pair → 4, clean → 0, absent log → exit 2 |
| 08 | `08-records.txt` | clause 4: each moved record still printed |

## Clause 1 — FAILS: ARM is 2, not 0
```
$ bash tools/gate/t4_glued.sh obj/t3_arm.log
t4_glued: 2 glued console lines in 'obj/t3_arm.log'          exit 1
'[GPU] irq handled (status=[KERNEL] Process 34: CAT.BIN exited unexpectedly or gracefully.'
'[GPU] irq handled (status=[NFS] nfs_test starting'
```
Both are the same writer: `src/kernel/virtio_gpu.c:447`
```c
uart_puts("[GPU] irq handled (status=");
print_int((int)status);
uart_puts(")\n");
```
three calls, so a peer's record lands between them. This site is **not** one of the four the fix
touched (`sys_fork` ×2, `[ARP]`, `console_write_user`) — so the "ARM glued 2 → 0" claim in the commit
message and the card's table does not hold on this run: the two went away, a third multi-call kernel
writer took their place in the count.

## Clause 2 — intel 2, named
```
'[KERNEL] process_create: no free process slots! zombies=[CKSUMTEST] starting'
'[WAITPID] ECHILD caller=[ARRIVED] CUT.BIN rc<0'
```
Writers: `process.c` (`process_create: no free process slots! zombies=`), and
`process.c:2782` `[WAITPID] ECHILD caller=` (five `uart_puts` calls: prefix, pid, space, name, newline).
Both are kernel-side, so per the card they are the "named residue" case rather than a user-record failure —
but they are the same defect class as clause 1, not "close enough to zero".

## Clause 3 — self-test green; the tier has no glued verdict of its own
```
self-test: glued fixture count=4 (want >0), clean fixture count=0 (want 0)
self-test: absent-log exit=2 (want non-zero) …
self-test: OK — counter can fail, stays 0 on clean, and refuses a missing log
```
`make t4_glued_selftest` → same, exit 0. The counter can fail, which is the part that matters.
The card also asks for "the tier's own glued verdict green on both arches": `tools/t3_tier.sh` contains
**no** `glued` reference (`grep -c glued tools/t3_tier.sh` → 0), so the tier does not run this counter at
all. That clause is unmeasurable as written; recording it rather than inferring a pass.

## Clause 4 — records still print
```
[ARP]                                   8
sys_fork: tf->regs[0] is now          143
Before eret                           232
irq handled                             3
```
No diagnostic was silently dropped.

## Tier shapes
```
ARM   : 20950 console lines, 7 FAIL lines, 6 user memory faults, 84 programs exited, halted=1
intel : 11965 console lines, 32 FAIL lines, 7 user memory faults, 78 programs exited, halted=0 (no 'System halt')
```
The intel run did not reach the halt, so its glued count is from a wedged wave; the two lines it has are
named above either way.
