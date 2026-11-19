#!/usr/bin/env python3
"""tools/cp/strace-summary.py — raw `strace -f -tt -T -yy` of a Wine run ->
per-syscall histogram, first-use order, sub-command inventories and the
HobbyOS coverage diff (docs/cyberpunk-evidence.md).

Only WINE-SIDE processes are counted: a pid becomes Wine-side at its first
successful execve() of a /CP/... path (so the host `env` and `chroot` that
tools/cp/run-wine-host.sh uses to enter the pinned root are excluded), and
every pid cloned/forked by a Wine-side pid is Wine-side from its first line.

Each process is labelled by its image: `wineserver`, `wine-preloader`/`wine`
(the loader before ntdll knows the .exe), or the Windows program
(`services.exe`, `cmd.exe`, ...) taken from the preloader's argv.

HobbyOS coverage is derived from the tree, never from a list in this file:
  * names: every `{L_SYS_<name>, fn, "..."}` row in src/kernel/linux_compat*.c
    (tables l_tab_core/fd/mem/proc/misc/thr/tmr; *_test.c excluded);
  * numbers: `case <n>:` arms of the PROC_FLAG_LINUX_ABI switch in
    src/kernel/arch/x64/trap.c (the legacy number-translation fallback),
    mapped to names with the kernel UAPI's asm/unistd_64.h;
  * everything else reaches `linux_compat_unhandled` -> -ENOSYS.
A name that has a handler can still be partial (a handler may refuse flags or
sub-commands); the sub-command tables below are for that second look.

Usage: strace-summary.py [--tree ROOT] [--unistd PATH] RAW [RAW...]
"""
import argparse
import collections
import glob
import os
import re
import sys

LINE = re.compile(r"^(\d+)\s+(\d\d:\d\d:\d\d\.\d+)\s+(.*)$")
CALL = re.compile(r"^([a-z_][a-z0-9_]*)\((.*)$")
RESUMED = re.compile(r"^<\.\.\. ([a-z_][a-z0-9_]*) resumed>(.*)$")


def unistd_map(path):
    nr = {}
    for line in open(path, errors="replace"):
        m = re.match(r"#define\s+__NR_(\w+)\s+(\d+)", line)
        if m:
            nr[m.group(1)] = int(m.group(2))
    return nr


def find_unistd(cli):
    for p in ([cli] if cli else []) + ["/usr/include/x86_64-linux-gnu/asm/unistd_64.h",
                                       "/usr/include/asm/unistd_64.h"]:
        if p and os.path.exists(p):
            return p
    sys.exit("strace-summary: no asm/unistd_64.h (pass --unistd)")


def hobbyos_coverage(tree, nr_of):
    names = {}
    for f in sorted(glob.glob(os.path.join(tree, "src/kernel/linux_compat*.c"))):
        if f.endswith("_test.c"):
            continue
        for i, line in enumerate(open(f, errors="replace"), 1):
            for m in re.finditer(r"\{\s*L_SYS_(\w+)\s*,\s*(\w+)\s*,\s*\"", line):
                names.setdefault(m.group(1), f"{os.path.basename(f)}:{i} {m.group(2)}")
    legacy = {}
    tp = os.path.join(tree, "src/kernel/arch/x64/trap.c")
    lines = open(tp, errors="replace").read().splitlines()
    start = next(i for i, l in enumerate(lines) if "linux_compat_dispatch(lp, tf, syscall_num)" in l)
    end = next(i for i in range(start, len(lines)) if "linux_compat_unhandled(lp" in lines[i])
    name_of = {v: k for k, v in nr_of.items()}
    for i in range(start, end):
        for m in re.finditer(r"case\s+(\d+)\s*:", lines[i]):
            n = int(m.group(1))
            if n in name_of:
                legacy.setdefault(name_of[n], f"trap.c:{i + 1} case {n}")
    return names, legacy, (start + 1, end + 1)


def image_from_execve(args):
    paths = re.findall(r'"((?:[^"\\]|\\.)*)"', args)
    if not paths:
        return "?"
    exe = paths[0]
    argv = paths[1:]
    base = exe.rsplit("/", 1)[-1]
    if base == "wine-preloader" and len(argv) >= 3:
        prog = argv[2].replace("\\\\", "\\")
        return prog.rsplit("\\", 1)[-1].rsplit("/", 1)[-1] or "wine"
    if base == "wine" and len(argv) >= 2 and exe.endswith("-unix/wine"):
        prog = argv[1].replace("\\\\", "\\")
        return prog.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
    return base


