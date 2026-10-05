# WX lane — x64 wedge family evidence (branch x64/wedge-fix @ 98fe20b base)

## Context
Lane WX of the browser wave; OS repo worktree only. Evidence captured 2026-10-05
on this box (64 cores, quiet load ~1.4 at capture). Fork (webkit-hobbyos + bw3)
is read-only evidence; its WK-3 x64 runs 20-23 and the merged-tip OS were used
as-is.

## Family items and verdicts (one-cause-vs-many)
1. Test-wave wedge near final STRESS: **PRE-EXISTING latent x64 class, NOT a GX
   regression.** Reproduced identically on THIS box for BOTH the merged tip
   (KVM, STRESS stalls at iteration 10-20; busy SYS_SPAWN famine + 34 zombie
   slots + "Loader (v2) starved (slot pressure)") AND the GX freeze tree
   9a3b87e (KVM control, STRESS stalls at iteration ~80; all-idle + LOSTWAKE
   dispose of STRESS.BIN). browser.md documents this class for weeks:
   "test-x64 known-incomplete", "late-IDLESTUCK at STRESS.BIN", "slots held at
   st=4 EXITED, unreaped", "wedge fires ~2/3 of runs" (soak), FATBIG
   tail-checksum got=0x800 want=0x0 and PROCTEST waitpid fails all recorded
   there too. The freeze "completes 2/2" evidence (batF-x64-wave2.log) was a
   luckier draw of the same probabilistic class. Merged tip's STRESS stall is
   EARLIER (iteration 10-20 vs ~80), consistent with the larger `.lbss`-claimed
   binaries (every wave bin +8-10%: STRESS.BIN 54,328 -> 59,096) consuming more
   per-process frames and reaching the same slot/frame pressure sooner; the
   kernel's process/loader code is byte-identical freeze-vs-tip (diff only
   touched Makefile, main.c (ARM-guarded), virtio_gpu*, linker.ld).
2. WK-3 x64 WebProcess early halt: **fork-side, precise handoff.** Current fork
   intel WebProcess (100,490,824 B) halts at "WK4 START network-smoke" in an
   all-CPU SYS_FUTEX spin (runs 20/21/22). The gate-era intel binary
   (99,720,440 B, browser/l8-wk3x64) running on the SAME merged OS scratch
   completes WK-3 ALL-DONE with cross-arch FPC parity 0x759431c5 (run 23).
   => The merged OS can run WK-3 x64; the current fork binary's WK-4
   network-smoke path (blocking DNS + TCP fetch) wedges on this OS. Handoff in
   WX-REPORT.json.
3. Early kernel #PF dump (vector 14, RIP 0x70020EB6, 4/4 runs): **one shared
   sub-cause of (2)'s diagnostics, NOT a wedge.** RIP 0x70020EB6 =
   sys_readargv (SYS_GETARGV) marshalling argv into a user page that is not yet
   resident (WEBPROC.BIN is lazily demand-loaded; the argv buffer page lives
   deep in the image at 0x1005FD2xxx). The x64 trap handler prints the
   kernel-mode FATAL dump, THEN the existing L9 pagein recovery (trap.c:2188)
   materializes the legal user page via vm_handle_fault and iretq retries —
   so the run continues (run 23 proves it: same dump, ALL-DONE). The dump is
   cosmetic-but-misleading (label "FATAL"), new at the merged tip (4/4) only
   because bigger binaries grow the argv/stack offset into the lazy region.

## Suggested OS-scope soft fix (landed with this branch)
trap.c: move the kernel-mode #PF user-space recovery check ABOVE the FATAL
dump so a recoverable legal-page materialization is silent (or prints a
one-line "##PF" note) instead of a 16-line "FATAL" scare dump; a genuine
overrun (hole/guard/prot) still reaches the dump+kill path. This silences item
3's noise; it does not change liveness.
