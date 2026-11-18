#!/usr/bin/env python3
"""tools/cp/p9/boot_p9.py -- the boot + play half of a P9 game boot (lane C9-play,
docs/cp-p9.md).  tools/cp/p8/run-p8.sh calls it instead of boot_p8.py when
tools/cp/p9/run-p9.sh sets P8_BOOT; run-p8.sh still owns the build, the
staging, the refcounted game export and the data disk.

What it adds to boot_p8.py (whose helpers it imports unchanged):
  * a bigger guest: --mem up to 32768 MiB, only with --big-guest, which takes
    flock /tmp/cp-bigguest.lock INSIDE the QEMU slot (docs/cyberpunk.md §7.2f:
    at most one guest above 12 GiB on strix at a time).  host-venus.sh refuses
    MEM above 12 GiB (its slot rule), so its argv is asked for 12288M and the
    two memory arguments are rewritten here.
  * past the main menu it does not stop: it runs a play script (--script),
    a small line language driving the game over QMP:
        wait S                    sleep S seconds (fractions ok)
        key Q [Q ...]             tap each qcode (down 120 ms, up)
        hold Q MS                 hold a key for MS ms
        down Q / up Q             press / release (combine with wait)
        look DX DY [N] [MS]       relative mouse: N steps of DX/N,DY/N every MS ms
                                  (PS/2 rel events -> the DDX's relative slave
                                  -> XI2 raw motion -> Wine WM_INPUT; cp-x2.md §5)
        click [left|right] [MS]   a mouse button
        keytiming HOLD_MS GAP_MS  tap timing for `key` (default 200 / 800)
        prof SECS [HZ] [TAG] [stack]  guest PC sampler: QMP 'info registers -a'
                                  (+ kernel stack words) -> OUT/prof-TAG.txt
                                  (tools/cp/p9/prof.py)
        shot TAG                  screendump + OCR, kept as TAG
        expect TAG TIMEOUT REGEX  screendump + OCR every 4 s until REGEX matches
                                  (case-insensitive) or TIMEOUT s pass -> FAIL
        expect_not ...            the same, until REGEX does NOT match
        seek TAG MENU DIR MAX ITEM  MENU main|pause: tap DIR (up/down) until the
                                  highlighted (yellow-framed) row is ITEM (by its
                                  screen row), at most MAX taps -> FAIL
        listseek TAG DIR MAX Y    save list: tap until the focused entry's cyan
                                  frame is centred at screen row Y (+-15)
        unwind TAG MAX REGEX      Esc while the screen matches REGEX (map/hub -> world)
        phase NAME                label the fps/mem rows from here on
        mark TEXT                 a timeline row
        repeat N / end            loop
        ctl                       interactive: execute lines appended to
                                  OUT/ctl.txt (from ssh) until a line "done"
        wait_exit TIMEOUT         wait for the game process to end
        stop                      end the boot now (QEMU stopped)
  * the session record: a screendump every --play-shot-every s (30) from the
    menu on, all kept, plus the serial log read incrementally with host
    timestamps, so every winevulkan `trace:fps` row, every launcher [P8MEM]
    row and every kernel [THRS] row of the game gets a wall time and a phase:
    OUT/fps.csv, OUT/mem.csv, OUT/thrs.csv, OUT/phases.txt.

Never answered: the game's first-run screens (agreement, account offer,
telemetry; boot_p8.FIRST_RUN).  If OCR sees one at any point, the boot stops
with verdict first-run:<name> and the screendump is kept.  The only input
before the main menu is boot_p8's Space at "PRESS ... TO CONTINUE".
"""
import argparse
import fcntl
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tools", "cp", "p8"))
import boot_p8 as P8  # noqa: E402
import p8grade  # noqa: E402  (the shared game-phase verdicts, H2-early)

BIG_LOCK = "/tmp/cp-bigguest.lock"
FPS_RX = re.compile(r"trace:fps:\S+ \S+ @ approx ([0-9.]+)fps, total ([0-9.]+)fps")
MEM_RX = re.compile(r"\[P8MEM\] frames_free=(\d+) free_mib=(\d+)")
THRS_RX = re.compile(r"\[THRS\] pid=(\d+) live=(\d+) .*?anon_mib=(\d+) .*?res_mib=(\d+) .*?(Cyberpunk2077\.e|REDEngineErr\w*|wineserver)")
FRAME_RX = re.compile(r"\[FRAME\] frames=(\d+) total_mib=(\d+)")
# C6-p7's present-cost row (tools/mesa/venus/hb_present_stats.h), one per process per 2 s
HBP_RX = re.compile(r"\[HBP\] pid=(\d+) n=(\d+) fps=([0-9.]+) path=(\w+).*?sub=(\d+)/\d+ wait=(\d+)/\d+"
                    r" flip=(\d+)/\d+.*?scanout=(\d+) putimage=(\d+) refused=(\d+)")
P7_RX = re.compile(r"\[P7\] scanout (taken|back|refused)[^\n]{0,120}")


def log(msg):
    print(f"[P9W] {msg}", flush=True)


