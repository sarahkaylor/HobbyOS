# GB-141-V — RED at 89ac644a (verifier run, 2026-10-31)

Graded head **`89ac644a`** (PR #275, `ticket/19-ioctl-v7`), base `6b91bad`, 7 files +271/−1.
**VERDICT: RED sha=89ac644a**

The probe is dispatched, carries `abi=0x1`, and runs — and **not one `write(1)` byte reaches the
console**. That is the clause the card exists for.

## Clause 1 — FAIL. No rows, no bytes

```
$ bash tools/gate/t4_gb019_ioctl.sh
=== t4_gb019_ioctl: booting intel guest with GB019-only manifest (GB-019-V7) ===
=== console rows (T3_LOG=…/obj/t3_intel.log) ===
t4_gb019_ioctl: FAIL — <5 dispatch rows
t4_gb019_ioctl: FAIL — TIOCGWINSZ non-tty != -ENOTTY
t4_gb019_ioctl: FAIL — TCGETS != -ENOTTY
t4_gb019_ioctl: FAIL — F_GETFL not a stored O_* value
t4_gb019_ioctl: FAIL — unknown request != -ENOTTY
t4_gb019_ioctl: FAIL — fcntl(72) F_GETFL not O_* (clause 4)
t4_gb019_ioctl: FAIL
```

The guest console, verbatim:

```
[WAVE-MANIFEST] GB019.BIN
v2 loader: PID=2 entry=0x100000124C sp=0x1800000000 resident=26 tables=6 abi=0x1
[ARRIVED] GB019.BIN rc>0
[EXITPC] pid=2 last_user_pc=0x1000005607
```

`grep -ac "gb019 .*req=" obj/t3_intel.log` → **0**.

## Clause 2 — the mechanism is named in the source, and it is not enough

```
src/user/gb019_ioctl_probe.c:53:  setvbuf(stdout, NULL, _IONBF, 0);
```

and the route the card names exists on this head:

```
src/kernel/fs.c:1808   if (fd == 1 || fd == 2)
src/kernel/fs.c:1809     return console_write_user(cur, buf, size);
```

with `open_fds[i] = -1` set at process creation (`process.c:1109`, `:4438`), so fd 1 should take
that branch. It does not produce a byte.

## Clause 3 — FAIL, as a consequence

No `req=` row exists in this run, so there is no GB-019 row to quote. (The gate's own row from a
hypothetical passing run is in the self-test, not in a guest.)

## Clause 4 — the falsifier cannot move anything

Removing `setvbuf` cannot make the console *more* silent than zero rows. The verdict is already at
the failing state with the fix applied, so the falsifier has no direction to move — which is the
same shape as GB-138-V4's constant verdict: not "unverified", but **unverifiable until the rows
appear at all**.

## Clause 5 — suites

```
groom_test EXIT=0 · check_stale_test EXIT=0 · drift_check_test EXIT=0
bash tools/gate/t4_gb019_ioctl.sh --self-test EXIT=0   (4/4)
```

## Where the process died

`last_user_pc=0x1000005607` resolves inside **`_Exit`**:

```
$ llvm-addr2line -f -e obj/intel/gb019_ioctl.elf 0x5607
_Exit
$ llvm-nm -n obj/intel/gb019_ioctl.elf | awk '$1<="0000000000005607"' | tail -2
00000000000055d0 T strerror
00000000000055f0 T _Exit
```

So `main` returned (`return 0`), `exit` ran, and the process was killed inside `_Exit` — the
GB-113 signature ("a child that exits 0 is observed as killed by a signal"), not a crash in the
probe's logic. The rows were therefore written *and dropped*, or never emitted; either way they are
not on the console, and the PR's claimed mechanism (unbuffered stdout ⇒ bytes reach `write(1)` ⇒
WK-1 route) is not what the run shows.

The ELF carries **11 `R_X86_64_RELATIVE`** relocations (`readelf -r`), which is the shape the GB-137
plan item is about; I am not claiming that as the cause — only recording it, because a static-PIE
probe that dies on exit is exactly the case where a missing relocation application would not
announce itself.

## Build deviation, named so nobody has to find it later

`make disk.img ARCH=intel` **fails at this head** before any of my changes:

```
configure: error: libpng support requested but library not found
make[1]: *** […/third_party/x11/build/intel/src/freetype/builds/unix/detect.mk:89: setup] Error 1
make: *** [Makefile:2299: x11-staged] Error 1
```

so the wave above ran on `make disk.img ARCH=intel DESKTOP=hobby` (DISK_RC=0). The probe is built
by system `musl-gcc -static-pie` (`Makefile:3810`) and the kernel `file_write`/`console_write_user`
path is not affected by `DESKTOP`, so I judge the RED attributable to the PR's claim — but the
deviation is real and the author should reproduce with a default image before arguing the point.

## Verdict

**RED sha=89ac644a** — clause 1 fails with zero rows on a run where the member is dispatched and
`abi=0x1`; clause 3 has no row to quote; clause 4's falsifier cannot move a verdict that is already
at the failing state. Suites pass.

Separate infra finding for the caretaker, not this card: `x11-staged` cannot configure freetype at
`6b91bad` ("libpng support requested but library not found"), so a default `make disk.img` fails on
this tree. That will bite the next gate that does not pass `DESKTOP=hobby`.