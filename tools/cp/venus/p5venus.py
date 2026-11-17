"""tools/cp/venus/p5venus.py -- the Venus seams for C5-vk2's Gate P5 harnesses
(tools/cp/p5/, owned by the P5/X lanes and NOT edited here; lane C6-venus).

venus_qemu_cmd() replaces p5lib.qemu_cmd: the same (cmd, qmp, serial) triple,
but the command is tools/cp/host-venus.sh's `argv` (virtio-gpu-gl-pci with
venus/blob/hostmem on a memfd guest, egl-headless on the RX 9070 XT) run under
`env` with host-venus.sh's environment (radeon ICD ordered 9070 XT first, the
dma-buf RDWR shim), without DISPLAY/WAYLAND_DISPLAY, and with QEMU's stderr
(where virgl_render_server reports) appended to <work>/qemu-stderr.txt rather
than to the harness's unread pipe.

install() patches p5lib in place; tools/cp/venus/pyhook/sitecustomize.py calls
it when VENUS_P5=1, so `python3 tools/cp/p5/boot_wine_p5.py` picks it up from
inside tools/cp/p5/run-wine-p5.sh unchanged.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
P5DIR = os.path.join(REPO, "tools", "cp", "p5")


def _host_venus(*args, env=None):
    r = subprocess.run(["bash", os.path.join(REPO, "tools/cp/host-venus.sh")] + list(args),
                       capture_output=True, text=True, check=True, env=env)
    return [ln for ln in r.stdout.splitlines() if ln != ""]


def venus_qemu_cmd(work, disk, args):
    env = dict(os.environ, MEM=f"{args.mem}M", SMP=str(args.smp), CPU=args.cpu)
    argv = _host_venus("argv", disk, work, env=env)
    envv = _host_venus("env", env=env)
    err = os.path.join(work, "qemu-stderr.txt")
    with open(os.path.join(work, "cmdline.txt"), "w") as f:
        f.write(" ".join(argv) + "\n" + "\n".join(envv) + "\n")
    cmd = (["sh", "-c", 'exec "$@" 2>>"$0"', err, "env", "-u", "DISPLAY", "-u", "WAYLAND_DISPLAY",
            "-u", "XAUTHORITY"] + envv + argv)
    print("[VENUS-P5] qemu: " + " ".join(argv), flush=True)
    return cmd, os.path.join(work, "qmp-sock"), os.path.join(work, "serial.log")


def install():
    if P5DIR not in sys.path:
        sys.path.insert(0, P5DIR)
    import p5lib
    p5lib.qemu_cmd = venus_qemu_cmd
    return p5lib
