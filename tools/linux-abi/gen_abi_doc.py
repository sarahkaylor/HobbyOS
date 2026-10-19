#!/usr/bin/env python3
"""tools/linux-abi/gen_abi_doc.py — regenerate the measured sections of docs/linux-abi.md.

The compat layer's contract must be honest: every number in docs/linux-abi.md is
measured on a real Linux x86_64 box by tools/linux-abi/probe_abi.c, never typed
from memory.  This script builds + runs the probe and splices its output into the
fenced regions of the doc, which are delimited by:

    <!-- PROBE:<section> -->        ... generated ...
    <!-- /PROBE -->

Sections: constants, syscalls, errno, structs, elf-constants.

  python3 tools/linux-abi/gen_abi_doc.py [--doc docs/linux-abi.md] [--check]

--check exits 1 if the committed doc differs from a fresh probe (use it in CI on
a Linux x86_64 runner so ABI drift fails loudly instead of silently).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PROBE = os.path.join(HERE, "probe_abi.c")

SECTIONS = {
    "constants": "numbers that must match",
    "syscalls": "syscall numbers (x86_64)",
    "errno": "errno values",
    "structs": "struct layouts",
    "elf-constants": "ELF/auxv/proc constants",
}


def run_probe():
    exe = os.path.join(tempfile.gettempdir(), "probe_abi_gen")
    cc = os.environ.get("CC", "cc")
    subprocess.run([cc, "-O0", "-o", exe, PROBE], check=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    # split on "=== title ===" markers, keeping the arch preamble with 'constants'
    parts, cur, buf = {}, "constants", []
    for line in out.splitlines():
        m = re.match(r"^=== (.+?) =+$", line)
        if m:
            parts[cur] = "\n".join(buf).strip("\n")
            cur, buf = m.group(1), []
        else:
            buf.append(line)
    parts[cur] = "\n".join(buf).strip("\n")
    return {key: parts.get(title, "") for key, title in SECTIONS.items()}


def splice(doc_text, sections):
    for key, blob in sections.items():
        begin = f"<!-- PROBE:{key} -->"
        end = "<!-- /PROBE -->"
        i = doc_text.find(begin)
        if i < 0:
            print(f"warning: no {begin} marker in the doc", file=sys.stderr)
            continue
        j = doc_text.find(end, i)
        if j < 0:
            print(f"warning: unterminated PROBE:{key} block", file=sys.stderr)
            continue
        body = f"{begin}\n```text\n{blob}\n```\n"
        doc_text = doc_text[:i] + body + doc_text[j:]
    return doc_text


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--doc", default=os.path.join(REPO, "docs", "linux-abi.md"))
    p.add_argument("--check", action="store_true")
    a = p.parse_args()

    sections = run_probe()
    doc = open(a.doc, encoding="utf-8").read()
    new = splice(doc, sections)
    if a.check:
        if new != doc:
            print("docs/linux-abi.md is STALE — re-run tools/linux-abi/gen_abi_doc.py")
            return 1
        print("docs/linux-abi.md matches a fresh ABI probe")
        return 0
    if new == doc:
        print("docs/linux-abi.md already up to date")
        return 0
    open(a.doc, "w", encoding="utf-8").write(new)
    print(f"updated {a.doc} ({len(sections)} measured sections)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
