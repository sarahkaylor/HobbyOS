#!/usr/bin/env python3
"""hs_gate.py -- static pre-check for a secure-falcon (HS) container, before a boot.

Why this exists (docs/nv-no-rm-path.md): every no-RM path on AD102 ends up
handing a signed container to SEC2's boot ROM, and a boot is the expensive unit
here (~6.5 min of VM cycle). The container itself carries the identity triple
the boot ROM compares against the part's fuses, so most candidates can be
dropped -- or promoted -- without touching hardware. This tool prints that
triple, applies the checks we can defend, and refuses to claim a verdict it has
no measurement for.

Format (SOURCE: nouveau nvkm/include/nvfw/hs.h and nvkm/subdev/gsp/ga102.c
`ga102_gsp_booter_ctor`, which reads meta[0..2] as fuse_ver/engine_id/ucode_id):

  nvfw_bin_hdr      0x10de, version, size, header_offset, data_offset, data_size
  nvfw_hs_header_v2 at header_offset: sig_prod_offset/size, patch_loc, patch_sig,
                    meta_data_offset/size, num_sigs_offset, header_offset, header_size
                    (the 7th dword is an OFFSET to the count, not the count)
  meta[3]           fuse_ver, engine_id, ucode_id
  nvfw_hs_load_header_v2 at hs.header_offset: os_code/data, num_apps, app[]

Fuse table: MEASURED on the card (evidence/nv/f1r-dma-done-bit.log):
  0x8241C0..0x8241FF = 13 x 0x1, then 0x7F, 0x7FFF, 0x7FFF
so per-ucode fuse word = fuse_words[ucode_id - 1].

Usage:
  hs_gate.py CONTAINER...                 # report
  hs_gate.py --selftest                   # assert the known corpus, exit 1 on drift
"""
import json
import os
import struct
import sys

FUSE_WORDS = [0x1] * 13 + [0x7F, 0x7FFF, 0x7FFF]  # MEASURED, 0x8241C0..0x8241FF
BIN_MAGIC = 0x10DE

# The corpus the selftest pins. Names are the files as this card staged them.
CORPUS = {
    "ad102-booter_load-570.144.bin": dict(role="booter", ucode_id=3, fuse_ver=1,
                                          engine_id=0x1,
                                          verdict="IDENTITY-COMPATIBLE"),
    "ga102-booter_load-570.144.bin": dict(role="booter", ucode_id=3, fuse_ver=1,
                                          engine_id=0x1,
                                          verdict="IDENTITY-COMPATIBLE"),
    "tu102-booter_load-570.144.bin": dict(role="booter", ucode_id=13, fuse_ver=0,
                                          engine_id=0x1,
                                          verdict="UCODE-ID-NOT-FOR-THIS-PART"),
    "ga102-sec2-hs_bl_sig.bin": dict(role="container", ucode_id=5, fuse_ver=1,
                                     engine_id=0x1,
                                     verdict="IDENTITY-COMPATIBLE"),
    "ga102-acr-ucode_ahesasc.bin": dict(role="container", ucode_id=1, fuse_ver=2,
                                        engine_id=0x1,
                                        verdict="FUSE-PRECEDENT-MISSING"),
    "ga102-acr-ucode_asb.bin": dict(role="container", ucode_id=1, fuse_ver=2,
                                    engine_id=0x400,
                                    verdict="FUSE-PRECEDENT-MISSING"),
    "ga102-acr-ucode_unload.bin": dict(role="container", ucode_id=1, fuse_ver=2,
                                       engine_id=0x1,
                                       verdict="FUSE-PRECEDENT-MISSING"),
}

# The one role whose ucode id is measured: every AD102/GA102 SEC2 booter we have
# reports ucode_id 3, and the TU102/TU116 containers report 13 -- which is why the
# booter sweep dropped them statically (docs/nv-booter-candidates.md).
ROLE_UCODE_ID = {"booter": 3}


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def fls(x):
    """Highest set bit, 1-based; 0 for 0.  (The driver uses fls() the same way.)"""
    n = 0
    while x:
        n += 1
        x >>= 1
    return n


