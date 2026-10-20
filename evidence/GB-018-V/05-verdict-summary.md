# GB-018-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-6d84faa6`, host **swarm-farm-2**
- Date: 2026-10-20 (UTC)
- Revision run: **e2b8e932c933431467ce2225a94cb36c2ebb64ae** — `HEAD` == `origin/main` (fetched), tree clean
- Env: `musl-tools` present; `third_party/llvm-21.1.8/dist-staging` present (seeded)

## Gate as written

| step | command | result |
|---|---|---|
| 1 | `bash tools/gate/t4_device.sh --self-test` | exit 0 — both negative cases fail as required |
| 2 | `make obj/t4_probe.elf` | no such target (`OBJ_DIR = obj/$(ARCH)`); `make ARCH=intel obj/intel/t4_probe.elf obj/intel/t4_legA.out` → exit 0 |
| 3 | `T3_STAGE='mcopy … ::T4PROBE.BIN' bash tools/t3_tier.sh intel` | **FAIL** — 999 lines, 5 FAIL lines, 44 user memory faults, 63 programs exited, halted=1; `T3_intel EXIT=1` |
| 4 | `t4_device.sh` over that serial | **FAIL** — `the probe never arrived in the guest`, exit 1 |

Step 3 verbatim:

```
T3_intel: note — halt marker found after make exited (poll raced the exit)
T3_intel: 999 console lines, 5 FAIL lines, 44 user memory faults, 63 programs exited, halted=1
T3_intel: FAIL — 5 FAIL line(s); first 20:
532:[ARP] routing off-subnet: … Graphics Validation: FAILED ( local_ip=0x000000000F02000A …
925:  FAIL: line startpoint
926:  FAIL: line midpoint
927:  FAIL: line endpoint
938:GRAPHICS TESTS FAILED
T3_intel EXIT=1
```

## Why RED

**R1 — not measured.** `T4PROBE  BIN  19328  2026-10-20 2:54` is on `disk.img`; the serial has zero
`t4-probe` lines. Note that `POLLTST.BIN` does run and exits (`[KERNEL] Process 49: POLLTST.BIN exited`),
but it emits no `row=` lines and is not the differential probe — "poll ran" is not parity evidence.

**R2 — none of the six DoD rows exists in the guest probe.** They are host-mock only:

```
src/host/t4_compat_probe.c:299  report("poll_ready", …)
src/host/t4_compat_probe.c:300  report("poll_ready_revents", …)
src/host/t4_compat_probe.c:306  report("select_ready", …)
src/host/t4_compat_probe.c:307  report("select_ready_fdset", …)
src/host/t4_compat_probe.c:323  report("poll_badfd", …)
src/host/t4_compat_probe.c:324  report("poll_badfd_revents", …)

occurrences in src/user/t4_probe.c: all six = 0
```

## DoD 3 (run timing-sensitive rows twice)

Moot and stated: the six rows are timing-sensitive and were to be run twice, but they were never
measured once — the probe never entered the guest. Across this session the intel wave was run twice
independently (GB-017-V at 02:31 and this ticket at 02:56, different worktrees) and both serials carry
zero `t4-probe` lines, so the absence is deterministic, not a 1-in-N flake.

## Verdict

**RED.** GB-018 (#19) → In Progress; this verify ticket closes Done.
