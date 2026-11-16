#!/usr/bin/env bash
# tools/cp/nfs/rsize_guard.sh [tree] — cp/nfsguard: the boot guard for 32 KiB
# NFS READs (docs/cp-nfs.md, "Regression guard").
#
# The 32 KiB READ fix has been made and then lost more than once.  This boots
# the real kernel against a real nfsd (via tools/cp/nfs/nfs_bench.sh: one
# localhost-only read-only export, /etc/exports lock and restore included),
# reads 64 MiB, and FAILs (rc 1) unless the guest actually used large READs:
#
#   G1  the data is right: [NB] done rc=0 (FNV-1a-64 match on 64 MiB)
#   G2  the mount learned the size: "[NFS] fsinfo rc=0 ... rsize=32768"
#   G3  no "[NFS] rsize fallback" (large replies were lost and it dropped
#       back to 1344-byte READs)
#   G4  every sampled "[NFSPIPE] len=65536" row has rs=32768 and issued<=2
#       (at least one row must be present)
#   G6  6 concurrent readers of reverse-order 4 KiB pages get the right
#       bytes (a readahead window handed out mid-refill corrupted Wine's
#       page-ins, 2026-11-16)
#   G7  (cp/nfs2) 6 concurrent readers of 6 DIFFERENT files get every 4 KiB
#       page right, and the phase fetches at most 2x what it reads
#       ("[NB] multi6.ra ... amp_pct=" <= 200): one shared readahead window
#       refilled 64 KiB for almost every page (~1600%)
#   G8  (cp/nfs2) one read() of 176 MiB + 777 bytes returns all of it with
#       the host's SHA-256 ("[NB] one176m ... match ... ok"); fstat/stat of an
#       NFS file give the server's atime/mtime/ctime; files behind symlinked
#       directories open, stat and read right (5 "[NB] symlink" rows); with
#       NB_BIG=1 also the 5 GiB SHA-256 row ("[NB] big5g").
#   G9  (cp/nfs2) lost replies never become data: with every 5th NFS reply
#       dropped and with every reply dropped for 8 s (a test kernel's fault
#       injection), fresh files read page by page and mapped come back
#       right -- no FAIL "[NB] inject." row, at least 5 rows
#   G5  over all READ RPCs (the last "[NFSRTT] periodic" set): bytes / RPCs
#       >= 16384.  1344-byte READs give ~1344; 32 KiB READs give ~32768.
#
# rc 2 means the guard could not judge: the build failed, the export could
# not be set up, or the server offers less than 32 KiB (rtmax/rtpref), so
# 32 KiB READs are impossible there.  Run it where the server offers 32 KiB
# (strix's kernel nfsd answers rtmax=rtpref=32768 over UDP).
#
# Run it after any merge that touches src/kernel/nfs.c, src/include/nfs.h or
# src/kernel/net.c, and in every integration gate batch.  The fast host-side
# tripwire for the same regressions is tools/cp/nfs/rsize_scan.py, part of
# `make host_tests`.
set -uo pipefail
SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
TREE="$(cd "${1:-$SELF}" && pwd)"
OUT="$TREE/obj/nfs-bench"
LOG="$OUT/rsize-guard.bench.log"

if [ -n "${RSIZE_GUARD_CONSOLE:-}" ]; then
  # Judge an existing guest console log (no build, no boot).  Used to test the
  # guard itself against before/after logs.
  C="$RSIZE_GUARD_CONSOLE"; LOG="(none: RSIZE_GUARD_CONSOLE)"; brc=0
else
  NFS_RSIZE_SCAN_ROOT="$TREE" python3 "$SELF/tools/cp/nfs/rsize_scan.py" \
    || echo "rsize_guard: (source scan failed too; booting anyway)"
  mkdir -p "$TREE/obj"
  bash "$SELF/tools/cp/nfs/nfs_bench.sh" "$TREE" > "$TREE/obj/rsize-guard.bench.log" 2>&1
  brc=$?
  mkdir -p "$OUT"; mv -f "$TREE/obj/rsize-guard.bench.log" "$LOG"
  C="$OUT/console.log"
fi
if [ ! -s "$C" ]; then
  tail -15 "$LOG"
  echo "rsize_guard: ERROR (no guest console; bench rc=$brc; see $LOG)"; exit 2
fi

fail=0
note() { echo "rsize_guard: $*"; }
bad()  { echo "rsize_guard: FAIL $*"; fail=1; }

# G1
grep -aq '^\[NB\] done rc=0' "$C" || bad "G1 a bench phase failed (bad byte count or checksum) ($(grep -a '^\[NB\]' "$C" | tail -2 | tr '\n' ' '))"

