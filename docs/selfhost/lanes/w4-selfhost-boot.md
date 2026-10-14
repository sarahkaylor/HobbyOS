# w4-kernel — Mission A: MODE=selfhost serial-console boot (acceptance 5.2/5.3)

(Wave-4 kernel lane, `selfhost/w4-kernel`.)

## What shipped

- **`MODE=selfhost`** → `-DKERNEL_MODE_SELFHOST` (Makefile MODE plumbing,
  mirrors `jsc`/`webproc`; `selfhost_run` convenience target).
- **main.c boot branch**: boots ONLY `SELFHOST.BIN` (the in-OS acceptance
  build driver, src/user/selfhost/) with `argv[1]=/SRC/SELFHOST.RUN` — **no
  desktop**. The driver's fd 1/2 are unset, so the boot-wave console-fd
  fallback (fs.c `file_write` 0002 → `console_write_user` → uart) relays its
  stdout/stderr to the SERIAL console; the driver's own markers go out via
  `SYS_WRITE_CONSOLE` (also serial). Every `[SELFHOST] phase=` marker lands
  in the serial capture with zero QMP keystroke fragility.
- Clean halt: the driver exits → all processes drain → the boot core
  falls through `start_scheduler()` → `System halt.` + `halt()`.

## Evidence

See `w4-kernel-report.json` (gates.serial_selfhost_boot) and the raw serial
log(s) referenced there.

## Driver script + markers

`/SRC/SELFHOST.RUN` (src/user/selfhost/) is the driver's script:
`<phase>|<tool>|<args>` lines → `[SELFHOST] phase=<sX>-start/-done/-fail`,
`ACCEPTANCE-COMPLETE`/`FAILED-AT`/`ABORT`. This LANE booted the real script
(with a lane-scratch `Makefile.hobbyos` fixture — the real one is w4-src's)
so the driver→MAKE.BIN→serial relay chain is exercised end to end.
