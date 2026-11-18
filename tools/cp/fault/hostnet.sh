#!/usr/bin/env bash
# tools/cp/fault/hostnet.sh PATTERN OUT -- H7-fault (docs/cp-fault.md §3): the
# host side of one game boot's NFS traffic, read-only, 1 Hz, until the QEMU
# whose command line matches PATTERN has exited (waits up to 60 min for it: slot and big-guest waits).
# Rows:
#   U <s> <Udp InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors ...>
#   Q <s> socks=<n> rq_max=<bytes> drops=<sum of skmem d over QEMU's UDP sockets>
#   N <s> nfsd_drops=<skmem d of the :2049 UDP socket> rq=<its recv queue>
#   R <s> <the /proc/net/rpc/nfsd "th" and "rc" lines>
#   V <s> compact_stall= thp_fault_alloc= thp_fault_fallback=
# Nothing is written outside OUT.
set -u
PAT="${1:?pattern}"; OUT="${2:?out}"
pid=""
for _ in $(seq 3600); do
  pid=$(pgrep -f -- "qemu-system.*$PAT" | head -1)
  [ -n "$pid" ] && break
  sleep 1
done
[ -n "$pid" ] || { echo "# hostnet: no qemu matching $PAT" > "$OUT"; exit 1; }
{
  echo "# hostnet pid=$pid pattern=$PAT start=$(date -Is)"
  while kill -0 "$pid" 2>/dev/null; do
    s=$(date +%s)
    echo "U $s $(awk '/^Udp: [0-9]/ {$1=""; print}' /proc/net/snmp)"
    ss -uamnp 2>/dev/null | awk -v p="pid=$pid," -v s="$s" '
      /^UNCONN|^ESTAB/ { own = index($0, p) > 0; rq = $2; lp = $4; next }
      /skmem/ { if (match($0, /d[0-9]+\)/)) d = substr($0, RSTART + 1, RLENGTH - 2) + 0; else d = 0
                if (own) { n++; dsum += d; if (rq + 0 > rqm) rqm = rq + 0 }
                if (lp ~ /:2049$/) { nd += d; nrq = rq } }
      END { printf "Q %s socks=%d rq_max=%d drops=%d\nN %s nfsd_drops=%d rq=%d\n", s, n, rqm, dsum, s, nd, nrq }'
    echo "R $s $(grep -E '^(th|rc)' /proc/net/rpc/nfsd 2>/dev/null | tr '\n' ' ')"
    echo "V $s $(awk '/^(compact_stall|thp_fault_alloc|thp_fault_fallback) / {printf "%s=%s ", $1, $2}' /proc/vmstat)"
    sleep 1
  done
  echo "# hostnet end=$(date -Is)"
} > "$OUT" 2>&1
