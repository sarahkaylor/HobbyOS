#!/usr/bin/env python3
"""run_selfhost_build_test.py — M4 acceptance driver (wave 2, from lane L5's
spec in docs/selfhost/acceptance.md §5).

Boosts the staged disk with L5's exact ARM QEMU line (§5.1), launches
SELFHOST.BIN (the in-OS build driver, src/user/selfhost/) via the QMP
keystroke path (run_console_test.py precedent: Apps menu -> menu index from
the [MENU] dump), and verdicts the S0..S4 build stages from [SELFHOST]
markers in the serial capture.  Verdicts are marker-substring based with a
FAIL-token scan (§8.4) — never guest exit codes.

Failure classes (in the result JSON; see --classifier-self-test):
  success            every configured stage reached its -done marker AND
                     ACCEPTANCE-COMPLETE is present AND zero FAIL tokens
  boot_failure       "Booting AArch64 OS" never appeared in the serial
                     capture (kernel never came up)
  driver_absent      booted, but no [SELFHOST] marker anywhere — the driver
                     is not on the image / not launchable from the menu
  script_missing     driver is up but reports SCRIPT-MISSING / ABORT
                     reason=script-missing (clean config failure)
  stage_failure      clean staged failure: phase=<sX>-fail / ABORT
                     missing-tool / a FAIL token after the driver started
  hang               the driver started but reached neither a completion
                     marker nor a failure marker before the timeout
  harness_error      local error (image/tool missing, qemu failed to start)

S4/S5 (host side: extract OUT.IMG, boot it, assert the kernel marker, then
run an in-OS-built program) are reported as not_run while the in-OS build
cannot succeed (no MAKE.BIN yet — S3 always fails first); they execute
automatically as soon as OUT.IMG exists on the staged disk.

Usage:
  tools/run_selfhost_build_test.py [options]
    --image PATH        disk image to boot (default: $SELFHOST_TEST_IMG or
                        disk.img)
    --stage             run tools/selfhost-stage.sh -f <image> first
    --no-launch         boot and classify boot markers only (no keystrokes;
                        useful for S4-style boots)
    --timeout N         per-phase wait seconds (default 240)
    --qmp-sock PATH     qmp unix socket path (default ./qmp-selfhost)
    --serial PATH       serial capture path (default ./serial-selfhost.log)
    --expected-class C  assert the final failure_class == C; exit 0 when it
                        matches (CI gate for the wave-2 honest outcome)
    --classifier-self-test  run the classify() unit checks and exit
    --json PATH         write the result JSON here (default:
                        ./run_selfhost_build_test.result.json)
"""
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AAVMF = os.path.expanduser("~/.local/share/AAVMF/AAVMF_CODE.fd")
QEMU = "qemu-system-aarch64"
W, H = 1920, 1080
TASKBAR_Y = H - 26
MAC = "52:54:00:12:34:56"

BOOT_MARKER = "Booting AArch64 OS"
MENU_RE = re.compile(r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)\s*=\s*([A-Za-z0-9_.]+)")
SELFHOST_TAG = "[SELFHOST]"
COMPLETE = "ACCEPTANCE-COMPLETE"
FAIL_RE = re.compile(r"\bFAIL(?:ED)?\b")

EXIT_OK, EXIT_HARNESS = 0, 15
EXIT_CODES = {
    "boot_failure": 10,
    "driver_absent": 11,
    "script_missing": 12,
    "stage_failure": 13,
    "hang": 14,
    "success": 0,
    "harness_error": 15,
}


def log(msg):
    print("[run_selfhost] %s" % msg, flush=True)


