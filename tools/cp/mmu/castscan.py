#!/usr/bin/env python3
"""Stream a clang -ast-dump (text) and print every IntegralToPointer /
PointerToIntegral cast whose location is in the given source prefix.

clang's text dump prints locations relative to the previous printed one:
  <path:L:C ...>   file + line + col
  <line:L:C ...>   same file, new line
  <col:C ...>      same file and line
We track the running (file,line) exactly as the dumper does, using the
begin location of each node's range.  Macro-expanded nodes print the
spelling location of the macro body; those are reported at that location.

usage: clang ... -Xclang -ast-dump -fno-color-diagnostics | castscan.py PREFIX TU
"""
import re
import sys

prefix = sys.argv[1]
tu = sys.argv[2]
LOC = re.compile(r'<(?:([^<>:, ]+):(\d+):(\d+)|line:(\d+):(\d+)|col:(\d+)|(<invalid sloc>)|(<scratch space>):(\d+):(\d+)|(<built-in>):(\d+):(\d+))')
# every location token in a line updates the running position (clang
# updates "last location" for both begin and end, and for the extra
# location printed after the range on decls).
ANYLOC = re.compile(r'(?<![\w>])(?:([/\w.+-]+\.[ch]|<scratch space>|<built-in>):(\d+):(\d+)|line:(\d+):(\d+)|col:(\d+))')
cur_file, cur_line = None, None
for raw in sys.stdin:
    kind = None
    if 'CStyleCastExpr' in raw or 'ImplicitCastExpr' in raw:
        if '<IntegralToPointer>' in raw:
            kind = 'I2P'
        elif '<PointerToIntegral>' in raw:
            kind = 'P2I'
    first = None
    # Type names embed non-location file positions ("unnamed struct at
    # src/include/fs.h:116:5"); the dumper does not update its last-location
    # state for those, so drop quoted types and "(... at f:l:c)" first.
    scan = re.sub(r"'[^']*'", "''", raw)
    scan = re.sub(r"\((?:anonymous|unnamed)[^()]* at [^()]*\)", "()", scan)
    for m in ANYLOC.finditer(scan):
        if m.group(1):
            cur_file, cur_line = m.group(1), int(m.group(2))
        elif m.group(4):
            cur_line = int(m.group(4))
        if first is None:
            first = (cur_file, cur_line)
    if kind and first and first[0] and first[0].startswith(prefix):
        expl = 'C' if 'CStyleCastExpr' in raw else 'implicit'
        ty = re.search(r"'([^']*)'", raw)
        print(f"{first[0]}:{first[1]}\t{kind}\t{expl}\t{ty.group(1) if ty else ''}\t{tu}")
