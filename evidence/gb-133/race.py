"""GB-133 race check: does a pte=0 resume of a pid fall between that pid's
[GB133-L] phase=pre and phase=post (the loader's stack prefault)?

race      -> resume with an absent stack top page happened while the loader was
             between creating the stack region and committing its top page.
post-only -> every absent resume came after the prefault, so the prefault ran
             but did not help (different page / different AS).
"""
import re
import sys

L = re.compile(r"\[GB133-L\] pid=(\d+) phase=(pre|post)")
R = re.compile(r"\[GB133-R\] (\w+) pid=(\d+) pg=0x([0-9A-F]+) pte=([01])")

log = sys.argv[1]
lines = open(log, errors="replace").read().splitlines()

in_window = {}     # pid -> True while between pre and post
race = []
post_absent = []
for i, ln in enumerate(lines):
    m = L.search(ln)
    if m:
        pid, ph = m.group(1), m.group(2)
        in_window[pid] = (ph == "pre")
        continue
    m = R.search(ln)
    if not m:
        continue
    pid, pte = m.group(2), m.group(4)
    if pte != "0":
        continue
    if in_window.get(pid):
        race.append((i, pid, ln))
    else:
        post_absent.append((i, pid, ln))

print(f"loader markers: {sum(1 for ln in lines if '[GB133-L]' in ln)}")
print(f"absent resumes INSIDE a pre..post window (race): {len(race)}")
print(f"absent resumes with no open window:              {len(post_absent)}")
for i, pid, ln in race[:5]:
    print(f"\n--- RACE at line {i} pid={pid}")
    for j in range(max(0, i - 3), min(len(lines), i + 2)):
        print("   ", lines[j][:140])