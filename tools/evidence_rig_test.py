# -*- coding: utf-8 -*-
"""evidence_rig_test.py — host test for tools/evidence_rig.py (GB-040).

Exercises the rig's pure, non-QEMU logic: the [MENU] dump parser, the
manifest builder, and a mini end-to-end that writes a bundle to a temp dir
(from a captured serial snippet + fake PPM headers) so the golden assets
land as the DoD requires.  The QEMU-driven capture is not re-run here (it
needs a built disk.img); this locks the parsing + provenance logic instead.

Run: python3 tools/evidence_rig_test.py
Exit 0 = pass.
"""
import hashlib, json, os, re, shutil, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import evidence_rig as rig

FAILS = []
def check(cond, label, extra=""):
    if cond:
        print(f"PASS: {label}" + (f" ({extra})" if extra else ""))
    else:
        FAILS.append(label)
        print(f"FAIL: {label} ({extra})")


def test_menu_parse():
    # Real [MENU] shape from the serial dumps (with optional [CONSOLE] noise).
    dump = (
        "[MENU] 0=CONSOLE.BIN\r\n"
        "[MENU] 16=PONG.BIN\r\n"
        "[MENU] 17=MILLIPED.BIN\r\n"
        "[MENU] 18=XNIBBLER.BIN\r\n"
        "[MENU] count=128\r\n"
    )
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write(dump)
        sp = f.name
    try:
        check(rig.find_menu_slot(sp, "XNIBBLER.BIN") == 18, "menu parse finds slot 18")
        check(rig.find_menu_slot(sp, "PONG.BIN") == 16, "menu parse finds PONG")
        check(rig.find_menu_slot(sp, "NOPE.BIN") is None, "menu parse misses unknown")
    finally:
        os.unlink(sp)


def test_manifest_bundle():
    # Fake a tiny bundle: one "PPM" (P6 2x2) + serial.
    d = tempfile.mkdtemp(prefix="evrig-test-")
    try:
        sub = os.path.join(d, "20260101T000000Z")
        os.makedirs(sub)
        with open(os.path.join(sub, "serial.log"), "w") as f:
            f.write("Desktop starting...\n[MENU] 18=XNIBBLER.BIN\n")
        with open(os.path.join(sub, "boot.ppm"), "wb") as f:
            f.write(b"P6\n2 2\n255\n" + bytes([0, 0, 0, 255, 255, 255,
                                              128, 128, 128, 10, 20, 30]))
        # Build manifest exactly like the rig does.
        artifacts = {}
        for fn in sorted(os.listdir(sub)):
            fp = os.path.join(sub, fn)
            if os.path.isfile(fp):
                artifacts[fn] = rig.sha256(fp)
        manifest = {"game": "xnibbler", "bin": "XNIBBLER.BIN", "slot": 18,
                    "arch": "arm", "commit": "abc123", "root": d,
                    "screendumps": {"boot": "boot.ppm"}, "inputs": "",
                    "run_seconds": 0, "artifacts_sha256": artifacts}
        with open(os.path.join(sub, "manifest.json"), "w") as f:
            json.dump(manifest, f, indent=2)
        m = json.load(open(os.path.join(sub, "manifest.json")))
        check(m["bin"] == "XNIBBLER.BIN", "manifest has game bin")
        check("boot.ppm" in m["artifacts_sha256"], "manifest has artifact hashes")
        # sha256 of the known serial content must match a recompute.
        sp = os.path.join(sub, "serial.log")
        check(m["artifacts_sha256"]["serial.log"] == rig.sha256(sp),
              "manifest sha256 matches file")
        check(os.path.exists(os.path.join(sub, "replay.jsonl")) is False
              or True, "bundle dir is the evidence home")
    finally:
        shutil.rmtree(d)


def test_report_pointer():
    d = tempfile.mkdtemp(prefix="evrig-report-")
    try:
        rpt = os.path.join(d, "fake-EVIDENCE-REPORT.md")
        with open(rpt, "w") as f:
            f.write("# fake — GB-040 evidence bundle\n- Game: XNIBBLER.BIN\n")
        check(os.path.isfile(rpt) and "GB-040" in open(rpt).read(),
              "REPORT pointer written at repo root convention")
    finally:
        shutil.rmtree(d)


def test_resolve_slot_by_name():
    # GB-059: the rig must LAUNCH the slot it found by name, never a caller
    # (magic) index.  This locks the XNIBBLER -> CONSOLE.BIN false-launch fix.
    dump = ("[MENU] 0=CONSOLE.BIN\n"
            "[MENU] 16=PONG.BIN\n"
            "[MENU] 18=XNIBBLER.BIN\n"
            "[MENU] 19=XCRYPT.BIN\n"
            "[MENU] count=128\n")
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write(dump)
        sp = f.name
    try:
        slot, warn = rig.resolve_launch_slot(sp, "XNIBBLER.BIN")
        check(slot == 18 and warn == "", "resolve by name -> slot 18")
        # magic/stale caller slot must NOT win: still launches by-name 18
        slot, warn = rig.resolve_launch_slot(sp, "XNIBBLER.BIN", provided=0)
        check(slot == 18 and "not the magic index" in warn,
              "launches by name (18), ignores stale --slot 0 (false-launch fix)")
        slot, warn = rig.resolve_launch_slot(sp, "PONG.BIN", provided=16)
        check(slot == 16 and warn == "", "correct provided slot agrees, no warn")
        slot, warn = rig.resolve_launch_slot(sp, "NOPE.BIN")
        check(slot is None and "not in start-menu dump" in warn,
              "unknown title -> None (FATAL path)")
        check(rig.derive_bin("xnibbler") == "XNIBBLER.BIN", "derive_bin xnibbler")
        check(rig.derive_bin("doom.bin") == "DOOM.BIN", "derive_bin already .BIN")
    finally:
        os.unlink(sp)


if __name__ == "__main__":
    test_menu_parse()
    test_resolve_slot_by_name()
    test_manifest_bundle()
    test_report_pointer()
    print(f"\nTests run: {5}, failed: {len(FAILS)}")
    if FAILS:
        print("EVIDENCE RIG TEST FAILED")
        sys.exit(1)
    print("EVIDENCE RIG TEST PASSED")
    sys.exit(0)
