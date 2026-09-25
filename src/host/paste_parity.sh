#!/bin/bash
# paste_parity.sh — byte-exact acceptance test for the HobbyOS ported paste.
#
# Runs our host-built paste (paste_host, argv[0] forced to "paste" via
# exec -a so error messages and usage text are comparable) against a GNU
# paste reference on the same inputs and compares stdout bytes, exit codes
# and stderr text across default parallel merging, -s serial mode, -d
# delimiter lists (cycling, escapes, EMPTY_DELIM), stdin ("-" and no-FILE)
# mode, empty/no-EOL/long/wide fixtures and error paths.
#
# Reference selection:
#   $2 = explicit path, else $PASTE_REF, else the first of these that
#   exists: /usr/bin/paste (GNU coreutils) — tests the port against a
#   modern GNU paste; the --help/--version text legitimately differs
#   (2.1 vs coreutils) and those cases are skipped in loose mode.  The
#   strictest bar is `make paste_parity_strict`, which builds
#   textutils-2.1's original paste as the reference instead.
set -u

PASTE_HOST="${1:-./obj/paste_host}"
REF="${3:-${2:-${PASTE_REF:-$(command -v paste || echo /usr/bin/paste)}}}"
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

# --- fixtures ---------------------------------------------------------
printf 'a1\na2\na3\n' > "$TMP/a.txt"
printf 'b1\nb2\n' > "$TMP/b.txt"
printf 'c1\n\nc3\nc4\n' > "$TMP/c.txt"          # one empty line inside
printf '' > "$TMP/empty.txt"
printf 'x1\nx2' > "$TMP/noeol.txt"              # no trailing newline
printf 'z1\nz2' > "$TMP/noeol2.txt"
printf 't1\tt2\tt3\n' > "$TMP/tabs.txt"
printf 'l1\n' > "$TMP/blank1.txt"
python3 - <<'PY' > "$TMP/longline.txt"
print("L" * 3000)
print("short")
PY
python3 - <<'PY' > "$TMP/many.txt"
for i in range(1000):
    print("m%d" % i)
PY
python3 - <<'PY' > "$TMP/big.txt"
import random
random.seed(7)
for i in range(1500):
    n = random.randint(0, 5)
    print("".join(random.choice("abcXYZ019 ") for _ in range(n)))
PY
printf 's1\ns2\ns3\ns4\ns5\n' > "$TMP/stdin.txt"   # always the stdin content
mkdir -p "$TMP/adir"                                # a directory (read error)
printf 'secret\n' > "$TMP/noperm.txt"; chmod 000 "$TMP/noperm.txt"

# Run a case for one implementation.
#   $1 = args string (word-split with read -r, so backslashes survive for
#        the -d escape cases; __SP__ / __EMPTY__ tokens stand in for a
#        quoted " " / "" argument);
#   $2 = input spec: "-" means "no FILE arguments at all, stdin is
#        stdin.txt"; otherwise a list of f:NAME tokens appended as file
#        arguments (f:- gives a literal "-" argument).  stdin is always
#        redirected from stdin.txt so a "-" argument has real data.
run_one() {
    local args_str="$1" spec="$2" impl="$3"
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__EMPTY__/}"
    done
    local infiles=()
    if [ "$spec" != "-" ]; then
        local tok name
        for tok in $spec; do
            name="${tok#f:}"
            if [ "$name" = "-" ]; then
                infiles+=("-")
            else
                infiles+=("$TMP/$name")
            fi
        done
    fi
    local cmd
    if [ "$impl" = hb ]; then
        cmd=(bash -c 'exec -a paste "$0" "$@"' "$PASTE_HOST")
    else
        cmd=(bash -c 'exec -a paste "$0" "$@"' "$REF")
    fi
    "${cmd[@]}" "${args[@]}" "${infiles[@]}" \
        <"$TMP/stdin.txt" >"$TMP/out" 2>"$TMP/err"
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
        echo "FAIL: stdout differs: paste $args_str ($spec)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: paste $args_str ($spec)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && [ "$rc_hb" != "0" ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): paste $args_str ($spec)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# Cases where 2.1-era paste and modern coreutils legitimately diverge.  Only
