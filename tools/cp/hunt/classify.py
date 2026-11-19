#!/usr/bin/env python3
"""tools/cp/hunt/classify.py -- crash / fault / glitch signatures of one
HobbyOS game boot (lane H1-hunt, wave 8, docs/cp-hunt.md).

    classify.py [--run ID] [--log RUNLOG] [--host HOSTTXT] [--json OUT.json] SERIAL
    classify.py --aggregate [--min N] A.json B.json ...     (k/n per signature)
    classify.py --mine ROOT...                               (old logs, deduplicated)

SERIAL is a boot's serial log (serial.log or serial.txt; CRs are dropped).
RUNLOG is the run-p8.sh / run-p9.sh output (verdict, timeline, menu time).
HOSTTXT is run-hunt.sh's host sampler (9070 XT busy %, loadavg, guests).

Every line is put in at most one class, each with a normalised signature
(hex -> 0x#, numbers -> N, pids/tids dropped), so the same fault in two
boots has the same key:

  kfault    kernel fault / panic / kill rows ([KERNEL] User process fault,
            FATAL, #PF in user space, [LCOMPAT] fault*, [KPANIC], [TORNFRAME],
            [USERFAULT], [BADVEC], [TLBTIMEOUT], [LOCKERR], signal-delivery
            fallbacks, rt_sigreturn bad rip)
  kstall    stall / lost-wake / timeout rows ([VENUS] stall|fence error|
            timeout, [NFSPIPE] stall, [BLKQ] stall|timeout|wedge, [STALLBREAK],
            [LOSTWAKE], [LOSTW], [EPOLLSPIN], exit_group drain timeout, [NTSYNC] LOSTWAKE)
  exit      "[KERNEL] Process N: NAME exited ..." (by NAME) and [EXITPC]
            rows with a non-zero code (NTSTATUS name when known)
  wine_err  Wine "tid:err:channel:func ..." lines, tagged benign/known/new
  dxvk      DXVK / vkd3d-proton "err:" and "warn:" lines (DXVK's "err:   "
            format and vkd3d-proton's "err:vkd3d..." channel)
  red       REDEngineErrorReporter: loaded / its process exits

Plus the boot's verdict, launch->menu time, the fps rows' gaps (if fps.csv is
beside SERIAL), per-process anon MiB growth from [THRS] (MiB/min, first ->
last), and the host's GPU busy %.  Pure stdlib.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import statistics
import sys

# --- normalisation -------------------------------------------------------------
TS_RX = re.compile(r"^\d+\.\d+:")                     # +timestamp
TID_RX = re.compile(r"^(?:[0-9a-f]{4}:){1,2}")         # Wine tid (or pid:tid with +pid)
HEX_RX = re.compile(r"0x(?:0x)?[0-9a-fA-F]+")
LONGHEX_RX = re.compile(r"\b[0-9a-fA-F]{8,16}\b")
NUM_RX = re.compile(r"(?<![A-Za-z0-9_.])\d+(?:\.\d+)?(?![A-Za-z0-9_])")


def norm(s, keep_hex=False):
    s = s.strip()
    if not keep_hex:
        s = HEX_RX.sub("0x#", s)
        s = LONGHEX_RX.sub("#", s)
    s = NUM_RX.sub("N", s)
    s = re.sub(r"\s+", " ", s)
    return s[:150]


INNER_TAG_RX = re.compile(r"\[[A-Z][A-Z0-9_-]{1,14}\]")
KEEP_RX = re.compile(r"\b(sig=|Vector: |vector=|code=)(0x[0-9A-Fa-f]+|\d+)")


def ksig(tag, line, m):
    """a kernel row's signature: from where the pattern matched, cut at the next
    row tag (old boots' serial consoles interleaved rows from two CPUs), the
    first 6 normalised tokens, with signal / vector / code numbers kept."""
    s = line[m.start():]
    t = INNER_TAG_RX.search(s, 1)
    if t:
        s = s[:t.start()]
    n = norm(s)
    for k, v in KEEP_RX.findall(s):
        for ph in ("N", "0x#"):
            if k + ph in n:
                n = n.replace(k + ph, k + v, 1)
                break
    return f"{tag}: {' '.join(n.split()[:6])}"


def strip_x(line):
    """Drop the session's "[X] " prefix, a Wine +timestamp and the tid."""
    if line.startswith("[X] "):
        line = line[4:]
    line = TS_RX.sub("", line.lstrip())
    line = TID_RX.sub("", line)
    return line


# --- kernel rows ---------------------------------------------------------------
KFAULT = [
    ("user-fault", re.compile(r"\[KERNEL\] User process fault! Vector:.*")),
    ("kernel-fatal", re.compile(r"\[KERNEL\] FATAL.*")),
    ("kpf-user", re.compile(r"\[KERNEL\] (Kernel-mode #PF|EL1 fault) in user address space.*")),
    ("fault-pc", re.compile(r"\[KERNEL\] fault-pc region=.*")),
    ("sigdeliv", re.compile(r"\[KERNEL\] signal delivery: .*")),
    ("fork-fail", re.compile(r"\[KERNEL\] fork: AS clone failed.*")),
    ("lcompat-fault", re.compile(r"\[LCOMPAT\] (fault[a-z-]*|rt_sigreturn: bad|signal: no usable).*")),
    ("kpanic", re.compile(r"\[(KPANIC|PANIC)\].*|.*\bkernel panic\b.*", re.I)),
    ("tornframe", re.compile(r"\[TORNFRAME\].*")),
    ("userfault", re.compile(r"\[USERFAULT\].*")),
    ("badvec", re.compile(r"\[BADVEC\].*")),
    ("tlbtimeout", re.compile(r"\[TLBTIMEOUT\].*")),
    ("lockerr", re.compile(r"\[LOCKERR\].*")),
    ("iommu", re.compile(r"\[IOMMU\] ERROR.*")),
    ("oom", re.compile(r".*\b(out of memory|frame allocation failed|OOM)\b.*", re.I)),
]
KSTALL = [
    ("venus", re.compile(r"\[VENUS\] (stall|fence error|fence_wait2 timeout|posted submit error|submit2 host error|"
                         r"submit2: no fence slot|blob_create: cmd submit failed).*")),
    ("nfspipe", re.compile(r"\[NFSPIPE\] stall.*")),
    ("blkq", re.compile(r"\[BLKQ\] (stall|admin timeout|wedge|reinit failed|reset).*")),
    ("stallbreak", re.compile(r"\[STALLBREAK\].*")),
    ("lostwake", re.compile(r"\[(LOSTWAKE|LOSTW)\].*|\[NTSYNC\] LOSTWAKE.*")),
    ("epollspin", re.compile(r"\[EPOLLSPIN\].*")),
    ("eg-drain", re.compile(r"\[(THREADS|P5)\] .*claim drain timeout.*")),
    ("nfs-timeout", re.compile(r"\[NET\] Error: .*timeout.*|\[NFS\] .*(fail|timeout).*", re.I)),
]
# kernel diagnostics that are instruments, not faults (each one checked by hand)
KDIAG = [
    # process.c's scheduler pick check flags elr in [0x70000000, USER_VA_BASE): a
    # Windows PE image (the game's is at 0x140000000) lives exactly there, so in a
    # compat process this is a false positive that burns the 20-row cap.
    ("badelr", re.compile(r"\[BADELR\] .*")),
    ("stallbrk-hold", re.compile(r"\[(HOLDER|LOCKHOLD|SPINHOLD)\].*")),
    # trap.c's stall watchdog: "console silent for more than 20s" -- in the
    # game boots it fires ~1 s after console output (cp-hunt H1-14)
    ("watchdog", re.compile(r"\[WATCHDOG\] console silent.*")),
    # integ17 (cp-kglitch): the stall-proof console's takeover row
    ("cons-takeover", re.compile(r"\[CONS\] takeover.*")),
]
PROC_RX = re.compile(r"\[KERNEL\] Process (\d+): (.*?) exited unexpectedly or gracefully")
EXITPC_RX = re.compile(r"\[EXITPC\] pid=(\d+) last_user_pc=(\S+) code=(0x[0-9A-Fa-f]+)")
NTSTATUS = {
    0xC0000005: "ACCESS_VIOLATION", 0xC000001D: "ILLEGAL_INSTRUCTION", 0xC0000135: "DLL_NOT_FOUND",
    0xC0000139: "ENTRYPOINT_NOT_FOUND", 0xC0000142: "DLL_INIT_FAILED", 0xC00000FD: "STACK_OVERFLOW",
    0xC0000409: "STACK_BUFFER_OVERRUN", 0xC0000374: "HEAP_CORRUPTION", 0x80000003: "BREAKPOINT",
    0xC000013A: "CONTROL_C_EXIT", 0xC0000094: "INT_DIVIDE_BY_ZERO", 0xC0000096: "PRIV_INSTRUCTION",
    0xC0000420: "ASSERTION_FAILURE", 0xC0000417: "INVALID_CRUNTIME_PARAMETER", 0x40010004: "DBG_TERMINATE_PROCESS",
    0xC0000017: "NO_MEMORY", 0xC000009A: "INSUFFICIENT_RESOURCES", 0xC0000008: "INVALID_HANDLE",
    0x1: "exit(1)", 0x3: "exit(3)",
}

# --- Wine / DXVK / vkd3d -------------------------------------------------------------
WINE_ERR_RX = re.compile(r"^(err|warn|fixme):([A-Za-z0-9_]+):([A-Za-z0-9_]+) (.*)")
DXVK_RX = re.compile(r"^(err|warn):  +(.*)")            # DXVK / vkd3d-proton logger
VKD3D_RX = re.compile(r"^(err|warn|fixme):(vkd3d[a-z_-]*|d3d12|dxgi):([A-Za-z0-9_]+):? (.*)")
# Wine err lines that are expected in this harness, with the reason.  A class
# "new" line is one no table here knows: those are the ones to read.
BENIGN = [
    (r"winediag:getaddrinfo Failed to resolve your host name", "no DNS name for the guest"),
    (r"wineboot:process_run_key .*winemenubuilder", "winemenubuilder disabled in the prefix"),
    (r"ntoskrnl:ZwLoadDriver .*winebth", "no Bluetooth driver"),
    (r"secur32:SECUR32_initSchannelSP no schannel", "no GnuTLS in the stage"),
    (r"kerberos:kerberos_LsaApInitializePackage", "no Kerberos in the stage"),
    (r"module:import_dll Library mfc\d+u?\.dll .*CChromaEditor", "Razer Chroma plugin needs MFC; game ignores it"),
    (r"mmdevapi:init_driver No driver from", "no sound device in this boot (HUNT_AUDIO=0)"),
    (r"readMonitorEdidFromKey", "DXVK: no EDID for the virtual monitor"),
    (r"DXGI: Failed to parse display metadata", "DXVK: no EDID / HDR metadata"),
    (r"DxgiAdapter::QueryInterface: Unknown interface query|^warn: #-fe5a-42a2-bd62-#",
     "DXVK: the game asks the adapter for an interface DXVK lacks (E_NOINTERFACE); every boot, menus fine"),
    (r"CreateDXGIFactory2: Ignoring flags|MakeWindowAssociation: Ignoring flags", "DXVK: flags it does not implement"),
    (r"winediag:.*ntsync", "ntsync availability note"),
    (r"wineusb|hidclass|plugplay", "no USB / HID devices"),
    (r"wine: failed to start .*syswow64", "no 32-bit Wine: the prefix update's Wow64Install step (stale prefix stamp, cp-hunt H1-04)"),
]
BENIGN_RX = [(re.compile(p), why) for p, why in BENIGN]

RED_LOAD_RX = re.compile(r"Loaded L\"[^\"]*REDEngineErrorReporter\.exe\"")
FPS_RX = re.compile(r"trace:fps:\S+ \S+ @ approx ([0-9.]+)fps")
THRS_RX = re.compile(r"\[THRS\] pid=(\d+) live=(\d+) .*?anon_mib=(\d+) .*?res_mib=(\d+) .*? (\S+)\s*$")


# --- round 3 (H6-hunt3): the session record: launcher census, exit, long-run trends ---
CENSUS_RX = re.compile(r"\[P8\] census (\S+) t=(\d+)s procs:(.*?)\(n=(\d+)\)")
P8FD_RX = re.compile(r"\[P8FD\] cap=(\d+) opened=(\d+) first_errno=(\d+)")
P8MEM_RX2 = re.compile(r"\[P8MEM\] frames_free=(\d+) free_mib=(\d+)")
P8END_RX = re.compile(r"\[P8\] end (\S+) status=(\S+?)(?: ms=(\d+))?\s*$")
P8TIMEOUT_RX = re.compile(r"\[P8\] timeout (\S+) after")
QUIESCE_RX = re.compile(r"\[P8\] quiesce after (\S+): extra=(\d+)(?: left:(.*))?")
THRS_FULL_RX = re.compile(r"\[THRS\] pid=(\d+) live=(\d+) gt=(\d+) hw=(\d+) slots=(\d+)/(\d+) regs=(\d+) anon=(\d+) "
                          r"anon_mib=(\d+) .*?res_mib=(\d+) .*? (\S+)\s*$")
THRS_CAP = 4000          # process.c rw_thr_sample(): `rows < 4000` for the whole boot
# + vkd3d-proton's "Failed to submit queue(s), vr -4" (VK_ERROR_DEVICE_LOST = -4) and the
# kernel's own give-up row before it (cp-hunt H6-07: h3-c06)
DEVLOST_RX = re.compile(r"DEVICE_LOST|device[ _-]lost|DEVICE_REMOVED|DEVICE_HUNG|0x887a000[5-7]|\bvr -4\b|"
                        r"submit2: no fence slot", re.I)
EMFILE_RX = re.compile(r"Too many open files|\bEMFILE\b|grab_object|ENFILE", re.I)
AWAY_RX = re.compile(r"^\[AWAY(-Z)?\] ")
SNDZ_RX = re.compile(r"^\[SND\] zeros")


def classify_session(text):
    """the launcher's 30 s census ([P8] census + [P8FD] + [P8MEM]), the game's
    end and the teardown after it (wineserver -k, quiesce), [THRS] trends per
    process name, device-lost / EMFILE rows and the [AWAY] / [SND] zeros counts"""
    s = {"census": [], "ends": {}, "timeouts": [], "quiesce": [], "thrs_rows": 0, "thrs_last_line": None,
         "thrs": {}, "devlost": [], "emfile": [], "away": 0, "away_z": 0, "snd_zeros": 0,
         "done": False, "sync": False}
    cur = None
    for ln, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip()
        if not line:
            continue
        m = CENSUS_RX.search(line)
        if m:
            cur = {"tag": m.group(1), "t": int(m.group(2)), "n": int(m.group(4)),
                   "procs": [p.split("/")[0] for p in m.group(3).split()], "line": ln}
            s["census"].append(cur)
            continue
        m = P8FD_RX.search(line)
        if m and cur is not None and "fd_cap" not in cur:
            cur["fd_cap"], cur["fd_opened"], cur["fd_errno"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
            continue
        m = P8MEM_RX2.search(line)
        if m and cur is not None and "free_mib" not in cur:
            cur["free_mib"] = int(m.group(2))
            continue
        m = P8END_RX.search(line)
        if m:
            s["ends"][m.group(1)] = {"status": m.group(2), "ms": int(m.group(3)) if m.group(3) else None, "line": ln}
            continue
        m = P8TIMEOUT_RX.search(line)
        if m:
            s["timeouts"].append((m.group(1), ln))
            continue
        m = QUIESCE_RX.search(line)
        if m:
            s["quiesce"].append({"after": m.group(1), "extra": int(m.group(2)),
                                 "left": (m.group(3) or "").split(), "line": ln})
            continue
        if "[P8] done" in line:
            s["done"] = True
        if "[P8] sync done" in line:
            s["sync"] = True
        m = THRS_FULL_RX.search(line)
        if m:
            s["thrs_rows"] += 1
            s["thrs_last_line"] = ln
            e = s["thrs"].setdefault(m.group(11), [])
            e.append({"pid": int(m.group(1)), "live": int(m.group(2)), "gt": int(m.group(3)), "slots": int(m.group(5)),
                      "regs": int(m.group(7)), "anon_mib": int(m.group(9)), "res_mib": int(m.group(10)), "line": ln})
            continue
        if AWAY_RX.match(line):
            s["away_z" if line.startswith("[AWAY-Z]") else "away"] += 1
            continue
        if SNDZ_RX.match(line):
            s["snd_zeros"] += 1
            continue
        x = strip_x(line)
        if DEVLOST_RX.search(x) and len(s["devlost"]) < 20:
            s["devlost"].append((ln, x[:160]))
        elif EMFILE_RX.search(x) and len(s["emfile"]) < 20:
            s["emfile"].append((ln, x[:160]))
    return s


def thrs_trend(rows):
    """first / quarter points / last of a process's [THRS] series (one row per 5 s
    while the group lives; the pid can change, e.g. a relaunched wineserver)"""
    if not rows:
        return None
    pts = [rows[0], rows[len(rows) // 4], rows[len(rows) // 2], rows[3 * len(rows) // 4], rows[-1]]
    return {"rows": len(rows), "pids": sorted({r["pid"] for r in rows}),
            "threads": [r["gt"] for r in pts], "regs": [r["regs"] for r in pts],
            "anon_mib": [r["anon_mib"] for r in pts], "res_mib": [r["res_mib"] for r in pts],
            "threads_max": max(r["gt"] for r in rows), "regs_max": max(r["regs"] for r in rows),
            "slots_max": max(r["slots"] for r in rows)}


def runlog_session(path):
    """load / world counts from the boot_p9 timeline (expect rows)"""
    r = {"loaded": 0, "inworld": 0, "expect_failed": [], "seek_failed": [], "expect_ok": {}}
    if not path or not os.path.exists(path):
        return r
    for ln in open(path, errors="replace"):
        if not ln.startswith("[P9W]"):        # the run log reprints the timeline at the end
            continue
        m = re.search(r"expect (\S+): matched", ln)
        if m:
            tag = m.group(1)
            r["expect_ok"][tag] = r["expect_ok"].get(tag, 0) + 1
            if tag.startswith("loaded"):
                r["loaded"] += 1
            if tag.startswith("inworld"):
                r["inworld"] += 1
        m = re.search(r"(EXPECT FAILED \S+|SEEK FAILED \S+|LISTSEEK FAILED \S+)", ln)
        if m:
            (r["expect_failed"] if m.group(1).startswith("EXPECT") else r["seek_failed"]).append(m.group(1))
    return r


def zombies(q):
    """quiesce's leftovers in state 4 (PROC_STATE_EXITED: gone, not yet reaped).
    p8launch never waits for its keepalive (`vkcube.exe --idle`, keep_pid), so
    every game boot ends with `vkcube.exe/N:s4` there (cp-hunt H6-03)"""
    return [p for p in q["left"] if p.endswith(":s4")]


def left_live(q):
    return [p for p in q["left"] if not p.endswith(":s4")]


def print_session(ses, rs):
    """printed only for boots that have these rows (the golden mini serial has none)"""
    c = ses["census"]
    if c:
        fd = [x for x in c if "fd_opened" in x]
        low = [x for x in fd if x["fd_opened"] < x["fd_cap"]]
        mem = [x["free_mib"] for x in c if "free_mib" in x]
        print(f"-- launcher census: {len(c)} rows (t {c[0]['t']}..{c[-1]['t']} s), procs n {c[0]['n']} -> "
              f"max {max(x['n'] for x in c)} -> {c[-1]['n']}; fd probe below its cap in {len(low)}/{len(fd)}"
              + (f" (first t={low[0]['t']} opened={low[0]['fd_opened']}/{low[0]['fd_cap']} errno={low[0]['fd_errno']})" if low else "")
              + (f"; free MiB {mem[0]} -> min {min(mem)} -> {mem[-1]}" if mem else ""))
        ag = [x for x in c if x["tag"] == "after-game"]
        if ag:
            print(f"   after the game: {ag[-1]['n']} procs: {' '.join(ag[-1]['procs'])[:240]}")
    e = ses["ends"]
    if e:
        print("-- launcher steps ended: " + ", ".join(f"{k} status={v['status']}" + (f" ms={v['ms']}" if v["ms"] is not None else "")
                                                for k, v in e.items()))
    for q in ses["quiesce"]:
        print(f"   quiesce after {q['after']}: extra={q['extra']}" + (f" left: {' '.join(q['left'])[:200]}" if q["left"] else ""))
    for t, ln in ses["timeouts"]:
        print(f"   TIMEOUT {t} (L{ln})")
    if e or ses["quiesce"]:
        g = e.get("game")
        k = e.get("kill")
        q = [x for x in ses["quiesce"] if x["after"] == "kill"]
        live = left_live(q[-1]) if q else None
        clean = bool(g and g["status"] == "0" and k and k["status"] == "0" and q and not live and ses["done"])
        print(f"   clean exit: {'yes' if clean else 'NO'} (game status={g['status'] if g else '-'}, wineserver -k "
              f"{(str(k['ms']) + ' ms') if k and k['ms'] is not None else '-'}, left after -k: "
              f"{(' '.join(live) or 'none') if q else '-'}{' + exited-unreaped ' + ' '.join(zombies(q[-1])) if q and zombies(q[-1]) else ''}, "
              f"sync {'yes' if ses['sync'] else 'no'}, [P8] done {'yes' if ses['done'] else 'no'})")
    if ses["thrs_rows"] >= 10:
        cap = ses["thrs_rows"] >= THRS_CAP
        print(f"-- [THRS] trend: {ses['thrs_rows']} rows{' = the kernel cap (' + str(THRS_CAP) + '): the census STOPPED at serial L' + str(ses['thrs_last_line']) if cap else ''}"
              "; per name: rows, threads / regions / anon MiB / res MiB at 0, 25, 50, 75, 100 %")
        for n, rows in sorted(ses["thrs"].items(), key=lambda kv: -len(kv[1])):
            t = thrs_trend(rows)
            if t["rows"] < 3:
                continue
            print(f"   {n:<18} {t['rows']:5d}  thr {t['threads']}  regs {t['regs']}  anon {t['anon_mib']}  res {t['res_mib']}"
                  f"  (max thr {t['threads_max']}, regs {t['regs_max']}, slots {t['slots_max']})")
    if ses["away"] or ses["away_z"] or ses["snd_zeros"]:
        print(f"-- [AWAY] rows {ses['away']}, [AWAY-Z] rows {ses['away_z']}, [SND] zeros rows {ses['snd_zeros']}")
    if ses["devlost"]:
        print(f"-- DEVICE LOST rows: {len(ses['devlost'])}")
        for ln, x in ses["devlost"][:5]:
            print(f"   L{ln} {x}")
    if ses["emfile"]:
        print(f"-- EMFILE / grab_object rows: {len(ses['emfile'])}")
        for ln, x in ses["emfile"][:5]:
            print(f"   L{ln} {x}")
    if rs and (rs["loaded"] or rs["expect_failed"] or rs["seek_failed"]):
        print(f"-- script: loads reached (expect loaded*) {rs['loaded']}, in world (expect inworld*) {rs['inworld']}"
              + (f"; FAILED: {' '.join(rs['expect_failed'] + rs['seek_failed'])}" if rs["expect_failed"] or rs["seek_failed"] else ""))


def benign(sig):
    for rx, why in BENIGN_RX:
        if rx.search(sig):
            return why
    return None


def classify_serial(text):
    """-> dict of class -> {signature: [count, first_line_no, example]} plus extras."""
    cls = {k: {} for k in ("kfault", "kstall", "kdiag", "exit", "exitpc", "audio", "wine_err", "dxvk", "red")}
    pidname = {}
    nfps = 0
    fps_vals = []
    fps_ts = []                         # guest time of each fps row (WINEDEBUG +timestamp)
    thrs = {}

    def add(c, sig, ln, ex):
        e = cls[c].setdefault(sig, [0, ln, ex[:200]])
        e[0] += 1

    for ln, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip()
        if not line:
            continue
        m = PROC_RX.search(line)
        if m:
            pidname[m.group(1)] = m.group(2)
            add("exit", m.group(2), ln, line)
            if "REDEngineErr" in m.group(2):
                add("red", "REDEngineErrorReporter exited", ln, line)
            continue
        m = EXITPC_RX.search(line)
        if m:
            code = int(m.group(3), 16)
            if code:
                name = pidname.get(m.group(1), "?")
                add("exitpc", f"{name} code={m.group(3)} {NTSTATUS.get(code, '')}".strip(), ln, line)
            continue
        hit = False
        for tag, rx in KFAULT:
            m = rx.search(line)
            if m:
                add("kfault", ksig(tag, line, m), ln, line)
                hit = True
                break
        if hit:
            continue
        for tag, rx in KSTALL:
            m = rx.search(line)
            if m:
                add("kstall", ksig(tag, line, m), ln, line)
                hit = True
                break
        if hit:
            continue
        for tag, rx in KDIAG:
            if rx.search(line):
                k = norm(line)
                if tag == "badelr":   # keep the image (elr >> 28) and the process name
                    m2 = re.search(r"elr=0x([0-9A-Fa-f]+).* (\S+)$", line)
                    k = f"elr~0x{int(m2.group(1), 16) >> 28:x}0000000 in {m2.group(2)}" if m2 else k
                add("kdiag", f"{tag}: {k}", ln, line)
                hit = True
                break
        if hit:
            continue
        if RED_LOAD_RX.search(line):
            add("red", "REDEngineErrorReporter.exe loaded", ln, line)
            continue
        m = THRS_RX.search(line)
        if m:
            thrs.setdefault(m.group(5), []).append((ln, int(m.group(3)), int(m.group(2)), int(m.group(4))))
            continue
        x = strip_x(line)
        m = FPS_RX.search(x)
        if m:
            nfps += 1
            fps_vals.append(float(m.group(1)))
            mt = re.match(r"\[X\]\s+(\d+\.\d+):", line)
            fps_ts.append(float(mt.group(1)) if mt else None)
            continue
        m = VKD3D_RX.match(x)
        if m and m.group(1) in ("err", "warn"):
            add("dxvk", f"{m.group(1)}:{m.group(2)}:{m.group(3)} {norm(m.group(4))}", ln, line)
            continue
        if line.startswith("[SND] xrun"):              # integ17 (cp-kglitch H1-11): the kernel's own row
            add("audio", "kernel [SND] xrun row", ln, line)
            continue
        if "snd_pcm_recover) underrun" in x:          # alsa-lib in the game process
            add("audio", "alsa-lib snd_pcm_recover: underrun occurred", ln, line)
            continue
        m = re.match(r"wine: failed to start (.*)", x)
        if m:
            add("wine_err", "wine: failed to start " + norm(m.group(1)), ln, line)
            continue
        m = WINE_ERR_RX.match(x)
        if m and m.group(1) == "err":
            sig = f"{m.group(2)}:{m.group(3)} {norm(m.group(4))}"
            add("wine_err", sig, ln, line)
            continue
        m = DXVK_RX.match(x)
        if m:
            add("dxvk", f"{m.group(1)}: {norm(m.group(2))}", ln, line)
            continue
    return cls, {"fps_rows": nfps, "fps": fps_vals, "fps_ts": fps_ts, "thrs": thrs}


# --- run log / host sampler ------------------------------------------------------------
def parse_runlog(path):
    r = {}
    if not path or not os.path.exists(path):
        return r
    t = open(path, errors="replace").read()
    m = re.findall(r"== verdict: (\S+)", t) or re.findall(r"verdict: ([^;\n]+);", t)
    if m:
        r["verdict"] = m[-1]
    m = re.search(r"\+\s+([0-9.]+) s\s+\S+\s+game launched", t)
    if m:
        r["launch_s"] = float(m.group(1))
    m = re.search(r"\+\s+([0-9.]+) s\s+\S+\s+main menu OCR", t)
    if m:
        r["menu_s"] = float(m.group(1))
        if "launch_s" in r:
            r["launch_to_menu_s"] = round(r["menu_s"] - r["launch_s"], 1)
    m = re.search(r"game process ended: (.*)", t)
    if m:
        r["game_end"] = m.group(1).strip()[:120]
        m2 = re.search(r"end game status=(-?\d+) ms=(\d+)", m.group(1))
        if m2:
            r["game_status"], r["game_ms"] = int(m2.group(1)), int(m2.group(2))
            # the H2-early signature: the game is gone seconds after launch
            # (the grader still calls this rc=0 "launcher-done")
            r["early_exit"] = int(m2.group(2)) < 60000 and "main menu OCR" not in t
    phases = re.findall(r"\[P9W\] \+\s+([0-9.]+) s\s+(\d\d:\d\d:\d\d)\s+phase (\S+)", t)
    if phases:
        r["phases"] = [(float(a), b, c) for a, b, c in phases]
    m = re.search(r"^rc=(\d+)", t, re.M)
    if m:
        r["rc"] = int(m.group(1))
    fails = re.findall(r"\[P9W\].*\b(FAIL[^\n]{0,120})", t)
    if fails:
        r["script_fail"] = fails[-1]
    return r


def hms(s):
    h, m, x = (int(v) for v in s.split(":"))
    return h * 3600 + m * 60 + x


def parse_host(path, phases=None):
    if not path or not os.path.exists(path):
        return {}
    busy, world, guests, ser = [], [], set(), []
    spans = []
    if phases:
        for i, (_, t, name) in enumerate(phases):
            end = phases[i + 1][1] if i + 1 < len(phases) else None
            if name == "world" or name.startswith("soak"):
                spans.append((hms(t), hms(end) if end else 10 ** 9))
    for ln in open(path, errors="replace"):
        p = ln.split()
        if p[:1] == ["S"] and len(p) >= 5 and p[4].isdigit():
            ser.append((hms(p[1]), int(p[4])))
        if p[:1] == ["S"] and len(p) >= 3 and p[2].isdigit():
            b = int(p[2])
            busy.append(b)
            if any(a <= hms(p[1]) < z for a, z in spans):
                world.append(b)
        elif p[:1] == ["G"] and len(p) >= 4:
            guests.add(p[3])

    def mq(v):
        if not v:
            return None
        v = sorted(v)
        return {"median": v[len(v) // 2], "p90": v[min(len(v) - 1, int(len(v) * 0.9))], "n": len(v)}
    # the longest stretch with no serial output once the guest has printed
    silent, since, size = 0, None, None
    for t, n in ser:
        if n != size:
            since, size = t, n
        elif n > 4096:              # past the firmware's ~113 bytes of screen codes
            silent = max(silent, t - since)
    return {"gpu_busy_all": mq(busy), "gpu_busy_world": mq(world), "guests": sorted(guests),
            "serial_silent_max_s": silent if ser else None}


def q10(v):
    """summarize.py's interpolated 10th percentile, so the tables agree"""
    v = sorted(v)
    k = (len(v) - 1) * 0.1
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def fps_gaps(serial_path, fps_ts=None, fps_vals=None):
    """Per phase of fps.csv: rows, median, p10 and -- from the guest timestamps
    of the same rows in the serial log (winevulkan prints one every ~1.5 s of
    guest time) -- the longest interval between rows and how many exceed 2.5 s
    (a frame that took >= 1 s: a hitch).  fps.csv's own game_s is the host's
    read time, which follows boot_p9's screendump cadence, not the game."""
    if serial_path.endswith(".serial.txt"):            # run-hunt.sh's copy: RUN.serial.txt + RUN.fps.csv
        p = serial_path[:-len(".serial.txt")] + ".fps.csv"
    else:
        p = os.path.join(os.path.dirname(serial_path), "fps.csv")
    if not os.path.exists(p):
        return None
    rows = list(csv.DictReader(open(p)))
    ts = None
    if fps_ts and fps_vals and len(fps_vals) >= len(rows):
        # boot_p9 may miss a row at either end: align on the fps values
        for k in range(0, len(fps_vals) - len(rows) + 1):
            if all(abs(float(rows[i]["approx_fps"]) - fps_vals[i + k]) < 0.06 for i in range(len(rows))):
                ts = fps_ts[k:k + len(rows)]
                break
    out = {}
    for ph in dict.fromkeys(r["phase"] for r in rows):
        idx = [i for i, r in enumerate(rows) if r["phase"] == ph]
        v = [float(rows[i]["approx_fps"]) for i in idx]
        if len(idx) < 2:
            continue
        e = {"rows": len(idx), "median": round(statistics.median(v), 1), "p10": round(q10(v), 1), "min": min(v)}
        if ts and all(ts[i] is not None for i in idx):
            g = [ts[b] - ts[a] for a, b in zip(idx, idx[1:]) if b == a + 1]
            if g:
                e["max_gap_s"] = round(max(g), 2)
                e["hitches"] = sum(1 for x in g if x > 2.5)
        out[ph] = e
    return out


def mem_growth(thrs):
    out = {}
    for name, rows in thrs.items():
        if len(rows) < 3:
            continue
        a = [r[1] for r in rows]
        res = [r[3] for r in rows]
        out[name] = {"rows": len(rows), "first": a[0], "max": max(a), "last": a[-1],
                     "res_first": res[0], "res_max": max(res), "res_last": res[-1],
                     "threads_max": max(r[2] for r in rows)}
    return out


def report(args):
    raw = open(args.serial, "rb").read().decode("utf-8", "replace").replace("\r", "")
    cls, ex = classify_serial(raw)
    rl = parse_runlog(args.log)
    host = parse_host(args.host, rl.get("phases"))
    gaps = fps_gaps(args.serial, ex.get("fps_ts"), ex.get("fps"))
    mem = mem_growth(ex["thrs"])
    rid = args.run or os.path.basename(os.path.dirname(os.path.abspath(args.serial)))
    ses = classify_session(raw)
    rs = runlog_session(args.log)
    res = {"run": rid, "session": {**{k: v for k, v in ses.items() if k != "thrs"},
                                   "thrs": {n: thrs_trend(r) for n, r in ses["thrs"].items()}},
           "script": rs, "serial": args.serial, "serial_lines": raw.count("\n"),
           "sha": hashlib.sha256(raw.encode()).hexdigest()[:16], "runlog": rl, "host": host,
           "fps_gaps": gaps, "mem": mem, "fps_rows": ex["fps_rows"],
           "classes": {c: {s: v for s, v in d.items()} for c, d in cls.items()}}
    for sig, v in res["classes"]["wine_err"].items():
        why = benign(sig)
        v.append(f"benign: {why}" if why else "new")
    for sig, v in res["classes"]["dxvk"].items():
        why = benign(sig)
        v.append(f"benign: {why}" if why else "new")
    if args.json:
        json.dump(res, open(args.json, "w"), indent=1)
    print(f"== classify {rid}: {args.serial} ({res['serial_lines']} lines, sha {res['sha']})")
    for k in ("verdict", "rc", "launch_s", "menu_s", "launch_to_menu_s", "game_end", "early_exit", "script_fail"):
        if k in rl:
            print(f"   {k}: {rl[k]}")
    print(f"   REDEngineErrorReporter: {'started' if res['classes']['red'] else 'not started'}"
          f"{' (exited)' if 'REDEngineErrorReporter exited' in res['classes']['red'] else ''}")
    for c, title in (("kfault", "kernel faults"), ("kstall", "kernel stalls / lost wakes / timeouts"),
                     ("kdiag", "kernel diagnostics (instrument rows)"),
                     ("exitpc", "[EXITPC] non-zero exit codes"), ("exit", "process exits (by name)"),
                     ("audio", "audio (alsa-lib in the guest)"),
                     ("wine_err", "Wine err: lines"), ("dxvk", "DXVK / vkd3d-proton err+warn"),
                     ("red", "REDEngineErrorReporter")):
        d = res["classes"][c]
        print(f"-- {title}: {sum(v[0] for v in d.values())} rows, {len(d)} signatures")
        for sig, v in sorted(d.items(), key=lambda kv: (-kv[1][0], kv[1][1])):
            tag = f"  [{v[3]}]" if len(v) > 3 else ""
            print(f"   {v[0]:5d}  L{v[1]:<7d} {sig}{tag}")
    if gaps:
        print("-- fps rows per phase (fps.csv): rows, median, p10, min, longest guest-time gap s, gaps > 2.5 s")
        for ph, g in gaps.items():
            print(f"   {ph:<16} {g['rows']:5d} {g['median']:7.1f} {g['p10']:7.1f} {g['min']:7.1f}"
                  f" {g.get('max_gap_s', '-'):>7} {g.get('hitches', '-'):>4}")
    if mem:
        print("-- [THRS] per process: rows, anon MiB first/max/last, res MiB first/max/last, max threads")
        for n, m in sorted(mem.items(), key=lambda kv: -kv[1]["max"]):
            print(f"   {n:<18} {m['rows']:5d} {m['first']:7d} {m['max']:7d} {m['last']:7d}"
                  f"   {m['res_first']:6d} {m['res_max']:6d} {m['res_last']:6d}  {m['threads_max']:4d}")
    print_session(ses, rs)
    if host:
        print(f"-- host: 9070 XT busy % whole boot {host.get('gpu_busy_all')}, world {host.get('gpu_busy_world')}")
        print(f"   other guests seen: {len(host.get('guests', []))} {' '.join(host.get('guests', []))[:300]}")
        if host.get("serial_silent_max_s") is not None:
            print(f"   longest silent serial stretch: {host['serial_silent_max_s']} s")
    return 0


def aggregate(paths, min_n, game_only=False, show_boots=12):
    runs = []
    for p in paths:
        j = json.load(open(p))
        runs += j if isinstance(j, list) else [j]       # --mine output is a list
    if game_only:
        runs = [r for r in runs if r.get("game", True)]
    n = len(runs)
    agg = {}
    for r in runs:
        for c, d in r["classes"].items():
            if c == "exit":
                continue
            for sig, v in d.items():
                e = agg.setdefault((c, sig), {"boots": [], "rows": 0, "tag": v[3] if len(v) > 3 else ""})
                e["boots"].append(r["run"])
                e["rows"] += v[0]
    print(f"== aggregate over {n} boots: {' '.join(r['run'] for r in runs)}")
    vs = {}
    for r in runs:
        vs.setdefault(r["runlog"].get("verdict", "?"), []).append(r["run"])
    print("   verdicts: " + ", ".join(f"{k}={len(v)}" for k, v in sorted(vs.items(), key=lambda kv: -len(kv[1]))))
    if n <= 40:
        print("   " + ", ".join(f"{r['run']}={r['runlog'].get('verdict', '?')}" for r in runs))
    for c in ("kfault", "kstall", "kdiag", "exitpc", "audio", "red", "wine_err", "dxvk"):
        items = [(k, v) for k, v in agg.items() if k[0] == c and len(v["boots"]) >= min_n]
        if not items:
            continue
        print(f"-- {c}")
        for (c_, sig), v in sorted(items, key=lambda kv: (-len(kv[1]["boots"]), -kv[1]["rows"])):
            print(f"   {len(v['boots'])}/{n}  rows={v['rows']:<5d} {sig}  {('[' + v['tag'] + ']') if v['tag'] else ''}")
            if len(v["boots"]) < n:
                b = v["boots"]
                print(f"          boots: {' '.join(b[:show_boots])}{' ...' if len(b) > show_boots else ''}")
    return 0


def mine(roots):
    """Every serial log under ROOTs, deduplicated by content hash (copies of the
    same boot, serial.log vs serial.txt, count once)."""
    seen = {}
    files = []
    for root in roots:
        if root.startswith("@"):                      # @LIST: one path per line
            files += [x.strip() for x in open(root[1:]) if x.strip()]
            continue
        for d, _, fs in os.walk(root):
            files += [os.path.join(d, f) for f in fs if f in ("serial.log", "serial.txt") or f.endswith(".serial")]
    for p in files:
        if p.endswith("serial.log") and os.path.exists(p[:-3] + "txt"):
            p = p[:-3] + "txt"
        try:
            raw = open(p, "rb").read().replace(b"\r", b"")
        except OSError:
            continue
        if len(raw) < 2000:
            continue
        h = hashlib.sha256(raw).hexdigest()
        if h in seen:
            seen[h]["dups"].append(p)
            continue
        seen[h] = {"path": p, "dups": [], "raw": raw}
    out = []
    for h, e in seen.items():
        cls, ex = classify_serial(e["raw"].decode("utf-8", "replace"))
        game = b"Cyberpunk2077" in e["raw"]
        vp = os.path.join(os.path.dirname(e["path"]), "verdict.txt")
        verdict = open(vp).read().strip() if os.path.exists(vp) else "?"
        out.append({"run": e["path"], "sha": h[:16], "dups": e["dups"], "game": game,
                    "red": bool(cls["red"]), "fps_rows": ex["fps_rows"], "runlog": {"verdict": verdict},
                    "classes": cls})
    json.dump(out, sys.stdout, indent=0)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial", nargs="*")
    ap.add_argument("--run")
    ap.add_argument("--log")
    ap.add_argument("--host")
    ap.add_argument("--json")
    ap.add_argument("--aggregate", action="store_true")
    ap.add_argument("--min", type=int, default=1)
    ap.add_argument("--mine", action="store_true")
    ap.add_argument("--game", action="store_true", help="--aggregate: game boots only")
    a = ap.parse_args()
    if a.aggregate:
        return aggregate(a.serial, a.min, a.game)
    if a.mine:
        return mine(a.serial)
    if len(a.serial) != 1:
        ap.error("one SERIAL")
    a.serial = a.serial[0]
    return report(a)


if __name__ == "__main__":
    sys.exit(main())
