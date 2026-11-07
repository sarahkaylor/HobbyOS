#!/usr/bin/env python3
"""Build a PRE-FIX (HEAD) vm.c that carries ONLY the L56 mmap-race probe.

Why: the probe must be able to observe the race on the unfixed kernel — the
fixed kernel inserts under one lock, so a same-base pair cannot occur there.
This takes the merged tip's vm.c and re-applies the probe's three call sites
(mr_sel at the chosen base, mr_fail on a refused insert, and the selector
letter) without any of the fix.  Usage:

    python3 tools/rw/l56-prefix-probe.py <fixed-vm.c> <out-vm.c>

The probe block itself is lifted verbatim from the fixed file, so there is
exactly one copy of the row formatting in the tree.
"""
import subprocess
import sys

BEGIN = "/* ---------------------------------------------------------------------\n * L56: the mmap base-select race probe."
END = "int64_t vm_mmap(struct process *grp"


def probe_block(fixed_src: str) -> str:
    a = fixed_src.index(BEGIN)
    b = fixed_src.index(END)
    return fixed_src[a:b]


def main():
    fixed_path, out_path = sys.argv[1], sys.argv[2]
    with open(fixed_path, "r", encoding="utf-8", errors="surrogateescape") as f:
        fixed = f.read()
    head = subprocess.run(["git", "show", "HEAD:src/kernel/vm.c"],
                          capture_output=True, text=True, check=True).stdout

    block = probe_block(fixed)
    assert "static void mr_fail(" in block and "static void mr_sel(" in block

    s = head
    s = s.replace(END, block + END, 1)

    # selector letter + probe call, pre-fix call sites
    s = s.replace("""  uint64_t base;
  int from_arena = 0;
""", """  uint64_t base;
  int from_arena = 0;
  char mr_w = '?'; /* L56 probe: which selector chose the base */
""", 1)
    s = s.replace("""    base = addr;
    if (vm_region_remove(as, base, len) != 0)""",
                  """    base = addr;
    mr_w = 'F';
    if (vm_region_remove(as, base, len) != 0)""", 1)
    s = s.replace("""        base = jb;
        from_arena = 1;""", """        base = jb;
        mr_w = 'A';
        from_arena = 1;""", 1)
    s = s.replace("""        base = hf;
        goto have_base;""", """        base = hf;
        mr_w = 'H';
        goto have_base;""", 1)
    s = s.replace("""        base = lh;
        goto have_base;""", """        base = lh;
        mr_w = 'L';
        goto have_base;""", 1)
    s = s.replace("""    mr_w = '1';
""", """    mr_w = '1';
""")
    s = s.replace("""    base = vm_hole_find(as, start, USER_MMAP_LIMIT_V2, len);
    if (!base)
      return -ENOMEM;
  }
have_base:""", """    base = vm_hole_find(as, start, USER_MMAP_LIMIT_V2, len);
    if (!base)
      return -ENOMEM;
    mr_w = '1';
  }
have_base:
  mr_sel(base, len, mr_w); /* L56 probe: one row per chosen base */""", 1)
    s = s.replace("""  if (vm_region_insert(as, base, len, p, kind, fl2, obj, obj_off) != 0) {
    if (anon_obj || file_backed || nfs_backed)""",
                  """  if (vm_region_insert(as, base, len, p, kind, fl2, obj, obj_off) != 0) {
    mr_fail(as, base, len); /* L56: name the blocker and the same-base peer */
    if (anon_obj || file_backed || nfs_backed)""", 1)

    for marker in ("mr_sel(base, len, mr_w)", "mr_fail(as, base, len)",
                   "char mr_w = '?'", "mr_w = 'A';", "mr_w = 'H';", "mr_w = 'L';",
                   "mr_w = '1';"):
        if marker not in s:
            print(f"missing: {marker}", file=sys.stderr)
            return 3
    with open(out_path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)
    print(f"wrote {out_path}: pre-fix vm.c + probe ({len(s)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
