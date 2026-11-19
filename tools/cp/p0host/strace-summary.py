#!/usr/bin/env python3
"""tools/cp/p0host/strace-summary.py <trace> <outdir> [--game-dir DIR] [--from HH:MM:SS] [--until HH:MM:SS]

Post-processes ONE `strace -f -tt -y -s 96 -o <trace>` log of a whole launch (every
process of the run) into small text tables:

  procs.txt        tgid -> name (last execve; Wine processes named by their .exe), #threads
  hist_all.txt     syscall histogram over all processes (calls, errors)
  hist_by_proc.txt per-process-name histogram (top 40 each)
  ioctl.txt        ioctl request x fd class x process
  prctl.txt        prctl / arch_prctl options x process
  files.txt        game-dir files: opens, read/pread bytes, mmaps (len/prot/flags) by the game
  mmaps.txt        all file-backed mmaps of game-dir files with sizes
Thread -> process attribution follows clone/clone3/fork/vfork return values (CLONE_THREAD
=> same process).  Nothing from the trace itself is copied except file paths + numbers.
"""
import re, sys, os, collections

trace, out = sys.argv[1], sys.argv[2]
GAME = "/mnt/fast/games/steamapps/common/Cyberpunk 2077/"
if "--game-dir" in sys.argv: GAME = sys.argv[sys.argv.index("--game-dir") + 1]
T_FROM = sys.argv[sys.argv.index("--from") + 1] if "--from" in sys.argv else None
T_UNTIL = sys.argv[sys.argv.index("--until") + 1] if "--until" in sys.argv else None
os.makedirs(out, exist_ok=True)

L = re.compile(r'^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+(.*)$')
CALL = re.compile(r'^([a-z_0-9]+)\((.*)$')
RES = re.compile(r'^<\.\.\. ([a-z_0-9]+) resumed>(.*)$')
RET = re.compile(r'\)\s+=\s+(-?\d+|0x[0-9a-f]+|\?)(?:\s+(E[A-Z0-9]+))?')
FDP = re.compile(r'^(-?\d+)<([^>]*)>')

tgid = {}            # tid -> tgid
pname = {}           # tgid -> name
nthreads = collections.Counter()
hist = collections.Counter(); herr = collections.Counter()
hproc = collections.defaultdict(collections.Counter)
ioctl = collections.Counter(); prctl = collections.Counter()
pending = {}         # tid -> (call, args)
fopen = collections.Counter(); fread = collections.Counter(); freadcalls = collections.Counter()
fmmap = []           # (proc, path, len, prot, flags, off)
first_ts = last_ts = None

def proc_of(tid):
    return tgid.setdefault(tid, tid)

def name_of(tid):
    return pname.get(proc_of(tid), "?")

def fdclass(p):
    if p.startswith("/dev/dri/"): return p
    if p.startswith("socket:"): return "socket"
    if p.startswith("anon_inode:"): return p.split("[")[0] + "[" + p.split("[")[1] if "[" in p else p
    if p.startswith("/dev/"): return p
    return "other"

def split_args(a):
    # top-level comma split (ignores commas inside {} [] "" ())
    res, cur, depth, q = [], [], 0, False
    i = 0
    while i < len(a):
        c = a[i]
        if q:
            cur.append(c)
            if c == "\\": cur.append(a[i+1]); i += 1
            elif c == '"': q = False
        elif c == '"': q = True; cur.append(c)
        elif c in "{[(": depth += 1; cur.append(c)
        elif c in "}])":
            if depth == 0: break
            depth -= 1; cur.append(c)
        elif c == "," and depth == 0: res.append("".join(cur).strip()); cur = []
        else: cur.append(c)
        i += 1
    res.append("".join(cur).strip())
    return res

counting = True
def finish(tid, call, args, tail):
    global first_ts
    m = RET.search(")" + tail if not tail.startswith(")") else tail)
    ret = m.group(1) if m else "?"
    err = m.group(2) if m else None
    p = name_of(tid)
    av = split_args(args)
    if call not in ("clone", "clone3", "fork", "vfork", "execve") and not counting:
        return
    if counting:
        hist[call] += 1; hproc[p][call] += 1
        if err: herr[call] += 1
    elif call not in ("clone", "clone3", "fork", "vfork", "execve"):
        return
    if call in ("clone", "clone3", "fork", "vfork") and ret not in ("?",) and not ret.startswith("-") and ret != "0":
        child = int(ret)
        if "CLONE_THREAD" in args:
            tgid[child] = proc_of(tid); nthreads[proc_of(tid)] += 1
        else:
            tgid[child] = child
            par = pname.get(proc_of(tid), "?")
            # wineserver daemonises by forking; the child IS the server
            pname[child] = "wineserver(daemon)" if par == "wineserver" else par + "(fork)"
    elif call == "execve" and ret == "0":
        argv = re.findall(r'"((?:[^"\\]|\\.)*)"', args)
        nm = None
        a0 = os.path.basename(argv[1]) if len(argv) > 1 else ""
        if a0.startswith("python"):     # `python3 proton waitforexitandrun <game.exe>`
            pname[proc_of(tid)] = "python3(proton script)"; return
        for x in argv[1:6]:
            if x.lower().endswith(".exe"): nm = x.replace("\\\\", "\\").split("\\")[-1].split("/")[-1]; break
        if not nm and argv: nm = os.path.basename(argv[0])
        pname[proc_of(tid)] = nm or "?"
    elif call == "ioctl" and av:
        fm = FDP.match(av[0]); fc = fdclass(fm.group(2)) if fm else "?"
        req = av[1] if len(av) > 1 else "?"
        req = re.sub(r'\(.*', '', req)
        ioctl[(p, fc, req, "ERR" if err else "ok")] += 1
    elif call == "prctl" and av:
        prctl[(p, "prctl", av[0])] += 1
    elif call == "arch_prctl" and av:
        prctl[(p, "arch_prctl", av[0])] += 1
    elif call in ("openat", "open", "openat2"):
        path = None
        qs = re.findall(r'"((?:[^"\\]|\\.)*)"', args)
        if qs: path = qs[0]
        if path and path.startswith(GAME) and not err:
            fopen[(p, path[len(GAME):])] += 1
    elif call in ("read", "pread64", "readv", "preadv", "preadv2") and av:
        fm = FDP.match(av[0])
        if fm and fm.group(2).startswith(GAME) and ret.lstrip("-").isdigit() and not err:
            fread[(p, fm.group(2)[len(GAME):])] += int(ret)
            freadcalls[(p, fm.group(2)[len(GAME):], call)] += 1
    elif call == "mmap" and len(av) >= 6:
        fm = FDP.match(av[4])
        if fm and fm.group(2).startswith(GAME) and not err:
            fmmap.append((p, fm.group(2)[len(GAME):], int(av[1], 0), av[2], av[3], av[5]))

