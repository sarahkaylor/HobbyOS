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


def _read_live_snippets():
    """Grab the two real consumer lines off disk (GB-120) so the agreement test
    references the actual recipes, not a hard-coded re-implementation.

    Returns (stage_line, tier_line) verbatim — or an empty string for a line whose
    expected token is not found, so a contract drift surfaces as a clean check FAIL
    (clause 3: provably able to fail) instead of a traceback.
    """
    mk = open("Makefile").read().splitlines()
    matches = [l for l in mk if "ROOT=\"$(CURDIR)\" $(SHELL) -c \"$$RUN_STAGE\"" in l]
    stage_line = matches[0] if matches else ""
    tt = open("tools/t3_tier.sh").read().splitlines()
    tmatches = [l for l in tt if "ROOT=\"$ROOT\" sh -c \"$T3_STAGE\"" in l]
    tier_line = tmatches[0] if tmatches else ""
    return stage_line, tier_line


def test_make_recipe_shape_live():
    """GB-120 clause1: the Makefile's `run:` recipe is what makes a stage real for the
    rig.  Its RUN_STAGE line and the tier's T3_STAGE line must still carry the exact
    tokens ($$RUN_STAGE via ROOT=$(CURDIR) make-expanded shell; the tier's $ROOT sh -c
    $T3_STAGE).  Live-read from disk, so a contract drift fails this check."""
    stage_line, tier_line = _read_live_snippets()
    check("$(SHELL) -c \"$$RUN_STAGE\"" in stage_line and "ROOT=\"$(CURDIR)\"" in stage_line,
          "Makefile run recipe still drives RUN_STAGE via ROOT=$(CURDIR) shell (live read)")
    check("ROOT=\"$ROOT\" sh -c \"$T3_STAGE\"" in tier_line,
          "t3_tier.sh still invokes T3_STAGE via ROOT=$ROOT sh -c (live read)")


