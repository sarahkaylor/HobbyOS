#!/usr/bin/env bash
# cp/p0guest: the Linux-guest-on-Venus VM (docs/cp-p0-guest.md).
#
#   host-venus.sh start [--avx-off]   boot the guest headless (egl-headless on the 9070 XT)
#   host-venus.sh stop                ACPI shutdown, then kill what this script started
#   host-venus.sh status
#   host-venus.sh ssh [cmd...]        ssh into the guest as cp
#   host-venus.sh shot <file.ppm>     QMP screendump of the scanout
#   host-venus.sh qmp '<json>'        raw QMP command
#   host-venus.sh get <guest-path> <host-path>
#   host-venus.sh push                copy ~/cp/p0guest/tools into the guest's ~/tools
#
# Everything lives under ~/cp/p0guest. The game and Proton directories are
# exported read-only via virtiofsd --readonly (and are mounted ro in the guest).
set -euo pipefail
ROOT=${CP_P0GUEST:-$HOME/cp/p0guest}
RUN=$ROOT/run
DISK=$ROOT/img/guest.qcow2
SEED=$ROOT/img/seed.iso
GAME=${GAME:-"/mnt/fast/games/steamapps/common/Cyberpunk 2077"}
PROTON=${PROTON:-"/mnt/fast/games/steamapps/common/Proton - Experimental"}
VCPUS=${VCPUS:-16}
MEM=${MEM:-32G}
HOSTMEM=${HOSTMEM:-16G}
RENDER=${RENDER:-/dev/dri/renderD129}          # RX 9070 XT (PCI 65:00.0)
VKDEV=${VKDEV:-1002:7550}                      # vendor:device for Mesa device-select
SSHPORT=${SSHPORT:-2225}
XRES=${XRES:-1920}
YRES=${YRES:-1080}
VIRTIOFSD=/usr/libexec/virtiofsd

# AVX family, as QEMU 10.2.1 names the CPUID flags (qemu-system-x86_64 -cpu help).
# Every VEX/EVEX-only extension the Zen 5 host exposes goes, so nothing can
# claim AVX-derived support while AVX itself is off.
AVX_OFF_FLAGS="-avx,-avx2,-fma,-f16c,-vaes,-vpclmulqdq,-avx-vnni,-avx-ifma,-avx-vnni-int8,-avx-ne-convert,-avx-vnni-int16,-avx512f,-avx512dq,-avx512ifma,-avx512cd,-avx512bw,-avx512vl,-avx512vbmi,-avx512vbmi2,-avx512vnni,-avx512bitalg,-avx512-vpopcntdq,-avx512-bf16,-avx512-vp2intersect,-avx512-fp16"

gssh() {
    ssh -q -i "$ROOT/ssh/id_ed25519" -p "$SSHPORT" -o BatchMode=yes \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile="$ROOT/ssh/known_hosts" \
        -o ConnectTimeout=5 cp@127.0.0.1 "$@"
}

qmp() {  # one QMP command, prints the reply
    python3 - "$RUN/qmp.sock" "$1" <<'EOF'
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
f.write(sys.argv[2] + '\n'); f.flush()
while True:
    line = f.readline()
    if not line: break
    if '"return"' in line or '"error"' in line:
        print(line.strip()); break
EOF
}