def split_ret(args):
    """'..args) = -1 ENOENT (x) <0.1>' -> (ret, errno-name or None)."""
    i = args.rfind(") = ")
    if i < 0:
        return None, None
    tail = args[i + 4:].split()
    if not tail:
        return None, None
    ret = re.sub(r"<.*$", "", tail[0])
    err = tail[1] if ret.startswith("-") and len(tail) > 1 and tail[1][:1].isupper() else None
    return ret, err


def parse(paths):
    pending = {}          # pid -> event awaiting its "<... resumed>"
    events = []           # [pid, ts, name, args, ret, err, unfinished]
    for path in paths:
        for raw in open(path, errors="replace"):
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            pid, ts, rest = int(m.group(1)), m.group(2), m.group(3)
            if rest.startswith("+++") or rest.startswith("---"):
                continue
            r = RESUMED.match(rest)
            if r:
                ev = pending.pop(pid, None)
                if ev is None:
                    continue
                ev[3] += r.group(2)
                ev[4], ev[5] = split_ret(ev[3])
                ev[6] = False
                continue
            c = CALL.match(rest)
            if not c:
                continue
            name, args = c.group(1), c.group(2)
            if args.endswith("<unfinished ...>"):
                ev = [pid, ts, name, args[: -len("<unfinished ...>")], None, None, True]
                pending[pid] = ev
                events.append(ev)
                continue
            ret, err = split_ret(args)
            events.append([pid, ts, name, args, ret, err, False])
    return events


def classify(events):
    """Keep only Wine-side events, labelled with their process image."""
    INF = float("inf")
    wine_from = collections.defaultdict(lambda: INF)   # pid -> first Wine-side index
    clones = []                                        # (index, parent, child)
    for i, ev in enumerate(events):
        pid, name, args, ret = ev[0], ev[2], ev[3], ev[4]
        if name == "execve" and ret == "0" and '"/CP/' in args.split(",", 1)[0]:
            wine_from[pid] = min(wine_from[pid], i)
        if name in ("clone", "clone3", "fork", "vfork") and ret and ret.isdigit() and int(ret) > 0:
            clones.append((i, pid, int(ret)))
    for i, parent, child in clones:          # in trace order, so parents resolve first
        if wine_from[parent] <= i:
            wine_from[child] = -1            # every line of the child counts
    image = {}
    out = []
    for i, (pid, ts, name, args, ret, err, _) in enumerate(events):
        if i < wine_from[pid]:
            continue
        if name == "execve" and ret == "0":
            image[pid] = image_from_execve(args)
        out.append((pid, ts, name, args, ret, err, image.get(pid, "?")))
        if name in ("clone", "clone3", "fork", "vfork") and ret and ret.isdigit() and int(ret) > 0:
            image.setdefault(int(ret), image.get(pid, "?"))
    # children seen before the parent's clone() returned inherit afterwards
    return [(pid, ts, n, a, r, e, img if img != "?" else image.get(pid, "?"))
            for (pid, ts, n, a, r, e, img) in out]


def first_arg(args):
    return args.split(",", 1)[0].strip()


def fd_kind(tok):
    m = re.search(r"<([^>]*)>", tok)
    if not m:
        return "?"
    d = m.group(1)
    if d.startswith("/dev/"):
        return d
    if d.startswith(("UNIX", "socket", "TCP", "UDP", "NETLINK")):
        return d.split(":", 1)[0]
    if d.startswith(("pipe", "anon_inode")):
        return d.split(":", 1)[0] + (":" + d.split(":", 2)[1] if d.startswith("anon_inode") else "")
    return "file"


DECOR = re.compile(r"<[^<>]*>")


