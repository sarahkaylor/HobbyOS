#!/usr/bin/env python3
"""w7_watch.py — poll the T4.3 loop serial every 60 s and append each NEW
[SELFHOST] phase marker + key lines to a state log.  Bounded: exits after
--max-min (default 240).  Never tight-loops (60 s between polls)."""
import argparse
import os
import re
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--max-min", type=int, default=240)
    args = ap.parse_args()

    seen = set()
    # find markers already present so we don't re-log them
    if os.path.exists(args.serial):
        for m in re.findall(r"\[SELFHOST\] [^\r\n]+", open(args.serial, errors="ignore").read()):
            seen.add(m)

    deadline = time.time() + args.max_min * 60
    with open(args.state, "a") as st:
        st.write("[w7watch] start %s (already-seen %d markers)\n"
                 % (time.strftime("%H:%M:%S"), len(seen)))
        st.flush()
        while time.time() < deadline:
            time.sleep(60)
            try:
                txt = open(args.serial, errors="ignore").read()
            except OSError:
                continue
            fresh = []
            for m in re.findall(r"\[SELFHOST\] [^\r\n]+", txt):
                if m not in seen:
                    seen.add(m)
                    fresh.append(m)
            if fresh:
                for m in fresh:
                    st.write("%s %s\n" % (time.strftime("%H:%M:%S"), m))
                st.flush()
    st.write("[w7watch] end %s\n" % time.strftime("%H:%M:%S"))


if __name__ == "__main__":
    sys.exit(main())
