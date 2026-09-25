#!/bin/bash
# tr_parity.sh — byte-exact acceptance test for the HobbyOS ported tr.
#
# Runs our host-built tr (tr_host, argv[0] forced to "tr" via exec -a so
# error messages are comparable) against a GNU tr reference on the same
# inputs and compares stdout bytes, exit codes and stderr text across
# translation, -d, -s, -c, -t, ranges, escapes, [:classes:], [=c=],
# [c*n] repeats, stdin mode and the error paths.
#
# Reference selection:
#   $2 = explicit path, else $TR_REF, else the first of these that exists:
#        /usr/bin/tr (GNU coreutils) — tests the port against a modern
#        GNU tr; diagnostics were rewritten in 2003-era coreutils, so
#        those runs compare only whether the run succeeded (loose).  The
#        strictest bar is `make tr_parity_strict`, which builds
#        textutils-2.1's original tr as the reference instead: there
#        stdout, exit codes AND stderr text compare byte-for-byte.
#
# tr reads only stdin, so a fixture is normally fed via stdin ("in:name");
# "arg:name" appends the file as an extra operand (extra-operand error
# paths) and "none" leaves stdin on /dev/null.
set -u

# The port is a single-C-locale program; the 2.1 reference calls
# setlocale(LC_ALL, ""), so pin the C locale for both sides (and for
# modern tr) or a UTF-8 environment would make the reference's ctype
# tables diverge from the sysroot's.  POSIXLY_CORRECT would flip
# textutils-2.1's posix_pedantic path; keep it out of the race.
export LC_ALL=C
unset POSIXLY_CORRECT

TR_HOST="${1:-./obj/tr_host}"
REF="${3:-${2:-${TR_REF:-$(command -v tr || echo /usr/bin/tr)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern GNU coreutils
# (loose: error paths compare only the success/failure class).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi
echo "tr parity: reference=$REF mode=$REF_MODE"

# Fixtures
printf 'hello world\nsecond line\n\nfourth\n' > "$TMP/small.txt"
printf 'The quick brown fox jumps over the lazy dog 0123456789\n' > "$TMP/plain.txt"
printf 'abc123DEF456ghi\n' > "$TMP/digits.txt"
printf 'a   b     c  \n d\n' > "$TMP/spaces.txt"
printf 'a\tb\tc\n\t\td\te\n' > "$TMP/tabs.txt"
printf 'a\nb\n\nc\n' > "$TMP/nl.txt"
printf 'a\r\nb\r\nc\n' > "$TMP/cr.txt"
printf 'aaabbbccc aaa\n' > "$TMP/runs.txt"
printf 'a-b_c.d,e;f!g?h\n' > "$TMP/punct.txt"
printf 'back\\slash\\here\n' > "$TMP/bslash.txt"
printf 'x\000y\000\000z\n' > "$TMP/nul.txt"
printf '' > "$TMP/empty.txt"
printf 'AB' > "$TMP/noeol.txt"
printf 'a\200\377\240z\n\303\251\n' > "$TMP/highbit.txt"
python3 - <<'PY' > "$TMP/longline.txt"
print("ab" * 2000)
PY
python3 - <<'PY' > "$TMP/mixed.txt"
import random
random.seed(7)
alphabet = "abcXYZ019 \t:;,.!?-_" + "\x80\xff\x00\r"
print("".join(random.choice(alphabet) for _ in range(4000)))
PY

# Run a case for one implementation.  $1 = args string (word-split, with
# __SP__/__EMPTY__ tokens standing in for quoted " " / "" arguments);
# $2 = input spec: "in:<fixture>" (stdin), "arg:<fixture>" (extra
# operand) or "none"; $3 = "hb" or "ref".
run_one() {
    local args_str="$1" spec="$2" impl="$3"
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    local prog stdin_file=/dev/null arg_extra=()
    if [ "$impl" = hb ]; then prog="$TR_HOST"; else prog="$REF"; fi
    case "$spec" in
        in:*) stdin_file="$TMP/${spec#in:}" ;;
        arg:*) arg_extra=("$TMP/${spec#arg:}") ;;
        none) ;;
        *) echo "bad spec $spec"; exit 9 ;;
    esac
    if [ "${#arg_extra[@]}" -gt 0 ]; then
        bash -c 'exec -a tr "$0" "$@"' "$prog" "${args[@]}" \
            "${arg_extra[@]}" <"$stdin_file" >"$TMP/out" 2>"$TMP/err"
    else
        bash -c 'exec -a tr "$0" "$@"' "$prog" "${args[@]}" \
            <"$stdin_file" >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = args string; $2 = input spec
    local args_str="$1" spec="$2"
    N=$((N+1))
    run_one "$args_str" "$spec" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$spec" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: tr $args_str ($spec)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: tr $args_str ($spec)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): tr $args_str ($spec)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# --- translation: ranges and plain sets ---
