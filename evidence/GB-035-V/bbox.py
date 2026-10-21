import glob, os, sys
from PIL import Image, ImageChops
D = "evidence/GB-035-V/t5"
pairs = [("nes-0.ppm", "nes-3.ppm"), ("nes-1.ppm", "nes-2.ppm"), ("menu.ppm", "nes-0.ppm")]
for a, b in pairs:
    A = Image.open(os.path.join(D, a)).convert("RGB")
    B = Image.open(os.path.join(D, b)).convert("RGB")
    d = ImageChops.difference(A, B)
    print(f"{a} vs {b}: bbox={d.getbbox()}")
# count diffs excluding the clock corner (x>=1820, y>=1000) and taskbar strip y>=1010
d = ImageChops.difference(Image.open(os.path.join(D, "nes-0.ppm")).convert("RGB"),
                          Image.open(os.path.join(D, "nes-3.ppm")).convert("RGB"))
px = d.load(); W, H = d.size
n_game = sum(1 for y in range(0, 1010) for x in range(0, 1820) if px[x, y] != (0, 0, 0))
print("differing px inside the game area (0..1820 x 0..1010) of nes-0 vs nes-3:", n_game)