# The main/pause menu rows: (label, centre y) in the canonical layout the
# tables were measured in (ctl1: the game's 1280x720 picture at 320,180 of a
# 1920x1080 frame).
MENUS = {
    "main": [("CONTINUE", 469), ("NEW GAME", 506), ("LOAD GAME", 544), ("SETTINGS", 581),
             ("MODS", 618), ("CREDITS", 656), ("QUIT GAME", 692)],
    "pause": [("RESUME", 470), ("SAVE GAME", 510), ("LOAD GAME", 550), ("MODS", 592),
              ("SETTINGS", 632), ("CREDITS", 672), ("EXIT TO MAIN MENU", 712), ("QUIT GAME", 752)],
}


def game_view(im, xscreen, game_res="1280x720"):
    """the game's own picture at the 1280x720 scale every crop and row table
    here was measured on.  A 1280x720 game: the window at 320,180 of a
    1920x1080 X screen, or the whole (top-left of the) shot with a 1280x720 X
    screen (scanout blob or desktop glass).  H1-08 (cp-hunt): a 1920x1080 game
    fills a 1920x1080 X screen and its UI is 1.5x larger, so it is scaled
    down to 1280x720 instead (before, the rows were looked for unscaled and
    never found: SEEK FAILED at 1080p)."""
    if game_res != "1280x720":
        gw, gh = (int(v) for v in game_res.split("x"))
        g = im if im.size == (gw, gh) else im.crop((0, 0, gw, gh))
        return g.resize((1280, 720))
    if im.size == (1920, 1080) and xscreen == "1920x1080":
        return im.crop((320, 180, 1600, 900))
    return im if im.size == (1280, 720) else im.crop((0, 0, 1280, 720))


def canon_image(im, xscreen, game_res="1280x720"):
    """game_view() pasted at 320,180 of a black 1920x1080 frame: the layout of
    MENUS and of the save-list frame search"""
    from PIL import Image
    c = Image.new("RGB", (1920, 1080))
    c.paste(game_view(im, xscreen, game_res), (320, 180))
    return c


def focus_in(im, menu):
    """the highlighted (yellow-framed) row of a main/pause menu in a canon
    image: (label or None, centre y or None, (y0, y1) or None)"""
    px = im.load()
    # the highlight's yellow is ~(230,190,60) on pmemsave shots but as dim as
    # ~(170,125,40) on VNC shots of the glass (r2-play-scan1); red rows have
    # g ~70.  Main menu: stop above the orange Phantom Liberty banner (y 736..777).
    ymax = 715 if menu == "main" else 790
    ys = [y for y in range(420, ymax)
          if sum(1 for x in range(400, 800, 2)
                 if px[x, y][0] > 140 and px[x, y][1] > 95 and px[x, y][2] < 90) > 2]
    cl = []
    for y in ys:                      # clusters: frame lines + text, gaps <= 12
        if not cl or y - cl[-1][1] > 12:
            cl.append([y, y])
        cl[-1][1] = y
    cl = [c for c in cl if c[1] - c[0] >= 20]   # a framed row is ~36 px tall
    if not cl:
        return None, None, None
    y0, y1 = max(cl, key=lambda c: c[1] - c[0])
    yc = (y0 + y1) / 2
    label = None
    for lab, ry in MENUS[menu]:
        if abs(ry - yc) <= 12:
            label = lab
    return label, yc, (y0, y1)


class Serial:
    """Incremental reader: only new bytes each call, complete lines only."""

    def __init__(self, path):
        self.path, self.off, self.part = path, 0, b""
        self.all_text_tail = ""

    def lines(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.off)
                data = f.read()
        except OSError:
            return []
        if not data:
            return []
        self.off += len(data)
        data = self.part + data
        cut = data.rfind(b"\n")
        if cut < 0:
            self.part = data
            return []
        self.part = data[cut + 1:]
        return data[:cut].decode("utf-8", "replace").replace("\r", "").split("\n")


