#!/usr/bin/env python3
"""tools/rw/dso-inventory.py — every DSO the RimWorld process maps, and how.

Input is `LD_DEBUG=libs,files` output (trace-host.sh libs).  ld.so labels each
library twice: the NEEDED graph ("needed by", resolved before the program's
entry point) and runtime dlopen ("dynamically loaded by", e.g. SDL loading
libX11.so.6 long after main()).  `calling init:` gives the load order and the
exact resolved path.

Usage: dso-inventory.py <ld-debug-libs.txt> [--md|--count]
"""
import re
import sys
from collections import OrderedDict

INIT = re.compile(r"calling init: (\S+)")
NEEDED = re.compile(r"file=(\S+) \[\d+\];\s+needed by (\S+) \[\d+\]")
DLOPEN = re.compile(r"file=(\S+) \[\d+\];\s+dynamically loaded by (\S+) \[\d+\]")


def main():
    path = sys.argv[1]
    idx = sys.argv.index("--md") if "--md" in sys.argv else None
    loaded, needed, dlopened = OrderedDict(), {}, {}
    for line in open(path, "r", errors="replace"):
        m = INIT.search(line)
        if m:
            p = m.group(1)
            loaded.setdefault(p.rsplit("/", 1)[-1], p)
            continue
        m = NEEDED.search(line)
        if m:
            needed.setdefault(m.group(1), m.group(2).rsplit("/", 1)[-1])
            continue
        m = DLOPEN.search(line)
        if m:
            dlopened.setdefault(m.group(1), m.group(2).rsplit("/", 1)[-1])

    dl_names = {n.rsplit("/", 1)[-1] for n in dlopened}
    dl_loaded = dl_names & set(loaded)
    dl_failed = sorted(dl_names - set(loaded))
    if "--count" in sys.argv:
        print(f"loaded={len(loaded)} dlopened={len(dl_names)} needed={len(needed)}")
        return 0

    print(f"# {len(loaded)} DSOs initialised: {len(loaded) - len(dl_loaded)} via NEEDED, "
          f"{len(dl_loaded)} via runtime dlopen")
    print(f"# {len(dl_names)} distinct dlopen requests; {len(dl_failed)} never mapped "
          f"(failed probes: {', '.join(dl_failed) if dl_failed else 'none'})")
    print()
    print("| DSO | loaded via | requester | resolved path |")
    print("|---|---|---|---|")
    for base, p in loaded.items():
        if base in dl_names:
            how = "dlopen"
            who = dlopened.get(base, dlopened.get(p, "?"))
        else:
            how = "NEEDED"
            who = needed.get(base, "RimWorldLinux")
        print(f"| `{base}` | {how} | {who} | `{p}` |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
