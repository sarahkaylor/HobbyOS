import glob, os, sys


def read(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = (int(x) for x in parts[1].split())
    return w, h, parts[3]


def cmp(a, b, label):
    w, h, pa = read(a)
    w2, h2, pb = read(b)
    assert (w, h) == (w2, h2)
    n = 0
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        row = y * w
        for x in range(w):
            i = (row + x) * 3
            if pa[i:i + 3] != pb[i:i + 3]:
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    bbox = None if n == 0 else (minx, miny, maxx, maxy)
    print(f"{label}: differing={n}px bbox={bbox} ({100.0*n/(w*h):.2f}% of screen)")


ctl = "evidence/gb-059-v/liveness-static-control/frame0.ppm"
live = "evidence/gb-059-v/liveness-xnibbler-keys/frame0.ppm"
rigb = sorted(glob.glob("evidence/XNIBBLER/*/"))[-1]
cmp(ctl, live, "desktop(control frame0) vs launched-XNIBBLER(frame0)")
cmp(os.path.join(rigb, "boot.ppm"), os.path.join(rigb, "launch.ppm"), "rig boot vs rig launch")
cmp(os.path.join(rigb, "boot.ppm"), os.path.join(rigb, "gameplay.ppm"), "rig boot vs rig gameplay")
cmp(ctl, os.path.join(rigb, "gameplay.ppm"), "desktop(control) vs rig gameplay")