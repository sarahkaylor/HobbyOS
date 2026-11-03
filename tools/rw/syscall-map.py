#!/usr/bin/env python3
"""tools/rw/syscall-map.py — strace histogram -> HobbyOS coverage table.

Reads `strace -f -c` output and answers one question per line: is this Linux
number answered by the compat class, by the legacy number-translation switch,
or by nothing (MISSING)?  The answer is *grepped from the tree*, not from a
hand-written list, so it cannot drift:

  * src/include/linux_syscall.h          L_SYS_<name> <nr>
  * src/kernel/linux_compat_{fd,mem,misc,proc}.c   {L_SYS_x, fn, "name"}
  * src/kernel/arch/x64/trap.c           the PROC_FLAG_LINUX_ABI switch cases

Emitted as markdown rows for docs/rimworld-evidence.md plus a coverage summary
(count and call-weighted), so the doc's numbers have a command behind them.

Usage:
  syscall-map.py out/strace-summary.txt [--top N] [--md|--summary|--missing]
                 [--tree <repo root>]
"""
import argparse
import os
import re
import sys

TABLES = ["linux_compat.c", "linux_compat_fd.c", "linux_compat_mem.c",
          "linux_compat_misc.c", "linux_compat_proc.c"]   # linux_compat.c = l_tab_core

# Numbers glibc 2.31's ld.so touches before it transfers to the program:
# startup-critical if missing.  Sourced from the M1 list in the plan plus what
# the histogram showed at the head of the run.
LD_SO = {
    "openat", "open", "read", "pread64", "close", "mmap", "mprotect", "munmap",
    "brk", "arch_prctl", "set_tid_address", "set_robust_list", "rseq",
    "getrandom", "newfstatat", "fstat", "stat", "lstat", "access",
    "readlink", "readlinkat", "getdents64", "madvise", "prlimit64", "uname",
    "getuid", "geteuid", "getgid", "getegid", "getpid", "gettid", "futex",
    "clone", "clone3", "sysinfo", "prctl", "sched_getaffinity", "statx",
}

# Why a missing handler matters, per syscall family.  Short on purpose: the
# doc table is a scoping tool, not an essay.
RISK = {
    "clone": "threads: Mono GC + PhysX + Unity job workers",
    "clone3": "threads: glibc 2.34+ only; 2.31 pin avoids it",
    "futex": "threads: every mutex/condvar/join; must model robust lists",
    "mprotect": "JIT: Mono writes then executes code (W^X)",
    "mmap": "loader + JIT + asset cache",
    "openat": "every asset, DSO and /proc read",
    "newfstatat": "loader search + asset stat",
    "getdents64": "Plugin/Mod directory scans",
    "ioctl": "X11 fd, DRM/ALSA, tty probes",
    "poll": "event loop (X11 fd + timers)",
    "ppoll": "event loop with signal mask",
    "epoll_wait": "alternative event loop; SDL may use it",
    "rt_sigaction": "Mono's crash/null-check handlers",
    "rt_sigprocmask": "thread signal masks",
    "tgkill": "Mono abort/GC suspension",
    "sched_getaffinity": "job worker count",
    "prctl": "thread names, dumpable, VMA naming",
    "getrandom": "ASLR canary + hash seeds",
    "clock_gettime": "frame timing; vDSO may cover it",
    "fstat": "fd type probes",
    "readlink": "/proc/self/exe resolution",
    "statx": "optional: glibc falls back to newfstatat on -ENOSYS",
    "rseq": "optional in 2.31: -ENOSYS is a valid answer",
    "membarrier": "optional: -ENOSYS is a valid answer",
}


def parse_numbers(tree):
    nr2name, name2nr = {}, {}
    path = os.path.join(tree, "src/include/linux_syscall.h")
    for line in open(path, errors="replace"):
        m = re.match(r"#define\s+L_SYS_(\w+)\s+(\d+)\b", line)
        if m:
            name, nr = m.group(1), int(m.group(2))
            nr2name[nr] = name
            name2nr.setdefault(name, nr)
    return nr2name, name2nr


def parse_tables(tree):
    """nr -> 'file:line (fn)' from the four subsystem tables."""
    out = {}
    entry = re.compile(r"\{\s*L_SYS_(\w+)\s*,\s*(\w+)\s*,\s*\"([^\"]*)\"")
    for fname in TABLES:
        path = os.path.join(tree, "src/kernel", fname)
        if not os.path.exists(path):
            continue
        for i, line in enumerate(open(path, errors="replace"), 1):
            for m in entry.finditer(line):
                out[m.group(1)] = f"{fname}:{i} ({m.group(2)})"
    return out


