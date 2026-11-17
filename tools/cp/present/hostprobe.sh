#!/usr/bin/env bash
# tools/cp/present/hostprobe.sh -- host-side view of one Venus game boot (lane
# C6-present, docs/cp-present.md).  Read-only: it never changes the guest, the
# host settings or another process; perf/bpftrace only observe our own QEMU and
# its render server.
#
#   hostprobe.sh RUN_DIR [DELAY_S] [DUR_S] [PHASE]
#
# Waits until RUN_DIR/timeline.txt shows "phase PHASE" (default world), sleeps
# DELAY_S (default 25), then for DUR_S (default 12) s records, into
# RUN_DIR/host-*.txt:
#   host-threads.txt  CPU % (user/sys) and context switches per thread of the
#                     run's QEMU and of every render-server process under it
#   host-kvm.txt      KVM events of the QEMU process (exits by reason, MMIO by
#                     address, PIO by port, IRQ injections) via bpftrace
#   host-gpu.txt      gpu_busy_percent of the render GPU every 100 ms
#   host-perf.txt     perf record -g of QEMU + render server (top symbols per
#                     thread), the .data file deleted afterwards
set -uo pipefail
RUN=${1:?usage: hostprobe.sh RUN_DIR [DELAY_S] [DUR_S] [PHASE]}
DELAY=${2:-25}
DUR=${3:-12}
PHASE=${4:-world}
RENDER=${RENDER:-/dev/dri/renderD129}
for _ in $(seq 1 900); do [ -d "$RUN" ] && break; sleep 2; done
cd "$RUN" || exit 2
log() { echo "[hostprobe] $(date +%T) $*" | tee -a host-probe.log; }

for _ in $(seq 1 1800); do
  grep -q " phase $PHASE\$" timeline.txt 2>/dev/null && break
  grep -q "verdict:" timeline.txt 2>/dev/null && { log "run ended before phase $PHASE"; exit 1; }
  sleep 4
done
log "phase $PHASE seen; sleeping $DELAY s"
sleep "$DELAY"
QPID=$(pgrep -f -- "-serial file:$RUN/serial.log" | head -1)
[ -n "$QPID" ] || { log "no QEMU for $RUN"; exit 1; }
desc() { local p; for p in $(pgrep -P "$1"); do echo "$p"; desc "$p"; done; }
RPIDS=$(desc "$QPID" | tr '\n' ' ')
log "qemu $QPID render-server tree: $RPIDS; qemu guests on host: $(pgrep -c qemu-system)"
ALLP="$QPID $RPIDS"

snap() { # pid tid comm utime stime vcsw nvcsw
  local p t
  for p in $ALLP; do
    for t in /proc/$p/task/*; do
      [ -r "$t/stat" ] || continue
      local st comm rest
      st=$(cat "$t/stat" 2>/dev/null) || continue
      comm=${st#*(}; comm=${comm%)*}; rest=${st##*) }
      set -- $rest
      local v n
      v=$(awk '/^voluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null)
      n=$(awk '/^nonvoluntary_ctxt_switches/{print $2}' "$t/status" 2>/dev/null)
      echo "$p ${t##*/} ${comm// /_} ${12} ${13} ${v:-0} ${n:-0}"
    done
  done
}

