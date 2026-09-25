#!/bin/bash
# cmp_parity.sh — byte-exact acceptance test for the HobbyOS ported cmp.
#
# Runs our host-built cmp (cmp_host, argv[0] forced to "cmp" via exec -a so
# diagnostics are comparable) against the GNU diffutils-2.8.1 reference
# built by src/host/build_diffutils_cmp_ref.sh, and compares stdout bytes,
# stderr bytes and exit codes across identical/different files, the -b/-c/
# -l/-s output formats, --ignore-initial, --bytes, stdin operands, SKIP
# operands, EOF handling, missing files, option errors and --help/--version.
#
# Comparison is always strict: the reference is the 2.8.1 original, so the
# bar is byte-for-byte stdout + stderr + exit status.  (use `make
# cmp_parity_strict` to build that reference first.)
#
# Usage: bash src/host/cmp_parity.sh [cmp_host] [diffutils-2.8.1-ref]
set -u

CMP_HOST="${1:-./obj/cmp_host}"
REF="${2:-${CMP_REF:-}}"
if [ -z "$REF" ]; then
    REF="$(ls obj/diffutils_cmp_ref /tmp/gnudiffutils-cmp-ref/diffutils_cmp 2>/dev/null | head -1)"
fi
if [ -z "$REF" ] || [ ! -x "$REF" ]; then
    echo "cmp_parity: no diffutils-2.8.1 reference found; run"
    echo "  bash src/host/build_diffutils_cmp_ref.sh obj/diffutils_cmp_ref"
    echo "or use: make cmp_parity_strict"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

# run_cmp LABEL BIN OUT ERR RC -- ARGS...   (stdin: $STDIN file or /dev/null)
run_case() {
    local label="$1" which="$2" bin out err rc
    shift 2
    if [ "$which" = ours ]; then
        bin="$CMP_HOST"
    else
        bin="$REF"
    fi
    : > "$TMP/$which.out"
    : > "$TMP/$which.err"
    if [ -n "${STDIN:-}" ]; then
        ( exec -a cmp "$bin" "$@" < "$STDIN" > "$TMP/$which.out" 2> "$TMP/$which.err" )
    else
        ( exec -a cmp "$bin" "$@" < /dev/null > "$TMP/$which.out" 2> "$TMP/$which.err" )
    fi
    rc=$?
    eval "${which}_out=\$TMP/$which.out ${which}_err=\$TMP/$which.err ${which}_rc=$rc"
}

check() {
    local what="$1"
    N=$((N + 1))
    local bad=0
    if ! diff -q "$ours_out" "$ref_out" > /dev/null; then
        echo "case $N FAIL ($what): stdout differs"
        diff "$ours_out" "$ref_out" | head -6
        bad=1
    fi
    if ! diff -q "$ours_err" "$ref_err" > /dev/null; then
        echo "case $N FAIL ($what): stderr differs"
        diff "$ours_err" "$ref_err" | head -6
        bad=1
    fi
    if [ "$ours_rc" != "$ref_rc" ]; then
        echo "case $N FAIL ($what): exit status $ours_rc != $ref_rc"
        bad=1
    fi
    if [ "$bad" = 1 ]; then
        fail=$((fail + 1))
    fi
}

# One case: runs both binaries with identical args (and optional stdin).
case_() {
    local label="$1"
    shift
    run_case "$label" ours "$@"
    run_case "$label" ref "$@"
    check "$label"
}

# --- fixtures --------------------------------------------------------------
printf ''                                   > "$TMP/empty"
printf 'hello\nworld\n'                     > "$TMP/a"
printf 'hello\nworxd\n'                     > "$TMP/b"
printf 'hello\nworld\n'                     > "$TMP/a2"
printf 'hello\nworld'                       > "$TMP/noeol"
printf 'hello\nworld\nextra\n'              > "$TMP/longer"
printf 'HELLO\nWORLD\n'                     > "$TMP/upper"
printf 'x\ny\nz\n'                          > "$TMP/xyz"
printf '\001\002\003\377\200\177'           > "$TMP/bin1"
printf '\001\002\004\377\200\177'           > "$TMP/bin2"
head -c 200000 /dev/urandom                 > "$TMP/rand1"
cp "$TMP/rand1" "$TMP/rand1c"
printf 'changed' | dd of="$TMP/rand1c" bs=1 seek=150000 conv=notrunc 2>/dev/null
printf 'aaa\nbbb\n'                         > "$TMP/skip_a"
printf 'xxx\naaa\nbbb\n'                    > "$TMP/skip_b"
mkdir -p "$TMP/adir"

# --- identical -------------------------------------------------------------
case_ "identical files"           "$TMP/a" "$TMP/a2"
case_ "identical empty"           "$TMP/empty" "$TMP/empty"
case_ "same name twice"           "$TMP/a" "$TMP/a"
case_ "identical big"             "$TMP/rand1" "$TMP/rand1c"

