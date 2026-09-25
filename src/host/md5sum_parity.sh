#!/bin/bash
# md5sum_parity.sh — byte-exact acceptance test for the HobbyOS ported md5sum.
#
# Runs our host-built md5sum (md5sum_host, argv[0] forced to "md5sum" via
# exec -a so error messages are comparable) against a GNU md5sum reference
# on the same inputs and compares stdout bytes, exit codes and stderr text
# across the RFC 1321 test vectors, byte/binary vs text mode, stdin ("-"
# and no FILE), multiple files, --string, escaped file names, check files
# (-c / --check / --status / -w), missing files, directories and malformed
# check-file lines.  The vectors themselves are also asserted against
# hard-coded digests and an independent python3 hashlib oracle, so a
# broken reference cannot mask a broken port.
#
# Reference selection:
#   $2 = explicit path, else $MD5SUM_REF, else the first of these that
#   exists: /usr/bin/md5sum (GNU coreutils) — tests the port against a
#   modern GNU md5sum; the --help/--version text and 2003-era diagnostic
#   rewording legitimately differ (2.1 vs coreutils), so stderr wording
#   and the two text-only long options run only in strict mode.  The
#   strictest bar is `make md5sum_parity_strict`, which builds
#   textutils-2.1's original md5sum as the reference instead.
set -u

MD5SUM_HOST="${1:-./obj/md5sum_host}"
REF="${3:-${2:-${MD5SUM_REF:-$(command -v md5sum || echo /usr/bin/md5sum)}}}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
N=0

SAME_OUT() { diff -q "$1" "$2" >/dev/null; }

# Detect the reference flavor: the textutils-2.1 original (strict: error
# text and exit codes compare byte-for-byte) vs modern GNU coreutils
# (loose: diagnostics were rewritten in 2003-era coreutils, so error
# paths compare only whether the run succeeded, not the wording, and
# --string/--help/--version are not exercised).
if "$REF" --version 2>/dev/null | grep -qi "textutils"; then
    REF_MODE=strict
else
    REF_MODE=loose
fi

# Some loose references are not GNU at all (this host's /usr/bin/md5sum is
# uutils' Rust coreutils).  uutils deviates from BOTH 2.1 and GNU on three
# observable points, so the affected races are skipped for it: it refuses
# backslash-escaped file names in --check input, and it ignores --status
# when reporting files it could not read.
REF_GNU=1
if "$REF" --version 2>/dev/null | grep -qi "uutils\|busybox"; then
    REF_GNU=0
    echo "note: reference is not GNU; skipping escaped-check-file and --status races"
fi

# --- fixtures ---------------------------------------------------------
: > "$TMP/empty.txt"
printf 'abc' > "$TMP/a.txt"
printf 'message digest' > "$TMP/b.txt"
printf 'abcdefghijklmnopqrstuvwxyz' > "$TMP/c.txt"
printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789' > "$TMP/d.txt"
printf '12345678901234567890123456789012345678901234567890123456789012345678901234567890' > "$TMP/e.txt"
printf 'The quick brown fox jumps over the lazy dog' > "$TMP/fox.txt"
printf 'one\n' > "$TMP/m1.txt"
printf 'two\n' > "$TMP/m2.txt"
printf 'three\n' > "$TMP/m3.txt"
mkdir -p "$TMP/adir"
python3 - "$TMP" <<'PY'
import sys
tmp = sys.argv[1]
w = lambda n, b: open(tmp + "/" + n, "wb").write(b)
w("n63.txt", b"A" * 63)              # one byte short of a padding block
w("n64.txt", b"A" * 64)              # exactly one block
w("n65.txt", b"A" * 65)              # one byte past a block
w("blk4k.txt", b"b" * 4096)          # exactly BLOCKSIZE (md5_stream's n == 0 path)
w("blk9k.txt", b"b" * (4096 * 2 + 37))  # two full blocks + partial tail
w("big1m.txt", b"a" * 1048576)       # 1 MiB: many blocks, exact multiple of BLOCKSIZE
w("million.txt", b"a" * 1000000)     # the classic one-million-'a' vector
w("bin256.txt", bytes(range(256)))   # every byte value
w("nulmix.bin", b"ab\0cd\xff\xfe\nno newline at end")
w("we\\ird.txt", b"x")               # backslash in the name
w("new\nline.txt", b"y")             # newline in the name
PY

