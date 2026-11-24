#!/usr/bin/env python3
"""Is the SEC2 booter's application-code region plaintext RISC-V or ciphertext?

Offline analysis of the 535.113.01 GSP booter containers.  The question this
answers (docs/nv-plan.md, WP-A5 next-step 2): the booter_load app-code blob is
the last input to the SEC2 launch that the host cannot verify, because secure
IMEM reads back 0xDEAD5EC1.  If those bytes are ciphertext, loading them as
plaintext code guarantees the boot ROM decrypts garbage, and no amount of
signature/layout fiddling can fix it.

Three independent measurements per region:

  (a) byte statistics   Shannon entropy, index of coincidence, zero fraction,
                        compressibility, and per-256-byte-window entropy.
                        Plaintext RISC-V code is ~4.5-6.5 bits/byte and
                        compresses; ciphertext is ~8.0 and does not.
  (b) disassembly       every byte stream is wrapped in an ELF and run through
                        llvm-objdump for riscv64 (RVC enabled).  A plaintext
                        image decodes with >98% known instructions; a random
                        stream leaves ~95% <unknown>.  A run-length count shows
                        whether valid instructions ever chain together.
  (c) block structure    a 16-byte block cipher in ECB mode leaks repeated
                        plaintext blocks as repeated ciphertext blocks.  Uniform
                        random data has none.  A trailing run of identical
                        ciphertext blocks is the signature of zero padding.
                        That is how the AES-128-ECB construction is established
                        without the key.

Usage:
    python3 analyse_booter_appcode.py [--root ../../third_party/nvfw] [--dump FILE]

All numbers in docs/nv-booter-appcode.md come from this program; the captured
output is analyse_booter_appcode.out next to it.  No file in the firmware tree
is modified.
"""
import argparse
import collections
import hashlib
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

FW_FILES = [
    "ad102/gsp/booter_load-535.113.01.bin",
    "ad102/gsp/booter_unload-535.113.01.bin",
    "ga102/gsp/booter_load-535.113.01.bin",
    "ga102/gsp/booter_unload-535.113.01.bin",
]

AD102_LOAD = "ad102/gsp/booter_load-535.113.01.bin"
RM_IMAGE = "ga102/gsp/gsp-535.113.01.bin"
# SHA-256 of the pinned blobs (docs/nv-gsp-firmware.md §2); a mismatch means
# the analysis is not about the bytes the document describes.
PINNED = {
    AD102_LOAD: "7d4ef3258da016f5208cfb1b348a9a1d69aaa13c2f8d4ee8c05d66bd05b45cd6",
    "ga102/gsp/booter_load-535.113.01.bin":
        "3ab46bf4c70d72e8fc89d98e6bdc9b83a61954a586629d26b8b5653a620237e0",
}

# The AES-128-ECB ciphertext of sixteen zero bytes under NVIDIA's public
# debug test key (key number 37), as published for the GA100 booter:
#   https://github.com/Consensus-Protocol/cmp170hx docs/unlock/falcon-and-booter.md
# A production image must NOT produce this value for its zero padding.
DEBUG_ZERO_BLOCK = bytes.fromhex("717d1494eaca317ff106195258b38377")


def le32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def entropy(x):
    n = len(x)
    if not n:
        return 0.0
    return -sum((v / n) * math.log2(v / n) for v in collections.Counter(x).values())


def index_of_coincidence(x):
    n = len(x)
    if n < 2:
        return 0.0
    return sum(v * (v - 1) for v in collections.Counter(x).values()) / (n * (n - 1))


