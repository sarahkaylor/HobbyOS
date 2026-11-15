#!/usr/bin/env python3
"""tools/cp/p0host/sampler.py <rundir> [interval_s] — 1 Hz /proc + sysfs sampler.

Finds the game process (comm starts with "Cyberpunk2077") and writes
<rundir>/samples.csv:
  t_ns, pid, rss_kb, hwm_kb, threads, fds, utime+stime ticks (game),
  ticks of all processes of this run (wineserver & co, by prefix path in environ is
  too costly -> by comm list), system VRAM used (card2), GTT used, game drm VRAM
  (fdinfo, summed over unique drm-client-id), host total CPU jiffies (busy, total).
Stops when the game has been gone for 10 s after having been seen, or on SIGTERM.
"""
import os, sys, time, signal

R = sys.argv[1]
iv = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
VR = "/sys/class/drm/card2/device/mem_info_vram_used"
GT = "/sys/class/drm/card2/device/mem_info_gtt_used"
WINE_COMMS = ("wineserver", "services.exe", "winedevice.exe", "explorer.exe", "plugplay.exe",
              "svchost.exe", "rpcss.exe", "steam.exe", "tabtip.exe", "conhost.exe", "rundll32.exe",
              "winedevice.ex", "wine64-preload", "wine-preloader", "GameThread", "steamwebhelper.", "start.exe", "xalia.exe")
stop = False
def onterm(*_):
    global stop
    stop = True
signal.signal(signal.SIGTERM, onterm)

def rd(p):
    try:
        with open(p) as f: return f.read()
    except OSError: return ""

def game_pid():
    # Wine sets argv[0] of the game process to its Windows path (Z:\\...\\Cyberpunk2077.exe);
    # the main thread's comm is renamed "GameThread", so comm cannot be used.
    for p in os.listdir("/proc"):
        if not p.isdigit(): continue
        a0 = rd(f"/proc/{p}/cmdline").split("\0", 1)[0]
        if "Cyberpunk2077.exe" in a0 and not a0.startswith("/"):
            return int(p)
    return None

def ticks(pid):
    s = rd(f"/proc/{pid}/stat")
    if not s: return 0
    f = s.rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])

def status(pid):
    d = {}
    for l in rd(f"/proc/{pid}/status").splitlines():
        k, _, v = l.partition(":")
        d[k] = v.strip()
    return d

def drm_vram(pid):
    seen, tot = set(), 0
    try:
        fds = os.listdir(f"/proc/{pid}/fdinfo")
    except OSError:
        return 0
    for fd in fds:
        t = rd(f"/proc/{pid}/fdinfo/{fd}")
        if "drm-client-id" not in t or "0000:65:00.0" not in t: continue
        cid = vram = None
        for l in t.splitlines():
            if l.startswith("drm-client-id:"): cid = l.split()[1]
            elif l.startswith("drm-memory-vram:"): vram = int(l.split()[1])  # KiB
        if cid and cid not in seen and vram is not None:
            seen.add(cid); tot += vram
    return tot

def cpu():
    f = rd("/proc/stat").splitlines()[0].split()[1:]
    v = list(map(int, f)); tot = sum(v[:8]); idle = v[3] + v[4]
    return tot - idle, tot

out = open(os.path.join(R, "samples.csv"), "w")
out.write("t_ns,pid,rss_kb,hwm_kb,threads,fds,game_ticks,wine_ticks,vram_sys_b,gtt_sys_b,game_vram_kib,host_busy,host_total\n")
seen_at = gone_since = None
while not stop:
    t = time.time_ns()
    pid = game_pid()
    wt = 0
    for p in os.listdir("/proc"):
        if p.isdigit():
            c = rd(f"/proc/{p}/comm").strip()
            if c in WINE_COMMS or c.startswith("Cyberpunk2077"):
                wt += ticks(p)
    b, tot = cpu()
    vr = rd(VR).strip() or 0; gt = rd(GT).strip() or 0
    if pid:
        seen_at = seen_at or t; gone_since = None
        st = status(pid)
        try: nfd = len(os.listdir(f"/proc/{pid}/fd"))
        except OSError: nfd = 0
        out.write(f"{t},{pid},{st.get('VmRSS','0 kB').split()[0]},{st.get('VmHWM','0 kB').split()[0]},"
                  f"{st.get('Threads','0')},{nfd},{ticks(pid)},{wt},{vr},{gt},{drm_vram(pid)},{b},{tot}\n")
    else:
        out.write(f"{t},,,,,,,{wt},{vr},{gt},,{b},{tot}\n")
        if seen_at and not gone_since: gone_since = t
        if gone_since and t - gone_since > 10e9: break
    out.flush()
    time.sleep(iv)
