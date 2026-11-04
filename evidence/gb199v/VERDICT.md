VERDICT: GREEN sha=b4f0ac43

**GREEN — GB-199-V @ `b4f0ac43` (PR #367, one commit on `90a672e6`; merge-tree vs `origin/main 90a672e6` clean → `027aef75`).** Independent gate on this VM: intel `MODE=test`, one-member wave, gcc `-pie` hello (`Type: DYN`, `PT_INTERP /lib64/ld-linux-x86-64.so.2`, `NEEDED libc.so.6`) staged with `LD64.SO` + `LIBC6.SO` + the real-name `libc.so.6`. All eight clauses quote the verifier's own serials, committed under `evidence/gb199v/` on `verify-gb199v`.

**Clause 1 — closure mapped, one line per DSO, deduped.**

```
elf loader: interpreter LD64.SO mapped
elf loader: DSO LIBC6.SO mapped
elf loader: PT_INTERP mapped: interp_base=0x1008000000 exe_base=0x1000000000 entry=0x1008023340
```

That is the alias-only staging (the author's evidenced form). With the real-name `libc.so.6` also staged (the gate names it), the resolver prefers the exact root name and the same single closure line reads `elf loader: DSO libc.so.6 mapped`; one `DSO` line either way — depth-first and deduped, as docs/linux-abi.md §14 states (`DSO <name>` one line per DSO). Both serials are in `evidence/gb199v/`.

**Clause 2 — startup stack, one line per auxv entry.**

```
auxv: AT_PAGESZ=0x1000
auxv: AT_PHDR=0x1000000040
auxv: AT_ENTRY=0x1000001060
auxv: AT_BASE=0x1008000000
```

`AT_BASE` equals `interp_base` (0x1008000000); `AT_ENTRY` is the executable entry (0x1000000000 + 0x1060), not the interpreter entry (0x1008023340). `AT_PHDR` is the executable's program-header address.

**Clause 3 — the guest executes ld.so; first stop named, no hang.**

```
[ARRIVED] LDHELLO.BIN rc>0
LDHELLO.BIN: error while loading shared libraries: libc.so.6: cannot open shared object file: No such file or directory
[KERNEL] Process 2: LDHELLO.BIN exited unexpectedly or gracefully.
[EXITPC] pid=2 last_user_pc=0x1008029C67 code=0x7F
System halt from CPU 1.
```

That error is glibc ld.so's own (it parsed the executable and the auxv, then failed to open the dependency); the kernel `[EXITPC]` names the PC and the run halts — no 30 s silent hang. My image stops one step earlier than the author's quoted `failed to map segment from shared object` (my compat open returns ENOENT for the dependency); both are ld.so's own named stop, which is what clause 3 requires, and the difference is quoted rather than hidden.

**Clause 4 — falsifier: the startup stack is what prevents the hole fault.** Kernel rebuilt with `GB199_NO_AUXV=1` (compile line carries `-DGB199_NO_AUXV`), same five staged files:

```
GB199-NOAUXV: startup stack omitted for the entered interpreter
[ARRIVED] LDHELLO.BIN rc>0
[KERNEL] pid=2 (LDHELLO.BIN) memory fault VA=0x0000001800000000 PC=0x0000001008021E18 rw=r  in=HOLE region=NONE ... -> killed
[EXITPC] pid=2 last_user_pc=0x0 code=0x4000000B
System halt from CPU 2.
```

The fault is exactly `VA=0x0000001800000000`, the hole above the stack top; the contrast proves the auxv block is doing the work.

**Clause 5 — regressions.**

```
MODE=so_test intel: HDYN: entered (dynamic) / HDYN: libc.so symbols resolved / HDYN: done
                    [EXITPC] pid=1 last_user_pc=0x1009040015 code=0x0 / System halt from CPU 7
MODE=so_test arm:   HDYN: entered (dynamic) / HDYN: libc.so symbols resolved / HDYN: done
                    [EXITPC] pid=1 last_user_pc=0x0 code=0x0 / System halt.
static W0PIE:       [ARRIVED] W0PIE.BIN rc>0
                    t4_w0_dispatch: PASS — W0PIE.BIN dispatched and carried PROC_FLAG_LINUX_ABI in the guest
```

**Clause 6 — suites/links.**

```
Checks run:    482
Checks failed: 0
TEST EXIT: 0

make ARCH=intel hobbyos.elf   rc=0
make ARCH=arm hobbyos.elf     rc=0
```

**Clause 7 — scope.** The native `so_test` boot above is the control: `/LDHOBBY.SO` is entered as the native dynamic path (`HDYN: entered (dynamic)`, no `PT_INTERP mapped` compat line). The compat entry is the only new path in this PR; ARM has no compat exec path (`#else lflags = ELF_LOAD_NATIVE`), so ARM parity for the entry is not applicable — its link and `so_test` regression passed.

**Clause 8 — evidence durable.** Raw serials (both staging variants, falsifier), tier logs, suite/link logs, the staging manifest and this verdict are committed at `evidence/gb199v/` on `origin/verify-gb199v` (force-added).

GREEN → GB-199 to Merge Gate; GB-199-V to Done.
