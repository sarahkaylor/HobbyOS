#!/usr/bin/env python3
"""evidence_rig.py — per-game evidence capture rig for the HobbyOS gaming
swarm (GB-040).

Runs the standard HobbyOS boot + a game launch under QEMU and captures a
complete, self-describing per-game evidence bundle:

  evidence/<game>/<timestamp>/
    serial.log      - the full serial console dump (boot + launch + run)
    *.ppm / *.png   - QMP screendumps taken at named checkpoints
    replay.jsonl    - the exact input sequence sent (QMP send-key / click),
                      so the run is reproducible
    manifest.json   - game, commit, arch, date, checkpoint list + sha256 of
                      every artifact

It then writes a `<game>-EVIDENCE-REPORT.md` at the repo root (the committed
pointer; evidence/ itself is gitignored) describing where the assets landed
and what they prove.  This is the "per-game evidence rig: serial log +
screendump + replay scripts" of GB-040, shared by every title's T5 run.

The rig is deliberately harness-agnostic: you hand it the launch sequence
(functions to boot, open the start menu, and navigate to the game's slot)
and it wraps the capture.  See --help.

Requires a built boot image (`make disk.img`); it only drives the OS.
"""
import argparse, hashlib, json, os, re, shutil, socket, subprocess, sys, time
from datetime import datetime, timezone

W, H = 1920, 1080


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class QMP:
    """Minimal QMP client (same contract the run_*_test.py harnesses use)."""
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(15)
        for _ in range(200):
            try:
                self.sock.connect(path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.5)
        else:
            raise RuntimeError("QMP socket never appeared: " + path)
        self.sock.recv(65536)
        self.cmd("qmp_capabilities")
        self.replay = []

    def cmd(self, name, args=None):
        m = {"execute": name}
        if args:
            m["arguments"] = args
        self.sock.sendall(json.dumps(m).encode() + b"\n")
        buf = b""
        while True:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            buf += chunk
            for part in buf.decode(errors="ignore").strip().split("\n"):
                try:
                    r = json.loads(part)
                except Exception:
                    continue
                if "return" in r or "error" in r:
                    return r

    def mouse(self, x, y):
        self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 0x7FFF / W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 0x7FFF / H)}},
        ]})
        time.sleep(0.1)
        self.replay.append({"t": "mouse", "x": x, "y": y})

    def click(self, x, y, button="left"):
        self.mouse(x, y)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.08)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": button, "down": False}}]})
        time.sleep(0.4)
        self.replay.append({"t": "click", "x": x, "y": y, "b": button})

    def key(self, qcode, pause=0.15):
        self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})
        time.sleep(pause)
        self.replay.append({"t": "key", "k": qcode})

    def screendump(self, path):
        # QMP screendump writes PPM; record the path so the caller can keep
        # the raw PPM plus (optionally) a PNG conversion.
        self.cmd("screendump", {"filename": path})
        time.sleep(0.4)
        return path


def boot(qemu_make, serial_log, qmp_sock, arch, timeout_s=180):
    """Start QEMU via `make run` (serial->file + QMP socket); wait for the
    desktop and the [MENU] dump.  Using `make run` guarantees the correct
    UEFI/CPU/serial invocation rather than hand-rebuilding the qemu line."""
    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    for f in (serial_log, qmp_sock):
        if os.path.exists(f):
            os.remove(f)
    subprocess.Popen(
        ["stdbuf", "-oL"] + qemu_make,
        stdout=open(serial_log, "w"), stderr=subprocess.STDOUT)
    q = QMP(qmp_sock)
    t0 = time.time()
    saw = ""
    while time.time() - t0 < timeout_s:
        try:
            saw = open(serial_log, errors="ignore").read()
        except OSError:
            saw = ""
        if "Desktop starting" in saw and "[MENU]" in saw:
            break
        time.sleep(0.5)
    if not saw.strip():
        subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
        raise RuntimeError("serial log empty after boot - a stale QEMU is "
                           "holding disk.img/QMP; kill it and retry")
    time.sleep(3)
    return q


def find_menu_slot(serial_log, name):
    """Return the start-menu index of `name` from the [MENU] n=NAME dump."""
    txt = open(serial_log, errors="ignore").read()
    for n, b in re.findall(r"\[MENU\]\s*(?:\[CONSOLE\]\s*)*(\d+)(?:\[CONSOLE\]\s*)*=(?:\[CONSOLE\]\s*)*([A-Za-z0-9_.]+)", txt):
        if b.strip() == name:
            return int(n)
    return None


