# GB-016-V — gate run manifest

- Verifier: DEEP (DeepSeek v4.1 Flash, dgx1), worktree `hermes-c5297c99`, host **swarm-farm-2**
- Date: 2026-10-20 (UTC)
- Revision run: **0d381008f3c3626a5cd2d0a335a129ab3b13f78f** — `HEAD` == `origin/main`, tree clean
- Environment fixes made to run the gate at all (both are gaming.md §11.2b prerequisites, not code):
  - `musl-tools` was absent → `apt-get install -y musl-tools` (§11.2b #2; without it leg A cannot be built).
  - `third_party/llvm-21.1.8/dist-staging` absent in the worktree → copied the 227 MB staging from this
    VM's main checkout (§11.2b #4 / §11.5). The first attempt had started a cold LLVM build and died in
    `tools-staged` with `No rule to make target 'obj/arm/libc.a'` — a build-system defect worth its own
    ticket, not a GB-016 result.

## Gate, run as written (with the two path corrections noted)

| step | command | result |
|---|---|---|
| 1 | `bash tools/gate/t4_device.sh --self-test` | exit 0 — "wrong shim FAILED as required", "absent probe FAILED as required" |
| 2 | `make obj/t4_probe.elf` | **no such target** (`OBJ_DIR = obj/$(ARCH)`, `ARCH ?= arm`); working form `make ARCH=intel obj/intel/t4_probe.elf` → exit 0, plus `make ARCH=intel obj/intel/t4_legA.out` → leg A captured |
| 3 | `T3_STAGE='mcopy … ::T4PROBE.BIN' bash tools/t3_tier.sh intel` | **FAIL** — `1428 console lines, 0 FAIL lines, 6 user memory faults, 9 programs exited, halted=0`; `T3_intel: FAIL — the console has no 'System halt' line`; `EXIT=1` |
| 4 | `T4_LEGA_OUT=obj/intel/t4_legA.out T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh` | **FAIL** — `the probe never arrived in the guest (no 't4-probe-arrived' … after 120s)`; `exit=1` |

## Why RED (two independent reasons)

**R1 — the row is not measured (DoD 4).** The fixture *was* staged: `mdir` shows
`T4PROBE  BIN  19328` on `disk.img` at 01:47, yet the serial carries zero `t4-probe` lines and the wave
ends stuck (`[IDLESTOCK] slot=11 … HEAPTEST.BIN`, `[LOCKFOREVER] cpu=6 lock=0x70200240`, no `System halt`).
Staged-but-never-dispatched: the GB-013 loader/GB-068 wall, exactly as the ticket predicted. DoD 4 says
absent rows ⇒ RED "not measured", never GREEN, never SKIP.

**R2 — the instrument cannot measure GB-016 even if the probe arrives.** `src/user/t4_probe.c` emits
**one** row:

```
70:    prow("write3", r, ..., msg, 3);
$ grep -c uname src/user/t4_probe.c   -> 0
$ grep -c '^row=' obj/intel/t4_legA.out -> 1
```

GB-016's DoD rows (`uname_rc`, `uname_sysname_linux`, `release`/`machine`) are defined in
`src/host/t4_compat_probe.c:278-280` — the **host-mock** probe, which never enters the guest. So this
verify ticket, as minted, cannot pass: extending `src/user/t4_probe.c` with the uname rows is a
prerequisite for any GREEN here, and that is a plan/ticket defect, not a GB-016 code defect.

## Verdict

**RED.** GB-016 (#17) returns to In Progress with the failing check quoted; the verify ticket closes
Done with this record. Two follow-ups belong on the board: (a) extend the device probe with the uname
rows, (b) the `obj/t4_probe.elf` path in the gate text is wrong (`obj/intel/…`).
