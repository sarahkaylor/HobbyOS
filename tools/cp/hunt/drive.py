#!/usr/bin/env python3
"""tools/cp/hunt/drive.py -- drive an interactive P9 boot (boot_p9.py `ctl`,
tools/cp/hunt/settings.p9) from the host with feedback, for the sequences a
fixed play script cannot do (lane H6-hunt3, docs/cp-hunt.md round 3):

    drive.py churn    RUN   [--loads 10] [--menu-loads 5] [--saves 10]
    drive.py menuloads RUN  [--menu-loads 5]
    drive.py settings RUN   [--cycles 3]
    drive.py quit     RUN   [--from pause|main]

The boot's own `seek` / `expect` end the boot when they fail (h3-ctl1,
h3-ctl3), so this driver sends only boot_p9's input / wait / shot lines
through OUT/ctl.txt and reads every result itself from the shots (numbers and
OCR text only; the shots stay on strix):

  * menu focus: the yellow highlight next to a label that tesseract found
    (position-independent: works for a 1280x720 game on a 1920x1080 X screen,
    a 1024x768 window, or a 1920x1080 game);
  * the save list: boot_p9's cyan-frame rows and the save name OCR'd inside
    the focused frame (QuickSave-3, ManualSave-86, ...);
  * the settings rows: the focused row's faint cyan tint right of its label;
  * waits: OCR until a regex matches or a timeout passes, never fatal.

Every step is printed as one line ("[DRV] ...") with host time; the run log
(OBJ/hunt-logs/RUN.log) has boot_p9's side.  Only in-game menus are driven.
The game's first-run screens never appear here (boot_p9 stops the boot on
one), and the GAMEPLAY settings tab is never entered: its "Share game
telemetry" row trips boot_p9's first-run guard (cp-hunt H6-01).
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
from PIL import Image, ImageStat  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p9"))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p8"))
import boot_p9 as B9  # noqa: E402  (focus_in / canon_image / MENUS: what boot_p9's seek sees)

# the game picture's place: P9_XSCREEN and the game's own Resolution (the
# settings boot changes the latter; Boot.view tracks it)
VIEW = {"xscreen": os.environ.get("P9_XSCREEN", "1280x720") or "1920x1080",
        "game_res": os.environ.get("DRIVE_VIEW_RES", "1280x720"), "res": "1280x720", "windowed": False}

# OCR spellings seen: Quicksove-1, ManuolSave-88, MonualSave-87 (h3-churn4)
SAVE_RX = re.compile(r"\b(Qu\w{1,3}k|Au\w{0,2}o|M\w{1,2}nu\w{1,2}l|Point\w*|End\w*)\s*S\w{0,2}ve-(\d+)", re.I)


def save_name(m):
    k = m.group(1).lower()
    kind = "QuickSave" if k.startswith("qu") else "AutoSave" if k.startswith("au") else \
        "ManualSave" if k.startswith("m") else m.group(1) + "Save"
    return f"{kind}-{m.group(2)}"
# the HUD after a load: the tracked quest + its objective (h3-ctl1..3 OCR), or
# the prologue's tutorial popup.  The save list itself says "The Ripperdoc" /
# "The Streetkid", so a screen that still shows the list or a dialog never counts.
HUD_RX = r"Jackie|Weapon|Crouch|Skip\s+Ahead|K\w?rk|second\s+floor|Move\s+with"
WORLD_RX = (rf"RIPPERD[O0]C.{{0,120}}({HUD_RX})|({HUD_RX}).{{0,120}}RIPPERD[O0]C|"
            rf"STREETKI[DO0].{{0,120}}({HUD_RX})|WELCOME\s+TO|First\s+time\s+in|"
            # the bottom-right input hints alone: after an in-world load the
            # quest title can stay hidden for minutes (h3-churn1 load 2: 391 s
            # by the title, ~90 s by these)
            r"Draw\s+Weapon.{0,60}Crouch")
NOT_WORLD_RX = r"LOAD\s*GAME|Latest\s+save|Play\s*time|sure\s+you\s+want|PRESS\b.{0,24}\bTO\s+CONTINUE"


class Ended(Exception):
    pass


def log(msg):
    print(f"[DRV] {time.strftime('%H:%M:%S')} {msg}", flush=True)


class Boot:
    def __init__(self, run, logs=None, max_s=3600):
        self.run = run
        self.deadline = time.time() + max_s
        self.out = os.path.join(ROOT, "obj", "cp-p9", run)
        self.ctl = os.path.join(self.out, "ctl.txt")
        self.log = os.path.join(logs or os.path.join(ROOT, "obj", "hunt-logs"), run + ".log")
        self.n = 0
        # tags stay unique when a driver is restarted on the same boot (a tag
        # whose shot already exists would be "found" at once: h3-churn1)
        self.pre = "d" + format(int(time.time()) % 46656, "x")
        if not os.path.isdir(self.out):
            raise SystemExit(f"drive: no {self.out}")
        self.states = os.path.join(self.out, "drive-states.txt")
        self.cur_state = None

    def state(self, name):
        """round 4 (H9-hunt4): what the boot is doing now, for glasswatch.py's
        report (a frozen glass counts as a hitch only in a world-like state)"""
        if name != self.cur_state:
            with open(self.states, "a") as f:
                f.write(f"{time.time():.3f} {name}\n")
            # and boot_p9's own phase, so fps.csv / classify's per-phase rows
            # follow the drive (h4-p01's whole session read as "menu")
            with open(self.ctl, "a") as f:
                f.write(f"phase {name}\n")
            self.cur_state = name

    def ended(self):
        try:
            return "verdict: " in open(self.log, errors="replace").read()
        except OSError:
            return True

    def shot(self, name, *lines, timeout=180):
        """append LINES + `shot TAG`; wait for the PNG; -> its path"""
        if time.time() > self.deadline:
            raise Ended("the driver's own deadline (--max-s) passed")
        self.n += 1
        tag = f"{self.pre}{self.n:03d}{name}"
        with open(self.ctl, "a") as f:
            for ln in lines:
                f.write(ln + "\n")
            f.write(f"shot {tag}\n")
        end = time.time() + timeout
        pat = os.path.join(self.out, "shots", f"s*-t*-{tag}.png")
        while time.time() < end:
            g = glob.glob(pat)
            if g and (os.path.exists(g[0][:-4] + ".ocr.txt") or time.time() > end - timeout + 30):
                time.sleep(0.2)
                return g[0]
            if self.ended():
                raise Ended(f"the boot ended (waiting for {tag})")
            time.sleep(0.5)
        raise Ended(f"no shot {tag} in {timeout} s")

    def until(self, name, rx, timeout, *first, every=4, reject=None):
        """first lines, then shots `every` s apart until the OCR matches rx
        (and not REJECT)"""
        end = time.time() + timeout
        p = self.shot(name, *first)
        while True:
            t = boot_ocr(p)
            if not re.search(rx, t, re.I | re.S):
                t = t + " " + ocr(p, scale=2)       # tesseract reads small HUD text better at 2x
            if re.search(rx, t, re.I | re.S) and not (reject and re.search(reject, t, re.I | re.S)):
                return p, t
            if time.time() >= end:
                log(f"until {name}: /{rx}/ not seen in {timeout} s (last OCR {t[:80]!r})")
                return None, t
            p = self.shot(name, f"wait {every}")


# --- readings -------------------------------------------------------------------
def boot_ocr(path):
    """boot_p9's own OCR of the shot (psm 11, bare + in the 1920x1080 canon
    layout), which it saves beside the PNG; the HUD reads there where a plain
    psm 6 pass does not (h3-churn1)"""
    try:
        return " ".join(open(path[:-4] + ".ocr.txt", errors="replace").read().split())
    except OSError:
        return ocr(path, psm=11)



def ocr(path, box=None, scale=1, psm=6, tsv=False):
    im = Image.open(path).convert("L")
    if box:
        im = im.crop(box)
    if scale != 1:
        im = im.resize((int(im.size[0] * scale), int(im.size[1] * scale)))
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
        im.save(tf.name)
    try:
        r = subprocess.run(["tesseract", tf.name, "-", "--psm", str(psm)] + (["tsv"] if tsv else []),
                           capture_output=True, text=True, timeout=60).stdout
    finally:
        os.unlink(tf.name)
    if not tsv:
        return " ".join(r.split())
    words = []
    for ln in r.splitlines()[1:]:
        p = ln.split("\t")
        if len(p) == 12 and p[11].strip() and float(p[10]) > 20:
            x, y, w, h = (int(int(v) / scale) for v in p[6:10])
            if box:
                x, y = x + box[0], y + box[1]
            words.append((p[11], x, y, w, h))
    return words


def find_labels(words, label):
    """every (x, y, w, h) where LABEL's words follow each other on one line"""
    parts = label.upper().split()
    out = []
    for i, (t, x, y, w, h) in enumerate(words):
        if t.upper().strip(".,:") != parts[0]:
            continue
        ok = True
        for k, p in enumerate(parts[1:], 1):
            if i + k >= len(words) or words[i + k][0].upper().strip(".,:") != p or abs(words[i + k][2] - y) > 8:
                ok = False
                break
        if ok:
            xe = words[i + len(parts) - 1][1] + words[i + len(parts) - 1][3]
            out.append((x, y, xe - x, h))
    return out