# compared when the reference IS textutils-2.1:
#   * a file without a trailing newline: 2.1's paste_parallel does putc (chr)
#     with chr == EOF, emitting a 0xFF byte where the line's last character
#     would be (and only then the delimiter); modern coreutils guards it with
#     `chr != EOF' and emits the delimiter/newline instead.
#   * repeated options: 2.1 accepts `-s -s' / `-d a -d b' (serial_merge++,
#     delims = last optarg); modern argparse rejects any option given twice
#     ("cannot be used multiple times", rc 1).
#   * serial-mode open/read errors: 2.1 keeps walking the file list over a
#     missing file (rc 1, later files still pasted) and emits a stray newline
#     for an unreadable one; modern coreutils produces no output for them.
cmp_case_21() {
    if [ "$REF_MODE" = strict ]; then
        cmp_case "$1" "$2"
    else
        echo "note: skipping (2.1-vs-modern divergence): paste $1 ($2)"
    fi
}

# --- default parallel mode: one line from each file, TAB-separated ----
cmp_case "" "f:a.txt f:b.txt"
cmp_case "" "f:a.txt f:b.txt f:c.txt"
cmp_case "" "f:b.txt f:a.txt"
cmp_case "" "f:a.txt f:c.txt f:b.txt"
cmp_case "" "f:a.txt"
cmp_case "" "f:many.txt f:a.txt"
cmp_case "" "f:a.txt f:many.txt"
cmp_case "" "f:tabs.txt f:b.txt"
cmp_case "" "f:big.txt f:a.txt"
cmp_case "" "f:big.txt f:c.txt f:empty.txt"
cmp_case "" "f:longline.txt f:a.txt"
cmp_case "" "f:a.txt f:longline.txt"
cmp_case "" "f:longline.txt f:longline.txt"

# --- empty files in every position ---
cmp_case "" "f:empty.txt f:a.txt"
cmp_case "" "f:a.txt f:empty.txt"
cmp_case "" "f:empty.txt f:empty.txt"
cmp_case "" "f:a.txt f:empty.txt f:b.txt"
cmp_case "" "f:empty.txt f:a.txt f:b.txt"
cmp_case "" "f:a.txt f:b.txt f:empty.txt"
cmp_case "" "f:blank1.txt f:empty.txt"

# --- files without a trailing newline (2.1 EOF-byte behavior: strict only) ---
cmp_case_21 "" "f:noeol.txt f:a.txt"
cmp_case_21 "" "f:a.txt f:noeol.txt"
cmp_case_21 "" "f:noeol.txt f:noeol2.txt"
cmp_case_21 "" "f:noeol.txt f:empty.txt"
cmp_case_21 "" "f:c.txt f:noeol.txt"
cmp_case_21 "" "f:- f:noeol.txt"

# --- -s / --serial ---
cmp_case "-s" "f:a.txt"
cmp_case "-s" "f:a.txt f:b.txt"
cmp_case "-s" "f:a.txt f:b.txt f:c.txt"
cmp_case "-s" "f:b.txt f:a.txt"
cmp_case "-s" "f:empty.txt f:a.txt"
cmp_case "-s" "f:a.txt f:empty.txt"
cmp_case "-s" "f:noeol.txt"
cmp_case "-s" "f:noeol.txt f:a.txt"            # serial mode has no EOF-byte quirk
cmp_case "-s" "f:tabs.txt"
cmp_case "-s" "f:longline.txt"
cmp_case "-s" "f:many.txt"
cmp_case "--serial" "f:a.txt f:b.txt"
cmp_case_21 "-s -s" "f:a.txt f:b.txt"

# --- -d / --delimiters ---
cmp_case "-d ," "f:a.txt f:b.txt"
cmp_case "-d," "f:a.txt f:b.txt"
cmp_case "-d :" "f:a.txt f:b.txt"
cmp_case "-d :" "f:a.txt f:b.txt f:c.txt"
cmp_case "-d :," "f:a.txt f:b.txt f:c.txt"
cmp_case "-d :,;" "f:a.txt f:b.txt f:c.txt f:many.txt"
cmp_case "-d ab" "f:a.txt f:b.txt f:c.txt"
cmp_case "--delimiters=:" "f:a.txt f:b.txt"
cmp_case "--delimiters :" "f:a.txt f:b.txt"
cmp_case "-d __SP__" "f:a.txt f:b.txt"
cmp_case "-d __EMPTY__" "f:a.txt f:b.txt"
cmp_case_21 "-d , -d :" "f:a.txt f:b.txt"
cmp_case "-s -d :" "f:a.txt f:b.txt f:c.txt"
cmp_case "-d: -s" "f:noeol.txt"
cmp_case "-d , -s" "f:many.txt"
cmp_case "--serial --delimiters=:" "f:a.txt f:b.txt f:c.txt"
cmp_case "-d :" "f:big.txt f:c.txt"
cmp_case "-d :" "f:empty.txt f:empty.txt"

