# cp-fork: fork cost proportional to what is mapped (wave 6, lane C1-fork)

Branch `cp-fork`, from `cyberpunk` @ 058fd3d5.  Evidence in `docs/cp-fork-evidence/`.
Numbers are from **strix** (Ryzen AI MAX+ 395, Zen 5, KVM, 8-vCPU intel guest) unless
marked "Linux host" (the same strix, kernel 7.0).

## 1. The bug (C9-play, stop P9-1)

Cyberpunk calls `CreateProcess(REDEngineErrorReporter.exe)` at launch; Wine's
`NtCreateUserProcess` forks the game process twice (it double-forks to detach) and
execs the preloader.  That took 16.5-17.6 s, and when it ran a little longer the game
exited on its own (integ7's "clean early exit").  At that point the game holds
~1300 regions, ~135 GiB of anonymous reservations (almost all PROT_NONE) and ~508 MiB
resident.

`vm_as_clone_into()` walked **every 4 KiB page of every region**, PROT_NONE
reservations included, taking one `vm_lock` irqsave per page (three per present page),
and copied every resident private page with a byte loop into a zeroed frame.  Cost
was proportional to reserved VA: C9's `p9forkprobe` F2a (a 64 GiB PROT_NONE
reservation alone) took 3.9 s per fork.

## 2. What changed

**Step 1 (aea9eb44): walk what is mapped.**
- Absent page-table levels are skipped whole with `vm_arch_skip_absent()` (one step
  per absent 4 KiB-table / 2 MiB / 1 GiB / 512 GiB span).
- `vm_lock` is taken once per batch of up to 32 present pages or 512 steps.  The
  region record, the parent leaf, the object frame and the child leaf are read and
  written in the same critical section, so a peer thread's munmap/mprotect cannot
  land between "look" and "map".  The region is re-found by index under every hold
  (the list may only shrink: fork copies the caller, D10/P1 OQ5).
- `vm_frame_copy()` copies 8 bytes at a time (no SIMD; frames are aligned on both
  arches) into an unzeroed frame.

That fixed the reservation cost (F2a 3914 -> 11.6 ms) but left the copy.  The copy is
worse than its byte count: the frame allocator's rotating hint hands a forking parent
frames the guest has never touched, and on strix QEMU's guest RAM is THP-madvised
with `defrag=madvise`, so the first touch of each fresh 2 MiB of guest RAM can stall
the vCPU in host compaction.  Measured inside one clone batch with IRQs off: a single
4 KiB copy took up to 663 ms; `compact_stall` rose by 9900 during one probe boot;
F2/F3 medians swung between 90 ms and 1.4 s with the host's load.

**Step 2 (1013849c): share private pages copy-on-write (x86_64).**
- At fork, each present private leaf (anonymous, or an object region's private
  copy) becomes read-only with a software COW bit (PTE bit 10) in the parent, and
  the child maps the same frame the same way.  A static open-addressed table
  (2^20 entries, 8 MiB .bss, linear probing with backward-shift deletion,
  `cowt_lock` under `vm_lock`) counts the address spaces that map each shared frame;
  an entry exists only while two or more do.
- A write fault on a COW leaf (`vm_handle_fault`, before the `[VMFIX]` prot-up that
  would otherwise hand the shared frame out writable), a kernel write through
  `vm_touch(write)` and `vm_kwrite` copy the frame while it is still shared, or take
  the leaf back writable in place when this AS is the last mapper.  A copy is
  followed by `vm_arch_shootdown()`: other threads may still read the shared frame
  through a cached translation (C1-pe's rule).
- Every leaf drop releases through `x64_leaf_release()` (`vm_arch_unmap`,
  `vm_arch_teardown`) or `vm_arch_unmap_keep()`, which free a COW frame only for its
  last mapper.  `vm_arch_prot()` keeps COW leaves read-only whatever mprotect asks.
- The parent's other CPUs are shot down once at the end of the clone (its leaves
  went read-only; a writable translation left on a sibling thread's CPU would let a
  later store reach the child).
- Object frames and SHARED leaves are re-mapped as before; ARM keeps the copying
  fork; a 3/4-full table falls back to copying for the rest of that fork.

Why COW was safe to add here and not before: every place that frees a user leaf goes
through three arch functions, and every kernel write into user memory goes through
`vm_touch(write)`/`vm_kwrite` or a hardware fault, so the count is consulted at all of
them.  A static table keeps frame counts around a fork exact (the unit tier checks
them).

## 3. Results (strix)

See the lane report and `docs/cp-fork-evidence/` for the tables and raw rows.

## 4. Tests

- `src/user/linux_probe_forkva.c`: C9's `p9forkprobe` cases (F1, F2a, F2b, F2, F3, F4)
  as a t3 probe; F2/F3 fail above 200 ms (C9's own probe fails at 1000 ms).
- `src/user/linux_probe_forkcow.c` K1-K9: snapshot and isolation both ways,
  `read(2)` into a shared page, three generations, a parent thread storing across
  fork, mprotect R/RW, MADV_DONTNEED, parent munmap under a live child, and 200
  fork/exit cycles of a 64 MiB parent without a leak.  Linux host: 5/5 runs PASS.
- `vm_test.c`: `test_vm_as_clone_fork` and the file-mapping fork test check the share
  (same frame, read-only + COW, counted), its break (a new frame, count gone) and the
  last mapper taking its leaf back in place.
- `[FORKT] pid clone_us regions resident batches max_hold_us`: one bounded row per
  slow clone (> 20 ms) and the first two.

## 5. Not done

- ARM still copies (its leaf paths and teardown are not COW-aware).
- The fork's remaining cost is ~0.5 us per shared page (4 page walks + one table
  insert); a 4 GiB-resident game would pay ~0.5 s per fork.  A per-table walk that
  copies whole PT pages would cut it further.
