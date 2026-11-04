# GB-196-V — independent gate, swarm-farm-1, 2026-11-04

Head graded: `91933a31` (PR #365, one commit on `138a6874`; merge-tree vs
`origin/main 65017cab` clean → `ca903b76`). Branch `verify-gb196v`.

## Result: GREEN

- PT_INTERP path, mapped interpreter and DT_NEEDED/first-missing pair measured in
  the verifier's own boot with a gcc `-pie` hello: `Type: DYN`,
  `PT_INTERP /lib64/ld-linux-x86-64.so.2`, `NEEDED libc.so.6`, staged as
  `LDHELLO.BIN` + `LD64.SO` + `LIBC6.SO` (see `STAGING.md`).
- Pre-change control: the three source files reverted to `138a6874` print the
  generic `elf loader: PT_INTERP present: dynamic binary` — no path, no DSO.
- Both boots refuse after the report: `compat interpreter is not entered`,
  `[ARRIVED] LDHELLO.BIN rc<0`.
- Regressions: `MODE=so_test` intel/arm → `HDYN: done` + `[EXITPC] code=0x0`;
  static `W0PIE.BIN` → `[ARRIVED] rc>0`, `abi=0x1`.
- `make host_tests` 482/0; both arch links rc=0.

The tier's 55-program floor fails on a one-member manifest (0 exited) — expected;
the `[ARRIVED]`/halt lines are the gate. Raw serials and staging are in
`evidence/gb196v/`.
