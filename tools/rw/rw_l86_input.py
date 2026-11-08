#!/usr/bin/env python3
"""rw_l86_input.py -- prove that mouse clicks and keyboard keys reach
RimWorld inside HBHOST's X window under IceWM, and drive the front-end to
world generation with coordinates found by OCR.

Why this is a separate module: tools/rw/rw_flow.py is the L81 driver for the
native DESKTOP.BIN session at 1920x1080 full screen, where the game's content
origin is the screen origin.  Under IceWM the game is a *hosted* window: HBHOST
owns an X window at some screen position, the game's 1280x720 content sits
16 px below that window's top edge (HBHOST's own menu bar), and every click has
to be aimed in screen coordinates.  So this driver:

  * reads the window rect from the serial log (`[HB] geometry client=x,y,WxH`)
    and the content offset from the guest's own `[XSW] ... gx= gy= w= h=`
    row, and logs every click as
        [L86] click <label> screen=(x,y) content=(x,y)
    so a click can be audited against both frames of reference;
  * OCRs the *client-area crop* (2x upscaled, which is what makes RimWorld's
    small UI font readable) and converts word boxes back to screen pixels;
  * verifies every click by (a) the pixel diff against the previous shot --
    count + bbox -- and (b) OCR of the new screen, never by the pointer;
  * types into the Seed field with real QMP key events and re-OCRs the field.

Everything it writes lands next to the harness's serial log:
  l86.jsonl     one JSON record per observation (append-only)
  <shot>.png    screenshots via the harness's own grab()
"""

import json
import os
import re
import subprocess
import time

import numpy as np
from PIL import Image


# ---------------------------------------------------------------- geometry
def parse_geometry(text):
    """(client_x, client_y, win_w, win_h) from the last `[HB] geometry` row.

    HBHOST logs the X *client area* (what IceWM grants inside its frame) and
    the screen size.  The title bar is above client_y; the game's content is
    16 px below client_y (HBHOST's menu bar, same 16 the client's own
    `[XSW] gx=0 gy=16` reports)."""
    m = None
    for m2 in re.finditer(r"\[HB\] geometry client=(-?\d+),(-?\d+),(\d+)x(\d+)", text):
        m = m2
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))


def parse_xsw(text):
    """Last [XSW] row -> dict(n, crc, gx, gy, w, h, t)."""
    last = None
    for m in re.finditer(r"\[XSW\] n=(\d+)\b([^\n]*)", text):
        last = m
    if not last:
        return None
    line = last.group(0)

    def g(name):
        m = re.search(r"\b%s=(-?\d+)" % name, line)
        return int(m.group(1)) if m else None
    crc = re.search(r"crc=0x([0-9a-f]+)", line)
    return {"n": int(last.group(1)), "crc": crc.group(1) if crc else None,
            "gx": g("gx"), "gy": g("gy"), "w": g("w"), "h": g("h"), "t": g("t")}


# --------------------------------------------------------------------- OCR
def tsv_words(png, psm=6):
    """tesseract TSV on `png` -> [dict(t,x,y,w,h,conf,line)] in PNG pixels.

    `line` is tesseract's (block, par, line) tuple: two words that share it are
    on one text line, which is what makes a multi-word label ("New colony")
    findable as a phrase instead of two unrelated tokens."""
    try:
        out = subprocess.run(["tesseract", png, "stdout", "--psm", str(psm), "tsv"],
                             capture_output=True, text=True, timeout=180).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    words = []
    for line in out.splitlines()[1:]:
        p = line.split("\t")
        if len(p) < 12:
            continue
        txt = p[11].strip()
        if not txt:
            continue
        try:
            conf = float(p[10])
            x, y, w, h = int(p[6]), int(p[7]), int(p[8]), int(p[9])
        except ValueError:
            continue
        if w > 0 and h > 0:
            words.append({"t": txt, "x": x, "y": y, "w": w, "h": h,
                          "conf": conf, "line": (p[1], p[2], p[3], p[4])})
    return words


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


# ---------------------------------------------------------------- liveness
def liveness_quiet(now, last_present_at, last_open_at):
    """Seconds since the front end last showed progress.

    L87: a frozen present counter is NOT a stall while the def loader is still
    opening files.  The measured load has a 215 s window with no present at
    all (L78 M2: n=280 at t=322 s, n=290 at t=537 s) while [XO] rows keep
    arriving; the menu shows up at n~600 after ~650 s.  A present and a loader
    open are both progress, so the quiet clock is the later of the two.
    """
    return now - max(last_present_at, last_open_at)


def is_stalled(quiet_s, stall_s):
    """True only after BOTH signals have been quiet for the whole budget."""
    return quiet_s > stall_s


def _box(ws):
    x0 = min(w["x"] for w in ws)
    y0 = min(w["y"] for w in ws)
    x1 = max(w["x"] + w["w"] for w in ws)
    y1 = max(w["y"] + w["h"] for w in ws)
    return {"t": " ".join(w["t"] for w in ws), "x": x0, "y": y0,
            "w": x1 - x0, "h": y1 - y0,
            "conf": min(w["conf"] for w in ws),
            "line": ws[0]["line"]}


