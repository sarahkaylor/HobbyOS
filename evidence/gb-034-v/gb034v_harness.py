#!/usr/bin/env python3
"""gb-034-v gate harness — DOOM launch-by-name + virtio-input keypress test.

Why this exists: tools/evidence_rig.py's QMP.key() sends legacy `send-key`
(PS/2 scancodes into the i8042).  On this tree the desktop reads virtio-input
(`get_events`), so every keyboard event the rig sent was discarded — the rig
found DOOM.BIN's slot 18 by name but never launched it (`[LAUNCH]` absent from
serial, all three screendumps identical, 105 colours of desktop chrome).  The
PLAN-014 note on GB-034-V requires QMP `input-send-event` (virtio-input); every
event in this harness is one, and each is recorded to replay.jsonl.

Phases:
  boot     -> wait for desktop + [MENU] dump
  launch   -> F1 (start menu), Home (sel=0), Down x slot, Enter   [all virtio]
  assert   -> serial must contain "[LAUNCH] DOOM.BIN"
  capture  -> screendump per checkpoint
  keys     -> "--keys ret,ret,ret" sends each key and snapshots after it
  control  -> "--control" waits the same wall time with NO input and snapshots

Usage:
  gb034v_harness.py --arch intel --out DIR --keys ret,ret,ret
  gb034v_harness.py --arch intel --out DIR --control
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.join(os.getcwd(), "tools"))
import evidence_rig as rig  # QMP client + boot() + find_menu_slot()


class VQMP(rig.QMP):
    """QMP with virtio-input key events (input-send-event, qcode)."""

    def _ev(self, qcode, down):
        forms = ({"events": [{"type": "key", "data": {
                     "down": down, "key": {"type": "qcode", "data": qcode}}}]},
                 {"events": [{"type": "key", "data": {"down": down, "key": qcode}}]})
        last = None
        for args in forms:
            r = self.cmd("input-send-event", args)
            if r and "error" not in r:
                return True
            last = r
        raise RuntimeError(f"input-send-event rejected qcode {qcode!r}: {last}")

    def key(self, qcode, pause=0.25):
        self._ev(qcode, True)
        time.sleep(0.05)
        self._ev(qcode, False)
        time.sleep(pause)
        self.replay.append({"t": "input-send-event/key", "qcode": qcode})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="intel")
    ap.add_argument("--bin", default="DOOM.BIN")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keys", default="", help="comma list of qcodes, snapshot after each")
    ap.add_argument("--before", type=float, default=4.0, help="settle before baseline")
    ap.add_argument("--after", type=float, default=2.5, help="settle after the last key")
    ap.add_argument("--gap", type=float, default=0.6, help="settle after each key")
    ap.add_argument("--control", action="store_true", help="no input: same timing only")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    serial = os.path.join(args.out, "serial.log")
    sock = f"/tmp/gb034v-qmp-{os.getpid()}"
    qmp_make = ["make", "run", f"ARCH={args.arch}",
                f"QEMU_ARGS=-display none -qmp unix:{sock},server,nowait"]

    print(f"[harness] booting {args.arch} -> {args.out}", flush=True)
    q = rig.boot(qmp_make, serial, sock, args.arch)   # returns a rig.QMP
    q.__class__ = VQMP                                # same instance, virtio keys
    Q = q

    def rd():
        try:
            return open(serial, errors="ignore").read()
        except OSError:
            return ""

    slot = rig.find_menu_slot(serial, args.bin)
    print(f"[harness] {args.bin} slot by name = {slot}", flush=True)
    if slot is None:
        print("[harness] FATAL: not in [MENU] dump")
        sys.exit(1)

    def snap(tag):
        p = os.path.join(args.out, f"{tag}.ppm")
        Q.screendump(p)
        print(f"[harness] screendump {tag}", flush=True)
        return p

    snap("00_boot")
    before = rd()

    Q.key("f1", 0.6)          # open start menu
    Q.key("home", 0.4)        # F1 pre-selects BROWSER when pinned; force 0
    for _ in range(slot):
        Q.key("down", 0.06)
    Q.key("ret", 0.8)         # launch
    time.sleep(1.5)
    launched = ("[LAUNCH] " + args.bin) in rd()
    print(f"[harness] [LAUNCH] {args.bin} in serial: {launched}", flush=True)
    snap("01_launch")

    time.sleep(args.before)
    snap("02_baseline")

    keys = [] if args.control else [x.strip() for x in args.keys.split(",") if x.strip()]
    for i, k in enumerate(keys, 1):
        Q.key(k, 0.2)
        time.sleep(args.gap)
        snap(f"1{i}_after_{k}")
        print(f"[harness] sent input-send-event key {k!r}", flush=True)
    if args.control:
        time.sleep(args.gap * max(1, len(args.keys.split(","))))

    time.sleep(args.after)
    snap("03_final")

    with open(os.path.join(args.out, "replay.jsonl"), "w") as f:
        for ev in Q.replay:
            f.write(json.dumps(ev) + "\n")

    lines = [ln for ln in rd().splitlines()
             if "DOOM" in ln or "LAUNCH" in ln or "IWAD" in ln]
    with open(os.path.join(args.out, "doom_serial_lines.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[harness] DOOM/LAUNCH serial lines: {len(lines)}", flush=True)
    for ln in lines[-15:]:
        print("   ", ln[:160])

    rig.subprocess.run(["pkill", "-9", "-f", "qemu-system[-]"],
                       stderr=rig.subprocess.DEVNULL)
    sys.exit(0 if launched else 2)


if __name__ == "__main__":
    main()
