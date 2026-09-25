#!/bin/bash
# tsort_parity.sh — byte-exact acceptance test for the HobbyOS ported tsort.
#
# Runs our host-built tsort (tsort_host, argv[0] forced to "tsort" via
# exec -a so error messages are comparable) against a GNU tsort reference
# on the same inputs and compares stdout bytes, exit codes and stderr text
# across simple chains, diamonds, cycles (incl. the exact loop listing),
# odd token counts, repeated and self edges, whitespace variants, stdin
# and FILE operands, binary junk and the error paths.
#
# Reference selection:
#   $2 = explicit path, else $TSORT_REF, else the first of these that exists:
#        /usr/bin/tsort — a modern coreutils tsort; compared in LOOSE mode:
#        its diagnostics and --help/--version text were rewritten, it rejects
#        an odd token count (a check textutils-2.1 does not have — 2.1 just
#        leaves the dangling token to be sorted, see the port's HobbyOS
#        note), and it reports directory read errors 2.1 ignores.  The
#        strictest bar is `make tsort_parity_strict`, which builds
#        textutils-2.1's original tsort as the reference instead: stdout,
#        stderr and the exit code compare byte-for-byte.
#
# Comparison levels (strict mode):
#   FULL  — stdout + stderr + exit code, all byte-exact.
#   OUTRC — stdout + exit code byte-exact; stderr NOT compared.  Used only
#           for the unknown-option cases, where the *sysroot* getopt prints
#           glibc-style quoted diagnostics ("invalid option -- 'x'") while
#           2.1's own getopt prints them unquoted ("invalid option -- x").
#           This is a sysroot-wide getopt wording difference (the same one
#           the cut/head/tail parity corpora sidestep); everything the port
#           itself prints still compares byte-for-byte.
# In loose mode stderr is never compared, exit codes compare as
# success/failure classes, a stdout difference is accepted only when our
# output is still a valid topological ordering of the same input token
# stream (verified with python3), and the cases tagged `modern` are skipped
# with a note (they are exactly the documented 2.1-vs-modern divergences).
set -u

TSORT_HOST="${1:-./obj/tsort_host}"
REF="${3:-${2:-${TSORT_REF:-$(command -v tsort || echo /usr/bin/tsort)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0
N_OUTRC=0
N_SKIPPED=0
N_ORDER_NOTES=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict:
# stdout + stderr + exit codes compare byte-for-byte) vs a modern
# coreutils tsort (loose: diagnostics were rewritten after 2.1).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# --- fixtures --------------------------------------------------------------
printf 'a b\n'                            > "$TMP/pair.txt"
printf 'a b\nb c\n'                       > "$TMP/chain3.txt"
printf 'b c\nc d\na b\n'                  > "$TMP/chain3rev.txt"
printf 'a b\nc d\n'                       > "$TMP/twochains.txt"
printf 'a b\na c\nb d\nc d\n'             > "$TMP/diamond.txt"
printf 'a c\nb d\na b\nc d\n'             > "$TMP/diamond_rev.txt"
printf 'a b\na c\nb c\nc d\n'             > "$TMP/diamond_x.txt"
printf 'a b\na c\nb d\nc d\na b\n'        > "$TMP/diamond_dup.txt"
printf 'a b\na c\nb d\nc d\nd e\n'        > "$TMP/diamond_chain.txt"
printf 'a b\nb a\n'                       > "$TMP/cycle2.txt"
printf 'a b\nb c\nc a\n'                  > "$TMP/cycle3.txt"
printf 'w x\nx y\ny z\nz w\n'             > "$TMP/cycle4.txt"
printf 'a b\nb c\nc a\nc d\n'             > "$TMP/cycletail.txt"
printf 'a b\nb a\nx y\ny x\n'             > "$TMP/twocycles.txt"
printf 'z a\na b\nb c\nc a\n'             > "$TMP/cycin.txt"
printf 'a b\nb a\nc d\n'                  > "$TMP/cycleplus.txt"
printf 'a a\n'                            > "$TMP/selfloop.txt"
printf 'a a\na b\n'                       > "$TMP/selfloop2.txt"
printf 'a b\na b\nb a\n'                  > "$TMP/dupcycle.txt"
printf 'x y\nx y\nx y\n'                  > "$TMP/dupedges.txt"
printf 'a b\nb c\na b\n'                  > "$TMP/dupchainedge.txt"
printf 'a b c\n'                          > "$TMP/odd3.txt"
printf 'a b c d e\n'                      > "$TMP/odd5.txt"
printf 'a b c d'                          > "$TMP/oddnonl.txt"
printf 'a b\nb a\nc\n'                    > "$TMP/oddloop.txt"
printf 'k\n'                              > "$TMP/onetok.txt"
printf ''                                 > "$TMP/empty.txt"
printf 'a   b\tc \n\t d  e \n\t\n f \n'   > "$TMP/ws.txt"
printf 'a\tb\tc\nd\te\tf\n'               > "$TMP/tabs.txt"
printf 'a b\r\nb c\r\n'                   > "$TMP/crlf.txt"
printf '\n\n\t \n'                        > "$TMP/blanks.txt"
printf 'ab abc\na abcd\n'                 > "$TMP/prefix.txt"
printf 'ZZ a\nzz A\na z\n'                > "$TMP/case.txt"
mkdir -p "$TMP/adir"
python3 - <<'PY' > "$TMP/chain12.txt"
for c in "abcdefghijk":
    print(c, chr(ord(c) + 1))
