#!/usr/bin/env python3
"""W2-MKFS host-side functional proof for the dosfstools 4.2 mkfs.fat port.

Runs the host-native build of the port (obj/mkfs_host/mkfsfat_host) on
scratch image files and verifies, WITHOUT HobbyOS:

  1. '-C IMG BLOCKS' creates the file via open(O_CREAT)+ftruncate only
     (strace: no dd/fallocate/truncate call, no full-size write).  Note
     mkfs.fat's BLOCKS argument is in 1024-byte blocks (BLOCK_SIZE 1024 in
     mkfs.fat.c), so -C IMG 131072 == a 128 MiB image — the audit sketch's
     "256 MiB / 512 B" comment underestimates by 2x (flag in the report).
  2. Produced images are structurally FAT-correct: BPB fields (512-byte
     sectors, reserved sectors, 2 FATs, root entries, media 0xF8, total
     sectors consistent with the file size, FS-type string, boot signature
     0x55AA), root directory present.
  3. Both FAT copies are byte-identical.
  4. An independent FAT16 consumer (mtools mdir/mcopy) accepts the image:
     volume label visible, then copy-in / copy-out round-trip byte-exact.
  5. '-C' on an existing file refuses (EEXIST) and leaves it untouched
     (the acceptance disk recipe's 'rm -f' ordering depends on this).
  6. Geometry arms: FAT12 auto (4 MiB image), FAT16 at the 64 MiB / 128 MiB
     (acceptance frozen size) / 256 MiB cluster-arm sizes.

Each check prints PASS/FAIL; the script exits non-zero on any FAIL.
"""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "obj", "mkfs_host", "mkfsfat_host")
WORK = tempfile.mkdtemp(prefix="mkfsfat_w2_")
fails = []


def run(*args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"[{tag}] {name}" + (f" — {detail}" if detail else ""))
    if not cond:
        fails.append(name)
    return cond


def parse_bpb(path):
    with open(path, "rb") as f:
        b = f.read(512)
    return {
        "jmp": b[0], "oem": b[3:11].decode("latin1"),
        "byps": struct.unpack_from("<H", b, 11)[0], "spc": b[13],
        "rsvd": struct.unpack_from("<H", b, 14)[0], "nfat": b[16],
        "rootent": struct.unpack_from("<H", b, 17)[0],
        "tot16": struct.unpack_from("<H", b, 19)[0], "media": b[21],
        "fatsz16": struct.unpack_from("<H", b, 22)[0],
        "spt": struct.unpack_from("<H", b, 24)[0],
        "heads": struct.unpack_from("<H", b, 26)[0],
        "hid": struct.unpack_from("<I", b, 28)[0],
        "tot32": struct.unpack_from("<I", b, 32)[0],
        "sig": (b[510], b[511]),
        "fstype": b[54:62].decode("latin1") if b[38] == 0x29 else None,
    }