def derive_bin(game):
    """Default launcher .BIN name for a game id (xnibbler -> XNIBBLER.BIN)."""
    g = game.upper().strip()
    return g if g.endswith(".BIN") else g + ".BIN"


def resolve_launch_slot(serial_log, bin_name, provided=None):
    """The authoritative launcher slot for bin_name, resolved BY NAME from the
    [MENU] dump (GB-059: never a magic slot index).  A caller-provided --slot
    is only a sanity check: if it disagrees with the observed slot, the
    observed slot wins (the run must launch the title it actually found, not a
    stale index — the XNIBBLER->CONSOLE.BIN false-launch bug)."""
    found = find_menu_slot(serial_log, bin_name)
    if found is None:
        return None, f"{bin_name} not in start-menu dump"
    warn = ""
    if provided is not None and provided != found:
        warn = (f"caller passed --slot {provided} but {bin_name} is at slot "
                f"{found} on this tree; launching by name ({found}), not the "
                "magic index")
    return found, warn


def launch_from_menu(q, slot, taskbar_y=H - 26, arch="arm", gesture="keys"):
    """Open the Apps menu and navigate down to the given slot, launching it.

    GB-116: on intel the pointer click below cannot work.  `src/user/desktop.c`
    maps **F1 (evdev 59) to open the start menu** for exactly that reason, with
    the comment: "on x64 the kernel's PS/2 input path does not deliver mouse
    buttons (browser.md, WF1B), so an F1 map keeps the menu launchable on both
    arches".  `Makefile:257` has no pointer device at all, so a QMP absolute
    click has nowhere to land and the [LAUNCH] line never appears -- measured as
    `[LAUNCH] = 0` on GB-059-V's intel run.

    `gesture`:
      keys  -- F1 opens the menu, then Home + Down*s + Enter (the default: measured
               to launch on BOTH arches -- intel 1, arm 1)
      click -- the pre-GB-116 behaviour, kept only to reproduce the intel failure
               (measured: intel 0, arm 1)

    Do NOT press the click *after* F1: the Apps button toggles the same menu shut
    (`desktop.c:2218` gates F1 on `!start_menu_open && !app_menu_open`), so an
    "F1 then click" gesture measures 0 on intel *and* 0 on ARM -- GB-116-V’s RED,
    which is why the first cut of this card (`--gesture auto`) was withdrawn.
    """
    if gesture == "keys":
        q.key("f1", 0.4)          # desktop.c: EV_KEY 59 opens the start menu
        time.sleep(0.4)
    elif gesture == "click":
        q.click(38, taskbar_y + 13)
        time.sleep(0.8)
    else:
        # A dead default is worse than a loud one: `perf_game_cycle.py` and
        # `t5/acceptance_run.py` both call this with no gesture, so an unknown
        # value here silently opens nothing and the launch fails for a reason
        # that is not in the log (GB-116-V2 clause 6).
        raise ValueError("launch_from_menu: unknown gesture %r (want 'keys' or 'click')" % (gesture,))
    q.key("home", 0.3)
    for _ in range(slot):
        q.key("down", 0.06)
    q.key("ret", 0.3)


