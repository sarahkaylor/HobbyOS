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
    "XUnmapWindow": "the protocol cannot retract a pixel window: the surface "
                    "stays up and stops receiving events",
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

# Full implementations whose behaviour is worth a sentence in the table (the
# status column alone would only say IMPL).
IMPL_NOTE = {
    "XDrawString": "the 8x8 ASCII font the native Xlib uses (`font8x8.h`, "
                   "vendored from `src/user_include/graphics/font.h`): `y` is "
                   "the baseline, advance 8 px, GXxor honoured through "
                   "`put_px`. M3 gate renders legible text.",
    "XDrawImageString": "`XDrawString` without the pre-fill (ImageString "
                        "semantics are close enough: the font cell is opaque)",
    "XChangeProperty": "stores name/type/format/data in a 12-slot property "
                       "list (`_NET_WM_NAME`, `_NET_WM_PID`, `WM_PROTOCOLS`, "
                       "`WM_CLASS` all round-trip); a title written this way "
                       "also drives the desktop frame (`ESC ] T`).",
    "XGetWindowProperty": "reads the property list back (malloc'd copy, as "
                          "Xlib returns); only the first 512 bytes of a "
                          "property are retained.",
    "XSetWMProtocols": "records the list, stores it as WM_PROTOCOLS/ATOM/32 "
                       "and arms the WM_DELETE_WINDOW path: the desktop's "
                       "in-band `ESC [ D ~` close becomes the ClientMessage SDL "
                       "reads (see XNextEvent).",
    "XNextEvent": "polls the desktop pipe, drains queued events, and turns the "
                  "desktop's close into a WM_PROTOCOLS/WM_DELETE_WINDOW "
                  "ClientMessage when the client registered one via "
                  "XSetWMProtocols; a client that never did keeps the old "
                  "exit-the-process close.",
}

STATEFUL = re.compile(
    r"d->|send_raw|shadow_blit|shadow_flush|dirty_add|queue_event|put_uint|"
    r"fb_probe|wait_event|pump_nonblock|input_bytes|input_settle|"
    r"set_geometry|key_event|mouse_event|"
    r"xc_(queue|dequeue|peek|pending|intern|atom_name|clip|content_to_screen|"
    r"dirty|root_to_window|window_to_root|keysym|keycode|lookup_string|init)|"
    r"xs_(write|read|open|close|mmap|ioctl|syscall6)|"
    r"XStoreName|XFillRectangle|XDrawLine|XUnmapWindow|XSync")


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


def pulse_set(game_dir):
    """Every pa_* name Unity's FMOD/SDL audio path resolves.

    Unlike the Xlib set this needs no host-library intersection: the ^pa_
    shape does not collide with binary noise, and the measured list is
    exactly the union of libpulse-simple's public surface and the async
    subset SDL's PulseAudio backend uses (see EXPORTS.prose.md §2.5).
    """
    so = os.path.join(game_dir, "UnityPlayer.so")
    strings = run(["strings", "-a", so]).splitlines()
    return sorted({s for s in strings if re.fullmatch(r"pa_[a-z_0-9]+", s)})


def udev_set(game_dir, libudev):
    """Every udev_* name SDL's joystick/udev backend resolves at runtime.

    Two filters, both necessary.  The name shape (`^udev_[...]`) drops the
    binary's own error strings, which also start with udev_:

        udev_new() failed
        udev_enumerate_new() failed
        udev_monitor_new_from_netlink() failed

    ...and the host's real libudev.so.1 decides what is actually an entry
    point, exactly like the libX11 set.  Without the host library (a CI box
    with no libudev) the shape filter alone is used; the three strings above
    are the only measured near-misses, and they contain a space and a
    parenthesis, so the shape filter already rejects them.
    """
    so = os.path.join(game_dir, "UnityPlayer.so")
    strings = run(["strings", "-a", so]).splitlines()
    candidates = {s for s in strings if re.fullmatch(r"udev_[a-z0-9_]+", s)}
    if libudev and os.path.exists(libudev):
        exported = set()
        for line in run(["nm", "-D", "--defined-only", libudev]).splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1] in ("T", "W", "i"):
                exported.add(parts[2].split("@")[0])
        candidates &= exported
    return sorted(candidates)