# --- known-vector expectations (RFC 1321 + independent hashlib check) --
V_EMPTY=d41d8cd98f00b204e9800998ecf8427e
V_ABC=900150983cd24fb0d6963f7d28e17f72
V_MSG=f96b697d7cb7938d525a2f31aaf161d0
V_ALPHA=c3fcd3d76192e4007dfb496cca67e13b
V_ALNUM=d174ab98d277d9f5a5611c2c9f419d9f
V_80=57edf4a22be3c955ac49da2e2107b67a
V_FOX=9e107d9d372bb6826bd81d3542a419d6
V_1M=7202826a7791073fe2787f0c94603278
V_1MA=7707d6ae4e027c70eea2a935c2296f21

python3 - "$TMP" > "$TMP/oracle.txt" <<'PY'
import hashlib, sys
tmp = sys.argv[1]
for n in ("empty.txt", "a.txt", "b.txt", "c.txt", "d.txt", "e.txt", "fox.txt",
          "m1.txt", "m2.txt", "m3.txt", "n63.txt", "n64.txt", "n65.txt",
          "blk4k.txt", "blk9k.txt", "big1m.txt", "million.txt", "bin256.txt",
          "nulmix.bin"):
    print("%s  %s" % (hashlib.md5(open(tmp + "/" + n, "rb").read()).hexdigest(), n))
PY
# Every fixture digest must match the port's own output (single-file runs).
N=$((N+1))
rm -f "$TMP/port_sums"
while read -r digest name; do
    printf '%s  %s\n' "$digest" "$TMP/$name" >> "$TMP/port_sums"
done < "$TMP/oracle.txt"
bash -c 'exec -a md5sum "$0" "$@"' "$MD5SUM_HOST" \
    "$TMP/empty.txt" "$TMP/a.txt" "$TMP/b.txt" "$TMP/c.txt" "$TMP/d.txt" \
    "$TMP/e.txt" "$TMP/fox.txt" "$TMP/m1.txt" "$TMP/m2.txt" "$TMP/m3.txt" \
    "$TMP/n63.txt" "$TMP/n64.txt" "$TMP/n65.txt" "$TMP/blk4k.txt" \
    "$TMP/blk9k.txt" "$TMP/big1m.txt" "$TMP/million.txt" "$TMP/bin256.txt" \
    "$TMP/nulmix.bin" \
    > "$TMP/out" 2> "$TMP/err"
if ! SAME_OUT "$TMP/out" "$TMP/port_sums"; then
    echo "FAIL: port digests differ from the hashlib oracle (all fixtures)"
    diff "$TMP/port_sums" "$TMP/out" | head -10
    fail=1
fi

# EXACT: run the port alone, compare stdout bytes + rc against a literal.
#   $1 = args string; $2 = stdin fixture name ("" = /dev/null);
#   $3 = expected stdout; $4 = expected rc; $5 = label
EXACT() {
    local args_str="$1" stdin_spec="$2" want_out="$3" want_rc="${4:-0}" label="$5"
    N=$((N+1))
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__TMP__/$TMP}"
    done
    local infile=/dev/null
    [ -n "$stdin_spec" ] && infile="$TMP/$stdin_spec"
    bash -c 'exec -a md5sum "$0" "$@"' "$MD5SUM_HOST" "${args[@]}" \
        <"$infile" >"$TMP/out" 2>"$TMP/err"
    local rc=$?
    printf '%s' "$want_out" > "$TMP/want"
    if ! SAME_OUT "$TMP/out" "$TMP/want"; then
        echo "FAIL: port stdout != known vector: $label"
        echo "  got:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        echo "  want: $(od -c "$TMP/want" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc" != "$want_rc" ]; then
        echo "FAIL: port rc $rc != $want_rc: $label"
        fail=1
    fi
}

