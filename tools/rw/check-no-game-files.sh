#!/usr/bin/env bash
# tools/rw/check-no-game-files.sh — hard gate: no RimWorld artifact may enter git.
#
# The game is proprietary (Ludeon EULA) and this tree is public; docs/rimworld-plan.md
# §2 makes "no game files in git" a binding rule.  Three independent checks:
#
#   1. NAME over the WORKTREE: a banned file/dir lying in the tree is caught before
#      anyone runs `git add` (the first version only looked at `git ls-files` and a
#      planted UnityPlayer.so passed — measured).
#   2. NAME over the INDEX: `git ls-files` must not list a banned path even if it
#      is deleted from the worktree.
#   3. CONTENT: a file whose size matches a known game artifact and whose sha256
#      prefix matches it is the same file under a different name.
#
# Only fingerprints live here (sizes + sha256 prefixes) — never the files.
# Exit 0 = clean, 1 = game artifact found, 2 = tooling error.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT" || exit 2

fail=0
note() { printf 'check-no-game-files: %s\n' "$*"; }

# --- 1. worktree names ------------------------------------------------------
while IFS= read -r f; do
  note "BANNED PATH present: ${f#./}"; fail=1
done < <(find . -path ./.git -prune -o -type f \
           \( -name 'RimWorldLinux' -o -name 'UnityPlayer.so' \
              -o -name 'Assembly-CSharp*.dll' -o -name 'libmonobdwgc-*.so' \
              -o -name 'libmono-native.so' -o -name 'libMonoPosixHelper.so' \
              -o -name 'start_RimWorld*.sh' \) -print)

for d in RimWorldLinux_Data MonoBleedingEdge; do
  if [ -d "$d" ]; then note "BANNED DIR present: $d"; fail=1; fi
done

# --- 2. index names --------------------------------------------------------
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  while IFS= read -r f; do
    base=$(basename "$f")
    case "$base" in
      RimWorldLinux|UnityPlayer.so|Assembly-CSharp*.dll|libmonobdwgc-*.so|\
      libmono-native.so|libMonoPosixHelper.so|start_RimWorld*.sh)
        note "BANNED PATH tracked: $f"; fail=1 ;;
    esac
    case "$f" in
      RimWorldLinux_Data/*|*/RimWorldLinux_Data/*|MonoBleedingEdge/*|*/MonoBleedingEdge/*)
        note "BANNED PATH tracked: $f"; fail=1 ;;
    esac
  done < <(git ls-files)
fi

# --- 3. content fingerprints -----------------------------------------------
# size  sha256-prefix(16)  artifact
FINGERPRINTS='14720 0bfe51325720c950 RimWorldLinux
33010672 4c64f497b3f67fdb UnityPlayer.so
15767552 082db1dd4f7f1d0b Assembly-CSharp.dll'

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
  note "OK — no RimWorld artifacts in the tree"
  exit 0
fi
note "FAIL — remove the file(s) above; the game must run from the user's own copy (NFS)"
exit 1
