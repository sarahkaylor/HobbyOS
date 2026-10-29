**GB-101 clause 1 — root cause found, measured, not guessed: the resume page is listed in the AS but has no PTE (`pte=0`), and the kernel's `rep movsq` writes into it at CPL0.**

Five intel T3 waves on `98fc494` (main, i.e. with the merged `[TORNFRAME]` detector) got me here. The last one is the one that answers clause 1.

## The fault site, named

Every kernel-mode panic in these waves reports the **same RIP**: `0x7003D8DF`. Disassembled from `hobbyos.elf`:

```
7003d88b <enter_user_space>:
7003d896:  mov    rax,QWORD PTR [r12+0x118]   ; tf->cs
7003d89e:  and    rax,0x3
7003d8a2:  cmp    rax,0x3
7003d8a6:  je     7003d8ce                    ; user-mode resume
   ... kernel branch: rdi = gs:0x18 - 0x140 (this CPU's kstack) ...
7003d8ce:  lea    rdi,[r13-0x140]             ; user branch: rdi = target_sp - 0x140
7003d8d8:  mov    rcx,0x25                    ; 37 quads = 296 bytes
7003d8df:  rep movs QWORD PTR [rdi],QWORD PTR [rsi]   <-- #PF write, err=0x2, CS=0x08
```

So the crash is not a mystery frame and not the detector's business: it is the **37-quad frame copy in `enter_user_space`'s user-mode branch**, writing 296 bytes at `target_sp-0x140` on the user stack, from CPL0. `err=0x2` = write, page not present.

## Why the page is not present — the measurement that was missing

`[RESUME-DIAG]` already checked `vm_region_find` and reported `region=yes prot=3 covers=1` — which is why the open question in the code read "HOLE or PROT?". I extended it three ways (`instrument.patch`): the live CR3 vs the AS root, a page-table walk of the destination (`vm_arch_walk`), and — the important one — **probe every resume and print only the anomaly**, because the original five-probe sample always caught the healthy prefix.

Two waves:

- five-probe sample (healthy prefix): `region=yes prot=3 dest=0x17FFFFFEB8 covers=1 cr3=0x20C04000 as=0x20C04000 match=1 pte=1 w=1`
- anomaly-only probe, 25 hits, **all 25 identical in the decisive field**:

```
[RESUME-DIAG] pid=22 region=yes prot=3 dest=0x00000017FFFFF388 covers=1 cr3=0x0000000021203000 as=0x0000000021203000 match=1 pte=0
[RESUME-DIAG] pid=26 region=yes prot=3 dest=0x00000017FFFFF2F8 covers=1 cr3=0x0000000021C17000 as=0x0000000021C17000 match=1 pte=0
[RESUME-DIAG] pid=1  region=yes prot=3 dest=0x00000017FFFFF388 covers=1 cr3=0x0000000024542000 as=0x0000000024542000 match=1 pte=0
```

`pte=0` on **25 of 25**; the destinations histogram is

```
14 dest=0x17FFFFF388    8 dest=0x17FFFFFCB8    1 each 0x17FFFFF2F8 / 0x17FFFFEFD8 / 0x17FFFFEFB8
```

— every one of them the **top page of the user window** (`0x17FFFFF000`), which is exactly where the resume frame goes.

So neither candidate in the code's open question was right. It is not a HOLE (the region exists, `covers=1`) and not PROT (`prot=3`), and not a wrong address space (`cr3 == as`, `match=1`). It is the **region list and the page table disagreeing**: the stack region is registered, the top page's leaf was never materialised, and a *kernel* store does not fault a lazy page in — it takes the #PF at CPL0 instead.

## Fix direction

The resume copy must not be the first toucher of a lazy page. In order of preference:

1. **Materialise the destination before the copy** — `vm_touch(&proc_table[next], target_sp - 320, 296, 1)` immediately before `enter_user_space(&local_tf, target_sp)` (or `vm_kwrite`, which materialises *and* re-validates). **Risk I did not test:** `vm_touch` allocates a frame, and this call site runs under `proc_lock` inside `schedule()` — that lock order needs a look before this ships. I am flagging it rather than asserting it.
2. **Map the top stack page when the AS is built**, so the resume frame always has a page under it. Cheaper at resume, but it makes every process resident for 4 KiB it may never use.
3. **Turn the crash into a named failure**: if the walk says `pte=0` at resume, fail that process by name (`[TORNFRAME]`-style) instead of iretq-ing into an unmapped page. This does not fix anything, but it converts a CPU halt into a tier-visible record — which is what clause 1's "no impossible-vector fatal" actually needs to stop being a coin flip.

## State of the card

- Clause 2: GREEN and merged (`98fc494`); the detector is on main and **fired 0 times in all five waves** — worth knowing: the torn-frame detector does not see this defect, because this is not a torn read, it is a kernel write to an absent page.
- Clause 1: **RED, root-caused**, not fixed. Runs on `98fc494`: 32165 lines / 37 FATAL; 26173 / 8; 17464 / 8 — the guest halts on every one of five waves.

Evidence: `evidence/gb-101-clause1/` — `anomalies.txt` (the 25), `healthy_sample.txt`, `instrument.patch`, `t3_anom.log` (+`.gz`), `t3_anom.tier.txt`, `t3_cr3.log`, `t3_pte.log`, `t3_intel_c1.log`. Local runs only; no CI used or cited.

— `wgfx-verify`, DEEP verifier, 2026-10-29T00:25Z