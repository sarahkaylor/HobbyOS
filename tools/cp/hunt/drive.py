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

SAVE_RX = re.compile(r"\b((?:Quick|Auto|Manual|Point\w*|End\w*)\s*S[ao]ve-\d+)", re.I)
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


def list_name(path, fr):
    """the save name inside the focused frame (or None for a group header):
    plain OCR, then the green channel contrast-stretched (h3-churn1: it read
    "ManualSave-86" where the plain pass did not)"""
    from PIL import ImageOps
    im = Image.open(path).convert("RGB")
    W, H = im.size
    y0, y1 = fr
    box = (int(W * 0.15), max(0, y0 - 6), int(W * 0.85), min(H, y1 + 6))
    t = ocr(path, box=box, scale=3, psm=6)
    m = SAVE_RX.search(t)
    if not m:
        g = ImageOps.autocontrast(im.crop(box).getchannel("G"))
        g = g.resize((g.size[0] * 3, g.size[1] * 3))
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
            g.save(tf.name)
        try:
            t2 = " ".join(subprocess.run(["tesseract", tf.name, "-", "--psm", "6"], capture_output=True,
                                         text=True, timeout=60).stdout.split())
        finally:
            os.unlink(tf.name)
        t += " | " + t2
        m = SAVE_RX.search(t2)
    return (re.sub(r"\s+", "", m.group(1)).replace("Sove", "Save") if m else None), t


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
        if lab is None and i >= 2:
            # cp-hunt H1-12: no visible focus after a sub-menu -> a relative mouse wiggle
            b.shot(f"{name}w{i}", "look 60 0 1 16", "look -60 0 1 16", "wait 0.5")
        b.shot(f"{name}k{i}", f"key {d}")
    log(f"SEEK {want}: not reached in {limit} taps")
    return False


def pause_open(b):
    p = b.shot("po", "key esc", "wait 4")
    if re.search(r"RESUME|EXIT\s+TO\s+MAIN", boot_ocr(p), re.I):
        return True
    p = b.shot("po2", "wait 2")          # once more: a popup may have eaten the Esc
    if re.search(r"RESUME|EXIT\s+TO\s+MAIN", boot_ocr(p), re.I):
        return True
    p = b.shot("po3", "key esc", "wait 4")
    return bool(re.search(r"RESUME|EXIT\s+TO\s+MAIN", boot_ocr(p), re.I))


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
    for i in range(steps):
        p = b.shot(f"ls{i}", "key down", "wait 1.2")
        fr = list_focus(p)
        if not fr:
            continue
        nm, _ = list_name(p, fr)
        seen.append(nm)
        if nm and ((prefer and nm == prefer) or (not prefer and nm not in avoid)):
            log(f"list: focus on {nm} (frame rows {fr[0]}..{fr[1]}) after {i + 1} taps")
            return nm
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
    if not pause_open(b):
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
    if p:
        b.shot(f"cfy{k}", "key ret", "wait 10")
    w = in_world(b, f"w{k}_", 400)
    log(f"LOAD {k} (pause menu -> {nm}): {'world ' + w if w else 'NO WORLD'} after {time.time() - t0:.0f} s")
    if w:
        b.shot(f"walk{k}", "hold w 3000", "look 400 0 20 16", "hold w 2000", "wait 2")
    return nm if w else None


def exit_to_main(b, k):
    if not pause_open(b) or not seek(b, PAUSE, "EXIT TO MAIN MENU", name=f"xm{k}_"):
        return False
    t0 = time.time()
    p, _ = b.until(f"xc{k}", r"sure.{0,20}quit", 15, "key ret", "wait 2")
    if not p:
        return False
    p, _ = b.until(f"tt{k}", r"PRESS\b.{0,24}\bTO\s+CONTINUE", 400, "key ret", "wait 5")
    log(f"EXIT TO MAIN MENU {k}: title after {time.time() - t0:.0f} s" if p else f"EXIT {k}: no title")
    if not p:
        return False
    p, _ = b.until(f"mm{k}", r"LOAD\s*GAME", 60, "key spc", "wait 5")
    return p is not None


def load_from_main(b, k, prefer):
    if not seek(b, MAIN, "LOAD GAME", name=f"ml{k}_"):
        return None
    b.shot(f"mlr{k}", "key ret", "wait 3")
    open_list(b)
    nm = list_pick(b, set(), prefer=prefer) or list_pick(b, set())
    if not nm:
        return None
    t0 = time.time()
    p, _ = b.until(f"ld{k}", r"PRESS\b.{0,24}\bTO\s+CONTINUE", 400, "key ret", "wait 10")
    if not p:
        log(f"MAIN LOAD {k} ({nm}): no PRESS ... TO CONTINUE")
        return None
    w = in_world(b, f"mw{k}_", 90) if b.shot(f"sp{k}", "key spc", "wait 5") else None
    log(f"MAIN LOAD {k} (main menu -> {nm}): {'world ' + w if w else 'NO WORLD'} after {time.time() - t0:.0f} s")
    return nm if w else None


def quit_game(b, frm):
    if frm == "main":
        if not exit_to_main(b, 99):
            return False
        if not seek(b, MAIN, "QUIT GAME", name="mq"):
            return False
    else:
        if not pause_open(b) or not seek(b, PAUSE, "QUIT GAME", name="pq"):
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


def recover(b, name="rc"):
    """back to the world from whatever menu or dialog is up: Esc (which also
    cancels an apply prompt) until the HUD's input hints show"""
    for i in range(6):
        p = b.shot(f"{name}{i}", "wait 1")
        t = boot_ocr(p)
        if re.search(WORLD_RX, t, re.I | re.S) and not re.search(NOT_WORLD_RX + r"|RESUME|apply", t, re.I | re.S):
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
    p = b.shot(name + "ge", "key ret", "wait 2")
    after = row_value(p, "Quick Preset", GRAPHICS_ROWS, GRAPHICS_TABLE)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["churn", "menuloads", "settings", "quit"])
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
    a = ap.parse_args()
    b = Boot(a.run, max_s=a.max_s)
    LIST_STEPS[0] = a.list_steps
    try:
        if a.what == "churn":
            churn(b, a)
        elif a.what == "menuloads":
            menu_loads(b, a)
        elif a.what == "settings":
            settings(b, a)
        else:
            quit_game(b, a.frm)
    except Ended as e:
        log(f"ENDED: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
