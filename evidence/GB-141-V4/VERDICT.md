# GB-141-V4 (#332) — VERDICT: GREEN

**Graded:** `f31334e0` — verified as PR #282's head before building (`git ls-remote origin refs/pull/282/head` → `f31334e023844b798961e870bb725e15162feeee`; worktree detached at it, `git rev-parse --short HEAD` printed `f31334e0`).
**Card's one clause:** a compat process' output reaches the console — a `printf`-produced row quoted through `T3_LOG`, with the byte count matching what the program wrote.
**Not graded here (by the card's instruction):** `TIOCGWINSZ`/`TCGETS`/`F_GETFL` *values*, `fcntl(72)` sharing — all GB-019 (#20).

---

## The run

```
$ bash tools/gate/t4_gb019_ioctl.sh
=== t4_gb019_ioctl: booting intel guest with GB019-only manifest (GB-019-V7) ===
=== console rows (T3_LOG=…/evidence/GB-141-V4/t3_intel_gb019_f31334e0.log) ===
gb019 TIOCGWINSZ     req=0x5413 r=-1 errno=25
gb019 TCGETS         req=0x5401 r=-1 errno=25
gb019 F_GETFL        req=0x0003 r=32770 errno=0
gb019 F_SETFL        req=0x0004 r=0 errno=0
gb019 F_GETFL+       req=0x0003 r=34818 errno=0
gb019 unknown        req=0x9999 r=-1 errno=25
t4_gb019_ioctl: PASS — 5 dispatch rows + 3 negatives + fcntl(72) from a Linux-ABI proc (clauses 2/3/4)
t4_gb019_ioctl: PASS
gate_rc=0
```

Six `printf` rows, read from the guest console via the `T3_LOG` contract, from a member the loader dispatched as compat:

```
v2 loader: PID=2 entry=0x10000012B5 sp=0x1800000000 resident=26 tables=6 abi=0x1
```

`abi=0x1` = `PROC_FLAG_LINUX_ABI` — the rows are a Linux-ABI process' own output, not the kernel narrating for it.

## Byte count

The probe (`src/user/gb019_ioctl_probe.c`) emits only `printf` rows — there is no separate raw `write(1, …)` row in it, so the card's "if the probe emits both" branch does not apply.

Byte counts, three ways:

1. **Independent of the program** — the format string evaluated by Python:
   `'gb019 TIOCGWINSZ     req=0x5413 r=-1 errno=25\n'` → **46 bytes** (and 46 for the `TCGETS` and `unknown` rows).
2. **Guest console** — `grep -a 'TIOCGWINSZ' … | od -c` ends `… =   2   5  \r  \n` = 45 characters + the UART's CR + LF; the program's 46th byte is the `\n`, the CR is a transport artifact the serial adds, not a program byte.
3. **Same binary on real Linux (leg A)** — `./obj/intel/gb019_ioctl.elf` on this host prints the *identical* line for the three deterministic rows:
   ```
   row 1 IDENTICAL (46 bytes incl newline)   TIOCGWINSZ
   row 2 IDENTICAL (46 bytes incl newline)   TCGETS
   row 6 IDENTICAL (46 bytes incl newline)   unknown
   ```
   The other three rows differ only in the *values*, as designed (Linux answers `ENOTTY` to request 3/4; HobbyOS returns the stored `O_*`), never in whether they arrive.

## The gate can fail (non-vacuous)

Built a broken control — a musl static-PIE that opens `/dev/null`, prints nothing and returns 0 — and pointed the same gate at it:

```
$ GB019_ELF_PATH=/tmp/gb019_silent.elf bash tools/gate/t4_gb019_ioctl.sh
=== console rows (T3_LOG=…/t3_intel_gb019_brokencontrol.log) ===
t4_gb019_ioctl: FAIL — <5 dispatch rows
t4_gb019_ioctl: FAIL — TIOCGWINSZ non-tty != -ENOTTY
t4_gb019_ioctl: FAIL — TCGETS != -ENOTTY
t4_gb019_ioctl: FAIL — F_GETFL not a stored O_* value
t4_gb019_ioctl: FAIL — unknown request != -ENOTTY
t4_gb019_ioctl: FAIL — fcntl(72) F_GETFL not O_* (clause 4)
t4_gb019_ioctl: FAIL
BROKEN_CONTROL_RC=1
```

The control is not vacuous — it **loaded and ran**: `v2 loader: PID=2 … abi=0x1`, `[ARRIVED] GB019.BIN rc>0`,
`[EXITPC] pid=2 last_user_pc=0x1000001DEE code=0x0` (GB-142's new field showing a clean exit), `System halt`.
Zero rows reached the console, and the gate said so. The clause check is therefore paintable red, not
decorative.

**Noted, not graded:** the script's `--self-test` "absence" leg (line 52, `grep -q … && { :; }`) is a no-op — the
reviewer flagged it and I agree; I did not use it as evidence.

## Not graded here (as the card says)

The six rows' *values* belong to GB-019 (#20): `TIOCGWINSZ`/`TCGETS` → `-1/errno=25`, `F_GETFL` → `32770` then
`34818` after `O_NONBLOCK`, `unknown` → `-1/errno=25`, `fc=r…`. Reported there, not scored here.

## Artefacts

`t4_gb019_gate_summary.txt` · `t3_intel_gb019_f31334e0.log` · `legA_host_rows.txt` ·
`t4_gb019_broken_control_summary.txt` · `t3_intel_gb019_brokencontrol.log`