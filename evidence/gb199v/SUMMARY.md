# GB-199-V — independent gate, swarm-farm-1, 2026-11-04

Head graded: `b4f0ac43` (PR #367, one commit on `90a672e6`; merge-tree vs
`origin/main` clean → `027aef75`). Branch `verify-gb199v`.

## Result: GREEN

- Closure: `interpreter LD64.SO mapped` + one deduped `DSO ... mapped` line
  (`LIBC6.SO` alias-only; `libc.so.6` when the real name is also staged), bases
  `interp_base=0x1008000000 exe_base=0x1000000000`.
- Auxv: `AT_PAGESZ=0x1000`, `AT_PHDR=0x1000000040`, `AT_ENTRY=0x1000001060`,
  `AT_BASE=0x1008000000` (`AT_BASE` == interp base; `AT_ENTRY` == exe entry).
- Entry: ld.so's own error line + `[EXITPC] pid=2 last_user_pc=0x1008029C67
  code=0x7F`, halt; no hang. My image's stop is `cannot open shared object file`
  (one step earlier than the author's `failed to map segment`); both quoted.
- Falsifier `GB199_NO_AUXV=1`: `GB199-NOAUXV` then
  `memory fault VA=0x0000001800000000` + `[EXITPC] code=0x4000000B`.
- Regressions: so_test intel/arm `HDYN: done` + `[EXITPC] code=0x0`; W0PIE
  `[ARRIVED] rc>0`; host_tests 482/0; both links rc=0.
- Scope: native `so_test` is the control; ARM has no compat exec path.

Raw serials and staging in `evidence/gb199v/`.