def convert_png(ppm_path, png_path):
    try:
        from PIL import Image
        Image.open(ppm_path).save(png_path)
        return True
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="game id, e.g. xnibbler")
    ap.add_argument("--bin", default=None,
                    help="menu name, e.g. XNIBBLER.BIN (default <GAME>.BIN)")
    ap.add_argument("--slot", type=int, default=None,
                    help="expected launcher slot (optional; the slot the rig "
                         "FOUND by name always wins — never a magic index)")
    ap.add_argument("--arch", default="arm", choices=["arm", "intel"])
    ap.add_argument("--root", default=os.getcwd(), help="repo root (default cwd)")
    ap.add_argument("--checkpoints", default="boot,launch,gameplay",
                    help="comma-separated screendump checkpoints")
    ap.add_argument("--run-seconds", type=int, default=8,
                    help="seconds to hold the game open after launch")
    ap.add_argument("--input", default="", help="optional key sequence, e.g. up,left,up,left")
    ap.add_argument("--gesture", default="keys", choices=["keys", "click"],
                    help="how to open the Apps menu: keys = F1 (works on intel AND arm; "
                         "intel has no pointer device, so the click cannot work there), "
                         "click = the pre-GB-116 behaviour, for reproducing that failure. "
                         "Do not combine them: the Apps button toggles the F1 menu shut.")
    args = ap.parse_args()

    os.chdir(args.root)
    arch = args.arch
    path = f"evidence/{args.game}/{datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')}"
    os.makedirs(path, exist_ok=True)

    serial_log = os.path.join(path, "serial.log")
    qmp_sock = f"/tmp/evidence-qmp-{args.game}"
    # Boot via `make run` (the proven path; it supplies the right UEFI/CPU/
    # serial flags) and point QMP at our socket.
    qemu_make = ["make", "run", f"ARCH={arch}",
                 f"QEMU_ARGS=-display none -qmp unix:{qmp_sock},server,nowait"]

    print(f"[rig] booting {arch} QEMU, capturing to {path}")
    q = boot(qemu_make, serial_log, qmp_sock, arch)

    slot = args.slot
    bin_name = args.bin or derive_bin(args.game)
    found, warn = resolve_launch_slot(serial_log, bin_name, provided=args.slot)
    if warn:
        print(f"[rig] WARNING: {warn}")
    if found is None:
        print(f"[rig] FATAL: {bin_name} not in start-menu dump")
        sys.exit(1)
    slot = found
    print(f"[rig] {bin_name} slot: found by name = {found}"
          f" (launching {found}, by name)")

    checkpoints = [c.strip() for c in args.checkpoints.split(",") if c.strip()]
    shots = {}

    def snap(cp):
        ppm = os.path.join(path, f"{cp}.ppm")
        shots[cp] = q.screendump(ppm)
        png = ppm[:-4] + ".png"
        if convert_png(ppm, png):
            shots[cp] = ppm

    if "boot" in checkpoints:
        snap("boot")
    launch_from_menu(q, slot, arch=args.arch, gesture=args.gesture)
    if "launch" in checkpoints:
        snap("launch")

    # Optional scripted input (replay seed), captured to replay.jsonl.
    if args.input:
        for code in [c.strip() for c in args.input.split(",") if c.strip()]:
            q.key(code, 0.2)
    time.sleep(args.run_seconds)
    if "gameplay" in checkpoints:
        snap("gameplay")

    # Write the input replay (everything sent, incl. launch nav).
    with open(os.path.join(path, "replay.jsonl"), "w") as f:
        for ev in q.replay:
            f.write(json.dumps(ev) + "\n")

    # Manifest: game, commit, arch, date, artifact sha256s.
    try:
        commit = subprocess.run(["git", "rev-parse", "HEAD"],
                                capture_output=True, text=True).stdout.strip()
    except Exception:
        commit = "n/a"
    artifacts = {}
    for fn in sorted(os.listdir(path)):
        fp = os.path.join(path, fn)
        if os.path.isfile(fp):
            artifacts[fn] = sha256(fp)
    manifest = {
        "game": args.game, "bin": bin_name, "slot": slot, "arch": arch,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "commit": commit, "root": args.root,
        "screendumps": {c: os.path.basename(p) for c, p in shots.items()},
        "inputs": args.input, "run_seconds": args.run_seconds,
        "artifacts_sha256": artifacts,
    }
    with open(os.path.join(path, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)

    # Root REPORT pointer (evidence/ is gitignored; this is the committed note).
    report = f"{args.game}-EVIDENCE-REPORT.md"
    with open(report, "w") as f:
        f.write(f"# {args.game} — GB-040 evidence bundle\n\n")
        f.write(f"- Game: `{bin_name}` (launcher slot {slot}, {arch})\n")
        f.write(f"- Commit: {commit}\n")
        f.write(f"- Captured: {manifest['captured_at']}\n\n")
        f.write("Assets landed in `evidence/{game}/<timestamp>/` (gitignored; "
                "see manifest.json there):\n\n")
        for cp, p in shots.items():
            f.write(f"- `{cp}` screendump: `{os.path.basename(p)}`\n")
        if args.input:
            f.write(f"- scripted input: {args.input}\n")
        f.write("- `serial.log` (full boot + run), `replay.jsonl` (input trace)\n")
        f.write("- `manifest.json` (artifact sha256 hashes for provenance)\n\n")
        f.write("Verify: relaunch with `--arch {arch} --slot {slot}` and diff "
                "screendumps; the replay.jsonl reproduces the input.\n")
    print(f"[rig] evidence bundle: {path}")
    print(f"[rig] report: {report}")

    subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"], stderr=subprocess.DEVNULL)
    sys.exit(0)


if __name__ == "__main__":
    main()
