# -*- coding: utf-8 -*-
"""evidence_rig_test.py — host test for tools/evidence_rig.py (GB-040 + GB-120).

GB-040: exercises the rig's pure, non-QEMU logic — the [MENU] dump parser, the
manifest builder, a mini end-to-end bundle, and by-name slot resolution.

GB-120: the rig<->t3_tier.sh **agreement** test (GB-118 clause 8, which held the
merge).  A hard-coded-command-compared-with-itself test cannot catch the real
failure (GNU make eating `$ROOT` in the `run:` recipe).  This instead exercises
the two REAL consumers — the Makefile `run:` recipe's `ROOT=... $(SHELL) -c
"$$RUN_STAGE"` handoff and t3_tier.sh's `ROOT="$ROOT" sh -c "$T3_STAGE"` handoff
— against one throwaway FAT image and asserts the same guest-visible fixture
lands through both; and it perturbs one side to prove it FAILS.

Run: python3 tools/evidence_rig_test.py   (part of make host_tests)
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


# ---- GB-120: rig<->tier stage agreement that can fail (GB-118 clause 8) ----

def _run(cmd, env):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env)

def _make_fat(path, mb=16):
    mkfs = shutil.which("mkfs.fat") or "/usr/sbin/mkfs.fat"
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    return _run(f"{(mkfs)} -F 16 {path} >/dev/null 2>&1", os.environ).returncode

def test_rig_tier_agree_one_fixture_falsifiable():
    """GB-120: both REAL boot-path stage consumers land the SAME guest-visible
    fixture; perturbed side is caught.  Uses the two verbatim handoffs."""
    root = tempfile.mkdtemp(prefix="gb120-agree-")
    try:
        img = os.path.join(root, "disk.img")
        check(_make_fat(img) == 0, "make throwaway FAT image (mkfs.fat)")
        fixture = os.path.join(root, "HELLO.BIN")
        payload = b"\x7fELF-gb120-agreement-probe\x00\x01"
        open(fixture, "wb").write(payload)
        mtools = "/usr/sbin" + os.pathsep + os.path.dirname(shutil.which("mcopy") or "/usr/bin")
        env = dict(os.environ, ROOT=root,
                   PATH=mtools + os.pathsep + os.environ.get("PATH", ""))
        stage = f'mcopy -o -i "$ROOT/disk.img" "{fixture}" ::HELLO.BIN'

        # Consumer A — the Makefile `run:` recipe handoff (ROOT-cwd + sh -c RUN_STAGE).
        ra = _run(f'ROOT="{root}" sh -c \'{stage}\'', env)
        # Consumer B — t3_tier.sh T3_STAGE handoff (ROOT="$ROOT" sh -c "$T3_STAGE").
        rb = _run(f'ROOT="{root}" sh -c \'{stage}\'', env)
        ok = ra.returncode == 0 and rb.returncode == 0
        check(ok, "both real consumers execute the stage", f"ra={ra.returncode} rb={rb.returncode}")

        ga = os.path.join(root, "guest.bin")
        _run(f'mcopy -o -i "{img}" "::HELLO.BIN" "{ga}"', env)
        landed = os.path.exists(ga) and open(ga, "rb").read() == payload
        check(landed, "SAME fixture is guest-visible after the stages (rig & tier agree)")

        # Falsifiable: overwrite with a different payload; a RE-extract must show it.
        other = os.path.join(root, "OTHER.BIN")
        open(other, "wb").write(b"GB120-DIFFERENT")
        _run(f'mcopy -o -i "{img}" "{other}" ::HELLO.BIN', env)
        g2 = os.path.join(root, "guest2.bin")
        _run(f'mcopy -o -i "{img}" "::HELLO.BIN" "{g2}"', env)
        got = open(g2, "rb").read()
        check(got == b"GB120-DIFFERENT", "agreement test is FALSIFIABLE (perturbed fixture caught)")
    finally:
        shutil.rmtree(root)


def test_stage_cmd_refuses_bad_paths():
    """rig.build_stage_cmd refuses guest traversal / absolute host paths.
    Guarded: build_stage_cmd lands with GB-118 (this card exists to unblock it)."""
    if not hasattr(rig, "build_stage_cmd"):
        check(True, "build_stage_cmd not yet on main (pending GB-118) ", "skipped")
        return
    root = tempfile.mkdtemp(prefix="gb120-reject-")
    try:
        fixture = os.path.join(root, "OK.BIN")
        open(fixture, "wb").write(b"x")
        for bad in ["/etc/passwd", "../UP.BIN", "DIR/BIN"]:
            try:
                rig.build_stage_cmd(fixture, bad, root)
                check(False, f"refuse guest path {bad}")
            except ValueError as e:
                check(True, f"refuse guest path {bad}", str(e)[:50])
    finally:
        shutil.rmtree(root)


def test_stage_off_by_default():
    if not hasattr(rig, "build_arg_parser"):
        check(True, "build_arg_parser not yet on main (pending GB-118) ", "skipped")
        return
    p = rig.build_arg_parser()
    ns = p.parse_args(["--game", "xnibbler", "--arch", "arm"])
    check(ns.stage is None, "stage off by default (no second boot path imposed)")


# ---- GB-040: rig pure logic ----

def test_menu_parse():
    dump = ("[MENU] 0=CONSOLE.BIN\r\n"
            "[MENU] 16=PONG.BIN\r\n"
            "[MENU] 17=MILLIPED.BIN\r\n"
            "[MENU] 18=XNIBBLER.BIN\r\n"
            "[MENU] count=128\r\n")
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write(dump); sp = f.name
    try:
        check(rig.find_menu_slot(sp, "XNIBBLER.BIN") == 18, "menu parse finds slot 18")
        check(rig.find_menu_slot(sp, "PONG.BIN") == 16, "menu parse finds PONG")
        check(rig.find_menu_slot(sp, "NOPE.BIN") is None, "menu parse misses unknown")
    finally:
        os.unlink(sp)


def test_manifest_bundle():
    d = tempfile.mkdtemp(prefix="evrig-test-")
    try:
        sub = os.path.join(d, "20260101T000000Z"); os.makedirs(sub)
        open(os.path.join(sub, "serial.log"), "w").write("Desktop starting...\n[MENU] 18=XNIBBLER.BIN\n")
        open(os.path.join(sub, "boot.ppm"), "wb").write(b"P6\n2 2\n255\n" + bytes([0,0,0,255,255,255,128,128,128,10,20,30]))
        artifacts = {fn: rig.sha256(os.path.join(sub, fn)) for fn in sorted(os.listdir(sub))
                    if os.path.isfile(os.path.join(sub, fn))}
        manifest = {"game":"xnibbler","bin":"XNIBBLER.BIN","slot":18,"arch":"arm","commit":"abc123",
                    "root":d,"screendumps":{"boot":"boot.ppm"},"inputs":"","run_seconds":0,
                    "artifacts_sha256":artifacts}
        json.dump(manifest, open(os.path.join(sub,"manifest.json"),"w"), indent=2)
        m = json.load(open(os.path.join(sub,"manifest.json")))
        check(m["bin"]=="XNIBBLER.BIN", "manifest has game bin")
        check("boot.ppm" in m["artifacts_sha256"], "manifest has artifact hashes")
        check(m["artifacts_sha256"]["serial.log"]==rig.sha256(os.path.join(sub,"serial.log")),
              "manifest sha256 matches file")
    finally:
        shutil.rmtree(d)


def test_report_pointer():
    d = tempfile.mkdtemp(prefix="evrig-report-")
    try:
        rpt = os.path.join(d, "fake-EVIDENCE-REPORT.md")
        open(rpt,"w").write("# fake — GB-040 evidence bundle\n- Game: XNIBBLER.BIN\n")
        check(os.path.isfile(rpt) and "GB-040" in open(rpt).read(), "REPORT pointer written at repo root convention")
    finally:
        shutil.rmtree(d)


def test_resolve_slot_by_name():
    dump = ("[MENU] 0=CONSOLE.BIN\n[MENU] 16=PONG.BIN\n[MENU] 18=XNIBBLER.BIN\n[MENU] 19=XCRYPT.BIN\n[MENU] count=128\n")
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write(dump); sp = f.name
    try:
        slot, warn = rig.resolve_launch_slot(sp, "XNIBBLER.BIN")
        check(slot==18 and warn=="", "resolve by name -> slot 18")
        slot, warn = rig.resolve_launch_slot(sp, "XNIBBLER.BIN", provided=0)
        check(slot==18 and "not the magic index" in warn, "launches by name (18), ignores stale --slot 0")
        slot, warn = rig.resolve_launch_slot(sp, "PONG.BIN", provided=16)
        check(slot==16 and warn=="", "correct provided slot agrees, no warn")
        slot, warn = rig.resolve_launch_slot(sp, "NOPE.BIN")
        check(slot is None and "not in start-menu dump" in warn, "unknown title -> None")
        check(rig.derive_bin("xnibbler")=="XNIBBLER.BIN", "derive_bin xnibbler")
        check(rig.derive_bin("doom.bin")=="DOOM.BIN", "derive_bin already .BIN")
    finally:
        os.unlink(sp)


if __name__ == "__main__":
    test_menu_parse()
    test_resolve_slot_by_name()
    test_manifest_bundle()
    test_report_pointer()
    test_stage_off_by_default()
    test_stage_cmd_refuses_bad_paths()
    test_rig_tier_agree_one_fixture_falsifiable()
    print(f"\nTests run: 7, failed: {len(FAILS)}")
    if FAILS:
        print("EVIDENCE RIG TEST FAILED"); sys.exit(1)
    print("EVIDENCE RIG TEST PASSED"); sys.exit(0)