def sub_inventory(rows):
    inv = collections.defaultdict(collections.Counter)
    first = {}
    for pid, ts, name, raw_args, ret, err, img in rows:
        key = None
        args = DECOR.sub("", raw_args) if name not in ("ioctl",) else raw_args
        if name == "ioctl":
            parts = [a.strip() for a in args.split(",")]
            stripped = [a.strip() for a in DECOR.sub("", args).split(",")]
            req = stripped[1] if len(stripped) > 1 else "?"
            req = re.sub(r"\)\s*=.*$", "", req)
            key = ("ioctl", f"{req} on {fd_kind(parts[0])}", err or "ok")
        elif name == "prctl":
            key = ("prctl", re.sub(r"\)\s*=.*$", "", first_arg(args)), err or "ok")
        elif name == "arch_prctl":
            key = ("arch_prctl", re.sub(r"\)\s*=.*$", "", first_arg(args)), err or "ok")
        elif name in ("setsockopt", "getsockopt"):
            parts = [a.strip() for a in args.split(",")]
            key = (name, f"{parts[1] if len(parts) > 1 else '?'} {parts[2] if len(parts) > 2 else '?'}", err or "ok")
        elif name in ("socket", "socketpair"):
            parts = [a.strip() for a in args.split(",")]
            key = (name, f"{parts[0]} {parts[1] if len(parts) > 1 else ''}", err or "ok")
        elif name == "fcntl":
            parts = [a.strip() for a in args.split(",")]
            cmd = re.sub(r"\)\s*=.*$", "", parts[1] if len(parts) > 1 else "?")
            key = ("fcntl", cmd, err or "ok")
        elif name == "futex":
            parts = [a.strip() for a in args.split(",")]
            key = ("futex", parts[1] if len(parts) > 1 else "?", err or "ok")
        elif name == "rt_sigaction":
            key = ("rt_sigaction", first_arg(args) + (" SA_ONSTACK" if "SA_ONSTACK" in args else ""), "-")
        elif name in ("clone", "clone3"):
            fl = re.search(r"flags=([A-Z_|]+(?:\|SIG\w+)?)", args)
            key = (name, fl.group(1) if fl else "?", err or "ok")
        elif name in ("sendmsg", "recvmsg") and "SCM_RIGHTS" in args:
            key = (name, "SCM_RIGHTS", err or "ok")
        elif name in ("openat", "open", "stat", "newfstatat", "access", "readlink", "statfs", "faccessat2"):
            pm = re.search(r'"(/(?:proc|sys|dev)/[^"]*)"', args)
            if pm:
                pth = re.sub(r"/\d+(/|$)", r"/<n>\1", pm.group(1))
                pth = re.sub(r"(cpu|ttyS|ttyUSB|ttyACM|lp|node|hidraw|event|js)\d+", r"\1<n>", pth)
                key = ("pseudo-path " + name, pth, err or "ok")
        elif name == "madvise":
            parts = [a.strip() for a in args.split(",")]
            key = ("madvise", re.sub(r"\)\s*=.*$", "", parts[-1]), err or "ok")
        elif name == "prlimit64":
            parts = [a.strip() for a in args.split(",")]
            key = ("prlimit64", parts[1] if len(parts) > 1 else "?", err or "ok")
        elif name == "mmap":
            parts = [a.strip() for a in args.split(",")]
            if len(parts) >= 4:
                key = ("mmap flags", parts[3], err or "ok")
        if key:
            inv[key[0]][(key[1], key[2])] += 1
            first.setdefault((key[0], key[1], key[2]), (ts, img))
    return inv, first