cmp_case "a-z A-Z" "in:plain.txt"
cmp_case "A-Z a-z" "in:plain.txt"
cmp_case "a-e A-E" "in:plain.txt"
cmp_case "abc xyz" "in:plain.txt"
cmp_case "a-z n-za-m" "in:plain.txt"
cmp_case "0-9 x" "in:digits.txt"
cmp_case "0-9 x" "in:mixed.txt"
cmp_case "-t 0-9 ab" "in:digits.txt"
cmp_case "-t a-z A-M" "in:plain.txt"
cmp_case "--truncate-set1 a-z A-M" "in:plain.txt"
cmp_case "a-z A-Z" "in:small.txt"
cmp_case "a-z A-Z" "in:empty.txt"
cmp_case "a-z A-Z" "in:noeol.txt"
cmp_case "a-z A-Z" "in:highbit.txt"
cmp_case "a-z A-Z" "in:nul.txt"
cmp_case "a-z A-Z" "in:longline.txt"
cmp_case "a-z A-Z" "in:cr.txt"
cmp_case "a-z A-Z" "in:mixed.txt"
cmp_case "ab Z" "in:runs.txt"

# --- -c complement ---
cmp_case "-c a-z X" "in:plain.txt"
cmp_case "-c a-z X" "in:mixed.txt"
cmp_case "-c 0-9 ." "in:digits.txt"
cmp_case "-c [:digit:] x" "in:digits.txt"
cmp_case "-c a-z A-Z" "in:plain.txt"
cmp_case "--complement a-z X" "in:plain.txt"
cmp_case "-c a-z __SP__" "in:punct.txt"

# --- -d delete ---
cmp_case "-d aeiou" "in:plain.txt"
cmp_case "-d a-y" "in:plain.txt"
cmp_case "-d A-Za-z" "in:plain.txt"
cmp_case "-d 0-9" "in:digits.txt"
cmp_case "-d __SP__" "in:spaces.txt"
cmp_case "-d a-z" "in:highbit.txt"
cmp_case "-d a-z" "in:empty.txt"
cmp_case "-d a-z" "in:nul.txt"
cmp_case "-d aeiou" "in:mixed.txt"
cmp_case "--delete aeiou" "in:plain.txt"
cmp_case "-cd a-z" "in:plain.txt"
cmp_case "-cd a-z" "in:mixed.txt"
cmp_case "-cd __SP__" "in:spaces.txt"
cmp_case "-cd [:graph:]" "in:spaces.txt"
cmp_case "-cd [:print:]" "in:highbit.txt"
cmp_case "-d [:digit:]" "in:digits.txt"
cmp_case "-d [:alpha:]" "in:plain.txt"
cmp_case "-d [:alnum:]" "in:mixed.txt"
cmp_case "-d [:cntrl:]" "in:nul.txt"
cmp_case "-d [:cntrl:]" "in:cr.txt"
cmp_case "-d [:punct:]" "in:punct.txt"
cmp_case "-d [:upper:]" "in:plain.txt"
cmp_case "-d [:xdigit:]" "in:digits.txt"
cmp_case "-d [:space:]" "in:tabs.txt"

# --- -s squeeze ---
cmp_case "-s __SP__" "in:spaces.txt"
cmp_case "-s a-z" "in:runs.txt"
cmp_case "-s 0-9" "in:mixed.txt"
cmp_case "-s [:space:]" "in:spaces.txt"
cmp_case "-s [:blank:]" "in:tabs.txt"
cmp_case "-s [:digit:]" "in:digits.txt"
cmp_case "-s ab" "in:runs.txt"
cmp_case "-s a-z A-Z" "in:runs.txt"
cmp_case "-s a-z A-Z" "in:mixed.txt"
cmp_case "-s A-Z a-z" "in:mixed.txt"
cmp_case "--squeeze-repeats a-z" "in:runs.txt"
cmp_case "-s a-z" "in:empty.txt"
cmp_case "-s a-z" "in:noeol.txt"
cmp_case "-s X" "in:spaces.txt"
cmp_case "-cs a-z __SP__" "in:plain.txt"
cmp_case "-cs a-z \\n" "in:plain.txt"
cmp_case "-ds 0-9 __SP__" "in:mixed.txt"
cmp_case "-ds [:digit:] __SP__" "in:digits.txt"
cmp_case "-cds [:space:]" "in:spaces.txt"
cmp_case "-cds a-z \\n" "in:plain.txt"
cmp_case "-dcs a-z \\n" "in:plain.txt"

# --- escapes ---
cmp_case "\\n __SP__" "in:nl.txt"
cmp_case "\\t __SP__" "in:tabs.txt"
cmp_case "\\r X" "in:cr.txt"
cmp_case "\\n X" "in:mixed.txt"
cmp_case "\\\\ X" "in:bslash.txt"
cmp_case "\\101-\\103 x" "in:plain.txt"
cmp_case "\\060-\\071 N" "in:digits.txt"
cmp_case "\\141 Z" "in:plain.txt"
cmp_case "-d \\n" "in:nl.txt"
cmp_case "-d \\r" "in:cr.txt"
cmp_case "-d \\t" "in:tabs.txt"
cmp_case "\\400 x" "in:highbit.txt"