# cmp_case: race the port against the reference on identical args/stdin.
#   $1 = args string (__TMP__ = fixture dir); $2 = stdin fixture ("" = /dev/null)
cmp_case() {
    local args_str="$1" stdin_spec="${2:-}" label="${3:-$1}"
    N=$((N+1))
    local args=()
    [ -n "$args_str" ] && read -r -a args <<< "$args_str"
    local i
    for i in "${!args[@]}"; do
        args[i]="${args[i]//__SP__/ }"
        args[i]="${args[i]//__TMP__/$TMP}"
    done
    local infile=/dev/null
    [ -n "$stdin_spec" ] && infile="$TMP/$stdin_spec"
    bash -c 'exec -a md5sum "$0" "$@"' "$MD5SUM_HOST" "${args[@]}" \
        <"$infile" >"$TMP/out" 2>"$TMP/err"
    local rc_hb=$?
    mv "$TMP/out" "$TMP/oh"; mv "$TMP/err" "$TMP/eh"
    bash -c 'exec -a md5sum "$0" "$@"' "$REF" "${args[@]}" \
        <"$infile" >"$TMP/out" 2>"$TMP/err"
    local rc_ref=$?
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: md5sum $args_str ($label)"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: md5sum $args_str ($label)"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): md5sum $args_str ($label)"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

hb_run() { bash -c 'exec -a md5sum "$0" "$@"' "$MD5SUM_HOST" "$@"; }
ref_run() { bash -c 'exec -a md5sum "$0" "$@"' "$REF" "$@"; }

# same_case: compare the last hb_run/ref_run results ($rc_hb/$rc_ref).
same_case() {
    local label="$1"
    N=$((N+1))
    if ! SAME_OUT "$TMP/oh" "$TMP/out"; then
        echo "FAIL: stdout differs: $label"
        echo "  hb:   $(od -c "$TMP/oh" | head -3 | tr '\n' '|')"
        echo "  ref:  $(od -c "$TMP/out" | head -3 | tr '\n' '|')"
        fail=1
    fi
    if [ "$rc_hb" != "$rc_ref" ]; then
        if [ "$REF_MODE" = strict ] \
            || ! { { [ "$rc_hb" = 0 ] && [ "$rc_ref" = 0 ]; } \
                || { [ "$rc_hb" != 0 ] && [ "$rc_ref" != 0 ]; }; }; then
            echo "FAIL: exit code $rc_hb vs $rc_ref: $label"
            fail=1
        fi
    fi
    if [ "$REF_MODE" = strict ] && ! SAME_OUT "$TMP/eh" "$TMP/err"; then
        echo "FAIL: stderr differs (rc=$rc_hb): $label"
        echo "  hb:  $(cat "$TMP/eh")"
        echo "  ref: $(cat "$TMP/err")"
        fail=1
    fi
}

# --- RFC 1321 vectors, hard-coded ------------------------------------
EXACT "__TMP__/empty.txt" "" "$V_EMPTY  $TMP/empty.txt
" 0 "empty vector"
EXACT "__TMP__/a.txt" "" "$V_ABC  $TMP/a.txt
" 0 "abc vector"
EXACT "__TMP__/b.txt" "" "$V_MSG  $TMP/b.txt
" 0 "message-digest vector"
EXACT "__TMP__/c.txt" "" "$V_ALPHA  $TMP/c.txt
" 0 "a-z vector"
EXACT "__TMP__/d.txt" "" "$V_ALNUM  $TMP/d.txt
" 0 "alnum vector"
EXACT "__TMP__/e.txt" "" "$V_80  $TMP/e.txt
" 0 "80-digit vector"
EXACT "__TMP__/fox.txt" "" "$V_FOX  $TMP/fox.txt
" 0 "fox vector"
EXACT "__TMP__/big1m.txt" "" "$V_1M  $TMP/big1m.txt
" 0 "1 MiB vector"
EXACT "__TMP__/million.txt" "" "$V_1MA  $TMP/million.txt
" 0 "one-million-a vector"
EXACT "__TMP__/a.txt __TMP__/empty.txt" "" "$V_ABC  $TMP/a.txt
$V_EMPTY  $TMP/empty.txt
" 0 "multi-file vector"
EXACT "-b __TMP__/a.txt" "" "$V_ABC *$TMP/a.txt
" 0 "binary marker vector"
EXACT "-t __TMP__/a.txt" "" "$V_ABC  $TMP/a.txt
" 0 "text mode vector"
EXACT "--binary --text __TMP__/a.txt" "" "$V_ABC  $TMP/a.txt
" 0 "--binary --text last wins"
EXACT "-" "a.txt" "$V_ABC  -
" 0 "stdin dash vector"
EXACT "" "a.txt" "$V_ABC  -
" 0 "stdin no-FILE vector"
EXACT "__TMP__/a.txt -" "b.txt" "$V_ABC  $TMP/a.txt
$V_MSG  -
" 0 "file then stdin"
EXACT "-- __TMP__/a.txt" "" "$V_ABC  $TMP/a.txt
" 0 "-- end of options"
EXACT "__TMP__/adir" "" "" 1 "directory: stdout empty, rc 1"
EXACT "__TMP__/nosuchfile.xyz" "" "" 1 "missing file: stdout empty, rc 1"
EXACT "__TMP__/a.txt __TMP__/nosuchfile.xyz __TMP__/b.txt" "" "$V_ABC  $TMP/a.txt
$V_MSG  $TMP/b.txt
" 1 "missing file in the middle"