def find_phrase(words, want, min_conf=25.0, fuzzy=True):
    """Every place `want` appears, as one box per occurrence.

    Three passes, cheapest first:
      1. a single word that contains the (normalised) phrase -- "Ignore";
      2. words on ONE tesseract line whose concatenation contains the phrase,
         which is how a two-word button label is read ("New" + "colony");
      3. a fuzzy first-word match when OCR mangled a long word ("Cassandra" ->
         "Cassandrra"), restricted to >= 6 letters so short labels cannot
         cross-match.
    """
    w = norm(want)
    if not w:
        return []
    out = []
    for x in words:
        if x["conf"] < min_conf:
            continue
        t = norm(x["t"])
        if t and w in t:
            out.append({**x, "t": x["t"]})
    if out:
        return out
    lines = {}
    for x in words:
        if x["conf"] < min_conf:
            continue
        lines.setdefault(x["line"], []).append(x)
    for ws in lines.values():
        ws = sorted(ws, key=lambda k: k["x"])
        joined, spans = "", []
        for x in ws:
            t = norm(x["t"])
            joined += t
            spans.append((len(joined) - len(t), len(joined), x))
        pos = joined.find(w)
        if pos >= 0:
            end = pos + len(w)
            hit = [x for (a, b, x) in spans if a < end and b > pos]
            if hit:
                out.append(_box(hit))
    if out or not fuzzy or len(w) < 6:
        return out
    # last resort: long word, first 5 letters equal, one edit of length
    for x in words:
        if x["conf"] < min_conf:
            continue
        t = norm(x["t"])
        if len(t) >= 6 and len(w) >= 6 and t[:5] == w[:5]:
            out.append({**x, "t": x["t"]})
    return out


def find_words(words, want, fuzzy=1):
    return find_phrase(words, want)


def uniq_rows(hits, tol=10):
    """Collapse duplicate OCR hits (same label on one line)."""
    rows = []
    for h in sorted(hits, key=lambda h: (h["y"], h["x"])):
        if any(abs(h["y"] - r["y"]) <= tol and abs(h["x"] - r["x"]) < 60
               for r in rows):
            continue
        rows.append(h)
    return rows


# ------------------------------------------------------------------ pixels
def diff_stats(a_png, b_png, thr=8):
    a = np.asarray(Image.open(a_png).convert("RGB")).astype(np.int16)
    b = np.asarray(Image.open(b_png).convert("RGB")).astype(np.int16)
    if a.shape != b.shape:
        return {"pixels": None, "bbox": None, "note": "shape mismatch"}
    d = np.abs(a - b).max(axis=2)
    mask = d > thr
    n = int(mask.sum())
    if n == 0:
        return {"pixels": 0, "bbox": None, "total": int(mask.size)}
    ys, xs = np.nonzero(mask)
    return {"pixels": n, "bbox": [int(xs.min()), int(ys.min()),
                                  int(xs.max()), int(ys.max())],
            "total": int(mask.size),
            "frac": round(n / float(mask.size), 5)}


def bbox_inside(inner, outer, pad=0):
    if not inner or not outer:
        return False
    return (inner[0] >= outer[0] - pad and inner[1] >= outer[1] - pad
            and inner[2] <= outer[2] + pad and inner[3] <= outer[3] + pad)


def changed_frac(a_png, b_png, box, thr=8):
    """Fraction of `box` (screen coordinates) whose pixels differ between the
    two shots.  This is how the modal's disappearance is attributed: the whole
    modal rect should change (it was a dialog, now it is not), while a box on
    an untouched part of the screen stays near zero."""
    try:
        a = np.asarray(Image.open(a_png).convert("RGB")).astype(np.int16)
        b = np.asarray(Image.open(b_png).convert("RGB")).astype(np.int16)
    except Exception:
        return None
    if not box:
        return None
    x0, y0, x1, y1 = [int(v) for v in box]
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(a.shape[1] - 1, x1), min(a.shape[0] - 1, y1)
    if x1 <= x0 or y1 <= y0:
        return None
    d = np.abs(a[y0:y1, x0:x1] - b[y0:y1, x0:x1]).max(axis=2)
    return round(float((d > thr).mean()), 4)


def box_union(boxes):
    boxes = [b for b in boxes if b]
    if not boxes:
        return None
    return [min(b[0] for b in boxes), min(b[1] for b in boxes),
            max(b[2] for b in boxes), max(b[3] for b in boxes)]


BROWN = ((106, 81, 46), (136, 100, 50), (150, 113, 54))