def find_label(words, label):
    f = find_labels(words, label)
    return f[0] if f else None


def column_boxes(words, labels):
    """one box per label, all in the same text column: a label's word can also
    occur in a description elsewhere on the screen ("...monitor's resolution.")"""
    cands = {lab: find_labels(words, lab) for lab in labels}
    xs = sorted(c[0] for v in cands.values() for c in v)
    if not xs:
        return {}
    col = xs[len(xs) // 2]
    out = {}
    for lab, v in cands.items():
        best = min(v, key=lambda c: abs(c[0] - col)) if v else None
        if best and abs(best[0] - col) < 80:
            out[lab] = best
    return out


def menu_focus(path, labels):
    """the highlighted (yellow) menu row among LABELS: boot_p9's own focus_in()
    on the canon view while the game picture's place is known, else the yellow
    pixels next to each label tesseract found (h3-ctl3: a 1024x768 window at
    x=128 made boot_p9's tables miss and its seek end the boot)"""
    im = Image.open(path).convert("RGB")
    if not VIEW["windowed"]:
        menu = "pause" if "RESUME" in labels else "main"
        try:
            lab, yc, rows = B9.focus_in(B9.canon_image(im, VIEW["xscreen"], VIEW["game_res"]), menu)
        except Exception:  # noqa: BLE001
            lab = None
        if lab in labels:
            return lab, 999
    words = ocr(path, scale=2, psm=11, tsv=True)
    px = im.load()
    best = (None, 0)
    for lab, (x, y, w, h) in column_boxes(words, labels).items():
        n = 0
        for yy in range(max(0, y - 10), min(im.size[1], y + h + 10)):
            for xx in range(max(0, x - 40), min(im.size[0], x + w + 220), 2):
                r, g, bb = px[xx, yy]
                if r > 140 and g > 95 and bb < 90:
                    n += 1
        if n > best[1]:
            best = (lab, n)
    return best if best[1] > 40 else (None, best[1])


def list_focus(path):
    """the save list's cyan focus frame in the shot's own coordinates: (y0, y1) or None"""
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    ys = [y for y in range(int(H * 0.08), int(H * 0.98))
          if sum(1 for x in range(int(W * 0.17), int(W * 0.83), 2)
                 if px[x, y][0] < 120 and px[x, y][1] > 170 and px[x, y][2] > 170) > W // 32]
    if not ys or ys[-1] - ys[0] > H // 5:
        return None
    if ys[-1] - ys[0] < H // 24:
        # one 1-3 px cyan line: the list has scrolled and the focused entry's
        # frame is clipped by the list's viewport, only its top border (low on
        # the screen) or its bottom border (high) shows (h3-churn2: y 520..522
        # while the OCR'd names stepped QuickSave-3, -2, -1 ...); an entry is
        # ~70 px tall at 720p
        e = int(H * 0.097)
        return (ys[0], min(H - 1, ys[0] + e)) if ys[0] > H // 2 else (max(0, ys[-1] - e), ys[-1])
    return ys[0], ys[-1]


# the saves this lane's boots can have on their data disk: the user's two,
# QuickSave-0..4 (F5 rotates over five slots), ManualSave-87.. from SAVE GAME
KNOWN_SAVES = {"AutoSave-16", "ManualSave-86"} | {f"QuickSave-{i}" for i in range(5)} | \
    {f"ManualSave-{i}" for i in range(87, 100)}


def list_name(path, fr):
    """the save name inside the focused frame (or None for a group header):
    tesseract over three versions of the frame band (plain; green channel
    contrast-stretched; binarised at 4x), and a name from KNOWN_SAVES wins
    (h3-churn4: one entry read "ManualSave-67" plain and "ManualSave-87"
    binarised; "Quicksove-1", "MonualSave-88")"""
    from PIL import ImageOps
    im = Image.open(path).convert("RGB")
    W, H = im.size
    y0, y1 = fr
    box = (int(W * 0.15), max(0, y0 - 6), int(W * 0.85), min(H, y1 + 6))
    texts = [ocr(path, box=box, scale=3, psm=6)]
    g = ImageOps.autocontrast(im.crop(box).getchannel("G"))
    l4 = ImageOps.autocontrast(im.crop(box).convert("L"))
    l4 = ImageOps.invert(l4.resize((l4.size[0] * 4, l4.size[1] * 4), Image.LANCZOS).point(lambda v: 255 if v > 140 else 0))
    for x in (g.resize((g.size[0] * 3, g.size[1] * 3)), l4):
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
            x.save(tf.name)
        try:
            texts.append(" ".join(subprocess.run(["tesseract", tf.name, "-", "--psm", "6"], capture_output=True,
                                                 text=True, timeout=60).stdout.split()))
        finally:
            os.unlink(tf.name)
    names = [save_name(m) for t in texts for m in SAVE_RX.finditer(t)]
    known = [n for n in names if n in KNOWN_SAVES]
    nm = known[0] if known else (names[0] if names else None)
    return nm, " | ".join(texts)


# VIDEO tab rows: label (x, top y) in a 1280x720 game picture at the origin (h3-ctl3)
VIDEO_TABLE = {"Monitor": (332, 165), "VSync": (332, 205), "Maximum FPS": (332, 245),
               "Windowed Mode": (330, 281), "Resolution": (332, 323), "HDR Mode": (330, 360),
               # fit-only rows: in Fullscreen (the panel at 1.5x over 1920x1080) tesseract
               # read none of the rows above, only these (h3-set2)
               "NVIDIA Reflex": (330, 399), "HOR Mode": (330, 360), "FSR Anti-Lag": (332, 478),
               "FSR Anti-Log": (332, 478), "3D Mode": (331, 516), "30 Mode": (331, 516)}
FIT_ROWS = ["NVIDIA Reflex", "HOR Mode", "FSR Anti-Lag", "FSR Anti-Log", "3D Mode", "30 Mode"]


def table_fit(words, labels, table):
    """scale + offset of a settings panel from the labels OCR found in their
    column: (sc, ox, oy) or None"""
    cb = column_boxes(words, labels)
    fit = [(table[l], (b[0], b[1])) for l, b in cb.items() if l in table]
    if len(fit) < 2:
        return None
    (X0, Y0), (x0, y0) = min(fit, key=lambda f: f[0][1])
    (X1, Y1), (x1, y1) = max(fit, key=lambda f: f[0][1])
    sc = (y1 - y0) / (Y1 - Y0) if Y1 != Y0 else 1.0
    if not 0.5 < sc < 2.5:
        return None
    ox = sorted(x - sc * X for (X, Y), (x, y) in fit)[len(fit) // 2]
    oy = sorted(y - sc * Y for (X, Y), (x, y) in fit)[len(fit) // 2]
    return sc, ox, oy


def row_value(path, row, labels=None, table=None):
    """OCR of one VIDEO row's value (the "< 1280x720 >" part right of its label)"""
    labels, table = labels or VIDEO_ROWS, table or VIDEO_TABLE
    words = ocr(path, scale=2, psm=11, tsv=True)
    f = table_fit(words, labels + [l for l in table if l not in labels], table)
    if not f:
        return ""
    sc, ox, oy = f
    X, Y = table[row]
    im = Image.open(path)
    box = (int(ox + sc * 600), max(0, int(oy + sc * (Y - 8))), min(im.size[0], int(ox + sc * 960)), int(oy + sc * (Y + 22)))
    # the focused row's text is dim on its tint: the green channel, contrast-
    # stretched, reads it best (h3-ctl3: "1024x768" where L read "waases")
    from PIL import ImageOps
    out = [ocr(path, box=box, scale=3, psm=7)]
    g = ImageOps.autocontrast(Image.open(path).convert("RGB").crop(box).getchannel("G"))
    g = g.resize((g.size[0] * 4, g.size[1] * 4))
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
        g.save(tf.name)
    try:
        out.append(" ".join(subprocess.run(["tesseract", tf.name, "-", "--psm", "7"], capture_output=True,
                                           text=True, timeout=60).stdout.split()))
    finally:
        os.unlink(tf.name)
    if row == "Resolution":
        # the WxH text is ~8 px tall on a 1920x1080 screen: tesseract reads it
        # only binarised and inverted at 5x, digits-only (h3-set2: "1280x720"
        # where the plain passes gave "te0x720")
        rb = (int(ox + sc * 640), max(0, int(oy + sc * (Y - 6))), int(ox + sc * 900), int(oy + sc * (Y + 18)))
        x = ImageOps.autocontrast(Image.open(path).convert("L").crop(rb))
        x = ImageOps.invert(x.resize((x.size[0] * 5, x.size[1] * 5), Image.LANCZOS).point(lambda v: 255 if v > 128 else 0))
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
            x.save(tf.name)
        try:
            out.append(" ".join(subprocess.run(["tesseract", tf.name, "-", "--psm", "7", "-c",
                                                "tessedit_char_whitelist=0123456789x"],
                                               capture_output=True, text=True, timeout=60).stdout.split()))
        finally:
            os.unlink(tf.name)
    return " / ".join(out)


def set_row(path, labels, table=None):
    """the focused settings row: its band carries a faint cyan tint, (g - r) of
    the band x 300..900 about +17 against -5..-20 for the others (h3-ctl3).
    With a TABLE the rows are placed from the labels OCR found (scale and
    offset fitted, so the panel can sit anywhere and at 1.5x); a focused
    label often does not OCR at all, so without a table a label missing from
    the column is the fallback answer."""
    im = Image.open(path).convert("RGB")
    words = ocr(path, scale=2, psm=11, tsv=True)
    cb = column_boxes(words, labels)
    rows = {}
    if table:
        cb = column_boxes(words, labels + [l for l in table if l not in labels])
        fit = [(table[l], (b[0], b[1])) for l, b in cb.items() if l in table]
        if len(fit) >= 2:
            (X0, Y0), (x0, y0) = min(fit, key=lambda f: f[0][1])
            (X1, Y1), (x1, y1) = max(fit, key=lambda f: f[0][1])
            sc = (y1 - y0) / (Y1 - Y0) if Y1 != Y0 else 1.0
            if 0.5 < sc < 2.5:
                ox = sorted(x - sc * X for (X, Y), (x, y) in fit)[len(fit) // 2]
                oy = sorted(y - sc * Y for (X, Y), (x, y) in fit)[len(fit) // 2]
                # local contrast: the row band against thin strips above and below it
                # (the panel is translucent, so the world behind shifts every row's
                # level; h3-ctl3: focused +17..+22, unfocused -12..+8)
                res = []
                for l in labels:
                    if l not in table:
                        continue
                    X, Y = table[l]

                    def gr(y0, y1):
                        r, g, bb = ImageStat.Stat(im.crop((int(ox + sc * 300), int(oy + sc * y0),
                                                          int(ox + sc * 900), int(oy + sc * y1)))).mean
                        return g - r
                    res.append((round(gr(Y - 3, Y + 15) - (gr(Y - 16, Y - 9) + gr(Y + 22, Y + 27)) / 2, 1), l))
                res.sort(reverse=True)
                if res and res[0][0] >= 13 and (len(res) < 2 or res[0][0] - res[1][0] >= 5):
                    return res[0][1], res[:3]
                return None, res[:3]
    sc_ = []
    for l, (x, y, w, h) in cb.items():
        x1 = x + int(im.size[0] * 0.45)

        def gr(y0, y1):
            r, g, bb = ImageStat.Stat(im.crop((x, max(0, y0), x1, max(1, y1)))).mean
            return g - r
        sc_.append((round(gr(y - 6, y + h + 6) - (gr(y - 19, y - 12) + gr(y + h + 13, y + h + 18)) / 2, 1), l))
    sc_.sort(reverse=True)
    if sc_ and sc_[0][0] >= 13 and (len(sc_) < 2 or sc_[0][0] - sc_[1][0] >= 5):
        return sc_[0][1], sc_[:3]
    if not table:
        miss = [l for l in labels if l not in cb]
        if len(miss) == 1 and len(cb) >= 2:
            return miss[0], sc_[:3] + [("missing", miss[0])]
    return None, sc_[:3]


# --- sequences -------------------------------------------------------------------
# every row, in order (h3-churn1: without MODS / CREDITS a focus on MODS had no
# index, so the seek never turned back up to LOAD GAME)
PAUSE = [lab for lab, _ in B9.MENUS["pause"]]
MAIN = [lab for lab, _ in B9.MENUS["main"]]


WIGGLE = [True]


def seek(b, labels, want, limit=10, name="sk"):
    """tap Up/Down until WANT is highlighted (direction from the label order)"""
    for i in range(limit + 1):
        p = b.shot(f"{name}{i}", "wait 0.6")
        lab, n = menu_focus(p, labels)
        log(f"seek {want}: tap {i} focus {lab}")
        if lab == want:
            log(f"seek {want}: on it after {i} taps")
            return True
        d = "down" if lab is None or labels.index(want) > labels.index(lab) else "up"
        if lab is None and i >= 2 and WIGGLE[0]:
            # cp-hunt H1-12: no visible focus after a sub-menu -> a relative mouse wiggle
            # (round 4: --no-wiggle measures H6-04, which says the wiggle hides it)
            b.shot(f"{name}w{i}", "look 60 0 1 16", "look -60 0 1 16", "wait 0.5")
        b.shot(f"{name}k{i}", f"key {d}")
    log(f"SEEK {want}: not reached in {limit} taps")
    return False


PAUSE_RX = (r"RESUME|EXIT\s+TO\s+MAIN|"
            # the pause menu's bottom bar: after the prologue save the rows
            # themselves did not OCR over that scene (h3-churn3: 5 loads lost)
            # (not "CD PROJEKT RED ACCOUNT": the main menu has that too)
            r"u\w{0,2}cks\w{0,2}ve\s+\S{0,3}\s*select")


def pause_open(b):
    b.state("menu:pause")
    p = b.shot("po", "key esc", "wait 4")
    if re.search(PAUSE_RX, boot_ocr(p), re.I):
        return True
    p = b.shot("po2", "wait 2")          # once more: a popup may have eaten the Esc
    if re.search(PAUSE_RX, boot_ocr(p), re.I):
        return True
    p = b.shot("po3", "key esc", "wait 4")
    return bool(re.search(PAUSE_RX, boot_ocr(p), re.I))


def in_world(b, name, timeout):
    """wait for the HUD; close the prologue's tutorial popup with Esc if it shows"""
    p, t = b.until(name, WORLD_RX, timeout, reject=NOT_WORLD_RX)
    if not p:
        return None
    if re.search(r"WELCOME\s+TO|First\s+time\s+in", t, re.I):
        p2, t2 = b.until(name + "e", WORLD_RX.replace(r"|WELCOME\s+TO|First\s+time\s+in", ""), 30,
                         "wait 3", "key esc", "wait 3")
        if re.search(r"RESUME|EXIT\s+TO\s+MAIN", t2 or "", re.I):
            b.shot(name + "r", "key esc", "wait 3")
        return "STREETKID" if p2 else None
    return "RIPPERDOC" if re.search(r"RIPPERD[O0]C|Jackie", t, re.I) else "STREETKID"


def open_list(b):
    """from the LOAD GAME list just opened: expand the groups"""
    p, t = b.until("ll", r"LOAD\s*GAME|Expand|Progression", 30, "wait 3")
    b.shot("l0", "key 0", "wait 2")
    return p is not None


LIST_STEPS = [24]


def list_pick(b, avoid, prefer=None, steps=24):
    """walk the list down from the top; return the first focused save whose
    name is not in AVOID (or is PREFER), with Enter not yet pressed"""
    seen = []
    # Down first, then Up: the list opens on the current save, and Down wraps
    # from the last entry back to the first entry of the CURRENT group -- the
    # newer group above it (in-session manual saves) is reached only with Up
    # (h3-churn4: 40 Downs cycled QuickSave-4..AutoSave-16 and never met
    # ManualSave-87..92, which the list's own first screen showed)
    for d, n in (("down", steps), ("up", steps)):
        for i in range(n):
            p = b.shot(f"ls{d[0]}{i}", f"key {d}", "wait 1.2")
            fr = list_focus(p)
            if not fr:
                continue
            nm, _ = list_name(p, fr)
            seen.append(nm)
            if nm and ((prefer and nm == prefer) or (not prefer and nm not in avoid)):
                log(f"list: focus on {nm} (frame rows {fr[0]}..{fr[1]}) after {i + 1} x {d}")
                return nm
            if d == "down" and nm and seen.count(nm) >= 2:
                break          # Down has come round once: try Up
    log(f"list: no pick (seen {seen})")
    return None


def make_saves(b, n):
    """n quick saves (F5) at different places, ~10 s of walking apart"""
    for i in range(n):
        b.shot(f"mv{i}", "down shift", "hold w 3000", "up shift", "look 300 0 10 16", "hold w 2000",
               "look -150 0 5 16", "wait 1")
        p = b.shot(f"qs{i}", "key f5", "wait 4")
        log(f"quick save {i + 1}/{n}: OCR {boot_ocr(p)[:60]!r}")


def manual_save(b, k):
    """pause -> SAVE GAME -> the list's NEW SAVE entry -> Enter (docs/cp-p9.md:
    SAVE GAME -> NEW SAVE wrote ManualSave-87 in ctl1; Return on an existing
    entry would ask to overwrite it, so the focused frame must read NEW SAVE)"""
    if not pause_open(b) or not seek(b, PAUSE, "SAVE GAME", name=f"sg{k}_"):
        b.shot("bk", "key esc", "wait 3")
        return False
    b.shot(f"sgr{k}", "key ret", "wait 3")
    for i in range(6):
        p = b.shot(f"sgl{k}_{i}", "key down", "wait 1.2")
        fr = list_focus(p)
        if not fr:
            continue
        _, t = list_name(p, fr)
        if re.search(r"NEW\s*SAVE", t, re.I):
            p, t = b.until(f"sgw{k}", r"overwrite|sure", 4, "key ret", "wait 3")
            if p:
                log(f"manual save {k}: a dialog ({t[:60]!r}): cancelled")
                b.shot("bk", "key esc", "wait 2")
            else:
                log(f"manual save {k}: NEW SAVE taken after {i + 1} taps")
            b.shot(f"sgx{k}", "wait 3", "key esc", "wait 3", "key esc", "wait 3")
            return not p
    log(f"manual save {k}: no NEW SAVE entry focused")
    b.shot("bk", "key esc", "wait 3", "key esc", "wait 3")
    return False


def load_in_world(b, avoid, k):
    if not pause_open(b) and not (recover(b, f"rc{k}_") and pause_open(b)):
        log(f"load {k}: the pause menu did not open")
        return None
    if not seek(b, PAUSE, "LOAD GAME", name=f"pl{k}_"):
        b.shot("bk", "key esc", "wait 3")
        return None
    b.shot(f"plr{k}", "key ret", "wait 3")
    open_list(b)
    nm = list_pick(b, avoid, steps=LIST_STEPS[0])
    if not nm:
        b.shot("bk", "key esc", "wait 3", "key esc", "wait 3")
        return None
    t0 = time.time()
    p, t = b.until(f"cf{k}", r"sure.{0,30}load", 15, "key ret", "wait 2")
    b.state("load")
    if p:
        b.shot(f"cfy{k}", "key ret", "wait 10")
    w = in_world(b, f"w{k}_", 400)
    log(f"LOAD {k} (pause menu -> {nm}): {'world ' + w if w else 'NO WORLD'} after {time.time() - t0:.0f} s")
    b.state("world" if w else "?")
    if w:
        b.shot(f"walk{k}", "hold w 3000", "look 400 0 20 16", "hold w 2000", "wait 2")
    return nm if w else None


def exit_to_main(b, k):
    if not (pause_open(b) or (recover(b, f"rx{k}_") and pause_open(b))) or \
            not seek(b, PAUSE, "EXIT TO MAIN MENU", name=f"xm{k}_"):
        return False
    t0 = time.time()
    p, _ = b.until(f"xc{k}", r"sure.{0,20}quit", 15, "key ret", "wait 2")
    if not p:
        return False
    b.state("exit-main")            # round 4: H6-06's long frame is in this transition
    p, _ = b.until(f"tt{k}", r"PRESS\b.{0,24}\bTO\s+CONTINUE", 400, "key ret", "wait 5")
    log(f"EXIT TO MAIN MENU {k}: title after {time.time() - t0:.0f} s" if p else f"EXIT {k}: no title")
    if not p:
        return False
    p, _ = b.until(f"mm{k}", r"LOAD\s*GAME", 60, "key spc", "wait 5")
    b.state("menu:main")
    return p is not None


def load_from_main(b, k, prefer):
    b.state("menu:main")
    if not seek(b, MAIN, "LOAD GAME", name=f"ml{k}_"):
        return None
    b.shot(f"mlr{k}", "key ret", "wait 3")
    open_list(b)
    nm = list_pick(b, set(), prefer=prefer) or list_pick(b, set())
    if not nm:
        return None
    t0 = time.time()
    b.state("load")
    p, _ = b.until(f"ld{k}", r"PRESS\b.{0,24}\bTO\s+CONTINUE", 400, "key ret", "wait 10")
    if not p:
        log(f"MAIN LOAD {k} ({nm}): no PRESS ... TO CONTINUE")
        return None
    w = in_world(b, f"mw{k}_", 90) if b.shot(f"sp{k}", "key spc", "wait 5") else None
    log(f"MAIN LOAD {k} (main menu -> {nm}): {'world ' + w if w else 'NO WORLD'} after {time.time() - t0:.0f} s")
    if w:
        b.state("world")
    return nm if w else None


def quit_game(b, frm):
    if frm == "main":
        if not exit_to_main(b, 99):
            return False
        if not seek(b, MAIN, "QUIT GAME", name="mq"):
            return False
    else:
        if not (pause_open(b) or (recover(b, "rq") and pause_open(b))) or not seek(b, PAUSE, "QUIT GAME", name="pq"):
            return False
    p, _ = b.until("qc", r"sure.{0,20}quit", 15, "exit_ok", "key ret", "wait 2")
    if not p:
        return False
    log("QUIT GAME confirmed; waiting for the game to exit")
    with open(b.ctl, "a") as f:
        f.write("key ret\nwait_exit 180\n")
    return True


def churn(b, a):
    """item 3: quick saves, then A.loads different saves loaded through the
    pause menu, then A.menu_loads x (EXIT TO MAIN MENU -> LOAD GAME)"""
    if not a.in_world:
        w = in_world(b, "w0_", 30)
        if not w:
            # from the main menu: LOAD GAME -> ManualSave-86
            if not load_from_main(b, 0, "ManualSave-86"):
                raise Ended("no world at the start")
    make_saves(b, a.saves)
    ms = sum(1 for k in range(1, a.manual_saves + 1) if manual_save(b, k))
    if a.manual_saves:
        log(f"manual saves: {ms}/{a.manual_saves}")
    loaded, ok = list(a.avoid or []), 0
    for k in range(1, a.loads + 1):
        nm = load_in_world(b, set(loaded), k)
        if nm:
            ok += 1
            loaded.append(nm)
    log(f"CHURN pause-menu loads: {ok}/{a.loads} reached the world, {len(set(loaded))} different saves: {loaded}")
    menu_loads(b, a)


def menu_loads(b, a):
    """A.menu_loads x (pause -> EXIT TO MAIN MENU -> title -> LOAD GAME ->
    ManualSave-86 / AutoSave-16 alternately -> world), then QUIT GAME"""
    if not in_world(b, "mw0_", 20) and not load_from_main(b, 0, "ManualSave-86"):
        raise Ended("no world at the start")
    mok = 0
    prefs = ["ManualSave-86", "AutoSave-16"]
    for k in range(1, a.menu_loads + 1):
        if exit_to_main(b, k) and load_from_main(b, k, prefs[k % 2]):
            mok += 1
    log(f"CHURN exit-to-main-menu -> load: {mok}/{a.menu_loads}")
    quit_game(b, "pause")


VIDEO_ROWS = ["Monitor", "VSync", "Maximum FPS", "Windowed Mode", "Resolution", "HDR Mode", "NVIDIA Reflex"]


TAB_RX = {
    "VIDEO": r"Windowed\s+Mode|Maximum\s+FPS|VSync",
    "GRAPHICS": r"Quick\s+Preset|Texture\s+Quality|Resolution\s+Scaling|Ray\s+Tracing",
    "SOUND": r"Controller\s+Speaker|Master\s+Volume|Dialogue\s+Volume|Music\s+Volume",
}
SETTINGS_STATE = {"opened": False}


def settings_tab(b, p):
    t = ocr(p, scale=2, psm=11) + " " + boot_ocr(p)
    for tab, rx in TAB_RX.items():
        if re.search(rx, t, re.I):
            return tab
    return None


def settings_open(b, name):
    """pause -> SETTINGS -> the VIDEO tab, never through GAMEPLAY: its "Share
    game telemetry" row stops the boot (cp-hunt H6-01).  Tab order (in the
    world): SOUND CONTROLS GAMEPLAY GRAPHICS VIDEO INTERFACE KEY BINDINGS
    ACCESSIBILITY UTILITIES, wrapping; key 1 = left, key 3 = right.  The game
    reopens Settings on the tab it was closed on, and a fresh session on
    SOUND, so: SOUND -> exactly 5 x left; GRAPHICS -> 1 x right; VIDEO ->
    done; anything this cannot name -> Esc out (a blind tap could land on
    GAMEPLAY: KEY BINDINGS and UTILITIES have too little text to read)"""
    if not pause_open(b) or not seek(b, PAUSE, "SETTINGS", name=name + "s"):
        return False
    p = b.shot(name + "o", "key ret", "wait 4")
    tab = None
    for i in range(3):
        tab = settings_tab(b, p)
        if tab:
            break
        p = b.shot(name + f"o{i}", "wait 2")
    log(f"settings opened on {tab}")
    if tab == "SOUND":
        p = b.shot(name + "l", *(["key 1", "wait 2"] * 5))
        tab = settings_tab(b, p)
    elif tab == "GRAPHICS":
        p = b.shot(name + "r", "key 3", "wait 2")
        tab = settings_tab(b, p)
    if tab == "VIDEO":
        SETTINGS_STATE["opened"] = True
        return True
    log(f"settings: not on VIDEO ({tab}); closing")
    b.shot(name + "q", "key esc", "wait 3")
    return False


def read_value(b, name, row, labels=None, table=None):
    """ROW's value read with the focus moved off it: the focused row's text is
    too dim on its tint for tesseract at 1280x720-in-1920x1080 (h3-set2: 20
    Enters, no value read), the unfocused one reads ("1280x720")"""
    p = b.shot(name, "key down", "wait 1.0")
    t = row_value(p, row, labels, table)
    if row == "Resolution":
        t += " // " + " ".join(re.findall(r"\d{3,4}\s*x\s*\d{3,4}", ocr(p, scale=2, psm=11)))
    b.shot(name + "u", "key up", "wait 1.0")
    return t


# round 4: what a menu / hub / dialog screen reads (any of these: not the world)
MENU_SCREEN_RX = (r"RESUME|EXIT\s+TO\s+MAIN|QUIT\s*GAME|SAVE\s+GAME|SKIP\s+TIME|DATABASE|STREET\s+CRED|"
                  r"Track\s+waypoint|BACKPACK|apply\s+these|LOAD\s*GAME|Quicks\w{1,2}ve|SETTINGS|"
                  r"Windowed\s+Mode|Quick\s+Preset|VSync|Master\s+Volume|Texture\s+Quality|Zoom\s*out|"
                  r"UPPER\s+BODY|restore\s+def|sure\s+you\s+want")


def is_world(p, t=None):
    """round 4 (h4-p02): the HUD phrases of WORLD_RX are missed by tesseract on
    some street views, and recover() then toggled the pause menu with Esc
    (journal 2/3 "back NO").  World = WORLD_RX, or the hint bar reading
    Crouch / Draw Weapon, or no menu words at all, in both OCR passes"""
    t = boot_ocr(p) if t is None else t
    if re.search(NOT_WORLD_RX + r"|RESUME|apply", t, re.I | re.S) or re.search(MENU_SCREEN_RX, t, re.I):
        return False
    if re.search(WORLD_RX, t, re.I | re.S):
        return True
    h = region_ocr(p, ("hints",))
    if re.search(r"Crouch|[DO]raw\s*Weapon|Weapon\s*\[", h, re.I):
        return True
    return not re.search(MENU_SCREEN_RX, ocr(p, scale=2), re.I)


def recover(b, name="rc"):
    """back to the world from whatever menu or dialog is up: Esc (which also
    cancels an apply prompt) until the HUD's input hints show"""
    # its own drive state: the Esc it presses opens / closes the pause menu, whose
    # static frames are not world hitches (h4-p02's two "walk" gaps were this)
    b.state("recover")
    for i in range(6):
        p = b.shot(f"{name}{i}", "wait 1")
        t = boot_ocr(p)
        if is_world(p, t):
            log(f"recover: in the world after {i} steps")
            return True
        if re.search(r"apply\s+these\s+changes", t, re.I):
            log("recover: an apply prompt: CONFIRM (a throwaway data disk; Esc would only bring it back)")
            b.shot(f"{name}y{i}", "key ret", "wait 4")
            continue
        b.shot(f"{name}e{i}", "key esc", "wait 3")
    return False


def video_set(b, row, value_rx, name, max_enter=20):
    """focus ROW on the VIDEO tab, press Enter until its value matches
    value_rx, then Esc + CONFIRM; -> (changed, seconds to the next good shot)"""
    for i in range(14):
        p = b.shot(f"{name}f{i}", "key down", "wait 1.2")
        lab, sc = set_row(p, VIDEO_ROWS, VIDEO_TABLE)
        if lab == row:
            break
    else:
        log(f"settings: row {row} not focused ({sc})")
        return False
    seen = []
    for j in range(max_enter):
        t = read_value(b, f"{name}r{j}", row)
        seen.append(t)
        if re.search(value_rx, t, re.I):
            break
        p = b.shot(f"{name}e{j}", "key ret", "wait 1.5")
    else:
        log(f"settings: {row} never showed /{value_rx}/ (values seen: {seen})")
        return False
    log(f"settings: {row} shows {t!r} after {j} x Enter (values seen: {seen})")
    t0 = time.time()
    p, t = b.until(f"{name}a", r"apply\s+these\s+changes", 10, "key esc", "wait 2")
    if not p:
        log(f"settings: {row} -> /{value_rx}/ already current (no apply prompt)")
        b.shot(f"{name}x", "wait 1")
        return True
    b.shot(f"{name}c", "key ret", "wait 4")
    # where the game picture is now (menu_focus's canon view)
    if row == "Resolution":
        m = re.search(r"(\d{3,4})\D{0,3}x\D{0,3}(\d{3,4})", value_rx.replace("\\s*", ""))
        VIEW["res"] = f"{m.group(1)}x{m.group(2)}" if m else VIEW["res"]
        if not VIEW.get("fullscreen"):
            VIEW["game_res"] = VIEW["res"]
    elif row == "Windowed Mode":
        VIEW["windowed"] = not re.search(r"Borderless|Fullscreen", value_rx.split("(?!")[0])
        # Fullscreen scales the picture over the whole 1920x1080 X screen (h3-set2:
        # the Settings panel at 1.5x), whatever the game's own resolution
        VIEW["fullscreen"] = bool(re.search(r"Fullscreen", value_rx))
        VIEW["game_res"] = "1920x1080" if VIEW["fullscreen"] else VIEW["res"]
    log(f"SET {row} -> /{value_rx}/ applied (view now {VIEW})")
    return t0


GRAPHICS_ROWS = ["Quick Preset", "Resolution Scaling", "Frame Generation"]
# GRAPHICS tab rows (x, top y) in the 1280x720 game picture (h3-set2's shot at
# 320,180 of 1920x1080).  "Quick Preset" is also the section title 44 px above
# its row, so the row is placed from the other, unique labels
GRAPHICS_TABLE = {"Quick Preset": (330, 163), "Resolution Scaling": (332, 205), "Frame Generation": (332, 245),
                  "Crowd Density": (332, 494)}


def graphics_preset(b, name):
    """GRAPHICS tab (one LEFT of VIDEO): Enter on Quick Preset cycles it (cp-hunt
    round 2: Right moves the focus instead); -> the new preset's OCR"""
    if not settings_open(b, name):
        return None
    p = b.shot(name + "g", "key 1", "wait 2")
    if settings_tab(b, p) != "GRAPHICS":
        log("settings: GRAPHICS tab not where expected; closing")
        return None
    # the tab opens with the focus already on its first row, Quick Preset
    # (h3-set2: its band +8.4 over the others before any key; a Down scrolls
    # the list and the table no longer fits), so Enter without moving
    before = row_value(p, "Quick Preset", GRAPHICS_ROWS, GRAPHICS_TABLE)
    # h3-set3: Enter right after the tab switch changed nothing 3/3 (the focus is
    # hidden until the first key); one Down shows it on the first row (h3-set2:
    # the band still on Quick Preset after it), then Enter
    # h3-set4: the first Down lands on the "Quick Preset" SECTION title, whose
    # Enter asks "Do you want to restore default settings for this section?"
    # (cancelled with Esc, 3/3); the row is the second Down
    p = b.shot(name + "gd", "key down", "wait 1.2", "key down", "wait 1.2")
    p = b.shot(name + "ge", "key ret", "wait 2")
    if re.search(r"restore\s+def", boot_ocr(p), re.I):
        log("settings: Enter opened the section's restore-defaults prompt: CANCEL")
        b.shot(name + "gx", "key esc", "wait 2")
        return None
    after = row_value(p, "Quick Preset", GRAPHICS_ROWS, GRAPHICS_TABLE) + " // " + " ".join(
        re.findall(r"Quick\s+Preset\W{0,6}(\w[\w ]{2,30})", boot_ocr(p)))
    pa, _ = b.until(name + "ga", r"apply\s+these\s+changes", 10, "key esc", "wait 2")
    if pa:
        b.shot(name + "gc", "key ret", "wait 5")
    log(f"SET Quick Preset {before!r} -> {after!r} ({'applied' if pa else 'no apply prompt'})")
    return after


def settings(b, a):
    """item 4: resolution 720p <-> 1080p (needs P9_XSCREEN=1920x1080 so the
    game offers 1920x1080), Windowed Mode cycled and back, the graphics
    Quick Preset changed and back; between changes, walk and shoot"""
    if a.in_world:
        recover(b)
    elif not in_world(b, "w0_", 30) and not load_from_main(b, 0, "ManualSave-86"):
        raise Ended("no world at the start")
    seqs = [("Resolution", r"1920\s*x\s*1080"), ("Resolution", r"1280\s*x\s*720"),
            ("Windowed Mode", r"\bFullscreen\b"), ("Windowed Mode", r"Windowed\s+Borderless"),
            ("Windowed Mode", r"\bWindowed\b(?!\s+Borderless)"), ("Windowed Mode", r"Windowed\s+Borderless")]
    if a.seq:
        seqs = [tuple(x.split("=", 1)) for x in a.seq.split(";") if "=" in x]
    for c in range(a.cycles):
        for k, (row, rx) in enumerate(seqs):
            if not settings_open(b, f"c{c}{k}"):
                b.shot("bk", "key esc", "wait 3", "key esc", "wait 3")
                continue
            r = video_set(b, row, rx, f"c{c}{k}v")
            # close Settings and the pause menu, then ~10 s in the world
            b.shot(f"c{c}{k}z", "key esc", "wait 3", "key esc", "wait 3")
            p = b.shot(f"c{c}{k}w", "hold w 3000", "look 300 0 10 16", "wait 3")
            log(f"after {row} -> /{rx}/ ({'ok' if r else 'FAILED'}): world OCR {boot_ocr(p)[:60]!r}")
        for k in range(a.presets):
            g = graphics_preset(b, f"c{c}p{k}")
            b.shot(f"c{c}p{k}z", "key esc", "wait 3", "key esc", "wait 3")
            p = b.shot(f"c{c}p{k}w", "hold w 3000", "look 300 0 10 16", "wait 3")
            log(f"after Quick Preset -> {g}: world OCR {boot_ocr(p)[:60]!r}")
        log(f"SETTINGS cycle {c + 1}/{a.cycles} done")
    quit_game(b, "pause")


def game_im(p):
    """the shot's game picture at 1280x720 (boot_p9's game_view)"""
    return B9.game_view(Image.open(p).convert("RGB"), VIEW["xscreen"], VIEW["game_res"])


def motion(p1, p2):
    """round 4: how much the world moved between two shots: mean |diff| (0-255)
    of 96x54 grey thumbnails of the picture's centre (the HUD corners cut off).
    Walking into a wall reads ~1-3, walking or turning in the open 10+."""
    if not p1 or not p2:
        return 0.0
    from PIL import ImageChops
    box = (160, 100, 1120, 560)
    t1 = game_im(p1).crop(box).convert("L").resize((96, 54))
    t2 = game_im(p2).crop(box).convert("L").resize((96, 54))
    return ImageStat.Stat(ImageChops.difference(t1, t2)).mean[0]


# the interaction prompt (right of the crosshair), the hint bar (bottom right)
# and the notifications (top left), in the 1280x720 game picture
REGIONS = {"prompt": (640, 250, 1120, 520), "hints": (880, 540, 1280, 720), "notes": (0, 60, 640, 300)}


def region_ocr(p, which=("prompt", "hints")):
    g = game_im(p)
    out = []
    for w in which:
        im = g.crop(REGIONS[w])
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
            im.save(tf.name)
        try:
            out.append(w + ":" + ocr(tf.name, scale=2, psm=6))
        finally:
            os.unlink(tf.name)
    return " | ".join(out)


def ocr_all(p):
    """boot_p9's OCR and a 2x pass, flattened (the HUD hints are small)"""
    return " ".join((boot_ocr(p) + " " + ocr(p, scale=2)).split())


def explore(b, a):
    """round 4: each in-world key once, with the OCR logged in full, to learn
    the screens the play loop has to recognise (map, inventory, journal,
    character, hub, phone, the vehicle call)"""
    if not in_world(b, "w0_", 30) and not load_from_main(b, 0, a.save):
        raise Ended("no world at the start")
    b.state("world")
    p = b.shot("x0", "wait 2")
    log(f"EXPLORE world: {ocr_all(p)[:400]!r}")
    for key, name in (("m", "map"), ("i", "inventory"), ("j", "journal"), ("p", "character"),
                      ("tab", "hub"), ("t", "phone")):
        b.state("menu:" + name)
        p = b.shot("x" + name, f"key {key}", "wait 4")
        log(f"EXPLORE {name} ({key}): {ocr_all(p)[:400]!r}")
        p = b.shot("x" + name + "c", "key esc", "wait 3")
        log(f"EXPLORE {name} after esc: {ocr_all(p)[:200]!r}")
        if not recover(b, "xr" + name):
            log(f"EXPLORE {name}: recover failed")
        b.state("world")
    b.state("vehicle-call")
    p = b.shot("xv", "key v", "wait 2")
    log(f"EXPLORE vehicle call (v): {ocr_all(p)[:300]!r}")
    for i in range(6):
        p = b.shot(f"xv{i}", "wait 4")
        log(f"EXPLORE vehicle +{4 * (i + 1)} s: {ocr_all(p)[:300]!r}")
    for i in range(8):
        p = b.shot(f"xl{i}", "look 300 0 10 16", "wait 1")
        log(f"EXPLORE look {i}: {ocr_all(p)[:300]!r}")
    b.state("world")
    log("EXPLORE done")


def explore2(b, a):
    """round 4: walking with feedback (motion between shots; turn when stuck),
    the vehicle call (tap / hold V) and a search for an enter prompt, the phone
    (hold T); every reading logged"""
    if not in_world(b, "w0_", 30) and not load_from_main(b, 0, a.save):
        raise Ended("no world at the start")
    b.state("walk")
    prev = b.shot("e0", "wait 1")
    for i in range(20):
        p = b.shot(f"ew{i}", "down shift", "hold w 3000", "up shift")
        m = motion(prev, p)
        log(f"EXPLORE2 walk {i}: motion {m:.1f} {region_ocr(p, ('prompt', 'hints', 'notes'))[:260]!r}")
        if m < 6:
            q = b.shot(f"et{i}", "look 500 0 20 16", "wait 0.5")
            log(f"EXPLORE2 turn {i}: motion {motion(p, q):.1f}")
            p = q
        prev = p
    b.state("vehicle-call")
    for tag, lines in (("tap", ["key v"]), ("hold", ["hold v 1500"])):
        p = b.shot(f"ev{tag}", *lines, "wait 1.5")
        log(f"EXPLORE2 vehicle {tag}: {region_ocr(p, ('prompt', 'hints', 'notes'))[:300]!r} // {boot_ocr(p)[:200]!r}")
        if tag == "hold":
            p = b.shot("evhx", "key esc", "wait 2")
            log(f"EXPLORE2 after esc: {boot_ocr(p)[:160]!r}")
    for i in range(5):
        p = b.shot(f"evw{i}", "wait 5")
        log(f"EXPLORE2 vehicle wait {5 * (i + 1)} s: {region_ocr(p, ('prompt', 'notes'))[:200]!r}")
    for i in range(12):
        p = b.shot(f"evs{i}", "look 250 0 10 16", "wait 1")
        r = region_ocr(p, ("prompt", "hints"))
        log(f"EXPLORE2 search {i}: {r[:220]!r}")
        if re.search(r"Enter|Get\s*in|Steal|Drive|Vehicle", r, re.I):
            q = b.shot(f"evf{i}", "key f", "wait 5")
            log(f"EXPLORE2 F at {i}: {region_ocr(q, ('prompt', 'hints', 'notes'))[:300]!r} // {boot_ocr(q)[:200]!r}")
            for j in range(3):
                q = b.shot(f"evd{i}{j}", "hold w 3000", "hold a 800")
                log(f"EXPLORE2 drive? {j}: motion {motion(p, q):.1f} {region_ocr(q, ('hints',))[:200]!r}")
                p = q
            break
    b.state("menu:phone")
    p = b.shot("eph", "hold t 1000", "wait 2")
    log(f"EXPLORE2 phone (hold t): {boot_ocr(p)[:300]!r}")
    b.shot("ephx", "key esc", "wait 3")
    recover(b, "ephr")
    b.state("world")
    log("EXPLORE2 done")


# --- round 4 (H9-hunt4): active play ------------------------------------------------
# in-world menus: key, what its screen reads (h4-x01's OCR).  Esc from each
# lands on the hub (LEVEL UP / SKIP TIME / DATABASE), a second Esc (recover)
# on the world
PLAY_MENUS = [
    ("map", "m", r"Track\s+waypoint|Zoom\s*out|FILTER|Jump\s+to|PRIMARY\s+GANGS"),
    ("inventory", "i", r"BACKPACK|UPPER\s+BODY|QUICK\s+ACCESS|LOWER\s+BODY|OUTFIT"),
    ("journal", "j", r"TALK\s+TO\s+JACKIE|BEAT\s+ON\s+THE\s+BRAT|OPEN\s+MAP|Go\s+to\s+the\s+fight"),
    # not the character screen (P): it opens on a "your Perks have been reset"
    # popup whose Esc leaves the pause menu up, and recover() could not close it
    # (h4-p01: 2/2)
    # not the phone: neither a tap of T (h4-x01) nor a 700 ms hold (h4-p01, 0/3)
    # opened anything in ManualSave-86, and the Esc after it opens the pause menu
]
# walk segments: (lines) -- sprint, walk, strafe, jump, crouch, turn and look up/down
WALKS = [
    ["down shift", "hold w 4000", "up shift", "look 400 0 20 16"],
    ["hold w 2500", "hold a 1200", "look -300 40 15 16", "look 0 -40 5 16", "hold w 2000"],
    ["down shift", "hold w 3000", "up shift", "key spc", "hold w 1500", "hold d 1200", "look 600 0 30 16"],
    ["key c", "hold w 2000", "key c", "look -500 0 25 16", "down shift", "hold w 3000", "up shift"],
    ["hold s 1500", "look 250 -30 10 16", "look 0 30 5 16", "down shift", "hold w 4000", "up shift"],
]


class Tally:
    def __init__(self):
        self.c = {}

    def add(self, k, ok):
        e = self.c.setdefault(k, [0, 0])
        e[1] += 1
        e[0] += 1 if ok else 0

    def s(self, k):
        e = self.c.get(k, [0, 0])
        return f"{e[0]}/{e[1]}"


def walk_seg(b, k, prev, tally):
    b.state("walk")
    if prev and re.search(PAUSE_RX, boot_ocr(prev), re.I):
        # a menu step left the pause menu up (h4-p01): back to the world first
        tally.add("recover-before-walk", recover(b, f"pwr{k}_"))
        b.state("walk")
    p = b.shot(f"pw{k}", *WALKS[k % len(WALKS)])
    m = motion(prev, p)
    moved = m >= 6
    tally.add("walk", moved)
    if not moved:
        # stuck against something: turn about 90-180 degrees and try again
        p = b.shot(f"pt{k}", f"look {700 + 300 * (k % 3)} 0 30 16", "down shift", "hold w 2500", "up shift")
        tally.add("unstuck", motion(prev, p) >= 6)
    in_w = is_world(p)
    if k % 5 == 0:
        log(f"PLAY walk {k}: motion {m:.1f} world-hud {'yes' if in_w else 'no'}")
    return p


def menu_seg(b, k, tally):
    name, key, rx = PLAY_MENUS[k % len(PLAY_MENUS)]
    b.state("menu:" + name)
    p = b.shot(f"pm{name}{k}", key if " " in key else f"key {key}", "wait 3")
    t = boot_ocr(p)
    ok = bool(re.search(rx, t, re.I))
    if not ok:
        t = t + " " + ocr(p, scale=2)
        ok = bool(re.search(rx, t, re.I))
    if ok and name == "map":
        # pan and zoom: the map streams its own tiles
        b.shot(f"pmz{k}", "look 300 150 15 16", "hold e 800", "look -300 -150 15 16", "hold q 800", "wait 1")
    p = b.shot(f"pmx{k}", "key esc", "wait 3")
    back = recover(b, f"pmr{k}_")
    tally.add("menus", ok and back)
    tally.add("menu:" + name, ok)
    if not (ok and back):
        log(f"PLAY menu {name}: opened {'yes' if ok else 'NO'} back {'yes' if back else 'NO'} (OCR {t[:100]!r})")
    b.state("world")
    return back


VEHICLE = {"ok": None}


def vehicle_seg(b, k, tally, drive_s):
    """call the vehicle (V), find the enter prompt, drive DRIVE_S seconds, get
    out (F).  -> seconds driven (0 if not entered)"""
    b.state("vehicle-call")
    p = b.shot(f"pv{k}", "key v", "wait 6")
    entered = False
    for i in range(10):
        p = b.shot(f"pvs{k}_{i}", "look 250 0 10 16", "wait 1")
        r = region_ocr(p, ("prompt",))
        if re.search(r"Enter|Get\s*in|Steal|Drive", r, re.I):
            q = b.shot(f"pvf{k}_{i}", "key f", "wait 5")
            h = region_ocr(q, ("hints",))
            entered = bool(re.search(VEH_RX, h + " " + boot_ocr(q), re.I))
            log(f"PLAY vehicle {k}: prompt at search {i} ({r[:60]!r}); after F: {'IN' if entered else 'not in'} ({h[:80]!r})")
            break
    tally.add("vehicle", entered)
    if not entered:
        b.state("world")
        return 0
    b.state("drive")
    t0 = time.time()
    prev = q
    j = 0
    while time.time() - t0 < drive_s:
        q = b.shot(f"pvd{k}_{j}", "down w", "wait 3", "hold a 700", "wait 2", "hold d 900", "wait 2", "up w",
                   "look 200 0 10 16", "down w", "wait 2", "up w")
        tally.add("drive-moving", motion(prev, q) >= 6)
        prev, j = q, j + 1
    d = time.time() - t0
    b.state("vehicle-exit")
    b.shot(f"pvx{k}", "hold s 2500", "key spc", "wait 2", "key f", "wait 4")
    b.state("world")
    return d


VEH_RX = r"Exit\s+vehicle|Handbrake|Radio|Exit\s+\[|km/h|MPH|Accelerate"


def quit_now(b, how):
    """the session's end: QUIT GAME from the pause menu (the confirm, then the
    game must exit); HOW only labels it"""
    b.state("quit:" + how)
    ok = quit_game(b, "pause")
    if not ok:
        # once more from the world (a held key or a popup can hide the pause
        # menu's focus: h4-p01)
        b.shot("pqu", "up w", "up shift", "key esc", "wait 3")
        recover(b, "pqur")
        ok = quit_game(b, "pause")
    log(f"PLAY QUIT ({how}): {'confirmed' if ok else 'FAILED to reach QUIT GAME'}")
    return ok


def exit_vehicle(b, tally):
    """QUIT GAME in the middle of a drive: the throttle held, Esc, QUIT"""
    for k in range(1):
        if vehicle_seg(b, 900 + k, tally, 20):
            break
    else:
        log("PLAY exit vehicle: never in a vehicle; quitting while sprinting instead")
        # Esc in mid-sprint; the keys are let go once the pause menu is up (held,
        # W and Shift hid the menu's focus for 10 taps: h4-p01)
        b.state("quit:sprint")
        # Shift is let go before the Esc: IceWM grabs Shift+Esc (its window menu,
        # cp-hunt H9-03), so Esc goes in with W still held, mid-walk
        b.shot("pqs", "down shift", "down w", "wait 3", "up shift", "key esc", "wait 2", "up w", "wait 2")
        # h4-p01 / h4-p04: no focus here for 10 taps; is it seek's own pointer
        # wiggle (H6-04's mechanism)?  The first seek moves no pointer
        w0, WIGGLE[0] = WIGGLE[0], False
        ok = seek(b, PAUSE, "QUIT GAME", name="pqss")
        WIGGLE[0] = w0
        log(f"PLAY sprint pause seek without the wiggle: {'focus found' if ok else 'NO focus'}")
        if not ok:
            return quit_now(b, "sprint-retry")
        p, _ = b.until("pqsc", r"sure.{0,20}quit", 15, "exit_ok", "key ret", "wait 2")
        if not p:
            return quit_now(b, "sprint-retry")
        log("PLAY QUIT (sprint): confirmed; waiting for the game to exit")
        with open(b.ctl, "a") as f:
            f.write("key ret\nwait_exit 180\n")
        return True
    b.state("drive")
    b.shot("pqd", "down w", "wait 3", "key esc", "wait 2", "up w", "wait 2")
    return quit_now(b, "vehicle")


def exit_load(b, tally):
    """QUIT GAME during a save load: pause -> LOAD GAME -> a save -> confirm;
    Esc tried on the loading screen (no menu is expected there), then QUIT GAME
    from the pause menu at the first moment the world's HUD is back (the
    streaming of the loaded world still running)"""
    if not pause_open(b) or not seek(b, PAUSE, "LOAD GAME", name="pqlg"):
        return quit_now(b, "load-fallback")
    b.shot("pqlr", "key ret", "wait 3")
    open_list(b)
    nm = list_pick(b, set(), prefer="ManualSave-86") or list_pick(b, set())
    p, _ = b.until("pqlc", r"sure.{0,30}load", 15, "key ret", "wait 2")
    if p:
        b.shot("pqly", "key ret", "wait 1")
    b.state("load")
    t0 = time.time()
    p = b.shot("pqle", "wait 4", "key esc", "wait 2")
    t = boot_ocr(p)
    log(f"PLAY exit load: {nm}; Esc on the loading screen -> {t[:80]!r}")
    if re.search(PAUSE_RX, t, re.I):
        log("PLAY exit load: a pause menu during the load: QUIT from it")
        return quit_now(b, "load-menu")
    p, t = b.until("pqlw", WORLD_RX, 400, every=2, reject=NOT_WORLD_RX)
    log(f"PLAY exit load: HUD after {time.time() - t0:.0f} s: quitting at once")
    if re.search(r"WELCOME\s+TO", t or "", re.I):
        b.shot("pqlx", "key esc", "wait 1")
    return quit_now(b, "load")


def exit_settings(b, tally, a):
    """QUIT GAME right after a settings apply: VIDEO Windowed Mode or a Quick
    Preset changed + CONFIRMed, Settings and the pause menu closed, QUIT"""
    t0 = None
    if a.exit_setting == "preset":
        g = graphics_preset(b, "pqs")
        t0 = time.time() if g else None
        log(f"PLAY exit settings: Quick Preset -> {g}")
    elif a.exit_setting == "resolution2":
        # Resolution away and back, the quit right after the second apply: the
        # pause menu is then read at the layout its tables were measured on
        # (h4-p03: after one 1024x768 apply the rows were read at the wrong
        # place; h4-p09: the VSync row read Off/30/60, i.e. not VSync, and the
        # Enters started the game's benchmark)
        if settings_open(b, "pqs"):
            # with a 1920x1080 X screen (P9_XSCREEN) the game offers 1920x1080 and
            # H6's h3-set2 path holds (10/10); at 1024x768 in a 1280x720 screen the
            # rows could not be placed again (h4-p11)
            away = r"1920\s*x\s*1080" if VIEW["xscreen"] == "1920x1080" else r"1024\s*x\s*768"
            r1 = video_set(b, "Resolution", away, "pqsa")
            r2 = video_set(b, "Resolution", r"1280\s*x\s*720", "pqsb") if r1 else None
            t0 = r2 or None
            log(f"PLAY exit settings: Resolution -> /{away}/ ({'applied' if r1 else 'NOT'}) -> 1280x720 "
                f"({'applied' if r2 else 'NOT'})")
    elif a.exit_setting == "vsync":
        # a setting that does not move the picture: after a Resolution apply the
        # pause menu's rows are read at the wrong place (h4-p03: 1024x768 in the
        # 1280x720 X screen, the seek "found" QUIT GAME on the world)
        if settings_open(b, "pqs"):
            cur = read_value(b, "pqsv", "VSync")
            want = r"\bOff\b" if re.search(r"\bOn\b", cur) else r"\bOn\b"
            t0 = video_set(b, "VSync", want, "pqsr")
            log(f"PLAY exit settings: VSync {cur[:30]!r} -> /{want}/ ({'applied' if t0 else 'NOT applied'})")
    else:
        if settings_open(b, "pqs"):
            cur = read_value(b, "pqsv", "Resolution")
            want = r"1280\s*x\s*720" if not re.search(r"1280\s*x\s*720", cur) else r"1024\s*x\s*768"
            t0 = video_set(b, "Resolution", want, "pqsr")
            log(f"PLAY exit settings: Resolution {cur[:30]!r} -> /{want}/ ({'applied' if t0 else 'NOT applied'})")
    p = b.shot("pqsz", "key esc", "wait 2")
    b.state("quit:settings")
    # the Settings screen closes back onto the pause menu: QUIT GAME from there
    # (unless the Esc went on to the world)
    if is_world(p) or not re.search(PAUSE_RX, boot_ocr(p), re.I):
        log("PLAY exit settings: not on the pause menu after the Esc; QUIT through pause_open")
        return quit_now(b, "settings")
    if not seek(b, PAUSE, "QUIT GAME", name="pqsq"):
        return quit_now(b, "settings-fallback")
    p, _ = b.until("pqsc", r"sure.{0,20}quit", 15, "exit_ok", "key ret", "wait 2")
    if not p:
        return False
    log(f"PLAY QUIT (settings): confirmed {time.time() - t0:.1f} s after the apply" if t0 else "PLAY QUIT (settings): confirmed (no apply)")
    with open(b.ctl, "a") as f:
        f.write("key ret\nwait_exit 180\n")
    return True


PRESET_RX = re.compile(r"Ray\s*Tracing\W{0,3}(Low|Medium|Ultra|Overdrive)|Steam\s*Deck|\b(Low|Medium|High|Ultra|Custom)\b", re.I)


def preset_name(t):
    m = PRESET_RX.search(t or "")
    if not m:
        return None
    if m.group(1):
        return "RT " + m.group(1).title()
    return m.group(0).title().replace("Steamdeck", "Steam Deck")


def presets(b, a):
    """round 4 item 4: the GRAPHICS Quick Preset changed in the world A.cycles
    times (Enter on the row steps to the next preset; graphics_preset), each
    applied, then ~15 s of walking on it; the names read back after each step"""
    if not in_world(b, "w0_", 30) and not load_from_main(b, 0, a.save):
        raise Ended("no world at the start")
    b.state("world")
    seen = []
    for k in range(a.cycles):
        b.state("settings")
        g = graphics_preset(b, f"qp{k}_")
        nm = preset_name(g.split(" // ")[-1] if g else None) or preset_name(g)
        seen.append(nm or "?")
        b.shot(f"qp{k}z", "key esc", "wait 3", "key esc", "wait 3")
        if not recover(b, f"qp{k}r"):
            log(f"PRESET {k}: not back in the world")
        b.state("walk")
        p = b.shot(f"qp{k}w", "down shift", "hold w 3000", "up shift", "look 400 0 20 16", "hold w 3000", "wait 4")
        log(f"PRESET {k + 1}/{a.cycles}: {nm} ({'applied' if g else 'NOT changed'}); world OCR {boot_ocr(p)[:60]!r}")
    log(f"PRESETS SUMMARY seen={','.join(seen)} distinct={len(set(seen) - {'?'})}")
    quit_now(b, "pause")


def play(b, a):
    """round 4 item 1: A.minutes of active input in the world, in segments:
    walk / sprint / strafe / jump / crouch / turn (motion-checked), every 3rd
    segment an in-world menu (map, inventory, journal, character in turn), every
    A.vehicle_every-th a vehicle call and drive; then the session's end (A.exit)"""
    if not in_world(b, "w0_", 30) and not load_from_main(b, 0, a.save):
        raise Ended("no world at the start")
    tally = Tally()
    b.state("world")
    t0 = time.time()
    prev = b.shot("p0", "wait 1")
    k = drive_s = mk = 0
    while time.time() - t0 < a.minutes * 60:
        if not b.ended() and a.vehicle_every and k % a.vehicle_every == a.vehicle_every - 1:
            drive_s += vehicle_seg(b, k, tally, a.drive_s)
        elif k % 3 == 2:
            # its own counter: with 3 menus, k % 3 == 2 picked the journal every
            # time (h4-p02: journal 13, map 0, inventory 0)
            menu_seg(b, mk, tally)
            mk += 1
        else:
            prev = walk_seg(b, k, prev, tally)
        k += 1
        if k % 10 == 0:
            log(f"PLAY {time.time() - t0:.0f} s: walk {tally.s('walk')} menus {tally.s('menus')} "
                f"vehicle {tally.s('vehicle')} drive {drive_s:.0f} s")
    act = (time.time() - t0) / 60
    log(f"PLAY active {act:.1f} min, {k} segments")
    b.state("world")
    ok = {"pause": lambda: quit_now(b, "pause"), "vehicle": lambda: exit_vehicle(b, tally),
          "load": lambda: exit_load(b, tally), "settings": lambda: exit_settings(b, tally, a),
          "none": lambda: True}[a.exit]()
    menus = ",".join(f"{n}:{tally.s('menu:' + n)}" for n, _, _ in PLAY_MENUS)
    log(f"PLAY SUMMARY active_min={act:.1f} segments={k} walk={tally.s('walk')} unstuck={tally.s('unstuck')} "
        f"menus={tally.s('menus')} per_menu={menus} vehicle={tally.s('vehicle')} drive_s={drive_s:.0f} "
        f"drive_moving={tally.s('drive-moving')} exit={a.exit}{'' if ok else ':FAILED'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["churn", "menuloads", "settings", "quit", "explore", "explore2", "play", "presets"])
    ap.add_argument("run")
    ap.add_argument("--loads", type=int, default=10)
    ap.add_argument("--menu-loads", type=int, default=5)
    ap.add_argument("--saves", type=int, default=10)
    ap.add_argument("--manual-saves", type=int, default=0)
    ap.add_argument("--cycles", type=int, default=2)
    ap.add_argument("--from", dest="frm", default="pause", choices=["pause", "main"])
    ap.add_argument("--in-world", action="store_true", help="the boot is in the world already (a restarted driver)")
    ap.add_argument("--avoid", nargs="*", help="saves already loaded (a restarted driver)")
    ap.add_argument("--max-s", type=int, default=3600, help="give up (and let the boot end) after this many s")
    ap.add_argument("--list-steps", type=int, default=24, help="Down taps to walk the save list for one pick")
    ap.add_argument("--seq", help="settings: ROW=REGEX;ROW=REGEX... instead of the default 720p/1080p/window cycle")
    ap.add_argument("--presets", type=int, default=2, help="settings: Quick Preset changes per cycle")
    ap.add_argument("--save", default="ManualSave-86", help="explore/play: the save loaded from the main menu")
    ap.add_argument("--minutes", type=float, default=15, help="play: minutes of active input")
    ap.add_argument("--vehicle-every", type=int, default=0, help="play: a vehicle call every N segments (0 = never)")
    ap.add_argument("--drive-s", type=float, default=60, help="play: seconds per drive")
    ap.add_argument("--exit", default="pause", choices=["pause", "vehicle", "load", "settings", "none"])
    ap.add_argument("--no-wiggle", action="store_true", help="seek: never move the pointer (cp-hunt H6-04)")
    ap.add_argument("--exit-setting", default="resolution", choices=["resolution", "preset", "vsync", "resolution2"])
    a = ap.parse_args()
    b = Boot(a.run, max_s=a.max_s)
    LIST_STEPS[0] = a.list_steps
    WIGGLE[0] = not a.no_wiggle
    try:
        if a.what == "churn":
            churn(b, a)
        elif a.what == "menuloads":
            menu_loads(b, a)
        elif a.what == "settings":
            settings(b, a)
        elif a.what == "explore":
            explore(b, a)
        elif a.what == "explore2":
            explore2(b, a)
        elif a.what == "play":
            play(b, a)
        elif a.what == "presets":
            presets(b, a)
        else:
            quit_game(b, a.frm)
    except Ended as e:
        log(f"ENDED: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
