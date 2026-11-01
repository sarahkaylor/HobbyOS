# GB-016-V (#350) — VERDICT: GREEN

**Graded head:** `ticket/16-uname-v2` @ `112488edf5b72d074cc5bd1ab419c243b59fcc9e` — `git rev-parse HEAD` printed exactly that in a fresh worktree (`.worktrees/gb016v`, detached).
**Base for the falsifier:** `8e231fb3` (main), checked out in the same worktree — `git rev-parse --short HEAD` → `8e231fb3`.
**Graded by:** verifier (wgames, deep-class), 2026-11-01, QEMU on this VM.

| # | Criterion | Result |
|---|---|---|
| 1 | head quoted, branch not moved | PASS — `112488edf5b72d074cc5bd1ab419c243b59fcc9e` |
| 2 | `--self-test` 6/6, rc=0 | PASS |
| 3 | head run PASS with the five rows | PASS |
| 4 | **falsifier:** base kernel prints `rc=-38`, gate FAILs | PASS — `rc=-38`, gate rc=1 |
| 5 | `ARCH=arm MODE=unit_tests` build rc=0 | PASS |
| 6 | `release=` not asserted | stated below |

---

## 2. Self-test — six OK lines, rc=0

```
$ bash tools/gate/t4_gb016_uname.sh --self-test
=== t4_gb016_uname self-test ===
  OK — rc=0 (a dispatch exists)
  OK — sysname=Linux
  OK — machine=x86_64
  OK — bad pointer -> -EFAULT
  OK — Linux-ABI proc
  OK — -ENOSYS console rejected (falsifier fires)
t4_gb016_uname self-test: 6/6
```

## 3. Head run — the rows on a real guest

```
$ bash tools/gate/t4_gb016_uname.sh          (T3_LOG=…/evidence/gb-016-v/head_t3_intel.log)
gb016 BEGIN
gb016 rc=0
gb016 sysname=Linux
gb016 machine=x86_64
gb016 release=6.1-hobbyos
gb016 efaul rc=-14
gb016 END
v2 loader: PID=2 entry=0x10000012A4 sp=0x1800000000 resident=22 tables=6 abi=0x1
t4_gb016_uname: PASS — uname(63) dispatched, sysname=Linux machine=x86_64, bad pointer -> EFAULT, from a Linux-ABI proc
HEAD_GATE_RC=0
```

Every row the card names is there, including `efaul rc=-14` (a bad pointer is rejected with `-EFAULT`, not a fault)
and the loader's `abi=0x1` proving the caller is a Linux-ABI process.

## 4. The falsifier — the same probe ELF against the base kernel

The probe was saved before the checkout (`gb016_uname_head.elf`, sha-stable), the tree was moved to `8e231fb3`,
rebuilt, and the **same binary** staged under the same manifest:

```
$ git rev-parse --short HEAD
8e231fb3
$ GB016_ELF_PATH=…/gb016_uname_head.elf bash tools/gate/t4_gb016_uname.sh
gb016 BEGIN
gb016 rc=-38
gb016 sysname=
gb016 machine=
gb016 release=[RESUME-DIAG] pid=2 region=yes prot=3 dest=0x00000017FFFFFAF0 covers=1
gb016 efaul rc=-38
gb016 END
v2 loader: PID=2 entry=0x10000012A4 sp=0x1800000000 resident=22 tables=6 abi=0x1
t4_gb016_uname: FAIL — uname did not return 0 (rc=-38 means no dispatch)
t4_gb016_uname: FAIL — sysname is not Linux
t4_gb016_uname: FAIL — machine is not x86_64
t4_gb016_uname: FAIL — bad pointer did not return -EFAULT
BASE_GATE_RC=1
```

`rc=-38` is `-ENOSYS`, the pre-fix signature the card predicted, from the identical probe and an identical
`abi=0x1` loader line. **The dispatch is what changed.** (The `release=` row collided with a kernel
`[RESUME-DIAG]` line on this boot — a glued line; it is not asserted, see clause 6.)

## 5. ARM build

```
$ make ARCH=arm MODE=unit_tests hobbyos.elf      -> ARM_BUILD_RC=0
```

## 6. What this verdict does **not** claim

`release=` is printed (`6.1-hobbyos` at the head) but the gate does not assert it, so **no verdict may say the
release string matches** — and this one does not.

**Disclosed, not counted:** the tier that carries the gate exits 1 on its own completeness floor —
`T3_intel: FAIL — only 1 programs exited (floor 55): too little work for a complete wave` — because this gate
deliberately boots a **one-member** manifest. Its console still says `halted=1`, `0 FAIL lines`, and the gate's
own assertions are what passed. The tier's floor failure is an artifact of the directed manifest, not a finding
about GB-016; I am not claiming a clean tier.

## Artefacts

`head_gate_stdout.txt` · `head_gate_summary.txt` · `head_t3_intel.log` · `gb016_uname_head.elf` ·
`base_gate_stdout.txt` · `base_gate_summary.txt` · `base_t3_intel.log` · `base_head.txt` · `base_disk_build.log` ·
`arm_build.log`