#!/usr/bin/env python3
"""tools/cp/p0host/table.py <evidence/runs dir> <tag>... — markdown tables from collected runs
(summary.txt, fps-menu-mangohud.txt, fps-bench-game.txt, bench-summary.json, meta.txt)."""
import sys, os, json, re

def kv(path):
    d = {}
    if os.path.exists(path):
        for tok in open(path).read().split():
            if "=" in tok:
                k, v = tok.split("=", 1); d[k] = v
    return d

base = sys.argv[1]; tags = sys.argv[2:]
rows = []
for t in tags:
    D = os.path.join(base, t)
    s, m, b = kv(D + "/summary.txt"), kv(D + "/fps-menu-mangohud.txt"), kv(D + "/fps-bench-game.txt")
    res = re.search(r"res=(\d+x\d+)", open(D + "/meta.txt").read()).group(1)
    j = {}
    if os.path.exists(D + "/bench-summary.json"):
        j = json.load(open(D + "/bench-summary.json"))["Data"]
    rows.append((t, res, s, m, b, j))

print("| run | res | menu fps mean / median / p90 / 1%-low (MangoHud, 30 s) | benchmark fps mean / median / p90 / 1%-low (game frames.csv) | game avgFps | host busy % menu / bench |")
print("|---|---|---|---|---|---|")
for t, res, s, m, b, j in rows:
    mm = f"{m['mean_fps']} / {m['median_fps']} / {m['p90_fps']} / {m['low1_fps']}" if m else "—"
    bb = f"{b['mean_fps']} / {b['median_fps']} / {b['p90_fps']} / {b['low1_fps']}" if b else "—"
    print(f"| {t} | {res} | {mm} | {bb} | {j.get('averageFps', 0):.1f} | {s.get('host_busy_pct_menu','—')} / {s.get('host_busy_pct_bench','—')} |")
print()
print("| run | launch→prompt s | launch→menu s | RSS menu / peak MiB | threads menu / peak | fds menu / peak | game VRAM menu / peak MiB | CPU cores menu / bench |")
print("|---|---|---|---|---|---|---|---|")
for t, res, s, m, b, j in rows:
    g = s.get
    print(f"| {t} | {g('launch_to_prompt_s','—')} | {g('launch_to_menu_s','—')} | {g('rss_mib_menu','—')} / {g('rss_peak_mib','—')} | "
          f"{g('threads_menu','—')} / {g('threads_peak','—')} | {g('fds_menu','—')} / {g('fds_peak','—')} | "
          f"{g('game_vram_mib_menu','—')} / {g('game_vram_peak_mib','—')} | {g('cpu_game_cores_menu','—')} / {g('cpu_game_cores_bench','—')} |")