if [ "$REF_MODE" = strict ]; then
    # --string is a textutils-2.1 option that coreutils later removed.
    EXACT "--string=abc" "" "$V_ABC  \"abc\"
" 0 "--string=abc vector"
    EXACT "--string=abc __TMP__/a.txt" "" "" 1 "--string plus FILE operand"
    EXACT "--string=abc --string=message__SP__digest" "" "$V_ABC  \"abc\"
$V_MSG  \"message digest\"
" 0 "two --string values"
else
    echo "note: skipping --string cases (2.1-only option)"
fi

# --- digest mode races ------------------------------------------------
for f in empty.txt a.txt b.txt c.txt d.txt e.txt fox.txt n63.txt n64.txt n65.txt \
         blk4k.txt blk9k.txt big1m.txt bin256.txt nulmix.bin; do
    cmp_case "__TMP__/$f" "" "digest $f"
done
cmp_case "-b __TMP__/bin256.txt" "" "-b bin256"
cmp_case "-b __TMP__/nulmix.bin" "" "-b nulmix"
cmp_case "-t __TMP__/bin256.txt" "" "-t bin256"
cmp_case "--binary __TMP__/big1m.txt" "" "--binary big1m"
cmp_case "--text __TMP__/a.txt" "" "--text a.txt"
cmp_case "__TMP__/m1.txt __TMP__/m2.txt __TMP__/m3.txt" "" "three files"
cmp_case "__TMP__/m1.txt __TMP__/m1.txt" "" "same file twice"
cmp_case "__TMP__/nosuchfile.xyz" "" "missing file"
cmp_case "__TMP__/a.txt __TMP__/adir" "" "file then directory"
cmp_case "__TMP__/adir __TMP__/a.txt" "" "directory then file"
cmp_case "- __TMP__/a.txt" "m1.txt" "stdin then file"
cmp_case "- - " "m2.txt" "stdin twice"
cmp_case "" "big1m.txt" "stdin 1MiB"
cmp_case "" "nulmix.bin" "stdin binary"
cmp_case "--zzz" "" "unknown long option"
cmp_case "-x" "" "unknown short option"
cmp_case "--check=foo" "" "--check takes no argument"
cmp_case "-b" "a.txt" "-b alone reads stdin"

# --- escaped file names ----------------------------------------------
hb_run "$TMP/we\\ird.txt" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run "$TMP/we\\ird.txt" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "backslash in name"
hb_run "$TMP/new
line.txt" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run "$TMP/new
line.txt" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "newline in name"
# and check-mode round trips over those escaped lines
hb_run "$TMP/we\\ird.txt" "$TMP/new
line.txt" > "$TMP/escaped.chk" 2>/dev/null
if [ "$REF_MODE" = strict ]; then
    ref_run "$TMP/we\\ird.txt" "$TMP/new
line.txt" > "$TMP/escaped.ref" 2>/dev/null
    N=$((N+1))
    if ! SAME_OUT "$TMP/escaped.chk" "$TMP/escaped.ref"; then
        echo "FAIL: escaped-name listing differs"
        fail=1
    fi