# G6 (cp/nfsfix): 6 concurrent readers of 4 KiB pages all saw the right bytes
# rows are matched anywhere in a line: kernel output interleaves the console
# (measured: "[NFSRA] hits=276 fills=[NB] conc6.child a=1 ... FAIL").
CR="$(grep -aoE '\[NB\] conc6(\.child)? a=[0-9]+ pages=[0-9]+ bad=[0-9]+ first_bad=[-0-9]+ (ok|FAIL)' "$C")"
if [ -z "$CR" ]; then bad "G6 no [NB] conc6 rows (the concurrent-reader phase did not run)"
else
  nc=$(grep -c '^\[NB\] conc6\.child ' <<<"$CR"); nbad=$(grep -c ' FAIL$' <<<"$CR")
  if [ "$nc" -ne 6 ] || [ "$nbad" -ne 0 ]; then bad "G6 concurrent 4 KiB readers: $nc/6 reported, $nbad FAIL rows: $(grep ' FAIL$' <<<"$CR" | head -2 | tr '\n' ' ')"
  else note "G6 6/6 concurrent readers, every 4 KiB page correct"; fi
fi

# G7 (cp/nfs2): interleaved readers of different files, per-file windows
MR="$(grep -aoE '\[NB\] multi6(\.child)? a=[0-9]+ pages=[0-9]+ bad=[0-9]+ first_bad=[-0-9]+ (ok|FAIL)' "$C")"
MA="$(grep -aoE '\[NB\] multi6\.ra read_kib=[0-9]+ fetched_kib=[0-9]+ rpcs=[0-9]+ amp_pct=[0-9]+ hit_pct_est=[0-9]+' "$C" | tail -1)"
if [ -z "$MR" ] || [ -z "$MA" ]; then bad "G7 no [NB] multi6 rows (the multi-file phase did not run)"
else
  nc=$(grep -c '^\[NB\] multi6\.child ' <<<"$MR"); nbad=$(grep -c ' FAIL$' <<<"$MR")
  amp=$(sed -n 's/.*amp_pct=\([0-9]*\).*/\1/p' <<<"$MA")
  if [ "$nc" -ne 6 ] || [ "$nbad" -ne 0 ]; then bad "G7 multi-file readers: $nc/6 reported, $nbad FAIL rows: $(grep ' FAIL$' <<<"$MR" | head -2 | tr '\n' ' ')"
  elif [ "${amp:-99999}" -gt 200 ]; then bad "G7 readahead amplification: $MA (want amp_pct <= 200)"
  else note "G7 6/6 readers of 6 files, every page correct; ${MA#\[NB\] multi6.ra }"; fi
fi

# G8 (cp/nfs2): large reads, stat times, symlinked directories
G8="$(tr -d '\r' < "$C" | grep -aoE '\[NB\] (one176m|big5g|fstat|stat|symlink) .*(ok|FAIL)$')"
n_one=$(grep -c '^\[NB\] one176m .* match .* ok$' <<<"$G8"); n_t=$(grep -cE '^\[NB\] f?stat .* ok$' <<<"$G8")
n_sl=$(grep -c '^\[NB\] symlink .* ok$' <<<"$G8"); n_f=$(grep -c ' FAIL$' <<<"$G8")
if [ "$n_one" -ne 1 ] || [ "$n_t" -ne 2 ] || [ "$n_sl" -ne 5 ] || [ "$n_f" -ne 0 ]; then
  bad "G8 one176m=$n_one/1 stat-times=$n_t/2 symlink=$n_sl/5 FAIL rows=$n_f: $(grep ' FAIL$' <<<"$G8" | head -3 | tr '\n' ' ')"
else note "G8 one 176 MiB read() hash match, stat times, 5/5 symlinked paths$(grep -q '^\[NB\] big5g .* ok$' <<<"$G8" && echo ', big5g sha256 match')"; fi

# G9 (cp/nfs2): hard-mount semantics under injected reply loss
G9="$(tr -d '\r' < "$C" | grep -aoE '\[NB\] inject\.[a-z0-9.]+ pages=[0-9]+ .*(ok|FAIL)$')"
n9=$(grep -c . <<<"$G9"); f9=$(grep -c ' FAIL$' <<<"$G9")
if [ -z "$G9" ] || [ "$n9" -lt 5 ] || [ "$f9" -ne 0 ]; then
  bad "G9 injected reply loss: $n9 rows, $f9 FAIL: $(grep ' FAIL$' <<<"$G9" | head -3 | tr '\n' ' ')"
