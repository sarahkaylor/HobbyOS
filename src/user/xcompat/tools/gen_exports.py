#!/usr/bin/env python3
"""Regenerate the frozen export interface F4 (src/user/xcompat/EXPORTS.md).

Two independent measurements, both from real artifacts:

  (a) the *required* set: every symbol Unity's embedded SDL2 asks libX11
      for.  SDL resolves Xlib through dlsym(), so the names are string
      constants in UnityPlayer.so; each one is kept only if the host's real
      libX11.so.6 actually exports it (which drops the binary noise that
      also matches ^X[A-Z]...).

  (b) the *implemented* set: whatever src/user/xcompat/libx11.c defines
      with XAPI, split into IMPL (touches the display/protocol state) and
      STUB (returns a constant), with the constant quoted from the source.

Usage:
  tools/gen_exports.py --game /mnt/fast/games/steamapps/common/RimWorld \
                       --libx11 /usr/lib/x86_64-linux-gnu/libX11.so.6

Outputs (all checked in, so the tree builds without the game):
  src/user/xcompat/required-libx11.txt   one symbol per line, sorted
  src/user/xcompat/required-libx11.h     the same list as a C array
  src/user/xcompat/exports-table.md      the generated markdown tables
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
XCOMPAT = os.path.dirname(HERE)          # .../src/user/xcompat

# Symbols whose implementation is real but not complete; the reason is the
# contract, so it is maintained here rather than guessed from the source.
PARTIAL = {
    "XDrawString": "fills the text box with the GC background and draws the "
                   "baseline: correct geometry, no glyphs (no font machinery "
                   "under compat)",
    "XDrawImageString": "same as XDrawString",
    "XUnmapWindow": "the protocol cannot retract a pixel window: the surface "
                    "stays up and stops receiving events",
    "XChangeProperty": "accepted and dropped; no property list is kept",
    "XGetWindowProperty": "answers None/Success; nothing round-trips except "
                          "WM hints (XGetWMHints/XGetWMNormalHints)",
    "XMoveWindow": "the desktop owns window placement; no-op",
    "XResizeWindow": "the desktop owns window size; no-op",
    "XMoveResizeWindow": "the desktop owns geometry; no-op",
    "XTranslateCoordinates": "window<->root is exact, window<->window is only "
                             "approximate (there are no child windows)",
    "XSetInputFocus": "accepted; the desktop routes key input to the focused "
                      "pixel window, there is no focus protocol",
    "XGetKeyboardMapping": "returns the shim's static 2-keysym-per-keycode map",
    "XAllocColor": "TrueColor: the pixel is the colour; no colour map exists",
    "XStringListToTextProperty": "copies into one static 1 KiB buffer",
}

STATEFUL = re.compile(
    r"d->|send_raw|shadow_blit|shadow_flush|dirty_add|queue_event|put_uint|"
    r"fb_probe|wait_event|pump_nonblock|input_bytes|input_settle|"
    r"set_geometry|key_event|mouse_event|"
    r"xc_(queue|dequeue|peek|pending|intern|atom_name|clip|content_to_screen|"
    r"dirty|root_to_window|window_to_root|keysym|keycode|lookup_string|init)|"
    r"xs_(write|read|open|close|mmap|ioctl|syscall6)|"
    r"XStoreName|XFillRectangle|XDrawLine|XUnmapWindow")


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def required_set(game_dir, libx11):
    so = os.path.join(game_dir, "UnityPlayer.so")
    if not os.path.exists(so):
        sys.exit("gen_exports: %s not found (pass --game)" % so)
    strings = run(["strings", "-a", so]).splitlines()
    # Xlib entry points SDL dlsyms; the ^X[A-Z] shape also matches binary
    # noise, so the host's real libX11 decides what is a symbol.
    candidates = {s for s in strings if re.fullmatch(r"X[A-Z][A-Za-z0-9_]*", s)}
    exported = set()
    for line in run(["nm", "-D", "--defined-only", libx11]).splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "W", "i"):
            exported.add(parts[2].split("@")[0])
    return sorted(candidates & exported)


def alsa_set(game_dir):
    so = os.path.join(game_dir, "UnityPlayer.so")
    strings = run(["strings", "-a", so]).splitlines()
    return sorted({s for s in strings if re.fullmatch(r"snd_[a-z_0-9]+", s)})


def implemented(src):
    """(name, kind, value) for every XAPI function defined in `src`."""
    text = open(src).read()
    out = []
    # Split on XAPI so each chunk is one definition's signature + body.
    for chunk in text.split("XAPI ")[1:]:
        m = re.match(r"[^\n(]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(", chunk)
        if not m:
            continue
        name = m.group(1)
        if name.startswith("__"):        # the #define XAPI line itself
            continue
        body_start = chunk.find("{")
        if body_start < 0:
            continue
        depth, i = 0, body_start
        while i < len(chunk):
            if chunk[i] == "{":
                depth += 1
            elif chunk[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = chunk[body_start:i + 1]
        kind = "IMPL" if STATEFUL.search(body) else "STUB"
        value = ""
        if kind == "STUB":
            r = re.search(r"return\s+([^;]+);", body)
            if r:
                value = " ".join(r.group(1).split())
        out.append((name, kind, value))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("RW_GAME_DIR", ""))
    ap.add_argument("--libx11",
                    default="/usr/lib/x86_64-linux-gnu/libX11.so.6")
    ap.add_argument("--no-write", action="store_true")
    args = ap.parse_args()

    libx11 = os.path.realpath(args.libx11)
    req = required_set(args.game, libx11)
    alsa = alsa_set(args.game) if args.game else []
    impl = implemented(os.path.join(XCOMPAT, "libx11.c"))

    print("required libX11 symbols : %d" % len(req))
    print("exported by the DSO     : %d (%d IMPL, %d STUB)"
          % (len(impl), sum(1 for _, k, _ in impl if k == "IMPL"),
             sum(1 for _, k, _ in impl if k == "STUB")))
    missing = [s for s in req if s not in {n for n, _, _ in impl}]
    print("required but missing    : %d %s" % (len(missing), " ".join(missing)))
    if alsa:
        print("required ALSA symbols   : %d" % len(alsa))

    if args.no_write:
        return 0 if not missing else 1

    with open(os.path.join(XCOMPAT, "required-libx11.txt"), "w") as f:
        f.write("\n".join(req) + "\n")

    if alsa:
        with open(os.path.join(XCOMPAT, "required-alsa.txt"), "w") as f:
            f.write("\n".join(alsa) + "\n")
        with open(os.path.join(XCOMPAT, "required-alsa.h"), "w") as f:
            f.write("/* generated by tools/gen_exports.py -- do not edit.\n"
                    " * The snd_* symbols Unity's FMOD backend dlsyms from\n"
                    " * libasound.so.2 (measured from UnityPlayer.so). */\n")
            f.write("#ifndef HOBBYOS_XCOMPAT_REQUIRED_ALSA_H\n"
                    "#define HOBBYOS_XCOMPAT_REQUIRED_ALSA_H\n\n")
            f.write("#define XCOMPAT_ALSA_COUNT %d\n\n" % len(alsa))
            f.write("static const char *const xcompat_alsa_required[] = {\n")
            for s in alsa:
                f.write('  "%s",\n' % s)
            f.write("};\n\n#endif\n")

    with open(os.path.join(XCOMPAT, "required-libx11.h"), "w") as f:
        f.write("/* generated by tools/gen_exports.py -- do not edit.\n"
                " * Every symbol Unity's embedded SDL2 dlsyms from libX11.so.6\n"
                " * (measured: strings UnityPlayer.so  INTERSECT the host\n"
                " * libX11.so.6 dynamic symbol table).  This is the F4 contract:\n"
                " * the compat DSO must export all of them. */\n")
        f.write("#ifndef HOBBYOS_XCOMPAT_REQUIRED_LIBX11_H\n"
                "#define HOBBYOS_XCOMPAT_REQUIRED_LIBX11_H\n\n")
        f.write("#define XCOMPAT_REQUIRED_COUNT %d\n\n" % len(req))
        f.write("static const char *const xcompat_required[] = {\n")
        for s in req:
            f.write('  "%s",\n' % s)
        f.write("};\n\n#endif\n")

    # The status tables.  IMPL first (what the next lane can rely on), then
    # the stubs with their literal return value.
    impl.sort(key=lambda r: r[0])
    status = {n: (k, v) for n, k, v in impl}
    lines = []

    def status_of(n):
        kind, value = status.get(n, ("MISSING", ""))
        if kind == "IMPL" and n in PARTIAL:
            return "PARTIAL", PARTIAL[n]
        if kind == "STUB":
            return "STUB", "returns `%s`" % (value or "0")
        return kind, ""

    lines.append("### (a) libX11.so.6 - the 129 symbols SDL requires\n")
    lines.append("| symbol | status | detail |")
    lines.append("|---|---|---|")
    for n in req:
        k, note = status_of(n)
        lines.append("| `%s` | %s | %s |" % (n, k, note))
    lines.append("")

    lines.append("### (b) libX11.so.6 - exported beyond the required set\n")
    lines.append("| symbol | status | detail |")
    lines.append("|---|---|---|")
    for name, kind, value in impl:
        if name in req:
            continue
        k, note = status_of(name)
        lines.append("| `%s` | %s | %s |" % (name, k, note))
    lines.append("")

    if alsa:
        lines.append("### (c) libasound.so.2 - the 19 symbols FMOD requires\n")
        lines.append("| symbol | behaviour |")
        lines.append("|---|---|")
        for n in alsa:
            if n == "snd_device_name_hint":
                b = "returns 0 with a NULL-*terminated* empty array"
            elif n == "snd_pcm_open":
                b = "returns -ENODEV, handle NULL"
            elif n == "snd_pcm_close":
                b = "returns 0 (idempotent)"
            elif n == "snd_strerror":
                b = "static text, `-19` -> \"No such device\""
            elif n == "snd_pcm_hw_params_sizeof":
                b = "returns 256 (the opaque params block size)"
            elif n == "snd_pcm_hw_params_any":
                b = "zeroes the block, returns 0"
            else:
                b = "returns -ENODEV"
            lines.append("| `%s` | %s |" % (n, b))
        lines.append("")
    with open(os.path.join(XCOMPAT, "exports-table.md"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # EXPORTS.md = hand-written prose + the tables just generated.  Keeping
    # the prose in its own file means this script can rewrite the whole
    # document without ever touching the reasoning.
    prose_path = os.path.join(XCOMPAT, "EXPORTS.prose.md")
    if os.path.exists(prose_path):
        prose = open(prose_path).read()
        with open(os.path.join(XCOMPAT, "EXPORTS.md"), "w") as f:
            f.write(prose)
            f.write("\n".join(lines) + "\n")
    print("wrote required-libx11.{txt,h} and exports-table.md")
    return 0 if not missing else 1


if __name__ == "__main__":
    sys.exit(main())