PY
python3 - <<'PY' > "$TMP/bigdag.txt"
import random
random.seed(20260925)
names = ["n%03d" % i for i in range(200)]
out = []
for i in range(200):
    for j in range(i + 1, min(i + 4, 200)):
        if random.random() < 0.7:
            out.append("%s %s" % (names[i], names[j]))
random.shuffle(out)
print("\n".join(out))
PY
python3 - <<'PY' > "$TMP/bigcyc.txt"
import random
random.seed(20260926)
names = ["m%03d" % i for i in range(150)]
out = []
for i in range(150):
    for j in range(i + 1, min(i + 3, 150)):
        if random.random() < 0.6:
            out.append("%s %s" % (names[i], names[j]))
# one back edge => exactly one cycle, plus a second small one
out.append("%s %s" % (names[100], names[10]))
out.append("%s %s" % (names[77], names[76]))
random.shuffle(out)
print("\n".join(out))
PY
python3 - <<'PY' > "$TMP/binjunk.bin"
import sys
sys.stdout.buffer.write(b"\x01\x02 x\n\xff\xfe y\ny \x03\x04\n")
sys.stdout.buffer.write(b"a\x00b c\n")
sys.stdout.buffer.write(b"\x7f\x80 \x81\x82\n")
PY
# default stdin fixture (rewritten per case with set_stdin)
printf 'a b\nb c\nc d\n' > "$TMP/in.txt"
set_stdin() { printf '%b' "$1" > "$TMP/in.txt"; }

# Loose-mode arbiter: is $2 a valid topological ordering of the token
# stream in $1?  (Every node exactly once, every input relation respected.)
cat > "$TMP/topocheck.py" <<'PY'
import re, sys
inp = open(sys.argv[1], 'rb').read()
out = open(sys.argv[2], 'rb').read()
toks = [t for t in re.split(rb'[ \t\n]+', inp) if t]
pairs = [(toks[i], toks[i + 1]) for i in range(0, len(toks) - 1, 2)]
lines = out.split(b'\n')
if lines and lines[-1] == b'':
    lines.pop()
nodes = set(toks)
if len(lines) != len(nodes) or len(lines) != len(set(lines)) or set(lines) != nodes:
    sys.exit(1)
pos = {l: i for i, l in enumerate(lines)}
for u, v in pairs:
    if u != v and pos[u] >= pos[v]:
        sys.exit(1)
sys.exit(0)
PY

# --- harness ---------------------------------------------------------------

# Run one case for one implementation.  $1 = args string (word-split);
# $2 = input spec: "-" (stdin only), "file:NAME" (FILE operand) or
# "file:-" (literal "-" operand with stdin).  Results land in
# $TMP/{out,err,rc}.
run_one() {
    local args_str="$1" spec="$2" impl="$3"
    local args=()
    if [ -n "$args_str" ]; then read -r -a args <<< "$args_str"; fi
    local cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a tsort "$0" "$@"' "$TSORT_HOST")
    else
        cmd=(bash -c 'exec -a tsort "$0" "$@"' "$REF")
    fi
    case "$spec" in
        "-")
            "${cmd[@]}" "${args[@]}" <"$TMP/in.txt" >"$TMP/out" 2>"$TMP/err"
            ;;
        "file:-")
            "${cmd[@]}" "${args[@]}" - <"$TMP/in.txt" >"$TMP/out" 2>"$TMP/err"
            ;;
        file:*)
            "${cmd[@]}" "${args[@]}" "$TMP/${spec#file:}" >"$TMP/out" 2>"$TMP/err"
            ;;
    esac
    echo "$?" > "$TMP/rc"
}