def parse(path, role="container"):
    b = open(path, "rb").read()
    r = {"file": os.path.basename(path), "size": len(b), "rows": [],
         "verdict": None, "role": role}
    if len(b) < 0x18 or u32(b, 0) != BIN_MAGIC:
        r["verdict"] = "NOT-A-CONTAINER"
        r["rows"].append(("bin_hdr", "no 0x10de magic (a payload, not a container)"))
        return r
    magic, ver, size, hdr_off, data_off, data_size = struct.unpack_from("<6I", b, 0)
    r["rows"].append(("bin_hdr", "v%d size=%d file=%d header=0x%x data=0x%x+%d"
                      % (ver, size, len(b), hdr_off, data_off, data_size)))
    # Which extent is authoritative is a measurement, not a convention: for a
    # container with a data region the published AD102 booter ends exactly at
    # data_offset+data_size (888+56832 = 57720 = file size) while its bin_size
    # header says 57856, i.e. bin_size overstates by 136 B.  A loader that
    # trusted bin_size would read past the file; one that trusted the data
    # region lands on its last byte.
    extent = (data_off + data_size) if data_size else size
    if extent != len(b):
        r["rows"].append(("bin_hdr", "EXTENT MISMATCH: %d vs file %d"
                          % (extent, len(b))))
    elif size != len(b):
        r["rows"].append(("bin_hdr", "note: bin_size overstates by %d B; the "
                          "data region is the authoritative extent"
                          % (size - len(b))))
    (sig_prod_off, sig_prod_size, patch_loc, patch_sig, meta_off, meta_size,
     num_sigs_off, load_off, load_size) = struct.unpack_from("<9I", b, hdr_off)
    if meta_off + 12 > len(b):
        r["verdict"] = "NO-META"
        return r
    fuse_ver, engine_id, ucode_id = struct.unpack_from("<3I", b, meta_off)
    r["triple"] = {"fuse_ver": fuse_ver, "engine_id": engine_id, "ucode_id": ucode_id}
    r["rows"].append(("identity", "fuse_ver=%d engine_id=0x%x (HS class 0x400: %s) ucode_id=%d"
                      % (fuse_ver, engine_id, bool(engine_id & 0x400), ucode_id)))
    # dword 7 of the header is the OFFSET of the signature count, not the count
    # itself (newer kernels call it numSigsOff).  Read the count from there; a
    # header that pointed outside the file would otherwise print 0x350 as 848.
    num_sigs = (u32(b, num_sigs_off)
                if num_sigs_off and num_sigs_off + 4 <= len(b) else None)
    r["rows"].append(("sig", "prod 0x%x+%d num_sigs_off=0x%x num_sigs=%s "
                      "patch_loc=0x%x patch_sig=0x%x"
                      % (sig_prod_off, sig_prod_size, num_sigs_off,
                         "?" if num_sigs is None else num_sigs,
                         patch_loc, patch_sig)))
    # A static gate cannot see which fused key a container was signed with.  Say
    # so, because "identity compatible" has been mistaken for "will run" before:
    # a GA102-signed booter on this AD102 answers 0x250009 (signature class).
    r["rows"].append(("sig", "signature class NOT CHECKED statically: a cross-chip "
                      "production signature is measured to fail on this part "
                      "(0x250009), and only AD102's own signature gets past it"))
    if load_off + 20 <= len(b):
        os_code_off, os_code_size, os_data_off, os_data_size, napps = \
            struct.unpack_from("<5I", b, load_off)
        r["rows"].append(("load_v2", "os_code 0x%x+%d os_data 0x%x+%d apps=%d"
                          % (os_code_off, os_code_size, os_data_off, os_data_size, napps)))
        r["layout"] = {"os_code_size": os_code_size, "os_data_size": os_data_size,
                       "data_size": data_size, "num_apps": napps}

    # ---- the checks -------------------------------------------------------
    want_ucode = ROLE_UCODE_ID.get(role)
    if want_ucode is not None and ucode_id != want_ucode:
        r["verdict"] = "UCODE-ID-NOT-FOR-THIS-PART"
        r["rows"].append(("role", "a %s container for this part must declare "
                          "ucode_id=%d (measured across every AD102/GA102 image); "
                          "this one declares %d" % (role, want_ucode, ucode_id)))
        return r
    if ucode_id < 1 or ucode_id > len(FUSE_WORDS):
        r["verdict"] = "UCODE-ID-NOT-FOR-THIS-PART"
        r["rows"].append(("fuse", "ucode_id=%d is outside the measured fuse table "
                          "(1..%d): no fuse word addresses it"
                          % (ucode_id, len(FUSE_WORDS))))
        return r
    word = FUSE_WORDS[ucode_id - 1]
    # The only compatibility this project has MEASURED is the booter's:
    # declared fuse_ver=1 with fuse word 0x1, which the driver scored
    # COMPATIBLE with sig_index 1. Under the reading "declared version v is
    # bit v-1 of the fuse word", that case is 1<<(1-1)=0x1 and it fits.
    expect = 1 << (fuse_ver - 1) if fuse_ver >= 1 else 0
    r["rows"].append(("fuse", "ucode_id=%d word=0x%x declared_fuse_ver=%d "
                      "(reading v-1 as a bit position expects 0x%x)"
                      % (ucode_id, word, fuse_ver, expect)))
    if ucode_id == 3 and fuse_ver == 1 and word == 0x1:
        r["verdict"] = "IDENTITY-COMPATIBLE"
        r["rows"].append(("fuse", "MATCH: this is the measured booter case "
                          "(COMPATIBLE, sig_index 1) -- and it still failed the "
                          "boot ROM after its signature verified (0x002B0009), so "
                          "identity compatibility is necessary, not sufficient"))
    elif expect and (word & expect):
        r["verdict"] = "IDENTITY-COMPATIBLE"
        r["rows"].append(("fuse", "MATCH under the same reading as the booter; "
                          "acceptance is a boot question, not a static one"))
    else:
        r["verdict"] = "FUSE-PRECEDENT-MISSING"
        r["rows"].append(("fuse", "NO PRECEDENT: no container with this declared "
                          "fuse version has ever been measured compatible on this "
                          "part; the boot ROM's own comparison is not established "
                          "for it. This is a candidate to TEST, not a drop."))
    # Booter-class loads (the GSP booter path) require the HS engine bit in the
    # descriptor, not in the container; record it rather than judging it.
    if engine_id & 0x400:
        r["rows"].append(("engine_id", "HS class bit set (0x400): the FWSEC "
                          "descriptor requires this; a SEC2 booter does not"))
    return r


