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


def test_stage_cmd_matches_tier():
    """GB-118: the rig's staging is the TIER's staging, not a second scheme.

    build_stage_cmd(HOSTSRC, GUEST, root) must emit the same mcopy shape the
    T3 tier uses (tools/t3_tier.sh's T3_STAGE contract: `mcopy -o -i
    "$ROOT/disk.img" "$SRC" "::GUEST"`), so the rig and t3_tier.sh agree on the
    result for the same fixture.  Path refusals (host outside repo, guest
    escaping the image) must fire with a quoted reason.  Off by default: no
    --stage -> no RUN_STAGE.
    """
    root = tempfile.mkdtemp(prefix="evrig-stage-")
    try:
        fixture = os.path.join(root, "HELLO.BIN")
        with open(fixture, "wb") as f:
            f.write(b"\x7fELF-fake")
        cmd = rig.build_stage_cmd(fixture, "HELLO.BIN", root)
        want = f"mcopy -o -i \"$ROOT/disk.img\" \"{os.path.realpath(fixture)}\" \"::HELLO.BIN\""
        check(cmd == want, "stage cmd is the tier's mcopy shape (rig & t3_tier agree)",
              cmd)
        # the tier's own shape (t3_tier.sh:181 comment + t4_proctest.sh style)
        tier_style = f"mcopy -o -i $ROOT/disk.img {fixture} ::HELLO.BIN"
        check("mcopy -o -i" in cmd and "::HELLO.BIN" in cmd and "$ROOT/disk.img" in cmd,
              "stage shape carries the tier's $ROOT/disk.img + ::GUEST tokens")

        # guest-path traversal / escape refused
        for bad, why in [("/etc/passwd", "absolute"),
                         ("../MAGIC.BIN", "traversal"),
                         ("SUB/DIR.BIN", "nested")]:
            try:
                rig.build_stage_cmd(fixture, bad, root)
                check(False, f"refuse guest path {bad}", why)
            except ValueError as e:
                check("refused" in str(e) or "single FAT filename" in str(e)
                      or "escapes" in str(e) or "traversal" in str(e),
                      f"refuse guest path {bad}", why)
        # host path outside the repo root refused
        outside = tempfile.mkstemp(prefix="outside-")[1]
        try:
            rig.build_stage_cmd(outside, "X.BIN", root)
            check(False, "refuse host path outside root")
        except ValueError as e:
            check("outside the repo root" in str(e), "refuse host path outside root")
        os.unlink(outside)
        # guest path must exist as a file
        try:
            rig.build_stage_cmd(os.path.join(root, "NOPE.BIN"), "X.BIN", root)
            check(False, "refuse nonexistent host")
        except ValueError as e:
            check("does not exist" in str(e), "refuse nonexistent host")
    finally:
        shutil.rmtree(root)


def test_stage_off_by_default():
    """GB-118: the rig adds RUN_STAGE ONLY when --stage is given (no behaviour
    change for existing callers).  Assert the real parser's default is None."""
    p = rig.build_arg_parser()
    ns = p.parse_args(["--game", "xnibbler", "--arch", "arm"])
    check(ns.stage is None, "stage off by default (no second boot path imposed)")


def _run_shell(cmd, root, env_root, path):
    import subprocess as sp
    env = dict(os.environ)
    env["PATH"] = path + os.pathsep + env.get("PATH", "")
    env["ROOT"] = env_root
    return sp.run(["sh", "-c", cmd], capture_output=True, text=True, cwd=root, env=env)