def mmap_layout(rows):
    """Fixed / low / large mmaps: the address-space facts P1 needs."""
    fixed, large = collections.Counter(), []
    for pid, ts, name, args, ret, err, img in rows:
        if name != "mmap":
            continue
        parts = [a.strip() for a in args.split(",")]
        if len(parts) < 4:
            continue
        try:
            addr = int(parts[0], 16) if parts[0].startswith("0x") else 0
            length = int(parts[1], 0)
        except ValueError:
            continue
        flags = parts[3]
        if ("MAP_FIXED" in flags) and addr and addr < (1 << 32):
            fixed[(hex(addr), hex(length), parts[2], flags, err or "ok", img)] += 1
        if length >= 64 << 20:
            large.append((ts, img, parts[0], hex(length), parts[2], flags, ret, err or "ok"))
    return fixed, large


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw", nargs="+")
    ap.add_argument("--tree", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--unistd")
    a = ap.parse_args()
    upath = find_unistd(a.unistd)
    nr_of = unistd_map(upath)
    covered, legacy, span = hobbyos_coverage(os.path.abspath(a.tree), nr_of)

    rows = classify(parse(a.raw))
    calls, errs = collections.Counter(), collections.defaultdict(collections.Counter)
    first, byimg = {}, collections.defaultdict(collections.Counter)
    pids, imgs = set(), collections.Counter()
    for i, (pid, ts, name, args, ret, err, img) in enumerate(rows):
        calls[name] += 1
        if err:
            errs[name][err] += 1
        if name not in first:
            first[name] = (i, ts, img)
        byimg[img][name] += 1
        if pid not in pids:
            pids.add(pid)
            imgs[img] += 1

    def status(n):
        if n in covered:
            return "table", covered[n]
        if n in legacy:
            return "legacy", legacy[n]
        return "MISSING", "-> linux_compat_unhandled, -ENOSYS"

    print(f"# strace summary: {', '.join(os.path.basename(p) for p in a.raw)}\n")
    print(f"Wine-side events: {len(rows)}; tasks (pids+tids): {len(pids)}; "
          f"distinct syscalls: {len(calls)}; unistd: `{upath}`.")
    print(f"HobbyOS coverage from tree: {len(covered)} table names "
          f"(src/kernel/linux_compat*.c), {len(legacy)} legacy cases "
          f"(src/kernel/arch/x64/trap.c:{span[0]}-{span[1]}).\n")
    print("Tasks per image: " + ", ".join(f"{k} {v}" for k, v in imgs.most_common()) + "\n")

    print("## Histogram (Wine-side processes), by first use\n")
    print("| # | syscall | nr | calls | errors | first use (t, image) | HobbyOS |")
    print("|---|---|---|---|---|---|---|")
    for rank, (n, (i, ts, img)) in enumerate(sorted(first.items(), key=lambda kv: kv[1][0]), 1):
        e = ", ".join(f"{k}×{v}" for k, v in errs[n].most_common(3))
        st, where = status(n)
        print(f"| {rank} | `{n}` | {nr_of.get(n, '?')} | {calls[n]} | {e or '-'} | {ts} {img} | "
              f"{'**MISSING**' if st == 'MISSING' else st + ' ' + where} |")

    miss = [n for n in sorted(first, key=lambda n: first[n][0]) if status(n)[0] == "MISSING"]
    print(f"\n## Missing from HobbyOS ({len(miss)}), ranked by first use\n")
    print("| rank | syscall | nr | calls | first use (t, image) |")
    print("|---|---|---|---|---|")
    for r, n in enumerate(miss, 1):
        i, ts, img = first[n]
        print(f"| {r} | `{n}` | {nr_of.get(n, '?')} | {calls[n]} | {ts} {img} |")

    print("\n## Calls per image (top 12 syscalls each)\n")
    for img, c in sorted(byimg.items(), key=lambda kv: -sum(kv[1].values())):
        top = ", ".join(f"{k} {v}" for k, v in c.most_common(12))
        print(f"- **{img}** ({sum(c.values())}): {top}")

    inv, ifirst = sub_inventory(rows)
    print("\n## Sub-command inventories\n")
    for fam in sorted(inv):
        print(f"### {fam}\n")
        print("| value | result | count | first (t, image) |")
        print("|---|---|---|---|")
        for (val, res), cnt in sorted(inv[fam].items(), key=lambda kv: ifirst[(fam, kv[0][0], kv[0][1])][0]):
            ts, img = ifirst[(fam, val, res)]
            print(f"| `{val[:110]}` | {res} | {cnt} | {ts} {img} |")
        print()

    fixed, large = mmap_layout(rows)
    agg = collections.defaultdict(lambda: [0, set()])
    for (ad, ln, pr, fl, res, img), cnt in fixed.items():
        agg[(ad, ln, pr, fl, res)][0] += cnt
        agg[(ad, ln, pr, fl, res)][1].add(img)
    print("## MAP_FIXED / MAP_FIXED_NOREPLACE mappings below 4 GiB\n")
    print("| addr | len | prot | flags | result | calls | images |")
    print("|---|---|---|---|---|---|---|")
    for (ad, ln, pr, fl, res), (cnt, imgs_) in sorted(agg.items(), key=lambda kv: (int(kv[0][0], 16), kv[0][1])):
        il = sorted(imgs_)
        print(f"| {ad} | {ln} | {pr} | {fl} | {res} | {cnt} | {len(il)}: {', '.join(il[:4])}{' …' if len(il) > 4 else ''} |")
    nr_ = collections.Counter()
    for pid, ts, name, args, ret, err, img in rows:
        if name == "mmap" and "MAP_FIXED_NOREPLACE" in args:
            nr_[err or "ok"] += 1
    print("\nMAP_FIXED_NOREPLACE results: " + ", ".join(f"{k} {v}" for k, v in nr_.most_common()))
    print("\n## mmap() of >= 64 MiB (first per image/hint/len/prot/flags)\n")
    print("| t | image | hint | len | prot | flags | ret | result |")
    print("|---|---|---|---|---|---|---|---|")
    seen = set()
    for row in large:
        k = (row[2], row[3], row[4], row[5])
        if k in seen:
            continue
        seen.add(k)
        print("| " + " | ".join(str(x) for x in row) + " |")


if __name__ == "__main__":
    sys.exit(main())
