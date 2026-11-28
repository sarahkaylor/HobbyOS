#!/usr/bin/env python3
"""check67.py -- the no-boot proof for the four changes mode 67 carries.

Card t_240a0ac8, doc docs/gb202-vendor-diff.md section 5:
  A1  RM_NONHEAP_BYTES_GB202 must be the vendor's kgspGetNonWprHeapSize_ad951d
      (2293760 / 0x230000), not 2228224 (which is
      memmgrGetFBEndReserveSizeEstimate_GB100's reserve, not a non-WPR heap).
  A2  GspFwWprMeta.pmuReservedSize lives at +0xF4 as a u32 (measured by
      compiling the vendor header), our encoder writes it there and our
      decoder reads it back, and `verified` is still at +0xF8.
  B1  the COT FRTS-in-FB pair mode 67 sends is the vendor's arithmetic:
      align2M(0x220000 + 0x1000 + pmuReserved) = 0x1C00000, size 1 MiB.
  B3  the args-page target is 2 and the descriptor page's is 1.

Every assertion is against source text or arithmetic; none needs a boot.
Exit 0 when all assertions hold.
"""
import re
import subprocess
import sys
import os
import shutil

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREE = HERE
COT = os.path.join(TREE, "src/kernel/nv_fsp_cot.c")
FW = os.path.join(TREE, "src/kernel/nv_fw.c")
FWH = os.path.join(TREE, "src/include/nv_fw.h")

fails = []
ncheck = [0]
def check(ok, what):
    ncheck[0] += 1
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails.append(what)

cot = open(COT).read()
fw = open(FW).read()
fwh = open(FWH).read()

def ival(tok):
    """A C integer literal -> int ('0x1C00000ULL', '2293760u')."""
    return int(re.sub(r"[uUlL]+$", "", tok), 0)

# ---- A1 ---------------------------------------------------------------------
m = re.search(r"#define\s+RM_NONHEAP_BYTES_GB202\s+(\S+)", cot)
check(m is not None, "A1 RM_NONHEAP_BYTES_GB202 is defined")
if m:
    check(ival(m.group(1)) == 2293760,
          "A1 RM_NONHEAP_BYTES_GB202 == 2293760 (0x230000), got %s" % m.group(1))
check("2228224u" not in cot and "RM_NONHEAP_BYTES_GB202 2228224" not in cot,
      "A1 no definition or use site still carries 2228224")
check(2228224 == 0x220000, "A1 0x220000 is the FB-end reserve estimate value")
check("mem_mgr_gb100.c:57-67" in cot,
      "A1 the comment names where 0x220000 actually comes from")

# ---- A2 ---------------------------------------------------------------------
check("uint32_t pmu_reserved_size;" in fwh, "A2 struct nv_wpr_meta_in has pmu_reserved_size")
check("fw_put32(out + 0xF4, in->pmu_reserved_size);" in fw,
      "A2 encoder writes it at +0xF4")
check("out->pmu_reserved_size = fw_le32(in + 0xF4);" in fw,
      "A2 decoder reads it from +0xF4")
check("#define WPR_OFF_VERIFIED 0xF8u" in fw, "A2 verified is still at +0xF8")

# ---- the vendor header, compiled (not recomputed) ---------------------------
# The layout is measured by compiling 580.178.04's own header (no RM sources
# needed).  Use a compiler when there is one; otherwise read the captured
# output committed beside this file, so the gate also runs where cc is absent.
tool_dir = os.path.join(HERE, "tools", "nv")
vendor_prog = os.path.join(tool_dir, "meas_wprmeta")
if not os.path.exists(vendor_prog):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc:
        subprocess.run([cc, "-O1", "-w", os.path.join(tool_dir, "meas_wprmeta.c"),
                        "-I", tool_dir, "-o", vendor_prog],
                       capture_output=True, text=True)
out = ""
if os.path.exists(vendor_prog):
    out = subprocess.run([vendor_prog], capture_output=True, text=True).stdout
if not out:
    try:
        out = open(os.path.join(tool_dir, "wprmeta-580-offsets.txt")).read()
    except OSError:
        out = ""
check("sizeof(GspFwWprMeta) = 256" in out, "vendor: sizeof(GspFwWprMeta) == 256")
check(re.search(r"off pmuReservedSize\s+0x0f4", out) is not None,
      "vendor: pmuReservedSize is at 0x0F4 (compiled from 580.178.04's header)")
check(re.search(r"off verified\s+0x0f8", out) is not None,
      "vendor: verified is at 0x0F8")
check(re.search(r"off nonWprHeapSize\s+0x068", out) is not None,
      "vendor: nonWprHeapSize is at 0x068")

# ---- the vendor arithmetic --------------------------------------------------
def align_up(v, a):
    return (v + a - 1) & ~(a - 1)

pmu = align_up(0x900000 + 0x1000000 + 0x4000, 128 * 1024)
check(pmu == 0x1920000, "arith: align128K(0x900000+0x1000000+0x4000) == 0x1920000 (got 0x%x)"
      % pmu)
fvid = align_up(0x220000 + 0x1000 + pmu, 2 * 1024 * 1024)
check(fvid == 0x1C00000, "arith: align2M(0x220000+0x1000+pmuReserved) == 0x1C00000 (got 0x%x)"
      % fvid)

m = re.search(r"#define\s+RM_PMU_RESERVED_BYTES\s+(\S+)", cot)
check(m is not None and ival(m.group(1)) == pmu,
      "B2 RM_PMU_RESERVED_BYTES == 0x1920000")
m = re.search(r"#define\s+RM_FRTS_VID_OFF\s+(\S+)", cot)
check(m is not None and ival(m.group(1)) == fvid,
      "B1 RM_FRTS_VID_OFF == 0x1C00000")
m = re.search(r"#define\s+RM_FRTS_VID_BYTES\s+(\S+)", cot)
check(m is not None and ival(m.group(1)) == 0x100000,
      "B1 RM_FRTS_VID_BYTES == 1 MiB")

# ---- mode 67 is wired in every place it must be -----------------------------
check('(mode == 65 || mode == 67) ? "FMCV.BIN"' in cot,
      "mode 67 stages the 580.178.04 FMC container")
check("if ((mode == 65 || mode == 67) && fmc.size != RM_FMC580_BYTES)" in cot,
      "mode 67 gates the FMC container size")
check("mode == 65 || mode == 66 ||\n        mode == 67) {" in cot and
      "if (mode == 67) meta.pmu_reserved_size = RM_PMU_RESERVED_BYTES;" in cot,
      "mode 67 sets pmu_reserved_size in the descriptor")
check("if (mode == 67) fvid = RM_FRTS_VID_OFF;" in cot,
      "mode 67 sends the vendor frtsVidmemOffset")
check("mode == 67) ? 0x100000u : 0);" in cot,
      "mode 67 sends frtsVidmemSize = 1 MiB")
check("(mode == 7 || mode == 67)" in cot,
      "mode 67 sends args-page target 2 (A6)")
check("NV_FMCP_OFF_BOOTTGT, GSP_DMA_TARGET_COHERENT_SYSTEM" in cot,
      "B3 the descriptor page target is still 1")

print()
print("assertions %d failures %d" % (ncheck[0], len(fails)))
for f in fails:
    print("  FAILED: " + f)
sys.exit(1 if fails else 0)