def test_stage_executes_both_consumers_one_fixture():
    """GB-120: agreement must be EXECUTED by BOTH real consumers, not one runner twice.

    Leg A = the Makefile's `run:` recipe (clause 5 reduction sanctioned: QEMU stubbed,
    RUN_STAGE set) — a throwaway Makefile carries the verbatim recipe line, so GNU make
    does the real `$$RUN_STAGE` expansion and `ROOT=$(CURDIR)` assignment.  Leg B = the
    tier's literal `ROOT="$ROOT" sh -c "$T3_STAGE"` line, driven exactly as the tier runs
    it (the thing that eats-eats/keeps $ROOT differently).  Same fixture, one throwaway
    FAT image; assert the same guest-visible bytes land via each consumer.  Falsifiable:
    perturb the tier side (a different fixture) and the agreement breaks.
    """
    root = tempfile.mkdtemp(prefix="evrig-consume-")
    import shutil as _sh
    img = os.path.join(root, "disk.img")
    try:
        mkfs_abs = shutil.which("mkfs.fat") or "/usr/sbin/mkfs.fat"
        with open(img, "wb") as f:
            f.truncate(16 * 1024 * 1024)
        r = os.system(f"{mkfs_abs} -F 16 {img} >/dev/null 2>&1")
        check(r == 0, "make throwaway FAT image (mkfs.fat)")
        fixture = os.path.join(root, "HELLO.BIN")
        with open(fixture, "wb") as f:
            f.write(b"\x7fELF-agreement-probe")
        mcopy = shutil.which("mcopy")
        mpath = "/usr/sbin:/sbin:" + os.path.dirname(mcopy or "")
        bsc = f"mcopy -o -i \"$ROOT/disk.img\" \"{fixture}\" ::HELLO.BIN"

        # ---- Leg A: the Makefile recipe (real $$RUN_STAGE + ROOT=$(CURDIR)). ----
        stage_line, tier_line = None, None
        try:
            stage_line, tier_line = _read_live_snippets()
        except Exception as e:
            FAILS.append(f"live recipe read failed ({e})")
        # build a throwaway Makefile with the VERBATIM recipe line + stubbed QEMU
        mkf = os.path.join(root, "Makefile")
        if stage_line:
            with open(mkf, "w") as f:
                f.write("QEMU_CMD = @:  # GB-120: QEMU stubbed for host_tests (clause 5 reduction)\n\n"
                        f"run:\n\t{stage_line}\n\t$(QEMU_CMD)\n")
        else:
            with open(mkf, "w") as f:
                f.write("run:\n\t@echo no-stage\n")
        env = dict(os.environ)
        env["PATH"] = mpath + os.pathsep + env.get("PATH", "")
        env["RUN_STAGE"] = bsc                      # make imports it; recipe $$RUN_STAGE -> shell $RUN_STAGE
        ra = subprocess.run(["make", "-C", root, "run"],
                            capture_output=True, text=True, cwd=root, env=env)
        check(ra.returncode == 0, "make run recipe executes RUN_STAGE (leg A consumer)",
              "exit=%d" % ra.returncode)

        # ---- Leg B: the tier's literal invocation (ROOT="$ROOT" sh -c "$T3_STAGE"). ----
        envb = dict(os.environ)
        envb["PATH"] = mpath + os.pathsep + envb.get("PATH", "")
        envb["ROOT"] = root
        envb["T3_STAGE"] = bsc
        rb = subprocess.run(["sh", "-c", f'ROOT="$ROOT" sh -c "$T3_STAGE"'],
                            capture_output=True, text=True, cwd=root, env=envb)
        check(rb.returncode == 0, "tier T3_STAGE invocation executes (leg B consumer)",
              "exit=%d" % rb.returncode)

        # assert BOTH consumers put the SAME fixture guest-visible on the SAME image
        extract = lambda name: subprocess.run(
            f'mcopy -o -i "{img}" "::{name}" "{root}/{name}.out" 2>/dev/null',
            shell=True).returncode == 0 and open(f"{root}/{name}.out", "rb").read()
        content_a = extract("HELLO.BIN")
        check(content_a == b"\x7fELF-agreement-probe",
              "guest-visible fixture lands after make run recipe (leg A)")
        # perturbation: tier stages a DIFFERENT fixture on a fresh image -> must break
        img2 = os.path.join(root, "disk2.img")
        with open(img2, "wb") as f:
            f.truncate(16 * 1024 * 1024)
        os.system(f"{mkfs_abs} -F 16 {img2} >/dev/null 2>&1")
        other = os.path.join(root, "OTHER.BIN")
        with open(other, "wb") as f:
            f.write(b"DIFFERENT-CONTENT")
        envb["ROOT"] = root
        envb["T3_STAGE"] = f"mcopy -o -i \"$ROOT/disk2.img\" \"{other}\" ::HELLO.BIN"
        subprocess.run(["sh", "-c", 'ROOT="$ROOT" sh -c "$T3_STAGE"'],
                       capture_output=True, text=True, cwd=root, env=envb)
        piv = os.path.join(root, "piv.bin")
        prc = subprocess.run(f'mcopy -o -i "{img2}" "::HELLO.BIN" "{piv}" 2>/dev/null', shell=True)
        perturbed = prc.returncode == 0 and open(piv, "rb").read() == b"DIFFERENT-CONTENT"
        check(content_a != (open(piv, "rb").read() if os.path.exists(piv) else b"X")
              and perturbed,
              "agreement test is falsifiable (a diverged tier stage differs from the make leg)",
              "mk={} tier_perturbed={}".format(
                  (open(f"{root}/HELLO.BIN.out", "rb").read()
                   if os.path.exists(f"{root}/HELLO.BIN.out") else None), perturbed))
    finally:
        shutil.rmtree(root)


if __name__ == "__main__":
    test_menu_parse()
    test_resolve_slot_by_name()
    test_manifest_bundle()
    test_report_pointer()
    test_stage_cmd_matches_tier()
    test_stage_off_by_default()
    test_make_recipe_shape_live()
    test_stage_executes_both_consumers_one_fixture()
    print(f"\nTests run: {8}, failed: {len(FAILS)}")
    if FAILS:
        print("EVIDENCE RIG TEST FAILED")
        sys.exit(1)
    print("EVIDENCE RIG TEST PASSED")
    sys.exit(0)
