#!/usr/bin/env bash
# tools/rw/trace-host.sh — reproducible host-side probes for the RimWorld
# Linux build (Unity 2022.3.35f1 + Mono), headless on Xvfb + llvmpipe.
#
# One mode per invocation, so an fps number is never taken while a tracer is
# running.  Everything lands in tools/rw/out/ (git-ignored); the committed
# artifact is docs/rimworld-evidence.md.
#
#   ./trace-host.sh fps      [secs]   # GALLIUM_HUD fps dump + thread/RSS sampler
#   ./trace-host.sh syscalls [secs]   # strace -f -c  -> out/strace-summary.txt
#   ./trace-host.sh sample   [secs]   # strace -f -e trace=<set> -> out/strace-sample.txt
#   ./trace-host.sh libs     [secs]   # LD_DEBUG=libs,files -> out/ld-debug-libs.txt
#   ./trace-host.sh bindings [secs]   # LD_DEBUG=bindings -> out/ld-debug-bindings.txt
#   ./trace-host.sh dlsym    [secs]   # ltrace dlopen/dlsym -> out/ltrace-dlsym.txt
#   ./trace-host.sh xcount   [secs]   # ltrace -c 'X*' -> out/ltrace-xcount.txt
#   ./trace-host.sh symbols           # merge dlsym+bindings -> tools/rw/x11-symbols.txt
#   ./trace-host.sh gl       [secs]   # apitrace trace -a gl -> out/rw.trace
#
# Environment (all optional):
#   RW_GAME        game dir        (default /mnt/fast/games/steamapps/common/RimWorld)
#   RW_OUT         output dir      (default <script>/out)
#   RW_DISPLAY     X display       (default :90; bumped to the first free one)
#   RW_WIDTH/RW_HEIGHT             (default 1280x720)
#   RW_LP_THREADS  llvmpipe threads(default 8)
#   RW_SAMPLE_TRACE                strace -e set for `sample`
#
# Rules encoded here (learned by the planner the hard way):
#   * LD_DEBUG / LD_LIBRARY_PATH are set ONLY in the game's environment, never
#     exported into the shell that also starts Xvfb — a global LD_LIBRARY_PATH
#     poisons Xvfb's own dlopen and the run dies before the game starts.
#   * one game instance at a time: the script refuses to start if another
#     RimWorldLinux is alive (fps numbers skew) and always reaps its child.
#   * the game gets a private HOME so Config/Saves/Player.log never land in
#     the read-only game tree or in the repo.
set -uo pipefail

# Measurement hazard, measured twice on this host: a lane that builds Mesa
# exports LD_LIBRARY_PATH=<its build>/libgl-xlib globally.  Every process then
# loads that half-built libGL (NEEDED libgallium-26.2.6.so, not yet built) and
# Xvfb's GLX dies with "MESA: error: amdgpu_device_initialize failed" while the
# game reports "Failed to initialize display" / "Unable to find a supported
# OpenGL core profile".  Scrub it unless a caller explicitly wants a DSO path.
if [ "${RW_KEEP_DSO_PATH:-0}" != 1 ]; then
  unset LD_LIBRARY_PATH MESA_LIB LIBGL_DRIVERS_PATH GALLIUM_DRIVERS_DIRS
fi

ROOT=$(cd "$(dirname "$0")" && pwd)
GAME=${RW_GAME:-/mnt/fast/games/steamapps/common/RimWorld}
OUT=${RW_OUT:-$ROOT/out}
WIDTH=${RW_WIDTH:-1280}
HEIGHT=${RW_HEIGHT:-720}
LP=${RW_LP_THREADS:-8}
MODE=${1:-}
SECS=${2:-90}
SAMPLE_TRACE=${RW_SAMPLE_TRACE:-openat,read,mmap,mprotect,clone,futex}

die() { echo "trace-host: $*" >&2; exit 2; }
[ -n "$MODE" ] || die "usage: $0 {fps|syscalls|sample|libs|bindings|dlsym|xcount|symbols|gl} [seconds]"
[ -x "$GAME/RimWorldLinux" ] || die "no RimWorldLinux under RW_GAME=$GAME"
command -v Xvfb >/dev/null || die "Xvfb not installed"
mkdir -p "$OUT"

if pgrep -x RimWorldLinux >/dev/null 2>&1; then
  die "a RimWorldLinux is already running (pid $(pgrep -x RimWorldLinux | tr '\n' ' ')); fps numbers would skew — kill it first"
