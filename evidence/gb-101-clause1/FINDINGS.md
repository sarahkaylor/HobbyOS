# GB-101 clause 1 — measurement on `98fc494` (origin/main), 3 consecutive intel T3 waves

Box: swarm-farm-1 (.230). Head: `98fc494` (`GB-101: [TORNFRAME] torn-frame detector` merged, #240).
Command: `bash tools/t3_tier.sh intel` (the §6 T3 tier, ARCH=intel). No CI consulted.
No source change was made; `git status` clean apart from untracked evidence.

## What the three runs say

| run | console lines | FAIL lines | FATAL kernel-mode lines | `[TORNFRAME]` records | impossible vector | tier verdict |
|---|---|---|---|---|---|---|
| 1 | 39043 | 190 | 32 | **0** | none | FAIL — 32 FATAL lines |
| 2 | 17332 | — | — | **0** | **`vector=1879291950`** | harness timeout (EXIT=124), no verdict |
| 3 | 10930 | 82 | 15 | **0** | none | FAIL — 15 FATAL lines |

The literal clause ("no impossible-vector fatal in 3 consecutive intel T3 runs") is **not met**: run 2
carried `vector=1879291950` (= `0x7003A33E`). And in all three runs the merged detector emitted **zero**
`[TORNFRAME]` records — including the run that carried the torn frame.

## Run 2's torn frame, verbatim

```
  RIP: ...97  RSP: 0x0000000031FFE458
  CS:  0x0000000031FFE388  SS:  0x000000007003DAE2  RFLAGS: 0x0000000031FFE7F0
  RAX: 0x0000000000000003  RBX: 0x0000000031FFE458  RCX: 0x0000000000000025  RDX: 0x000000007556D1D0
  ...
[KERNEL] FATAL kernel-mode fault, halting this CPU: vector=1879291950 rip=0x000000002BFFE938 cr2=0x00000017FFFFF388 pid=36 err=6
```

Symbolized against the MODE=test kernel (`hobbyos.elf`):

```
0x7003DAE2  = restore_user_sp_helper + 0x12      (the SS slot)
0x7003A33E  = arch_fpu_restore      + 0x1E      (the vector slot)
0x7003D8DF  = enter_user_space      (run 1's coherent #PF RIP)
0x700269C1  = schedule + 0x?? (process.c:1804)   (run 1's STACK[0])
```

## What this rules in

1. **The detector does not see this instance.** `general_interrupt_inner`'s gate fires on
   `vector > 0xFF || cs not in {0x08,0x1B}` at handler entry. Run 2 reached the *generic* fatal path
   (`trap.c:3090`), not the gate — so at entry the frame was coherent and the garbage appeared later.
2. **The garbage values are return addresses from the exit path's own helper calls.** `SS` holds an
   address inside `restore_user_sp_helper`; the `vector` slot holds one inside `arch_fpu_restore`. Both
   are functions `common_trap_exit` / the resume path `call`s. In the exit sequence the frame's slots are
   consumed and `rsp` is advanced (`add rsp,264` / `add rsp,16`) before helpers are called, so a pushed
   return address lands in a slot that still *looks like* a frame field. That is the "fields belong to
   different frames" shape the Coordinator predicted on 10-26 — with a named mechanism instead of a guess.
3. **The dominant killer is a different, coherent fault.** Runs 1 and 3 die on
   `FATAL: Exception in Kernel Mode! Vector: 14 Error Code: 0x2` with `RIP 0x7003D8DF` =
   `enter_user_space`, `STACK[0]` = a return address into `schedule`, `pid=21`, `RDI/cr2 = 0x17FFFFF2F8`
   — i.e. `enter_user_space`'s frame copy writing a user-stack page that is not mapped, immediately after
   `Loader (v2) starved (slot pressure)`. Those frames are coherent (CS=0x08, kernel RIP, real return
   address on the stack), so they are not this card's torn-frame defect.

## What is not claimed

- I did not instrument the exit path, so (2) is a symbolization-backed hypothesis, not a measured write.
- Run 2 has no tier verdict (my 2400 s harness timeout fired before the tier's 2700 s deadline); its
  guest log is still evidence of what the kernel printed.
- No fix is proposed here. Clause 1 needs one, and the two candidate mechanisms are now separable.