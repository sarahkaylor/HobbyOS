#!/usr/bin/env python3
"""Control for GB-031-V: is the cross-dest delta exactly the DWARF source path?

Copies tools/sdl/build-sdl2-static-musl.sh to /tmp, adds the
-ffile-prefix-map/-fdebug-prefix-map pair that PR #161/#169 propose (configure
CFLAGS + app link), leaving everything else identical.  Two clean builds at
different dests with the patched script decide whether that one change is what
closes clause 1 — the tree is not modified.

Usage: prefixmap_control.py <repo-root>
"""
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
src = os.path.join(root, "tools/sdl/build-sdl2-static-musl.sh")
dst = "/tmp/build-sdl2-prefixmap.sh"
s = open(src).read()

anchor = 'cd "$DEST/stage/SDL2-$SDL2_VER"\n'
assert anchor in s, "anchor cd not found"
pfx = (anchor +
       '# CONTROL: the one change under test (dest-independent debug paths).\n'
       'PFX="-ffile-prefix-map=$(pwd)=. -fdebug-prefix-map=$(pwd)=."\n')
s = s.replace(anchor, pfx, 1)

old_cc = 'CC="clang --target=x86_64-linux-musl" ./configure'
assert old_cc in s, "configure line changed shape"
s = s.replace(old_cc, 'CC="clang --target=x86_64-linux-musl $PFX" ./configure', 1)

old_link = 'clang --target=x86_64-linux-musl -static -no-pie'
assert old_link in s, "app link line changed shape"
s = s.replace(old_link, 'clang --target=x86_64-linux-musl $PFX -static -no-pie', 1)

open(dst, "w").write(s)
os.chmod(dst, 0o755)
print(f"[control] wrote {dst} (adds -ffile-prefix-map/-fdebug-prefix-map to configure + app link)")