start() {
    local cpu=host label=avx-on
    [ "${1:-}" = --avx-off ] && { cpu="host,$AVX_OFF_FLAGS"; label=avx-off; }
    mkdir -p "$RUN"
    if [ -f "$RUN/qemu.pid" ] && kill -0 "$(cat "$RUN/qemu.pid")" 2>/dev/null; then
        echo "already running (pid $(cat "$RUN/qemu.pid"))"; return 0
    fi
    rm -f "$RUN"/*.sock
    # Read-only exports. --sandbox none: we run unprivileged (no userns setup);
    # --readonly makes virtiofsd refuse every modifying request.
    local tag dir
    for tag in cp2077 proton; do
        [ $tag = cp2077 ] && dir=$GAME || dir=$PROTON
        nohup "$VIRTIOFSD" --shared-dir "$dir" --socket-path "$RUN/fs-$tag.sock" \
            --readonly --sandbox none --cache auto --thread-pool-size 16 \
            > "$RUN/virtiofsd-$tag.txt" 2>&1 &
        echo $! > "$RUN/virtiofsd-$tag.pid"
    done
    for _ in $(seq 50); do [ -S "$RUN/fs-cp2077.sock" ] && [ -S "$RUN/fs-proton.sock" ] && break; sleep 0.1; done

    # Venus device selection: virgl_render_server inherits QEMU's environment.
    #   order (default): radeon ICD only (drops host lavapipe) and the Mesa
    #          device-select layer puts the 9070 XT first. The guest then sees
    #          the 9070 XT as Venus GPU0 and the 8060S as GPU1; the guest-side
    #          env (guest-env.sh) hides the rest.
    #   bang:  MESA_VK_DEVICE_SELECT="vid:did!" (hide all other devices) — on
    #          virglrenderer 1.2.0 this makes the guest's vkEnumeratePhysicalDevices
    #          fail with VK_ERROR_INITIALIZATION_FAILED and the render server
    #          logs "vkr: venus driver is required to call
    #          vkEnumeratePhysicalDevices first". Kept for the record only.
    #   none:  no host filtering (guest sees 9070 XT, 8060S and llvmpipe via Venus).
    case "${VKFILTER:-order}" in
        order) export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json
               export VK_ICD_FILENAMES=$VK_DRIVER_FILES
               export MESA_VK_DEVICE_SELECT="$VKDEV" ;;
        bang)  export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json
               export VK_ICD_FILENAMES=$VK_DRIVER_FILES
               export MESA_VK_DEVICE_SELECT="$VKDEV!" ;;
        none)  ;;
    esac

    # dma-buf RDWR shim (dmabuf-rdwr-shim.c explains why). SHIM=0 runs without it.
    local preload=
    if [ "${SHIM:-1}" = 1 ]; then
        local so=$ROOT/build/dmabuf-rdwr-shim.so
        if [ ! -f "$so" ] || [ "$ROOT/tools/dmabuf-rdwr-shim.c" -nt "$so" ]; then
            mkdir -p "$ROOT/build"
            cc -O2 -Wall -shared -fPIC -o "$so" "$ROOT/tools/dmabuf-rdwr-shim.c" -ldl
        fi
        preload=$so
    fi

    echo "$label cpu=$cpu shim=${SHIM:-1} vkfilter=${VKFILTER:-order}" > "$RUN/cpu.txt"
    # Not -daemonize: QEMU then closes stderr, and virgl_render_server (a child
    # of QEMU) reports Venus failures only there.
    LD_PRELOAD=$preload nohup qemu-system-x86_64 \
        -name cp-p0guest,debug-threads=on \
        -d guest_errors \
        -machine q35,accel=kvm,memory-backend=mem0 \
        -cpu "$cpu" -smp "$VCPUS,sockets=1,cores=$VCPUS,threads=1" -m "$MEM" \
        -object memory-backend-memfd,id=mem0,size="$MEM",share=on \
        -device virtio-gpu-gl-pci,blob=true,hostmem="$HOSTMEM",venus=true,xres="$XRES",yres="$YRES" \
        -display egl-headless,rendernode="$RENDER" \
        -vnc unix:"$RUN/vnc.sock" \
        -vga none \
        -drive file="$DISK",if=virtio,format=qcow2,discard=unmap,cache=writeback \
        -drive file="$SEED",if=virtio,format=raw,readonly=on \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$SSHPORT"-:22 -device virtio-net-pci,netdev=n0 \
        -chardev socket,id=fs0,path="$RUN/fs-cp2077.sock" -device vhost-user-fs-pci,chardev=fs0,tag=cp2077,queue-size=1024 \
        -chardev socket,id=fs1,path="$RUN/fs-proton.sock" -device vhost-user-fs-pci,chardev=fs1,tag=proton,queue-size=1024 \
        -device virtio-keyboard-pci -device virtio-tablet-pci \
        -qmp unix:"$RUN/qmp.sock",server=on,wait=off \
        -serial file:"$RUN/serial.txt" \
        -pidfile "$RUN/qemu.pid" \
        > "$RUN/qemu-stderr.txt" 2>&1 < /dev/null &
    for _ in $(seq 50); do [ -s "$RUN/qemu.pid" ] && break; sleep 0.1; done
    echo "qemu pid $(cat "$RUN/qemu.pid") ($label)"
    for i in $(seq 120); do gssh true 2>/dev/null && { echo "ssh up after ${i}x2s"; return 0; }; sleep 2; done
    echo "ssh did not come up; see $RUN/serial.txt" >&2; return 1
}

stop() {
    if [ -f "$RUN/qemu.pid" ] && kill -0 "$(cat "$RUN/qemu.pid")" 2>/dev/null; then
        gssh sudo poweroff 2>/dev/null || qmp '{"execute":"system_powerdown"}' || true
        for _ in $(seq 60); do kill -0 "$(cat "$RUN/qemu.pid")" 2>/dev/null || break; sleep 1; done
        kill -0 "$(cat "$RUN/qemu.pid")" 2>/dev/null && kill "$(cat "$RUN/qemu.pid")"
    fi
    local p
    for p in "$RUN"/virtiofsd-*.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
        rm -f "$p"
    done
    rm -f "$RUN/qemu.pid"
    echo stopped
}

case "${1:-}" in
    start) shift; start "$@" ;;
    stop) stop ;;
    status) [ -f "$RUN/qemu.pid" ] && ps -o pid,etime,rss,args -p "$(cat "$RUN/qemu.pid")" || echo "not running" ;;
    ssh) shift; gssh "$@" ;;
    shot) qmp "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"$2\"}}" ;;
    qmp) qmp "$2" ;;
    get) scp -q -r -P "$SSHPORT" -i "$ROOT/ssh/id_ed25519" -o BatchMode=yes -o StrictHostKeyChecking=no \
             -o UserKnownHostsFile="$ROOT/ssh/known_hosts" cp@127.0.0.1:"$2" "$3" ;;
    push) tar -C "$ROOT/tools" -cf - . | gssh 'mkdir -p ~/tools && tar -C ~/tools -xf -' ;;
    *) sed -n '2,12p' "$0"; exit 2 ;;
esac