fi
if [ "$REF_GNU" = 1 ]; then
    hb_run -c "$TMP/escaped.chk" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
    ref_run -c "$TMP/escaped.chk" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
    same_case "-c over escaped names"
else
    # uutils rejects 2.1/GNU-style escaped names outright; assert the port's
    # own round trip instead of racing it.
    N=$((N+1))
    hb_run -c "$TMP/escaped.chk" >"$TMP/out" 2>"$TMP/err"; rc_hb=$?
    if [ "$rc_hb" != 0 ] || ! grep -q ": OK" "$TMP/out"; then
        echo "FAIL: port -c round trip over escaped names"
        fail=1
    fi
fi

# --- check files ------------------------------------------------------
# Written from the hard-coded vectors, not from either implementation.
{
    printf '%s  %s\n' "$V_ABC" "$TMP/a.txt"
    printf '%s  %s\n' "$V_MSG" "$TMP/b.txt"
} > "$TMP/chk-ok.txt"
{
    printf '%s  %s\n' "$V_ABC" "$TMP/a.txt"
    printf '900150983cd24fb0d6963f7d28e17f73  %s\n' "$TMP/a.txt"
    printf '%s  %s\n' "$V_MSG" "$TMP/b.txt"
    printf '%s  %s\n' "$V_ALPHA" "$TMP/c.txt"
} > "$TMP/chk-mixed.txt"
printf '900150983cd24fb0d6963f7d28e17f73  %s\n' "$TMP/a.txt" > "$TMP/chk-bad.txt"
printf '900150983CD24FB0D6963F7D28E17F72  %s\n' "$TMP/a.txt" > "$TMP/chk-upper.txt"
printf '%s *%s\n' "$V_ABC" "$TMP/a.txt" > "$TMP/chk-star.txt"
{
    printf '# a comment line\n'
    printf '\n'
    printf '%s  %s\n' "$V_ABC" "$TMP/a.txt"
} > "$TMP/chk-comment.txt"
printf '%s  %s' "$V_ABC" "$TMP/a.txt" > "$TMP/chk-nonl.txt"
: > "$TMP/chk-empty.txt"
printf 'not a checksum line at all\n' > "$TMP/chk-garbage.txt"
{
    printf 'garbage here\n'
    printf '%s  %s\n' "$V_ABC" "$TMP/a.txt"
} > "$TMP/chk-partial.txt"
{
    printf '%s\ta.txt\n' "$V_ABC"
    printf 'abc\n'
} > "$TMP/chk-malformed.txt"
printf 'd41d8cd98f00b204e9800998ecf8427e  %s\n' "$TMP/nosuchfile.xyz" > "$TMP/chk-missing.txt"
printf '%s  %s\n' "$V_ABC" "$TMP/adir" > "$TMP/chk-dir.txt"
printf '%s  -\n' "$V_ABC" > "$TMP/chk-stdin.txt"

# -c OK / FAILED / mixed, with rc
cmp_case "-c __TMP__/chk-ok.txt" "" "-c pass"
cmp_case "--check __TMP__/chk-ok.txt" "" "--check pass"
cmp_case "-c __TMP__/chk-bad.txt" "" "-c fail"
cmp_case "-c __TMP__/chk-mixed.txt" "" "-c mixed"
cmp_case "-c __TMP__/chk-upper.txt" "" "-c uppercase hex"
cmp_case "-c __TMP__/chk-star.txt" "" "-c star marker"
cmp_case "-c __TMP__/chk-comment.txt" "" "-c comment+blank lines"
cmp_case "-c __TMP__/chk-nonl.txt" "" "-c no trailing newline"
cmp_case "-c __TMP__/chk-empty.txt" "" "-c empty checkfile"
cmp_case "-c __TMP__/chk-garbage.txt" "" "-c garbage checkfile"
cmp_case "-c __TMP__/chk-partial.txt" "" "-c garbage line before a good one"
cmp_case "-c __TMP__/chk-missing.txt" "" "-c listed file missing"
cmp_case "-c __TMP__/chk-dir.txt" "" "-c lists a directory"
cmp_case "-c __TMP__/chk-stdin.txt" "" "-c listing - (stdin)"
cmp_case "-c - __TMP__/nosuchfile.xyz" "" "-c from stdin (missing file)"
cmp_case "-c __TMP__/nosuch-chk.txt" "" "-c missing checkfile"
cmp_case "-b -c __TMP__/chk-ok.txt" "" "-b -c conflict"
cmp_case "-t -c __TMP__/chk-ok.txt" "" "-t -c conflict"
cmp_case "--status __TMP__/a.txt" "" "--status without -c"
cmp_case "-w __TMP__/a.txt" "" "-w without -c"
cmp_case "-c --status __TMP__/chk-ok.txt" "" "-c --status pass"
cmp_case "-c --status __TMP__/chk-bad.txt" "" "-c --status fail"
if [ "$REF_GNU" = 1 ]; then
    cmp_case "-c --status __TMP__/chk-missing.txt" "" "-c --status missing"
