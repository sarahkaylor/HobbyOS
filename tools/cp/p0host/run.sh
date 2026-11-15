#!/usr/bin/env bash
# tools/cp/p0host/run.sh <tag> <W> <H> [nav.py args...]
# One complete run: settings template -> (flock measure.lock) launch -> sampler -> nav -> stop.
# knobs: NOLOCK=1 (setup/exploration only), SETTINGS=<template json>, plus launch.sh knobs.
set -u
. "$(dirname "$0")/env.sh"
tag=$1; W=$2; H=$3; shift 3
R=$P0/runs/$tag
if [ -z "${P0HOST_LOCKED:-}" ] && [ -z "${NOLOCK:-}" ]; then
  # setup (settings) happens before taking the lock
  PFXDIR=${PFXDIR:-$PFX_PROTON}
  S="${SETTINGS_PATH:-$PFXDIR/$SETTINGS_REL}"
  cp "${SETTINGS:-$P0/settings-low-native.json}" "$S"
  python3 "$T/settings.py" "$S" set "/video/display/Resolution=${W}x${H}"
  mkdir -p "$R"
  echo "$(date +%s%N) waiting for $LOCK" >> "$R/events.log"
  P0HOST_LOCKED=1 exec flock "$LOCK" "$0" "$tag" "$W" "$H" "$@"
fi
if [ -n "${NOLOCK:-}" ]; then
  PFXDIR=${PFXDIR:-$PFX_PROTON}
  S="${SETTINGS_PATH:-$PFXDIR/$SETTINGS_REL}"
  cp "${SETTINGS:-$P0/settings-low-native.json}" "$S"
  python3 "$T/settings.py" "$S" set "/video/display/Resolution=${W}x${H}"
fi
mkdir -p "$R"
echo "$(date +%s%N) lock held=${P0HOST_LOCKED:-no}" >> "$R/events.log"
cat "$VRAM_USED" > "$R/vram_before_b"
if [ -n "${PRE_HOOK:-}" ]; then
  echo "$(date +%s%N) pre-hook: $PRE_HOOK" >> "$R/events.log"
  bash -c "$PRE_HOOK" >> "$R/events.log" 2>&1
fi
"$T/launch.sh" "$tag" "$W" "$H"
setsid python3 "$T/sampler.py" "$R" 1 > /dev/null 2>&1 < /dev/null &
SP=$!
python3 "$T/nav.py" "$tag" "$@" 2>&1 | tee "$R/nav.log"
rc=${PIPESTATUS[0]}
"$T/stop.sh" "$tag"
sleep 2; kill "$SP" 2>/dev/null
echo "$(date +%s%N) run end rc=$rc" >> "$R/events.log"
exit "$rc"