fi

# `symbols` is pure post-processing: merge the two X11 traces into the F4 seed.
if [ "$MODE" = symbols ]; then
  for f in "$OUT/ltrace-dlsym.txt" "$OUT/ld-debug-bindings.txt"; do
    [ -s "$f" ] || die "missing $f — run '$0 dlsym 20' and '$0 bindings 40' first"
  done
  python3 "$ROOT/x11-symbols.py" "$OUT/ltrace-dlsym.txt" "$OUT/ld-debug-bindings.txt" \
    >"$ROOT/x11-symbols.txt"
  head -6 "$ROOT/x11-symbols.txt"
  exit 0
fi

# ---- display ---------------------------------------------------------------
DISP=${RW_DISPLAY:-:90}
n=0
while [ "$n" -lt 40 ] && [ -e "/tmp/.X${DISP#:}-lock" ]; do
  n=$((n + 1)); DISP=":$((90 + n))"
done
XVFB_PID=
cleanup() { [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; XVFB_PID=; }
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM HUP

Xvfb "$DISP" -screen 0 "${WIDTH}x${HEIGHT}x24" -nolisten tcp -ac \
  >"$OUT/xvfb-$MODE.log" 2>&1 &
XVFB_PID=$!
for _ in $(seq 1 60); do
  xdpyinfo -display "$DISP" >/dev/null 2>&1 && break
  sleep 0.1
done
xdpyinfo -display "$DISP" >/dev/null 2>&1 || die "Xvfb failed on $DISP (see $OUT/xvfb-$MODE.log)"
# Xvfb answers the core protocol before its GLX/DRI3 probe settles; launching
# into that window measured as "Mesa: Failed to initialize display" five times
# and then "Unable to find a supported OpenGL core profile".
for _ in $(seq 1 60); do
  DISPLAY="$DISP" LIBGL_ALWAYS_SOFTWARE=1 glxinfo -B >/dev/null 2>&1 && break
  sleep 0.25
done

# ---- game environment: scoped to the child, never to this shell -------------
# -force-glcore pins Unity to the GLX/GL path.  Without it, Unity probes
# Vulkan first and on this host it picks RADV (AMD Radeon Graphics) and
# segfaults at "requesting resize" — measured 2026-11-02.  Vulkan is a
# non-goal (AD-1), so every probe run pins GL.  RW_GFX_FLAG=-force-vulkan
# reproduces the other path.
GAME_ARGS=(-screen-width "$WIDTH" -screen-height "$HEIGHT" -screen-fullscreen 0
           "${RW_GFX_FLAG:--force-glcore}")
START_PID=
SAMPLER=
start() {  # start <wrapper...>  (args are appended to the game's own args)
  ( export DISPLAY="$DISP" HOME="$OUT/home" LC_ALL=C LANG=C
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS="$LP"
    # RW_GL_OVERRIDE=3.3 reproduces the "is 3.3 core enough?" run: Mesa still
    # creates the context Unity asks for (4.5) but reports 3.3/GLSL 3.30.
    if [ -n "${RW_GL_OVERRIDE:-}" ]; then
      export MESA_GL_VERSION_OVERRIDE="$RW_GL_OVERRIDE"
      export MESA_GLSL_VERSION_OVERRIDE="${RW_GLSL_OVERRIDE:-330}"
    fi
    exec "$@" -logFile "$OUT/$MODE-player.log" "${GAME_ARGS[@]}" ) \
    >"$OUT/$MODE-stdout.log" 2>"$OUT/$MODE-stderr.log" &
  START_PID=$!
}

stop_game() {
  local pid=$START_PID child i
  [ -n "$pid" ] || return
  # Tracee first: strace -c / apitrace flush their output only when the game
  # exits.  pgrep -P finds the game under the wrapper (its threads are not
  # processes, so the list is one entry).
  child=$(pgrep -P "$pid" 2>/dev/null | head -1)
  if [ -n "$child" ]; then kill -TERM "$child" 2>/dev/null; else kill -TERM "$pid" 2>/dev/null; fi
  for i in $(seq 1 80); do kill -0 "$pid" 2>/dev/null || break; sleep 0.25; done
  if kill -0 "$pid" 2>/dev/null; then
    child=$(pgrep -P "$pid" 2>/dev/null | head -1)
    [ -n "$child" ] && kill -KILL "$child" 2>/dev/null
    kill -KILL "$pid" 2>/dev/null
  fi
  wait "$pid" 2>/dev/null
  START_PID=
}

finish() {
  stop_game
  if [ -n "$SAMPLER" ]; then wait "$SAMPLER" 2>/dev/null; SAMPLER=; fi
}
screenshot() {
  import -display "$DISP" -window root "$OUT/$MODE-screen.png" 2>/dev/null &&
    echo "  screenshot: $OUT/$MODE-screen.png"
}

# RimWorld stops on a modal "Could not initialize Steam API" box and never
# reaches the main menu until it is dismissed (measured: a 90 s run rendered
# only the title art + that box; `xdotool key Return` does not dismiss it).
# The box's buttons are found by colour (click-ignore.py) and clicked with
# XTEST, which needs no window manager and no input focus.
dismiss_steam() {
  local shot="$OUT/$MODE-probe.png" xy px py
  rm -f "$shot"
  # If import fails (window not mapped yet) the file is absent and the
  # detector must not answer from a stale screenshot of an earlier attempt.
  import -display "$DISP" -window root "$shot" 2>/dev/null || return 0
  xy=$(python3 "$ROOT/click-ignore.py" "$shot" 2>/dev/null) || {
    echo "  dismiss: no Steam modal on screen"; return 0; }
  # NOTE: $xy is "483 554" — it must be split, not quoted: `xdotool mousemove
  # "483 554"` is one malformed argument and (with stderr dropped) silently
  # clicked wherever the pointer already was.  That cost two runs.
  read -r px py <<<"$xy"
  echo "  dismiss: clicking Ignore at $px,$py"
  # A bare `xdotool click` (press+release inside one frame) is ignored by
  # Unity's SDL event loop here; a held button of ~180 ms is accepted.
  DISPLAY="$DISP" xdotool mousemove "$px" "$py" 2>/dev/null
  sleep 0.2
  DISPLAY="$DISP" xdotool mousedown 1 2>/dev/null
  sleep 0.18
  DISPLAY="$DISP" xdotool mouseup 1 2>/dev/null
  sleep 2
  import -display "$DISP" -window root "$OUT/$MODE-screen-after-dismiss.png" 2>/dev/null
  if python3 "$ROOT/click-ignore.py" "$OUT/$MODE-screen-after-dismiss.png" >/dev/null 2>&1; then
    echo "  dismiss: modal still up after click, retrying with a longer hold"
    DISPLAY="$DISP" xdotool mousedown 1 2>/dev/null
    sleep 0.8
    DISPLAY="$DISP" xdotool mouseup 1 2>/dev/null
    sleep 1
  fi
}

sample_proc() {
  local pid=$START_PID gpid
  gpid=$(pgrep -P "$pid" 2>/dev/null | head -1); [ -n "$gpid" ] || gpid=$pid
  bash "$ROOT/proc-sample.sh" "$gpid" "$OUT/$MODE-proc.csv" 2 "$SECS" &
  SAMPLER=$!
}

run_window() {  # run for SECS, retrying the Steam-modal dismissal
  local t=0
  if [ "${RW_DISMISS_STEAM:-1}" = 0 ]; then
    sleep "$SECS"; return
  fi
  while [ "$t" -lt "$SECS" ]; do
    sleep 5; t=$((t + 5))
    # Unity maps its window ~8-12 s in; a click before that goes to the root.
    # Retry every 15 s: under strace/apitrace the box can appear minutes late.
    if [ "$t" -ge 15 ] && [ $((t % 15)) -eq 0 ]; then
      [ "$t" -eq 15 ] && screenshot_early
      dismiss_steam
    fi
  done
}
screenshot_early() {
  import -display "$DISP" -window root "$OUT/$MODE-screen-before-dismiss.png" 2>/dev/null
}

case "$MODE" in
  fps)
    rm -rf "$OUT/hud"; mkdir -p "$OUT/hud"
    ( export DISPLAY="$DISP" HOME="$OUT/home" LC_ALL=C LANG=C
      export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LP_NUM_THREADS="$LP"
      if [ -n "${RW_GL_OVERRIDE:-}" ]; then
        export MESA_GL_VERSION_OVERRIDE="$RW_GL_OVERRIDE"
        export MESA_GLSL_VERSION_OVERRIDE="${RW_GLSL_OVERRIDE:-330}"
      fi
      export GALLIUM_HUD=fps GALLIUM_HUD_DUMP_DIR="$OUT/hud"
      exec "$GAME/RimWorldLinux" -logFile "$OUT/$MODE-player.log" "${GAME_ARGS[@]}" ) &
    START_PID=$!
    sleep 2; sample_proc
    echo "fps: running ${SECS}s on $DISP (llvmpipe, LP_NUM_THREADS=$LP)"
    run_window; screenshot; finish
    python3 "$ROOT/fps.py" "$OUT/hud" | tee "$OUT/fps-stats.txt"
    ;;

  syscalls)
    start strace -f -c -o "$OUT/strace-summary.txt" "$GAME/RimWorldLinux"
    run_window; finish
    head -8 "$OUT/strace-summary.txt"
    ;;

  sample)
    # NOTE: `read` in the set makes this file large (tens of MB for 15 s) and
    # slows the game; the counts live in strace-summary.txt, the sample exists
    # for *which paths/sizes*, so keep the window short (default 20 s).
    start strace -f -e trace="$SAMPLE_TRACE" -e signal=none -o "$OUT/strace-sample.txt" "$GAME/RimWorldLinux"
    run_window; finish
    ls -lh "$OUT/strace-sample.txt"
    ;;

  libs)
    start env LD_DEBUG=libs,files LD_DEBUG_OUTPUT="$OUT/ld-debug-libs.raw" "$GAME/RimWorldLinux"
    run_window; finish
    cat "$OUT"/ld-debug-libs.raw.* >"$OUT/ld-debug-libs.txt" 2>/dev/null
    rm -f "$OUT"/ld-debug-libs.raw.*
    grep -cE "calling init:" "$OUT/ld-debug-libs.txt" || true
    ;;

  bindings)
    start env LD_DEBUG=bindings LD_DEBUG_OUTPUT="$OUT/ld-debug-bindings.raw" "$GAME/RimWorldLinux"
    run_window; finish
    cat "$OUT"/ld-debug-bindings.raw.* >"$OUT/ld-debug-bindings.txt" 2>/dev/null
    rm -f "$OUT"/ld-debug-bindings.raw.*
    ;;

  dlsym)
    LT=${RW_LTRACE:-$(command -v ltrace || true)}
    [ -n "$LT" ] && [ -x "$LT" ] || die "ltrace not found (RW_LTRACE=... or: apt-get download ltrace && dpkg -x ltrace_*.deb .)"
    # ltrace aborts (SIGABRT in ltrace itself, not in the game) after ~15 s on
    # this binary; the trace file up to that point is valid and complete.
    start "$LT" -F "$ROOT/ltrace.conf" -f -e 'dlopen+dlsym' -o "$OUT/ltrace-dlsym.txt" "$GAME/RimWorldLinux"
    run_window; finish
    echo "  dlopen calls: $(grep -c 'dlopen(' "$OUT/ltrace-dlsym.txt")"
    echo "  dlsym  calls: $(grep -c 'dlsym(' "$OUT/ltrace-dlsym.txt")"
    ;;

  xcount)
    LT=${RW_LTRACE:-$(command -v ltrace || true)}
    [ -n "$LT" ] && [ -x "$LT" ] || die "ltrace not found"
    # -c counts calls that go through a PLT; SDL's dlsym'd pointers do not.
    # This mode exists to *measure* that limitation rather than assert it.
    start "$LT" -F "$ROOT/ltrace.conf" -f -c -e 'X*' -o "$OUT/ltrace-xcount.txt" "$GAME/RimWorldLinux"
    run_window; finish
    head -25 "$OUT/ltrace-xcount.txt"
    ;;

  gl)
    rm -f "$OUT/rw.trace"
    start apitrace trace -a gl -o "$OUT/rw.trace" "$GAME/RimWorldLinux"
    run_window; finish
    ls -lh "$OUT/rw.trace"
    apitrace info "$OUT/rw.trace" | head -40
    ;;

  *) die "unknown mode $MODE" ;;
esac

if grep -q "RimWorld 1\.6" "$OUT/$MODE-player.log" 2>/dev/null; then
  echo "  engine reached: $(grep -m1 'RimWorld 1\.6' "$OUT/$MODE-player.log")"
fi
echo "done: mode=$MODE out=$OUT"