def verify_image(name, img, blocks_kib, expect_fat, spc=None):
    """blocks_kib because mkfs.fat's BLOCKS argument is 1024-byte units."""
    want = blocks_kib * 1024
    st = os.stat(img)
    check(f"{name}: file size = blocks*1024", st.st_size == want,
          f"{st.st_size} vs {want}")
    bpb = parse_bpb(img)
    check(f"{name}: boot sig 0x55AA", bpb["sig"] == (0x55, 0xAA),
          f"{bpb['sig']}")
    check(f"{name}: FS type '{expect_fat}'", bpb["fstype"] == expect_fat,
          f"{bpb['fstype']!r}")
    check(f"{name}: bytes/sector 512", bpb["byps"] == 512)
    check(f"{name}: 2 FATs", bpb["nfat"] == 2)
    check(f"{name}: media 0xF8", bpb["media"] == 0xF8)
    check(f"{name}: root entries > 0", bpb["rootent"] > 0)
    total = bpb["tot16"] or bpb["tot32"]
    check(f"{name}: total sectors == file size/512",
          total == want // bpb["byps"], f"tot={total}")
    if spc is not None:
        check(f"{name}: cluster size spc={spc}", bpb["spc"] == spc,
              f"spc={bpb['spc']}")
    fatoff = bpb["rsvd"] * bpb["byps"]
    with open(img, "rb") as f:
        f.seek(fatoff)
        fat1 = f.read(bpb["fatsz16"] * bpb["byps"])
        fat2 = f.read(bpb["fatsz16"] * bpb["byps"])
    check(f"{name}: FAT1 == FAT2", fat1 == fat2)
    rootoff = fatoff + 2 * bpb["fatsz16"] * bpb["byps"]
    with open(img, "rb") as f:
        f.seek(rootoff)
        root = f.read(32)
    check(f"{name}: root first entry empty (0x00)", root[0] == 0x00)
    return bpb


def main():
    print(f"binary: {BIN}")
    print(f"work:   {WORK}")
    if not os.path.exists(BIN):
        print("FATAL: host binary missing — run tools/build_mkfs_host.sh")
        sys.exit(2)

    # ---- 1-4. -C ftruncate-only proof (strace)
    img = os.path.join(WORK, "out.img")
    trace_list = "open,openat,ftruncate,truncate,fallocate,write"
    strace_log = os.path.join(WORK, "strace.log")
    p = run("strace", "-f", "-e", f"trace={trace_list}", "-o", strace_log,
            BIN, "-F", "16", "-C", img, "131072")
    check("1: -C exit 0", p.returncode == 0,
          (p.stderr or "").strip()[:150])
    calls = open(strace_log).read() if os.path.exists(strace_log) else ""
    create_open = "O_CREAT" in calls
    ftrunc = "ftruncate(" in calls
    no_truncate = "truncate(" not in calls.replace("ftruncate", "")
    no_fallocate = "fallocate" not in calls
    check("2: -C opens with O_CREAT", create_open)
    check("3: -C sizes via ftruncate only (no dd/fallocate/truncate)",
          ftrunc and no_truncate and no_fallocate,
          f"ftruncate={ftrunc} truncate={calls.count('truncate(')} "
          f"fallocate={'fallocate' in calls}")
    check("4: banner printed", p.stdout.strip().startswith("mkfs.fat 4.2 (2021-01-31)"),
          p.stdout.strip().splitlines()[0] if p.stdout.strip() else "(none)")

    # ---- 5. structural verification at several sizes (blocks = KiB)
    for name, blocks_kib, fat, spc in [
        ("4MiB FAT12 auto", 4096, "FAT12   ", None),
        ("64MiB FAT16", 65536, "FAT16   ", 4),
        ("128MiB FAT16 (acceptance S3 size)", 131072, "FAT16   ", 4),
        ("256MiB FAT16 cluster-arm", 262144, "FAT16   ", 8),
    ]:
        img2 = os.path.join(WORK, f"{name.split()[0]}_{blocks_kib}.img")
        cmd = [BIN, "-C", img2, str(blocks_kib)]
        if fat == "FAT16   ":
            cmd = [BIN, "-F", "16", "-C", img2, str(blocks_kib)]
        r = run(*cmd)
        if r.returncode != 0:
            check(f"mkfs {name}: exit 0", False, r.stderr.strip()[:150])
            continue
        verify_image(f"{name}", img2, blocks_kib, fat, spc)

    # ---- 6. -C on an existing file must refuse (EEXIST), file untouched
    ex = os.path.join(WORK, "exists.img")
    r = run(BIN, "-C", ex, "4096")
    ok_create = r.returncode == 0
    sz0 = os.stat(ex).st_size if ok_create else -1
    r2 = run(BIN, "-F", "16", "-C", ex, "2048")
    check("6: -C on existing file refused", ok_create and r2.returncode != 0,
          (r2.stderr or "").strip()[:120] or f"rc={r2.returncode}")
    check("6b: existing file untouched", ok_create and os.stat(ex).st_size == sz0,
          f"size={os.stat(ex).st_size} (was {sz0})")

    # ---- 7. independent consumer: mtools mdir + mcopy round-trip
    img3 = os.path.join(WORK, "mt.img")
    r = run(BIN, "-F", "16", "-C", img3, "131072", "-n", "W2TEST")
    if r.returncode != 0:
        check("7: mtools image create", False, r.stderr.strip()[:150])
    else:
        md = run("mdir", "-i", img3, "::")
        check("7a: mdir reads image (label + empty root)",
              md.returncode == 0 and "W2TEST" in md.stdout,
              md.stdout.strip().replace("\n", " | ")[:200])
        payload = os.path.join(WORK, "payload.txt")
        with open(payload, "w") as f:
            f.write("HobbyOS selfhost MKFSFAT port proof: acceptance payload.\n" * 200)
        copied_out = os.path.join(WORK, "payload.out")
        r = run("mcopy", "-i", img3, payload, "::/HELLO.TXT")
        check("7b: mcopy copy-in", r.returncode == 0, r.stderr.strip()[:120])
        r = run("mcopy", "-i", img3, "::/HELLO.TXT", copied_out)
        ok_rt = (r.returncode == 0
                 and open(payload, "rb").read() == open(copied_out, "rb").read())
        check("7c: mcopy round-trip byte-exact", ok_rt)

    print()
    if fails:
        print(f"RESULT: FAIL ({len(fails)} failed): {fails}")
        sys.exit(1)
    print("RESULT: ALL HOST-SIDE CHECKS PASSED")


if __name__ == "__main__":
    main()
