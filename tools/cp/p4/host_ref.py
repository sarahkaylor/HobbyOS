#!/usr/bin/env python3
"""tools/cp/p4/host_ref.py -- Gate P4 HOST reference (lane C4-x2).

Runs the X variant of Wine (tools/cp/build-wine.sh, CP_WINE_VARIANT=x) on the
Linux host against the pinned glibc 2.31 runtime and the /CP/X11 client
libraries -- inside a chroot that holds exactly the files HobbyOS holds
(the tools/cp/run-wine-host.sh pattern: private mount namespace, the Wine
tree runs as the calling user) -- talking to a PRIVATE Xvfb on its own display
number (never the desktop session's X server).  It drives the same scenario as
the in-OS gate with xdotool and records screenshots and the probe rows, so the
HobbyOS run has a reference for every row.

  python3 tools/cp/p4/host_ref.py [--out DIR] [--display N] [--steps a,b,..]
  steps: boot, rawinput, winecfg, notepad, modes   (default: all, in order)

Env: CP_WINE_WORK (default ~/cp/wine-build-x), CP_X11_WORK (default
~/cp/x11-build-x2), CP_RUNTIME (default <repo>/third_party/cp-runtime).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
WORK = os.environ.get("CP_WINE_WORK", os.path.expanduser("~/cp/wine-build-x"))
X11W = os.environ.get("CP_X11_WORK", os.path.expanduser("~/cp/x11-build-x2"))
RT = os.environ.get("CP_RUNTIME", os.path.join(REPO, "third_party", "cp-runtime"))
ROOTFS = os.path.join(WORK, "hostroot-p4")
UID, GID = os.getuid(), os.getgid()


def log(m):
    print(f"[P4H] {m}", flush=True)


def sh(*a, check=True, **kw):
    return subprocess.run(list(a), check=check, **kw)


def build_rootfs():
    man = os.path.join(WORK, "stage-manifest.sha256")
    xman = os.path.join(X11W, "stage-manifest.sha256")
    stamp = hashlib.sha256(open(man, "rb").read() + open(xman, "rb").read()).hexdigest()[:16]
    sp = os.path.join(ROOTFS, ".stamp")
    if os.path.exists(sp) and open(sp).read().strip() == stamp:
        return
    if os.path.exists(ROOTFS):
        sh("rm", "-rf", ROOTFS)
    for d in ("tmp", "dev", "proc", "etc", "CP"):
        os.makedirs(os.path.join(ROOTFS, d), exist_ok=True)
    for deb in ("libc6_2.31-0ubuntu9.18_amd64.deb", "libgcc-s1_10.5.0-1ubuntu1~20.04_amd64.deb"):
        sh("dpkg-deb", "-x", os.path.join(RT, "dl", deb), ROOTFS)
    for d in ("usr/share", "etc/ld.so.conf.d", "usr/lib/x86_64-linux-gnu/gconv"):
        sh("rm", "-rf", os.path.join(ROOTFS, d))
    sh("cp", "-a", os.path.join(WORK, "stage", "CP", "WINE"), os.path.join(ROOTFS, "CP", "WINE"))
    sh("cp", "-a", os.path.join(X11W, "stage", "CP", "X11"), os.path.join(ROOTFS, "CP", "X11"))
    with open(os.path.join(ROOTFS, "etc", "passwd"), "w") as f:
        f.write(f"cp:x:{UID}:{GID}:cp:/tmp/home:/bin/false\n")
    with open(os.path.join(ROOTFS, "etc", "group"), "w") as f:
        f.write(f"cp:x:{GID}:\n")
    open(sp, "w").write(stamp + "\n")
    log(f"rootfs rebuilt: {ROOTFS}")


class Host:
    def __init__(self, display, out):
        self.display = display
        self.out = out
        self.env = {"DISPLAY": f":{display}"}

    def wine(self, args, logname, background=False, wdebug="-all", extra_env=None):
        """Run `args` (a /CP/WINE/bin program + args) in the chroot."""
        inner = ('R=$1; shift; mount --rbind /dev "$R/dev"; mount -t proc proc "$R/proc"; '
                 'mkdir -p "$R/tmp/.X11-unix"; mount --bind /tmp/.X11-unix "$R/tmp/.X11-unix"; exec "$@"')
        envv = ["env", "-i", "HOME=/tmp/home", "USER=cp", "LOGNAME=cp", "WINEPREFIX=/tmp/pfx",
                f"WINEDEBUG={wdebug}", "PATH=/CP/WINE/bin", "LANG=C", "TERM=dumb",
                f"DISPLAY=:{self.display}", "LD_LIBRARY_PATH=/CP/X11/lib:/CP/WINE/lib",
                "WINEDLLOVERRIDES=mscoree,mshtml=,winemenubuilder.exe=d"]
        envv += [f"{k}={v}" for k, v in (extra_env or {}).items()]
        cmd = ["sudo", "unshare", "-m", "--propagation", "private", "sh", "-c", inner, "sh", ROOTFS,
               *envv, shutil.which("chroot"), f"--userspec={UID}:{GID}", ROOTFS, *args]
        lf = open(os.path.join(self.out, logname), "w")
        p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
        if background:
            return p
        p.wait()
        lf.close()
        return p.returncode

    def xdo(self, *a):
        return subprocess.run(["xdotool", *a], env=dict(os.environ, **self.env),
                              capture_output=True, text=True)

    def find_win(self, name, timeout=60):
        t0 = time.time()
        while time.time() - t0 < timeout:
            r = self.xdo("search", "--name", name)
            ids = r.stdout.split()
            if ids:
                return ids[-1]
            time.sleep(0.5)
        return None

    def shot(self, name):
        src = os.path.join(self.out, "fb", "Xvfb_screen0")
        dst = os.path.join(self.out, name + ".png")
        subprocess.run(["convert", "xwd:" + src, dst], check=False)
        log(f"screenshot {dst}")
        return dst


def wait_line(path, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            for ln in open(path, errors="replace"):
                if needle in ln:
                    return ln.strip()
        except FileNotFoundError:
            pass
        time.sleep(0.25)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(WORK, "p4-host"))
    ap.add_argument("--display", type=int, default=47)
    ap.add_argument("--steps", default="boot,rawinput,winecfg,notepad,modes")
    ap.add_argument("--warm", action="store_true", help="keep the prefix from the previous run")
    a = ap.parse_args()
    steps = a.steps.split(",")
    os.makedirs(os.path.join(a.out, "fb"), exist_ok=True)
    if os.path.exists(f"/tmp/.X11-unix/X{a.display}"):
        log(f"display :{a.display} is in use; pick another --display")
        return 2
    build_rootfs()
    if not a.warm:
        sh("sudo", "rm", "-rf", os.path.join(ROOTFS, "tmp"))
        os.makedirs(os.path.join(ROOTFS, "tmp", "home"), exist_ok=True)
    os.chmod(os.path.join(ROOTFS, "tmp"), 0o1777)
    os.makedirs(os.path.join(ROOTFS, "tmp", "home"), exist_ok=True)
    rawexe = os.path.join(REPO, "obj", "p4", "rawinput.exe")
    if os.path.exists(rawexe):
        shutil.copy(rawexe, os.path.join(ROOTFS, "tmp", "rawinput.exe"))
    xv = subprocess.Popen(["Xvfb", f":{a.display}", "-screen", "0", "1920x1080x24", "-nolisten", "tcp",
                           "-fbdir", os.path.join(a.out, "fb"), "-noreset"],
                          stdout=open(os.path.join(a.out, "xvfb.log"), "w"), stderr=subprocess.STDOUT)
    h = Host(a.display, a.out)
    res = {}
    try:
        time.sleep(2)
        if "boot" in steps:
            t0 = time.time()
            rc = h.wine(["/CP/WINE/bin/wine", "wineboot", "-i"], "wineboot.log")
            # wineboot's launcher returns before the install finishes; wait
            # for the whole tree (wineserver -w returns when it exits).
            h.wine(["/CP/WINE/bin/wineserver", "-w"], "wineboot-wait.log")
            res["wineboot"] = f"rc={rc} {time.time() - t0:.1f}s"
            log(f"wineboot -i: {res['wineboot']}")
        if "rawinput" in steps:
            lp = os.path.join(a.out, "rawinput.log")
            p = h.wine(["/CP/WINE/bin/wine", "Z:\\tmp\\rawinput.exe"],
                       "rawinput.log", background=True)
            if wait_line(lp, "[RI] phase free ready", 60):
                for _ in range(10):
                    h.xdo("mousemove_relative", "--", "7", "3")
                    time.sleep(0.05)
            wait_line(lp, "[RI] phase free total", 30)
            if wait_line(lp, "[RI] phase clip ready", 30):
                time.sleep(1)
                for _ in range(10):
                    h.xdo("mousemove_relative", "--", "7", "3")
                    time.sleep(0.05)
                for _ in range(5):
                    h.xdo("mousemove_relative", "--", "-4", "-9")
                    time.sleep(0.05)
                # the same edge push as p4_gate.py (expected sums 850, 25)
                for _ in range(20):
                    h.xdo("mousemove_relative", "--", "40", "2")
                    time.sleep(0.05)
            wait_line(lp, "[RI] phase clip total", 100)
            h.shot("rawinput")
            try:
                p.wait(timeout=120)
            except subprocess.TimeoutExpired:
                log("rawinput.exe still running; killing the tree")
            res["rawinput"] = [ln.strip() for ln in open(lp) if "[RI]" in ln and "input flags" not in ln]
        if "winecfg" in steps:
            p = h.wine(["/CP/WINE/bin/wine", "winecfg"], "winecfg.log", background=True)
            wid = h.find_win("Wine configuration", 90)
            log(f"winecfg window: {wid}")
            if wid:
                time.sleep(3)
                h.shot("winecfg-0")
                g = h.xdo("getwindowgeometry", wid).stdout
                log("winecfg geometry: " + " ".join(g.split()))
                h.xdo("windowactivate", "--sync", wid)
                # Ctrl+Tab walks the property sheet's tabs.
                for i in range(1, 4):
                    h.xdo("key", "--window", wid, "ctrl+Tab")
                    time.sleep(1.5)
                    h.shot(f"winecfg-{i}")
                h.xdo("key", "--window", wid, "Escape")
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                log("winecfg did not exit on Escape")
        if "notepad" in steps:
            p = h.wine(["/CP/WINE/bin/wine", "notepad"], "notepad.log", background=True)
            wid = h.find_win("Notepad", 90)
            log(f"notepad window: {wid}")
            if wid:
                time.sleep(2)
                h.xdo("windowactivate", "--sync", wid)
                h.xdo("type", "--delay", "60", "Hello from HobbyOS P4")
                time.sleep(2)
                h.shot("notepad")
        if "modes" in steps:
            lp = os.path.join(a.out, "modes.log")
            h.wine(["/CP/WINE/bin/wine", "Z:\\tmp\\rawinput.exe", "modes"], "modes.log")
            res["modes"] = [ln.strip() for ln in open(lp) if "[RI]" in ln]
    finally:
        h.wine(["/CP/WINE/bin/wineserver", "-k"], "kill.log")
        xv.terminate()
        xv.wait(timeout=10)
    for k, v in res.items():
        if isinstance(v, list):
            log(f"{k}:")
            for ln in v:
                print("   " + ln, flush=True)
        else:
            log(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