# The contract of the pulse shim, one row per symbol.  Keyed by name so the
# generated table cannot drift from the prose; the fallback documents itself.
PULSE_BEHAVIOUR = {
    "pa_simple_new": "returns NULL; `*error` = `PA_ERR_CONNECTIONREFUSED` (6), "
                     "`PA_ERR_INVALID` (3) if the sample spec is malformed",
    "pa_simple_free": "no-op (NULL-safe: FMOD's cleanup calls it after a failed new)",
    "pa_simple_read": "returns `-PA_ERR_INVALID`",
    "pa_simple_write": "returns `-PA_ERR_INVALID`",
    "pa_simple_drain": "returns `-PA_ERR_INVALID`",
    "pa_simple_flush": "returns `-PA_ERR_INVALID`",
    "pa_simple_get_latency": "returns `(pa_usec_t)-1` (the documented unknown)",
    "pa_strerror": "static text, `6` -> \"Connection refused\"",
    "pa_get_library_version": "returns `\"17.0.0\"` (the ABI's measured era)",
    "pa_channel_map_init_auto": "fills the caller's map with the default "
                                "layout, returns it (NULL for 0 channels or >32)",
    "pa_context_new": "returns a sentinel handle (non-NULL: constructing a "
                      "context does not contact the server in the real library)",
    "pa_context_connect": "returns `-PA_ERR_CONNECTIONREFUSED`",
    "pa_context_get_state": "returns `PA_CONTEXT_FAILED` (5)",
    "pa_context_errno": "returns `PA_ERR_CONNECTIONREFUSED` (6)",
    "pa_context_disconnect": "no-op",
    "pa_context_unref": "no-op",
    "pa_context_get_server_info": "returns NULL (no operation object)",
    "pa_context_get_sink_info_list": "returns NULL (no operation object)",
    "pa_context_get_source_info_list": "returns NULL (no operation object)",
    "pa_operation_get_state": "returns `PA_OPERATION_CANCELLED` (2), NULL-safe",
    "pa_operation_unref": "no-op (NULL-safe)",
    "pa_mainloop_new": "returns a sentinel handle (non-NULL)",
    "pa_mainloop_get_api": "returns a complete inert vtable of 12 callbacks",
    "pa_mainloop_iterate": "returns `-1` immediately (never blocks, even with "
                           "`block=1`): the strongest stop signal for a "
                           "READY-wait loop",
    "pa_mainloop_free": "no-op",
}

# The libudev shim's contract, one row per measured symbol.  Everything in
# this table is "zero devices": what matters is that the *answer* is the one
# SDL's error paths already handle, never a failure to construct an object.
UDEV_BEHAVIOUR = {
    "udev_new": "returns a non-NULL sentinel handle (a caller treats NULL as "
                "\"no udev available\" and gives up)",
    "udev_unref": "no-op, returns NULL, NULL-safe",
    "udev_enumerate_new": "returns a non-NULL sentinel enumerator, even for a "
                          "NULL udev handle",
    "udev_enumerate_unref": "no-op, returns NULL, NULL-safe",
    "udev_enumerate_add_match_subsystem": "returns 0 (filter accepted; nothing to match)",
    "udev_enumerate_add_match_property": "returns 0 (filter accepted; nothing to match)",
    "udev_enumerate_scan_devices": "returns 0 — success with zero devices",
    "udev_enumerate_get_list_entry": "returns NULL: the empty list",
    "udev_list_entry_get_next": "returns NULL (NULL-safe)",
    "udev_list_entry_get_name": "returns NULL (NULL-safe)",
    "udev_device_new_from_syspath": "returns NULL: no such device",
    "udev_device_new_from_devnum": "returns NULL: no such device",
    "udev_device_unref": "no-op, returns NULL, NULL-safe",
    "udev_device_get_action": "returns NULL: no event",
    "udev_device_get_devnode": "returns NULL (NULL-safe)",
    "udev_device_get_devnum": "returns 0 (`dev_t` is 8 bytes on x86-64 glibc)",
    "udev_device_get_parent_with_subsystem_devtype": "returns NULL (NULL-safe)",
    "udev_device_get_property_value": "returns NULL (NULL-safe)",
    "udev_device_get_subsystem": "returns NULL (NULL-safe)",
    "udev_device_get_sysattr_value": "returns NULL (NULL-safe)",
    "udev_monitor_new_from_netlink": "returns a non-NULL sentinel monitor and "
                                     "creates the never-signalling pipe",
    "udev_monitor_enable_receiving": "returns 0 (enabled; nothing will arrive)",
    "udev_monitor_filter_add_match_subsystem_devtype": "returns 0",
    "udev_monitor_get_fd": "the pipe read end (never readable), or -1 if "
                           "`pipe2` failed",
    "udev_monitor_receive_device": "returns NULL: the documented \"nothing "
                                   "pending\" answer, never an error",
    "udev_monitor_unref": "no-op, returns NULL; the pipe fds stay open",
}


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


