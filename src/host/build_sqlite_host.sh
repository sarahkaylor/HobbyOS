#!/bin/sh
# build_sqlite_host.sh - host build + smoke for the vendored SQLite 3.49.2
# amalgamation (L6, browser plan AD-8 / P6.2).
#
# Usage (any cwd; paths are derived from the script location):
#   src/host/build_sqlite_host.sh
#
# Recipe: straight amalgamation compile, one TU, thread-safe serialized
# (SQLITE_THREADSAFE=1, the P6.2 requirement).
#   1. verify third_party/sqlite-amalgamation-3490200.zip against its .sums,
#      and the extracted sqlite3.c against the SHA3-256 published in the
#      official release log for 3.49.2
#   2. unzip the pinned archive if the tree is missing (gitignored)
#   3. cc sqlite3.c -> libsqlite3.a  (+ the sqlite3 CLI from shell.c)
#   4. build + run src/host/sqlite_smoke.c against libsqlite3.a
#
# Overridable: CC, REPO.  Target-build deltas (documented in the README):
# SQLITE_OS_OTHER=1 + a single HobbyOS VFS over the libc file API, WAL off
# initially, fcntl locking per P6.1.
set -eu

REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
CC=${CC:-cc}
VER=sqlite-amalgamation-3490200
SRC=$REPO/third_party/$VER
OUT=$REPO/obj/third_party/$VER
CFLAGS="-O2 -DSQLITE_THREADSAFE=1"

# 1. checksum the pinned archive, then unpack if the tree is absent
(cd "$REPO/third_party" && sha256sum -c "$VER.zip.sums")
if [ ! -d "$SRC" ]; then
  unzip -q "$REPO/third_party/$VER.zip" -d "$REPO/third_party"
fi

# 1b. sqlite3.c must match the SHA3-256 of the official 3.49.2 release log
python3 - "$SRC/sqlite3.c" <<'EOF'
import hashlib
import sys

want = "17f4857fc6a0def2749d248d5365c59282c044270533c6c2b21287295f01eb23"
got = hashlib.sha3_256(open(sys.argv[1], "rb").read()).hexdigest()
print("sqlite3.c sha3-256:", got)
if got != want:
    print("sqlite3.c sha3-256 MISMATCH (want", want + ")")
    sys.exit(1)
print("sqlite3.c sha3-256 matches the official 3.49.2 release log")
EOF

# 2. build the static library + CLI from the amalgamation
mkdir -p "$OUT"
$CC $CFLAGS -c "$SRC/sqlite3.c" -o "$OUT/sqlite3.o"
ar rcs "$OUT/libsqlite3.a" "$OUT/sqlite3.o"
$CC $CFLAGS -o "$OUT/sqlite3_cli" "$SRC/sqlite3.c" "$SRC/shell.c" \
  -lpthread -ldl -lm

# 3. smoke: in-memory CREATE/INSERT/SELECT + PRAGMA compile_options
$CC $CFLAGS -Wall -Wextra -I"$SRC" "$REPO/src/host/sqlite_smoke.c" \
  "$OUT/libsqlite3.a" -lpthread -ldl -lm -o "$OUT/sqlite_smoke"
"$OUT/sqlite_smoke"
