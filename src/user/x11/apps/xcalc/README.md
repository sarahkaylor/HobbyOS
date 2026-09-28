# xcalc — Skalculator for HobbyOS

A port of [Skalculator](https://github.com/fvcalderan/Skalculator), a
single-file RPN calculator written in pure Xlib by Felipe V. Calderan
(BSD-3-Clause, see `LICENSE.txt`).  Here it runs on HobbyOS's X11 support
library (`src/user/x11/`) inside a tile of the tiling desktop.  The
binary lands on the disk image as `/XCALC.BIN`.

## What changed in the port

- **Fixed-point core.**  HobbyOS user space has no floating point
  (`-mgeneral-regs-only`), so the original `double` stack became `int64`
  milliunits (`FIX = 1000`).  Values saturate at ±2,147,483.647 so that
  every operation stays inside `int64` without overflow checks in the hot
  path.  `%g` formatting became a fixed-point formatter (`1.5`, `0.2`,
  no exponent notation).
- **Tile-responsive layout.**  The original hard-coded a 200×370 window.
  The port recomputes its layout from the content rectangle the desktop
  assigns (`layout_compute()` in `main.c`): the keypad scales with the
  tile (caps at 120×64 per key so a full-screen tile does not become four
  enormous keys), the four text lines (three stack rows + the entry line)
  take 16% of the height, and a reflow (Expose/ConfigureNotify) redraws
  everything.
- **Keyboard support was added.**  The original is mouse-only.  The port
  accepts digits, `.`, `+ - * / ^` `%` (as `%`), Enter (ENTER/push),
  Backspace (C), Escape (AC), and `s`/`p`/`c`/`a`/`i`/`n` for
  SWAP/POP/C/AC/1-x/+−.
- **Entry point.**  Uses the crt0 `int main(int argc, char **argv)`
  convention; args and `-f FILE` (one line, via the HobbyOS libc) work as
  in the original.  `-h`/`--help` prints usage on the serial console.
- **Keypad grid.**  Reordered into a regular 4×6 grid (`buttons[6][4]` in
  `main.c`); the digit cluster keeps its conventional calculator order.

## Using it

Launch **XCALC** from the start menu.  Stack semantics are the original's
RPN: type a number, ENTER pushes it; an operator combines the stack head
with the entry and leaves the result in the entry line.

Keyboard | Mouse key | Action
---|---|---
digits `.` | 0-9 `.` | digit entry
Enter | ENTER | push entry onto the stack
`+ - * /` | `+ - * /` | arithmetic on head and entry
`^` | `^` | integer powers (fractional exponents show `E`)
`%` | `%` | percentage of the head (head × entry/100)
Backspace | C | clear the entry
Escape | AC | clear entry and stack
`s` `p` `i` `n` | SWAP POP 1/x +/− | swap, pop, reciprocal, sign

## Limitations

- Range: |value| ≤ 2,147,483.647 (saturates), no exponent notation
  (`1e3` parses as `1`).
- Division by zero and `0^-n` show `inf`; unsupported powers show `E`.
- The entry line holds 40 characters (the original's 256 is wider than
  any realistic tile).

## Building and testing

```bash
make obj/arm/xcalc.bin      # the app (ARM)
make xcalc_test_host        # host tests: core, layout, pixels, entry point
make xcalc_test             # QEMU acceptance: launch + mouse + keyboard + AC
```

The host test (`src/host/xcalc_test.c`) covers the fixed-point core, the
keypad behaviour, the real pixel output (read back from the framebuffer),
and the real entry point (forked child driven over pipes).  The QEMU test
(`run_xcalc_test.py`) boots the desktop, launches XCALC, clicks keys with
the mouse, types with the keyboard, and captures screenshots.
