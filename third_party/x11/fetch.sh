#!/usr/bin/env bash
# fetch.sh -- download + sha256-verify the pinned X11 stack tarballs.
#
#   bash third_party/x11/fetch.sh [--stage NAME[,NAME...]] [--record]
#
# Reads pins/<stage>.txt (all stages when --stage is omitted).  Each tarball
# lands in third_party/x11/dl/ (gitignored) and must match the committed
# sums/<tarball>.sha256.  A pin without a sums file is a hard error unless
# --record is given (trust-on-first-use: the computed digest is written to
# sums/ and must then be committed and listed in README.md).
# Tarballs are untrusted data: this script only downloads and hashes them;
# build.sh extracts each into its own fresh directory.
set -euo pipefail
X11="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$X11/../.." && pwd)"
DL="${X11_DL_DIR:-$X11/dl}"
STAGES=""; RECORD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --stage) STAGES="$2"; shift 2;;
    --record) RECORD=1; shift;;
    *) echo "usage: $0 [--stage NAME[,NAME]] [--record]" >&2; exit 2;;
  esac
done
mkdir -p "$DL"
pinfiles=()
if [ -z "$STAGES" ]; then
  for f in "$X11"/pins/*.txt; do pinfiles+=("$f"); done
else
  IFS=, read -r -a st <<<"$STAGES"
  for s in "${st[@]}"; do
    [ -f "$X11/pins/$s.txt" ] || { echo "fetch: no pins/$s.txt" >&2; exit 2; }
    pinfiles+=("$X11/pins/$s.txt")
  done
fi
fail=0
for pf in "${pinfiles[@]}"; do
  while read -r name src _rest; do
    case "$name" in ''|'#'*) continue;; esac
    out="$DL/$name"
    if [ ! -f "$out" ]; then
      case "$src" in
        vendored:*) cp "$REPO/${src#vendored:}" "$out.part" ;;
        https://*) echo "fetch: $name <- $src"
                   curl -fsSL --retry 3 --max-time 600 -o "$out.part" "$src" ;;
        *) echo "fetch: bad source for $name: $src" >&2; exit 2;;
      esac
      mv "$out.part" "$out"
    fi
    sum="$X11/sums/$name.sha256"
    got="$(sha256sum "$out" | cut -d' ' -f1)"
    if [ ! -f "$sum" ]; then
      if [ "$RECORD" = 1 ]; then
        printf '%s  %s\n' "$got" "$name" > "$sum"
        echo "fetch: RECORDED $name $got"
        continue
      fi
      echo "fetch: missing sums/$name.sha256 (run with --record once, commit it)" >&2
      fail=1; continue
    fi
    want="$(cut -d' ' -f1 "$sum")"
    if [ "$got" != "$want" ]; then
      echo "fetch: SHA256 MISMATCH $name: got $got want $want" >&2
      rm -f "$out"; fail=1; continue
    fi
    echo "fetch: ok $name"
  done < "$pf"
done
exit $fail
