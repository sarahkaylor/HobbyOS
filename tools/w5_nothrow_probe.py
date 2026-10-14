#!/usr/bin/env python3
"""w5_nothrow_probe.py — lane w5-nothrow: in-OS clang smokes with the FIXED
(cxxrt-shim) closure (T2.3b version + T2.4 hello.c->hello.o).

Boosts a copy of the w3-tools staged disk, stages the freshly relinked +
host-flat-converted clang as /TOOLS/CLANG3.BIN (probe name, rides alongside
the owned frozen /TOOLS entries), drives it through a staged /SRC/SELFHOST.RUN
script via the SELFHOST.BIN driver, and captures serial.

Probes:
  w5v : CLANG3.BIN --version                    (no-regression smoke)
  w5c : CLANG3.BIN -c HELLO.C -> HELLO.O        (T2.4, acceptance flags)

Judged by serial markers (acceptance 8.4); the produced /SRC/T2SRC/HELLO.O is
additionally pulled off the image and verified as a valid aarch64 ELF
relocatable on the host (compile actually happened in-OS).

Usage:  python3 tools/w5_nothrow_probe.py [--base IMG] [--only w5v,w5c]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import run_selfhost_build_test as H  # noqa: E402  (reuse boot/QMP/menu machinery)

WORK = os.path.join(ROOT, "obj", "w5nothrow")

# Base image: the w3-tools staged disk (has /TOOLS + RESDIR + SELFHOST.BIN).
BASE = "/home/sarah/hobbyos-selfhost-lanes/w3-tools/disk.img"

# Fixed toolchain binaries (obj/selfhost/dist-staging, refreshed by this lane).
DIST = os.path.join(ROOT, "obj/selfhost/dist-staging/usr/local/bin")

ACCEPT_CFLAGS = ("-O2 -Wall -Wextra -I/SRC/src/user_include "
                 "-I/SRC/src/user_include/graphics -I/SRC/src/include "
                 "-I/SRC/src/libc/include --target=aarch64-none-elf "
                 "-ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include")

PROBES = {
    "w5v": {
        "label": "T2.3b clang --version (fixed closure, flat CLANG3.BIN)",
        "run": "w5-version|CLANG3.BIN|--version\n",
        "expect": "clang_banner",
    },
    "w5c": {
        "label": "T2.4 clang -c hello.c -> hello.o (fixed closure, flat CLANG3.BIN)",
        # spawn args capped at 255 (process.h args[256]): trimmed like w4 t24b.
        "run": ("w5-compile|CLANG3.BIN|--target=aarch64-none-elf "
                "-ffreestanding -mcpu=cortex-a53 "
                "-I/SRC/src/user_include -I/SRC/src/include -I/SRC/src/libc/include "
                "-isystem /TOOLS/RESDIR/include -c /SRC/T2SRC/HELLO.C -o /SRC/T2SRC/HELLO.O\n"),
        "expect": "compile_o",
    },
}

TERMINAL_RE = re.compile(r"ACCEPTANCE-COMPLETE|FAILED-AT|\[SELFHOST\] ABORT")


def mcopy_stdin(image, name, content):
    tmp = os.path.join(WORK, ".run-tmp")
    with open(tmp, "w") as f:
        f.write(content)
    subprocess.run(["mcopy", "-o", "-i", image, tmp, name], check=True)


def mcopy_file(image, local, name):
    subprocess.run(["mcopy", "-o", "-i", image, local, name], check=True)


def stage_t2src(image):
    subprocess.run(["mmd", "-i", image, "::/SRC/T2SRC"])
    mcopy_file(image, os.path.join(ROOT, "src/user/hello.c"), "::/SRC/T2SRC/HELLO.C")


def stage_flat_clang(image):
    """Host flat conversion (w4 t23b method): llvm-objcopy -O binary of the
    fixed clang-21 -> /TOOLS/CLANG3.BIN (probe name; frozen /TOOLS untouched).
    entry==0x1000000000==first-LOAD (linker.ld USER_IMG_BASE) so _start lands
    at offset 0 of the flat image."""
    local = os.path.join(WORK, "flat", "CLANG3.BIN")
    os.makedirs(os.path.dirname(local), exist_ok=True)
    if not os.path.exists(local):
        subprocess.run(["/usr/bin/llvm-objcopy-21", "-O", "binary",
                        os.path.join(DIST, "clang-21"), local], check=True)
    mcopy_file(image, local, "::/TOOLS/CLANG3.BIN")


def boot_and_run(image, serial, qmp_sock, timeout, extra=()):
    qemu = H.boot(image, serial, qmp=qmp_sock, extra=extra)
    launched = False
    try:
        el = H.wait_for(serial, H.BOOT_MARKER, min(120, timeout))
        if el is None:
            return "", False, el
        txt = H.read_serial(serial)
        if "[MENU]" not in txt:
            H.wait_for(serial, "[MENU]", 30)
            txt = H.read_serial(serial)
        idx = H.menu_index(txt, "SELFHOST.BIN")
        if idx is None:
            return txt, False, el
        q = H.Q(qmp_sock)
        # Optional gdbstub root-causing: attach gdb with a conditional
        # breakpoint on free() that latches the caller (x30) when x0 == the
        # HobbyOS malloc BLOCK_MAGIC (0x48424F424C4B4D47) is passed to free.
        gdb_script = os.path.join(WORK, "gdb-free.txt")
        gdb = None
        if os.environ.get("W5_GDB_FREE"):
            with open(gdb_script, "w") as f:
                f.write(
                    "set pagination off\n"
                    "set confirm off\n"
                    "target remote :12345\n"
                    "break *0x100484c7d0\n"
                    "commands\n"
                    "  silent\n"
                    "  if $x0 == 0x48424F424C4B4D47\n"
                    "    printf \"\\n### CULPRIT x0=%#lx x1=%#lx x30=%#lx x29=%#lx\\n\", $x0, $x1, $x30, $x29\n"
                    "    x/20gx $sp\n"
                    "    quit\n"
                    "  end\n"
                    "  continue\n"
                    "end\n"
                    "continue\n")
            gdb = subprocess.Popen(
                ["gdb-multiarch", "-q", "-batch", "-x", gdb_script],
                stdout=open(os.path.join(WORK, "gdb-free.out"), "w"),
                stderr=subprocess.STDOUT, text=True)
            time.sleep(6)  # let gdb attach + arm the breakpoint
        H.launch_driver(q, idx)
        launched = True
        deadline = time.time() + timeout
        while time.time() < deadline:
            t = H.read_serial(serial)
            if TERMINAL_RE.search(t):
                if gdb:
                    gdb.terminate()
                    gdb.wait(timeout=5)
                return t, True, el
            time.sleep(0.5)
        if gdb:
            gdb.terminate()
            gdb.wait(timeout=5)
        return H.read_serial(serial), True, el
    finally:
        H.stop_qemu(qemu)


def verify_hello_o(image):
    """Pull the in-OS-produced /SRC/T2SRC/HELLO.O off the image and check it
    is a valid aarch64 ELF relocatable (compile really happened in-OS)."""
    out = os.path.join(WORK, "HELLO.O")
    try:
        subprocess.run(["mcopy", "-o", "-i", image, "::/SRC/T2SRC/HELLO.O", out],
                       check=True)
    except subprocess.CalledProcessError:
        return None
    if not os.path.exists(out) or os.path.getsize(out) == 0:
        return None
    r = subprocess.run(["/usr/bin/llvm-readelf-21", "-h", out],
                       capture_output=True, text=True)
    return {"size": os.path.getsize(out), "readelf": r.stdout + r.stderr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--only", default=",".join(PROBES))
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    img = os.path.join(WORK, "probe.img")
    if not os.path.exists(img):
        print("[w5] copying base image %s -> %s" % (args.base, img), flush=True)
        shutil.copy2(args.base, img)

    stage_t2src(img)
    stage_flat_clang(img)

    results = {}
    for key in [k for k in PROBES if k in args.only.split(",")]:
        p = PROBES[key]
        print("[w5] %s  (%s)" % (p["label"], key), flush=True)
        mcopy_stdin(img, "::/SRC/SELFHOST.RUN", p["run"])
        serial = os.path.join(WORK, "%s.serial.log" % key)
        qmp = os.path.join(WORK, "qmp-%s" % key)
        extra = ()
        if key == "w5c":
            # Optional: gdbstub for capturing free()'s caller at the
            # BLOCK_MAGIC-data-abort (set W5_GDB_FREE=1 for that boot).
            if os.environ.get("W5_GDB_FREE"):
                extra = ("-gdb", "tcp::12345")
        text, launched, boot_el = boot_and_run(img, serial, qmp, args.timeout, extra)
        cls, detail = H.classify(text, timeout_hit=True)
        clang_banner = "clang version" in text
        brk_hit = ("EC: 0x000000000000003C" in text) or ("brk" in text.lower()
                                                         and "EC:" in text)
        kern_fault = "[KERNEL]" in text and "fault" in text
        res = {
            "label": p["label"],
            "expected": p["expect"],
            "launched": launched,
            "boot_s": boot_el,
            "class": cls,
            "detail": detail,
            "clang_banner": clang_banner,
            "brk_hit": brk_hit,
            "kern_fault": kern_fault,
            "serial": serial,
        }
        if key == "w5c":
            res["hello_o"] = verify_hello_o(img)
        results[key] = res
        print("[w5]   -> %s: %s (banner=%s brk=%s fault=%s)"
              % (cls, detail, clang_banner, brk_hit, kern_fault), flush=True)
        if key == "w5c" and res["hello_o"]:
            print("[w5]   -> HELLO.O %d bytes; %s"
                  % (res["hello_o"]["size"],
                     res["hello_o"]["readelf"].replace("\n", " | ")[:200]),
                  flush=True)
    import json
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
