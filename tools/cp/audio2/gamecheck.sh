#!/usr/bin/env bash
# tools/cp/audio2/gamecheck.sh -- one line per game audio boot from the
# run-hunt evidence (lane H5-audio2, docs/cp-audio2.md §4.3):
#   gamecheck.sh LOGDIR RUN...
# prints WAV dropouts (wavgaps.py), alsa-lib's printed underruns, the kernel's
# xruns and held-back xruns (sums of the [SND] stat rows: a game never closes
# its PCM), the most the device returned in 100 ms, the first 32 xrun rows
# classified (writer late = wr_ago_us > 40 ms; device fast = done40ms > 2400,
# 1.25x real time), and the console's host-stall count.
set -u
D="${1:?logdir}"; shift
for r in "$@"; do
  S="$D/$r.serial.txt"; W="$D/$r.wav.txt"
  [ -f "$S" ] || { echo "$r: no $S"; continue; }
  sum() { grep -aoE "\[SND\] stat .*" "$S" | grep -oE "$1=\+[0-9]+" | tr -dc '0-9\n' | awk '{s+=$1} END {print s+0}'; }
  mx=$(grep -aoE "\[SND\] stat .*" "$S" | sed -n 's/.* max100ms=\([0-9]*\).*/\1/p' | sort -n | tail -n 1)
  cls=$(grep -aoE '\[SND\] xrun n=.*' "$S" | awk '{for(i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]}
        w=(v["wr_ago_us"]>40000); d=(v["done40ms"]>2400); k=(w?"wlate":"wok") "/" (d?"dfast":"drt"); c[k]++}
        END {for (k in c) printf "%s:%d ", k, c[k]}')
  echo "$r dropouts=$(sed -n 's/^wavgaps: dropouts=//p' "$W" 2>/dev/null) dropout_ms=$(sed -n 's/^wavgaps: dropout_ms_total=//p' "$W" 2>/dev/null)" \
    "wav_s=$(sed -n 's/^wavgaps: duration_s=//p' "$W" 2>/dev/null) alsa_underrun_rows=$(grep -ac 'snd_pcm_recover) underrun' "$S")" \
    "kernel_xruns=$(sum xruns) held=$(sum held) max100ms=${mx:-?} xrun_rows[$cls]" \
    "$(grep -aoE '\[CONS\] lines=.*stolen_ms=[0-9]+' "$S" | tail -n 1 | grep -oE 'stalls=[0-9]+ stolen_ms=[0-9]+')"
done