def parse_container(b, name):
    """Decode NVFW_BIN_HDR + NVFW_HS_UCODE_HDR(v2) + NVFW_HS_LOAD_HDR."""
    out = {"name": name, "file_size": len(b)}
    if len(b) < 24:
        out["error"] = "shorter than nvfw_bin_hdr"
        return out
    (magic, ver, bin_size, hdr_off, data_off, data_size) = struct.unpack("<6I", b[:24])
    out.update(magic=magic, version=ver, bin_size=bin_size, hdr_off=hdr_off,
               data_off=data_off, data_size=data_size,
               end=data_off + data_size,
               tiles_file=(data_off + data_size == len(b)))
    if magic != 0x10DE or ver != 1:
        out["error"] = "not a v1 0x10DE container"
        return out
    hs = [le32(b, hdr_off + 4 * i) for i in range(9)]
    (out["sig_prod_off"], out["sig_prod_size"], out["patch_loc"], out["patch_sig"],
     out["meta_off"], out["meta_size"], out["num_sig_off"], out["loadhdr_off"],
     out["loadhdr_size"]) = hs
    if out["loadhdr_off"] + out["loadhdr_size"] > len(b):
        out["error"] = "load header outside the file"
        return out
    out["patch_site"] = le32(b, out["patch_loc"])
    out["num_sig"] = le32(b, out["num_sig_off"])
    out["meta"] = [le32(b, out["meta_off"] + 4 * i)
                   for i in range(out["meta_size"] // 4)]
    lh = [le32(b, out["loadhdr_off"] + 4 * i)
          for i in range(out["loadhdr_size"] // 4)]
    out["loadhdr"] = lh
    (out["os_code_off"], out["os_code_size"], out["os_data_off"],
     out["os_data_size"], out["num_apps"]) = lh[:5]
    out["apps"] = [lh[5 + 4 * i: 9 + 4 * i] for i in range(out["num_apps"])]
    total = out["os_code_size"] + out["os_data_size"] + sum(
        a[1] + a[3] for a in out["apps"])
    out["size_sum"] = total
    out["regions"] = [("os_code", out["os_code_off"], out["os_code_size"]),
                      ("os_data", out["os_data_off"], out["os_data_size"])]
    for i, a in enumerate(out["apps"]):
        out["regions"].append(("app%d_code" % i, a[0], a[1]))
        out["regions"].append(("app%d_data" % i, a[2], a[3]))
    return out


def region(b, c, off, size):
    start = c["data_off"] + off
    return b[start:start + size]


def byte_stats(x):
    return {
        "len": len(x),
        "H": entropy(x),
        "IC": index_of_coincidence(x),
        "zero": x.count(0) / len(x) if x else 0.0,
        "distinct": len(set(x)),
        "zlib": len(zlib.compress(x, 9)) / len(x) if x else 0.0,
    }


def trajectories(x):
    """Trailing run of identical 16-byte blocks: zero padding under ECB."""
    if len(x) % 16:
        return 0, b""
    last = x[-16:]
    n = 0
    for i in range(len(x) // 16 - 1, -1, -1):
        if x[i * 16:(i + 1) * 16] == last:
            n += 1
        else:
            break
    return n, last


def block_repeats(x, bs):
    n = len(x) // bs
    c = collections.defaultdict(list)
    for i in range(n):
        c[x[i * bs:(i + 1) * bs]].append(i)
    reps = {k: v for k, v in c.items() if len(v) > 1}
    return n, len(c), reps


def ascii_runs(x, minimum=6):
    """Printable runs, used to show that some regions are intelligible text."""
    out = []
    run = b""
    start = 0
    for i, v in enumerate(x):
        if 32 <= v < 127:
            if not run:
                start = i
            run += bytes([v])
        else:
            if len(run) >= minimum:
                out.append((start, run))
            run = b""
    if len(run) >= minimum:
        out.append((start, run))
    return out


# ---------------------------------------------------------------- disassembly
DIS_ELF = "elf64-littleriscv"
RISCV_MATTR = "+c,+m,+a,+f,+d"


def disassemble(blob, tmpdir, label):
    """Return instruction-parcel statistics for one byte stream.

    RVC is dense, so a uniform random stream still decodes ~84% of its 16-bit
    parcels as *some* instruction.  The decisive number is the longest run of
    consecutive decoded instructions: real code runs to the end of the image,
    ciphertext breaks every few parcels.
    """
    if not (shutil.which("llvm-objcopy") and shutil.which("llvm-objdump")):
        return None
    raw = os.path.join(tmpdir, "blob.bin")
    elf = os.path.join(tmpdir, "blob.elf")
    with open(raw, "wb") as f:
        f.write(blob)
    subprocess.run(["llvm-objcopy", "-I", "binary", "-O", DIS_ELF,
                    "--binary-architecture=riscv64", raw, elf],
                   check=True, capture_output=True)
    out = subprocess.run(["llvm-objdump", "-d", "--triple=riscv64",
                          "--mattr=" + RISCV_MATTR, "--section=.data", elf],
                         capture_output=True, text=True, check=True).stdout
    mnemonics = collections.Counter()
    known_flags = []
    offsets = []
    addrs = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+(.*)$", line)
        if not m:
            continue
        addrs.append(int(m.group(1), 16))
        offsets.append(int(m.group(1), 16))
        body = m.group(2)
        if "<unknown>" in body:
            known_flags.append(False)
        else:
            known_flags.append(True)
            mnemonics[body.strip().split("\t")[0].split()[0]] += 1
    total = len(known_flags)
    unknown = total - sum(known_flags)
    run = best = 0
    first_bad = None
    for ok, off in zip(known_flags, offsets):
        if ok:
            run += 1
            best = max(best, run)
        else:
            run = 0
            if first_bad is None:
                first_bad = off
    widths = collections.Counter(addrs[i + 1] - addrs[i] for i in range(len(addrs) - 1))
    return {"total": total, "unknown": unknown, "known": total - unknown,
            "run": best, "first_bad": first_bad, "widths": dict(widths),
            "mnemonics": mnemonics.most_common(5)}


def disassembly_report(app, root, tmpdir):
    print("\n-- (b) disassembly: every byte stream through llvm-objdump riscv64 --")
    rng = random.Random(0x10DE)
    samples = [("app-code (the region in question)", app),
               ("uniform random (same length)",
                bytes(rng.randrange(256) for _ in range(len(app))))]
    rm_path = os.path.join(root, RM_IMAGE)
    if os.path.exists(rm_path):
        rm = open(rm_path, "rb").read()
        samples.append(("control: plaintext RISC-V, RM .fwimage@0x100000",
                        rm[0x100000:0x100000 + len(app)]))
    else:
        print("   (plaintext control unavailable: %s not fetched)" % RM_IMAGE)
    print("   note: RVC is dense -- most random 16-bit parcels decode as an"
          " instruction, so the longest valid run is the discriminator")
    for label, blob in samples:
        r = disassemble(blob, tmpdir, label)
        if r is None:
            print("   llvm-objdump/llvm-objcopy not installed; (b) not measured")
            return
        print("   %-46s parcels %6d  <unknown> %6d (%5.1f%%)  longest valid run %5d"
              "  first invalid @%s" % (
                  label, r["total"], r["unknown"],
                  100.0 * r["unknown"] / r["total"], r["run"],
                  ("%#x" % r["first_bad"]) if r["first_bad"] is not None else "none"))
        print("      top mnemonics: %s" % " ".join("%s:%d" % kv for kv in r["mnemonics"]))


# ---------------------------------------------------------------------- main
def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.path.join(here, "..", "..", "third_party", "nvfw"),
                    help="directory holding ad102/ and ga102/ (default: third_party/nvfw)")
    ap.add_argument("--dump", help="write the AD102 app-code bytes here and exit after the dump")
    args = ap.parse_args()
    root = os.path.abspath(args.root)

    print("booter app-code analysis — root %s" % root)
    print("tool: analyse_booter_appcode.py; hint: 535.113.01, NVFW v1 containers")

    parsed = {}
    for rel in FW_FILES:
        p = os.path.join(root, rel)
        if not os.path.exists(p):
            print("\n== %s: MISSING" % rel)
            continue
        b = open(p, "rb").read()
        c = parse_container(b, rel)
        parsed[rel] = (b, c)
        print("\n== %s (%d bytes)" % (rel, len(b)))
        if "error" in c:
            print("   unparsable: %s" % c["error"])
            continue
        print("   container: magic %#x ver %d  data_off %#x data_size %#x  end %#x"
              "  tiles file %s" % (c["magic"], c["version"], c["data_off"],
                                   c["data_size"], c["end"], c["tiles_file"]))
        print("   HS(v2):   sig %#x/%#x  patch_loc %#x (patch site %#x)  patch_sig %#x"
              "  meta %#x/%#x %s  num_sig %d  load_hdr %#x/%#x" % (
                  c["sig_prod_off"], c["sig_prod_size"], c["patch_loc"],
                  c["patch_site"], c["patch_sig"], c["meta_off"], c["meta_size"],
                  c["meta"], c["num_sig"], c["loadhdr_off"], c["loadhdr_size"]))
        print("   load hdr: os_code (data+%#x,%#x)  os_data (data+%#x,%#x)"
              "  apps %d %s" % (c["os_code_off"], c["os_code_size"],
                               c["os_data_off"], c["os_data_size"], c["num_apps"],
                               ["code(data+%#x,%#x) data(data+%#x,%#x)" %
                                (a[0], a[1], a[2], a[3]) for a in c["apps"]]))
        print("   header sizes sum to %#x, data_size %#x -> %s" % (
            c["size_sum"], c["data_size"], c["size_sum"] == c["data_size"]))
        for name, off, size in c["regions"]:
            if not size:
                continue
            x = region(b, c, off, size)
            s = byte_stats(x)
            n16, distinct, reps = block_repeats(x, 16)
            run, tail = trajectories(x)
            print("   %-10s data+%#06x  %6d B  H=%.4f IC=%.5f zero=%.3f "
                  "zlib=%.3f distinct=%d ascii>=6=%d 16B:%d/%d rep=%d tailrun=%d%s" % (
                      name, off, s["len"], s["H"], s["IC"], s["zero"], s["zlib"],
                      s["distinct"], len(ascii_runs(x)), distinct, n16, len(reps), run,
                      (" tail=%s" % tail.hex()) if run > 1 else ""))

    # ---- the detailed case
    if AD102_LOAD in parsed:
        b, c = parsed[AD102_LOAD]
        app = region(b, c, c["apps"][0][0], c["apps"][0][1])
        if args.dump:
            open(args.dump, "wb").write(app)
            print("\nwrote %d app-code bytes to %s" % (len(app), args.dump))
            return

        print("\n== detail: %s app0 code (file [%#x,%#x))" % (
            AD102_LOAD, c["data_off"] + c["apps"][0][0],
            c["data_off"] + c["apps"][0][0] + len(app)))
        s = byte_stats(app)
        print("   sha256      %s" % hashlib.sha256(app).hexdigest())
        print("   length      %d B = %d x 16 B" % (len(app), len(app) // 16))
        print("   (a) byte statistics")
        print("       H(byte) %.4f bits   H_min %.4f   IC %.5f (uniform = %.5f)"
              % (s["H"], -math.log2(max(collections.Counter(app).values()) / len(app)),
                 s["IC"], 1.0 / 256))
        print("       distinct byte values %d/256   zero fraction %.4f   zlib ratio %.4f"
              % (s["distinct"], s["zero"], s["zlib"]))
        hs = [entropy(app[i:i + 256]) for i in range(0, len(app), 256)]
        print("       per-256-B window entropy: n=%d min %.4f mean %.4f max %.4f"
              % (len(hs), min(hs), sum(hs) / len(hs), max(hs)))
        print("   (c) block structure")
        for bs in (16, 32, 64):
            n, distinct, reps = block_repeats(app, bs)
            print("       %2d-B blocks: %d total, %d distinct, %d repeated values,"
                  " %d excess" % (bs, n, distinct, len(reps), n - distinct))
        print("       printable runs >= 6 B in the app-code region: %d" %
              len(ascii_runs(app)))
        dmem = region(b, c, c["os_data_off"], c["os_data_size"])
        print("       same container, DMEM image: %d printable runs >= 6 B, e.g. %s"
              % (len(ascii_runs(dmem)),
                 [r.decode("ascii") for _, r in ascii_runs(dmem)[:4]]))
        run, tail = trajectories(app)
        print("       trailing identical 16-B blocks: %d  (=> %d zero plaintext bytes"
              " before padding, if the tail is padding)" % (run, run * 16))
        print("       tail block %s" % tail.hex())
        print("       != AES-128-ECB(K_debug37, 0^16) %s : %s"
              % (DEBUG_ZERO_BLOCK.hex(), tail != DEBUG_ZERO_BLOCK))
        print("       repeated 16-B block values and their block indices:")
        for k, v in sorted(block_repeats(app, 16)[2].items(), key=lambda kv: kv[1][0]):
            print("         count %d  %s  %s" % (len(v), k.hex(), v))

        # Cross-image probe: the same plaintext block under the same key gives
        # the same ciphertext block; the zero-padding block is a fixed plaintext
        # every image ends with, so it names the key scope.
        print("\n   (c2) key-scope probe: shared 16-B ciphertext blocks between images")
        blk = {}
        for rel, (bb, cc) in parsed.items():
            if "error" in cc or not cc["apps"]:
                continue
            x = region(bb, cc, cc["apps"][0][0], cc["apps"][0][1])
            blk[rel] = [x[i * 16:(i + 1) * 16] for i in range(len(x) // 16)]
        rels = list(blk)
        for i in range(len(rels)):
            for j in range(i + 1, len(rels)):
                common = set(blk[rels[i]]) & set(blk[rels[j]])
                print("       %-38s vs %-38s %4d shared" %
                      (rels[i], rels[j], len(common)))
        for rel, bs in blk.items():
            run, tail = trajectories(b"".join(bs))
            if run:
                print("       zero-padding block %-38s %s  (%d trailing blocks)" %
                      (rel, tail.hex(), run))
        with tempfile.TemporaryDirectory() as td:
            disassembly_report(app, root, td)

        # ---- (c) what the headers say
        print("\n-- (c) what the container headers declare --")
        print("   nvfw_bin_hdr: magic 0x10DE version 1, data_off 0x%x, data_size 0x%x"
              " (no compression field, no uncompressed-size field)" % (
                  c["data_off"], c["data_size"]))
        print("   nvfw_hs_ucode_hdr_v2: %d signature slot(s) of %d B; the two slots are"
              " RSA-3072-sized and the driver patches the fuse-selected one over the"
              " image at data+0x%x" % (c["num_sig"], c["sig_prod_size"] // c["num_sig"],
                                       c["patch_site"] - c["data_off"]))
        print("   signature meta: fuse_ver %d engine_id %d ucode_id %d"
              % tuple(c["meta"][:3]))
        print("   nvfw_hs_load_hdr_v2: os_code (data+0x%x,0x%x), os_data (data+0x%x,0x%x),"
              " app0 code (data+0x%x,0x%x)" % (
                  c["os_code_off"], c["os_code_size"], c["os_data_off"],
                  c["os_data_size"], c["apps"][0][0], c["apps"][0][1]))
        print("   the load header is a copy descriptor: offsets/sizes/counts only,"
              " and its regions tile data_size exactly (%s)"
              % (c["size_sum"] == c["data_size"]))
        print("   => nothing in the headers marks the image encrypted; the encoding is"
              " only observable in the bytes, and the BROM is the only entity that runs"
              " before the CPU does")


if __name__ == "__main__":
    main()