def qcode_event(q, down):
    return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--cpdata", required=True)
    ap.add_argument("--phase", choices=["game"], required=True)
    ap.add_argument("--smp", type=int, default=8)
    ap.add_argument("--mem", type=int, default=12288)
    ap.add_argument("--big-guest", action="store_true")
    ap.add_argument("--cpu", default="host")
    ap.add_argument("--timeout", type=int, default=5400)
    ap.add_argument("--shot-every", type=int, default=15)
    ap.add_argument("--play-shot-every", type=int, default=30)
    ap.add_argument("--stall-s", type=int, default=1200)
    ap.add_argument("--press-continue", action="store_true")
    ap.add_argument("--gpu", choices=["lavapipe", "venus"], default="venus")
    ap.add_argument("--script", required=True)
    ap.add_argument("--menu-hits", type=int, default=2)
    ap.add_argument("--xscreen", default="1920x1080", help="XHOBBY's screen size (run-p8.sh P8_XSCREEN)")
    ap.add_argument("--game-res", default="1280x720",
                    help="the game's own Resolution setting (run-p9.sh P9_GAME_RES; cp-hunt H1-08)")
    args = ap.parse_args()
    # SIGTERM -> SystemExit so `finally` stops QEMU and drops the locks (gateA2:
    # a plain SIGTERM left its 32 GiB guest running without a slot).
    import signal
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if args.mem > 12288 and not args.big_guest:
        sys.exit("boot_p9: -m above 12 GiB needs --big-guest (the big-guest lock)")
    if args.mem > 32768:
        sys.exit("boot_p9: -m above 32 GiB is not approved")
    script = [ln.rstrip("\n") for ln in open(args.script)]
    shots_dir = os.path.join(args.out, "shots")
    os.makedirs(shots_dir, exist_ok=True)

    # QEMU command: boot_p8's, with the memory size rewritten for a big guest.
    want = args.mem
    args.mem = min(want, 12288)
    if args.gpu == "venus":
        cmd, qmp_path, serial = P8.venus_qemu_cmd(args.out, args.disk, args.cpdata, args)
        sys.path.insert(0, os.path.join(REPO, "tools", "cp", "venus"))
        import p5venus  # noqa: E402
        p5venus._patch_qmp()
    else:
        cmd, qmp_path, serial = P8.qemu_cmd(args.out, args.disk, args.cpdata, args)
    if want != args.mem:
        old_m, new_m = f"{args.mem}M", f"{want}M"
        cmd = [new_m if a == old_m else a.replace(f"size={old_m}", f"size={new_m}") for a in cmd]
        assert any(a == new_m for a in cmd), "could not rewrite -m"
        with open(os.path.join(args.out, "cmdline.txt"), "a") as f:
            f.write(f"# boot_p9: guest memory rewritten {old_m} -> {new_m} (big guest)\n")
        args.mem = want
    for p in (serial, qmp_path):
        if os.path.exists(p):
            os.remove(p)

    tl = open(os.path.join(args.out, "timeline.txt"), "w")
    fps_f = open(os.path.join(args.out, "fps.csv"), "w")
    fps_f.write("host_s,game_s,phase,approx_fps,total_fps\n")
    mem_f = open(os.path.join(args.out, "mem.csv"), "w")
    mem_f.write("host_s,game_s,phase,free_mib\n")
    thr_f = open(os.path.join(args.out, "thrs.csv"), "w")
    thr_f.write("host_s,game_s,phase,pid,threads,anon_mib,res_mib,name\n")
    hbp_f = open(os.path.join(args.out, "hbp.csv"), "w")
    hbp_f.write("host_s,game_s,phase,pid,n,fps,path,sub_us,wait_us,flip_us,scanout,putimage,refused\n")
    ph_f = open(os.path.join(args.out, "phases.txt"), "w")
    t0 = None
    st = {"phase": "boot", "game_t": None}

    def mark(what):
        line = f"+{time.time() - t0:8.1f} s  {time.strftime('%H:%M:%S')}  {what}"
        tl.write(line + "\n")
        tl.flush()
        log(line)

    def gtime():
        return time.time() - st["game_t"] if st["game_t"] else -1.0

    def set_phase(name):
        st["phase"] = name
        ph_f.write(f"{time.time() - t0:.1f} {gtime():.1f} {name}\n")
        ph_f.flush()
        mark(f"phase {name}")

    log("waiting for a QEMU slot (tools/cp/qemu-slot.sh semantics)")
    lockf, slot = P8.acquire_slot()
    big = None
    if args.mem > 12288:
        big = open(BIG_LOCK, "a")
        log(f"slot {slot} held; waiting for {BIG_LOCK} (one guest above 12 GiB at a time)")
        fcntl.flock(big, fcntl.LOCK_EX)
    t0 = time.time()
    mark(f"slot {slot} acquired{' + big-guest lock' if big else ''}; boot -smp {args.smp} -m {args.mem}M "
         f"-cpu {args.cpu} (KVM, {args.gpu}); QEMU guests on the host before ours: {P8.guests_running()}")
    proc = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    log(f"qemu pid {proc.pid}")
    verdict = "timeout"
    why = ""
    seen = set()
    ser = Serial(serial)
    # the rows p8grade needs, from "[P8] begin game" on (kept small: P9 logs grow to GBs)
    grade_rx = re.compile(r"\[P8\] (?:begin|end) game|REDEngineErrorR|steam_api64\.dll\" at")
    glines = []
    kept = {"n": 0}
    try:
        qmp = P8.Qmp(qmp_path, deadline_s=40)

        def send(events):
            r = qmp.cmd("input-send-event", {"events": events})
            if "error" in r:
                mark(f"QMP input-send-event error: {r['error']}")

        def shoot(tag, do_ocr=True):
            path = os.path.join(shots_dir, "cur.ppm")
            if not (qmp.shot(path) and os.path.exists(path)):
                mark(f"screendump failed ({tag})")
                return None, ""
            kept["n"] += 1
            name = f"s{kept['n']:03d}-t{gtime():06.0f}-{tag}"
            keep = os.path.join(shots_dir, name + ".ppm")
            shutil.copy(path, keep)
            pngp = keep[:-4] + ".png"
            if P8.png(keep, pngp):
                os.remove(keep)
            text = ""
            if do_ocr:
                ocr_src = pngp
                try:
                    from PIL import Image
                    im = Image.open(pngp)
                    if im.size == (1280, 720):
                        # a VNC shot of a 1280x720 scanout: also OCR it in the layout
                        # the scripts' patterns were written on (the game window at
                        # 320,180 of 1920x1080, black around)
                        c = Image.new("RGB", (1920, 1080))
                        c.paste(im.convert("RGB"), (320, 180))
                        ocr_src = os.path.join(shots_dir, "ocr-canon.png")
                        c.save(ocr_src)
                except Exception as e:  # noqa: BLE001
                    log(f"canon OCR image: {e}")
                text = P8.ocr(pngp) or ""
                if ocr_src != pngp:
                    # tesseract reads some 1280x720 screens only bare (scan2: the
                    # menu) and others only in the old layout (the LOAD GAME
                    # title): use both
                    text += "\n" + (P8.ocr(ocr_src) or "")
                with open(os.path.join(shots_dir, name + ".ocr.txt"), "w") as f:
                    f.write(text)
            flat = " ".join(text.split())
            log(f"shot {name}: OCR {flat[:200]!r}")
            for fr_name, rx in P8.FIRST_RUN:
                if rx.search(flat):
                    raise FirstRun(fr_name, name)
            return name, flat

        class Abort(Exception):
            pass

        class FirstRun(Exception):
            def __init__(self, what, shot):
                super().__init__(what)
                self.what, self.shot = what, shot

        def pump():
            """the per-tick duties: serial rows with wall times, milestone marks."""
            for ln in ser.lines():
                if grade_rx.search(ln) and ("[P8] begin game " in seen or "[P8] begin game " in ln):
                    glines.append(ln)
                m = FPS_RX.search(ln)
                if m:
                    fps_f.write(f"{time.time() - t0:.1f},{gtime():.1f},{st['phase']},{m.group(1)},{m.group(2)}\n")
                    continue
                m = MEM_RX.search(ln)
                if m:
                    mem_f.write(f"{time.time() - t0:.1f},{gtime():.1f},{st['phase']},{m.group(2)}\n")
                    mem_f.flush()
                    continue
                m = THRS_RX.search(ln)
                if m:
                    thr_f.write(f"{time.time() - t0:.1f},{gtime():.1f},{st['phase']},{m.group(1)},{m.group(2)},"
                                f"{m.group(3)},{m.group(4)},{m.group(5)}\n")
                    continue
                m = HBP_RX.search(ln)
                if m:
                    hbp_f.write(f"{time.time() - t0:.1f},{gtime():.1f},{st['phase']}," + ",".join(m.groups()) + "\n")
                    hbp_f.flush()
                    continue
                m = P7_RX.search(ln)
                if m:
                    k = "p7:" + m.group(1)
                    st[k] = st.get(k, 0) + 1
                    if st[k] <= 8:
                        mark(f"kernel: {m.group(0).strip()}")
                m = FRAME_RX.search(ln)
                if m and "frame" not in seen:
                    seen.add("frame")
                    mark(f"kernel frame pool: {ln.strip()[:200]}")
                if ln.startswith("[X] ") or ln.startswith("[P8] "):
                    st["prog_rows"] = st.get("prog_rows", 0) + 1
                u = ln[4:] if ln.startswith("[X] ") else ln
                for key, label in (("[P8] mount ", "NFS mount"), ("[P8] warm-up: ", "warm-up result"),
                                   ("[P8] begin game ", "game launched"), ("[P8] end game ", "game process ended"),
                                   ("[P8] sync done", "sync done"), ("[P8] done", "launcher done")):
                    i = u.find(key)
                    if i >= 0 and key not in seen:
                        seen.add(key)
                        mark(f"{label}: {u[i:i + 200]}")
                        if key == "[P8] begin game ":
                            st["game_t"] = time.time()
                            set_phase("launch")
                            if args.gpu == "venus":
                                for r in P8.dri_nodes(proc.pid):
                                    mark(f"dri nodes at game launch: {r}")
                for pat in ("Unhandled exception", "Unhandled page fault", "wine: Unhandled", "memory fault",
                            "wineserver crashed", "err:virtual", "REDEngineErrorReporter"):
                    if pat in ln and ("crash:" + pat) not in seen:
                        seen.add("crash:" + pat)
                        mark(f"serial row: {ln.strip()[:220]}")
            fps_f.flush()
            thr_f.flush()

        prof_rx = re.compile(r"CPU#(\d+).*?RSP=([0-9a-f]+).*?RIP=([0-9a-f]+).*?CPL=(\d).*?HLT=(\d)", re.S)
        word_rx = re.compile(r"0x([0-9a-f]{16})")

        def sample_regs(tag, secs, hz, stack, sleeper):
            """QMP 'info registers -a' (+ kernel stack words) -> OUT/prof-TAG.txt"""
            n = 0
            with open(os.path.join(args.out, f"prof-{tag}.txt"), "a") as f:
                end = time.time() + secs
                while time.time() < end:
                    r = qmp.cmd("human-monitor-command", {"command-line": "info registers -a"})
                    for blk in r.get("return", "").split("CPU#")[1:]:
                        m = prof_rx.search("CPU#" + blk)
                        if not m:
                            continue
                        cpu, rsp, rip, cpl, hlt = m.groups()
                        extra = ""
                        if stack and cpl == "0" and hlt == "0":
                            x = qmp.cmd("human-monitor-command",
                                        {"command-line": f"x /48gx 0x{rsp}", "cpu-index": int(cpu)})
                            ws = [int(w, 16) for w in word_rx.findall(x.get("return", ""))]
                            ks = [w for w in ws if 0x70000000 <= w < 0x80000000]
                            extra = " k=" + ",".join(f"{w:x}" for w in ks[:16])
                        f.write(f"{time.time() - t0:.2f} {cpu} {rip} {cpl} {hlt}{extra}\n")
                    n += 1
                    sleeper(1.0 / hz)
            return n

        def prompt_ocr(name):
            """the title prompt "PRESS [SPACE] TO CONTINUE" read from its own band
            (game y 540..660, x 300..980, red text kept, 3x up-scaled): on the
            1280x720 scanout glass the full-screen OCR missed it over the busy
            title art (r2-fps-scan1; docs/cp-p7.md's OCR/Space trap)"""
            try:
                from PIL import Image
                im = Image.open(os.path.join(shots_dir, name + ".png")).convert("RGB")
                c = game_view(im, args.xscreen, args.game_res).crop((300, 540, 980, 660))
                # the prompt is red text (~239,79,73): keep red pixels as black ink
                g = Image.new("L", c.size, 255)
                cpx, gpx = c.load(), g.load()
                for y in range(c.size[1]):
                    for x in range(c.size[0]):
                        r, gg, b = cpx[x, y]
                        if r > 170 and gg < 140 and b < 140:
                            gpx[x, y] = 0
                g = g.resize((g.size[0] * 3, g.size[1] * 3))
                cp = os.path.join(shots_dir, name + "-prompt.png")
                g.save(cp)
                r = subprocess.run(["tesseract", cp, "-", "--psm", "6"], capture_output=True, text=True, timeout=30)
                t = " ".join(r.stdout.split())
            except Exception as e:  # noqa: BLE001
                log(f"prompt OCR failed: {e}")
                return False
            hit = bool(P8.PRESS_CONTINUE.search(t))
            log(f"prompt band OCR {name}: {t[:80]!r} -> {hit}")
            return hit

        def menu_ocr(name):
            """the main menu's item column read on its own (canonical layout x
            420..760, y 440..720: red items, the focused one yellow, kept as black
            ink, 2x): over the menu's animated background tesseract read none of
            the items on whole 1280x720-screen shots in some boots (r2-fps-put1)"""
            try:
                from PIL import Image
                im = Image.open(os.path.join(shots_dir, name + ".png")).convert("RGB")
                c = game_view(im, args.xscreen, args.game_res).crop((100, 260, 440, 540))
                g = Image.new("L", c.size, 255)
                cpx, gpx = c.load(), g.load()
                for y in range(c.size[1]):
                    for x in range(c.size[0]):
                        r, gg, b = cpx[x, y]
                        if (r > 160 and gg < 120 and b < 120) or (r > 190 and gg > 150 and b < 110):
                            gpx[x, y] = 0
                g = g.resize((g.size[0] * 2, g.size[1] * 2))
                cp = os.path.join(shots_dir, name + "-menu.png")
                g.save(cp)
                r = subprocess.run(["tesseract", cp, "-", "--psm", "6"], capture_output=True, text=True, timeout=30)
                t = " ".join(r.stdout.split())
            except Exception as e:  # noqa: BLE001
                log(f"menu OCR failed: {e}")
                return ""
            log(f"menu column OCR {name}: {t[:100]!r}")
            return t

        def alive():
            return proc.poll() is None and "[P8] done" not in seen

        # ---- phase 1: boot to the main menu (boot_p8's rules) --------------
        prev_px = None
        baseline = None
        next_shot = 0
        menu_hits = 0
        presses = 0
        last_press = 0
        last_change = time.time()
        last_rows = [0, time.time()]
        stall_sampled = False
        while time.time() - t0 < args.timeout and alive():
            pump()
            # before the game starts the launcher/Wine rows are the progress; when
            # only kernel rows trickle for 120 s, sample where the vCPUs are once
            # (r2-fps-scan3: Wine froze 14 s in, one vCPU pinned at 100 % for 8 min)
            if not st["game_t"] and not stall_sampled:
                prog = st.get("prog_rows", 0)
                if prog != last_rows[0]:
                    last_rows[:] = [prog, time.time()]
                elif time.time() - last_rows[1] > 120:
                    stall_sampled = True
                    mark("no launcher/Wine row for 120 s before the game: sampling the vCPUs (prof-prestall)")
                    sample_regs("prestall", 20, 4, True, time.sleep)
            if "[P8] end game " in seen:
                # H2-early: was "game-exited-before-menu"; now the P8 verdict
                verdict, why = p8grade.before_menu_exit("\n".join(glines)) or (
                    "early-exit", "the game process ended before the main menu")
                mark(f"{verdict.upper()}: {why}")
                break
            gt = st["game_t"]
            if gt and time.time() >= next_shot:
                next_shot = time.time() + args.shot_every
                path = os.path.join(shots_dir, "cur.ppm")
                if qmp.shot(path) and os.path.exists(path):
                    px = P8.ppm(path)
                    if baseline is None:
                        baseline = px
                        mark(f"baseline screendump {px[0]}x{px[1]}")
                    # after a first menu hit, OCR again even if the menu is static
                    if prev_px is None or menu_hits > 0 or P8.diff_frac(prev_px, px) > 0.01:
                        last_change = time.time()
                        dfb = P8.diff_frac(baseline, px)
                        if "first-window" not in seen and dfb > 0.02:
                            seen.add("first-window")
                            mark(f"first screen change after launch ({dfb:.1%} differs from the baseline)")
                        try:
                            name, flat = shoot("pre")
                        except FirstRun as e:
                            mark(f"FIRST-RUN SCREEN '{e.what}' at {e.shot}: stopping without answering it")
                            verdict = f"first-run:{e.what}"
                            break
                        words = [w for w in P8.MENU_WORDS if w in flat.upper()]
                        if len(words) < 2 and name:
                            words = [w for w in P8.MENU_WORDS if w in menu_ocr(name).upper()]
                        if len(words) >= 2:
                            menu_hits += 1
                            mark(f"main menu OCR at {name}: {words}")
                            if menu_hits >= args.menu_hits:
                                verdict = "main-menu"
                                break
                            next_shot = time.time() + 4
                        elif (args.press_continue and presses < 6 and time.time() - last_press > 15
                              and (P8.PRESS_CONTINUE.search(flat) or prompt_ocr(name))):
                            presses += 1
                            last_press = time.time()
                            qmp.cmd("send-key", {"keys": [{"type": "qcode", "data": "spc"}]})
                            mark(f"title prompt 'PRESS ... TO CONTINUE' at {name}: sent one Space (#{presses})")
                    prev_px = px
            if time.time() - last_change > args.stall_s:
                mark(f"STALL before the menu: no screen change for {args.stall_s} s")
                verdict = "stall"
                break
            time.sleep(0.5)

        # ---- phase 2: the play script --------------------------------------
        if verdict == "main-menu":
            set_phase("menu")
            mark("main menu reached: running the play script " + os.path.basename(args.script))
            ctl_path = os.path.join(args.out, "ctl.txt")
            open(ctl_path, "a").close()
            ctl_off = os.path.getsize(ctl_path)
            next_play = time.time() + args.play_shot_every
            held = set()

            def tick(seconds):
                """sleep while keeping the session record; False if the boot must end"""
                nonlocal next_play
                end = time.time() + seconds
                while True:
                    pump()
                    if not alive():
                        raise Abort("launcher-done" if st.get("exit_ok") else "qemu-or-launcher-ended")
                    if "[P8] end game " in seen and not st.get("exit_ok"):
                        raise Abort("game-exited")
                    if time.time() - t0 > args.timeout:
                        raise Abort("timeout")
                    if time.time() >= next_play:
                        next_play = time.time() + args.play_shot_every
                        shoot("rec", do_ocr=True)
                    left = end - time.time()
                    if left <= 0:
                        return True
                    time.sleep(min(0.25, left))

            def expect(tag, timeout, rx, negate=False):
                end = time.time() + timeout
                while True:
                    name, flat = shoot(tag)
                    hit = bool(re.search(rx, flat, re.I))
                    if not hit and not negate and "CONTINUE" in rx and name:
                        hit = prompt_ocr(name)      # the red "PRESS ... TO CONTINUE" band
                    if hit != negate:
                        mark(f"expect {tag}: {'no longer ' if negate else ''}matched /{rx}/ at {name}")
                        return True
                    if time.time() >= end:
                        mark(f"EXPECT FAILED {tag}: /{rx}/ {'still matches' if negate else 'not seen'} "
                             f"within {timeout} s (last {name})")
                        return False
                    if not tick(4):
                        return False


            def prof(secs, hz, tag, stack=False):
                """guest PC sampler: QMP 'info registers -a' at hz for secs; one row
                per vCPU per sample: host_s cpu rip cpl hlt [k=addr,...] -- with
                stack, also the words on a kernel-mode vCPU's stack that look like
                kernel text addresses (0x70000000..0x7fffffff), innermost first,
                i.e. a rough call chain (tools/cp/p9/prof.py).  The samples stop
                the vCPUs briefly, so the phase is 'prof-TAG' while it runs and its
                fps rows do not count as play."""
                prev_phase = st["phase"]
                set_phase(f"prof-{tag}")
                n = sample_regs(tag, secs, hz, stack, tick)
                set_phase(prev_phase)
                mark(f"prof {tag}: {n} samples x vCPUs -> prof-{tag}.txt")

            # Row centres (screen y, 1920x1080 desktop, the game's 1280x720 window at
            # 320,180) of the main and pause menus, measured on ctl1's screendumps.
            # The highlighted row is yellow text in a yellow frame; OCR of that crop
            # was unreliable (MODS -> 'MOOS', QUIT GAME -> 'TT AME'), its y is not.
            def canon(name):
                from PIL import Image
                im = Image.open(os.path.join(shots_dir, name + ".png")).convert("RGB")
                return canon_image(im, args.xscreen, args.game_res)

            def focus_row(tag, menu):
                """the highlighted row of a main/pause menu: (label, y) or (None, y)"""
                name, _ = shoot(tag, do_ocr=False)
                if not name:
                    return None, None
                label, yc, rows = focus_in(canon(name), menu)
                if rows is None:
                    log(f"focus {name}: no highlighted row")
                    return None, None
                y0, y1 = rows
                log(f"focus {name}: rows {y0}..{y1} centre {yc:.0f} -> {label}")
                return label, yc

            def seek(tag, menu, item, direction, limit):
                """DIR is only the first tap's direction when no row is highlighted yet
                (the focus is hidden until the first key); after that the direction
                follows from the highlighted row's index vs ITEM's."""
                names = [lab for lab, _ in MENUS[menu]]
                want = names.index(item)
                for i in range(limit + 1):
                    label, yc = focus_row(f"{tag}{i}", menu)
                    if label == item:
                        mark(f"seek {tag}: {menu} menu focus on {item} after {i} taps")
                        return True
                    d = direction if label is None else ("down" if want > names.index(label) else "up")
                    if i < limit:
                        send([qcode_event(d, True)])
                        tick(st.get("key_hold", 0.2))
                        send([qcode_event(d, False)])
                        tick(st.get("key_gap", 0.8))
                mark(f"SEEK FAILED {tag}: {menu} menu row {item!r} not focused after {limit} x {direction}")
                return False

            def list_focus(tag):
                """the save list's focused entry: centre y of its cyan frame (screen
                rows with > 40 cyan pixels in x 540..1380), or None"""
                name, _ = shoot(tag, do_ocr=False)
                if not name:
                    return None
                im = canon(name)
                px = im.load()
                ys = [y for y in range(250, 900)
                      if sum(1 for x in range(540, 1380, 2)
                             if px[x, y][0] < 120 and px[x, y][1] > 170 and px[x, y][2] > 170) > 40]
                c = (ys[0] + ys[-1]) / 2 if ys and ys[-1] - ys[0] < 120 else None
                log(f"list focus {name}: frame rows {ys[:1]}..{ys[-1:]} centre {c}")
                return c

            def list_seek(tag, direction, limit, want):
                for i in range(limit + 1):
                    c = list_focus(f"{tag}{i}")
                    if c is not None and abs(c - want) <= 15:
                        mark(f"listseek {tag}: entry at y={c:.0f} focused after {i} taps")
                        return True
                    d = direction if c is None else ("down" if want > c else "up")
                    if i < limit:
                        send([qcode_event(d, True)])
                        tick(st.get("key_hold", 0.2))
                        send([qcode_event(d, False)])
                        tick(st.get("key_gap", 0.8))
                mark(f"LISTSEEK FAILED {tag}: no entry focused at y={want} after {limit} taps")
                return False

            def run_lines(lines, depth=0):
                """returns None to go on, or a verdict string"""
                i = 0
                while i < len(lines):
                    raw = lines[i].strip()
                    i += 1
                    if not raw or raw.startswith("#"):
                        continue
                    # plain whitespace split (shlex would eat the regexes' backslashes:
                    # gateA1 failed on /LOADs*GAME/)
                    w = raw.split()
                    op, a = w[0], w[1:]
                    if op == "repeat":
                        body, nest, j = [], 1, i
                        while j < len(lines):
                            t = lines[j].strip().split()
                            if t and t[0] == "repeat":
                                nest += 1
                            elif t and t[0] == "end":
                                nest -= 1
                                if nest == 0:
                                    break
                            body.append(lines[j])
                            j += 1
                        i = j + 1
                        for _ in range(int(a[0])):
                            r = run_lines(body, depth + 1)
                            if r:
                                return r
                        continue
                    if op == "wait":
                        if not tick(float(a[0])):
                            return "game-exited"
                    elif op == "key":
                        # REDengine's UI drops taps that come faster than ~0.5 s
                        # apart (ctl1: 9 ups 0.27 s apart moved the focus 2-3
                        # rows; 0.8 s apart, every tap counts), and in the world
                        # at 5-15 fps a 120 ms tap can fall between two frames.
                        for q in a:
                            send([qcode_event(q, True)])
                            tick(st.get("key_hold", 0.2))
                            send([qcode_event(q, False)])
                            tick(st.get("key_gap", 0.8))
                    elif op == "keytiming":
                        st["key_hold"], st["key_gap"] = float(a[0]) / 1000.0, float(a[1]) / 1000.0
                    elif op == "prof":
                        prof(float(a[0]), float(a[1]) if len(a) > 1 else 10.0, a[2] if len(a) > 2 else st["phase"],
                             stack=(len(a) > 3 and a[3] == "stack"))
                    elif op == "hold":
                        send([qcode_event(a[0], True)])
                        ok = tick(int(a[1]) / 1000.0)
                        send([qcode_event(a[0], False)])
                        if not ok:
                            return "game-exited"
                    elif op == "down":
                        send([qcode_event(a[0], True)])
                        held.add(a[0])
                    elif op == "up":
                        send([qcode_event(a[0], False)])
                        held.discard(a[0])
                    elif op == "look":
                        dx, dy = int(a[0]), int(a[1])
                        n = int(a[2]) if len(a) > 2 else 1
                        ms = int(a[3]) if len(a) > 3 else 16
                        for k in range(n):
                            ex = dx * (k + 1) // n - dx * k // n
                            ey = dy * (k + 1) // n - dy * k // n
                            ev = []
                            if ex:
                                ev.append({"type": "rel", "data": {"axis": "x", "value": ex}})
                            if ey:
                                ev.append({"type": "rel", "data": {"axis": "y", "value": ey}})
                            if ev:
                                send(ev)
                            if not tick(ms / 1000.0):
                                return "game-exited"
                    elif op == "click":
                        b = a[0] if a else "left"
                        ms = int(a[1]) if len(a) > 1 else 120
                        send([{"type": "btn", "data": {"down": True, "button": b}}])
                        tick(ms / 1000.0)
                        send([{"type": "btn", "data": {"down": False, "button": b}}])
                    elif op == "shot":
                        shoot(a[0] if a else "shot")
                    elif op in ("expect", "expect_not"):
                        if not expect(a[0], float(a[1]), raw.split(None, 3)[3], negate=(op == "expect_not")):
                            return f"expect-failed:{a[0]}" if alive() and "[P8] end game " not in seen \
                                else "game-exited"
                    elif op == "seek":
                        # seek TAG MENU DIR MAX ITEM...: tap DIR until MENU's row ITEM is highlighted
                        if not seek(a[0], a[1], raw.split(None, 5)[5], a[2], int(a[3])):
                            return f"seek-failed:{a[0]}"
                    elif op == "listseek":
                        # listseek TAG DIR MAX Y: focus the save-list entry whose frame centre is Y
                        if not list_seek(a[0], a[1], int(a[2]), float(a[3])):
                            return f"listseek-failed:{a[0]}"
                    elif op == "unwind":
                        # unwind TAG MAX REGEX: while the screen's OCR matches REGEX (a
                        # map/hub/menu), press Esc (Esc steps map -> hub -> world)
                        rx = raw.split(None, 3)[3]
                        for k in range(int(a[1]) + 1):
                            name, flat = shoot(f"{a[0]}{k}")
                            if not re.search(rx, flat, re.I):
                                mark(f"unwind {a[0]}: clear after {k} x Esc")
                                break
                            if k == int(a[1]):
                                return f"unwind-failed:{a[0]}"
                            send([qcode_event("esc", True)])
                            tick(0.2)
                            send([qcode_event("esc", False)])
                            tick(3)
                    elif op == "phase":
                        set_phase(a[0])
                    elif op == "mark":
                        mark("script: " + " ".join(a))
                    elif op == "exit_ok":
                        st["exit_ok"] = True       # the game is about to quit on purpose
                    elif op == "wait_exit":
                        st["exit_ok"] = True
                        end = time.time() + float(a[0])
                        while time.time() < end and "[P8] end game " not in seen and alive():
                            tick(1)
                        if "[P8] end game " not in seen:
                            return "exit-timeout"
                    elif op == "stop":
                        return "script-stop"
                    elif op == "ctl":
                        nonlocal ctl_off
                        mark(f"interactive: append commands to {ctl_path}; 'done' ends")
                        while True:
                            if not tick(0.5):
                                return "game-exited"
                            sz = os.path.getsize(ctl_path)
                            if sz <= ctl_off:
                                continue
                            with open(ctl_path) as f:
                                f.seek(ctl_off)
                                new = f.read()
                            if not new.endswith("\n"):
                                continue
                            ctl_off = sz
                            cmds = new.splitlines()
                            for c in cmds:
                                mark(f"ctl: {c.strip()}")
                            if any(c.strip() == "done" for c in cmds):
                                r = run_lines([c for c in cmds if c.strip() != "done"], depth + 1)
                                if r:
                                    return r
                                break
                            r = run_lines(cmds, depth + 1)
                            if r:
                                return r
                    else:
                        mark(f"script: unknown op {op!r} (ignored)")
                return None

            try:
                r = run_lines(script)
            except FirstRun as e:
                mark(f"FIRST-RUN SCREEN '{e.what}' at {e.shot}: stopping without answering it")
                r = f"first-run:{e.what}"
            except Abort as e:
                r = str(e)
            for q in list(held):
                send([qcode_event(q, False)])
            if r is None:
                verdict = "script-done"
            else:
                verdict = r
            if verdict in ("game-exited", "launcher-done") and st.get("exit_ok"):
                verdict = "script-done"
            mark(f"play script ended: {verdict}")
            if verdict == "game-exited":
                e = p8grade.game_end("\n".join(glines))
                why = "the game ended during the play script (after the menu)" + (": " + p8grade.describe(e) if e else "")
                mark(f"verdict detail: {why}")
            # let the launcher finish (wineserver -k, sync) if the game has quit
            if "[P8] end game " in seen:
                end = time.time() + 180
                while time.time() < end and alive():
                    pump()
                    time.sleep(1)
                pump()
        if verdict == "timeout" and "[P8] done" in seen:
            verdict, why = p8grade.launcher_end_verdict("\n".join(glines), "game")
            mark(f"{verdict.upper()}: {why}")
        if proc.poll() is not None and verdict == "timeout":
            verdict = f"qemu-exited rc={proc.returncode}"
        if proc.poll() is None:
            try:
                shoot("final")
            except Exception as e:  # noqa: BLE001
                log(f"final screendump: {e}")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
        if big is not None:
            big.close()
        P8.release_slot(lockf)
        mark(f"verdict: {verdict}; QEMU stopped, slot released; guests on the host now: {P8.guests_running()}")
        for f in (tl, fps_f, mem_f, thr_f, ph_f, hbp_f):
            f.close()
    open(os.path.join(args.out, "verdict.txt"), "w").write(verdict + "\n" + (why + "\n" if why else ""))
    if verdict in ("script-done", "script-stop", "launcher-done"):
        return 0
    return 3 if verdict == "early-exit" else 1


if __name__ == "__main__":
    sys.exit(main())
