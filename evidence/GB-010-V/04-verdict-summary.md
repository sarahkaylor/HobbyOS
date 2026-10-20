# GB-010-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-fbfe2bc7`
- Date: 2026-10-20 (UTC)
- Revision run: **0d381008f3c3626a5cd2d0a335a129ab3b13f78f** — `HEAD` == `origin/main` (fetched), worktree clean
- Host: this VM, `cc` = system gcc; worktree seeded with `tools/swarm/seed-worktree-vendored.sh`

## Gate, run exactly as written

| # | command | result | log |
|---|---|---|---|
| 1 | `python3 tools/linux-abi/gen_abi_doc.py --check` | **exit 1 — `docs/linux-abi.md is STALE`** | `01-gen-abi-doc-check.log` |
| 2 | one-number mutation of the doc in a scratch copy | fires: exit 1 mutated, exit 0 restored | `02-drift-detector-mutation.log` |
| 3 | `make host_tests` (the `LINUX LAYOUT/ABI` rows) | `ALL APPS SUITE TESTS PASSED`, 482 checks 0 failed; the three rows below print their own counts with 0 failed | `03-host-tests.log` |

## The failing check (DoD item 1)

```
$ python3 tools/linux-abi/gen_abi_doc.py --check
docs/linux-abi.md is STALE — re-run tools/linux-abi/gen_abi_doc.py
exit=1
```

Cause, measured: `tools/linux-abi/probe_abi.c` prints three `GAMEPAD ...` lines (added by
`8385989 probe_abi: print Linux gamepad/joystick evdev BTN_/ABS_ codes (GB-042 parity)`), the
committed doc contains `GAMEPAD` **0** times. Regenerating in place adds exactly:

```
+GAMEPAD BTN_A=0x130 BTN_B=0x131 BTN_X=0x133 BTN_Y=0x134 BTN_TL=0x136 BTN_TR=0x137 BTN_TL2=0x138 BTN_TR2=0x139
+GAMEPAD BTN_SELECT=0x13a BTN_START=0x13b BTN_MODE=0x13c BTN_THUMBL=0x13d BTN_THUMBR=0x13e
+GAMEPAD ABS_X=0x0 ABS_Y=0x1 ABS_Z=0x2 ABS_RX=0x3 ABS_RY=0x4 ABS_RZ=0x5 ABS_HAT0X=0x10 ABS_HAT0Y=0x11
```

`docs/linux-abi.md | 3 +++`. The doc was never regenerated after GB-042 extended the probe. This is
not host/compiler variance — the lines are literal prints in the probe source, so any Linux x86_64
runner on `main` reports the same staleness.

## DoD item 2 — the detector provably fires

Scratch copy (`/home/sarah/.hermes/cache/scratch/abi-mutation.md`, tracked doc untouched):
regenerate → `--check` exit 0; mutate one number (`read 0` → `read 9`) → **exit 1**; restore → exit 0.

## DoD item 3 — assertions run inside `make host_tests`

```
=== 12 checks, 0 failed ===
ALL LINUX ABI TABLE TESTS PASSED
  PASS all 99 layout golden rows match (sizes + offsets of linux_layout.h)
=== 100 checks, 0 failed ===
ALL LINUX LAYOUT TESTS PASSED
  PASS all 65 constant golden rows match (L_* macros in linux_syscall.h)
  PASS no invented constant rows (65 checked)
=== 67 checks, 0 failed ===
ALL LINUX ABI CONSTANTS TESTS PASSED
```

## Verdict

**RED** — DoD item 1 fails on `origin/main` at `0d38100`: the doc is stale relative to its own probe.
Items 2 and 3 pass and are proven able to fail. Fix is mechanical: `python3
tools/linux-abi/gen_abi_doc.py`, commit the 3 added lines, re-run this verify ticket.
