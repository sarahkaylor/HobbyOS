# tools/cp/venus/pyhook/sitecustomize.py -- with VENUS_P5=1 and this directory on
# PYTHONPATH, every Python the P5 harnesses start gets p5lib.qemu_cmd replaced
# by the Venus host recipe (tools/cp/venus/p5venus.py).  Inert otherwise.
import os
import sys

if os.environ.get("VENUS_P5") == "1":
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import p5venus  # noqa: E402

    p5venus.install()
