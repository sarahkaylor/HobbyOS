#!/usr/bin/env bash
# tools/cp/check-no-game-files.sh — hard gate: no Cyberpunk 2077 artifact may
# enter git (docs/cyberpunk.md §1.5, AD-12).  Modelled on
# tools/rw/check-no-game-files.sh, with the same three independent checks:
#
#   1. NAME over the WORKTREE: a banned file/dir lying in the tree is caught
#      before anyone runs `git add` (case-insensitive: the game ships on NTFS).
#   2. NAME over the INDEX: `git ls-files` must not list a banned path even if
#      it was deleted from the worktree.
#   3. CONTENT: a file whose size matches a known game artifact and whose
#      sha256 prefix matches it is the same file under a different name.
#      The fingerprint table is filled from the user's copy by the P0 host-run
#      lane (sizes + sha256 prefixes only, never the files); empty until then.
#
# Banned names: the game's executables and launchers, REDengine archives
# (*.archive), the game's top-level content trees (r6/…, archive/pc, red4ext,
# engine/config/platform, a "Cyberpunk 2077" install dir), and GOG installer /
# manifest names.  The plan docs and this tool directory are allow-listed.
#
# Exit 0 = clean, 1 = game artifact found, 2 = tooling error.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

fail=0
note() { printf 'check-no-game-files[cp]: %s\n' "$*"; }

# Basename globs (only `*` and `.` may appear), matched case-insensitively.
NAMES=(
  'Cyberpunk2077.exe' 'Cyberpunk2077.pdb' 'REDprelauncher.exe'
  'REDEngineErrorReporter.exe' '*.archive'
  'setup_cyberpunk_2077*' 'setup_cyberpunk*.exe' 'setup_cyberpunk*.bin'
  'goggame-*.info' 'goggame-*.hashdb' 'galaxy*.dll'
)
# Directory path suffixes, matched case-insensitively.
DIRS=(
  'r6/cache' 'r6/config' 'r6/scripts' 'archive/pc' 'red4ext'
  'engine/config/platform' 'Cyberpunk 2077'
)

allowed() { # $1 = relative path
  case "$1" in
    docs/cyberpunk*.md|docs/cp-*.md|tools/cp/*) return 0 ;;
  esac
  return 1
}

# --- 1. worktree names (find -iname / -ipath) ---------------------------------
name_expr=(); for p in "${NAMES[@]}"; do
  [ ${#name_expr[@]} -gt 0 ] && name_expr+=(-o); name_expr+=(-iname "$p"); done
dir_expr=(); for d in "${DIRS[@]}"; do
  [ ${#dir_expr[@]} -gt 0 ] && dir_expr+=(-o); dir_expr+=(-ipath "*/$d"); done

while IFS= read -r f; do
  f=${f#./}; allowed "$f" && continue
  note "BANNED PATH present: $f"; fail=1
done < <(find . -path ./.git -prune -o -type f \( "${name_expr[@]}" \) -print)

while IFS= read -r d; do
  d=${d#./}; allowed "$d" && continue
  note "BANNED DIR present: $d"; fail=1
done < <(find . -path ./.git -prune -o -type d \( "${dir_expr[@]}" \) -print)

# --- 2. index names (the same tables as anchored EREs over git ls-files) -----
glob2re() { printf '%s' "$1" | sed -e 's/[.]/\\./g' -e 's/[*]/[^\/]*/g'; }
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  re=''
  for p in "${NAMES[@]}"; do re="${re:+$re|}(^|/)$(glob2re "$p")\$"; done
  for d in "${DIRS[@]}";  do re="$re|(^|/)$(glob2re "$d")/"; done
  while IFS= read -r f; do
    allowed "$f" && continue
    note "BANNED PATH tracked: $f"; fail=1
  done < <(git ls-files | grep -iE "$re")
fi

# --- 3. content fingerprints -------------------------------------------------
# "size sha256-prefix(16) artifact" rows, filled in P0 from the user's GOG copy.
FINGERPRINTS=''

hit=0
while read -r size hash name; do
  [ -n "${size:-}" ] || continue
  while IFS= read -r f; do
    got=$(sha256sum "$f" | cut -c1-16)
    if [ "$got" = "$hash" ]; then
      note "GAME ARTIFACT (content match $name): ${f#./}"; hit=1
    fi
  done < <(find . -path ./.git -prune -o -type f -size "${size}c" -print 2>/dev/null)
done <<EOF
$FINGERPRINTS
EOF
[ "$hit" = 1 ] && fail=1

if [ "$fail" = 0 ]; then
  nfp=$(printf '%s' "$FINGERPRINTS" | grep -c . || true)
  note "OK — no Cyberpunk 2077 artifacts in the tree ($nfp content fingerprints)"
  exit 0
fi
note "FAIL — remove the file(s) above; the game runs from the user's own copy on the host"
exit 1
