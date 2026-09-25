#!/bin/bash
# cut_parity.sh — byte-exact acceptance test for the HobbyOS ported cut.
#
# Runs our host-built cut (cut_host, argv[0] forced to "cut" via exec -a
# so error messages are comparable) against a GNU cut reference on the
# same inputs and compares stdout bytes, exit codes and stderr text
# across byte/field/character modes, list grammar, delimiters, -s,
# --output-delimiter, stdin mode, multi-file mode and error paths.
#
# Reference selection:
#   $2 = explicit path, else $CUT_REF, else the first of these that exists:
#        /usr/bin/cut (GNU coreutils) — tests the port against a modern
#        GNU cut; its rewritten --help/--version text is not compared in
#        loose mode.  The strictest bar is `make cut_parity_strict` which
#        builds textutils-2.1's original cut as the reference instead;
#        in strict mode --help and --version ARE raced byte-exact (they
#        caught a real bug: a braced GETOPT_HELP_OPTION_DECL silently
#        disabled both options).
set -u

CUT_HOST="${1:-./obj/cut_host}"
REF="${3:-${2:-${CUT_REF:-$(command -v cut || echo /usr/bin/cut)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern GNU coreutils
# (loose: diagnostics were rewritten in 2003-era coreutils, so error
# paths compare only whether the run succeeded, not the wording).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# Fixtures
printf 'hello world\nsecond line\n\nfourth\n' > "$TMP/small.txt"
printf 'a\tb\tc\td\n' > "$TMP/tabs.txt"
printf 'one:two:three\nfour:five\nnosep\n\na:b\n' > "$TMP/colon.txt"
printf '  lead  trail  \nmid dle\n' > "$TMP/spaces.txt"
printf 'x:y:z\n1:2:3\n::\n:only\n' > "$TMP/edge.txt"
printf '' > "$TMP/empty.txt"
printf 'noeol:end' > "$TMP/noeol.txt"
printf 'aa:bb:cc' > "$TMP/nonl.txt"
python3 - <<'PY' > "$TMP/big.txt"
import random
random.seed(11)
for i in range(3000):
    n = random.randint(0, 8)
    fields = []
    for _ in range(n):
        fields.append("".join(random.choice("abcXYZ019 ,") for _ in range(random.randint(1,6))).replace(",","").strip() or "f")
    print(":".join(fields))
PY
printf 'bytes with spaces and\x01ctl chars\n' > "$TMP/binish.txt"

HB() { exec -a cut "$CUT_HOST" "$@"; }

# Run a case for one implementation.  $1 = args string (word-split, with
# __SP__/__EMPTY__ tokens standing in for quoted " " / "" arguments);
# $2 = input mode: "file:<name>" or "-" or "file:-" (stdin).
run_one() {
    local args_str="$1" file="$2" impl="$3"
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    local inredir=""
    if [ "$file" = "-" ]; then
        inredir="$TMP/small.txt"
    else
        local name="${file#file:}"
        [ "$name" = "-" ] && name="-"
        args_extra=("$TMP/$name")
    fi
    local cmd
    if [ "$impl" = "hb" ]; then
        cmd=(bash -c 'exec -a cut "$0" "$@"' "$CUT_HOST")
    else
        cmd=(bash -c 'exec -a cut "$0" "$@"' "$REF")
    fi
    if [ "$file" = "-" ]; then
        "${cmd[@]}" "${args[@]}" <"$TMP/small.txt" >"$TMP/out" 2>"$TMP/err"
    elif [ "$file" = "file:-" ]; then
        "${cmd[@]}" "${args[@]}" - <"$TMP/small.txt" >"$TMP/out" 2>"$TMP/err"
    else
        "${cmd[@]}" "${args[@]}" "$TMP/${file#file:}" >"$TMP/out" 2>"$TMP/err"
    fi
    echo "$?" > "$TMP/rc"
}

cmp_case() {  # $1 = args string; $2 = input spec
    local args_str="$1" file="$2"
    N=$((N+1))
    run_one "$args_str" "$file" hb
    local rc_hb; rc_hb=$(<"$TMP/rc")
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    run_one "$args_str" "$file" ref
    local rc_ref; rc_ref=$(<"$TMP/rc")
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: cut $args_str ($file)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: cut $args_str ($file)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): cut $args_str ($file)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# --- byte/char modes: -b / -c lists ---
BLISTS=("1" "2" "-3" "1,3" "2-4" "1,3-5" "-2,4-" "5-" "1-" "1-1" "3-2" " 1 , 2 ")
for lst in "${BLISTS[@]}"; do
    cmp_case "-b $lst" "file:small.txt"
    cmp_case "-c $lst" "file:small.txt"
done
# Empty elements ("1,,2") are accepted by textutils-2.1 (as "1,2") but
# rejected by modern coreutils ("fields are numbered from 1"-era
# tightening); the port follows its 2.1 lineage, so this pair only
# compares when the reference IS 2.1.
if [ "$REF_MODE" = strict ]; then
    cmp_case "-b 1,,2" "file:small.txt"
    cmp_case "-c 1,,2" "file:small.txt"
else
    echo "note: skipping 1,,2 list cases (2.1 accepts, modern cut rejects)"
fi
cmp_case "-b 1-3,7-" "file:tabs.txt"
cmp_case "-c 10-" "file:oneline-absent"

