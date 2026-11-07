#!/usr/bin/env python3
"""rw_flow.py -- drive RimWorld's front-end with OCR + pixel-verified QMP clicks.

L81 rewrite of the L79 prototype.  What changed and why:

  * ADVANCE IS A PIXEL, NOT A WORD.  L79's `phrase_box(png, "Generate")`
    matched the wrong label on the Create-world screen and clicked x=544
    (Back/Reset all) instead of the rightmost footer button; the run then
    left the screen and the record said "advanced".  Every advancing click
    here is the RIGHTMOST flat-brown footer button (tools/rw/click-ignore.py's
    rule, extended by brown_buttons()), and OCR is only used for card/radio
    labels that have no reliable pixel shape.

  * A STAGE IS EVIDENCE, NOT A GUESS.  Every stage entry records the OCR text,
    the [XSW] present counter/CRC, the pixel diff against the previous
    screenshot (bbox + clock-region share) and an fps window computed from the
    guest's own [XSW] t= values.

  * A WALL IS MEASURED.  A stage that shows no pixel change outside the panel
    clock for RW_FLOW_WALL_SECS (default 90 s) while the present counter is
    also static is a wall; it dumps the last [THRS]/[XSW]/[FUTR]/[BLKD] rows,
    the GPU refusal count and the OCR, then stops instead of clicking blindly.

  * RESUMABLE.  flow.jsonl is append-only (one record per event, each tagged
    with a run id) and flow-state.json is rewritten after every stage, so a
    run that dies leaves the stage ladder and the last screen on disk.  A new
    run starts at the menu again (the VM has to be re-driven from boot), but
    RW_FLOW_SKIP_FPS=1 skips the 15 s measurement windows when all you want is
    the next screen.

Env:
  RW_FLOW=1                 enable this driver (read by rw_desktop_drive.py)
  RW_FLOW_STAGE_SECS=15     fps/pixel window per stage
  RW_FLOW_OCR_EVERY=8       seconds between polls while a stage is settling
  RW_FLOW_TOTAL=...         overall budget (default --run-timeout - 120)
  RW_FLOW_WALL_SECS=90      no-pixel-change wall threshold
  RW_FLOW_GEN_MAX=1500      cap on the world-generation wait
  RW_FLOW_SKIP_FPS=1        no per-stage measurement windows
"""
import json
import os
import re
import subprocess
import time

import numpy as np
from PIL import Image

# Panel clock: bottom-right.  Anything that changes only here is "static".
CLOCK = (1780, 1030)
SHOT_TAGS = {}


def _tsv_words(png):
    """tesseract TSV -> list of dicts (line key, text, box)."""
    try:
        out = subprocess.run(
            ["tesseract", png, "stdout", "--psm", "3", "tsv"],
            capture_output=True, text=True, timeout=180).stdout
    except Exception:
        return []
    words = []
    for line in out.splitlines()[1:]:
        f = line.split("\t")
        if len(f) < 12:
            continue
        try:
            conf = float(f[10])
            if conf < 0:
                continue
            words.append({
                "line": (f[1], f[2], f[3], f[4]),
                "text": f[11].strip(),
                "x": int(f[6]), "y": int(f[7]),
                "w": int(f[8]), "h": int(f[9]),
            })
        except ValueError:
            continue
    return [w for w in words if w["text"]]


def ocr_text(png):
    try:
        out = subprocess.run(["tesseract", png, "stdout", "--psm", "3"],
                             capture_output=True, text=True, timeout=180).stdout
    except Exception:
        return ""
    return " ".join(out.split())


def _norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def _lev(a, b):
    if a == b:
        return 0
    if abs(len(a) - len(b)) > 2:
        return 9
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1,
                           prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def _close(a, b):
    """Exact token, or a plausible OCR miss on a long word ("Generate" ->
    "Gengyate").  Short words must match exactly: 'Next'/'Back' differ by
    two edits and a fuzzy match there would click the wrong button."""
    if a == b:
        return True
    if len(a) < 6 or len(b) < 6:
        return False
    return _lev(a, b) <= 2


