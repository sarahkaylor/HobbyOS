#!/usr/bin/env bash
# fs-v2 T2 dry-run launcher — own instance v2, own port 8855, own lock.
# Uses V1's canonical multi-mode runner (unmerged from fs-v1 lane; my tree's
# tools/ copy is the legacy WC runner).  Evidence lands in MY lane dir.
set -u
LANE=/home/sarah/hobbyos-lanes/fs-v2
V1_RUNNER=/home/sarah/hobbyos-lanes/fs-v1/tools/run_browser_accept.sh
DISK=/home/sarah/hobbyos-lanes/fs-v1/disk.img        # merged-tree browser (pre-R1-fix), proven at tip
MODE="${1:-fixture}"
TAG="${2:-dry-fixture}"
EVDIR="$LANE/continuation/fs-v2/evidence/$TAG"
# authoritative fixture dirs per the canonical mapping (fs-v2 fork + OS tree)
export WIKI_PROXY_FIXTURES="/home/sarah/webkit-lanes/fs-v2/HobbyOS/continuation/wk3/fixtures:$LANE/tests/fixtures/browser"
export ACCEPT_ID=v2
flock -w 7200 /tmp/fs-accept-v2.lock bash "$V1_RUNNER" \
  --mode "$MODE" --instance v2 --port 8855 \
  --evdir "$EVDIR" --disk "$DISK" --topic Hobbyist_operating_system
