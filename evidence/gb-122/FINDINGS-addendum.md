# GB-122 addendum — the page is **never committed**, not cleared. And my "no AS" explanation is wrong.

Addendum to `FINDINGS.md`, same card, same host. Head `215b79b` + `41-diag.patch` (diagnostic
only, not shipped). Coordinator's re-scope at 23:30Z asked for the unmap trace and for the
user-branch question answered directly. Both are answered below, and the second answer
contradicts what I told you an hour ago.

## 1. The measurement (`40-diag-wave.log`, `42-the-one-absent-resume.txt`)

Instrumented at the resume site in `schedule()`, where the process and its AS are both in hand:
for every resume landing in the user stack window, walk **the process's own AS** for the copy's
destination page and print it with CR3 and the parent pid.

```
375 resumes sampled
374  pte=1 w=1
  1  pte=0 w=0
```

The one:

```
[GB122] pid=15 parent=6 sp=0x17FFFFF4C8 dest=0x17FFFFF388 pte=0 w=0 cr3=0x20F1A000 root=0x20F1A000 if=3
```

Three facts in one line:

- **`cr3 == root`** — the tables walked are the ones in use. The wrong-address-space theory is dead.
- **`parent=6`** — the failing resume is a **forked child**, not a loader-created process.
- **`dest=0x17FFFFF388`, `sp=0x17FFFFF4C8`** — the carded address, unchanged.

And the parent, sampled at four depths in the same run:

```
[GB122] pid=6 parent=3 sp=0x17FFFFFFF8 dest=0x17FFFFFEB8 pte=1 w=1
[GB122] pid=6 parent=3 sp=0x17FFFFFF90 dest=0x17FFFFFE50 pte=1 w=1
[GB122] pid=6 parent=3 sp=0x17FFFFF6F0 dest=0x17FFFFF5B0 pte=1 w=1
```

The parent is healthy everywhere it was seen. The child resumes **0x140 deeper** than any depth
the parent was observed at, on a page that is **a hole in the parent too**.

## 2. Answer to your step 1 (trace the unmap): there is no unmap

The page is not cleared after build — it is **never committed** for this class. The user stack is
a demand reserve (`USER_MAIN_STK_SIZE` 8 MiB, "demand-zero" in the loader comment), the loader
prefaults only the top **64 KiB** (`USER_MAIN_STK_GUARD`, `program_loader.c:834-840`), and
`vm_as_clone_into` — quoted from its own call site — *"copied frame-by-frame … **holes stay
holes**"*. A forked child that resumes below the parent's touched window therefore lands the
296-byte frame on a hole, and a CPL0 store cannot demand-page it.

374 of 375 resumes are healthy, which is why this looked like a rare corruption rather than a
structural hole: the child has to be forked and then resumed deeper than the parent ever went.

## 3. Answer to your step 2 (who is supposed to commit that page): nobody is

The only writer that can commit a demand-zero stack page is the **user** fault path
(`vm_handle_fault` from a user trap). The resume copy runs at CPL0 *before* the user ever runs,
so there is no path that commits it. Step 5 of the re-scope applies verbatim: **the page is never
committed for this class, so the fix is "commit it"** — materialise the destination page before
the copy, or extend the fork path to prefault the same guard span the loader prefaults. That is
the smaller fix, and it is now the card.

## 4. Correction I owe you: route (2) failed for a different reason than I said

At the resume site the diagnostic prints **`cr3 == root`**, i.e. the user table *is* installed
when `enter_user_space` is entered. So my earlier conclusion — "the function carries no address
space, so a CR3 walk cannot know which tables to read" — **does not hold**: a CR3 walk at that
point should have seen `pte=1`, and 374/375 of the time it would have.

That means my register-only guard refused 68 of 68 programs for an **implementation reason I have
not yet identified**, not because the architecture forbids it. I am not going to leave my own
error standing as an architectural finding, so: **treat "no AS" as unproven.** What survives
from that build is narrower and still useful — the scoped kernel-branch-only version changed
nothing (the fault is the user branch), and the wave catches a refuse-everything build instantly.

## 5. What is measured vs what is not

- Measured: 1 absent resume in 375, on a forked child, `cr3 == root`, parent healthy at all
  sampled depths.
- Not measured: which `vm_as_clone_into` branch leaves that hole (private-copy vs shared vs never
  registered) — the diagnostic names the process and the address, not the loop iteration.
- The wave in `40-diag-*` **stalled** at 913 lines and was killed rather than waited out; its
  `Vector: 14` count is 0, so it is evidence for the sample table only, not for a wave verdict.
  Two banners in it belong to other families (`vector=6 rip=0x7003A1B0 cr2=0xFFFF…ED pid=13`,
  `vector=13 rip=0x25FFE210`) and are noted, not absorbed.

**No RED/GREEN** — `debug` card. Next step is small and is on the card: commit the destination
page (or prefault at fork), then the five-wave clause becomes obtainable for the first time.