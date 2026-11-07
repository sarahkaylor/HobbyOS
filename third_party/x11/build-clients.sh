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
  local o="$HB_OBJ_DIR/xclients"
  mkdir -p "$o"
  : > "$HB_LOG_DIR/clients.log"

  # XFILES.BIN: "Files (X11)", a file browser drawn directly on Xlib with Xft
  # text.  Source: src/user/x11/apps/xfiles/main.c, GPL-2.0-or-later.
  if hb_want xfiles; then
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
  else
    hb_log "xfiles: skipped (--only)"
  fi

  # XDEPTH8.BIN: the depth-8 PseudoColor probe (lane xdepth8).  The server
  # only advertises a depth 8 visual when HB_XDEPTH=8, so this is the client
  # that proves XMatchVisualInfo(8, PseudoColor) + an 8-bit XPutImage work.
  if hb_want xdepth8; then
    local src8="$HB_REPO/src/user/x11/apps/xdepth8/main.c"
    [ -f "$src8" ] || hb_die "xdepth8: missing $src8"

    # shellcheck disable=SC2046,SC2086
    hb_run clients "$CC" $CFLAGS \
      $("$PKG_CONFIG" --cflags x11) \
      -c "$src8" -o "$o/xdepth8.o"

    # shellcheck disable=SC2046
    hb_run clients hb_link_flat "$o/XDEPTH8.BIN" "$o/xdepth8.o" \
      $("$PKG_CONFIG" --libs x11)

    cp "$o/XDEPTH8.BIN" "$HB_BIN_DIR/XDEPTH8.BIN"
    hb_log "clients: XDEPTH8.BIN ($(stat -c %s "$HB_BIN_DIR/XDEPTH8.BIN") bytes)"
  else
    hb_log "xdepth8: skipped (--only)"
  fi
}