else note "G9 $n9/$n9 injected-loss rows right ($(grep -o 'inject.blackout.read [^ ]* [^ ]* [^ ]* [^ ]* ms=[0-9]*' <<<"$G9"))"; fi

# G2 (and the can't-judge case)
# Only well-formed rows are judged: two CPUs writing the console can garble a
# row into nonsense (measured: "rs=32768[NFSRTT]  issued=periodic2 ...").
FS="$(grep -aoE '\[NFS\] fsinfo rc=-?[0-9]+ rtmax=[0-9]+ rtpref=[0-9]+ rsize=[0-9]+' "$C" | tail -1)"
if [ -z "$FS" ]; then
  bad "G2 no '[NFS] fsinfo' row: the mount never asked the server for its READ size"
else
  note "$FS"
  rtmax="$(sed -n 's/.*rtmax=\([0-9]*\).*/\1/p' <<<"$FS")"
  rtpref="$(sed -n 's/.*rtpref=\([0-9]*\).*/\1/p' <<<"$FS")"
  rsize="$(sed -n 's/.*rsize=\([0-9]*\).*/\1/p' <<<"$FS")"
  if grep -q 'rc=0' <<<"$FS" && { [ "${rtmax:-0}" -lt 32768 ] || [ "${rtpref:-0}" -lt 32768 ]; }; then
    echo "rsize_guard: ERROR server offers rtmax=$rtmax rtpref=$rtpref (< 32 KiB): cannot judge here"; exit 2
  fi
  grep -q 'rc=0' <<<"$FS" || bad "G2 FSINFO failed ($FS)"
  [ "${rsize:-0}" = 32768 ] || bad "G2 mount READ size is ${rsize:-?}, want 32768"
fi

# G3
grep -aq '\[NFS\] rsize fallback' "$C" && bad "G3 $(grep -a '\[NFS\] rsize fallback' "$C" | head -1)"

# G4
P="$(grep -aoE '\[NFSPIPE\] len=65536 rs=[0-9]+ issued=[0-9]+ replies=[0-9]+ ' "$C")"
if [ -z "$P" ]; then
  bad "G4 no [NFSPIPE] len=65536 rows (the pipelined READ path never ran)"
else
  np=$(wc -l <<<"$P" | tr -d ' ')
  wrong="$(awk '{rs="";is="";for(i=1;i<=NF;i++){if($i~/^rs=/)rs=substr($i,4);if($i~/^issued=/)is=substr($i,8)} if(rs!=32768||is+0>2)print}' <<<"$P")"
  if [ -n "$wrong" ]; then bad "G4 $(wc -l <<<"$wrong" | tr -d ' ')/$np 64 KiB reads not done as <=2 x 32 KiB RPCs, e.g. $(head -1 <<<"$wrong")"
  else note "G4 $np/$np sampled 64 KiB reads: rs=32768 issued<=2"; fi
fi

# G5: the last periodic set = consecutive "[NFSRTT] periodic want=" rows at the end
R="$(awk '/^\[NFSRTT\] periodic want=/{if(!inset){set="";inset=1} set=set $0 "\n"; next} {if(inset)last=set; inset=0} END{if(inset)last=set; printf "%s", last}' "$C")"
if [ -z "$R" ]; then
  bad "G5 no [NFSRTT] periodic rows"
else
  read -r rpcs bytes < <(awk '{for(i=1;i<=NF;i++){if($i~/^n=/)n+=substr($i,3); if($i~/^bytes=/)b=substr($i,7)}} END{print n+0, b+0}' <<<"$R")
  if [ "${rpcs:-0}" -le 0 ]; then bad "G5 no READ RPCs counted"
  else
    avg=$(( bytes / rpcs ))
    per64=$(awk -v r="$rpcs" -v b="$bytes" 'BEGIN{printf "%.1f", r*65536/b}')
    if [ "$avg" -lt 16384 ]; then bad "G5 average READ RPC carries $avg bytes ($per64 RPCs per 64 KiB; want >= 16384, i.e. <= 4)"
    else note "G5 $rpcs READ RPCs for $bytes bytes: $avg bytes/RPC, $per64 RPCs per 64 KiB"; fi
  fi
fi

grep -aE '^\[NB\] (seq65536|one176m|big5g)' "$C" | sed 's/^/rsize_guard: /'
if [ $fail -ne 0 ]; then echo "rsize_guard: FAIL ($TREE; console $C)"; exit 1; fi
echo "rsize_guard: PASS ($TREE)"; exit 0
