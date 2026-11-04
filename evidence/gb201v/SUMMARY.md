# GB-201-V — VERDICT: GREEN

- Graded: PR #369 head **0670e5f6e57894c6bf3f7b3f026ed500d1d87b51** (`GB-201: compat open resolves staged SONAMEs — ld.so maps libc and runs the hello`)
- Base for the contrast: `5efe9a40` (`git merge-base origin/main 0670e5f6`, includes GB-199, predates GB-201)
- Host: swarm-farm-3, QEMU 10.2.1; CI not consulted (policy 2026-10-26)
- Tier config: intel `MODE=test`, `T3_SMP=8`, `T3_MIN_PROGRAMS=1`, `T3_MIN_LINES=50`, `W0_GATE=0` — this is a directed one-member wave; the tier's full-wave floors and W0 corpus gate are for the 63-program wave, and `W0_GATE=0` is the runner's documented knob (`tools/t3_tier.sh:553`). The tier's halt/FAIL/kernel-fault checks still ran.

## Clause 1 — open resolves by SONAME (PASS)

`serial_ldhello.log:130`, on the image with `LDHELLO.BIN` + `LD64.SO` + `LIBC6.SO` staged and **no** `libc.so.6`:

```
open: /lib/x86_64-linux-gnu/libc.so.6 -> LIBC6.SO fd=3
```
fd=3 is a valid descriptor; the loader had already mapped the 8.3 file (`serial_ldhello.log:121`: `elf loader: DSO LIBC6.SO mapped`). Tier: `T3_intel: 141 console lines, 0 FAIL lines, 0 user memory fault records across 0 processes, 1 programs exited, halted=1` → `PASS ... 141 lines`, `T3_intel EXIT=0`.

## Clause 2 — unknown SONAME is still ENOENT (PASS)

Control probe: musl static-PIE (`Type: DYN`, no `PT_INTERP`, no `NEEDED`; `artifacts.txt`), calling `open("libc.so.6")` then `open("nosuch.so.9")`. `serial_probe.log`:

```
[ARRIVED] SNAME.BIN rc>0
open: libc.so.6 -> LIBC6.SO fd=3
[GB201] libc.so.6 open fd=3 errno=0
[GB201] nosuch.so.9 open fd=-1 errno=2 (ENOENT=2)
[EXITPC] pid=2 last_user_pc=0x100000517E code=0x0
System halt from CPU 3.
```
Tier PASS, 129 lines, EXIT=0. No wildcard: an unknown name is ENOENT.

Staging disclosure: the first probe boot had only `SNAME.BIN` on the regenerated image, so the table resolved to a file that was not there and printed `open: libc.so.6 -> LIBC6.SO fd=-2` / `[GB201] libc.so.6 open fd=-1 errno=2`. That is an image-staging gap, not a code result; it also shows the resolver does not fabricate an fd. The quoted run above stages `LIBC6.SO`.

## Clause 3 — ld.so maps and runs (PASS)

`serial_ldhello.log`:

```
elf loader: DSO LIBC6.SO mapped
[ARRIVED] LDHELLO.BIN rc>0
open: /lib/x86_64-linux-gnu/libc.so.6 -> LIBC6.SO fd=3
[LCOMPAT] ENOSYS nr=334 name=LDHELLO.BIN pid=2 rip=0x1008016F8E rdi=0x10802220A0
hello from glibc pie
[KERNEL] Process 2: LDHELLO.BIN exited unexpectedly or gracefully.
[EXITPC] pid=2 last_user_pc=0x10800F7198 code=0x0
System halt from CPU [HALT-CD] cpu=3 free=64 alloc=0 exited=0 running=0 blocked=0 other=0
```
`nr=334` is `rseq`, tolerated as ENOSYS; the hello runs and exits `code=0x0`. No hang (halt line reached inside the 300 s deadline).

## Clause 4 — pre-change contrast (PASS)

Same five-file staging, same one-member wave, base tree `5efe9a40`:

```
elf loader: DSO LIBC6.SO mapped
[ARRIVED] LDHELLO.BIN rc>0
LDHELLO.BIN: error while loading shared libraries: libc.so.6: cannot open shared object file: No such file or directory
[EXITPC] pid=2 last_user_pc=0x1008029C67 code=0x7F
System halt.
```
The difference is the open resolution, not a renamed file: `LIBC6.SO` was staged in both runs (`serial_contrast.log:121` shows the same `DSO LIBC6.SO mapped`). Tier PASS, 135 lines, EXIT=0.

## Clause 5 — regressions (PASS)

```
$ make MODE=so_test ARCH=intel run     rc=0
HDYN: done
[EXITPC] pid=3 last_user_pc=0x1009040015 code=0x0

$ make MODE=so_test ARCH=arm run       rc=0
HDYN: done
[EXITPC] pid=3 last_user_pc=0x0 code=0x0

$ make t4_w0_dispatch                 rc=0
=== t4_w0_dispatch: [ARRIVED] W0PIE.BIN x1, abi=0x1 x1 (boot rc=1) ===
[ARRIVED] W0PIE.BIN rc>0
t4_w0_dispatch: PASS — W0PIE.BIN dispatched and carried PROC_FLAG_LINUX_ABI in the guest
```

## Clause 6 — suites and links (PASS)

```
$ make host_tests
=== Test Results ===
Checks run:    482
Checks failed: 0
ALL APPS SUITE TESTS PASSED
TEST EXIT: 0
rc=0

$ make ARCH=intel hobbyos.elf   rc=0
$ make ARCH=arm   hobbyos.elf   rc=0
```

## Clause 7 — bounded table, x64-only (PASS)

`src/kernel/arch/x64/trap.c:242`:

```
} compat_sonames[] = {
  { "libc.so.6", "LIBC6.SO" },
  { "ld-linux-x86-64.so.2", "LD64.SO" },
};
```
Exact and bounded; no prefix/wildcard match. Only `src/kernel/arch/x64/trap.c` defines `compat_open`/`compat_sonames`; `src/kernel/arch/arm/trap.c` has no compat `open` case — the change is x64-only, as the card requires.

## Clause 8 — evidence durable (PASS)

Committed under `evidence/gb201v/` in branch `verify-gb201v` (force-added past the gitignore), including `SUMMARY.md`, the three raw serials, the three tier logs, the regression/suite/link logs, `artifacts.txt`, and `pr.diff`. Path in the verdict comment.
