#!/usr/bin/env bash
# tools/cp/host-venus.sh -- the HobbyOS-on-Venus host recipe (Gate P6, lane C6-venus).
#
#   host-venus.sh check                 render node identity, versions, shim build
#   host-venus.sh argv DISK WORK [QEMU-ARGS...]
#                                       print the QEMU argv, one arg per line
#                                       (the harnesses in tools/cp/venus/ read it)
#   host-venus.sh boot DISK WORK [QEMU-ARGS...]
#                                       run that QEMU in the foreground; WORK gets
#                                       serial.log, qemu-stderr.txt, qmp-sock
#
# Always under a QEMU slot (docs/cyberpunk.md §7.4):
#   bash tools/cp/qemu-slot.sh bash tools/cp/host-venus.sh boot disk.img obj/venus
#
# What it does, and why (docs/cp-venus.md §1; the P0 recipe is docs/cp-p0-guest.md §2):
#   * virtio-gpu-gl-pci with venus=true,blob=true,hostmem=$HOSTMEM: Venus capset
#     (id 4), blob resources, and a host-visible shared-memory BAR of $HOSTMEM.
#   * memory-backend-memfd,share=on bound with -machine memory-backend=: guest RAM
#     must be a shareable memfd so virgl_render_server (a separate process) can
#     see guest-memory blobs.
#   * -display egl-headless,rendernode=$RENDER: GL/EGL on the RX 9070 XT render
#     node, no window anywhere.  DISPLAY and WAYLAND_DISPLAY are removed from the
#     environment so nothing can reach the user's desktop session.
#   * the render server inherits QEMU's environment: only the radeon ICD, and the
#     9070 XT *ordered* first (MESA_VK_DEVICE_SELECT=vid:did without the `!`; the
#     `!` form breaks guest enumeration, P0 §2.1 item 4).
#   * LD_PRELOAD of tools/cp/venus/venus-host-shim.c: (a) P0's dma-buf DRM_RDWR fix
#     (without it the render server exports read-only dma-bufs, QEMU's mmap fails
#     with EACCES and every RESOURCE_MAP_BLOB fails, P0 §2.1 item 3); (b) every
#     open of a DRM node that is not the render GPU's fails (CP_SHIM_DENY), so
#     the render server never opens the 8060S that runs the user's desktop, not
#     even to enumerate -- the guest then sees exactly one Venus GPU.
#   * no -daemonize: the render server reports Venus failures only on QEMU's stderr.
#
# Env: MEM (MiB or G, default 12G, refused above 12G), HOSTMEM (default 8G), SMP (8),
#      CPU (host), RENDER (/dev/dri/renderD129), VKDEV (1002:7550), SHIM (1),
#      VENUS_BUILD (~/cp/venus-build: shim .so lives in $VENUS_BUILD/shim),
#      OVMF (~/.local/share/OVMF/OVMF_CODE_4M.fd), QEMU (qemu-system-x86_64),
#      VENUS_TRACE (comma list of QEMU trace patterns, e.g. virtio_gpu_cmd_*),
#      VENUS_VNC (1: -vnc unix:WORK/vnc-sock, for tools/cp/venus/vncshot.py),
#      VENUS_IOEVENTFD (1 = ioeventfd=on on the GPU, the default; 0 = QEMU's
#      default, off), VENUS_GPU_OPTS (extra virtio-gpu-gl-pci properties;
#      C6-present's A/B knobs, docs/cp-present.md), CP_AUDIO_WAV (C4-audio:
#      virtio-sound whose output is written to this WAV file; unset = none).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MEM=${MEM:-12G}
HOSTMEM=${HOSTMEM:-8G}
SMP=${SMP:-8}
CPU=${CPU:-host}
RENDER=${RENDER:-/dev/dri/renderD129}
VKDEV=${VKDEV:-1002:7550}
SHIM=${SHIM:-1}
VENUS_BUILD=${VENUS_BUILD:-$HOME/cp/venus-build}
OVMF=${OVMF:-$HOME/.local/share/OVMF/OVMF_CODE_4M.fd}
QEMU=${QEMU:-qemu-system-x86_64}