# --- differences -----------------------------------------------------------
case_ "differ first diff"         "$TMP/a" "$TMP/b"
case_ "differ -b"                 -b "$TMP/a" "$TMP/b"
case_ "differ -c"                 -c "$TMP/a" "$TMP/b"
case_ "differ -l"                 -l "$TMP/a" "$TMP/b"
case_ "differ -l -b"              -l -b "$TMP/a" "$TMP/b"
case_ "differ -s"                 -s "$TMP/a" "$TMP/b"
case_ "differ quiet long"         --quiet "$TMP/a" "$TMP/b"
case_ "differ silent long"        --silent "$TMP/a" "$TMP/b"
case_ "differ verbose long"       --verbose "$TMP/a" "$TMP/b"
case_ "differ binary -b"          -b "$TMP/bin1" "$TMP/bin2"
case_ "differ binary -l -b"       -l -b "$TMP/bin1" "$TMP/bin2"
case_ "differ big -l"             -l "$TMP/rand1" "$TMP/rand1c"
case_ "differ upper"              "$TMP/a" "$TMP/upper"

# --- EOF and length differences -------------------------------------------
case_ "EOF no newline"            "$TMP/a" "$TMP/noeol"
case_ "shorter first"             "$TMP/noeol" "$TMP/a"
case_ "empty vs nonempty"         "$TMP/empty" "$TMP/a"
case_ "nonempty vs empty"         "$TMP/a" "$TMP/empty"
case_ "prefix vs longer"          "$TMP/a" "$TMP/longer"
case_ "EOF -l"                    -l "$TMP/a" "$TMP/longer"
case_ "EOF -s"                    -s "$TMP/a" "$TMP/longer"

# --- --bytes / -n ----------------------------------------------------------
case_ "-n within identical part"  -n 5 "$TMP/a" "$TMP/b"
case_ "-n 6 hits difference"      -n 6 "$TMP/a" "$TMP/b"
case_ "-n long name"              --bytes 6 "$TMP/a" "$TMP/b"
case_ "-n 0"                      -n 0 "$TMP/a" "$TMP/b"
case_ "-n suffix 1K"              -n 1K "$TMP/a" "$TMP/b"
case_ "-n invalid"                -n abc "$TMP/a" "$TMP/b"

# --- --ignore-initial / -i / SKIP operands ---------------------------------
case_ "-i 6 both"                 -i 6 "$TMP/skip_b" "$TMP/skip_b"
case_ "-i 0:0"                    -i 0:0 "$TMP/skip_a" "$TMP/skip_b"
case_ "-i 4 diff offsets"         -i 4:0 "$TMP/skip_a" "$TMP/skip_b"
case_ "-i long name"              --ignore-initial=4:0 "$TMP/skip_a" "$TMP/skip_b"
case_ "-i invalid"                -i xx "$TMP/a" "$TMP/b"
case_ "SKIP operand 1"            "$TMP/skip_a" "$TMP/skip_b" 4
case_ "SKIP operand 2"            "$TMP/skip_a" "$TMP/skip_b" 4 0

# --- stdin operands --------------------------------------------------------
STDIN="$TMP/a" case_ "stdin via -"    "$TMP/a" -
STDIN="$TMP/b" case_ "stdin differ"   "$TMP/a" -
STDIN="$TMP/a" case_ "stdin first"    - "$TMP/a"
STDIN="$TMP/a" case_ "missing FILE2 reads stdin" "$TMP/a"

# --- errors ----------------------------------------------------------------
case_ "missing file"              "$TMP/does_not_exist" "$TMP/a"
case_ "both missing"              "$TMP/nope1" "$TMP/nope2"
case_ "missing file -s"           -s "$TMP/does_not_exist" "$TMP/a"
case_ "directory operand"         "$TMP/adir" "$TMP/a"
case_ "no operands"               
case_ "extra operands"            "$TMP/a" "$TMP/b" 0 0 0
case_ "bad option"                -Z "$TMP/a" "$TMP/b"
case_ "bad long option"           --nope "$TMP/a" "$TMP/b"
case_ "-l -s conflict"            -l -s "$TMP/a" "$TMP/b"
case_ "missing operand after -n"  -n
case_ "-i overflow"               -i 99999999999999999999999999 "$TMP/a" "$TMP/b"

# --- help / version --------------------------------------------------------
case_ "--help"                    --help
case_ "--version"                 --version
case_ "-v"                        -v

echo
if [ "$fail" = 0 ]; then
    echo "cmp parity: $N/$N byte-exact PASS"
    exit 0
else
    echo "cmp parity: $((N - fail))/$N passed, $fail FAILED"
    exit 1
fi
