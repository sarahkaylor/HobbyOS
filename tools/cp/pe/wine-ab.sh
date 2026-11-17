#!/usr/bin/env bash
# tools/cp/pe/wine-ab.sh TREE TAG N -- C1-pe: N Gate P5 sessions in TREE
# (docs/cp-pe.md), each graded for Wine PE-loader faults.  Evidence goes to
# $PE_EV (default ~/cp/lanes/pe-ev): TAG-runK.serial and TAG-summary.txt.
#
# One session = tools/cp/p5/run-wine-p5.sh with the wave-5 rerun line
# (wineboot, the keepalive vkcube, then vkcube / d3d11 / d3d12 for >= 60 s
# each) -- many Wine processes starting at once on NFS, as in C4-x3's runs.
# A run's fault count is every first "[KERNEL] pid=N (X.exe) memory fault"
# row per Wine process, plus Wine's own "Unhandled page fault/exception" and
# empty-name import_dll lines.  A fault PC inside ntdll.dll is symbolized
# (llvm-symbolizer on the build's ntdll.dll, rebased to its ImageBase).
set -uo pipefail
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TREE="$(cd "${1:?usage: wine-ab.sh TREE TAG N}" && pwd)"
TAG="${2:?tag}"
N="${3:-6}"
EV="${PE_EV:-$HOME/cp/lanes/pe-ev}"; mkdir -p "$EV"
WB="$HOME/cp/wine-build-x"
NTDLL="$WB/build-x86_64/dlls/ntdll/x86_64-windows/ntdll.dll"
SUM="$EV/$TAG-summary.txt"
# PE_AB_MODE=storm: tools/cp/pe/winestorm.c instead of the P5 probes (wineboot,
# keepalive, warm-up, then PE_ROUNDS x PE_PAR concurrent Wine starts).  The
# P5 runner is reused by a generated copy that builds that launcher.
RUNNER=tools/cp/p5/run-wine-p5.sh
if [ "${PE_AB_MODE:-p5}" = storm ]; then
  mkdir -p "$TREE/tools/cp/pe"
  [ "$TREE" = "$SELF" ] || cp "$SELF/tools/cp/pe/winestorm.c" "$TREE/tools/cp/pe/winestorm.c"
  sed -e 's#tools/cp/p5/winep5.c#tools/cp/pe/winestorm.c#' "$TREE/tools/cp/p5/run-wine-p5.sh" > "$TREE/tools/cp/pe/.p5storm.sh"
  RUNNER=tools/cp/pe/.p5storm.sh
  export P5_CFLAGS="-DPE_ROUNDS=${PE_ROUNDS:-8} -DPE_PAR=${PE_PAR:-6}" P5_PROBES=storm
fi
echo "# $TAG: $N Gate P5 sessions in $TREE ($(date -u +%FT%TZ), host strix)" >> "$SUM"
sym() { # sym PC -> loader function for a PC in ntdll's PE image, else ""
  local pc=$((16#${1#0x})) base=$((16#6FFFFFF40000)) img=$((16#170000000))
  if [ "$pc" -ge "$base" ] && [ "$pc" -lt $((base + 16#E2000)) ] && [ -f "$NTDLL" ]; then
    llvm-symbolizer --obj="$NTDLL" "$(printf '0x%x' $((pc - base + img)))" 2>/dev/null | head -2 | tr '\n' ' '
  fi
}
for k in $(seq 1 "$N"); do
  OUTLOG="$EV/$TAG-run$k.out"
  ( cd "$TREE" && CP_WINE_STAGE=$HOME/cp/wine-build-x/stage/CP/WINE WINE_BUILD=$WB CP_VK_WORK=$HOME/cp/vk2-build \
      CP_X11_WORK=$HOME/cp/x11-build-x2 CP_P5_PE=$HOME/cp/vk2-pe P5_EXPORT="/srv/nfs/cp-pe-p5-$TAG" \
      P5_XHOBBY=$HOME/cp/vk2-scratch/x2-xserver/XHOBBY.BIN \
      P5_XHOBBY_SHA256=935c760fd450eb9a3326e75691a9b8feaacf155f19033e807f98a4bf78424c97 \
      P5_XSESSION_ENV="HB_SECOND_SCREEN=0" PATH=$HOME/.local/bin:$PATH MAKEFLAGS=-j16 TOOLS_SKIP=1 LO_ON_DISK= \
      bash "$RUNNER" > "$OUTLOG" 2>&1 )
  RC=$?
  S="$TREE/obj/cp-p5/wine/serial.log"
  cp "$S" "$EV/$TAG-run$k.serial" 2>/dev/null
  S="$EV/$TAG-run$k.serial"
  # first fault row per Wine process (pid, name)
  FR="$(grep -aE '\[KERNEL\] pid=[0-9]+ \([^)]*\.exe\) memory fault' "$S" 2>/dev/null |
        awk '{ if (!seen[$2]++) print }')"
  NK=$(printf '%s' "$FR" | grep -c 'memory fault')
  # every fatal signal of a process leaves one [SIGDUMP] row (a #GP has no
  # "memory fault" row): distinct pids of .exe processes
  NS=$(grep -aE '\[SIGDUMP\] pid=[0-9]+ .* nm=[^ ]*\.exe' "$S" 2>/dev/null | sed 's/ tgid.*nm=/ /; s/ rip=.*//' | sort -u | wc -l)
  NU=$(grep -acE 'Unhandled (page fault|exception)' "$S" 2>/dev/null)
  NI=$(grep -acE 'err:module:import_dll Library  \(' "$S" 2>/dev/null)
  NPE=$(grep -ac '\[PECOW\]' "$S" 2>/dev/null)
  LASTPE=$(grep -a '\[PECOW\]' "$S" 2>/dev/null | tail -1 | sed 's/.*cow_with_peers=\([0-9]*\) cows=\([0-9]*\).*/peers=\1 cows=\2/')
  PS=$(grep -a '^\[PS\] storm done' "$S" 2>/dev/null | tail -1 | sed 's/^\[PS\] storm done //')
  P5=$(grep -a '^\[P5W\] \(PASS\|FAIL\)' "$OUTLOG" "$S" 2>/dev/null | tail -1 | sed 's/^[^[]*//' | cut -c1-80)
  echo "run $k rc=$RC fault_procs=$NK sigdump_procs=$NS unhandled=$NU emptyname_imports=$NI pecow_rows=$NPE ${LASTPE:+last_pecow($LASTPE)} ${PS:+storm($PS)} p5=\"$P5\"" | tee -a "$SUM"
  printf '%s\n' "$FR" | grep 'memory fault' | while IFS= read -r row; do
    pc=$(printf '%s' "$row" | sed -n 's/.* PC=\(0x[0-9A-Fa-f]*\).*/\1/p')
    echo "    $(printf '%s' "$row" | cut -c1-190)  $(sym "$pc")" | tee -a "$SUM"
  done
  grep -aE '\[SIGDUMP\]|Unhandled (page fault|exception)|err:module:import_dll Library  \(' "$S" 2>/dev/null | head -6 | sed 's/^/    wine: /' | tee -a "$SUM"
done
