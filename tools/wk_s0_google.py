#!/usr/bin/env python3
"""S0 (the user's original scenario): apps menu -> browser -> google.com via
address bar + GO -> wait for render -> click the page search box with the
mouse -> type "cat pictures" with the keyboard.

Standalone statement of the baseline case, run through tools/wk_lane.py.
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wk_lane import Lane  # noqa: E402

TEXT = "cat pictures"


def main():
    L = Lane("s0-google")
    res = {"scenario": "S0 google.com address bar + page search box", "steps": {}}
    try:
        print("boot: %.1fs" % L.boot_secs)
        res["boot_secs"] = round(L.boot_secs, 1)
        L.shot("00-desktop")
        x, y, w, h = L.launch_browser()
        res["geom"] = [x, y, w, h]
        L.shot("01-browser-home")

        m, d0 = L.navigate("https://www.google.com", timeout=900)
        L.shot("02-google")
        res["doc_home"] = d0
        print("doc:", d0)

        fields = L.inputs()
        res["inputs"] = fields
        if not fields:
            res["steps"]["page_input"] = "FAIL: no <input> in the rendered page"
        else:
            name, fx, fy, fw, fh = fields[0]
            echo = L.click_page(fx + fw // 2, fy + fh // 2)
            res["click_echo"] = echo
            L.shot("03-focused")
            mm = L.mark()
            L.q.keys(TEXT)
            time.sleep(1.5)
            L.shot("04-typed")
            keys = re.findall(r"\[WIN\] page-key ch=0x([0-9a-f]{2})", L.text()[mm:])
            typed = "".join(chr(int(k, 16)) for k in keys)
            res["forwarded_keys"] = typed
            val = re.findall(r"\[WIN\] value .*", L.text()[mm:])
            res["value_probe"] = val[-1:] if val else []
            res["steps"]["page_input"] = "OK" if typed.strip() == TEXT else "FAIL"
        print("result:", res["steps"])
    finally:
        L.report(os.path.join(L.dir, "report.json"), res)
        L.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
