#!/usr/bin/env python3
"""Find open(...) calls that pass O_CREAT with no mode argument.

On Linux, open(path, O_CREAT[, flags]) without a mode is undefined: the mode
comes from whatever the ABI register happened to hold, so the created file's
permissions are garbage.  Our host shim (ho_open) now reads va_arg only when
O_CREAT is set — a caller that omits the mode therefore creates a file with an
arbitrary mode (often 0000) and every later open() fails EACCES.
"""
import os, re, sys

roots = sys.argv[1:] or ["src/user"]
hits = []
for root in roots:
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".c"):
                continue
            p = os.path.join(dirpath, fn)
            src = open(p, errors="replace").read()
            for m in re.finditer(r"\bopen\s*\(", src):
                i = m.end()
                depth = 1
                while i < len(src) and depth:
                    if src[i] == "(":
                        depth += 1
                    elif src[i] == ")":
                        depth -= 1
                    i += 1
                call = src[m.start():i]
                if "O_CREAT" not in call:
                    continue
                inner = call[call.index("(") + 1:call.rindex(")")]
                # top-level commas
                d = 0
                commas = 0
                for ch in inner:
                    if ch in "([{":
                        d += 1
                    elif ch in ")]}":
                        d -= 1
                    elif ch == "," and d == 0:
                        commas += 1
                line = src[:m.start()].count("\n") + 1
                if commas < 2:
                    hits.append((p, line, " ".join(call.split())[:110]))

print(f"{len(hits)} open(O_CREAT) calls without a mode argument:")
for p, line, call in hits:
    print(f"  {p}:{line}\n      {call}")