# cmp_case <level> <flags> <args string> <input spec>
#   flags: "" or "modern" (skip in loose mode: documented divergence)
cmp_case() {
    local level="$1" flags="$2" args_str="$3" spec="$4"
    if [ "$REF_MODE" = loose ] && [ "$flags" = modern ]; then
        N_SKIPPED=$((N_SKIPPED + 1))
        return
    fi
    N=$((N + 1))
    if [ "$level" = OUTRC ]; then
        N_OUTRC=$((N_OUTRC + 1))
    fi

    run_one "$args_str" "$spec" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")

    local infile="$TMP/in.txt"
    case "$spec" in
        file:-) ;;
        file:*) infile="$TMP/${spec#file:}" ;;
    esac
    local desc="tsort $args_str [$spec]"

    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        if [ "$REF_MODE" = loose ] && python3 "$TMP/topocheck.py" "$infile" "$TMP/oh" 2>/dev/null; then
            N_ORDER_NOTES=$((N_ORDER_NOTES + 1))
            echo "note: stdout order differs from the modern reference but is a valid topological order: $desc"
        else
            echo "FAIL: stdout differs: $desc"
            echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
            echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
            fail=1
        fi
    fi

    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: $desc"
            fail=1
        fi
    fi

    if [ "$level" = FULL ] && [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs: $desc"
        echo "  hb:  $(cat "$TMP/eh" | tr '\n' '|')"
        echo "  ref: $(cat "$TMP/err" | tr '\n' '|')"
        fail=1
    fi
}

# --- corpus: simple chains -------------------------------------------------
cmp_case FULL "" "" "file:pair.txt"
cmp_case FULL "" "" "file:chain3.txt"
cmp_case FULL "" "" "file:chain12.txt"
cmp_case FULL "" "" "file:chain3rev.txt"
cmp_case FULL "" "" "file:twochains.txt"
set_stdin 'a b\nb c\nc d\n'
cmp_case FULL "" "" "-"
set_stdin 'c d\nd e\nb c\na b\n'
cmp_case FULL "" "" "-"

# --- corpus: diamond graphs ------------------------------------------------
cmp_case FULL "" "" "file:diamond.txt"
cmp_case FULL "" "" "file:diamond_rev.txt"
cmp_case FULL "" "" "file:diamond_x.txt"
cmp_case FULL "" "" "file:diamond_dup.txt"
cmp_case FULL "" "" "file:diamond_chain.txt"
set_stdin 'a b\na c\nb d\nc d\n'
cmp_case FULL "" "" "-"
set_stdin 'd e\na c\nb d\na b\nc d\n'
cmp_case FULL "" "" "-"

# --- corpus: cycles (stderr lists the loop members; exit code 1) -----------
cmp_case FULL "" "" "file:cycle2.txt"
set_stdin 'a b\nb a\n'
cmp_case FULL "" "" "-"
cmp_case FULL "" "" "file:cycle3.txt"
set_stdin 'a b\nb c\nc a\n'
cmp_case FULL "" "" "-"
cmp_case FULL "" "" "file:cycle4.txt"
cmp_case FULL "" "" "file:cycletail.txt"
cmp_case FULL "" "" "file:twocycles.txt"
cmp_case FULL "" "" "file:cycin.txt"
cmp_case FULL "" "" "file:cycleplus.txt"
cmp_case FULL "" "" "file:dupcycle.txt"
cmp_case FULL "" "" "file:selfloop.txt"
cmp_case FULL "" "" "file:selfloop2.txt"
set_stdin 'w x\nx y\ny z\nz w\n'
cmp_case FULL "" "-" "file:-"
cmp_case FULL "" "" "file:cycletail.txt"

# --- corpus: odd token counts (2.1: no diagnostic, dangling token sorts) ---
cmp_case FULL modern "" "file:odd3.txt"
set_stdin 'a b c\n'
cmp_case FULL modern "" "-"
cmp_case FULL modern "" "file:odd5.txt"
cmp_case FULL modern "" "file:oddnonl.txt"
cmp_case FULL modern "" "file:oddloop.txt"
cmp_case FULL modern "" "file:onetok.txt"
set_stdin 'a b c\n'
cmp_case FULL modern "" "file:-"