# escapes in the delimiter list: collapse_escapes
cmp_case '-d \n' "f:a.txt f:b.txt"
cmp_case '-d \t' "f:a.txt f:b.txt"
cmp_case '-d \0' "f:a.txt f:b.txt"
cmp_case '-d \0' "f:a.txt f:b.txt f:c.txt"
cmp_case '-d \\' "f:a.txt f:b.txt"
cmp_case '-d \r' "f:a.txt f:b.txt"
cmp_case '-d \b' "f:a.txt f:b.txt"
cmp_case '-d \f' "f:a.txt f:b.txt"
cmp_case '-d \v' "f:a.txt f:b.txt"
cmp_case '-d \q' "f:a.txt f:b.txt"
cmp_case '-d \n,' "f:a.txt f:b.txt f:c.txt"
cmp_case '-d \0,' "f:a.txt f:b.txt f:c.txt"
cmp_case '-d \t -s' "f:a.txt f:b.txt"
cmp_case '-d \0 -s' "f:a.txt f:b.txt"
# NOTE: GNU paste never errors on a short delimiter LIST — it cycles back to
# the list's beginning (the "delimiter list exhausted" message belongs to
# non-GNU pastes).  The :,-and-1-char cases above cover the wrap-around.

# --- stdin: "-" among the FILEs, and the no-FILE form ---
cmp_case "" "-"
cmp_case "" "f:-"
cmp_case "" "f:a.txt f:-"
cmp_case "" "f:- f:a.txt"
cmp_case "" "f:- f:-"
cmp_case "" "f:- f:- f:-"
cmp_case "" "f:b.txt f:- f:empty.txt"
cmp_case "" "f:a.txt f:b.txt f:-"
cmp_case "-s" "-"
cmp_case "-s" "f:-"
cmp_case "-s" "f:- f:a.txt"
cmp_case "-d :" "f:a.txt f:-"
cmp_case "-d : -s" "f:-"
cmp_case "--" "f:a.txt f:b.txt"

# --- error paths ---
cmp_case "" "f:nonexistent_xyz.txt"                 # missing file (rc 1)
cmp_case "" "f:a.txt f:nonexistent_xyz.txt"         # good file then missing
cmp_case "" "f:nonexistent_xyz.txt f:a.txt"         # missing then good
cmp_case_21 "-s" "f:a.txt f:nonexistent_xyz.txt"    # serial: 2.1 continues
cmp_case_21 "-s" "f:nonexistent_xyz.txt f:a.txt"
cmp_case "" "f:adir"                                # directory: read error
cmp_case_21 "-s" "f:adir"
cmp_case "" "f:noperm.txt"                          # unreadable file
cmp_case "-d" "f:a.txt"                             # missing option argument
cmp_case "--delimiters" "f:a.txt"                   # missing long option arg
cmp_case "-q" "f:a.txt"                             # unknown short option
cmp_case "--bogus" "f:a.txt"                        # unknown long option
cmp_case "-dxy -s" "f:a.txt"                        # 2-char delim is fine (list)

# --- --help / --version: 2.1 text is reproduced bit-exactly, so these
# only compare against the 2.1 reference (coreutils rewrote both). ---
if [ "$REF_MODE" = strict ]; then
    cmp_case "--help" "-"
    cmp_case "--version" "-"
    cmp_case "--vers" "-"                           # unique long-option prefix
else
    echo "note: skipping --help/--version cases (2.1 text vs coreutils)"
fi

if [ "$fail" = 0 ]; then
    echo "paste parity: PASS ($N cases byte-exact vs $REF, $REF_MODE mode)"
    exit 0
fi
echo "paste parity: FAIL (of $N cases)"
exit 1