GPUDEV=/sys/class/drm/${RENDER##*/}/device
( n=$((DUR * 10)); for _ in $(seq 1 $n); do cat "$GPUDEV/gpu_busy_percent"; sleep 0.1; done > host-gpu.raw ) &
GP=$!

BT=$(mktemp)
cat > "$BT" <<BPF
tracepoint:kvm:kvm_exit /pid == $QPID/ { @exit[args->exit_reason] = count(); }
tracepoint:kvm:kvm_userspace_exit /pid == $QPID/ { @uexit[args->reason] = count(); }
tracepoint:kvm:kvm_mmio /pid == $QPID/ { @mmio[args->type, args->gpa & 0xfffffffffffff000] = count(); }
tracepoint:kvm:kvm_fast_mmio /pid == $QPID/ { @fastmmio[args->gpa] = count(); }
tracepoint:kvm:kvm_pio /pid == $QPID/ { @pio[args->rw, args->port] = count(); }
tracepoint:kvm:kvm_set_irq /pid == $QPID/ { @set_irq[args->gsi, args->level] = count(); }
tracepoint:kvm:kvm_pic_set_irq /pid == $QPID/ { @pic_set_irq[args->pin, args->coalesced] = count(); }
tracepoint:kvm:kvm_ioapic_set_irq /pid == $QPID/ { @ioapic_set_irq[args->pin] = count(); }
tracepoint:kvm:kvm_msi_set_irq /pid == $QPID/ { @msi = count(); }
tracepoint:kvm:kvm_inj_virq /pid == $QPID/ { @inj_virq[args->vector] = count(); }
tracepoint:kvm:kvm_apic_ipi /pid == $QPID/ { @ipi = count(); }
tracepoint:kvm:kvm_vcpu_wakeup /pid == $QPID/ { @wakeup[args->waited] = count(); }
interval:s:$DUR { exit(); }
BPF
S0=$(snap); T0=$(date +%s.%N)
sudo -n bpftrace "$BT" > host-kvm.txt 2>&1
S1=$(snap); T1=$(date +%s.%N)
wait $GP
rm -f "$BT"

{
  echo "# host-threads: $DUR s window after phase $PHASE + $DELAY s; qemu $QPID; render tree $RPIDS"
  echo "# pid tid comm  user% sys% vcsw/s nvcsw/s   (100% = one host CPU)"
  python3 -I - "$T0" "$T1" <<PY
import sys
t0, t1 = float(sys.argv[1]), float(sys.argv[2]); dt = t1 - t0
hz = 100.0
a = {}
for l in """$S0""".splitlines():
    f = l.split()
    if len(f) == 7: a[f[1]] = f
rows = []
for l in """$S1""".splitlines():
    f = l.split()
    if len(f) != 7 or f[1] not in a: continue
    b = a[f[1]]
    u = (int(f[3]) - int(b[3])) / hz / dt * 100
    s = (int(f[4]) - int(b[4])) / hz / dt * 100
    v = (int(f[5]) - int(b[5])) / dt
    n = (int(f[6]) - int(b[6])) / dt
    rows.append((u + s, f[0], f[1], f[2], u, s, v, n))
rows.sort(reverse=True)
tot = {}
for r in rows:
    tot.setdefault(r[1], [0, 0]); tot[r[1]][0] += r[4]; tot[r[1]][1] += r[5]
    if r[0] >= 0.5 or r[6] >= 50:
        print("%7s %7s %-18s %6.1f %6.1f %8.0f %7.0f" % (r[1], r[2], r[3], r[4], r[5], r[6], r[7]))
for p, (u, s) in tot.items():
    print("# process %s total user %.1f%% sys %.1f%%" % (p, u, s))
PY
} > host-threads.txt
python3 -I -c '
import sys
v=[int(x) for x in open("host-gpu.raw").read().split()]
v.sort()
print("gpu_busy_percent samples=%d mean=%.1f p50=%d p90=%d max=%d" % (len(v), sum(v)/max(1,len(v)), v[len(v)//2], v[int(len(v)*0.9)], v[-1]) if v else "no samples")
' > host-gpu.txt
log "threads/kvm/gpu done; perf record"
sudo -n perf record -q -F 997 -g -o host-perf.data -p "$(echo $ALLP | tr ' ' ',')" -- sleep "$DUR" >/dev/null 2>&1
sudo -n perf report -i host-perf.data --no-children --sort comm,dso,sym --stdio -g none --percent-limit 0.4 2>/dev/null | grep -v '^$' | head -150 > host-perf.txt
sudo -n perf report -i host-perf.data --no-children --sort tid --stdio -g none 2>/dev/null | grep -v '^$' | head -60 > host-perf-tid.txt
sudo -n rm -f host-perf.data host-perf.data.old
log "done"
