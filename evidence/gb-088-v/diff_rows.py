import sys

def parse_ppm(path):
    raw = open(path, "rb").read(); vals, i = [], 0
    while len(vals) < 4:
        while raw[i:i+1].isspace(): i += 1
        if raw[i:i+1] == b"#":
            while raw[i:i+1] not in (b"\n", b""): i += 1
            continue
        j = i
        while not raw[j:j+1].isspace(): j += 1
        vals.append(raw[i:j]); i = j
    return raw, i+1, int(vals[1]), int(vals[2])

a, b = sys.argv[1], sys.argv[2]
ra, oa, w, h = parse_ppm(a); rb, ob, _, _ = parse_ppm(b)
row = w*3
band = {}
for y in range(h):
    xa = ra[oa+y*row:oa+(y+1)*row]; xb = rb[ob+y*row:ob+(y+1)*row]
    if xa == xb: continue
    n = 0
    x0 = 10**9; x1 = -1
    for x in range(0, row, 3):
        if xa[x:x+3] != xb[x:x+3]:
            px = x//3
            n += 1
            x0 = min(x0, px); x1 = max(x1, px)
    band[y] = (n, x0, x1)
ys = sorted(band)
print(f"rows with changes: {len(ys)} of {h}")
# group contiguous runs
runs = []
start = prev = ys[0]
for y in ys[1:]:
    if y == prev+1:
        prev = y
    else:
        runs.append((start, prev)); start = prev = y
runs.append((start, prev))
tot = 0
for (s, e) in runs:
    n = sum(band[y][0] for y in range(s, e+1))
    tot += n
    x0 = min(band[y][1] for y in range(s, e+1)); x1 = max(band[y][2] for y in range(s, e+1))
    print(f"  y {s}-{e}: {n} px, x {x0}-{x1}")
print("total", tot)