# --- corpus: empty and whitespace-only input -------------------------------
cmp_case FULL "" "" "file:empty.txt"
set_stdin ''
cmp_case FULL "" "" "-"
cmp_case FULL "" "" "file:blanks.txt"
cmp_case FULL "" "-" "file:-"

# --- corpus: repeated edges / repeated tokens ------------------------------
cmp_case FULL "" "" "file:dupedges.txt"
cmp_case FULL "" "" "file:dupchainedge.txt"
set_stdin 'x y\nx y\n'
cmp_case FULL "" "" "-"
set_stdin 'a b\nb c\na b\nb c\n'
cmp_case FULL "" "" "-"
set_stdin 'a b a b\n'
cmp_case FULL "" "" "-"

# --- corpus: whitespace variants -------------------------------------------
# NB: CR is NOT a delimiter in 2.1 (DELIM is " \t\n"), so "b\r" is a token of
# its own here; modern references treat \r as whitespace, which the loose run
# reports as a valid-order note rather than a failure.
cmp_case FULL "" "" "file:ws.txt"
cmp_case FULL "" "" "file:tabs.txt"
cmp_case FULL "" "" "file:crlf.txt"
set_stdin '\n\n  \t\n a b\n'
cmp_case FULL "" "" "-"
set_stdin 'a\t\tb'
cmp_case FULL "" "" "-"
set_stdin 'a   b    c\nd'
cmp_case FULL "" "" "-"
set_stdin '   a  b\n\tc\td\t\n'
cmp_case FULL "" "" "-"

# --- corpus: token ordering edge cases -------------------------------------
cmp_case FULL "" "" "file:prefix.txt"
cmp_case FULL "" "" "file:case.txt"
set_stdin '0 1\n1 2\n2 3\n'
cmp_case FULL "" "" "-"
set_stdin 'aa bb\n'
cmp_case FULL "" "" "-"
set_stdin 'AB ab\nab AB\n'
cmp_case FULL "" "" "-"

# --- corpus: stdin / FILE plumbing -----------------------------------------
cmp_case FULL "" "" "file:-"
cmp_case FULL "" "-" "-"
cmp_case FULL "" "--" "file:chain3.txt"
set_stdin 'a b\nb c\n'
cmp_case FULL "" "--" "-"
cmp_case FULL "" "" "file:nonexistent_xyz.txt"
cmp_case FULL "" "--" "file:nonexistent_xyz.txt"
cmp_case FULL "" "file:pair.txt file:chain3.txt" "-"
cmp_case FULL "" "file:nonexistent_xyz.txt file:chain3.txt" "-"
cmp_case FULL "" "file:adir" "-"
cmp_case FULL modern "" "file:adir" "-"

# --- corpus: options -------------------------------------------------------
set_stdin ''
cmp_case FULL modern "--help" "-"
cmp_case FULL modern "--version" "-"
cmp_case OUTRC "" "-x" "-"
cmp_case OUTRC "" "--bogus" "-"
cmp_case OUTRC modern "-h" "-"
# Not tested here (documented adaptation, see the port's HobbyOS note):
# `tsort --help FILE` / `--version FILE` — 2.1's parse_long_options only
# honors them when argc == 2, then rejects the option; the port follows the
# repo's getopt-macro pattern (like cut/head/tail/nl) and prints usage(0).
echo "note: '--help FILE'/'--version FILE' not compared (2.1 argc==2-only quirk, see port note)"

# --- corpus: large / binary -------------------------------------------------
cmp_case FULL "" "" "file:bigdag.txt"
cmp_case FULL "" "" "file:bigcyc.txt"
# modern references decode input as UTF-8 and refuse non-text bytes; 2.1
# (bytes in, bytes out) is the model the port follows, so this one is
# strict-only.
cmp_case FULL modern "" "file:binjunk.bin"
set_stdin 'p q\nq r\nr s\ns p\np t\nt u\n'
cmp_case FULL "" "" "-"

if [ "$fail" = 0 ]; then
    echo "tsort parity: PASS ($N cases vs $REF; $N_OUTRC stdout+rc only; $N_SKIPPED modern-divergence skips; $N_ORDER_NOTES valid-order notes)"
    exit 0
fi
echo "tsort parity: FAIL (of $N cases)"
exit 1
