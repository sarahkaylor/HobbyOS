# GB-017-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-35d35492`, host **swarm-farm-2**
- Date: 2026-10-20 (UTC)
- Revision run: **e2b8e932c933431467ce2225a94cb36c2ebb64ae** — `HEAD` == `origin/main` (fetched), tree clean
- Env: `musl-tools` present; `third_party/llvm-21.1.8/dist-staging` present (seeded from main checkout)

## Gate as written

| step | command | result |
|---|---|---|
| 1 | `bash tools/gate/t4_device.sh --self-test` | exit 0 — wrong shim FAILED as required, absent probe FAILED as required |
| 2 | `make obj/t4_probe.elf` | **no such target** (`OBJ_DIR = obj/$(ARCH)`); working form `make ARCH=intel obj/intel/t4_probe.elf obj/intel/t4_legA.out` → exit 0 |
| 3 | `T3_STAGE='mcopy … ::T4PROBE.BIN' bash tools/t3_tier.sh intel` | **FAIL** — 946 lines, 4 FAIL lines, 39 user memory faults, 60 programs exited, halted=1; `T3_intel EXIT=1` |
| 4 | `T4_LEGA_OUT=… T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh` | **FAIL** — probe never arrived (no `t4-probe-arrived`), exit 1 |

Step 3 output verbatim:

```
T3_intel: note — halt marker found after make exited (poll raced the exit)
T3_intel: 946 console lines, 4 FAIL lines, 39 user memory faults, 60 programs exited, halted=1
T3_intel: FAIL — 4 FAIL line(s); first 20:
872:  FAIL: line startpoint
873:  FAIL: line midpoint
874:  FAIL: line endpoint
885:GRAPHICS TESTS FAILED
T3_intel EXIT=1
```

## Why RED

**R1 — not measured.** `T4PROBE  BIN  19328  2026-10-20 2:28` is on `disk.img`, but the serial carries
zero `t4-probe` lines; the wave's 60 exited programs do not include the fixture.

**R2 — the instrument cannot emit any row this ticket names.** All six DoD rows exist only in the host
probe, none in the guest probe:

```
src/host/t4_compat_probe.c:136  report("getrandom_16", …)
src/host/t4_compat_probe.c:149  report("urandom_open", …)
src/host/t4_compat_probe.c:153  report("urandom_read16", …)
src/host/t4_compat_probe.c:164  report("null_open_w", …)
src/host/t4_compat_probe.c:167  report("null_write3", …)
src/host/t4_compat_probe.c:172  report("null_open_r", …)

urandom_open     0   urandom_read16   0   null_open_w      0
null_write3      0   null_open_r      0   getrandom_16     0      # occurrences in src/user/t4_probe.c
$ grep -n 'prow("' src/user/t4_probe.c
70:    prow("write3", r, …);
```

So step 4 could only ever diff one row (`write3`), and DoD 3's "state that you deliberately excluded the
random payload" is moot until the rows exist.

## Class-level note for the plan

This is the same defect as **GB-016-V** (verified earlier at 0d38100, same finding). GB-018-V, GB-019-V,
GB-020-V and GB-021-V are minted against the *same* device probe, so they will all fail for R2 until
`src/user/t4_probe.c` gains their rows. One ticket to extend the probe unblocks five verifications; the
individual waves only re-measure the dispatch wall.

## Verdict

**RED.** GB-017 (#18) → In Progress with the failing check quoted; this verify ticket closes Done.