# --- field mode: -f lists, default TAB delimiter ---
FLISTS=("1" "2" "-2" "2-" "1,3" "2-3" "1-4" "4" "5" "1-2,4-")
for lst in "${FLISTS[@]}"; do
    cmp_case "-f $lst" "file:tabs.txt"
done

# --- custom delimiters ---
for lst in "1" "2" "-2" "2-3" "1,3-"; do
    cmp_case "-d : -f $lst" "file:colon.txt"
    cmp_case "-d: -f $lst" "file:colon.txt"
done
cmp_case "-d : -f 1,3-" "file:edge.txt"
cmp_case "-d : -f 2" "file:noeol.txt"
cmp_case "-d : -f 2" "file:nonl.txt"
cmp_case "-d __SP__ -f 1" "file:spaces.txt"
cmp_case "-d __SP__ -f 2,4" "file:spaces.txt"
cmp_case "-d __EMPTY__ -f 1" "file:small.txt"

# --- -s (only-delimited) ---
for lst in "1" "2" "2-" "1,2"; do
    cmp_case "-d : -s -f $lst" "file:colon.txt"
done
cmp_case "-d : -s -f 2" "file:edge.txt"
cmp_case "-s -f 2" "file:tabs.txt"

# --- --output-delimiter ---
cmp_case "-d : -f 1- --output-delimiter='|'" "file:colon.txt"
cmp_case "-d : -f 1,3 --output-delimiter='XX'" "file:colon.txt"
cmp_case "--output-delimiter='-' -f 1,3" "file:tabs.txt"
cmp_case "-d : -s -f 2- --output-delimiter=''" "file:colon.txt"

# --- -n (ignored) and combinations ---
cmp_case "-n -b 1-2" "file:small.txt"
cmp_case "-nb 1,3" "file:small.txt"
cmp_case "-b 1-3 -" "file:-"
cmp_case "-f 2 -" "file:-"
cmp_case "-d : -f 1,2" "file:big.txt"
cmp_case "-b 1-20" "file:big.txt"
cmp_case "-d : -f 2-4" "file:big.txt"
cmp_case "-b 1-4" "file:empty.txt"
cmp_case "-f 1" "file:empty.txt"
cmp_case "-b 1-30" "file:binish.txt"

# --- stdin mode ---
cmp_case "-b 1-5" "-"
cmp_case "-f 1" "-"
cmp_case "-d __SP__ -f 1" "-"
cmp_case "-d __SP__ -s -f 2- --output-delimiter=x" "-"

# --- multi-file ---
N=$((N+1))
bash -c 'exec -a cut "$0" "$@"' "$CUT_HOST" -d : -f 1 "$TMP/colon.txt" "$TMP/edge.txt" >"$TMP/oh" 2>/dev/null
bash -c 'exec -a cut "$0" "$@"' "$REF" -d : -f 1 "$TMP/colon.txt" "$TMP/edge.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: multi-file output differs"; fail=1
fi
N=$((N+1))
bash -c 'exec -a cut "$0" "$@"' "$CUT_HOST" -b 1-3 "$TMP/small.txt" "$TMP/empty.txt" "$TMP/tabs.txt" >"$TMP/oh" 2>/dev/null
bash -c 'exec -a cut "$0" "$@"' "$REF" -b 1-3 "$TMP/small.txt" "$TMP/empty.txt" "$TMP/tabs.txt" >"$TMP/out" 2>/dev/null
if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
    echo "FAIL: multi-file byte-mode output differs"; fail=1
fi

# --- error paths (exit 2 + stderr) ---
cmp_case "" "file:small.txt"                    # no list
cmp_case "-b 1 -c 1" "file:small.txt"           # two list types
cmp_case "-b 1 -f 1" "file:small.txt"
cmp_case "-b" "file:small.txt"
cmp_case "-d xy -f 1" "file:small.txt"          # multi-char delimiter
cmp_case "-d : -b 1" "file:small.txt"           # delimiter w/o field mode
cmp_case "-s -b 1" "file:small.txt"             # -s w/o field mode
cmp_case "-b 1" "file:file:nonexistent_xyz.txt" # missing file (rc 1)
cmp_case "-f x" "file:small.txt"                # non-numeric list
cmp_case "-f 1-2-3" "file:small.txt"            # malformed range
cmp_case "-b 3-1" "file:small.txt"              # inverted range

# --- --help / --version: raced byte-exact against the strict 2.1
#     reference only (modern coreutils rewrote both texts) ---
if [ "$REF_MODE" = strict ]; then
    for opt in --help --version; do
        N=$((N+1))
        bash -c 'exec -a cut "$0" "$@"' "$CUT_HOST" "$opt" >"$TMP/oh" 2>"$TMP/eh"
        local_rc_hb=$?
        bash -c 'exec -a cut "$0" "$@"' "$REF" "$opt" >"$TMP/out" 2>"$TMP/err"
        local_rc_ref=$?
        if ! SAME_OUT "$TMP/oh" "$TMP/out" || [ "$local_rc_hb" != "$local_rc_ref" ]; then
            echo "FAIL: $opt differs (rc $local_rc_hb vs $local_rc_ref)"
            fail=1
        fi
    done
else
    echo "note: skipping --help/--version cases (modern text differs; strict ref races them)"
fi

if [ "$fail" = 0 ]; then
    echo "cut parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "cut parity: FAIL (of $N cases)"
exit 1