def brown_buttons(png, box=None):
    """Centres of RimWorld's flat-brown dialog buttons inside `box` (client
    crop), in PNG coordinates of `png`.  Same rule as rw_flow.brown_buttons:
    the affirmative on every dialog is the RIGHTMOST footer band."""
    try:
        im = Image.open(png).convert("RGB")
    except Exception:
        return []
    if box:
        im = im.crop(box)
    a = np.asarray(im)
    m = np.zeros(a.shape[:2], dtype=bool)
    for col in BROWN:
        m |= np.all(a == np.array(col), axis=2)
    h, w = m.shape
    bands = []
    for y in range(h // 2, h):
        xs = np.nonzero(m[y])[0]
        if len(xs) == 0:
            continue
        runs, s0, p = [], int(xs[0]), int(xs[0])
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
                if y - b["y1"] <= 6 and not (x1r < b["x0"] - 6 or x0r > b["x1"] + 6):
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
        bw, bh = b["x1"] - b["x0"], b["y1"] - b["y0"]
        if not (60 <= bw <= 420 and 16 <= bh <= 50):
            continue
        out.append(((b["x0"] + b["x1"]) // 2, (b["y0"] + b["y1"]) // 2,
                    (b["x0"], b["y0"], b["x1"], b["y1"])))
    if not out:
        return out
    ymax = max(t[1] for t in out)
    footer = sorted([t for t in out if t[1] >= ymax - 40], key=lambda t: -t[0])
    return footer


def find_brown_pair(png, box, min_run=80):
    """Fallback for the Steam modal when OCR cannot read `Ignore`.

    Same rule as tools/rw/click-ignore.py / rw_desktop_drive.find_ignore_xy --
    the modal's two buttons are flat brown bands on one row and the LEFT one is
    Ignore -- but scanned inside the game window's client crop and with a
    minimum run scaled for a 1280x720 content area.  Returns the screen (x,y)
    of the left button's centre, or None."""
    try:
        im = Image.open(png).convert("RGB")
    except Exception:
        return None
    im = im.crop(box)
    px = im.load()
    W, H = im.size
    band = {}
    for y in range(H // 3, H):
        xs = [x for x in range(W) if px[x, y] == BROWN[0]]
        if len(xs) > 2 * min_run:
            band[y] = xs
    if not band:
        return None
    ys = sorted(band)
    groups, cur = [], [ys[0]]
    for y in ys[1:]:
        if y - cur[-1] <= 2:
            cur.append(y)
        else:
            groups.append(cur)
            cur = [y]
    groups.append(cur)
    g = max(groups, key=len)
    if len(g) < 10:
        return None
    cols = [0] * W
    for y in g:
        for x in band[y]:
            cols[x] += 1
    runs, start = [], None
    for x in range(W):
        if cols[x] > len(g) // 3 and start is None:
            start = x
        elif cols[x] <= len(g) // 3 and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, W - 1))
    runs = [r for r in runs if r[1] - r[0] >= min_run]
    if len(runs) < 2:
        return None
    left = runs[0]
    return (box[0] + (left[0] + left[1]) // 2, box[1] + (g[0] + g[-1]) // 2)


class L86:
    def __init__(self, q, grab, log_path, args, shots):
        self.q = q
        self.grab = grab
        self.log_path = log_path
        self.args = args
        self.shots = shots
        self.out_dir = os.path.abspath(getattr(args, "shot_dir", None)
                                       or os.path.dirname(os.path.abspath(log_path)))
        self.jsonl = os.path.join(self.out_dir, "l86.jsonl")
        self.run_id = time.strftime("%Y%m%dT%H%M%S")
        self.n = 0
        self.t0 = time.time()
        self.geom = None
        self.content = None      # (x, y) of game content (0,0) on screen
        self.crop_box = None     # client-area crop in screen coords
        self.rows = []           # report rows

    # -- plumbing ---------------------------------------------------------
    def serial(self):
        try:
            t = open(self.log_path, errors="replace").read()
        except OSError:
            return ""
        cut = t.find("--- Player.log (")
        return t[:cut] if cut > 0 else t

    def emit(self, rec):
        rec.setdefault("ev", "note")
        rec["run"] = self.run_id
        rec["wall"] = round(time.time(), 3)
        rec["elapsed"] = round(time.time() - self.t0, 1)
        with open(self.jsonl, "a") as f:
            f.write(json.dumps(rec) + "\n")
        keep = {k: v for k, v in rec.items()
                if k in ("ev", "stage", "label", "screen", "content", "shot",
                         "pixels", "bbox", "text", "note", "ok", "hold",
                         "field_before", "field_after", "n", "crc", "elapsed")}
        print("[L86] " + json.dumps(keep, ensure_ascii=False)[:800], flush=True)

    def shot(self, tag):
        self.n += 1
        p = self.grab(f"l86_{self.n:03d}_{re.sub(r'[^A-Za-z0-9]+', '_', tag)}")
        self.refresh_content()
        return p

    def establish_geometry(self, timeout=120):
        """Wait for `[HB] geometry` and derive the content origin on screen.

        The client area includes HBHOST's 16 px menu bar at the top; the game's
        content starts at (gx, gy) *inside* that area.  The guest's own
        `[XSW] ... gx= gy=` row carries it, but the row is `-1 -1` until the
        window has its content rect (the first rows after the pixel-mode
        switch), so a negative value falls back to HBHOST's menu-bar height
        (16, the `[HB] hd_set_size ... content=0,16` row)."""
        end = time.time() + timeout
        while time.time() < end:
            g = parse_geometry(self.serial())
            if g:
                self.geom = g
                self.crop_box = (g[0], g[1], g[0] + g[2], g[1] + g[3])
                self.refresh_content()
                x = parse_xsw(self.serial())
                self.emit({"ev": "geometry", "stage": "geom",
                           "note": "client=%d,%d %dx%d xsw gx=%s gy=%s w=%s h=%s"
                                   % (g[0], g[1], g[2], g[3],
                                      x.get("gx") if x else None,
                                      x.get("gy") if x else None,
                                      x.get("w") if x else None,
                                      x.get("h") if x else None),
                           "screen": [self.content[0], self.content[1]],
                           "content": [0, 0]})
                print("[L86] content origin on screen = (%d,%d); client area "
                      "(%d,%d)-(%d,%d)" % (self.content[0], self.content[1],
                                           self.crop_box[0], self.crop_box[1],
                                           self.crop_box[2], self.crop_box[3]),
                      flush=True)
                return True
            time.sleep(2)
        return False

    def refresh_content(self):
        """Re-derive the content origin; cheap enough to call per shot."""
        if not self.geom:
            return
        x = parse_xsw(self.serial())
        gx, gy = 0, 16
        if x and x.get("gx") is not None and x["gx"] >= 0:
            gx, gy = x["gx"], x.get("gy") or 0
        self.content = (self.geom[0] + gx, self.geom[1] + gy)

    def to_content(self, sx, sy):
        if not self.content:
            return (None, None)
        return (sx - self.content[0], sy - self.content[1])

    def ocr(self, png, tag="ocr", psm=6, upscale=2, box=None):
        """OCR the client area (2x upscale) -> words in SCREEN coordinates.

        RimWorld's UI font is ~12 px at 1280x720; tesseract finds whole
        sentences of it only after a 2x upscale, and the upscale is why the
        box conversion divides by 2.
        """
        if png is None:
            return []
        cb = box or self.crop_box
        im = Image.open(png).convert("RGB")
        if cb:
            im = im.crop(cb)
        if upscale != 1:
            im = im.resize((im.size[0] * upscale, im.size[1] * upscale),
                           Image.LANCZOS)
        tmp = os.path.join(self.out_dir, "ocr_%s.png" % re.sub(r'[^A-Za-z0-9]+', '_', tag))
        im.save(tmp)
        words = []
        for wd in tsv_words(tmp, psm=psm):
            ox = (cb[0] if cb else 0) + wd["x"] // upscale
            oy = (cb[1] if cb else 0) + wd["y"] // upscale
            words.append({**wd, "x": ox, "y": oy,
                          "w": max(1, wd["w"] // upscale),
                          "h": max(1, wd["h"] // upscale)})
        return words

    def text_of(self, words):
        return " ".join(w["t"] for w in words)

    def find(self, words, want, fuzzy=0):
        hits = find_words(words, want, fuzzy=fuzzy)
        return uniq_rows(hits)

    # -- clicking ---------------------------------------------------------
    def click(self, label, sx, sy, stage, hold=0.12, settle=3.0, why=""):
        """One absolute click at SCREEN (sx,sy), logged in both frames."""
        label = label or "footer"
        cx, cy = self.to_content(sx, sy)
        before = self.shot("%s_before_%s" % (stage, label))
        print("[L86] click %-22s screen=(%d,%d) content=(%s,%s) hold=%.2f"
              % (label, sx, sy, cx, cy, hold), flush=True)
        self.q.click(sx, sy, self.args.fb_w, self.args.fb_h, hold=hold)
        time.sleep(settle)
        after = self.shot("%s_after_%s" % (stage, label))
        d = diff_stats(before, after) if before and after else {}
        self.emit({"ev": "click", "stage": stage, "label": label,
                   "screen": [sx, sy], "content": [cx, cy], "hold": hold,
                   "pixels": d.get("pixels"), "bbox": d.get("bbox"),
                   "shot": os.path.basename(after) if after else None,
                   "why": why})
        self.rows.append({"step": stage, "action": "click " + label,
                          "screen": [sx, sy], "content": [cx, cy],
                          "pixels": d.get("pixels"), "bbox": d.get("bbox"),
                          "shot": os.path.basename(after) if after else None})
        return before, after, d

    def wait_for_text(self, want, timeout, tag, psm=6, stage=""):
        """Poll OCR until every token in `want` (list) is on screen."""
        end = time.time() + timeout
        last = None
        words = []
        while time.time() < end:
            png = self.shot(tag)
            words = self.ocr(png, tag=tag, psm=psm)
            txt = self.text_of(words)
            last = (png, words, txt)
            if all(self.find(words, w) for w in want):
                return last
            time.sleep(2)
        return last

    # -- stages -----------------------------------------------------------
    def focus_window(self):
        """Give the game window X focus before the first content click.

        IceWM's default focus model is ClickToFocus; a click on the title bar
        (the WM's frame, above HBHOST's client area) focuses and raises the
        window without delivering anything to the game.  Doing it explicitly
        means the first content click is not spent on focusing."""
        if not self.geom:
            return
        tx = self.geom[0] + self.geom[2] // 2
        ty = self.geom[1] - 12
        if ty < 2:
            ty = 6
        self.emit({"ev": "focus", "stage": "focus", "screen": [tx, ty],
                   "note": "click IceWM title bar (WM frame, not the client)"})
        self.q.click(tx, ty, self.args.fb_w, self.args.fb_h)
        time.sleep(2.0)

    def dismiss_modal(self, png=None):
        """1. The Steam modal: click its Ignore with a coordinate read by OCR,
        then prove it is gone (OCR + a diff bbox that only covers the modal)."""
        if png is None:
            png = self.shot("modal_probe")
        words = self.ocr(png, tag="modal_probe")
        txt = self.text_of(words)
        hits = self.find(words, "Ignore")
        how = "ocr"
        if hits:
            h = hits[0]
            mx, my = h["x"] + h["w"] // 2, h["y"] + h["h"] // 2
            modal_words = [w for w in words if abs(w["y"] - h["y"]) < 260]
            modal_box = box_union([[w["x"], w["y"], w["x"] + w["w"],
                                    w["y"] + w["h"]] for w in modal_words])
        else:
            mx = my = None
            modal_box = None
        if mx is None:
            how = "brown-band"
            xy = find_brown_pair(png, self.crop_box)
            if not xy:
                self.emit({"ev": "modal", "stage": "modal", "ok": False,
                           "text": txt[:300],
                           "note": "no Ignore label and no brown button pair"})
                return png, words
            mx, my = xy
            modal_box = [self.crop_box[0], my - 90, self.crop_box[2], my + 40]
        self.emit({"ev": "modal", "stage": "modal", "ok": True, "how": how,
                   "screen": [mx, my], "bbox": modal_box,
                   "text": txt[:300], "note": "Ignore located"})
        before_png, after, d = self.click("Ignore", mx, my, "modal", hold=0.3,
                                          settle=4.0, why="dismiss Steam modal")
        w2 = self.ocr(after, tag="modal_after") if after else []
        t2 = self.text_of(w2)
        gone = not self.find(w2, "Ignore") and "steam" not in t2.lower()
        inside = bbox_inside(d.get("bbox"), modal_box, pad=24)
        frac = changed_frac(before_png, after, modal_box) if after else None
        self.emit({"ev": "modal_gone", "stage": "modal", "ok": bool(gone),
                   "bbox": d.get("bbox"), "pixels": d.get("pixels"),
                   "inside_modal": bool(inside), "modal_changed_frac": frac,
                   "text": t2[:400],
                   "shot": os.path.basename(after) if after else None})
        print("[L86] modal gone=%s  diff=%s px bbox=%s  %.1f%% of the modal "
              "rect changed  inside=%s"
              % (gone, d.get("pixels"), d.get("bbox"),
                 100.0 * (frac or 0), inside), flush=True)
        return after, w2

    def main_menu(self, timeout=None):
        """Wait for the main menu, dismissing the Steam modal when it is the
        thing on screen.

        The modal arrives with the menu, not with the window: waiting for
        "Ignore" once at launch is too early (measured: at 54 s the game is
        still on its banner, and the modal turned up ~5 min later).  So the
        modal is handled here, in the loop that is already waiting."""
        if timeout is None:
            timeout = int(os.environ.get("RW_L86_MENU_TIMEOUT", "1500"))
        end = time.time() + timeout
        png = None
        words = []
        dismissed = 0
        last_n, last_n_at = None, time.time()
        last_opens, last_opens_at = None, time.time()
        last_report = 0.0
        while time.time() < end:
            png = self.shot("menu_wait")
            words = self.ocr(png, tag="menu_wait")
            txt = self.text_of(words)
            if self.find(words, "New colony"):
                self.emit({"ev": "stage", "stage": "menu", "text": txt[:400],
                           "shot": os.path.basename(png) if png else None})
                return png, words, txt
            if self.find(words, "Ignore") or "could not initialize" in txt.lower():
                if dismissed < 4:
                    dismissed += 1
                    self.emit({"ev": "modal_seen", "stage": "menu",
                               "text": txt[:200], "note": "dismissing"})
                    self.dismiss_modal(png)
                    continue
            self.emit({"ev": "poll", "stage": "menu", "text": txt[:200],
                       "shot": os.path.basename(png) if png else None})
            # L87: a boot is not "stalled" because presents stopped.  The
            # measured front-end load has a 215 s window with no present at all
            # (L78 M2: n=280 at t=322 s, n=290 at t=537 s) while the def loader
            # is still opening files; the menu arrives at n~600 after ~650 s.
            # The old 150 s present-only rule fired inside that window, returned
            # control to the harness, and the harness killed QEMU -- which read
            # as a guest freeze in L86.  Liveness is presents OR loader opens,
            # and the quiet threshold is above the measured plateau.
            x = parse_xsw(self.serial())
            n = x["n"] if x else None
            opens = len(re.findall(r"^\[XO\] ", self.serial(), re.M))
            now = time.time()
            if n is not None and n != last_n:
                last_n, last_n_at = n, now
            if opens != last_opens:
                last_opens, last_opens_at = opens, now
            quiet = liveness_quiet(now, last_n_at, last_opens_at)
            if now - last_report >= 60:
                last_report = now
                self.emit({"ev": "loading", "stage": "menu", "n": n,
                           "opens": opens, "quiet": int(quiet)})
                print("[L86] loading: n=%s opens=%d quiet=%ds"
                      % (n, opens, int(quiet)), flush=True)
            stall_s = int(os.environ.get("RW_L86_STALL_SECS", "900"))
            if n is not None and is_stalled(quiet, stall_s):
                self.emit({"ev": "stalled", "stage": "menu", "n": n,
                           "opens": opens, "quiet": int(quiet),
                           "note": "no present and no loader open for %ds"
                                   % int(quiet)})
                print("[L86] STALLED: n=%s opens=%d quiet=%ds"
                      % (n, opens, int(quiet)), flush=True)
                return png, words, txt
            time.sleep(15)
        return png, words, self.text_of(words)

    FOOTER_WORDS = ("Next", "Generate", "Start", "OK", "Accept", "Select",
                    "Confirm", "Continue", "Settle")

    def find_advance(self, png, words, label):
        """Where to click to move on, as (screen_x, screen_y, how).

        OCR first: the affirmative is a word, and at 1280x720 the footer is a
        single full-width brown strip with the labels drawn on it (measured:
        `Back` at (536,736) and `Next` at (1373,736) on the scenario page), so
        the 1920x1080 "rightmost flat-brown band" rule cannot find it -- one
        band spans the window.  The band rule survives as the fallback for
        dialogs whose labels OCR cannot read.
        """
        if label:
            hits = self.find(words, label)
            if hits:
                h = hits[-1] if len(hits) > 1 else hits[0]
                # the bottom-most/right-most occurrence is the footer button
                h = sorted(hits, key=lambda k: (-k["y"], -k["x"]))[0]
                return (h["x"] + h["w"] // 2, h["y"] + h["h"] // 2, "ocr:" + h["t"])
            return (None, None, "none")
        cands = []
        for w in self.FOOTER_WORDS:
            for h in self.find(words, w):
                cands.append((h["y"], h["x"], h))
        if cands:
            cands.sort(key=lambda c: (-c[0], -c[1]))
            h = cands[0][2]
            return (h["x"] + h["w"] // 2, h["y"] + h["h"] // 2,
                    "ocr-footer:" + h["t"])
        bufs = brown_buttons(png, (self.crop_box[0], self.crop_box[1],
                                   self.crop_box[2] - self.crop_box[0],
                                   self.crop_box[3] - self.crop_box[1]))
        if bufs:
            bx, by, _ = bufs[0]
            return (bx + self.crop_box[0], by + self.crop_box[1], "brown-band")
        return (None, None, "none")

    def advance(self, stage, label, expect, timeout=60, how="ocr",
                hold=0.12, must_change=True):
        """Click `label` (or the footer affirmative when label is None) and
        prove the result: OCR must show one of `expect` afterwards when the
        click is a transition; a selection click (`must_change=False`) is
        proven by the pixel diff instead."""
        png = self.shot("adv_%s_probe" % stage)
        words = self.ocr(png, tag="adv_%s" % stage)
        sx, sy, why = self.find_advance(png, words, label)
        if sx is None:
            self.emit({"ev": "no_target", "stage": stage, "label": label,
                       "text": self.text_of(words)[:400]})
            return False, png, words
        self.emit({"ev": "target", "stage": stage, "label": label,
                   "screen": [sx, sy], "note": why})
        _, after, d = self.click(label, sx, sy, stage, hold=hold)
        # prove the transition by OCR, polling while the frame settles.  A
        # first click that changes nothing gets ONE more click at the same
        # coordinates (IceWM can spend the first click of an unfocused window
        # on focusing) -- never a different button.
        for attempt in (1, 2):
            end = time.time() + timeout
            w2 = self.ocr(after, tag="%s_after" % stage) if after else []
            t2 = self.text_of(w2)
            while time.time() < end:
                if any(self.find(w2, e) for e in expect):
                    break
                time.sleep(2)
                after = self.shot("%s_settle" % stage)
                w2 = self.ocr(after, tag="%s_settle" % stage) if after else []
                t2 = self.text_of(w2)
            ok = any(self.find(w2, e) for e in expect)
            if not must_change:
                ok = bool((d.get("pixels") or 0) > 300)
            if ok or attempt == 2:
                break
            px = d.get("pixels") or 0
            if px > 2000:
                break
            self.emit({"ev": "retry", "stage": stage, "label": label,
                       "note": "no OCR change and %s px; clicking once more" % px})
            _, after, d = self.click(label, sx, sy, stage + "_retry", hold=hold)
        self.emit({"ev": "click_result", "stage": stage, "label": label,
                   "ok": bool(ok), "pixels": d.get("pixels"),
                   "bbox": d.get("bbox"), "text": t2[:500],
                   "shot": os.path.basename(after) if after else None})
        print("[L86] %-10s click %-18s -> %s (pixels=%s bbox=%s)"
              % (stage, str(label), "ADVANCED" if ok else "NO CHANGE",
                 d.get("pixels"), d.get("bbox")), flush=True)
        return ok, after, w2

    def radio(self, stage, label, row_tol=14):
        """Click a radio option: the label's own row, at the ring if one is
        drawn to its right, else on the label."""
        png = self.shot("radio_%s" % label)
        words = self.ocr(png, tag="radio_%s" % label)
        hits = self.find(words, label)
        if not hits:
            self.emit({"ev": "no_target", "stage": stage, "label": label,
                       "text": self.text_of(words)[:300]})
            return False, png
        h = hits[0]
        ly = h["y"] + h["h"] // 2
        # ring search: bright pixels on the label's row, right of the text
        im = Image.open(png).convert("RGB").crop(self.crop_box)
        a = np.asarray(im).astype(int)
        ox, oy = self.crop_box[0], self.crop_box[1]
        yy = ly - oy
        band = a[max(0, yy - 9):yy + 10, h["x"] - ox + h["w"]:a.shape[1], :]
        bright = ((band[:, :, 0] > 170) & (band[:, :, 1] > 170)
                  & (band[:, :, 2] > 170))
        cols = bright.sum(axis=0)
        xs = np.nonzero(cols >= 2)[0]
        target = (h["x"] + h["w"] // 2, ly)
        if len(xs):
            runs, s0, p = [], int(xs[0]), int(xs[0])
            for x in xs[1:]:
                x = int(x)
                if x - p > 10:
                    runs.append((s0, p))
                    s0 = x
                p = x
            runs.append((s0, p))
            for (x0, x1) in runs:
                if 10 <= x1 - x0 <= 30:
                    target = (h["x"] + h["w"] + (x0 + x1) // 2, ly)
                    break
        _, after, d = self.click(label, target[0], target[1], stage, hold=0.15)
        self.emit({"ev": "radio", "stage": stage, "label": label,
                   "screen": list(target), "pixels": d.get("pixels"),
                   "bbox": d.get("bbox")})
        return True, after

    # -- keyboard ---------------------------------------------------------
    def type_seed(self, digits="12345"):
        """3. Click the Seed field, type digits with QMP send-key, OCR again."""
        png = self.shot("worldopts_seed_probe")
        words = self.ocr(png, tag="seed")
        hits = self.find(words, "Seed")
        if not hits:
            self.emit({"ev": "seed", "ok": False, "note": "no Seed label",
                       "text": self.text_of(words)[:400]})
            return False
        h = hits[0]
        # the field sits to the right of the label on the same row
        row = [w for w in words
               if abs((w["y"] + w["h"] // 2) - (h["y"] + h["h"] // 2)) <= 12]
        row.sort(key=lambda w: w["x"])
        fx = h["x"] + h["w"] + 90
        if len(row) > 1:
            nxt = [w for w in row if w["x"] > h["x"] + h["w"] + 8]
            if nxt:
                fx = nxt[0]["x"] + nxt[0]["w"] // 2
        fy = h["y"] + h["h"] // 2
        # OCR the field's own strip before typing
        strip = (h["x"] - 4, fy - 18,
                 min(self.crop_box[2], h["x"] + 420), fy + 18)
        before_txt = self.text_of(self.ocr(png, tag="seed_before", psm=7, box=strip))
        self.emit({"ev": "seed_click", "stage": "seed", "screen": [fx, fy],
                   "field_before": before_txt,
                   "note": "seed label at (%d,%d)" % (h["x"], h["y"])})
        _, after, d = self.click("Seed", fx, fy, "seed", hold=0.15, settle=3.0)
        clicked_txt = self.text_of(self.ocr(after, tag="seed_focus", psm=7, box=strip)) \
            if after else ""
        self.emit({"ev": "seed_focus", "stage": "seed", "field_before": clicked_txt,
                   "pixels": d.get("pixels"), "bbox": d.get("bbox")})
        # type the digits: QMP qcodes '1'..'9'
        for ch in digits:
            self.q.key(ch, pause=0.35)
        time.sleep(3.0)
        typed = self.shot("seed_typed")
        after_txt = self.text_of(self.ocr(typed, tag="seed_after", psm=7, box=strip))
        d2 = diff_stats(after, typed) if after and typed else {}
        changed = bool(after_txt.strip()) and after_txt != clicked_txt
        self.emit({"ev": "seed_typed", "stage": "seed", "ok": bool(changed),
                   "field_before": clicked_txt, "field_after": after_txt,
                   "pixels": d2.get("pixels"), "bbox": d2.get("bbox"),
                   "shot": os.path.basename(typed) if typed else None})
        print("[L86] seed field OCR before=%r after=%r typed=%r (pixels=%s)"
              % (clicked_txt, after_txt, digits, d2.get("pixels")), flush=True)
        return changed

    def tab_probe(self):
        """3b. One Tab: the focus ring is a small, localised change."""
        png = self.shot("tab_before")
        self.q.key("tab", pause=0.6)
        time.sleep(2.5)
        png2 = self.shot("tab_after")
        d = diff_stats(png, png2)
        self.emit({"ev": "tab", "stage": "tab", "pixels": d.get("pixels"),
                   "bbox": d.get("bbox"), "note": "one Tab, no other input"})
        print("[L86] Tab: %s px changed, bbox=%s" % (d.get("pixels"),
                                                     d.get("bbox")), flush=True)
        return d

    # -- world gen / site --------------------------------------------------
    def wait_generating(self, timeout=420):
        end = time.time() + timeout
        png = None
        words = []
        while time.time() < end:
            png = self.shot("gen_wait")
            words = self.ocr(png, tag="gen_wait", psm=6)
            txt = self.text_of(words)
            if self.find(words, "Select starting site") or \
                    self.find(words, "starting site"):
                self.emit({"ev": "stage", "stage": "site",
                           "text": txt[:400],
                           "shot": os.path.basename(png) if png else None})
                return png, words, True
            if self.find(words, "World generation") or self.find(words, "Generating"):
                self.emit({"ev": "generating", "stage": "generating",
                           "text": txt[:200]})
            time.sleep(6)
        return png, words, False

    def land_point(self, png):
        """A point on the planet's land, inside the content rect, away from
        the info panel and the top bar (rw_flow.land_point's rule, cropped)."""
        im = Image.open(png).convert("RGB").crop(self.crop_box)
        a = np.asarray(im).astype(int)
        h, w, _ = a.shape
        r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
        land = (r >= 75) & (r >= b - 8) & (g >= 40) & (r < 235) & (g < 225)
        land[:70, :] = False
        land[h - 120:, :] = False
        land[:, :40] = False
        land[:, w - 40:] = False
        k = 9
        ii = np.zeros((h + 1, w + 1), dtype=np.int32)
        ii[1:, 1:] = np.cumsum(np.cumsum(land.astype(np.int32), 0), 1)
        best = None
        for y in range(80, h - 130, 12):
            for x in range(50, w - 50, 12):
                y0, x0 = y - k // 2, x - k // 2
                s = (ii[y0 + k, x0 + k] - ii[y0, x0 + k]
                     - ii[y0 + k, x0] + ii[y0, x0])
                if s < int(k * k * 0.9):
                    continue
                d = (x - w // 2) ** 2 + (y - h // 2) ** 2
                if best is None or d < best[0]:
                    best = (d, x, y)
        if not best:
            return None
        return (self.crop_box[0] + best[1], self.crop_box[1] + best[2])


def run(q, grab, log_path, args, shots):
    d = L86(q, grab, log_path, args, shots)
    os.makedirs(d.out_dir, exist_ok=True)
    if not d.establish_geometry(240):
        print("[L86] FAIL: no [HB] geometry row -- the hosted window never "
              "reported its rect", flush=True)
        return 3

    # 0. focus the window through the WM's frame, then the first content click
    d.focus_window()

    # 1. the Steam modal arrives with the menu, so main_menu() below owns it.
    # 2. menu -> scenario -> storyteller -> worldopts, every click OCR-located
    png, words, txt = d.main_menu()
    ok, png, words = d.advance("menu", "New colony",
                               ["Choose scenario", "Crashlanded",
                                "Rich explorer", "Scenario"], 120)
    if ok:
        # Crashlanded is a card: clicking it only changes the selection
        d.advance("scenario", "Crashlanded",
                  ["Three crashlanded survivors", "Crashlanded"], 40,
                  must_change=False)
        ok, png, words = d.advance("scenario", None,
                                   ["Choose storyteller", "Cassandra",
                                    "storyteller", "Difficulty"], 120)
    if ok:
        d.advance("storyteller", "Cassandra Classic", ["Cassandra"], 40,
                  must_change=False)
        d.radio("storyteller", "Strive to survive")
        d.radio("storyteller", "Reload anytime mode")
        ok, png, words = d.advance("storyteller", None,
                                   ["Create world", "Planet coverage",
                                    "Globe coverage", "Generate", "Seed"], 120)
    got_site = False
    if ok:
        # 3. keyboard: the seed field
        d.type_seed("12345")
        d.tab_probe()
    if ok:
        # 4. Generate -> world gen -> site.  Generate's proof is the next
        # screen and world generation takes tens of seconds: the click is
        # verified by its pixel change here, the transition by
        # wait_generating() below.
        d.advance("worldopts", "Generate", ["Select starting site"], 5,
                  must_change=False)
        png, words, got_site = d.wait_generating(480)
    if got_site:
        lp = d.land_point(png) if png else None
        if lp:
            d.emit({"ev": "site_point", "stage": "site", "screen": list(lp)})
            _, after, dd = d.click("tile", lp[0], lp[1], "site", hold=2.0,
                                   settle=4.0, why="2 s hold selects a tile")
            w2 = d.ocr(after, tag="site_panel") if after else []
            t2 = d.text_of(w2)
            self_hit = any(d.find(w2, k) for k in
                           ("Temperate", "Terrain", "Movement", "Biome",
                            "Rainfall", "Temperature"))
            d.emit({"ev": "site_panel", "stage": "site", "ok": bool(self_hit),
                    "pixels": dd.get("pixels"), "bbox": dd.get("bbox"),
                    "text": t2[:600],
                    "shot": os.path.basename(after) if after else None})
            print("[L86] tile panel=%s text=%r" % (self_hit, t2[:200]), flush=True)
            # look for a confirm control
            cands = [k for k in ("OK", "Select", "Accept", "Next", "Settle",
                                 "Confirm", "Start") if d.find(w2, k)]
            d.emit({"ev": "site_confirm_candidates", "stage": "site",
                    "text": ",".join(cands) or "none"})
            for k in cands[:3]:
                d.advance("site", k, ["Create characters", "characters",
                                      "Backstory", "Ideoligion"], 30)
            for key in ("ret", "spc"):
                before = d.shot("site_key_%s" % key)
                d.q.key(key, pause=0.6)
                time.sleep(3)
                after2 = d.shot("site_key_%s_after" % key)
                dk = diff_stats(before, after2)
                okc = bool(d.find(d.ocr(after2, tag="site_key_%s" % key),
                                  "characters"))
                d.emit({"ev": "site_key", "stage": "site", "label": key,
                        "pixels": dk.get("pixels"), "bbox": dk.get("bbox"),
                        "ok": okc})
                if okc:
                    break
            # double click on the same tile
            before = d.shot("site_dclick_before")
            d.q.click(lp[0], lp[1], args.fb_w, args.fb_h, hold=0.15)
            time.sleep(0.4)
            d.q.click(lp[0], lp[1], args.fb_w, args.fb_h, hold=0.15)
            time.sleep(4)
            after3 = d.shot("site_dclick_after")
            dk = diff_stats(before, after3)
            d.emit({"ev": "site_double", "stage": "site",
                    "pixels": dk.get("pixels"), "bbox": dk.get("bbox"),
                    "text": d.text_of(d.ocr(after3, tag="site_dclick"))[:300]})
    d.shot("final")
    d.emit({"ev": "done", "stage": "end",
            "note": "reached_site=%s" % got_site})
    with open(os.path.join(d.out_dir, "l86-rows.json"), "w") as f:
        json.dump(d.rows, f, indent=1)
    return 0
