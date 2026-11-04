VERDICT: GREEN sha=91933a31

**GREEN — GB-196-V @ `91933a31` (PR #365, one commit on `138a6874`; merge-tree vs `origin/main 65017cab` clean → `ca903b76`).** Independent gate on this VM, intel `MODE=test` one-member wave. All seven clauses quote the verifier's own serials, committed under `evidence/gb196v/` on `verify-gb196v`.

**Clause 1 — the compat boot names the PT_INTERP path.** With `LDHELLO.BIN` (gcc `-pie`, `readelf`: `Type: DYN`, `NEEDED libc.so.6`), `LD64.SO`, `LIBC6.SO`, one-member `::WAVE.CFG`:

```
elf loader: PT_INTERP /lib64/ld-linux-x86-64.so.2
```

**Clause 2 — the interpreter and the dependency set are named, and the miss is named.** Same boot:

```
elf loader: interpreter LD64.SO mapped
elf loader: DT_NEEDED LIBC6.SO
```

Same boot with `LIBC6.SO` not staged:

```
elf loader: PT_INTERP /lib64/ld-linux-x86-64.so.2
elf loader: interpreter LD64.SO mapped
elf loader: first missing DSO LIBC6.SO
```

**Clause 3 — pre-change control.** The three source files reverted to `138a6874`, rebuilt image, same four staged files:

```
elf loader: PT_INTERP present: dynamic binary
```

No path, no DSO name. The card predicted `is not the native /LDHOBBY.SO`; the measured compat refusal is the generic line above (the reviewer recorded this erratum too). The falsifier stands: the old line carries neither the path nor the dependency, the new one does.

**Clause 4 — the loader refuses after the report.** Both boots:

```
elf loader: compat interpreter is not entered (GB-196 stops at the named report)
[ARRIVED] LDHELLO.BIN rc<0
System halt from CPU [HALT-CD] ...
```

The program never enters `ld.so`; the tier's 55-program floor is expected to fail for a one-member manifest (`0 programs exited`, 126 console lines, 0 FAIL lines) — the halt and the `[ARRIVED]` line are the gate.

**Clause 5 — regressions.** `MODE=so_test` intel and arm, both:

```
[ARRIVED] HDYN.BIN rc>0
HDYN: entered (dynamic)
HDYN: libc.so symbols resolved
HDYN: done
[EXITPC] pid=1 last_user_pc=0x1009040015 code=0x0   (intel)
[EXITPC] pid=1 last_user_pc=0x0        code=0x0   (arm)
System halt from CPU 4 / CPU 1
```

Static-PIE compat (`make t4_w0_dispatch`, W0PIE-only manifest):

```
=== t4_w0_dispatch: [ARRIVED] W0PIE.BIN x1, abi=0x1 x1 (boot rc=1) ===
[ARRIVED] W0PIE.BIN rc>0
t4_w0_dispatch: PASS — W0PIE.BIN dispatched and carried PROC_FLAG_LINUX_ABI in the guest
```

**Clause 6 — suites and links.**

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0

make ARCH=intel hobbyos.elf   rc=0
make ARCH=arm hobbyos.elf     rc=0
```

**Clause 7 — evidence is durable.** Raw serials, staging manifest, tier logs, suite/link logs and this verdict are committed at `evidence/gb196v/` on `origin/verify-gb196v` (force-added; the branch is a verifier branch, not the PR). Staging is reproducible from `evidence/gb196v/STAGING.md`.

GREEN → GB-196 to Merge Gate; GB-196-V to Done.