def report(r):
    print("== %s (%d B) -> %s" % (r["file"], r["size"], r["verdict"]))
    for k, v in r["rows"]:
        print("   %-9s %s" % (k, v))


def selftest():
    roots = [os.environ.get("HS_CORPUS", "."),
             os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                          "..", "evidence")]
    bad = 0
    seen = 0
    for name, want in CORPUS.items():
        path = None
        for root in roots:
            cand = os.path.join(root, name)
            if os.path.exists(cand):
                path = cand
                break
            cand = os.path.join(root, "fw", name)
            if os.path.exists(cand):
                path = cand
                break
        if not path:
            print("  SKIP %-34s (not staged; pass HS_CORPUS=<dir>)" % name)
            continue
        r = parse(path, role=str(want.get("role", "container")))
        seen += 1
        got = r["verdict"]
        t = r.get("triple", {})
        ok = (got == want["verdict"] and t.get("ucode_id") == want["ucode_id"]
              and t.get("fuse_ver") == want["fuse_ver"]
              and t.get("engine_id") == want["engine_id"])
        print("  %s %-34s verdict=%-24s triple=(%s/%s/%s) want=(%s/%s/%s)"
              % ("PASS" if ok else "FAIL", name, got,
                 t.get("fuse_ver", "?"), hex(int(t.get("engine_id") or 0)),
                 t.get("ucode_id", "?"),
                 want["fuse_ver"], hex(want["engine_id"]), want["ucode_id"]))
        bad += 0 if ok else 1
    print("\nselftest: %d containers checked, %d mismatched" % (seen, bad))
    if seen < len(CORPUS):
        print("NOTE: %d of %d corpus files were not staged (set HS_CORPUS); "
              "an unrun check is not a pass" % (len(CORPUS) - seen, len(CORPUS)))
        bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    if not args:
        print(__doc__)
        sys.exit(2)
    out = {}
    for p in args:
        r = parse(p)
        report(r)
        out[r["file"]] = r
    print()
    print(json.dumps({k: {"verdict": v["verdict"], "triple": v.get("triple")}
                      for k, v in out.items()}, indent=2))
