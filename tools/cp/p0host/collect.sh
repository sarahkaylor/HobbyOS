#!/usr/bin/env bash
# tools/cp/p0host/collect.sh — build $P0/evidence/ (small text summaries only) from runs/.
# Never copies game files, the prefix, saves, or full logs; the Mac side pulls this dir.
set -u
. "$(dirname "$0")/env.sh"
E=$P0/evidence
rm -rf "$E"; mkdir -p "$E"/{runs,vk,va,strace,misc}
BR="$PFX_PROTON/pfx/drive_c/users/steamuser/Documents/CD Projekt Red/Cyberpunk 2077/benchmarkResults"

{ echo "# versions (collected $(date -Is))"
  echo "proton: $(cat "$PROTON_DIR/version")"
  echo "proton wine: $("$PROTON_DIR/files/bin/wine" --version 2>/dev/null)"
  echo "dxvk: $(cat "$PROTON_DIR/files/lib/wine/dxvk/version")"
  echo "vkd3d-proton: $(cat "$PROTON_DIR/files/lib/wine/vkd3d-proton/version")"
  echo "proton toolmanifest require_tool_appid: $(grep require_tool_appid "$PROTON_DIR/toolmanifest.vdf" | awk '{print $2}')"
  echo "distro wine: $(/usr/bin/wine --version 2>/dev/null)"
  echo "mesa: $(dpkg-query -W -f='${Version}' mesa-vulkan-drivers:amd64)"
  echo "kernel: $(uname -r)"
  echo "gamescope: $(dpkg-query -W -f='${Version}' gamescope)  mangohud: $(dpkg-query -W -f='${Version}' mangohud)"
  echo "strace: $(strace -V | head -1)"
  echo "game appmanifest buildid: $(grep -m1 buildid /mnt/fast/games/steamapps/appmanifest_1091500.acf | awk '{print $2}')"
  vulkaninfo --summary 2>/dev/null | grep -E "deviceName|apiVersion|driverInfo|deviceID" | head -8
} > "$E/misc/versions.txt"
cp "$P0/pe.txt" "$E/misc/pe-Cyberpunk2077.exe.txt" 2>/dev/null

for R in "$P0"/runs/*/; do
  t=$(basename "$R"); [ -f "$R/t0_ns" ] || continue
  case "$t" in explore*) continue ;; esac
  D=$E/runs/$t; mkdir -p "$D"
  cp "$R/meta.txt" "$R/events.log" "$D/" 2>/dev/null
  [ -f "$R/nav.log" ] && cp "$R/nav.log" "$D/"
  [ -f "$R/samples.csv" ] && { python3 "$T/summarise.py" "$R" > "$D/summary.txt" 2>&1; gzip -9c "$R/samples.csv" > "$D/samples.csv.gz"; }
  # newest MangoHud log written after this attempt's t0 (a re-used run dir may hold older ones)
  f=$(find "$R/mangohud" -name '*.csv' ! -name '*summary.csv' -newer "$R/t0_ns" 2>/dev/null | sort | tail -1)
  if [ -n "$f" ]; then
    python3 "$T/fps.py" "$f" > "$D/fps-menu-mangohud.txt" 2>&1; gzip -9c "$f" > "$D/mangohud-frames.csv.gz"
    cp "${f%.csv}_summary.csv" "$D/mangohud-summary.csv" 2>/dev/null
  fi
  b=$(sed -n 's/.*benchmark result \(benchmark_[0-9_-]*\).*/\1/p' "$R/events.log" | tail -1)
  if [ -n "$b" ] && [ -d "$BR/$b" ]; then
    cp "$BR/$b/summary.json" "$D/bench-summary.json"
    python3 "$T/fps.py" "$BR/$b/frames.csv" > "$D/fps-bench-game.txt"
    gzip -9c "$BR/$b/frames.csv" > "$D/bench-frames.csv.gz"
  fi
  for f in "$R"/proc_*_{status,smaps_rollup,limits,threads,arch_status}.txt; do [ -f "$f" ] && cp "$f" "$D/"; done
  [ -f "$R/proc_menu_maps.txt" ] && { cp "$R/proc_menu_maps.txt" "$D/"; python3 "$T/maps.py" "$R/proc_menu_maps.txt" > "$D/maps-menu-summary.txt"; }
  [ -f "$R/proc_bench_maps.txt" ] && python3 "$T/maps.py" "$R/proc_bench_maps.txt" > "$D/maps-bench-summary.txt"
  [ -f "$R/game_dir_changes.txt" ] && cp "$R/game_dir_changes.txt" "$D/"
  # Proton log: header + vkd3d/dxvk info lines + errors (not the whole multi-MB log)
  for L in "$R"/plog/*.log; do
    [ -f "$L" ] || continue
    sed -n '1,12p' "$L" > "$D/proton-log-head.txt"
    grep -E "vkd3d-proton|DXVK|dxvk|err:" "$L" | sed -E 's/^[0-9.]+:[0-9a-f]+:[0-9a-f]+://' | sort | uniq -c | sort -rn | head -150 > "$D/proton-log-vk-err.txt"
    grep -E "loaddll" "$L" | grep -oE 'Loaded L"[^"]+" at [0-9A-F]+: [a-z]+' | sort -u > "$D/proton-log-loaddll.txt"
  done
  tail -60 "$R/gamescope.log" > "$D/gamescope-tail.txt" 2>/dev/null
  if [ -f "$R/vkdump.txt" ]; then
    cp "$R/vkdump.txt" "$E/vk/vkdump.txt"; python3 "$T/vksum.py" "$R/vkdump.txt" > "$E/vk/vksum.txt"
  fi
  for f in "$R"/proc_*_tree.txt "$R"/proc_*_top.txt "$R"/proc_*_fds.txt; do [ -f "$f" ] && cp "$f" "$D/"; done
done
# strace summaries: fsync (Proton default) and in-server sync; launch->menu and 30 s at the menu
for t in trace trace-srvsync; do
  for w in summary-launch summary-menu30; do
    [ -d "$P0/runs/$t/$w" ] && { mkdir -p "$E/strace/$t/$w"; cp "$P0/runs/$t/$w/"*.txt "$E/strace/$t/$w/"; }
  done
  [ -f "$P0/runs/$t/read-sizes.txt" ] && cp "$P0/runs/$t/read-sizes.txt" "$E/strace/$t/"
  [ -f "$P0/runs/$t/trace.txt" ] && ls -l "$P0/runs/$t/trace.txt" > "$E/strace/$t/trace-size.txt"
done
du -sh "$E"; find "$E" -type f | wc -l
