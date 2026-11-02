#!/usr/bin/env bash
# third_party/x11/build-clients.sh -- stage "clients": first-party X11 apps.
#
# XFILES.BIN is "Files (X11)", a file browser drawn directly on Xlib with Xft
# text (no toolkit).  Source: src/user/x11/apps/xfiles/main.c, GPL-2.0-or-later.
#
# Sourced by build.sh; must define stage_clients.  Uses the helpers from
# build.sh (hb_run, hb_log, hb_link_flat, hb_want) and the env exported by
# cross/env.sh ($CC, $CFLAGS, $PKG_CONFIG, $HB_OBJ_DIR, $HB_BIN_DIR).
set -euo pipefail

stage_clients() {
  hb_want xfiles || { hb_log "xfiles: skipped (--only)"; return 0; }
  local o="$HB_OBJ_DIR/xclients"
  mkdir -p "$o"
  : > "$HB_LOG_DIR/clients.log"

  local src="$HB_REPO/src/user/x11/apps/xfiles/main.c"
  [ -f "$src" ] || hb_die "xfiles: missing $src"

  # shellcheck disable=SC2046,SC2086
  hb_run clients "$CC" $CFLAGS \
    $("$PKG_CONFIG" --cflags xft fontconfig x11) \
    -c "$src" -o "$o/xfiles.o"

  # shellcheck disable=SC2046
  hb_run clients hb_link_flat "$o/XFILES.BIN" "$o/xfiles.o" \
    $("$PKG_CONFIG" --libs xft fontconfig x11)

  cp "$o/XFILES.BIN" "$HB_BIN_DIR/XFILES.BIN"
  hb_log "clients: XFILES.BIN ($(stat -c %s "$HB_BIN_DIR/XFILES.BIN") bytes)"
}