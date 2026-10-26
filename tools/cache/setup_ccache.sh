#!/usr/bin/env bash
# setup_ccache.sh — GB-006: configure the shared build cache for the HobbyOS
# farm.  Wraps the compiler in ccache and points the cache at a shared
# CCACHE_DIR (set CCACHE_DIR to a shared/network store on the farm to share
# hits across VMs; the prebuilt clang 21.1.8 cross tree seeds the toolchain
# side, see README.md "seeding").
#
# Usage:
#   bash tools/cache/setup_ccache.sh          # probe + print the env snippet
#   eval "$(bash tools/cache/setup_ccache.sh) # configure the current shell
#
# Recommended: CCACHE_DIR=/var/cache/hobbyos-ccache (shared NFS) so every farm
# VM hits the same object cache — this is what makes GB-046-style two-arch
# parity CI cheap to re-run per merge.
set -euo pipefail

CCACHE_BIN="${CCACHE:-ccache}"
CC="${CC:-clang}"

if ! command -v "$CCACHE_BIN" >/dev/null 2>&1; then
  echo "setup_ccache.sh: ccache not found (have sccache? set CCACHE=sccache)." >&2
  exit 2
fi
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "setup_ccache.sh: compiler '$CC' not found (set CC, e.g. clang-21)." >&2
  exit 2
fi

# Default shared cache dir; override with CCACHE_DIR for a network store.
CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/hobbyos-ccache}"
mkdir -p "$CCACHE_DIR"

# Smoke test: ccache must be able to compile with $CC before we wrap anything.
probe="$(mktemp "${TMPDIR:-/tmp}/ccache-probe.XXXXXX.c")"
printf 'int probe(void){return 42;}\n' > "$probe"
if ! "$CCACHE_BIN" "$CC" -c "$probe" -o "${probe%.c}.o" >/dev/null 2>&1; then
  echo "setup_ccache.sh: '$CCACHE_BIN $CC' smoke compile failed." >&2
  rm -f "$probe" "${probe%.c}.o"
  exit 2
fi
rm -f "$probe" "${probe%.c}.o"

cat <<EOF
export CCACHE_DIR="$CCACHE_DIR"
export CC="$CCACHE_BIN $CC"
# Use with the project Makefile:  make CC="ccache clang" clean all
# sccache alternative (if the farm already runs it):  sccache --start-server
EOF