def parse_legacy(tree):
    """nr -> action inside the PROC_FLAG_LINUX_ABI arm of trap.c.

    A case's action may sit on the same line (`case 2: syscall_num = SYS_OPEN;`)
    or on the next one (`case 186:` then `tf->regs[0] = ...; return;`), so each
    case is scanned over the lines until the next `case`/`default:`.
    """
    path = os.path.join(tree, "src/kernel/arch/x64/trap.c")
    lines = open(path, errors="replace").read().splitlines()
    start = next(i for i, l in enumerate(lines) if "PROC_FLAG_LINUX_ABI" in l and "abi_flags" in l)
    end = next(i for i in range(start, len(lines)) if "linux_compat_unhandled" in lines[i])
    out = {}
    case = re.compile(r"case\s+(\d+)\s*:")
    for i in range(start, end + 1):
        for m in case.finditer(lines[i]):
            nr = int(m.group(1))
            block = [lines[i][m.end():]]
            for j in range(i + 1, min(i + 4, end)):
                if case.search(lines[j]) or "default:" in lines[j]:
                    break
                block.append(lines[j])
            text = " ".join(block)
            to = re.search(r"syscall_num\s*=\s*(SYS_\w+)", text)
            fn = re.search(r"(compat_\w+)\(", text)
            if to:
                out[nr] = f"legacy -> {to.group(1)}"
            elif fn:
                out[nr] = f"legacy switch ({fn.group(1)})"
            elif "tf->regs[0]" in text:
                out[nr] = "legacy switch (answered inline)"
            else:
                out[nr] = "legacy switch"
    return out


def parse_strace(path):
    rows = []
    hdr = re.compile(r"^\s*[\d.]+\s+[\d.]+\s+\S+\s+(\d+)\s+(\d+)?\s*(\w+)\s*$")
    for line in open(path, errors="replace"):
        m = hdr.match(line)
        if m and m.group(3) != "total":   # strace's summary row, not a syscall
            rows.append({"calls": int(m.group(1)), "errors": int(m.group(2) or 0),
                         "name": m.group(3)})
    return rows


# x86_64 Linux numbers for calls that src/include/linux_syscall.h does not
# declare yet.  They are printed with a `*` so the table never confuses
# "HobbyOS has a number for it" with "the Linux ABI has a number for it".
KNOWN_NR = {
    "fstatfs": 138, "rt_sigsuspend": 130, "prctl": 157, "getpriority": 140,
    "setpriority": 141, "sched_get_priority_max": 146,
    "sched_get_priority_min": 147, "sched_setscheduler": 144,
    "sched_getparam": 143, "sched_getscheduler": 145, "ftruncate": 77,
    "shmget": 29, "shmat": 30, "shmctl": 31, "shmdt": 67,
    "getresuid": 118, "getresgid": 120, "execve": 59,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("summary")
    ap.add_argument("--tree", default=os.path.join(os.path.dirname(__file__), "..", ".."))
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--md", action="store_true")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--missing", action="store_true")
    a = ap.parse_args()
    tree = os.path.abspath(a.tree)

    nr2name, name2nr = parse_numbers(tree)
    tables = parse_tables(tree)
    legacy = parse_legacy(tree)
    rows = parse_strace(a.summary)
    if not rows:
        print(f"syscall-map: no rows in {a.summary}", file=sys.stderr)
        return 2

    total_calls = sum(r["calls"] for r in rows)
    covered_calls = missing_calls = 0
    for r in rows:
        nr = name2nr.get(r["name"])
        r["nr"] = nr
        if nr is None:
            r["handler"] = "MISSING (no L_SYS_ number)"
            r["kind"] = "missing"
        elif r["name"] in tables:
            r["handler"] = tables[r["name"]]
            r["kind"] = "table"
        elif nr in legacy:
            r["handler"] = legacy[nr]
            r["kind"] = "legacy"
        else:
            r["handler"] = "MISSING"
            r["kind"] = "missing"
        r["startup"] = r["name"] in LD_SO
        r["risk"] = RISK.get(r["name"], "")
        if r["kind"] == "missing":
            missing_calls += r["calls"]
        else:
            covered_calls += r["calls"]

    rows.sort(key=lambda r: -r["calls"])
    top = rows[: a.top]

    if a.stats:
        by_kind = {}
        for r in rows:
            by_kind.setdefault(r["kind"], [0, 0])
            by_kind[r["kind"]][0] += 1
            by_kind[r["kind"]][1] += r["calls"]
        print(f"total syscalls in histogram: {len(rows)} distinct, {total_calls} calls")
        for k in ("table", "legacy", "missing"):
            n, c = by_kind.get(k, (0, 0))
            print(f"  {k:8}: {n:3} distinct  {c:9} calls  ({100.0*c/total_calls:5.1f}%)")
        print(f"  top-{a.top} calls: {sum(r['calls'] for r in top)} "
              f"({100.0*sum(r['calls'] for r in top)/total_calls:.1f}% of all)")
        miss = [r for r in rows if r["kind"] == "missing"]
        print(f"  MISSING distinct={len(miss)} calls={sum(r['calls'] for r in miss)}")
        print(f"  startup-critical missing: " +
              (", ".join(f"{r['name']}({r['calls']})" for r in miss if r['startup']) or "none"))
        return 0

    if a.missing:
        for r in rows:
            if r["kind"] == "missing":
                nr = r["nr"] if r["nr"] is not None else KNOWN_NR.get(r["name"], "?")
                print(f"{r['name']:24} nr={nr:>4} "
                      f"calls={r['calls']:>9} startup={'yes' if r['startup'] else 'no':3} "
                      f"{r['risk']}")
        return 0

    print("| syscall | nr | calls | handler (file:line) | risk |")
    print("|---|---:|---:|---|---|")
    for r in top:
        star = " **startup**" if r["startup"] else ""
        nr = r["nr"] if r["nr"] is not None else (
            f"{KNOWN_NR[r['name']]}*" if r["name"] in KNOWN_NR else "—")
        print(f"| `{r['name']}` | {nr} | {r['calls']} | {r['handler']}{star} | {r['risk']} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
