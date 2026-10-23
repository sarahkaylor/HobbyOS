#!/usr/bin/env python3
"""GB-084 open-mode scanner.

Finds open( ... ) calls that pass O_CREAT but only TWO arguments — i.e. omit
the optional third mode argument.  On the host (leg B) ho_open is a vararg
function; a 2-arg open(path, O_CREAT) leaves the mode unspecified (x86-64 vararg
reads a garbage register/stack slot -> an uninitialized/random file mode), which
is a real defect the T4 differential can even trip on.  The DoD: 0 such sites in
tree, and reverting any one FAILs this scanner.

Method: tokenize each .c, find identifiers 'open' followed by a parenthesized
argument list, count top-level commas between the parens, and check if any
argument contains O_CREAT while the arg-count is 1 (path + flags only).  Handles
multi-line calls and nested parens.
"""
import os, re, sys

SCAN_DIR = os.environ.get("OPEN_MODE_SCAN_DIR", os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "src"))

def find_bad_call(text, path):
    bad = []
    # find every 'open(' token that is an identifier open (not e.g. hopen)
    for m in re.finditer(r'(?<![A-Za-z0-9_])open\s*\(', text):
        start = m.end() - 1  # the '('
        i = start + 1
        depth = 1
        args = []
        cur = []
        while i < len(text) and depth > 0:
            c = text[i]
            if c == '(':
                depth += 1; cur.append(c)
            elif c == ')':
                depth -= 1
                if depth == 0:
                    args.append("".join(cur)); break
                else:
                    cur.append(c)
            elif c == ',' and depth == 1:
                args.append("".join(cur)); cur = []
            else:
                cur.append(c)
            i += 1
        args = [a.strip() for a in args]
        flags = args[1] if len(args) >= 2 else ""
        if len(args) == 2 and "O_CREAT" in flags:
            bad.append((path, start))
        if len(args) == 2 and "CREAT" in flags:
            bad.append((path, start))
    return bad

def main():
    bad = []
    for root, _, files in os.walk(SCAN_DIR):
        if "/third_party" in root:
            continue
        for fn in files:
            if not fn.endswith(".c"):
                continue
            p = os.path.join(root, fn)
            try:
                text = open(p, encoding="utf-8", errors="replace").read()
            except Exception:
                continue
            bad += find_bad_call(text, p)
    # dedupe (the two conditions above can double-report)
    seen = set(); uniq = []
    for p, off in bad:
        if (p, off) not in seen:
            seen.add((p, off)); uniq.append((p, off))
    print("open-mode scan: %d two-argument O_CREAT open() calls in '%s'" % (len(uniq), SCAN_DIR))
    for p, off in uniq:
        print("  %s:%d" % (p, off))
    sys.exit(0 if not uniq else 1)

if __name__ == "__main__":
    main()