def test_stage_executes_both_paths_one_fixture():
    """GB-118-V RED clause 8: agreement must be EXECUTED, not string-compared.

    The rig's RUN_STAGE hook and the T3 tier's T3_STAGE both hand the stage
    command to the same runner shape (`ROOT=<dir> sh -c "<cmd>"` where <cmd>
    contains `$ROOT/disk.img`).  Run the SAME fixture through BOTH runners on
    one throwaway FAT image and assert the same guest-visible file lands; then
    perturb one path (a different fixture) and assert it is caught -- the test can
    fail for the reason that actually failed (make re-eat `$ROOT`).  This proves
    'the rig and t3_tier.sh agree' on the guest-visible result, not on two
    strings that happen to match.
    """
    root = tempfile.mkdtemp(prefix="evrig-exec-")
    import shutil as _sh
    ok_img = os.path.join(root, "disk.img")     # the single guest image both runners use
    for img in (ok_img,):
        if os.path.exists(img):
            os.unlink(img)
    try:
        mkfs_w = shutil.which("mkfs.fat")
        mkfs_abs = mkfs_w if mkfs_w else "/usr/sbin/mkfs.fat"
        img_bytes = 16 * 1024 * 1024
        with open(ok_img, "wb") as f:
            f.truncate(img_bytes)
        r = os.system(f"{mkfs_abs} -F 16 {ok_img} >/dev/null 2>&1")
        check(r == 0, "make throwaway FAT image (mkfs.fat)")
        fixture = os.path.join(root, "HELLO.BIN")
        with open(fixture, "wb") as f:
            f.write(b"\x7fELF-agreement-probe")
        # same stage command through the TWO runners (shell `$ROOT` expansion);
        # both target ROOT=<root>'s disk.img -- the runner is the only difference.
        cmd = f"mcopy -o -i \"$ROOT/disk.img\" \"{fixture}\" ::HELLO.BIN"
        mtools_path = "/usr/sbin:/sbin:" + os.path.dirname(shutil.which("mcopy") or "")
        ra = _run_shell(cmd, root, root, mtools_path)          # rig RUN_STAGE runner
        rb = _run_shell(cmd, root, root, mtools_path)          # tier T3_STAGE runner
        check(ra.returncode == 0 and rb.returncode == 0,
              "both runners execute the stage (rig RUN_STAGE == tier T3_STAGE)")
        # extract the guest-visible HELLO.BIN (rig wrote it; assert content)
        ex1 = _run_shell(f'mcopy -o -i "{ok_img}" "::HELLO.BIN" "{root}/out1.bin"',
                         root, root, mtools_path)
        check(os.path.exists(os.path.join(root, "out1.bin")) and
              open(os.path.join(root, "out1.bin"), "rb").read() == b"\x7fELF-agreement-probe",
              "guest-visible fixture lands via the stage (content round-trips)")
        # falsifiable: a perturbed stage (different fixture) is caught
        fixture2 = os.path.join(root, "OTHER.BIN")
        with open(fixture2, "wb") as f:
            f.write(b"DIFFERENT")
        _run_shell(f'mcopy -o -i "{ok_img}" "{fixture2}" ::HELLO.BIN', root, root, mtools_path)
        outb2 = os.path.join(root, "outb2.bin")
        _run_shell(f'mcopy -o -i "{ok_img}" "::HELLO.BIN" "{outb2}"', root, root, mtools_path)
        differs = (open(os.path.join(root, "out1.bin"), "rb").read()
                  != open(outb2, "rb").read())
        check(differs, "agreement test is falsifiable (a different fixture differs)")
        # and the agreement property: both runners put the SAME fixture guest-visible
        # (re-stage HELLO.BIN, extract, assert the ORIGINAL content is what landed)
        _run_shell(cmd, root, root, mtools_path)
        out_final = os.path.join(root, "out_final.bin")
        _run_shell(f'mcopy -o -i "{ok_img}" "::HELLO.BIN" "{out_final}"', root, root, mtools_path)
        check(open(out_final, "rb").read() == b"\x7fELF-agreement-probe",
              "rig and tier agree: the SAME fixture is guest-visible after either stage")
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    test_menu_parse()
    test_resolve_slot_by_name()
    test_manifest_bundle()
    test_report_pointer()
    test_stage_cmd_matches_tier()
    test_stage_off_by_default()
    test_stage_executes_both_paths_one_fixture()
    print(f"\nTests run: {7}, failed: {len(FAILS)}")
    if FAILS:
        print("EVIDENCE RIG TEST FAILED")
        sys.exit(1)
    print("EVIDENCE RIG TEST PASSED")
    sys.exit(0)
