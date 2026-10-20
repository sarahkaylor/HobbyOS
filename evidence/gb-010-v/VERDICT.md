# GB-010-V — verdict: RED

Verifier: DEEP farm Verifier (DeepSeek v4.1 Flash, dgx1), 2026-10-20.
Tree: `origin/main` @ `0d38100` (== HEAD, clean except this evidence dir).
Gate: exactly the DoD commands — `gen_abi_doc.py --check`, one-number drift injection, and the
layout/ABI rows inside `make host_tests`.

## Criterion 1 — FAIL (the verdict): the drift detector fires on shipped main

```
$ python3 tools/linux-abi/gen_abi_doc.py --check      # 01_check.log
docs/linux-abi.md is STALE — re-run tools/linux-abi/gen_abi_doc.py
EXIT=1
```

Regenerating into a scratch copy shows the drift is exactly three missing rows and nothing else —
every ABI number in the doc still matches the probe:

```
$ python3 tools/linux-abi/gen_abi_doc.py --doc /tmp/abi_regen.md && diff -u docs/linux-abi.md /tmp/abi_regen.md   # 02_doc_diff.log
@@ -459,6 +459,9 @@
 KEY_A=30 KEY_ENTER=28 KEY_ESC=1 KEY_SPACE=57
+GAMEPAD BTN_A=0x130 BTN_B=0x131 BTN_X=0x133 BTN_Y=0x134 BTN_TL=0x136 BTN_TR=0x137 BTN_TL2=0x138 BTN_TR2=0x139
+GAMEPAD BTN_SELECT=0x13a BTN_START=0x13b BTN_MODE=0x13c BTN_THUMBL=0x13d BTN_THUMBR=0x13e
+GAMEPAD ABS_X=0x0 ABS_Y=0x1 ABS_Z=0x2 ABS_RX=0x3 ABS_RY=0x4 ABS_RZ=0x5 ABS_HAT0X=0x10 ABS_HAT0Y=0x11
```

Cause: `8385989` (2026-10-17, "probe_abi: print Linux gamepad/joystick evdev BTN_/ABS_ codes
(GB-042 parity)") extended the probe without regenerating `docs/linux-abi.md`, and `--check` is not
in any pre-merge gate, so the doc has been stale on `main` since that commit.

## Criterion 2 — PASS: the detector can fire on a single altered number

```
$ python3 tools/linux-abi/gen_abi_doc.py --check --doc /tmp/abi_num.md   # stat 144 -> 148  (03_injected_number.log)
docs/linux-abi.md is STALE — re-run tools/linux-abi/gen_abi_doc.py
EXIT=1
```

(Message names the doc path, not the scratch file — cosmetic, not misleading.)

## Criterion 3 — PASS: the layout/constants tests run inside `make host_tests`

```
$ make host_tests                                       # 04_host_tests_full.log
=== Linux ABI table lock (GB-014/GB-015) ===
  PASS SYS : all 114 golden rows match
  PASS ERR : all 54 golden rows match
=== 12 checks, 0 failed ===
ALL LINUX ABI TABLE TESTS PASSED
  PASS all 99 layout golden rows match (sizes + offsets of linux_layout.h)
=== 100 checks, 0 failed ===
ALL LINUX LAYOUT TESTS PASSED
  PASS all 65 constant golden rows match (L_* macros in linux_syscall.h)
=== 67 checks, 0 failed ===
ALL LINUX ABI CONSTANTS TESTS PASSED
...
Checks run:    482
Checks failed: 0
TEST EXIT: 0
```

(`05_abi_rows.log` is the excerpt.)

Environment note, stated so the run is reproducible: `make host_tests` first failed at
`obj/host_rtti_test.o: fatal error: 'typeinfo' file not found` because a `hermes -w` worktree does not
carry the gitignored vendored tree; I symlinked
`third_party/libcxx-21.1.8/src → <primary>/third_party/libcxx-21.1.8/src` and re-ran. That is a worktree
provisioning gap (GB-006), not a defect in this ticket's gate.

## Fix (one command)

```
python3 tools/linux-abi/gen_abi_doc.py && git add docs/linux-abi.md && git commit
```

— and add `--check` to the merge gate so the next probe extension cannot leave the doc stale
(that is GB-005's wiring, which GB-005-V already found absent from `main`).