def pulse_exported(src):
    """pa_* names defined with PULSEAPI in pulse_null.c (the DSO's surface)."""
    text = open(src).read()
    return sorted(set(re.findall(r"PULSEAPI\s+[A-Za-z_][A-Za-z0-9_ *]*?\b"
                                 r"(pa_[a-z_0-9]+)\s*\(", text)))


def udev_exported(src):
    """udev_* names defined with UDEVAPI in udev_null.c (the DSO's surface)."""
    text = open(src).read()
    return sorted(set(re.findall(r"UDEVAPI\s+[A-Za-z_][A-Za-z0-9_ *]*?\b"
                                 r"(udev_[a-z0-9_]+)\s*\(", text)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("RW_GAME_DIR", ""))
    ap.add_argument("--libx11",
                    default="/usr/lib/x86_64-linux-gnu/libX11.so.6")
    ap.add_argument("--libudev",
                    default="/usr/lib/x86_64-linux-gnu/libudev.so.1")
    ap.add_argument("--no-write", action="store_true")
    args = ap.parse_args()

    libx11 = os.path.realpath(args.libx11)
    req = required_set(args.game, libx11)
    alsa = alsa_set(args.game) if args.game else []
    pulse = pulse_set(args.game) if args.game else []
    udev = udev_set(args.game, args.libudev) if args.game else []
    impl = implemented(os.path.join(XCOMPAT, "libx11.c"))
    pulse_impl = pulse_exported(os.path.join(XCOMPAT, "pulse_null.c"))
    udev_impl = udev_exported(os.path.join(XCOMPAT, "udev_null.c"))

    print("required libX11 symbols : %d" % len(req))
    print("exported by the DSO     : %d (%d IMPL, %d STUB)"
          % (len(impl), sum(1 for _, k, _ in impl if k == "IMPL"),
             sum(1 for _, k, _ in impl if k == "STUB")))
    missing = [s for s in req if s not in {n for n, _, _ in impl}]
    print("required but missing    : %d %s" % (len(missing), " ".join(missing)))
    if alsa:
        print("required ALSA symbols   : %d" % len(alsa))
    if pulse:
        pmiss = [s for s in pulse if s not in pulse_impl]
        print("required pulse symbols  : %d (exported by the shims: %d, missing: %d)"
              % (len(pulse), len(pulse_impl), len(pmiss)))
        if pmiss:
            print("  missing: %s" % " ".join(pmiss))
            missing += pmiss
    if udev:
        umiss = [s for s in udev if s not in udev_impl]
        print("required udev symbols   : %d (exported by the shim: %d, missing: %d)"
              % (len(udev), len(udev_impl), len(umiss)))
        if umiss:
            print("  missing: %s" % " ".join(umiss))
            missing += umiss

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

    if pulse:
        with open(os.path.join(XCOMPAT, "required-pulse.txt"), "w") as f:
            f.write("\n".join(pulse) + "\n")
        with open(os.path.join(XCOMPAT, "required-pulse.h"), "w") as f:
            f.write("/* generated by tools/gen_exports.py -- do not edit.\n"
                    " * The pa_* symbols Unity's audio path (FMOD's pulse output +\n"
                    " * SDL's PulseAudio backend) resolves from libpulse-simple.so.0\n"
                    " * and, through it, libpulse.so.0.  Measured from\n"
                    " * UnityPlayer.so's string table.  This is the F4 contract for\n"
                    " * the pulse shim: BOTH built sonames export every one of\n"
                    " * them (see pulse_null.c). */\n")
            f.write("#ifndef HOBBYOS_XCOMPAT_REQUIRED_PULSE_H\n"
                    "#define HOBBYOS_XCOMPAT_REQUIRED_PULSE_H\n\n")
            f.write("#define XCOMPAT_PULSE_COUNT %d\n\n" % len(pulse))
            f.write("static const char *const xcompat_pulse_required[] = {\n")
            for s in pulse:
                f.write('  "%s",\n' % s)
            f.write("};\n\n#endif\n")

    if udev:
        with open(os.path.join(XCOMPAT, "required-udev.txt"), "w") as f:
            f.write("\n".join(udev) + "\n")
        with open(os.path.join(XCOMPAT, "required-udev.h"), "w") as f:
            f.write("/* generated by tools/gen_exports.py -- do not edit.\n"
                    " * The udev_* symbols SDL's joystick/udev backend resolves\n"
                    " * from libudev.so.1.  Measured from UnityPlayer.so's\n"
                    " * string table, intersected with the host's real libudev\n"
                    " * (which drops the binary's own \"udev_new() failed\"\n"
                    " * style messages).  This is the F4 contract for the\n"
                    " * libudev shim. */\n")
            f.write("#ifndef HOBBYOS_XCOMPAT_REQUIRED_UDEV_H\n"
                    "#define HOBBYOS_XCOMPAT_REQUIRED_UDEV_H\n\n")
            f.write("#define XCOMPAT_UDEV_COUNT %d\n\n" % len(udev))
            f.write("static const char *const xcompat_udev_required[] = {\n")
            for s in udev:
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
        if kind == "IMPL" and n in IMPL_NOTE:
            return "IMPL", IMPL_NOTE[n]
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

    if pulse:
        lines.append("### (d) libpulse-simple.so.0 / libpulse.so.0 - the %d "
                     "symbols the audio path resolves\n" % len(pulse))
        lines.append("Both built sonames export this entire table (plus the "
                     "extras in (e)):\n")
        lines.append("| symbol | behaviour |")
        lines.append("|---|---|")
        for n in pulse:
            lines.append("| `%s` | %s |"
                         % (n, PULSE_BEHAVIOUR.get(n, "returns a failure value")))
        lines.append("")

        extra = [n for n in pulse_impl if n not in pulse]
        if extra:
            lines.append("### (e) libpulse - exported beyond the required set\n")
            lines.append("| symbol | behaviour |")
            lines.append("|---|---|")
            for n in extra:
                lines.append("| `%s` | %s |"
                             % (n, PULSE_BEHAVIOUR.get(n, "returns a failure value")))
            lines.append("")
    if udev:
        lines.append("### (f) libudev.so.1 - the %d symbols SDL's udev backend "
                     "resolves\n" % len(udev))
        lines.append("| symbol | behaviour |")
        lines.append("|---|---|")
        for n in udev:
            lines.append("| `%s` | %s |"
                         % (n, UDEV_BEHAVIOUR.get(n, "returns a failure value")))
        lines.append("")

        uextra = [n for n in udev_impl if n not in udev]
        if uextra:
            lines.append("### (g) libudev.so.1 - exported beyond the required "
                         "set\n")
            lines.append("Nothing in UnityPlayer.so names these; they exist so "
                         "a caller that probes the wider public surface gets "
                         "the same empty answer instead of a missing symbol. "
                         "Every one is NULL-safe.\n")
            lines.append("| symbol | behaviour |")
            lines.append("|---|---|")
            for n in uextra:
                lines.append("| `%s` | %s |"
                             % (n, UDEV_BEHAVIOUR.get(n, "no-op / NULL")))
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
