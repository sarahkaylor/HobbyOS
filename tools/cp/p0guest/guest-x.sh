#!/usr/bin/env bash
# cp/p0guest (runs IN the guest): the display servers the ladder uses.
#
#   guest-x.sh kms  [WxH]   Xorg :0, modesetting + glamor on the virtio-gpu KMS
#                           scanout (card0). Venus WSI presents by DRI3 dma-buf
#                           (no guest-CPU pixel copy); the host shows the
#                           scanout through egl-headless.
#   guest-x.sh xvfb [WxH]   Xvfb :1. No DRI3: every present is a CPU copy of the
#                           image into guest memory (the software-copy path).
#   guest-x.sh mode WxH     xrandr the KMS output to WxH
#   guest-x.sh stop
set -euo pipefail
GEOM=${2:-1920x1080}
case "${1:-}" in
kms)
    sudo tee /etc/X11/cp-kms.conf >/dev/null <<'EOF'
Section "ServerFlags"
    Option "AutoAddGPU" "off"
    Option "BlankTime" "0"
    Option "StandbyTime" "0"
    Option "SuspendTime" "0"
    Option "OffTime" "0"
EndSection
Section "Device"
    Identifier "virtio"
    Driver "modesetting"
    Option "kmsdev" "/dev/dri/card0"
    Option "AccelMethod" "glamor"
EndSection
EOF
    sudo pkill -x Xorg 2>/dev/null || true; sleep 1
    sudo sh -c 'nohup Xorg :0 -config /etc/X11/cp-kms.conf -ac -nolisten tcp -noreset vt7 > /tmp/xorg-kms.txt 2>&1 &'
    for _ in $(seq 50); do DISPLAY=:0 xdpyinfo >/dev/null 2>&1 && break; sleep 0.2; done
    DISPLAY=:0 xset s off -dpms
    "$0" mode "$GEOM"
    ;;
mode)
    export DISPLAY=:0
    out=$(xrandr | awk '/ connected/{print $1; exit}')
    # QEMU's EDID has no 1280x720; add the CVT 1280x720@60 timing (cvt 1280 720 60).
    if [ "$2" = 1280x720 ] && ! xrandr | grep -q '^ *1280x720 '; then
        xrandr --newmode 1280x720 74.50 1280 1344 1472 1664 720 723 728 748 -hsync +vsync 2>/dev/null || true
        xrandr --addmode "$out" 1280x720
    fi
    xrandr --output "$out" --mode "$2" && xrandr | head -2 ;;
xvfb)
    pkill -x Xvfb 2>/dev/null || true; sleep 0.5
    nohup Xvfb :1 -screen 0 "${GEOM}x24" -ac -nolisten tcp > /tmp/xvfb.txt 2>&1 &
    for _ in $(seq 50); do DISPLAY=:1 xdpyinfo >/dev/null 2>&1 && break; sleep 0.2; done
    DISPLAY=:1 xdpyinfo | grep -E 'dimensions|DRI3|MIT-SHM' || true
    ;;
stop)
    sudo pkill -x Xorg 2>/dev/null || true; pkill -x Xvfb 2>/dev/null || true ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
