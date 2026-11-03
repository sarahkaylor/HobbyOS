#!/usr/bin/env bash
# tools/rw/proc-sample.sh — sample a process's thread count / RSS over time.
#
#   proc-sample.sh <pid> <out.csv> [interval_s] [duration_s]
#
# Writes t_s,threads,rss_kb,vsz_kb,open_fds.  Used by trace-host.sh fps so the
# 125-thread / 1.37 GB claim is a time series, not a single eyeballed ps line.
set -uo pipefail
pid=${1:?pid}
out=${2:?out.csv}
interval=${3:-2}
dur=${4:-90}

printf 't_s,threads,rss_kb,vsz_kb,open_fds\n' >"$out"
start=$(date +%s)
while kill -0 "$pid" 2>/dev/null; do
  now=$(date +%s); t=$((now - start))
  [ "$t" -gt $((dur + 10)) ] && break
  vals=$(awk '/^Threads:/{th=$2} /^VmRSS:/{rss=$2} /^VmSize:/{vsz=$2}
               END{printf "%d,%d,%d", th+0, rss+0, vsz+0}' "/proc/$pid/status" 2>/dev/null)
  [ -z "$vals" ] && break
  nfd=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
  printf '%d,%s,%d\n' "$t" "$vals" "$nfd" >>"$out"
  sleep "$interval"
done
