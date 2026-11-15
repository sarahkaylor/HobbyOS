#!/usr/bin/env bash
# tools/cp/p0host/check-no-game-files.sh — hard gate: no Cyberpunk 2077 artifact may enter git.
#
# Cloned from tools/rw/check-no-game-files.sh (docs/cyberpunk.md AD-12) and adapted to the
# Steam build measured by lane C7-p0host (game 2.5).  Three independent checks:
#   1. NAME over the WORKTREE (a banned file lying in the tree, before `git add`)
#   2. NAME over the INDEX (`git ls-files`)
#   3. CONTENT: size + sha256 prefix of known game binaries, under any name
# Also refuses Wine-prefix artifacts that would carry the user's data (saves, registry).
# Only fingerprints live here — never the files.  Exit 0 clean, 1 artifact found, 2 error.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT" || exit 2
fail=0
note() { printf 'check-no-game-files(cp): %s\n' "$*"; }

BANNED_FIND=( -name 'Cyberpunk2077.exe' -o -name '*.archive' -o -name '*.bk2' -o -name 'steam_api64.dll'
  -o -name 'REDGalaxy64.dll' -o -name 'GameServicesSteam.dll' -o -name 'bink2w64.dll' -o -name 'oo2ext_7_win64.dll'
  -o -name 'PhysX3*_x64.dll' -o -name 'redlexer_native.dll' -o -name 'REDprelauncher.exe'
  -o -name 'REDEngineErrorReporter.exe' -o -name 'oodle_dictionary.bin' -o -name '*.dat' -o -name 'sav.dat'
  -o -name 'user.reg' -o -name 'system.reg' -o -name 'userdef.reg' -o -name 'vkd3d-proton.cache*'
  -o -name 'GamePipelineLibrary.cache' -o -name 'UserSettings.json' )

# --- 1. worktree names ------------------------------------------------------
while IFS= read -r f; do
  note "BANNED PATH present: ${f#./}"; fail=1
done < <(find . -path ./.git -prune -o -path ./third_party -prune -o -type f \( "${BANNED_FIND[@]}" \) -print)
for d in archive/pc/content r6/scripts "Cyberpunk 2077" compatdata/1091500; do
  if [ -d "$d" ]; then note "BANNED DIR present: $d"; fail=1; fi
done

# --- 2. index names --------------------------------------------------------
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  while IFS= read -r f; do
    base=$(basename "$f")
    case "$base" in
      Cyberpunk2077.exe|*.archive|*.bk2|steam_api64.dll|REDGalaxy64.dll|GameServicesSteam.dll|bink2w64.dll|\
      oo2ext_7_win64.dll|PhysX3*_x64.dll|redlexer_native.dll|REDprelauncher.exe|REDEngineErrorReporter.exe|\
      oodle_dictionary.bin|sav.dat|user.reg|system.reg|userdef.reg|vkd3d-proton.cache*|GamePipelineLibrary.cache|\
      UserSettings.json)
        note "BANNED PATH tracked: $f"; fail=1 ;;
    esac
    case "$f" in
      third_party/*) ;;
      *.dat) note "BANNED PATH tracked (.dat): $f"; fail=1 ;;
    esac
  done < <(git ls-files)
fi

# --- 3. content fingerprints (sizes from `ls -l`, sha256 from pe.py, 2026-11-14) -----
FINGERPRINTS='66704848 d36e74c47592e9c6 Cyberpunk2077.exe'
hit=0
while read -r size hash name; do
  [ -n "${size:-}" ] || continue
  while IFS= read -r f; do
    got=$(sha256sum "$f" | cut -c1-16)
    [ "$got" = "$hash" ] && { note "GAME ARTIFACT (content match $name): ${f#./}"; hit=1; }
  done < <(find . -path ./.git -prune -o -type f -size "${size}c" -print 2>/dev/null)
done <<EOF2
$FINGERPRINTS
EOF2
# any file > 64 MiB in the lane's evidence/tools is suspicious (archives are GBs, logs too big)
while IFS= read -r f; do
  note "OVERSIZE (>64 MiB) file in lane dirs: $f"; hit=1
done < <(find tools/cp/p0host evidence/cp-p0host docs/cp-p0-host.md -type f -size +65536k 2>/dev/null)
[ "$hit" = 1 ] && fail=1

if [ "$fail" = 0 ]; then note "OK — no Cyberpunk artifacts in the tree"; exit 0; fi
note "FAIL — remove the file(s) above; the game runs from the user's own install"
exit 1