def phrase_box(png, phrase):
    """Centre of `phrase` in `png`, via word boxes on one text line."""
    toks = [_norm(t) for t in phrase.split() if _norm(t)]
    if not toks:
        return None
    words = _tsv_words(png)
    lines = {}
    for w in words:
        lines.setdefault(w["line"], []).append(w)
    import itertools
    cands = []
    for ws in lines.values():
        ws = sorted(ws, key=lambda w: w["x"])
        for i in range(len(ws)):
            for extra in (0, 1):
                win = ws[i:i + len(toks) + extra]
                if len(win) < len(toks):
                    continue
                for pick in itertools.combinations(range(len(win)), len(toks)):
                    if any(not _close(_norm(win[k]["text"]), t)
                           for k, t in zip(pick, toks)):
                        continue
                    x0 = min(win[k]["x"] for k in pick)
                    y0 = min(win[k]["y"] for k in pick)
                    x1 = max(win[k]["x"] + win[k]["w"] for k in pick)
                    y1 = max(win[k]["y"] + win[k]["h"] for k in pick)
                    h = max(win[k]["h"] for k in pick)
                    cands.append((x0, y0, x1, y1, h, (x1 - x0) * (y1 - y0)))
    if not cands:
        # OCR often breaks a label across two lines ("Reload anytime" /
        # "mode").  Fall back to reading order: the first token, followed by
        # the rest within the next few words, and click the first token.
        ws = sorted(words, key=lambda w: (w["y"] // 12, w["x"]))
        seq = [_norm(w["text"]) for w in ws]
        for idx in range(len(seq)):
            if not _close(seq[idx], toks[0]):
                continue
            k, j = 1, idx + 1
            while k < len(toks) and j < len(seq) and j <= idx + 6:
                if _close(seq[j], toks[k]):
                    k += 1
                j += 1
            if k == len(toks):
                w = ws[idx]
                return (w["x"] + w["w"] // 2, w["y"] + w["h"] // 2,
                        (w["x"], w["y"], w["x"] + w["w"], w["y"] + w["h"]))
        return None
    # A screen can repeat a word ("Crashlanded" as a row title and inside a
    # description).  The clickable label is the one set in the LARGER font;
    # ties go to the topmost, then the tightest box.
    cands.sort(key=lambda c: (-c[4], c[1], c[5]))
    x0, y0, x1, y1, _, _ = cands[0]
    return ((x0 + x1) // 2, (y0 + y1) // 2, (x0, y0, x1, y1))


RADIO_LABELS = {"reload anytime mode", "commitment mode", "strive to survive",
                "peaceful", "community builder", "adventure story",
                "blood and dust", "losing is fun"}


def radio_target(png, label_box):
    """The radio circle that belongs to a label row, if it is drawn to the
    right of the text.  RimWorld's storyteller page lays the difficulty and
    game-mode options out as label + circle at a fixed column (~x=893 at
    1920x1080); clicking the text alone does nothing.  Find the bright ring
    on the label's own row instead of trusting an offset."""
    try:
        a = np.asarray(Image.open(png).convert("RGB")).astype(int)
    except Exception:
        return None
    y = (label_box[1] + label_box[3]) // 2
    x_lo, x_hi = 700, min(a.shape[1], 1000)
    band = a[max(0, y - 9):y + 10, x_lo:x_hi, :]
    bright = ((band[:, :, 0] > 180) & (band[:, :, 1] > 180)
              & (band[:, :, 2] > 180))
    cols = bright.sum(axis=0)
    xs = np.nonzero(cols >= 2)[0]
    runs = []
    if len(xs):
        s0 = p = int(xs[0])
        for x in xs[1:]:
            x = int(x)
            if x - p > 10:
                runs.append((s0, p))
                s0 = x
            p = x
        runs.append((s0, p))
    for (x0, x1) in runs:
        x0 += x_lo
        x1 += x_lo
        if x0 < label_box[2] + 10:
            continue
        if 12 <= x1 - x0 <= 28:
            return ((x0 + x1) // 2, y, (x0, y - 8, x1, y + 8))
    return None


def img_stats(png):
    try:
        im = Image.open(png).convert("RGB")
    except Exception:
        return {}
    a = np.asarray(im)
    small = a[::4, ::4].reshape(-1, 3)
    nz = np.any(small != 0, axis=1).sum()
    colours = len({tuple(c) for c in small})
    return {"w": im.size[0], "h": im.size[1],
            "nonblack_sampled": int(nz), "colours_sampled": int(colours)}


BROWN = (106, 81, 46)


def brown_buttons(png):
    """Centres of the game's flat-brown dialog buttons, footer rightmost first.

    RimWorld paints dialog buttons a flat fill (106,81,46 at rest,
    136,100,50 while hovered).  At 1 fps tesseract sometimes cannot read the
    label at all ("Generate" -> "Gengyate", or nothing), so this is the
    primary way to find an advancing button.  Runs are grouped by row band,
    not by column: tall brown panels (the faction list) must not merge with
    the buttons below them.
    """
    try:
        a = np.asarray(Image.open(png).convert("RGB"))
    except Exception:
        return []
    m = np.zeros(a.shape[:2], dtype=bool)
    for col in ((106, 81, 46), (136, 100, 50), (150, 113, 54)):
        m |= np.all(a == np.array(col), axis=2)
    h, w = m.shape
    y0 = h // 2
    bands = []
    for y in range(y0, h):
        xs = np.nonzero(m[y])[0]
        if len(xs) == 0:
            continue
        runs = []
        s0 = p = int(xs[0])
        for x in xs[1:]:
            x = int(x)
            if x - p > 2:
                if p - s0 >= 40:
                    runs.append((s0, p))
                s0 = x
            p = x
        if p - s0 >= 40:
            runs.append((s0, p))
        for (x0r, x1r) in runs:
            hit = None
            for b in bands:
                if (y - b["y1"] <= 6
                        and not (x1r < b["x0"] - 6 or x0r > b["x1"] + 6)):
                    hit = b
                    break
            if hit:
                hit["y1"] = y
                hit["x0"] = min(hit["x0"], x0r)
                hit["x1"] = max(hit["x1"], x1r)
            else:
                bands.append({"x0": x0r, "x1": x1r, "y0": y, "y1": y})
    out = []
    for b in bands:
        bw = b["x1"] - b["x0"]
        bh = b["y1"] - b["y0"]
        if not (80 <= bw <= 380 and 18 <= bh <= 46):
            continue
        out.append(((b["x0"] + b["x1"]) // 2, (b["y0"] + b["y1"]) // 2,
                    (b["x0"], b["y0"], b["x1"], b["y1"])))
    if not out:
        return out
    # The footer row is the bottom band; within it the advancing button
    # (Next / Generate / Start) is the RIGHTMOST.  Hover changes a button's
    # bbox by a pixel or two, so a plain y sort can put "Reset all" first.
    ymax = max(t[1] for t in out)
    footer = sorted([t for t in out if t[1] >= ymax - 40],
                    key=lambda t: -t[0])
    rest = sorted([t for t in out if t[1] < ymax - 40],
                  key=lambda t: (-t[1], -t[0]))
    return footer + rest


def diff_stats(a_png, b_png, thr=8):
    a = np.asarray(Image.open(a_png).convert("RGB")).astype(np.int16)
    b = np.asarray(Image.open(b_png).convert("RGB")).astype(np.int16)
    if a.shape != b.shape:
        return {"shape_a": list(a.shape), "shape_b": list(b.shape)}
    d = np.abs(a - b).max(axis=2)
    mask = d > thr
    n = int(mask.sum())
    if n == 0:
        return {"pixels": 0, "bbox": None, "clock_frac": 0.0,
                "total": int(mask.size)}
    ys, xs = np.nonzero(mask)
    x0, x1, y0, y1 = int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max())
    clock = int(((xs >= CLOCK[0]) & (ys >= CLOCK[1])).sum())
    return {"pixels": n, "frac": round(n / mask.size, 5),
            "bbox": [x0, y0, x1, y1], "clock_frac": round(clock / max(1, n), 4),
            "total": int(mask.size)}


def classifier(text):
    s = text.lower()
    if (("tutorial" in s and "quit to os" in s)
            or ("new colony" in s and "tutorial" in s)):
        return "menu"
    if "storyteller" in s or "cassandra classic" in s:
        return "storyteller"
    if "crashlanded" in s or ("scenario" in s and "rich explorer" in s):
        return "scenario"
    if "select starting site" in s or "select random site" in s:
        return "site"
    if "create characters" in s or ("randomize" in s and "backstory" in s):
        return "characters"
    if "ideoligion" in s or "ideology system inactive" in s:
        return "ideology"
    if "generating" in s:
        return "generating"
    if "create world" in s or "planet coverage" in s or "globe coverage" in s:
        return "worldopts"
    if "difficulty" in s or "strive to survive" in s or "playstyle" in s:
        return "difficulty"
    if ("architect" in s or "work" in s and "assign" in s
            or "wildlife" in s and "beauty" in s):
        return "map"
    return "unknown"


# What to click on each screen, in order.  An action is one of:
#   ("phrase", "Text")   OCR-located label (cards, storyteller names)
#   ("radio",  "Text")   label + the radio circle on its own row
#   ("advance", None)    the rightmost flat-brown footer button
ADVANCE = {
    "menu": [("phrase", "New colony")],
    "scenario": [("phrase", "Crashlanded"), ("advance", None)],
    "storyteller": [("phrase", "Cassandra Classic"),
                    ("radio", "Strive to survive"),
                    ("radio", "Reload anytime mode"),
                    ("advance", None)],
    "worldopts": [("advance", None)],
    "site": [("phrase", "Select random site"), ("advance", None)],
    "characters": [("advance", None)],
    "ideology": [("advance", None)],
}

CLICK_WAIT = 8.0


class Flow:
    def __init__(self, q, grab, log_path, args, shots, find_ignore):
        self.q = q
        self.grab = grab
        self.log_path = log_path
        self.args = args
        self.shots = shots
        self.find_ignore = find_ignore
        self.out_dir = os.path.dirname(os.path.abspath(log_path))
        self.jsonl = os.path.join(self.out_dir, "flow.jsonl")
        self.state_path = os.path.join(self.out_dir, "flow-state.json")
        self.run_id = time.strftime("%Y%m%dT%H%M%S")
        self.n = 0
        self.last_text = ""
        self.t_start = time.time()
        self.stage_enter = {}

    # -- plumbing ---------------------------------------------------------
    def emit(self, rec):
        rec["wall"] = round(time.time(), 3)
        rec["run"] = self.run_id
        rec["elapsed"] = round(time.time() - self.t_start, 1)
        with open(self.jsonl, "a") as f:
            f.write(json.dumps(rec) + "\n")
        keys = {k: v for k, v in rec.items()
                if k in ("ev", "tag", "stage", "to", "fps", "n", "crc",
                         "changed", "pixels", "bbox", "clock_frac", "classes",
                         "text", "click", "phrase", "target", "elapsed",
                         "note", "how", "span_s")}
        print("[flow] " + json.dumps(keys, ensure_ascii=False)[:900], flush=True)

    def checkpoint(self, stage, note=""):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"run": self.run_id, "stage": stage, "note": note,
                       "wall": round(time.time(), 3),
                       "elapsed": round(time.time() - self.t_start, 1)},
                      f, indent=1)
        os.replace(tmp, self.state_path)

    def shot(self, tag):
        self.n += 1
        name = f"f{self.n:03d}_{tag}"
        p = self.grab(name)
        if p:
            SHOT_TAGS[name] = p
        return p

    def serial(self):
        try:
            return open(self.log_path, errors="replace").read()
        except OSError:
            return ""

    def xsw_rows(self):
        rows = []
        for m in re.finditer(r"\[XSW\] n=(\d+)\b[^\n]*", self.serial()):
            line = m.group(0)
            n = int(m.group(1))
            t = re.search(r"\bt=(\d+)", line)
            crc = re.search(r"crc=0x([0-9a-f]+)", line)
            ch = re.search(r"changed=(\d)", line)
            if not t:
                continue
            rows.append({"n": n, "t": int(t.group(1)),
                         "crc": crc.group(1) if crc else None,
                         "changed": int(ch.group(1)) if ch else None})
        return rows

    def xsw(self):
        rows = self.xsw_rows()
        return rows[-1] if rows else None

    def fps(self, secs=15):
        """Present rate over the last `secs` of the guest's own clock.

        [XSW] is written every 10 presents with the guest monotonic t in ms.
        A 15 s window at the menu (1 fps) contains 1-2 rows; the returned
        span_s records what was actually measured so a 10 s number is never
        passed off as a 15 s one.
        """
        rows = self.xsw_rows()
        if len(rows) < 2:
            return {"fps": None, "span_s": None, "n": rows[-1]["n"] if rows else None}
        last = rows[-1]
        want = last["t"] - int(secs * 1000)
        ref = None
        for r in reversed(rows[:-1]):
            if r["t"] <= want:
                ref = r
                break
        if ref is None:
            ref = rows[-2]
            if last["t"] - ref["t"] > secs * 1000 * 2.5:
                return {"fps": None, "span_s": 0,
                        "n": last["n"], "crc": last["crc"],
                        "changed": last["changed"]}
        dt = (last["t"] - ref["t"]) / 1000.0
        dn = last["n"] - ref["n"]
        return {"fps": round(dn / dt, 3) if dt > 0 else None,
                "span_s": round(dt, 1), "n": last["n"], "crc": last["crc"],
                "changed": last["changed"]}

    def wait_modal_gone(self, png, where):
        """Dismiss the Steam modal only when OCR says it is the modal.

        L79 called find_ignore() on every poll, and the menu's own brown
        buttons have the same fill.  Gate it on the text.
        """
        txt = ocr_text(png).lower()
        if "steam" not in txt and "could not initialize" not in txt:
            return False
        xy = self.find_ignore(png)
        if not xy:
            return False
        self.emit({"ev": "modal", "where": where, "click": list(xy),
                   "text": "Could not initialize Steam API"})
        self.q.click(xy[0], xy[1], self.args.fb_w, self.args.fb_h)
        time.sleep(3)
        return True

    def click_xy(self, xy, stage, phrase, how):
        before = self.xsw()
        self.emit({"ev": "click", "stage": stage, "phrase": phrase,
                   "how": how, "click": [xy[0], xy[1]], "before": before})
        self.q.click(xy[0], xy[1], self.args.fb_w, self.args.fb_h)
        time.sleep(CLICK_WAIT)
        return self.shot(f"{stage}_after_{re.sub(r'[^A-Za-z0-9]+', '_', phrase)}")

    def verify_click(self, png_before, png_after, stage, phrase):
        d = diff_stats(png_before, png_after) if png_after else {}
        txt = ocr_text(png_after) if png_after else ""
        cls = classifier(txt)
        after = self.xsw()
        self.emit({"ev": "click_result", "stage": stage, "phrase": phrase,
                   "pixels": d.get("pixels"), "bbox": d.get("bbox"),
                   "clock_frac": d.get("clock_frac"), "classes": cls,
                   "text": txt[:600],
                   "n": (after or {}).get("n"), "crc": (after or {}).get("crc"),
                   "changed": (after or {}).get("changed"),
                   "note": "changed" if isinstance(d.get("pixels"), int)
                           and d["pixels"] > 5000 else "no-change"})
        return png_after, cls, txt, d

    def measure_stage(self, stage, png, secs):
        """A short fps/pixel window on a settled stage."""
        a = self.fps(secs)
        n0 = (self.xsw() or {}).get("n")
        tw = time.time()
        if secs > 0:
            time.sleep(secs)
        p2 = self.shot(f"{stage}_end")
        d = diff_stats(png, p2) if p2 else {}
        txt = ocr_text(p2) if p2 else ""
        b = self.fps(secs)
        rec = {"ev": "stage_fps", "stage": stage, "fps": b.get("fps"),
               "span_s": b.get("span_s"), "n": b.get("n"),
               "crc": b.get("crc"), "changed": b.get("changed"),
               "elapsed": round(time.time() - tw, 1),
               "pixels": d.get("pixels"), "bbox": d.get("bbox"),
               "clock_frac": d.get("clock_frac"),
               "classes": classifier(txt), "text": txt[:600],
               "shot": os.path.basename(p2) if p2 else None,
               "n0": n0}
        self.emit(rec)
        return rec

    # -- stage entry ------------------------------------------------------
    def enter_stage(self, stage, png, txt):
        self.last_text = txt
        self.stage_enter[stage] = time.time()
        a = self.xsw()
        self.emit({"ev": "stage", "stage": stage, "note": "enter",
                   "n": (a or {}).get("n"), "crc": (a or {}).get("crc"),
                   "changed": (a or {}).get("changed"),
                   "classes": stage, "text": txt[:800],
                   "shot": os.path.basename(png) if png else None})
        self.checkpoint(stage, "enter")

    # -- the drive --------------------------------------------------------
    def wait_menu(self, deadline, poll):
        cls, png, txt = "unknown", None, ""
        while time.time() < deadline:
            png = self.shot("wait_menu")
            if png is None:
                time.sleep(5)
                continue
            self.wait_modal_gone(png, "wait")
            txt = ocr_text(png)
            cls = classifier(txt)
            f = self.fps(15)
            d = {}
            prev = getattr(self, "_prev_poll", None)
            if prev:
                d = diff_stats(prev, png)
            self._prev_poll = png
            self.emit({"ev": "poll", "stage": "wait_menu", "classes": cls,
                       "text": txt[:300], "shot": os.path.basename(png),
                       "fps": f.get("fps"), "span_s": f.get("span_s"),
                       "n": f.get("n"), "crc": f.get("crc"),
                       "pixels": d.get("pixels"), "bbox": d.get("bbox"),
                       "clock_frac": d.get("clock_frac")})
            if cls == "menu":
                return png, txt
            low = txt.lower()
            if ("findnextentry" in low or "could not load defs" in low
                    or ("input/output error" in low and "defs" in low)):
                self.emit({"ev": "wall", "stage": "def-load", "text": txt[:800],
                           "note": "NFS/def load I/O error; the game is on its "
                                   "error console, not the menu"})
                return None, None
            time.sleep(poll)
        self.emit({"ev": "wall", "stage": "menu-not-reached",
                   "text": txt[:600], "note": "OCR never saw Tutorial/New colony/Quit to OS"})
        return None, None

    def advance_candidates(self, png, stage):
        """(xy, label, how) for an advancing click.

        ONLY the rightmost footer button.  L79's "try the next candidate"
        loop is how a run that missed "Generate" ended up clicking "Reset
        all" and then "Back"; on this UI the affirmative (Next / Generate /
        Start) is always the rightmost footer button, and any other footer
        button moves AWAY from the colony.  A rightmost button that does not
        respond is a wall to report, not a reason to click something else.
        """
        out = []
        btns = brown_buttons(png)
        if btns and stage not in ("menu",):
            out.append((btns[0], "footer", "brown-button"))
        for phrase in ("Next", "Start", "Generate"):
            box = phrase_box(png, phrase)
            if box and box[0] > 900:  # footer/right side only
                out.append((box, phrase, "ocr"))
        return out[:2]

    def try_advance(self, png, stage, deadline):
        """Click the advancing button and give the frame time to change.

        At 1 fps and with a world-generation transition a screen can take
        tens of seconds to repaint; the same button is retried (up to 3
        clicks, each followed by up to 4 polls) and nothing else is clicked.
        """
        cands = self.advance_candidates(png, stage)
        if not cands:
            self.emit({"ev": "no_advance", "stage": stage, "tried": 0,
                       "note": "no rightmost footer button found"})
            return png, stage, self.last_text, False
        xy, label, how = cands[0]
        for attempt in (1, 2, 3):
            if time.time() > deadline:
                break
            self.emit({"ev": "resolve", "stage": stage, "phrase": label,
                       "how": how, "attempt": attempt,
                       "click": [xy[0], xy[1]]})
            png2 = self.click_xy(xy, stage, label, how)
            png2, cls2, txt2, d = self.verify_click(png, png2, stage, label)
            px = d.get("pixels")
            px = px if isinstance(px, int) else 0
            if cls2 != stage or px > 40000:
                return png2, cls2, txt2, True
            # No repaint yet: poll the same screen a few times before the
            # next click.  A world-generation screen can take >8 s to appear.
            for _ in range(4):
                time.sleep(6)
                p3 = self.shot(f"{stage}_wait_{label}")
                if not p3:
                    continue
                d3 = diff_stats(png, p3)
                txt3 = ocr_text(p3)
                cls3 = classifier(txt3)
                px3 = d3.get("pixels")
                px3 = px3 if isinstance(px3, int) else 0
                self.emit({"ev": "settle", "stage": stage, "phrase": label,
                           "classes": cls3, "pixels": px3,
                           "bbox": d3.get("bbox"),
                           "clock_frac": d3.get("clock_frac"),
                           "text": txt3[:300]})
                if cls3 != stage or px3 > 40000:
                    return p3, cls3, txt3, True
                png2 = p3
            png = png2 or png
        self.emit({"ev": "no_advance", "stage": stage, "tried": 3,
                   "phrase": label})
        return png, stage, self.last_text, False

    def drive_stage(self, stage, png, txt, deadline, secs):
        """Run one stage's action list, then return the next (stage, png, txt)."""
        self.enter_stage(stage, png, txt)
        if secs > 0 and os.environ.get("RW_FLOW_SKIP_FPS", "0") != "1":
            rec = self.measure_stage(stage, png, secs)
            if rec.get("shot"):
                cand = os.path.join(self.args.shot_dir, rec["shot"])
                if os.path.exists(cand):
                    png = cand
                    txt = rec.get("text", txt)
        for kind, phrase in ADVANCE.get(stage, []):
            if time.time() > deadline:
                break
            if kind == "advance":
                png, cls2, txt2, moved = self.try_advance(png, stage, deadline)
                if moved:
                    return cls2, png, txt2
                continue
            box = phrase_box(png, phrase)
            if not box:
                self.emit({"ev": "skip", "stage": stage, "phrase": phrase,
                           "note": "not on screen"})
                continue
            if kind == "radio" or phrase.lower() in RADIO_LABELS:
                rt = radio_target(png, box[2])
                if rt:
                    self.emit({"ev": "radio", "stage": stage, "phrase": phrase,
                               "label": list(box[2]), "click": [rt[0], rt[1]]})
                    box = rt
            png2 = self.click_xy(box, stage, phrase, kind)
            png2, cls2, txt2, d = self.verify_click(png, png2, stage, phrase)
            if cls2 != stage:
                return cls2, png2, txt2
            px = d.get("pixels")
            if isinstance(px, int) and px > 5000:
                self.emit({"ev": "selected", "stage": stage, "phrase": phrase,
                           "text": txt2[:300], "pixels": px})
            png, txt = png2 or png, txt2 or txt
        return stage, png, txt

    def poll_until_change(self, stage, png, deadline, poll, cap_s,
                          wall_secs, gen=False):
        """Wait on a screen with no button: a load/progress screen.

        Returns (new_stage, new_png, new_txt) or emits a wall and returns
        (None, png, txt).  A wall is BOTH no pixel change outside the clock
        and no new present, for wall_secs.
        """
        last_change = time.time()
        prev = png
        n_last = (self.xsw() or {}).get("n") or 0
        t_cap = time.time() + cap_s
        while time.time() < deadline and time.time() < t_cap:
            time.sleep(poll)
            p2 = self.shot(f"{stage}_poll")
            if not p2:
                continue
            d = diff_stats(prev, p2) if prev else {}
            txt = ocr_text(p2)
            cls = classifier(txt)
            f = self.fps(20)
            n_now = (self.xsw() or {}).get("n") or 0
            live = (n_now > n_last)
            px = d.get("pixels")
            px = px if isinstance(px, int) else 0
            cf = d.get("clock_frac")
            cf = cf if isinstance(cf, (int, float)) else 0.0
            changed = px > 3000 and cf < 0.9
            if changed or (gen and live):
                last_change = time.time()
            n_last = n_now
            self.emit({"ev": "poll", "stage": stage, "classes": cls,
                       "text": txt[:400], "fps": f.get("fps"),
                       "span_s": f.get("span_s"), "pixels": d.get("pixels"),
                       "bbox": d.get("bbox"), "clock_frac": d.get("clock_frac"),
                       "note": "changed" if changed else ("live" if live else "static")})
            if cls != stage and cls != "unknown":
                return cls, p2, txt
            prev = p2
            if time.time() - last_change > (240 if gen else wall_secs):
                self.emit({"ev": "wall", "stage": stage,
                           "text": txt[:800], "shot": os.path.basename(p2),
                           "note": ("no pixel change and no present for "
                                    f"{int(time.time() - last_change)} s")})
                return None, p2, txt
        self.emit({"ev": "wall", "stage": stage, "text": self.last_text[:800],
                   "note": f"stage cap {cap_s}s reached"})
        return None, png, self.last_text

    def dump_wall_rows(self, stage):
        """The rows that name the wall (L78's l78-wall.py set, inline)."""
        txt = self.serial()
        rows = {}
        for tag in ("XSW", "THRS", "FUTR", "BLKD", "GDC", "XO"):
            m = list(re.finditer(r"\[%s\][^\n]*" % tag, txt))
            rows[tag] = [x.group(0)[:300].rstrip() for x in m[-3:]]
        rows["GPU_refusals"] = len(re.findall(r"res_create refused", txt))
        rows["Player_log_exceptions"] = len(
            re.findall(r"Exception|NullReference|StackTrace", txt))
        tail = [l for l in txt.splitlines()[-40:]]
        rows["serial_tail"] = tail
        path = os.path.join(self.out_dir, f"wall-{stage}.json")
        with open(path, "w") as f:
            json.dump(rows, f, indent=1)
        print(f"[flow] wall rows -> {path}", flush=True)
        for k in ("XSW", "THRS", "BLKD", "FUTR"):
            for r in rows.get(k, [])[-1:]:
                print(f"[flow]   {k}: {r[:220]}", flush=True)
        print(f"[flow]   GPU_refusals: {rows['GPU_refusals']}", flush=True)
        return rows

    def run(self):
        total = float(os.environ.get(
            "RW_FLOW_TOTAL", str(max(300, self.args.run_timeout - 120))))
        stage_secs = float(os.environ.get("RW_FLOW_STAGE_SECS", "15"))
        poll = float(os.environ.get("RW_FLOW_OCR_EVERY", "8"))
        wall_secs = float(os.environ.get("RW_FLOW_WALL_SECS", "90"))
        gen_max = float(os.environ.get("RW_FLOW_GEN_MAX", "1500"))
        deadline = time.time() + total
        self.emit({"ev": "start", "budget": total,
                   "run_timeout": self.args.run_timeout,
                   "stage_secs": stage_secs})

        # 1. main menu (the load takes ~9 min on this tree)
        png, txt = self.wait_menu(deadline, max(8.0, poll))
        if png is None:
            return 2

        # 2. walk the screens
        stage = "menu"
        for _ in range(40):
            if time.time() > deadline:
                self.emit({"ev": "wall", "stage": stage, "text": txt[:600],
                           "note": "budget exhausted"})
                break
            if stage == "map":
                self.emit({"ev": "reached", "stage": "map", "text": txt[:1200],
                           "shot": os.path.basename(png or "")})
                self.checkpoint("map", "reached")
                return 0
            if stage in ("generating", "unknown"):
                new_stage, png2, txt2 = self.poll_until_change(
                    stage, png, deadline, poll, gen_max, wall_secs,
                    gen=(stage == "generating"))
                if new_stage is None:
                    self.dump_wall_rows(stage)
                    return 2
                stage, png, txt = new_stage, png2, txt2
                continue
            new_stage, png2, txt2 = self.drive_stage(
                stage, png, txt, deadline, stage_secs)
            png, txt = png2 or png, txt2 or txt
            if new_stage != stage:
                self.emit({"ev": "advanced", "stage": stage, "to": new_stage,
                           "text": (txt or "")[:400]})
                stage = new_stage
                time.sleep(2)
                continue
            # The click produced a repaint but OCR has not named the new
            # screen yet (1 fps + a transition).  Settle before calling it.
            ns, p2, t2 = self.poll_until_change(
                stage, png, deadline, max(6.0, poll),
                1200 if stage in ("worldopts", "generating") else 150,
                wall_secs, gen=(stage in ("worldopts", "generating")))
            if ns:
                self.emit({"ev": "advanced", "stage": stage, "to": ns,
                           "text": (t2 or "")[:400]})
                stage, png, txt = ns, p2, t2
                continue
            self.dump_wall_rows(stage)
            return 2
        self.emit({"ev": "end", "stage": stage, "text": (txt or "")[:600]})
        return 0


def run(q, grab, log_path, args, shots, find_ignore):
    return Flow(q, grab, log_path, args, shots, find_ignore).run()
