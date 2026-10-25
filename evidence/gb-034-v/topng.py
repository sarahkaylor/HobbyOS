#!/usr/bin/env python3
"""PPM -> PNG for vision review of the gate frames."""
import sys
from PIL import Image

for src in sys.argv[1:]:
    dst = src[:-4] + ".png"
    Image.open(src).save(dst)
    print(dst)