# --------------------------------------------------------------------------
# QMP client (keystroke drive) — copied shape from run_console_test.py
# --------------------------------------------------------------------------
class Q:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(15)
        for _ in range(200):
            try:
                self.s.connect(path)
                break
            except Exception:
                time.sleep(0.5)
        self.s.recv(65536)
        self.c("qmp_capabilities")

    def c(self, ex, args=None):
        m = {"execute": ex}
        if args:
            m["arguments"] = args
        self.s.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            buf += self.s.recv(65536)
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    r = json.loads(part)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r

    def mv(self, x, y):
        self.c("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}}]})
        time.sleep(0.1)

    def click(self, x, y):
        self.mv(x, y)
        self.c("input-send-event",
               {"events": [{"type": "btn", "data": {"button": "left", "down": True}}]})
        time.sleep(0.08)
        self.c("input-send-event",
               {"events": [{"type": "btn", "data": {"button": "left", "down": False}}]})
        time.sleep(0.4)

    def key(self, q, pause=0.12):
        self.c("send-key", {"keys": [{"type": "qcode", "data": q}]})
        time.sleep(pause)


# --------------------------------------------------------------------------
# QEMU management (one process per image; never broad pkill)
# --------------------------------------------------------------------------
def qemu_cmd(image, serial, qmp, extra):
    cmd = [
        QEMU, "-M", "virt", "-cpu", "cortex-a53", "-smp", "8", "-m", "16384M",
        "-accel", "tcg,thread=multi", "-bios", AAVMF,
        "-display", "none",
        "-serial", "file:%s" % serial,
        "-drive", "if=none,file=%s,format=raw,id=hd0" % image,
        "-device", "virtio-blk-device,drive=hd0",
        "-device", "virtio-gpu-device",
        "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
        "-netdev", "user,id=net0",
        "-device", "virtio-net-device,netdev=net0,mac=%s" % MAC,
        "-semihosting", "-action", "shutdown=poweroff",
    ]
    if qmp:
        cmd += ["-qmp", "unix:%s,server,nowait" % qmp]
    cmd += extra
    return cmd


def boot(image, serial, qmp=None, extra=()):
    """Start qemu; returns the Popen handle (caller terminates it)."""
    for p in (serial, qmp):
        if p and os.path.exists(p):
            os.remove(p)
    return subprocess.Popen(qemu_cmd(image, serial, qmp, list(extra)),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_qemu(proc):
    if not proc or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)


def read_serial(path):
    try:
        with open(path, errors="ignore") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(path, needle, timeout, start=None):
    start = start if start is not None else time.time()
    while time.time() - start < timeout:
        if needle in read_serial(path):
            return time.time() - start
        time.sleep(0.5)
    return None


# --------------------------------------------------------------------------
# Classification (pure — tested by --classifier-self-test)
# --------------------------------------------------------------------------
def classify(text, timeout_hit=True, configured=()):
    """Map a serial capture to (failure_class, detail).  The order matters:
    a driver that ABORTs also never prints ACCEPTANCE-COMPLETE, and a FAIL
    token anywhere after driver start overrides a done-looking capture."""
    boot_ok = BOOT_MARKER in text
    if not boot_ok:
        return "boot_failure", "kernel marker '%s' never appeared" % BOOT_MARKER
    if SELFHOST_TAG not in text:
        return "driver_absent", "booted but no [SELFHOST] marker in capture"

    # driver session = text after the first driver marker
    didx = text.index(SELFHOST_TAG)
    driver_text = text[didx:]

    if "SCRIPT-MISSING" in driver_text or "reason=script-missing" in driver_text:
        return "script_missing", "driver found no command script (SCRIPT-MISSING)"

    fail_m = re.search(r"phase=([A-Za-z0-9_-]+)-fail", driver_text)
    if fail_m:
        return "stage_failure", "stage %s reported a clean -fail marker" % fail_m.group(1)

    if "ABORT" in driver_text:
        m = re.search(r"missing-tool=([^\s]+)", driver_text)
        if m:
            return "stage_failure", "missing tool %s (clean staged failure)" % m.group(1)
        m2 = re.search(r"reason=([^\s]+)", driver_text)
        why = m2.group(1) if m2 else "abort"
        return "stage_failure", "driver ABORT reason=%s" % why

    done_phases = set(re.findall(r"phase=([A-Za-z0-9_-]+)-done", driver_text))
    if COMPLETE in driver_text:
        if FAIL_RE.search(driver_text):
            return "stage_failure", "FAIL token after driver start despite ACCEPTANCE-COMPLETE"
        return "success", "ACCEPTANCE-COMPLETE with %d stage(s) done" % len(done_phases)

    if FAIL_RE.search(driver_text):
        return "stage_failure", "FAIL token after driver start (no completion marker)"

    if timeout_hit:
        return "hang", "driver started but reached no terminal marker before timeout"
    return "hang", "no terminal marker yet (classified while still running)"


def classifier_self_test():
    cases = [
        ("boot_failure", "firmware chatter\nNo bootable device\n"),
        ("driver_absent",
         "Booting AArch64 OS\nMode: DESKTOP - Launching desktop...\n[MENU] count=4\n[MENU] 0=DESKTOP.BIN\n"),
        ("script_missing",
         "Booting AArch64 OS\n[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN\n"
         "[SELFHOST] SCRIPT-MISSING path=/SRC/SELFHOST.RUN\n[SELFHOST] ABORT reason=script-missing\n"),
        ("stage_failure",
         "Booting AArch64 OS\n[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN\n"
         "[SELFHOST] phase=s0-check-start\n"
         "[SELFHOST] phase=s0-check-fail tool=MAKE.BIN reason=tool-missing rc=127\n"
         "[SELFHOST] ABORT stage=s0-check missing-tool=MAKE.BIN\n"
         "[SELFHOST] FAILED-AT stage=s0-check\n"),
        ("stage_failure_failtoken",
         "Booting AArch64 OS\n[SELFHOST] DRIVER-UP\n[SELFHOST] phase=s0-check-start\n"
         "[SELFHOST] phase=s0-check-done rc=0\n[SELFHOST] ACCEPTANCE-COMPLETE stages=1\n"
         "make: error: FAIL: clang crashed\n"),
        ("hang",
         "Booting AArch64 OS\n[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN\n"
         "[SELFHOST] phase=s0-check-start\n"),
        ("success",
         "Booting AArch64 OS\n[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN\n"
         "[SELFHOST] phase=s0-check-start\n[SELFHOST] phase=s0-check-done rc=0\n"
         "[SELFHOST] phase=s1-user-done rc=0\n[SELFHOST] ACCEPTANCE-COMPLETE stages=4\n"),
    ]
    ok = True
    for want, text in cases:
        got, detail = classify(text)
        cls = got if not want.endswith("_failtoken") else "stage_failure"
        mark = "PASS" if cls == want.split("_failtoken")[0] else "FAIL"
        if cls != want.split("_failtoken")[0]:
            ok = False
        print("classifier %-22s -> %-15s %s  (%s)" % ("%s" % want, got, mark, detail))
    return ok


# --------------------------------------------------------------------------
# Keystroke drive of the driver (fallback path, current kernel)
# --------------------------------------------------------------------------
def menu_index(text, want):
    entries = {}
    for a, b in re.findall(r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)\s*=\s*([A-Za-z0-9_.]+)", text):
        entries[int(a)] = b
    for i in sorted(entries):
        if entries[i].upper() == want.upper():
            return i
    return None


def launch_driver(q, idx):
    """Open Apps menu and click the SELFHOST.BIN entry (run_console_test
    precedent)."""
    q.click(38, TASKBAR_Y + 13)
    time.sleep(0.8)
    q.key("home", 0.3)
    for _ in range(idx):
        q.key("down", 0.06)
    q.key("ret", 0.3)
    time.sleep(1.5)


# --------------------------------------------------------------------------
# Helpers: script + OUT.IMG from the image (read-only extractor)
# --------------------------------------------------------------------------
def fat16_get(image, name):
    """python3 fat16img.py get <image> <name> -> bytes or None."""
    try:
        out = subprocess.run(
            [sys.executable, os.path.join(ROOT, "fat16img.py"), "get", image, name],
            capture_output=True, timeout=30)
        if out.returncode == 0 and out.stdout:
            return out.stdout
    except Exception:
        pass
    return None


def read_staged_script(image):
    raw = fat16_get(image, "SRC/SELFHOST.RUN")
    if raw is None:
        host = os.path.join(ROOT, "src/user/selfhost/SELFHOST.RUN")
        if os.path.exists(host):
            with open(host) as f:
                raw = f.read().encode()
    if not raw:
        return []
    phases = []
    for line in raw.decode(errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "|" not in line:
            continue
        phases.append(line.split("|", 1)[0].strip())
    return phases


# --------------------------------------------------------------------------
# Main driver
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default=os.environ.get("SELFHOST_TEST_IMG", "disk.img"))
    ap.add_argument("--stage", action="store_true")
    ap.add_argument("--no-launch", action="store_true")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--qmp-sock", default="./qmp-selfhost")
    ap.add_argument("--serial", default="./serial-selfhost.log")
    ap.add_argument("--expected-class", default=None)
    ap.add_argument("--classifier-self-test", action="store_true")
    ap.add_argument("--json", default="./run_selfhost_build_test.result.json")
    ap.add_argument("--keep-qemu", action="store_true")
    args = ap.parse_args()

    if args.classifier_self_test:
        ok = classifier_self_test()
        sys.exit(0 if ok else 1)

    result = {
        "lane": "w2-harness",
        "script": os.path.basename(__file__),
        "image": args.image,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "configured_stages": [],
        "stages_reached": 0,
        "failure_class": "harness_error",
        "failure_detail": "",
        "boot": {"boot_ok": False, "elapsed_s": None},
        "driver": {"launched": False, "menu_index": None},
        "markers": {},
        "s4": {"status": "not_run", "detail": "no OUT.IMG on the staged disk (in-OS build cannot succeed until MAKE.BIN exists)"},
        "s5": {"status": "not_run", "detail": "depends on S4 / OUT.IMG"},
        "log": args.serial,
        "expected_class": args.expected_class,
    }

    def save():
        with open(args.json, "w") as f:
            json.dump(result, f, indent=2)
        log("result JSON written: %s" % args.json)

    if not os.path.exists(AAVMF):
        result["failure_detail"] = "AAVMF firmware missing at %s" % AAVMF
        save()
        log("ERROR: %s" % result["failure_detail"])
        return EXIT_HARNESS
    if not os.path.exists(args.image):
        result["failure_detail"] = "image not found: %s" % args.image
        save()
        log("ERROR: %s" % result["failure_detail"])
        return EXIT_HARNESS

    if args.stage:
        log("staging payload onto %s (selfhost-stage.sh -f)" % args.image)
        r = subprocess.run([os.path.join(ROOT, "tools/selfhost-stage.sh"), "-f", args.image])
        if r.returncode != 0:
            result["failure_detail"] = "selfhost-stage.sh failed rc=%d" % r.returncode
            save()
            return EXIT_HARNESS

    result["configured_stages"] = read_staged_script(args.image)
    log("configured stages: %s" % (result["configured_stages"] or "(none parsed)"))

    # ---------------- S0..S3 boot + driver run ----------------
    t0 = time.time()
    qemu = boot(args.image, args.serial, qmp=args.qmp_sock)
    try:
        boot_el = wait_for(args.serial, BOOT_MARKER, min(90, args.timeout))
        result["boot"]["boot_ok"] = boot_el is not None
        result["boot"]["elapsed_s"] = round(boot_el, 1) if boot_el else None
        log("boot marker '%s': %s" % (BOOT_MARKER, "ok %.1fs" % boot_el if boot_el else "NOT SEEN"))

        serial_txt = read_serial(args.serial)

        if args.no_launch:
            cls, detail = classify(serial_txt, timeout_hit=boot_el is None)
            result["failure_class"], result["failure_detail"] = cls, detail if cls == "boot_failure" or boot_el is None else "boot ok (no-launch mode)"
            save()
            return EXIT_OK if boot_el is not None else EXIT_CODES[cls]

        # driver launch via the Apps menu
        if boot_el is not None:
            if "[MENU]" not in serial_txt:
                wait_for(args.serial, "[MENU]", 30)
            serial_txt = read_serial(args.serial)
            idx = menu_index(serial_txt, "SELFHOST.BIN")
            result["driver"]["menu_index"] = idx
            if idx is None:
                result["failure_class"] = "driver_absent"
                result["failure_detail"] = "booted but SELFHOST.BIN not in the Apps menu (not staged on the image root?)"
                save()
                log("VERDICT: %s" % result["failure_class"])
                return EXIT_CODES["driver_absent"]
            log("SELFHOST.BIN menu index: %s" % idx)
            q = Q(args.qmp_sock)
            launch_driver(q, idx)
            result["driver"]["launched"] = True

        # verdict window
        deadline = time.time() + args.timeout
        terminal = None
        while time.time() < deadline:
            txt = read_serial(args.serial)
            didx = txt.find(SELFHOST_TAG)
            driver_text = txt[didx:] if didx >= 0 else ""
            if COMPLETE in driver_text or "ABORT" in driver_text or \
               re.search(r"phase=[A-Za-z0-9_-]+-fail", driver_text) or \
               "SCRIPT-MISSING" in driver_text:
                terminal = txt
                break
            time.sleep(0.5)
        if terminal is None:
            terminal = read_serial(args.serial)

        cls, detail = classify(terminal, timeout_hit=terminal is None)
        result["failure_class"], result["failure_detail"] = cls, detail

        # marker counts over the driver session
        d = terminal[terminal.find(SELFHOST_TAG):] if SELFHOST_TAG in terminal else terminal
        started = sorted(set(re.findall(r"phase=([A-Za-z0-9_-]+)-start", d)))
        done = sorted(set(re.findall(r"phase=([A-Za-z0-9_-]+)-done", d)))
        result["markers"] = {
            "driver_up": d.count("DRIVER-UP"),
            "phase_start": len(re.findall(r"phase=[A-Za-z0-9_-]+-start", d)),
            "phase_done": len(re.findall(r"phase=[A-Za-z0-9_-]+-done", d)),
            "phase_fail": len(re.findall(r"phase=[A-Za-z0-9_-]+-fail", d)),
            "abort": d.count("[SELFHOST] ABORT"),
            "complete": d.count(COMPLETE),
            "fail_token": len(FAIL_RE.findall(d)),
        }
        # stages_reached = number of distinct started stages
        result["stages_reached"] = len(started)
        result["phases"] = {"started": started, "done": done}

        # ---------------- S4/S5 (only meaningful when a build exists) ------
        out = fat16_get(args.image, "OUT.IMG")
        if out is None:
            result["s4"] = {"status": "not_run",
                            "detail": "no OUT.IMG on the staged disk (in-OS build cannot succeed until MAKE.BIN exists)"}
            result["s5"] = {"status": "not_run", "detail": "requires OUT.IMG (S4)"}
        else:
            out_path = os.path.join(os.path.dirname(args.json), "out-selfhost.img")
            with open(out_path, "wb") as f:
                f.write(out)
            result["s4"] = {"status": "run", "boot_ok": False, "detail": "OUT.IMG extracted (%d bytes) to %s" % (len(out), out_path)}
            q2 = boot(out_path, args.serial + ".s4", qmp=None)
            try:
                el = wait_for(args.serial + ".s4", BOOT_MARKER, min(120, args.timeout))
                result["s4"]["boot_ok"] = el is not None
                result["s4"]["detail"] += "; boot marker %s" % ("ok %.1fs" % el if el else "NOT SEEN")
                if el is None:
                    result["failure_class"] = "stage_failure"
                    result["failure_detail"] = "S4 failed: OUT.IMG did not boot"
            finally:
                stop_qemu(q2)
    finally:
        if not args.keep_qemu:
            stop_qemu(qemu)

    save()
    verdict = result["failure_class"]
    log("VERDICT: %s — %s" % (verdict, result["failure_detail"]))
    log("markers: %s" % json.dumps(result["markers"]))
    log("elapsed: %.1fs total" % (time.time() - t0))

    expected = args.expected_class
    if expected and verdict == expected:
        log("expected-class %s matched -> exit 0" % expected)
        return EXIT_OK
    if verdict == "success":
        return EXIT_OK
    return EXIT_CODES.get(verdict, EXIT_HARNESS)


if __name__ == "__main__":
    sys.exit(main())
