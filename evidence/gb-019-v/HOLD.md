# GB-019-V — no verdict issued: card is blocked by construction (returned to Backlog)

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `3dc6dcb` (clean worktree).
Claimed at 20:40Z because the card was in `Ready`; measured; put back in `Backlog`+`blocked`.

**This is not a RED on GB-019.** A RED would say "the implementation failed the gate". The gate
cannot measure this ticket's rows at all, so no GREEN/RED is issued and #20's lane is untouched.

## Ground 1 — GB-072 (staged programs are never dispatched): still open

```
$ grep -n "test_wave_loader\|manifest" src/kernel/main.c
140: static void test_wave_loader(void *arg) {
476: /* The test wave loads in a kernel thread (test_wave_loader). */
483: process_create_kernel_nowait(test_wave_loader, 0);
```

The boot wave is still the hardcoded list; no manifest path exists. DoD step 3
(`T3_STAGE=… bash tools/t3_tier.sh intel`) therefore stages a file the kernel never spawns —
exactly what GB-066-V, GB-070-V and GB-016/017/018-V each measured as 0 `t4-probe` lines.

## Ground 2 — GB-073 (the rows this DoD grades do not exist in the guest probe): still open, at runtime

Built with the working invocation and executed on this host:

```
$ make ARCH=intel obj/intel/t4_probe.elf          # 04_build_probe_intel.log
EXIT=0
$ ./obj/intel/t4_probe.elf | grep -c "^row="
27
$ ./obj/intel/t4_probe.elf | grep -E "^row=(fcntl|ioctl|dent)" | wc -l
0
$ ./obj/intel/t4_probe.elf | grep "^row=" | cut -d' ' -f1
row=write3 row=uname_rc row=uname_sysname_linux row=uname_machine_x86_64 row=getrandom_16
row=urandom_open row=urandom_read16 row=null_open_w row=null_write3 row=null_open_r
row=null_read1 row=pipe2 row=poll_ready row=poll_ready_revents row=select_ready
row=select_ready_fdset row=pselect6_ready row=pselect6_ready_fdset row=poll_badfd
row=poll_badfd_revents row=sockpair row=sockpoll_ready row=sockpoll_revents
row=sockselect_ready row=sockselect_fdset row=sockpselect_ready row=sockpselect_fdset
```

`fcntl_getfl_rdwr`, `fcntl_setfl_append`, `fcntl_getfl_after`, `fcntl_append_bit`,
`ioctl_tiocgwinsz_entty`, `ioctl_tcgets_entty`: **0 of 6 emitted**. Step 4 has nothing to diff.

## What does work (so the blame is placed correctly)

```
$ bash tools/gate/t4_device.sh --self-test        # 01_selftest.log
[self-test] clause 2 (wrong shim must FAIL):   OK — wrong shim FAILED as required
[self-test] clause 3 (absent probe must FAIL, not skip): OK — absent probe FAILED as required
self-test result: fail_count=0
EXIT=0
```

The gate itself can fail — it is simply aimed at rows that do not exist yet.

## Two defects in this card's gate text

1. **Step 2 is not a target.** `make obj/t4_probe.elf` → `No rule to make target 'obj/t4_probe.elf'`,
   and `make obj/intel/t4_probe.elf` fails too, because `OBJ_DIR = obj/$(ARCH)` and `ARCH` defaults to
   `arm` (Makefile:74, 3325). The working invocation is
   **`make ARCH=intel obj/intel/t4_probe.elf`** (exit 0 above).
2. **The card was promoted against an explicit hold.** The caretaker comment of 2026-10-20 16:26Z says
   "still correctly held; do NOT promote this card yet" with both grounds re-measured on `54860b2`.
   Both are still open on `3dc6dcb`, 4 h later. Whatever promoted it to `Ready` did so without
   re-checking GB-072/GB-073.

## Unblock condition

GB-072 (manifest dispatch for staged programs) and GB-073 (the six rows added to the guest probe,
`--check` non-vacuous) both `Done`. Then mint a **fresh** card — the ticket's own comment says this one
is consumed and should not launder into "first verdict" by reuse.
