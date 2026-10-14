#!/usr/bin/env python3
"""w4_clang_probe.py — lane w4-clang: in-OS smokes T2.3/T2.4/T2.5 (failing-first).

Boots a copy of the w3-tools staged disk (which carries /TOOLS/{CLANG,LLD,
LLVMAR,OBJCOPY,MAKE,MKFSFAT}.BIN + RESDIR + the SELFHOST.BIN driver), stages a
per-probe /SRC/SELFHOST.RUN script by mcopy (no QMP keystrokes for the command
text — the staged-script path per the lane brief), launches SELFHOST.BIN from
the desktop Apps menu, and captures the serial evidence.

Probes:
  t23-clang-version : CLANG.BIN --version            (T2.3)
  t24-clang-compile : CLANG.BIN -c HELLO.C -> .o     (T2.4, acceptance flags)
  t25-lld-link      : LLD.BIN link HELLO.O+libc.a    (T2.5 link)
  t25-hello-run     : run /HELLO.BIN in-OS           (T2.5 run-phase control)

Every probe is judged by serial markers (acceptance §8.4), never guest exit
codes.  The kernel's own loader line ("Program too large (N bytes > 1048576)")
is extracted verbatim where present.

Usage:  python3 tools/w4_clang_probe.py [--base IMG] [--only t23,...]
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

WORK = os.path.join(ROOT, "obj", "w4clang")

# Base image: the w3-tools staged disk (has /TOOLS + SELFHOST.BIN + HELLO.BIN).
BASE = "/home/sarah/hobbyos-selfhost-lanes/w3-tools/disk.img"

# Acceptance compile flags (buildtools-audit.md §7 USER_CFLAGS + -isystem RESDIR),
# made absolute for the in-OS probe (the driver spawns with cwd=/; the real
# Makefile.hobbyos runs make from /SRC so its relative -I paths resolve the
# same way).
ACCEPT_CFLAGS = ("-O2 -Wall -Wextra -I/SRC/src/user_include "
                 "-I/SRC/src/user_include/graphics -I/SRC/src/include "
                 "-I/SRC/src/libc/include --target=aarch64-none-elf "
                 "-ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include")

PROBES = {
    "t23": {
        "label": "T2.3 clang --version (ELF as staged)",
        "run": "t23-version|CLANG.BIN|--version\n",
        "expect": "load_wall",     # ELF executed as flat -> entry fault
    },
    "t24": {
        "label": "T2.4 clang -c hello.c (ELF as staged)",
        "run": ("t24-compile|CLANG.BIN|%s -c /SRC/T2SRC/HELLO.C -o /SRC/T2SRC/HELLO.O\n"
                % ACCEPT_CFLAGS),
        "expect": "load_wall",
    },
    "t25link": {
        "label": "T2.5 lld + objcopy link (ELF as staged)",
        "run": ("t25-link|LLD.BIN|-T /SRC/T2SRC/linker.ld -e _start "
                "-o /SRC/T2SRC/HELLO.ELF /SRC/T2SRC/HELLO.O /SRC/T2SRC/libc.a\n"
                "t25-objcopy|OBJCOPY.BIN|-O binary /SRC/T2SRC/HELLO.ELF /SRC/T2SRC/HELLO.BIN\n"),
        "expect": "load_wall",
    },
    "t25run": {
        "label": "T2.5 run HELLO.BIN in-OS",
        "run": "t25-hello-run|HELLO.BIN|\n",
        "expect": "run_ok",
    },
    # --- host-flat-converted toolchain probes (the M2 objcopy -O binary
    # step validated as a probe: new staged names, owned /TOOLS untouched) --
    "t23b": {
        "label": "T2.3 clang --version (flat-converted CLANG2.BIN)",
        "run": "t23b-version|CLANG2.BIN|--version\n",
        "expect": "clang_banner",
    },
    "t24b": {
        "label": "T2.4 clang -c hello.c (flat CLANG2.BIN, in-OS)",
        # NB: spawn args are capped at 255 chars (process.h args[256]); the
        # accepted command below is trimmed to fit (the real Makefile.hobbyos
        # runs make from /SRC with short relative -I + passes the rest via a
        # shell exec, which has a bigger budget).
        "run": ("t24b-compile|CLANG2.BIN|--target=aarch64-none-elf "
                "-ffreestanding -mcpu=cortex-a53 "
                "-I/SRC/src/user_include -I/SRC/src/include -I/SRC/src/libc/include "
                "-isystem /TOOLS/RESDIR/include -c /SRC/T2SRC/HELLO.C -o /SRC/T2SRC/HELLO.O\n"),
        "expect": "compile_o",
    },
    "npnew": {
        "label": "control: tiny C++ new(nothrow) in-OS (NPNEW.BIN)",
        "run": "npnew-run|NPNEW.BIN|\n",
        "expect": "npnew_ok",
    },
    "t25b": {
        "label": "T2.5 lld+objcopy link (flat, in-OS)",
        "run": ("t25b-link|LLD2.BIN|-T /SRC/T2SRC/linker.ld -e _start "
                "-o /SRC/T2SRC/HELLO.ELF /SRC/T2SRC/HELLO.O /SRC/T2SRC/libc.a\n"
                "t25b-objcopy|OBJ2.BIN|-O binary /SRC/T2SRC/HELLO.ELF /HELLO2.BIN\n"
                "t25b-run|HELLO2.BIN|\n"),
        "expect": "run_ok",
    },
}

# Toolchain ELFs -> flat .bin conversions staged under probe names (owned
# /TOOLS/<FROZEN>.BIN names are NOT touched; these ride alongside).
FLAT_TOOLS = [
    ("clang-21",   "CLANG2.BIN"),
    ("lld",        "LLD2.BIN"),
    ("llvm-objcopy", "OBJ2.BIN"),
    ("llvm-ar",    "AR2.BIN"),
]

TERMINAL_RE = re.compile(r"ACCEPTANCE-COMPLETE|FAILED-AT|\[SELFHOST\] ABORT")
TOO_LARGE_RE = re.compile(r"Program too large \(\d+ bytes > \d+\): \S+")


def mcopy_stdin(image, name, content):
    """Stage a text blob into the image at name (mtools: stdin via '-' is not
    supported in 4.0.49, so write a temp file and mcopy -o over it)."""
    tmp = os.path.join(WORK, ".run-tmp")
    with open(tmp, "w") as f:
        f.write(content)
    subprocess.run(["mcopy", "-o", "-i", image, tmp, name], check=True)


def mcopy_file(image, local, name):
    subprocess.run(["mcopy", "-o", "-i", image, local, name], check=True)


def stage_flat_tools(image):
    """Host-side M2 probe step: objcopy -O binary the four toolchain ELFs and
    stage them under probe names (/TOOLS/CLANG2.BIN ...).  Never touches the
    owned /TOOLS/<FROZEN>.BIN entries.  Uses the host llvm-objcopy (the
    dist-staging binaries are aarch64 ELFs and cannot run on the host)."""
    src = os.path.join(ROOT, "obj/selfhost/dist-staging/usr/local/bin")
    flat = os.path.join(WORK, "flat")
    os.makedirs(flat, exist_ok=True)
    for elf, name in FLAT_TOOLS:
        local = os.path.join(flat, name)
        if not os.path.exists(local):
            subprocess.run(["/usr/bin/llvm-objcopy-21", "-O", "binary",
                            os.path.join(src, elf), local], check=True)
        mcopy_file(image, local, "::/TOOLS/%s" % name)


def stage_probe(image, probe_key):
    run = PROBES[probe_key]["run"]
    mcopy_stdin(image, "::/SRC/SELFHOST.RUN", run)


def stage_t2src(image):
    """Stage the T2.4/T2.5 source material: hello.c, linker.ld, libc.a.
    Idempotent: mmd rc=1 on an existing dir is fine (mtools quirk)."""
    subprocess.run(["mmd", "-i", image, "::/SRC/T2SRC"])
    mcopy_file(image, os.path.join(ROOT, "src/user/hello.c"), "::/SRC/T2SRC/HELLO.C")
    mcopy_file(image, os.path.join(ROOT, "src/user/linker.ld"), "::/SRC/T2SRC/linker.ld")
    mcopy_file(image, "/home/sarah/hobbyos-selfhost-lanes/w3-tools/obj/arm/libc.a",
               "::/SRC/T2SRC/libc.a")
    mcopy_file(image, "/home/sarah/hobbyos-selfhost-lanes/w3-tools/obj/arm/hello.o",
               "::/SRC/T2SRC/HELLO.O")


def boot_and_run(image, serial, qmp_sock, timeout):
    """Boot image, launch SELFHOST.BIN from the Apps menu, wait for a terminal
    marker.  Returns (serial_text, launched_bool, elapsed)."""
    qemu = H.boot(image, serial, qmp=qmp_sock)
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
        H.launch_driver(q, idx)
        launched = True
        deadline = time.time() + timeout
        while time.time() < deadline:
            t = H.read_serial(serial)
            if TERMINAL_RE.search(t):
                return t, True, el
            time.sleep(0.5)
        return H.read_serial(serial), True, el
    finally:
        H.stop_qemu(qemu)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--only", default=",".join(PROBES))
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    img = os.path.join(WORK, "probe.img")
    if not os.path.exists(img):
        log("copying base image %s -> %s" % (args.base, img))
        shutil.copy2(args.base, img)

    stage_t2src(img)
    stage_flat_tools(img)
    # control probe: tiny C++ new(nothrow) binary at the disk root
    npn = os.path.join(WORK, "npnew/NPNEW.BIN")
    if os.path.exists(npn):
        mcopy_file(img, npn, "::/NPNEW.BIN")

    results = {}
    for key in [k for k in PROBES if k in args.only.split(",")]:
        p = PROBES[key]
        log("%s  (%s)" % (p["label"], key))
        stage_probe(img, key)
        serial = os.path.join(WORK, "%s.serial.log" % key)
        qmp = os.path.join(WORK, "qmp-%s" % key)
        text, launched, boot_el = boot_and_run(img, serial, qmp, args.timeout)
        cls, detail = H.classify(text, timeout_hit=True)
        too_large = TOO_LARGE_RE.findall(text)
        hello_mark = "[HELLO] entering main" in text
        clang_banner = "clang version" in text
        results[key] = {
            "label": p["label"],
            "expected": p["expect"],
            "launched": launched,
            "boot_s": boot_el,
            "class": cls,
            "detail": detail,
            "loader_wall_line": too_large,
            "hello_ran": hello_mark,
            "clang_banner": clang_banner,
            "serial": serial,
        }
        log("  -> %s: %s" % (cls, detail))
        for w in too_large:
            log("  -> LOADER WALL: %s" % w)
        if hello_mark:
            log("  -> [HELLO] marker seen on serial")
        if clang_banner:
            log("  -> clang version banner seen on serial")
        if launched and cls == "hang":
            log("  -> HANG (check %s)" % serial)
    print(json_dumps(results))


def log(m):
    print("[w4_clang] %s" % m, flush=True)


def json_dumps(o):
    import json
    return json.dumps(o, indent=2)


if __name__ == "__main__":
    sys.exit(main())
