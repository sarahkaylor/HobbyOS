#!/usr/bin/env python3
"""check_wpr_bus_provenance -- gate the WPR/booter hand-off on address provenance.

THE CLAIM
---------
Every address in the WPR metadata block, and every entry in the radix3 tree,
that the booter or GSP-RM reads is the BUS address nv_dma_bus() issued for the
mapping that owns those bytes -- never a `.phys`.  `bus` and `phys` are equal
only while VT-d translation is off (src/include/nv_dma.h), so a field built from
phys works on this machine and points the booter at unmapped memory the first
time translation is live.  There is no error path for that: the booter walks a
page table that is not there and the card reports a mailbox code.

This is the sibling of tools/nv/check_rpc_bus_provenance.py (the RPC transport
path, t_b6fc285d).  The two cover different halves of the same rule; either one
can land without the other.

WHAT IS AUDITED
---------------
  * src/kernel/nv_gsp.c, the body of nv_gsp_firmware_check(): the four
    booter-read metadata fields (sysmem_addr_radix3_elf / _bootloader /
    _signature and the radix3 build), plus the SEC2 DMA source address.
    `.phys` there is permitted exactly three ways, all CPU-side (ALLOWED_META).
  * src/kernel/nv_fw.c: the radix3 builders (entries must be BUS + offset;
    the tree and the image are nv_dma mappings), and nv_fw_load_blob() (a
    mapping, not a frame run).
  * src/include/nv_fw.h and src/include/nv.h: no `phys` member survives in
    struct nv_fw_blob, and the boot-path state carries a bus/root_bus field.

RULES
-----
  A. In the firmware-check region, no `m.sysmem_addr_*` assignment is built
     from `.phys`, and every one of them spells a bus value.
  B. In that region every line containing `.phys` matches one of the three
     CPU-side forms (phys_to_virt to REACH the bytes, a diagnostic log row, or
     the one `img_phys` local that the log uses).
  C. No device-facing call in that region (nv_radix3_build,
     nv_gsp_sec2_start_booter_ex, falcon_dma_block, nv_wr32/nv_wr64) takes an
     argument containing `.phys`.
  D. Positive: the seam is still one accessor -- nv_fw_blob_bus() of the RM and
     loader blobs, tree.root_bus in the metadata, loader_bus + 0x6C and
     img_bus + sig_off as OFFSETS, d->booter_bus as the SEC2 DMA source.
  E. nv_fw.c: no fw_put64() writes a `phys` expression into the tree; the
     level-2 PTEs use image_bus and both pointer levels use tree_bus.
  F. nv_fw.c: nv_fw_load_blob() allocates through nv_dma_alloc(), and
     struct nv_fw_blob has no physical-address member of its own.

Comment and string contents are stripped before every rule, so prose that
mentions `phys` is not a finding -- only code is.

Exit: 0 clean, 1 violation, 2 an audited marker is missing (the code moved and
this guard must be updated deliberately, not bypassed).

Runs in well under a second, stdlib only.  Wired into `make host_tests`
(`make wpr_bus_provenance` runs it alone).
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

NV_GSP_C = ROOT / "src/kernel/nv_gsp.c"
NV_FW_C = ROOT / "src/kernel/nv_fw.c"
NV_FW_H = ROOT / "src/include/nv_fw.h"
NV_H = ROOT / "src/include/nv.h"

META_START = "int nv_gsp_firmware_check(struct nv_dev *d) {"
META_END = "int nv_gsp_boot(struct nv_dev *d) {"

RADIX_INTO = "int nv_radix3_build_into("
RADIX_BUILD = "int nv_radix3_build("
LOAD_BLOB = "int nv_fw_load_blob(const char *name, struct nv_fw_blob *out) {"

# The three CPU-side uses of the mapping's physical address in the boot path.
# Each is an address the CPU dereferences, or a number printed for the log;
# none crosses to the device.
ALLOWED_META = [
    (re.compile(r"phys_to_virt\s*\([^;]*\.phys\s*\)"),
     "CPU view of the bytes"),
    (re.compile(r"uart_print_hex\s*\([^;]*\.phys\s*\)"),
     "diagnostic log row (CPU-side address, never published)"),
    (re.compile(r"^\s*uint64_t\s+img_phys\s*=\s*rm\.map\.phys\s*;\s*$"),
     "the image's CPU base, used by the log row only"),
]

META_FIELD = re.compile(r"\bm\.sysmem_addr_[a-z0-9_]+\s*=\s*[^;]*;")
BUS_SPELLING = re.compile(r"(_bus\b|\.root_bus\b)")

DEVICE_CALLS = re.compile(
    r"\b(nv_radix3_build|nv_gsp_sec2_start_booter_ex|falcon_dma_block|"
    r"nv_wr32|nv_wr64|nv_rd32|nv_rd64)\s*\(")

REQUIRED_META = [
    "uint64_t img_bus = nv_fw_blob_bus(&rm);",
    "uint64_t loader_bus = (ldr_rc == 0) ? nv_fw_blob_bus(&loader) : 0;",
    "struct nv_radix3_tree tree;",
    "nv_radix3_build(img_phys, img_bus, elf.fwimage.size, &tree)",
    "m.sysmem_addr_radix3_elf = tree.root_bus;",
    "m.sysmem_addr_signature = sig_off ? (img_bus + sig_off) : 0;",
    "loader_bus + 0x6C",
    "d->booter_bus = nv_fw_blob_bus(&booter);",
]

# The SEC2 xfer engine's source address lives in nv_gsp_sec2_start_booter_ex(),
# outside the metadata region (it is the DMA that READS the booter).
REQUIRED_SEC2 = "uint64_t base = d->booter_bus;"

REQUIRED_FW = [
    "image_bus + i * NV_RADIX3_PAGE",
    "+ tree_bus + (uint64_t)j * NV_RADIX3_PAGE",
    "+ tree_bus + (uint64_t)k * NV_RADIX3_PAGE",
    "nv_dma_alloc((uint64_t)need * NV_RADIX3_PAGE, &out->map)",
    "nv_dma_alloc((uint64_t)pages * NV_DMA_PAGE, &map)",
]


def strip_c(source):
    """Blank comments, string and character literals; keep every newline.

    Returns text with the same line numbering as `source`, so a finding can be
    reported as file:line and the reviewer can look at the real line.
    """
    out = []
    i, n = 0, len(source)
    state = "code"
    while i < n:
        c = source[i]
        if state == "code":
            if c == "/" and i + 1 < n and source[i + 1] == "/":
                state, i = "line", i + 2
                out.append("  ")
            elif c == "/" and i + 1 < n and source[i + 1] == "*":
                state, i = "block", i + 2
                out.append("  ")
            elif c == '"':
                state, i = "str", i + 1
                out.append(" ")
            elif c == "'":
                state, i = "chr", i + 1
                out.append(" ")
            else:
                out.append(c)
                i += 1
        elif state == "line":
            out.append("\n" if c == "\n" else " ")
            if c == "\n":
                state = "code"
            i += 1
        elif state == "block":
            if c == "*" and i + 1 < n and source[i + 1] == "/":
                state, i = "code", i + 2
                out.append("  ")
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        else:  # str / chr
            if c == "\\" and i + 1 < n:
                out.append(" ")
                out.append("\n" if source[i + 1] == "\n" else " ")
                i += 2
            elif (state == "str" and c == '"') or (state == "chr" and c == "'"):
                state, i = "code", i + 1
                out.append(" ")
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
    return "".join(out)


def load(path):
    if not path.is_file():
        raise MarkerMissing("audited file missing: %s" % path.relative_to(ROOT))
    return strip_c(path.read_text(encoding="utf-8", errors="replace"))


class MarkerMissing(Exception):
    pass


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def region_between(text, start_marker, end_marker, where):
    start = text.find(start_marker)
    if start < 0:
        raise MarkerMissing("audited marker missing in %s: %r" % (where, start_marker))
    end = text.find(end_marker, start)
    if end < 0:
        raise MarkerMissing("audited end marker missing in %s: %r" % (where, end_marker))
    return start, text[start:end]


def call_args(text, open_paren):
    depth = 0
    for i in range(open_paren, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
    return text[open_paren + 1:open_paren + 500]


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("--gate", action="store_true",
                    help="exit non-zero on a violation (the Makefile form)")
    ap.add_argument("--verbose", action="store_true",
                    help="print every checked fact, not only violations")
    args = ap.parse_args()

    violations = []
    stats = []

    try:
        gsp_text = load(NV_GSP_C)
        fw_text = load(NV_FW_C)
        fw_h = load(NV_FW_H)
        nv_h = load(NV_H)
    except MarkerMissing as e:
        print("nv wpr bus provenance: FAIL (audited marker missing)")
        print("  " + str(e))
        return 2

    try:
        offset, region = region_between(gsp_text, META_START, META_END,
                                        "src/kernel/nv_gsp.c")
        radix_start, radix_into = region_between(
            fw_text, RADIX_INTO, RADIX_BUILD, "src/kernel/nv_fw.c")
        _, radix_build = region_between(
            fw_text, RADIX_BUILD, "void nv_radix3_release(", "src/kernel/nv_fw.c")
        _, load_blob = region_between(
            fw_text, LOAD_BLOB, "int nv_fw_load(", "src/kernel/nv_fw.c")
    except MarkerMissing as e:
        print("nv wpr bus provenance: FAIL (audited marker missing)")
        print("  " + str(e))
        return 2

    region_line = gsp_text.count("\n", 0, offset) + 1

    # ---- A: the four metadata fields are BUS values -----------------------
    fields = 0
    for m in META_FIELD.finditer(region):
        fields += 1
        stmt = re.sub(r"\s+", " ", m.group(0))
        if ".phys" in stmt:
            violations.append(
                "src/kernel/nv_gsp.c:%d: %s -- a metadata field the BOOTER"
                " reads is built from phys; it must be a bus value"
                " (nv_dma_bus) plus an offset inside that mapping"
                % (region_line + line_of(region, m.start()) - 1, stmt))
        elif not BUS_SPELLING.search(stmt):
            violations.append(
                "src/kernel/nv_gsp.c:%d: %s -- metadata field has no bus"
                " spelling; this guard cannot tell what it is"
                % (region_line + line_of(region, m.start()) - 1, stmt))
    if fields < 3:
        violations.append(
            "src/kernel/nv_gsp.c: only %d sysmem_addr_* assignment(s) found in"
            " nv_gsp_firmware_check(); the three booter-read addresses moved or"
            " were renamed -- update this guard with the new expressions"
            % fields)
    stats.append("metadata: %d sysmem_addr_* field(s), all bus-derived" % fields)

    # ---- D: the seam is still one accessor --------------------------------
    flat = re.sub(r"\s+", " ", region)
    for needle in REQUIRED_META:
        if re.sub(r"\s+", " ", needle) not in flat:
            violations.append(
                "src/kernel/nv_gsp.c: seam expression not found: %r -- the"
                " WPR/booter glue must publish the nv_dma_bus() answer; update"
                " this guard with the new expression" % needle)
    stats.append("seam: nv_fw_blob_bus() -> img_bus/loader_bus/root_bus -> meta")

    # ---- D2: the SEC2 DMA source is the blob's bus address ----------------
    if re.sub(r"\s+", " ", REQUIRED_SEC2) not in re.sub(r"\s+", " ", gsp_text):
        violations.append(
            "src/kernel/nv_gsp.c: seam expression not found: %r -- the xfer"
            " engine reads the booter out of system memory, so that source is a"
            " bus address; update this guard with the new expression"
            % REQUIRED_SEC2)

    # ---- B: every `.phys` in the region is one of the CPU-side forms ------
    allowed_hits = 0
    for lineno, line in enumerate(region.splitlines(), start=1):
        if ".phys" not in line:
            continue
        if any(rx.search(line) for rx, _ in ALLOWED_META):
            allowed_hits += 1
            continue
        violations.append(
            "src/kernel/nv_gsp.c:%d: %.120s -- .phys outside the three CPU-side"
            " uses; a device-visible address must come from nv_dma_bus()"
            % (region_line + lineno - 1, line.strip()))
    stats.append("boot path: %d allowed .phys use(s), all CPU-side" % allowed_hits)

    # ---- C: no device-facing call takes a `.phys` expression --------------
    device_hits = 0
    for m in DEVICE_CALLS.finditer(region):
        device_hits += 1
        a = call_args(region, m.end() - 1)
        if ".phys" in a:
            violations.append(
                "src/kernel/nv_gsp.c:%d: %s(... .phys ...) -- device-facing"
                " call fed a physical address"
                % (region_line + line_of(region, m.start()) - 1, m.group(1)))
    stats.append("device-facing calls scanned: %d" % device_hits)

    # ---- E: no phys expression is written into the tree -------------------
    pte_hits = 0
    for body, where in ((radix_into, "nv_radix3_build_into"),
                        (radix_build, "nv_radix3_build")):
        for m in re.finditer(r"\bfw_put64\s*\(", body):
            pte_hits += 1
            a = call_args(body, m.end() - 1)
            if "phys" in a:
                violations.append(
                    "src/kernel/nv_fw.c: %s(): fw_put64(... phys ...) -- a tree"
                    " entry is an address the DEVICE walks; it must be"
                    " image_bus/tree_bus plus a page offset" % where)
    fw_flat = re.sub(r"\s+", " ", radix_into + " " + radix_build + " " + load_blob)
    for needle in REQUIRED_FW:
        if re.sub(r"\s+", " ", needle) not in fw_flat:
            violations.append(
                "src/kernel/nv_fw.c: seam expression not found: %r -- the tree"
                " must hold bus addresses and the blobs must be nv_dma"
                " mappings; update this guard with the new expression" % needle)
    stats.append("radix3: %d fw_put64() store(s), all bus-derived" % pte_hits)

    # ---- F: the loader owns a mapping, and the blob has no phys member ----
    if "frame_alloc_contig(" in load_blob:
        violations.append(
            "src/kernel/nv_fw.c: nv_fw_load_blob() still calls"
            " frame_alloc_contig() -- a blob that publishes an address must own"
            " an nv_dma mapping")
    blob_body = ""
    if "struct nv_fw_blob" in fw_h:
        blob_body = fw_h.split("struct nv_fw_blob", 1)[1].split("};", 1)[0]
    else:
        violations.append(
            "src/include/nv_fw.h: struct nv_fw_blob not found -- the blob must"
            " carry its mapping; update this guard")
    if re.search(r"\buint64_t\s+phys\s*;", blob_body):
        violations.append(
            "src/include/nv_fw.h: struct nv_fw_blob declares a bare `phys`"
            " member; the mapping (map) is the only carrier")
    if "struct nv_radix3_tree" not in fw_h:
        violations.append(
            "src/include/nv_fw.h: struct nv_radix3_tree not found -- the tree's"
            " mapping and its root bus must be one return value")
    nv_dev_body = ""
    if "struct nv_dev {" in nv_h:
        nv_dev_body = nv_h.split("struct nv_dev {", 1)[1].split("};", 1)[0]
    else:
        violations.append(
            "src/include/nv.h: struct nv_dev not found -- update this guard")
    nv_dev_flat = re.sub(r"[ \t]+", " ", nv_dev_body)
    if "uint64_t booter_bus;" not in nv_dev_flat:
        violations.append(
            "src/include/nv.h: `uint64_t booter_bus;` not found in struct nv_dev"
            " -- the SEC2 DMA source must be a bus address")
    if "struct nv_radix3_tree radix3;" not in nv_dev_flat:
        violations.append(
            "src/include/nv.h: `struct nv_radix3_tree radix3;` not found in"
            " struct nv_dev -- the boot path must keep the tree's mapping, not"
            " a root phys")
    if "booter_phys" in nv_dev_flat or "radix3_root;" in nv_dev_flat:
        violations.append(
            "src/include/nv.h: struct nv_dev carries a raw phys root/booter"
            " again -- both are device-visible addresses (nv_dma_bus)")
    stats.append("loader: nv_dma_alloc mapping, no phys member on the blob")

    if args.verbose:
        for s in stats:
            print("  " + s)

    if violations:
        print("nv wpr bus provenance: FAIL (%d violation%s)"
              % (len(violations), "" if len(violations) == 1 else "s"))
        for v in violations:
            print("  " + v)
        return 1

    print("nv wpr bus provenance: ok -- %d metadata field(s), %d tree store(s),"
          " %d device-facing call(s), %d allowed CPU-side .phys use(s),"
          " 0 raw phys" % (fields, pte_hits, device_hits, allowed_hits))
    return 0


if __name__ == "__main__":
    sys.exit(main())
