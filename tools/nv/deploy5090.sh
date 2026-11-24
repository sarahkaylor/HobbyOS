#!/bin/bash
# deploy5090.sh -- build a HobbyOS image and boot it on r1 VM 313 (RTX 5090, PCI 41:00).
#
# The 5090 lane holds an ssh key whose authorised_keys entry is a FORCED COMMAND:
# /usr/local/bin/nv5090 on r1 accepts only status|config|start|stop|reset|serial|push-image
# and hardcodes VMID 313. So this script never runs `qm` itself -- it pushes the image and
# asks the wrapper to start the VM. It cannot touch VM 300 (the 4090) or the host.
#
#   tools/nv/deploy5090.sh [--mode test|unit_tests|desktop] [--wave NAME]
#
# Env: R1_HOST (default root@192.168.10.174), R1_KEY (default ~/.ssh/id_r1_5090),
#      SSH_OPTS, SKIP_BUILD=1 to reuse the existing disk.img.
set -u
R1_HOST="${R1_HOST:-root@192.168.10.174}"
R1_KEY="${R1_KEY:-$HOME/.ssh/id_r1_5090}"
MODE=test
WAVE=""
DISK_MB=${DISK_MB:-1024}

while [ $# -gt 0 ]; do
  case "$1" in
  --mode) MODE="$2"; shift 2 ;;
  --wave) WAVE="$2"; shift 2 ;;
  -h | --help) sed -n '2,16p' "$0"; exit 0 ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say() { printf '[5090] %s\n' "$*"; }
r1() { ssh -i "$R1_KEY" -o BatchMode=yes -o StrictHostKeyChecking=no "$R1_HOST" "$@"; }

if [ "${SKIP_BUILD:-0}" = "1" ]; then
  [ -f disk.img ] || { echo "no disk.img to deploy" >&2; exit 1; }
else
  say "building ARCH=intel MODE=$MODE"
  make ARCH=intel MODE="$MODE" LO_ON_DISK= TOOLS_SKIP=1 BROWSER_SKIP=1 \
    -j"$(nproc 2>/dev/null || echo 8)" disk.img || { echo "build failed" >&2; exit 1; }
fi

if [ -n "$WAVE" ]; then
  printf '%s\n' "$WAVE" > /tmp/nv-wave-5090.cfg
  mcopy -o -i disk.img /tmp/nv-wave-5090.cfg ::/WAVE.CFG || exit 1
  say "staged /WAVE.CFG = $WAVE"
fi

say "pushing image to VM 313"
gzip -1 -c disk.img | r1 push-image || { echo "image push refused" >&2; exit 1; }

say "starting VM 313"
r1 start

if [ "$MODE" != "desktop" ]; then
  # test/unit_tests guests halt themselves; poll the wrapper's status.
  for _ in $(seq 1 60); do
    st=$(r1 status 2>/dev/null | awk '{print $2}')
    [ "$st" != "running" ] && break
    sleep 5
  done
  r1 status || true
fi

OUT="evidence/nv/5090-${MODE}-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$(dirname "$OUT")"
r1 serial 200000 >"$OUT" 2>/dev/null
say "serial log: $OUT ($(wc -l <"$OUT" 2>/dev/null | tr -d ' ') lines)"