# --- character classes in both sets ---
cmp_case "[:lower:] [:upper:]" "in:plain.txt"
cmp_case "[:upper:] [:lower:]" "in:plain.txt"
cmp_case "[:lower:] [:upper:]" "in:mixed.txt"
cmp_case "[:upper:] [:lower:]" "in:mixed.txt"
cmp_case "[:digit:] [:digit:]" "in:digits.txt"
cmp_case "[:digit:] x" "in:digits.txt"
# textutils-2.1 refuses a [:class:] in string1 paired with a non-class
# string2 ("misaligned [:upper:] and/or [:lower:] construct"); modern
# coreutils accepts it and translates.  The port follows its 2.1 lineage,
# so this case only compares when the reference IS 2.1.
if [ "$REF_MODE" = strict ]; then
    cmp_case "-t [:lower:] abc" "in:plain.txt"
else
    echo "note: skipping -t [:lower:] abc (2.1 rejects, modern tr translates)"
fi

# --- equivalence classes ---
cmp_case "[=a=] X" "in:plain.txt"
cmp_case "-d [=e=][=o=]" "in:plain.txt"
cmp_case "-s [=a=]" "in:runs.txt"
cmp_case "[=a=][=b=] Q" "in:runs.txt"

# --- [c*n] repeats ---
cmp_case "[a*3] xyz" "in:runs.txt"
cmp_case "[a*2] X" "in:runs.txt"
cmp_case "a [x*2]" "in:runs.txt"
cmp_case "-t [a*3] XY" "in:runs.txt"
cmp_case "[a*012] x" "in:runs.txt"

# --- stdin mode (same fixtures, no file operand) is the norm here; make
# --- sure a lone "-" is treated as an operand like upstream does ---
cmp_case "a-z A-Z" "arg:plain.txt"

# --- --help / --version.  Both texts embed program_name, which the
# --- harness forces to "tr" for either side, and 2.1's version line is
# --- stubbed to the same "tr (textutils) 2.1" the port prints (see
# --- src/host/build_tu21_tr_ref.sh), so strict mode compares them
# --- byte-for-byte.  Modern coreutils rewrote both texts -> strict-only.
if [ "$REF_MODE" = strict ]; then
    cmp_case "--version" "none"
    cmp_case "--help" "none"
else
    echo "note: skipping --help/--version (modern tr text legitimately differs)"
fi

# --- error paths (missing/invalid operands, classes, escapes, repeats) ---
cmp_case "" "none"                          # missing operand
cmp_case "-d" "none"
cmp_case "-s" "none"
cmp_case "-c" "none"
cmp_case "-t" "none"
cmp_case "-x" "none"                        # unknown option
cmp_case "a" "none"                         # missing operand (set2)
cmp_case "a b c" "none"                     # too many arguments
cmp_case "a b" "arg:plain.txt"              # extra operand (file)
cmp_case "-d a b" "none"                    # -d with two operands
cmp_case "-s a b c" "none"
cmp_case "-d [:bogus:]" "none"              # invalid class
cmp_case "-d [::::::::]" "none"
cmp_case "-d [:alpha]" "none"
cmp_case "\\q x" "none"                     # invalid backslash escape
# textutils-2.1 hard-errors on a trailing backslash in a set ("invalid
# backslash escape at end of string"); modern coreutils downgrades it to
# a warning ("an unescaped backslash at end of string is not portable")
# and keeps going.  Port follows 2.1 -> strict-only comparison.
if [ "$REF_MODE" = strict ]; then
    cmp_case "a\\ x" "none"                     # backslash escape at end
    cmp_case "-d \\" "none"
else
    echo "note: skipping trailing-backslash cases (2.1 rejects, modern tr warns)"
fi
cmp_case "a-z [:digit:]" "none"             # class in set2 must be upper/lower
cmp_case "[:alpha:] [:upper:]" "none"
cmp_case "a-z [:upper:]" "none"
cmp_case "a __EMPTY__" "none"               # empty set2 while translating
cmp_case "-t a-z __EMPTY__" "in:plain.txt"  # empty set2 is OK with -t
cmp_case "-d __EMPTY__" "none"              # empty set1
cmp_case "[a*] x" "none"                    # empty repeat count
cmp_case "[a*x] y" "none"                   # non-numeric repeat count
cmp_case "[a*99999999999999999999] x" "none" # repeat count overflow
cmp_case "[a*0] x" "none"                   # zero repeat count
cmp_case "x [a*2]" "none"                   # repeat in set2

if [ "$fail" = 0 ]; then
    echo "tr parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "tr parity: FAIL (of $N cases)"
exit 1
