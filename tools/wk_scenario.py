#!/usr/bin/env python3
"""wk_scenario.py - the HobbyOS WebKit browser multi-site scenario suite.

Each scenario is an end-to-end user story driven ONLY through QMP input
(apps menu, mouse clicks, keyboard) against a live HobbyOS ARM desktop, and
judged from the guest's own serial receipts plus the framebuffer.

  python3 tools/wk_scenario.py --id S1 --lane a        # one scenario, own lane
  python3 tools/wk_scenario.py --id S0 --id S1         # several, sequentially
  python3 tools/wk_scenario.py --list

Every lane boots its own QEMU (own working dir, own disk clone, own QMP
socket), so several scenarios can run at the same time from different
processes -- that is how the parallel test sweep is done.

Results land in <lane dir>/report.json with one row per step:
  {"step": "...", "ok": true|false, "detail": "...", "evidence": {...}}
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wk_lane import Lane  # noqa: E402

CHROME_H = 84


# ---------------------------------------------------------------------------
# helpers shared by the scenarios
# ---------------------------------------------------------------------------
def nav_receipts(L, since):
    seg = L.text()[since:]
    return {
        "fetch": re.findall(r"\[WIN\] net fetch url=(\S+) ok=(\d+) status=(\d+) bytes=(\d+) "
                            r"effective=(\S*) err=(.*)", seg),
        "load_ok": re.findall(r"\[WIN\] load-ok url=(\S+) ms=(\d+)", seg),
        "fail": re.findall(r"\[WIN\] (?:load open-fail|FAIL load-timeout)(.*)", seg),
        "frames": re.findall(r"\[WIN\] frame view=(\d+)x(\d+) nonZero=(\d+) checksum=0x([0-9a-f]+)", seg),
    }


def new_nav(L, since, timeout=420, label="nav"):
    """Wait for a navigation to settle after `since`; returns (url, failed)."""
    m = L.wait_for(r"\[WIN\] (load-ok url=\S+|load open-fail|FAIL load-timeout)",
                   timeout=timeout, since=since, label=label)
    if "open-fail" in m.group(0) or "load-timeout" in m.group(0):
        return None, True
    if "load-ok" in m.group(0):
        url = m.group(0).split("url=", 1)[1].strip()
        L.wait_for(r"\[WIN\] frame view=", timeout=timeout, since=since, label=label + " frame")
        return url, False
    return None, False


def click_page(L, px, py):
    """Click a page-viewport point, correcting for the driver's screen-space
    first-click quirk (see hobbyos-gui-test pitfall 0)."""
    return L.click_page(px, py)


def find_link(L, pred, max_y=800):
    for href, x, y, w, h in L.links():
        if w < 4 or h < 4 or y < 0 or y > max_y:
            continue
        if pred(href):
            return href, x + w // 2, y + h // 2
    return None


def link_candidates(L, pred, max_y=920, avoid=None):
    """All in-viewport census entries matching pred, ordered by preference.

    The census is NOT visibility-filtered (hidden nav anchors still carry a
    layout rect), so a candidate click can land on whatever is painted on top.
    Callers try them in order and keep the first that emits `[WIN] link ->`.
    `avoid` is a host string whose self-links (bare-host hrefs) rank last --
    clicking a site's own logo just reloads the page it is already on.
    """
    out = []
    for href, x, y, w, h in L.links():
        if w < 8 or h < 6 or y < 0 or y + h > max_y or x < 0:
            continue
        if pred(href):
            out.append((href, x + w // 2, y + h // 2, x, y, w, h))

    def score(t):
        href = t[0]
        paths = href.split("://", 1)[-1]
        bare_host = "/" not in paths
        self_host = bool(avoid) and avoid.lower() in href.lower()
        return (bare_host, self_host, t[3] < 40, t[1] < 0)

    out.sort(key=score)
    return out


def click_link_until_nav(L, cands, timeout=420, label="link"):
    """Click candidate links until one navigates. Returns (url, attempts, since).

    attempts = [{"href","px","py","link_receipt","navigated"}] -- the misses
    are evidence about census rects that are not clickable where they claim.
    """
    attempts = []
    for href, px, py, *_ in cands[:6]:
        mm = L.mark()
        click_page(L, px, py)
        time.sleep(1.2)
        got = re.findall(r"\[WIN\] link -> (\S+)", L.text()[mm:])
        attempts.append({"href": href, "px": px, "py": py,
                         "link_receipt": got[-1] if got else None,
                         "focus": (re.findall(r"\[WIN\] focus=(.*)", L.text()[mm:]) or [""])[-1],
                         "navigated": bool(got)})
        if got:
            url, failed = new_nav(L, mm, timeout=timeout, label=label)
            return url, attempts, mm
        time.sleep(0.5)
    return None, attempts, 0


def row(step, ok, detail, **ev):
    return {"step": step, "ok": bool(ok), "detail": detail, **({"evidence": ev} if ev else {})}


# ---------------------------------------------------------------------------
# S0 - the user's original acceptance story (google.com)
# ---------------------------------------------------------------------------
def s0(L, R):
    L.shot("00-desktop")
    L.launch_browser()
    m, doc, failed = L.navigate("https://www.google.com", timeout=600)
    L.shot("01-google")
    R.append(row("address bar -> GO loads google.com", not failed,
                 "failed" if failed else "load-ok", doc=doc, nav=nav_receipts(L, m)))
    if failed:
        return
    fields = L.inputs()
    R.append(row("page has a search <input>", bool(fields), str(fields[:2])))
    if not fields:
        return
    name, fx, fy, fw, fh = fields[0]
    echo = click_page(L, fx + fw // 2, fy + fh // 2)
    L.shot("02-focused")
    mm = L.mark()
    L.q.keys("cat pictures")
    time.sleep(1.5)
    L.shot("03-typed")
    typed = "".join(chr(int(k, 16)) for k in
                    re.findall(r"\[WIN\] page-key ch=0x([0-9a-f]{2})", L.text()[mm:]))
    probe = re.findall(r"\[WIN\] page-value=(.*)|\[WIN\] value (.*)", L.text()[mm:])
    R.append(row("mouse click focuses the page search box", bool(echo),
                 "click echo %s" % (echo,), focus=re.findall(r"\[WIN\] focus=(.*)", L.text()[mm:])[-1:]))
    R.append(row("keyboard types 'cat pictures' into it", typed.strip() == "cat pictures",
                 "forwarded=%r" % typed, probe=str(probe[-1:])))


# ---------------------------------------------------------------------------
# S1 - Wikipedia: click an in-article link with the mouse, then go Back
# ---------------------------------------------------------------------------
def s1(L, R):
    L.launch_browser()
    # Part 1: a page whose link is definitely visible -- example.com's IANA link.
    m, doc, failed = L.navigate("https://example.com", timeout=600)
    L.shot("01-example")
    R.append(row("example.com renders", not failed, (doc or {}).get("raw", "failed"),
                 nav=nav_receipts(L, m)))
    if failed:
        return
    cands = link_candidates(L, lambda h: True)
    R.append(row("link census exposes the page's link", bool(cands),
                 "%d candidate(s): %s" % (len(cands), [c[0] for c in cands[:3]])))
    if not cands:
        return
    url, attempts, mm = click_link_until_nav(L, cands, timeout=600, label="link nav")
    L.shot("02-article")
    R.append(row("mouse click on a page link navigates", bool(url),
                 "url=%s" % url, attempts=attempts))
    if not url:
        return
    m3 = L.mark()
    c = L.chrome()
    L.q.click(*c["back"])
    L.shot("03-after-back-click")
    url3, failed3 = new_nav(L, m3, timeout=600, label="back")
    L.shot("04-back")
    R.append(row("chrome Back returns to the previous page", bool(url3) and not failed3,
                 "url=%s" % url3,
                 back_receipt=re.findall(r"\[WIN\] back target=(.*)", L.text()[m3:])))

    # Part 2: the same story on a modern site (Wikipedia's Vector-2022 skin puts
    # hidden nav anchors in the census's first 12 slots -- evidence for the
    # census/visibility bug).
    m4, doc4, failed4 = L.navigate("https://en.wikipedia.org/wiki/Cat", timeout=600)
    if failed4:
        return
    cands4 = link_candidates(L, lambda h: "/wiki/" in h)
    hrefs = [c[0] for c in cands4[:6]]
    url4, attempts4, _ = click_link_until_nav(L, cands4, timeout=600, label="wiki link")
    L.shot("05-wiki-link")
    R.append(row("Wikipedia: a census link is clickable at its stated rect", bool(url4),
                 "url=%s candidates=%s" % (url4, hrefs), attempts=attempts4))


# ---------------------------------------------------------------------------
# S2 - Hacker News: open a story, Back, then Forward
# ---------------------------------------------------------------------------
def s2(L, R):
    L.launch_browser()
    m, doc, failed = L.navigate("https://news.ycombinator.com", timeout=600)
    L.shot("01-hn")
    R.append(row("news.ycombinator.com renders", not failed and "Hacker News" in (doc or {}).get("raw", ""),
                 (doc or {}).get("raw", "failed"), nav=nav_receipts(L, m)))
    if failed:
        return
    hit = link_candidates(L, lambda h: "item?id=" in h)
    if not hit:
        hit = link_candidates(L, lambda h: h.startswith("http"), avoid="news.ycombinator.com")
    R.append(row("link census exposes a story link", bool(hit),
                 "%d candidate(s): %s" % (len(hit), [c[0] for c in hit[:3]])))
    if not hit:
        return
    before = (L.doc() or {}).get("title") or ""
    url, attempts, mm = click_link_until_nav(L, hit, timeout=600, label="story nav")
    L.shot("03-story-loaded")
    R.append(row("story link click navigates", bool(url), "url=%s" % url, attempts=attempts))
    if not url:
        return
    c = L.chrome()
    m3 = L.mark()
    L.q.click(*c["back"])
    u_back, f3 = new_nav(L, m3, timeout=600, label="back")
    L.shot("04-back")
    R.append(row("Back returns to the HN front page", bool(u_back) and not f3
                 and u_back.rstrip("/").endswith("news.ycombinator.com"),
                 "url=%s (story was %s)" % (u_back, url)))
    m4 = L.mark()
    L.q.click(*c["fwd"])
    L.shot("05-forward-click")
    u_fwd, f4 = new_nav(L, m4, timeout=600, label="forward")
    L.shot("06-forward")
    R.append(row("Forward returns to the story (not the front page)",
                 bool(u_fwd) and not f4 and u_fwd != u_back,
                 "url=%s vs back=%s vs story=%s" % (u_fwd, u_back, url),
                 receipts=re.findall(r"\[WIN\] (?:forward|back) target=(.*)", L.text()[m4:])))


# ---------------------------------------------------------------------------
# S3 - Scrolling a long article with the wheel and PageDown
# ---------------------------------------------------------------------------
def s3(L, R):
    L.launch_browser()
    m, doc, failed = L.navigate("https://en.wikipedia.org/wiki/Cat", timeout=600)
    L.shot("01-top")
    R.append(row("long article renders", not failed, (doc or {}).get("raw", "failed"),
                 nav=nav_receipts(L, m)))
    if failed:
        return
    x, y, w, h = L.geom()
    cx, cy = x + w // 2, y + CHROME_H + 300
    pos0 = L.scroll_pos()

    m1 = L.mark()
    L.q.wheel(cx, cy, updown=1, ticks=3)
    time.sleep(2.0)
    L.shot("02-wheel-down")
    p1 = L.scroll_pos()
    R.append(row("mouse wheel down scrolls the page", p1 is not None and (pos0 is None or p1 > pos0),
                 "pos %s -> %s" % (pos0, p1),
                 wheel=re.findall(r"\[WIN\] wheel btn=(\d+) dir=(\w+)", L.text()[m1:])[:4],
                 scroll=re.findall(r"\[WIN\] scroll pos=(-?\d+)", L.text()[m1:])[:6]))

    m2 = L.mark()
    L.q.key("pgdn")
    time.sleep(2.0)
    L.shot("03-pagedown")
    p2 = L.scroll_pos()
    R.append(row("PageDown scrolls further", p2 is not None and (p1 is None or p2 > p1),
                 "pos %s -> %s" % (p1, p2),
                 receipts=re.findall(r"\[WIN\] (key=pgdn action=scroll-down|scrollPage dir=-?\d+\S*)",
                                     L.text()[m2:])[:4]))

    m3 = L.mark()
    L.q.wheel(cx, cy, updown=-1, ticks=3)
    time.sleep(2.0)
    L.shot("04-wheel-up")
    p3 = L.scroll_pos()
    R.append(row("mouse wheel up scrolls back", p3 is not None and p2 is not None and p3 < p2,
                 "pos %s -> %s" % (p2, p3)))
    frames = re.findall(r"\[WIN\] frame view=\d+x\d+ nonZero=\d+ checksum=0x([0-9a-f]+)",
                        L.text()[m:])
    R.append(row("each scroll repaints (distinct frame checksums)", len(set(frames)) > 1,
                 "%d distinct checksums" % len(set(frames)), checksums=frames[-6:]))


# ---------------------------------------------------------------------------
# S4 - Tabs: open a second tab, load a second site, switch back
# ---------------------------------------------------------------------------
def s4(L, R):
    L.launch_browser()
    m, doc, failed = L.navigate("https://en.wikipedia.org/wiki/Cat", timeout=600)
    L.shot("01-tab1-cat")
    R.append(row("tab 1 loads wikipedia", not failed, (doc or {}).get("raw", "failed")))
    if failed:
        return
    boxes, newtab = L.tab_boxes(["https://en.wikipedia.org/wiki/Cat"])
    nx, ny = (newtab[0] + newtab[2]) // 2, (newtab[1] + newtab[3]) // 2
    mm = L.mark()
    L.click_content(nx, ny, "new tab")
    L.shot("02-newtab-click")
    try:
        L.wait_for(r"\[WIN\] tab new idx=\d+", timeout=60, since=mm, label="tab new")
        new_ok, detail = True, ""
    except RuntimeError as e:
        new_ok, detail = False, str(e)
    R.append(row("clicking + opens a second tab", new_ok, detail,
                 receipts=re.findall(r"\[WIN\] (tab new .*|ui-click kind=\S+(?: i=-?\d+)?)",
                                     L.text()[mm:])[:6]))
    if not new_ok:
        return
    m2, doc2, failed2 = L.navigate("https://example.com", timeout=600)
    L.shot("03-tab2-example")
    R.append(row("tab 2 navigates to example.com", not failed2,
                 (doc2 or {}).get("raw", "failed")))
    boxes, _ = L.tab_boxes(["https://en.wikipedia.org/wiki/Cat", "https://example.com"])
    b0 = boxes[0]
    m3 = L.mark()
    L.click_content((b0[0] + b0[2]) // 2, (b0[1] + b0[3]) // 2, "tab1")
    time.sleep(2.0)
    L.shot("04-tab1-again")
    sw = re.findall(r"\[WIN\] tab switch idx=(\d+) url=(\S+)", L.text()[m3:])
    R.append(row("clicking tab 1 switches back to the wikipedia page", bool(sw),
                 str(sw), title_now=(L.doc() or {}).get("raw")))
    # close tab 2 (it is now tab index 1)
    boxes2, _ = L.tab_boxes(["https://en.wikipedia.org/wiki/Cat", "https://example.com"])
    b1 = boxes2[1]
    m4 = L.mark()
    L.click_content(b1[2] - 9, (b1[1] + b1[3]) // 2, "close tab2")
    time.sleep(1.5)
    L.shot("05-tab2-closed")
    R.append(row("tab 2's close box closes it", bool(re.findall(r"\[WIN\] tab close", L.text()[m4:])),
                 str(re.findall(r"\[WIN\] tab close .*", L.text()[m4:])[:2])))


# ---------------------------------------------------------------------------
# S5 - Bookmarks + reload: star a page, leave it, come back through the bar
# ---------------------------------------------------------------------------
def s5(L, R):
    L.launch_browser()
    m, doc, failed = L.navigate("https://en.wikipedia.org/wiki/Cat", timeout=600)
    L.shot("01-cat")
    R.append(row("wikipedia renders", not failed, (doc or {}).get("raw", "failed")))
    if failed:
        return
    c = L.chrome()
    m1 = L.mark()
    L.q.click(*c["star"])
    time.sleep(1.5)
    L.shot("02-starred")
    add = re.findall(r"\[WIN\] bookmark add url=(\S+) title=(.*)", L.text()[m1:])
    R.append(row("clicking the star bookmarks the page", bool(add), str(add),
                 receipts=re.findall(r"\[WIN\] (bookmark .*|bookmarks save .*)", L.text()[m1:])[:4]))
    if not add:
        return
    bm_title = add[-1][1].strip()

    m2, doc2, failed2 = L.navigate("https://example.com", timeout=600)
    L.shot("03-example")
    R.append(row("second site loads (example.com)", not failed2, (doc2 or {}).get("raw", "failed")))

    m3 = L.mark()
    L.q.click(*c["reload"])
    time.sleep(1.0)
    u_reload, f3 = new_nav(L, m3, timeout=600, label="reload")
    L.shot("04-reloaded")
    R.append(row("reload re-fetches the current page", bool(u_reload) and not f3,
                 "url=%s" % u_reload,
                 receipts=re.findall(r"\[WIN\] (reload target=.*|net fetch url=\S+)", L.text()[m3:])[:3]))

    # bookmark chip geometry is derived from the title the driver itself printed
    wch = max(60, min(200, 8 * len(bm_title[:39]) + 14 + 18))
    chip = (6 + wch // 2, 60 + 9)
    m4 = L.mark()
    L.click_content(chip[0], chip[1], "bookmark chip")
    time.sleep(1.0)
    L.shot("05-chip-click")
    try:
        u_bm, f4 = new_nav(L, m4, timeout=600, label="bookmark nav")
    except RuntimeError as e:
        u_bm, f4 = None, True
        R.append(row("bookmark nav wait failed", False, str(e)))
    L.shot("06-back-on-cat")
    R.append(row("clicking the bookmark chip returns to the saved page",
                 bool(u_bm) and not f4, "url=%s title=%r" % (u_bm, bm_title),
                 receipts=re.findall(r"\[WIN\] bookmark nav .*", L.text()[m4:])[:2]))
    # WK7-repaint: the blank-window bug — the bookmark/uiclick path painted an
    # all-zero frame over the last real one and held it through the drain.
    # Assert the return actually PAINTS the page again: a non-zero frame and
    # no all-white [IMG1p] colors=1 dom=0x000000 receipt after the nav.
    # The in-tick repaint lands a few ms AFTER load-ok (same engine tick,
    # post-census), so WAIT for a nonZero>0 frame instead of sampling the
    # log the instant new_nav returns on load-ok (which could match the
    # pre-load blank).
    try:
        L.wait_for(r"\[WIN\] frame view=\d+x\d+ nonZero=[1-9][0-9]*", timeout=90,
                   since=m4, label="bookmark repaint frame")
        repainted = True
    except RuntimeError:
        repainted = False
    seg5 = L.text()[m4:]
    frx = [int(v) for v in re.findall(r"\[WIN\] frame view=\d+x\d+ nonZero=(\d+)", seg5)]
    blk = re.findall(r"\[IMG1p\] colors=1 dom=0x0{6}", seg5)
    R.append(row("bookmark return paints a real (non-blank) frame",
                 repainted and not blk,
                 "nonZeroTail=%r blankIMG=%d" % (frx[-4:], len(blk)),
                 receipts=re.findall(r"\[WIN\] (frame-skip-blank .*|frame view=.*)", seg5)[-4:]))


# ---------------------------------------------------------------------------
# S6 - DuckDuckGo html form: click field, type, press Enter (no click on submit)
# ---------------------------------------------------------------------------
def s6(L, R):
    L.launch_browser()
    m, doc, failed = L.navigate("https://html.duckduckgo.com/html/", timeout=600)
    L.shot("01-ddg")
    R.append(row("html.duckduckgo.com renders", not failed, (doc or {}).get("raw", "failed"),
                 nav=nav_receipts(L, m)))
    if failed:
        return
    fields = L.inputs()
    R.append(row("search field is in the census", bool(fields), str(fields[:2])))
    if not fields:
        return
    name, fx, fy, fw, fh = fields[0]
    click_page(L, fx + fw // 2, fy + fh // 2)
    L.shot("02-field-focused")
    mm = L.mark()
    L.q.keys("hobby os")
    time.sleep(1.0)
    L.shot("03-typed")
    typed = "".join(chr(int(k, 16)) for k in
                    re.findall(r"\[WIN\] page-key ch=0x([0-9a-f]{2})", L.text()[mm:]))
    R.append(row("typing reaches the field", typed.strip() == "hobby os", "forwarded=%r" % typed))

    m2 = L.mark()
    L.q.key("ret")
    L.shot("04-enter-pressed")
    try:
        url, failed2 = new_nav(L, m2, timeout=600, label="enter submit")
    except RuntimeError as e:
        url, failed2 = None, True
        R.append(row("waiting for the Enter submit", False, str(e)))
    L.shot("05-results")
    # A real submit must land on a NEW url carrying the query, not re-render
    # the form page it started from.
    submitted = bool(url) and ("q=" in url or "query=" in url) and url != "https://html.duckduckgo.com/html/"
    R.append(row("Enter submits the form and results load", submitted,
                 "url=%s (same page as before: %s)" % (url, url == "https://html.duckduckgo.com/html/"),
                 receipts=re.findall(r"\[WIN\] (engine-nav .*|query nav=.*|query-entry=.*|"
                                     r"query no-navigation.*|nav name=.*)",
                                     L.text()[m2:])[:6],
                 doc=(L.doc() or {}).get("raw")))

    # navigation to a host that cannot exist: what does the user see?
    # WK7-errorpage: the driver now renders a real in-browser error page
    # (about:error substitute doc) with the attempted URL in the omnibox, so
    # navigate() sees load-ok on the error doc and `failed` is False.
    m3, doc3, failed3 = L.navigate("https://nope-hobbyos.invalid/", timeout=300)
    L.shot("06-bad-domain")
    seg3 = L.text()[m3:]
    errpage = re.findall(r"\[WIN\] errpage url=(\S+) status=\d+ bytes=\d+ class=(\S+) err=(.*)", seg3)
    err_title = re.findall(r"\[WIN\] doc title=Can't reach this page[^|]*", seg3)
    err_paint = re.findall(r"\[WIN\] frame view=\d+x\d+ nonZero=(\d+)", seg3)
    good = bool(errpage) and bool(err_title) and (not failed3) \
        and [int(v) for v in err_paint or [0]]
    R.append(row("an unresolvable host shows an in-browser error page", bool(good),
                 "errpage=%s title=%s lastNonZero=%r" % (
                     str(errpage)[:80], bool(err_title), err_paint[-3:]),
                 receipts=re.findall(r"\[WIN\] (errpage .*|load-ok .*)", seg3)[:4],
                 title_now=(L.doc() or {}).get("raw")))


SCENARIOS = {"S0": s0, "S1": s1, "S2": s2, "S3": s3, "S4": s4, "S5": s5, "S6": s6}


def run(ids, lane_prefix, smp=3, mem="5120M"):
    out = {}
    for sid in ids:
        name = "%s-%s" % (lane_prefix, sid.lower()) if lane_prefix else sid.lower()
        L = Lane(name, smp=smp, mem=mem, stage_certs=True)
        rows = []
        rec = {"scenario": sid, "lane": name, "boot_secs": round(L.boot_secs, 1)}
        t0 = time.time()
        try:
            SCENARIOS[sid](L, rows)
        except Exception as e:
            rows.append({"step": "scenario aborted", "ok": False,
                         "detail": "%s: %s" % (type(e).__name__, e)})
            try:
                L.shot("zz-aborted")
            except Exception:
                pass
        rec["secs"] = round(time.time() - t0, 1)
        rec["steps"] = rows
        rec["passed"] = sum(1 for r in rows if r["ok"])
        rec["failed"] = sum(1 for r in rows if not r["ok"])
        rec["lane_dir"] = L.dir
        L.report(os.path.join(L.dir, "report.json"), rec)
        L.quit()
        out[sid] = rec
        print(json.dumps({k: rec[k] for k in ("scenario", "boot_secs", "secs",
                                              "passed", "failed")}), flush=True)
        for r in rows:
            print("  [%s] %s -- %s" % ("PASS" if r["ok"] else "FAIL", r["step"], r["detail"][:160]),
                  flush=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--id", action="append", default=[])
    ap.add_argument("--lane", default="")
    ap.add_argument("--smp", type=int, default=3)
    ap.add_argument("--mem", default="5120M")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        for k, f in SCENARIOS.items():
            doc = (f.__doc__ or "").strip()
            print(k, "-", (doc.splitlines() or ["(no doc)"])[0])
        return 0
    ids = [i.upper() for i in (a.id or list(SCENARIOS))]
    bad = [i for i in ids if i not in SCENARIOS]
    if bad:
        raise SystemExit("unknown scenario id(s): %s" % bad)
    res = run(ids, a.lane, smp=a.smp, mem=a.mem)
    total_f = sum(r["failed"] for r in res.values())
    print("\nTOTAL: %d step(s) failed across %d scenario(s)" % (total_f, len(res)))
    return 1 if total_f else 0


if __name__ == "__main__":
    sys.exit(main())