die() { echo "host-venus: $*" >&2; exit 1; }

mem_mib() { # 12G / 12288M / 12288 -> MiB
    local v=$1
    case $v in
        *G|*g) echo $(( ${v%[Gg]} * 1024 )) ;;
        *M|*m) echo "${v%[Mm]}" ;;
        *) echo "$v" ;;
    esac
}

render_check() {
    # The render node must be the RX 9070 XT (1002:7550) on amdgpu, never the
    # 8060S iGPU that drives the user's desktop.
    local n=${RENDER##*/} sys
    sys=/sys/class/drm/$n/device
    [ -e "$RENDER" ] || die "$RENDER does not exist"
    local ven dev drv
    ven=$(cat "$sys/vendor"); dev=$(cat "$sys/device")
    drv=$(basename "$(readlink -f "$sys/driver")")
    [ "${ven#0x}:${dev#0x}" = "$VKDEV" ] || die "$RENDER is ${ven#0x}:${dev#0x}, expected $VKDEV"
    [ "$drv" = amdgpu ] || die "$RENDER driver is $drv, expected amdgpu"
    echo "render $RENDER = ${ven#0x}:${dev#0x} driver=$drv pci=$(basename "$(readlink -f "$sys")")"
}

shim_so() {
    local so=$VENUS_BUILD/shim/venus-host-shim.so src=$REPO/tools/cp/venus/venus-host-shim.c
    if [ ! -f "$so" ] || [ "$src" -nt "$so" ]; then
        mkdir -p "$VENUS_BUILD/shim"
        cc -O2 -Wall -shared -fPIC -o "$so" "$src" -ldl >&2
    fi
    echo "$so"
}

argv() {
    local disk=$1 work=$2; shift 2
    local mib; mib=$(mem_mib "$MEM")
    [ "$mib" -le 12288 ] || die "MEM=$MEM is above the 12 GiB slot limit"
    mkdir -p "$work"
    local fw=$work/ovmf.fd
    # C6-present (docs/cp-present.md §3): the device's queue notify as an
    # ioeventfd.  QEMU 10.2.1 defaults it off for virtio-gpu, so every guest
    # kick was a userspace MMIO exit that took the BQL in the vCPU thread --
    # behind egl-headless' ~3 ms scanout readback on the main thread.  On,
    # the kick is an eventfd write in KVM and the vCPU runs on.
    local gpuopt=
    [ "${VENUS_IOEVENTFD:-1}" = 1 ] && gpuopt=,ioeventfd=on
    [ -f "$fw" ] || cp "$OVMF" "$fw"
    local a=(
        "$QEMU" -name hobbyos-venus,debug-threads=on
        -machine q35,accel=kvm,memory-backend=mem0
        -cpu "$CPU" -smp "$SMP" -m "${mib}M"
        -object "memory-backend-memfd,id=mem0,size=${mib}M,share=on"
        -drive "if=pflash,format=raw,file=$fw"
        -device "virtio-gpu-gl-pci,blob=true,hostmem=$HOSTMEM,venus=true$gpuopt${VENUS_GPU_OPTS:+,$VENUS_GPU_OPTS}"
        -vga none
        -display "egl-headless,rendernode=$RENDER"
        -serial "file:$work/serial.log"
        -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1
        -drive "file=$disk,format=raw,id=disk0,if=none"
        -device nvme,drive=disk0,serial=1234,bus=pcie.1
        -device edu -netdev user,id=net0
        -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56
        -action shutdown=poweroff
        -qmp "unix:$work/qmp-sock,server,nowait"
        -d guest_errors -D "$work/qemu-debug.log"
    )
    # P7 (docs/cp-p7.md §4): VENUS_VNC=1 adds a VNC listener on a unix socket
    # in WORK.  egl-headless reads every flushed GL scanout back into the
    # console surface, so tools/cp/venus/vncshot.py sees what is on the glass
    # (QMP screendump cannot: "no surface" for a GL scanout).  Never a TCP port.
    if [ "${VENUS_VNC:-0}" = 1 ]; then
        rm -f "$work/vnc-sock"
        a+=(-vnc "unix:$work/vnc-sock")
    fi
    # C4-audio (docs/cp-audio.md): CP_AUDIO_WAV=/abs/out.wav adds virtio-sound
    # with QEMU's `wav` backend.  Never a host sound server: PipeWire on strix
    # is the user's desktop session.  Unset: the argv is unchanged.
    if [ -n "${CP_AUDIO_WAV:-}" ]; then
        a+=(-audiodev "wav,id=cpsnd0,path=$CP_AUDIO_WAV,out.frequency=48000,out.channels=2,out.format=s16"
            -device virtio-sound-pci,audiodev=cpsnd0,streams=1)
    fi
    if [ -n "${VENUS_TRACE:-}" ]; then
        local t IFS=,
        for t in $VENUS_TRACE; do a+=(-trace "enable=$t"); done
    fi
    printf '%s\n' "${a[@]}" "$@"
}

deny_nodes() { # every DRM node that is not the render GPU's (colon list)
    local mine n out=
    mine=$(readlink -f "/sys/class/drm/${RENDER##*/}/device")
    for n in /sys/class/drm/card[0-9]* /sys/class/drm/renderD*; do
        case ${n##*/} in *-*) continue ;; esac   # connectors (card1-DP-1)
        [ "$(readlink -f "$n/device")" = "$mine" ] && continue
        out=${out:+$out:}/dev/dri/${n##*/}
    done
    echo "$out"
}

envv() { # the environment QEMU (and so virgl_render_server) runs with
    local pre=
    [ "$SHIM" = 1 ] && pre=$(shim_so)
    # Render-node fencing (venus-host-shim.c): the render server must never open
    # the desktop GPU's nodes, not even to enumerate (DENY=0 turns it off).
    [ "$SHIM" = 1 ] && [ "${DENY:-1}" = 1 ] && printf '%s\n' "CP_SHIM_DENY=$(deny_nodes)"
    printf '%s\n' "LD_PRELOAD=$pre" \
        "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json" \
        "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json" \
        "MESA_VK_DEVICE_SELECT=$VKDEV"
}

case "${1:-}" in
    check)
        render_check
        "$QEMU" --version | head -1
        dpkg-query -W -f='virglrenderer ${Version}\n' libvirglrenderer1 2>/dev/null || true
        ls -l /usr/libexec/virgl_render_server
        [ "$SHIM" = 1 ] && echo "shim $(shim_so)" && echo "deny $(deny_nodes)"
        "$QEMU" -device virtio-gpu-gl-pci,help 2>/dev/null | grep -E '^\s+(blob|hostmem|venus|context_init)' || true
        ;;
    argv) shift; [ $# -ge 2 ] || die "usage: argv DISK WORK [args]"; render_check >&2; argv "$@" ;;
    env) envv ;;
    boot)
        shift; [ $# -ge 2 ] || die "usage: boot DISK WORK [args]"
        [ -n "${CP_QEMU_SLOT_HELD:-}" ] || die "run under tools/cp/qemu-slot.sh"
        render_check
        work=$2
        mapfile -t A < <(argv "$@")
        mapfile -t E < <(envv)
        echo "host-venus: ${A[*]}" > "$work/cmdline.txt"
        printf '%s\n' "${E[@]}" >> "$work/cmdline.txt"
        exec env -u DISPLAY -u WAYLAND_DISPLAY -u XAUTHORITY "${E[@]}" "${A[@]}" 2> "$work/qemu-stderr.txt"
        ;;
    *) sed -n '2,13p' "$0"; exit 2 ;;
esac