else
    echo "note: skipping --status-with-missing-file (uutils prints a FAILED line)"
fi
if [ "$REF_MODE" = strict ]; then
    # 2.1 accepts only one check file; coreutils (GNU and uutils) accepts
    # several, so this is a 2.1-only assertion.
    cmp_case "-c __TMP__/chk-ok.txt __TMP__/chk-ok.txt" "" "-c two args"
else
    N=$((N+1))
    hb_run -c "$TMP/chk-ok.txt" "$TMP/chk-ok.txt" >"$TMP/out" 2>"$TMP/err"; rc_hb=$?
    if [ "$rc_hb" != 1 ] || ! grep -q "only one argument" "$TMP/err"; then
        echo "FAIL: port accepts two check files (2.1 rejects them)"
        fail=1
    fi
fi
cmp_case "-c -w __TMP__/chk-partial.txt" "" "-c -w warn on good+garbage"
cmp_case "-c __TMP__/chk-partial.txt" "" "-c no -w on good+garbage"
cmp_case "-c -w __TMP__/chk-malformed.txt" "" "-c -w malformed lines only"
cmp_case "-c -w __TMP__/chk-empty.txt" "" "-c -w empty checkfile"
cmp_case "-wc __TMP__/chk-partial.txt" "" "-wc combined shorts"

# checkfile from stdin, with stdin also the data source for a "-" entry
hb_run -c - < "$TMP/chk-ok.txt" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run -c - < "$TMP/chk-ok.txt" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "-c - checkfile from stdin"
hb_run -c "$TMP/chk-stdin.txt" < "$TMP/a.txt" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run -c "$TMP/chk-stdin.txt" < "$TMP/a.txt" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "-c checkfile naming - (reads stdin)"

# checkfile written by the port itself (round trip), including binary mode
hb_run "$TMP/bin256.txt" "$TMP/nulmix.bin" > "$TMP/self.chk" 2>/dev/null
hb_run -b "$TMP/big1m.txt" >> "$TMP/self.chk" 2>/dev/null
hb_run -c "$TMP/self.chk" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run -c "$TMP/self.chk" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "-c over the port's own listing"
hb_run -c --status "$TMP/self.chk" >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
ref_run -c --status "$TMP/self.chk" >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
same_case "-c --status over the port's own listing"

# --- --help / --version (2.1 text; gated to strict mode) --------------
if [ "$REF_MODE" = strict ]; then
    hb_run --help >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
    ref_run --help >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
    same_case "--help text"
    hb_run --version >"$TMP/oh" 2>"$TMP/eh"; rc_hb=$?
    ref_run --version >"$TMP/out" 2>"$TMP/err"; rc_ref=$?
    same_case "--version text"
    # The port alone must also produce the exact 2.1 banner.
    N=$((N+1))
    if [ "$(cat "$TMP/oh")" != "md5sum (textutils) 2.1" ]; then
        echo "FAIL: port --version is not 'md5sum (textutils) 2.1'"
        fail=1
    fi
else
    echo "note: skipping --help/--version (2.1 vs coreutils text differs)"
fi

if [ "$fail" = 0 ]; then
    echo "md5sum parity: PASS ($N cases byte-exact vs $REF)"
    exit 0
fi
echo "md5sum parity: FAIL (of $N cases, vs $REF)"
exit 1
