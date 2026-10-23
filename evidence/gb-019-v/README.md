# GB-019-V — verification evidence (DEEP Verifier, 2026-10-23)

Subject: GB-019 (#20) ioctl/fcntl subset — verdict **RED**.
Revision: `46f19bbd2ef9bcf3ecdd892237463cf007c3d7a7` = `origin/main` (`PLAN-010`), fresh worktree.

| # | file | what it is |
|---|---|---|
| 01 | `01-selftest.log` | `t4_device.sh --self-test` → wrong-shim FAIL + absent-probe FAIL, exit 0 |
| 02 | `02-probe-build.log` | `make ARCH=intel obj/intel/t4_probe.elf` → exit 0 (the ticket's `make obj/t4_probe.elf` still has no rule) |
| 03 | `03-legA.log` | leg A on real Linux: **39 rows**, including all six in scope |
| 04 | `04-t3-intel-staged.log` | the staged intel wave (§ below) |
| 05 | `05-device-gate.log` | the leg A/leg B gate → exit 1, "the probe never arrived in the guest" |
| 06 | `06-facts.txt` | serial/manifest/ELF facts quoted in the verdict |

## What ran

```
$ bash tools/gate/t4_device.sh --self-test                      exit 0   (both fail paths proven)
$ make ARCH=intel obj/intel/t4_probe.elf                        exit 0   61,008 bytes
$ make ARCH=intel obj/intel/t4_legA.out                         exit 0   39 rows on real Linux
$ T3_STAGE='mcopy -o -i "$ROOT/disk.img" obj/intel/t4_probe.elf ::T4PROBE.BIN && mdir …' \
    bash tools/t3_tier.sh intel
  T4PROBE  BIN     61008 …          <- staging itself succeeded
  T3_intel: 77155 console lines, 223 FAIL lines, 28 user memory fault records
            across 18 processes, 85 programs exited, halted=1
$ T4_LEGA_OUT=obj/intel/t4_legA.out T4_SERIAL=obj/t3_intel.log bash tools/gate/t4_device.sh
  t4_device: FAIL — the probe never arrived in the guest (no 't4-probe-arrived'
    in obj/t3_intel.log after 120s).  This is a FAIL, not a SKIP (DoD clause 3).
  DEVICE_GATE_RC=1
```

Serial census: **0** `t4-probe-arrived`, **0** `^row=` lines in 77,155 console lines while 85
programs exited and the guest halted. So the six in-scope rows (`fcntl_getfl_rdwr`,
`fcntl_setfl_append`, `fcntl_getfl_after`, `fcntl_append_bit`, `ioctl_tiocgwinsz_entty`,
`ioctl_tcgets_entty`) exist on the Linux leg (they are in `03-legA.log`) and have **no** guest
counterpart to compare against.

## Why the staged probe cannot run (measured here, not inferred)

1. **The image's manifest is empty.** `mdir -i disk.img ::/` shows `WAVE CFG 0 bytes`, so
   `test_wave_loader` takes the literal-list default (`src/kernel/main.c:155` reads `WAVE.CFG`
   and requires `n > 0`); `T4PROBE.BIN` is not in that list, so a staged file is never dispatched.
2. **Naming it would not help at this revision.** `obj/intel/t4_probe.elf` is `ET_EXEC`, entry
   `0x401c18` — below the user window — and `src/kernel/elf_loader.c:124` refuses exactly that:
   *"elf loader: ET_EXEC linked below the user window; a -static-pie build is required"*.
3. **The static-pie path is not implemented either:** `grep -c R_X86_64_RELATIVE
   src/kernel/elf_loader.c` = **0**, so a `-static-pie` probe would enter and fault on its first
   GOT dereference. The repository's own `tests/fixtures/abi/WAVE.CFG` records this same chain in
   a comment and deliberately keeps `T4PROBE.BIN` out of the manifest.

GB-072 (manifest dispatch) is real — the loader reads the file and would dispatch what it names —
but the *fixture* it names is not dispatchable, and the staged image carries an empty manifest, so
the card's premise ("GB-072 landed, therefore a staged program dispatches") does not hold on
`46f19bb`.

## Chain to a usable gate (for the implementation ticket)

1. build the probe `-static-pie` (ET_DYN, no `PT_INTERP`);
2. implement `R_X86_64_RELATIVE` in `elf_loader.c` (and prove it with a fixture that dereferences
   a GOT entry);
3. stage a `WAVE.CFG` that names `T4PROBE.BIN` (the gate's `T3_STAGE` should do that itself — the
   current command stages only the ELF);
4. then this gate's step 4 has two legs to diff.

Until then, no verdict about the fcntl/ioctl rows is possible from the guest, and a GREEN here
would be a verdict from a gate that never ran its subject.
