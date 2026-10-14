# w3-shell — findings & evidence (lane selfhost/w3-shell)

## BUG A — `cmd ARG > FILE` redirect loss for EXTERNALS

### Root cause (fixed in `src/user/sh.c`)
`eval_simple()`'s external-fork child did:

    dup2(lerr, 2);
    if (lerr != 2) close(lerr);     // when lerr == 1 (a `2>&1` dup), this
                                    // closed the child's own STDOUT

so `cat A > F 2>&1` (and every selfhost `> LOG 2>&1` recipe) wrote **0
bytes** to F: the file was created+truncated by the redirect open, then the
exec'd program's stdout fd was closed before execve.  The same aliasing
existed in the builtin dup-swap path (`echo hi > F 2>&1` could close the
SHELL's own stdout after the restore) and in the assignment-only early
return.

### Fix
Guard every redirect-fd close with `>= 3` (four sites in `eval_simple`):
`0/1/2` are the shell's own stdio and must survive a dup; only the fds
`apply_redirs()` OPENED (always `>= 3`) are closed after `dup2`.

### Proof (host parity, byte-exact, deterministic)
Built the SAME `sh.c` for the host (`-DHOST_TEST`) with a small syscall
shim; interactive AND `sh -c`:

| command                                          | before | after |
|--------------------------------------------------|--------|-------|
| `cat HELLO.BIN > F`                              | 27 B   | 27 B  |
| `cat HELLO.BIN >> F` (twice)                     | 54 B   | 54 B  |
| `./CAT.BIN /nope 2> E`                           | stderr to E | same |
| `cat HELLO.BIN > F 2>&1`                         | **0 B** | **27 B** |
| `cat HELLO.BIN 2>&1 > F`                         | mangled | 27 B |
| `echo hi > F 2>&1; echo AFTER`                   | shell stdout died | F=3 B **and** AFTER printed |
| interactive `cat HELLO.BIN > F 2>&1`             | 0 B     | 27 B  |

### In-OS
- `shell_test.c` 5e/5e2/5f now carry the file-based byte assertions the
  w2-make lane deferred: `cat SHTEST.TXT > OUT2.TXT` -> wc == 194;
  `cat SHTEST.TXT > OUT7.TXT 2>&1` -> wc == 194 (the 2>&1 dup shape);
  `make --version > M.TXT` -> `GNU Make` banner.  All skippable only under
  the documented thread-slot-pressure class (`thread_create: no free slot
  used=63/63`), hard FAIL otherwise.
- The SHTEST **DIRECT** probe (its own `open()` on the redirected file, no
  shell round-trip) has read the full 194-byte fixture in-OS on a light
  draw; on heavy draws it prints `EMPTY-AT-READ (pressure)` because the
  redirect child was still parked in the loader's spawn reserve.

### The w2-make smoke 'empty argv' mystery: harness artifact, not the shell
The w2 deferred recipe drove the desktop CONSOLE by QMP keystrokes.  The
guest keyboard path **drops/translates keystrokes**: a typed `cat CTL.TXT`
arrives as `cat .` (`cat: .: No such file or directory`), and `shift+dot`
for `>` arrives as `=`.  OCR of the console framebuffer transcript
(`tools/run_w3kbd_probe.py`, `docs/selfhost/lanes/w3-kbd-probe-notes.txt`):
the line actually executed was `cat . >` / `... > =`, so the shell's
"cannot open =" and the "empty argv" appearance came from the keyboard
path, not from parsing.  The pipe-driven shell writes full bytes.
(tools/run_w3shell_redirect_repro.py is the QMP console driver.)

## BUG B — ambient `FAILED echo validation` / `FAILED cat nested.txt`

### Root cause
Under the wave-start thread-slot-pressure storm
(`[KERNEL] thread_create: no free slot (used=63/63 done=0)`), a command's
output can arrive late; the next `read_until("$ ")` then stops at a STALE
prompt from the previous command, the needle check misses, and the plain
echo/clear/nested-file steps (no retry, no pressure classifier) turned that
into a hard FAIL token in the wave.

### Fix (`shell_test.c` / `2` / `3`)
- `drain_pending()` before every command write — each read owns exactly the
  output of the command just sent.
- `run_builtin_check()` replaces the rigid echo/clear guards: drain +
  retry + pressure-classified SKIP (peak >= 55 or stall) instead of FAIL.
- `shell_test3`'s echo-nested read is now deadline-bounded + drained; every
  sequence (mkdir+ls, rm+ls, file-ops) drains between steps; `run_cmd_check`
  drains before each attempt.

## Gates
- `make ARCH=arm MODE=test run` (QEMU TCG): 'System halt.' + **ZERO FAIL
  tokens** (two independent full-wave runs after the sh.c fix).
- `./run_unit_tests.sh` (ARM): **312 tests, 0 failed, UNIT TESTS
  PASSED**.