with open(trace, errors="replace") as fh:
    for line in fh:
        m = L.match(line)
        if not m: continue
        tid, ts, rest = int(m.group(1)), m.group(2), m.group(3)
        if T_UNTIL and ts > T_UNTIL: break
        counting = not (T_FROM and ts < T_FROM)
        proc_of(tid)
        if rest.startswith("+++") or rest.startswith("---"): continue
        if counting:
            first_ts = first_ts or ts; last_ts = ts
        r = RES.match(rest)
        if r:
            call = r.group(1)
            pc = pending.pop(tid, (call, ""))
            finish(tid, call, pc[1], r.group(2))
            continue
        c = CALL.match(rest)
        if not c: continue
        call, args = c.group(1), c.group(2)
        if rest.endswith("<unfinished ...>"):
            pending[tid] = (call, args.replace("<unfinished ...>", ""))
            continue
        finish(tid, call, args, args[args.rfind(")"):] if ")" in args else "")

def w(name, lines):
    with open(os.path.join(out, name), "w") as f:
        f.write("\n".join(lines) + "\n")

# process table
pc = collections.Counter()
for t, g in tgid.items(): pc[g] += 1
w("procs.txt", [f"# trace {first_ts} .. {last_ts}", "tgid\ttids_seen\tname"] +
  [f"{g}\t{pc[g]}\t{pname.get(g,'?')}" for g in sorted(pc)])
tot = sum(hist.values())
w("hist_all.txt", [f"# all processes: {tot} syscalls, {len(hist)} distinct", "calls\terrors\tpct\tsyscall"] +
  [f"{n}\t{herr[s]}\t{100*n/tot:.2f}\t{s}" for s, n in hist.most_common()])
lines = []
for p, h in sorted(hproc.items(), key=lambda x: -sum(x[1].values())):
    t = sum(h.values())
    lines.append(f"== {p}: {t} syscalls, {len(h)} distinct")
    lines += [f"  {n}\t{100*n/t:.2f}\t{s}" for s, n in h.most_common()]
w("hist_by_proc.txt", lines)
w("ioctl.txt", ["count\tproc\tfd\trequest\tresult"] +
  [f"{n}\t{p}\t{fc}\t{rq}\t{ok}" for (p, fc, rq, ok), n in sorted(ioctl.items(), key=lambda x: -x[1])])
w("prctl.txt", ["count\tproc\tcall\toption"] +
  [f"{n}\t{p}\t{c}\t{o}" for (p, c, o), n in sorted(prctl.items(), key=lambda x: -x[1])])
fl = ["# game-dir files touched: opens, bytes read (read/pread64/readv), calls"]
paths = sorted(set(k[1] for k in fopen) | set(k[1] for k in fread))
for path in paths:
    o = sum(v for k, v in fopen.items() if k[1] == path)
    b = sum(v for k, v in fread.items() if k[1] == path)
    rc = {c: v for (pp, f, c), v in freadcalls.items() if f == path}
    try: sz = os.path.getsize(GAME + path)
    except OSError: sz = -1
    mm = [x for x in fmmap if x[1] == path]
    fl.append(f"{path}\tsize={sz}\topens={o}\tbytes_read={b}\treads={rc}\tmmaps={len(mm)}\tmmap_bytes={sum(x[2] for x in mm)}")
fl.append(f"# TOTAL bytes read from game dir: {sum(fread.values())}")
for p in sorted(set(k[0] for k in fread)):
    fl.append(f"# bytes read by {p}: {sum(v for k, v in fread.items() if k[0] == p)}")
w("files.txt", fl)
w("mmaps.txt", ["proc\tlen\tprot\tflags\toffset\tpath"] +
  [f"{p}\t{ln}\t{pr}\t{fl_}\t{off}\t{path}" for p, path, ln, pr, fl_, off in sorted(fmmap, key=lambda x: -x[2])])
print("done", tot, "syscalls", len(pc), "tasks")
