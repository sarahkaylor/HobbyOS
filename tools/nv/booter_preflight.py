#!/usr/bin/env python3
"""booter_preflight.py -- assert what a SEC2 booter boot is about to stage.

Why this exists
---------------
A boot is the expensive unit on this lane (~6.5 min of VM cycle), and the lane's
default booter is the first globbed `third_party/nvfw/ad102/gsp/booter_load-*.bin`
-- the 535 control, not necessarily the candidate under test.  A boot that read
a stale or wrong container, or that DMA-read past the end of a truncated one,
produces a register word that looks exactly like a candidate's negative.  This
tool is the gate in front of the launch: it prints the sha256/size/path of the
file that will be staged, compares it against a caller-supplied pin, and refuses
when the container's own load header does not account for the file's bytes.

What it checks (and what it deliberately does not)
--------------------------------------------------
* hash + size + path of the file on disk  -> the `booter staged:` row
* `nvfw_bin_hdr` + `nvfw_hs_header_v2` + `nvfw_hs_load_header_v2` are present
  and inside the file
* the load header's regions (os_code, each app's code, os_data, each app's data)
  tile the container's data region contiguously from offset 0, with at most
  0x100 bytes of dword-aligned zero padding (measured on `booter_unload`: its
  images end 0x100 B before `data_off + data_size` and those bytes are zero)
* the extents the guest derives (`nv_gsp.c`: IMEM = data_off + app[0].code_off,
  DMEM = data_off + os.data_off) meet exactly, so no DMA reads past EOF
* a bootloader-only container (`data_size == 0`) is bounded by its own
  `os_code` extent, NOT by the booter's `0x7700 + 0x6000` constant

It does NOT judge identity compatibility (fuse_ver/engine_id/ucode_id) -- that
is `tools/nv/no-rm/hs_gate.py`'s job -- and it does not claim a container will
run.  Identity compatibility is necessary, never sufficient.

Rows and exit codes
-------------------
  booter staged: <sha256> size=<n> path=<p>     always, before anything else
  booter pin: <hex> | none                      the expected sha in force
  booter header: ...                            the parsed header, verbatim
  booter geometry: ...                          the derived bound and verdict
  booter REFUSED: <what> -- <why>               a refusal, with both values

  0  accepted        2  absent/unreadable        3  geometry/format
  4  sha256 mismatch 5  bad usage (e.g. a pin that is not 64 hex digits)

Usage:
  booter_preflight.py [--expect-sha HEX] FILE
  booter_preflight.py --selftest
"""
import hashlib
import os
import struct
import sys
import tempfile

MAGIC = 0x10DE
CONTAINER_VER = 1
HDR_DWORDS = 6
HS_DWORDS = 9
LOAD_HDR_MIN = 0x14
MAX_PAD = 0x100          # measured: booter_unload ends 0x100 B before its data region
DESC_MAX = 0x1000        # nv_fw.h: NV_FW_CONTAINER_DESC_MAX
APP_MAX = 4              # nv_fw.h: NV_FW_APP_MAX (entries the driver records)

EXIT_OK = 0
EXIT_ABSENT = 2
EXIT_GEOMETRY = 3
EXIT_SHA = 4
EXIT_USAGE = 5


class Refusal(Exception):
    def __init__(self, code, what, why):
        Exception.__init__(self, why)
        self.code = code
        self.what = what
        self.why = why


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def hexs(v):
    return "0x%x" % v


def sha256_file(path):
    h = hashlib.sha256()
    n = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            n += len(chunk)
            h.update(chunk)
    return h.hexdigest(), n


def _region(name, off, size, data_size):
    if size == 0:
        return None
    if off + size > data_size:
        raise Refusal(EXIT_GEOMETRY,
                      "%s region 0x%x+0x%x" % (name, off, size),
                      "ends past the container's data region (data_size 0x%x)"
                      % data_size)
    return (name, off, size)


def parse(buf):
    """Parse a container and return the derived geometry, or raise Refusal."""
    n = len(buf)
    if n < HDR_DWORDS * 4:
        raise Refusal(EXIT_GEOMETRY, "bin_hdr", "file is %d B, shorter than the "
                      "6-dword nvfw_bin_hdr" % n)
    magic, ver, bin_size, hdr_off, data_off, data_size = struct.unpack_from(
        "<6I", buf, 0)
    if magic != MAGIC:
        raise Refusal(EXIT_GEOMETRY, "bin_hdr magic 0x%x" % magic,
                      "not 0x10de: this is a payload or a descriptor, not a "
                      "bootable container (docs/nv-no-rm-path.md has the "
                      "ls-hsbl signature-descriptor case)")
    if ver != CONTAINER_VER:
        raise Refusal(EXIT_GEOMETRY, "bin_hdr version %d" % ver,
                      "the driver accepts version %d only" % CONTAINER_VER)
    if hdr_off < HDR_DWORDS * 4:
        raise Refusal(EXIT_GEOMETRY, "header_offset 0x%x" % hdr_off,
                      "overlaps the bin_hdr (must be >= 0x%x)"
                      % (HDR_DWORDS * 4))
    if hdr_off + HS_DWORDS * 4 > n:
        raise Refusal(EXIT_GEOMETRY, "hs header 0x%x+0x%x"
                      % (hdr_off, HS_DWORDS * 4),
                      "outside the %d B file" % n)

    info: dict = {
        "bin_size": bin_size, "hdr_off": hdr_off,
        "data_off": data_off, "data_size": data_size,
    }

    (sig_off, sig_size, patch_loc, patch_sig, meta_off, meta_size,
     num_sig_off, loadhdr_off, loadhdr_size) = struct.unpack_from(
         "<%dI" % HS_DWORDS, buf, hdr_off)
    info.update(loadhdr_off=loadhdr_off, loadhdr_size=loadhdr_size,
                sig_off=sig_off, sig_size=sig_size, num_sig_off=num_sig_off)

    if data_size == 0:
        # ---- bootloader-only: no data region, so the file itself is the image
        desc_end = n
        if data_off != 0:
            raise Refusal(EXIT_GEOMETRY, "data_off 0x%x with data_size 0"
                          % data_off, "a container with no data region must "
                          "place its image at file offset 0")
    else:
        desc_end = data_off
        if hdr_off >= data_off:
            raise Refusal(EXIT_GEOMETRY, "header_offset 0x%x" % hdr_off,
                          "not below data_offset 0x%x" % data_off)
        if data_off + data_size != n:
            raise Refusal(EXIT_GEOMETRY,
                          "bound 0x%x = data_off 0x%x + data_size 0x%x"
                          % (data_off + data_size, data_off, data_size),
                          "does not match the file's %d B (0x%x): the header "
                          "does not account for this file" % (n, n))
        if bin_size < data_size or bin_size - data_size > DESC_MAX:
            raise Refusal(EXIT_GEOMETRY,
                          "bin_size 0x%x vs data_size 0x%x"
                          % (bin_size, data_size),
                          "the descriptor region must be 1..0x%x B (nv_fw.h's "
                          "NV_FW_CONTAINER_DESC_MAX)" % DESC_MAX)

    if loadhdr_off < hdr_off or loadhdr_off + loadhdr_size > desc_end:
        raise Refusal(EXIT_GEOMETRY,
                      "load_header 0x%x+0x%x" % (loadhdr_off, loadhdr_size),
                      "outside the descriptor region [0x%x, 0x%x)"
                      % (hdr_off, desc_end))
    if loadhdr_size < LOAD_HDR_MIN:
        raise Refusal(EXIT_GEOMETRY, "load_header size 0x%x" % loadhdr_size,
                      "shorter than the 5-dword v2 header (0x%x)"
                      % LOAD_HDR_MIN)

    (os_code_off, os_code_size, os_data_off, os_data_size,
     napps) = struct.unpack_from("<5I", buf, loadhdr_off)
    if napps > APP_MAX:
        raise Refusal(EXIT_GEOMETRY, "num_apps %d" % napps,
                      "more than the driver's %d" % APP_MAX)
    if napps > (loadhdr_size - LOAD_HDR_MIN) // 16:
        raise Refusal(EXIT_GEOMETRY, "num_apps %d" % napps,
                      "does not fit the 0x%x B load header" % loadhdr_size)
    apps = [struct.unpack_from("<4I", buf, loadhdr_off + LOAD_HDR_MIN + 16 * i)
            for i in range(napps)]
    info.update(os_code_off=os_code_off, os_code_size=os_code_size,
                os_data_off=os_data_off, os_data_size=os_data_size,
                napps=napps)

    if data_size == 0:
        # ---- the bootloader-only bound, taken from the container's own header
        if napps:
            raise Refusal(EXIT_GEOMETRY, "num_apps %d with no data region"
                          % napps, "an app image cannot live outside the data "
                          "region")
        if os_data_size:
            raise Refusal(EXIT_GEOMETRY,
                          "os_data 0x%x+0x%x with data_size 0"
                          % (os_data_off, os_data_size),
                          "declares a data image but no data region")
        bound = os_code_off + os_code_size
        if bound != n:
            raise Refusal(EXIT_GEOMETRY,
                          "bound 0x%x = os_code 0x%x+0x%x"
                          % (bound, os_code_off, os_code_size),
                          "does not match the file's %d B (0x%x)" % (n, n))
        info.update(kind="bootloader-only", bound=bound, imem_off=os_code_off,
                    imem_len=os_code_size, dmem_off=bound, dmem_len=0, pad=0)
        return info

    # ---- booter class: the load header must tile the data region
    if napps == 0:
        raise Refusal(EXIT_GEOMETRY, "num_apps 0 in a data-region container",
                      "the guest would fall back to the literal 0x100/0x7600 "
                      "IMEM block (nv_gsp.c), which this header does not place")

    regions = []
    for name, off, size in (("os_code", os_code_off, os_code_size),
                            ("os_data", os_data_off, os_data_size)):
        r = _region(name, off, size, data_size)
        if r:
            regions.append(r)
    for i, a in enumerate(apps):
        for name, off, size in (("app%d_code" % i, a[0], a[1]),
                                ("app%d_data" % i, a[2], a[3])):
            r = _region(name, off, size, data_size)
            if r:
                regions.append(r)
    regions.sort(key=lambda r: r[1])

    cur = 0
    for name, off, size in regions:
        if off != cur:
            raise Refusal(EXIT_GEOMETRY,
                          "%s at 0x%x" % (name, off),
                          "does not continue the tiling (previous region ends "
                          "at 0x%x): the load header's regions must cover the "
                          "data region from 0" % cur)
        cur = off + size
    total = cur
    pad = data_size - total
    if pad < 0:
        raise Refusal(EXIT_GEOMETRY, "regions cover 0x%x > data_size 0x%x"
                      % (total, data_size), "a region overlaps another")
    if pad > MAX_PAD or pad % 4:
        raise Refusal(EXIT_GEOMETRY, "padding 0x%x" % pad,
                      "the only measured tail padding is <= 0x%x B, "
                      "dword-aligned (booter_unload has 0x100)" % MAX_PAD)
    if pad:
        tail = buf[data_off + total:data_off + data_size]
        if any(tail):
            raise Refusal(EXIT_GEOMETRY, "padding 0x%x at 0x%x"
                          % (pad, data_off + total),
                          "is not zero (%d non-zero byte(s)): the load header "
                          "does not account for these bytes"
                          % sum(1 for x in tail if x))

    # ---- the extents the guest actually DMAs (nv_gsp.c, verbatim rule)
    imem_off = data_off + apps[0][0]
    imem_len = apps[0][1]
    dmem_off = data_off + os_data_off
    dmem_len = os_data_size
    if imem_off + imem_len != dmem_off:
        raise Refusal(EXIT_GEOMETRY,
                      "imem 0x%x+0x%x ends at 0x%x" % (imem_off, imem_len,
                                                        imem_off + imem_len),
                      "but dmem starts at 0x%x: the two DMA images must meet"
                      % dmem_off)
    if dmem_off + dmem_len != data_off + total:
        raise Refusal(EXIT_GEOMETRY,
                      "dmem 0x%x+0x%x ends at 0x%x"
                      % (dmem_off, dmem_len, dmem_off + dmem_len),
                      "but the load header's images end at 0x%x"
                      % (data_off + total))
    info.update(kind="booter-class", bound=data_off + data_size,
                imem_off=imem_off, imem_len=imem_len, dmem_off=dmem_off,
                dmem_len=dmem_len, pad=pad)
    return info


def report(path, expect):
    row = "booter staged: %s size=%d path=%s"
    try:
        digest, size = sha256_file(path)
    except FileNotFoundError:
        raise Refusal(EXIT_ABSENT, "booter %s" % path,
                      "does not exist (NV_BOOTER_FILE names the container to "
                      "stage; a typo must not boot the previous image)")
    except OSError as exc:
        raise Refusal(EXIT_ABSENT, "booter %s" % path, str(exc))
    print(row % (digest, size, path))

    pin = (expect or "").strip().lower()
    if pin.startswith("sha256:"):
        pin = pin[len("sha256:"):]
    if pin:
        if len(pin) != 64 or any(c not in "0123456789abcdef" for c in pin):
            raise Refusal(EXIT_USAGE, "NV_EXPECT_BOOTER_SHA256=%r" % expect,
                          "is not a 64-digit hex sha256; a malformed pin must "
                          "not read as a match")
        print("booter pin: %s (NV_EXPECT_BOOTER_SHA256)" % pin)
    else:
        print("booter pin: none -- the staged image is asserted by hash but "
              "not pinned to a candidate")

    with open(path, "rb") as f:
        buf = f.read()
    try:
        info = parse(buf)
    except Refusal as r:
        print("booter header: size=%d bin_size=%d" %
              (len(buf), u32(buf, 8)) if len(buf) >= 12 else
              "booter header: size=%d (too short)" % len(buf))
        raise r

    print("booter header: magic=0x%x v=%d bin_size=%d data_off=0x%x "
          "data_size=0x%x load_header=0x%x+0x%x os_code=0x%x+0x%x "
          "os_data=0x%x+0x%x napps=%d"
          % (MAGIC, CONTAINER_VER, info["bin_size"], info["data_off"],
             info["data_size"], info["loadhdr_off"], info["loadhdr_size"],
             info["os_code_off"], info["os_code_size"], info["os_data_off"],
             info["os_data_size"], info["napps"]))
    if info["kind"] == "bootloader-only":
        print("booter geometry: bootloader-only data_size=0 bound=0x%x "
              "from os_code_size 0x%x -- the 0x7700+0x6000 booter constant is "
              "not applied" % (info["bound"], info["os_code_size"]))
    else:
        print("booter geometry: booter-class data_off=0x%x data_size=0x%x "
              "imem=0x%x+0x%x dmem=0x%x+0x%x pad=0x%x bound=0x%x "
              "verdict=TILES-THE-FILE"
              % (info["data_off"], info["data_size"], info["imem_off"],
                 info["imem_len"], info["dmem_off"], info["dmem_len"],
                 info["pad"], info["bound"]))
    if pin and digest != pin:
        raise Refusal(EXIT_SHA,
                      "staged sha256 %s" % digest,
                      "!= NV_EXPECT_BOOTER_SHA256 %s -- refusing to launch on "
                      "the wrong image" % pin)
    return digest, size, info


def selftest():
    """Build containers in a temp dir and assert accept/refuse on each."""
    good = 0
    bad = 0

    def build(path, *, data_off=0x378, app_code=(0x100, 0x7600),
              os_data=(0x7700, 0x6000), pad=0, pad_byte=0, magic=MAGIC,
              ver=CONTAINER_VER, file_trim=0, no_loadhdr=False):
        os_code = (0, 0x100)
        napps = 1 if app_code else 0
        total = os_code[1] + (app_code[1] if app_code else 0) + os_data[1]
        data_size = total + pad
        hdr_off = 0x18
        hs = struct.pack("<9I", 0x3C, 0x300, 0x33C, 0x340, 0x344, 0xC, 0x350,
                         0x354 if not no_loadhdr else 0, 0x24)
        loadhdr = struct.pack("<5I", os_code[0], os_code[1], os_data[0],
                              os_data[1], napps)
        if app_code:
            loadhdr += struct.pack("<4I", app_code[0], app_code[1], 0x100, 0)
        blob = bytearray(data_off + data_size)
        struct.pack_into("<6I", blob, 0, magic, ver, data_size + 0x400,
                         hdr_off, data_off, data_size)
        blob[hdr_off:hdr_off + len(hs)] = hs
        blob[0x354:0x354 + len(loadhdr)] = loadhdr
        blob[data_off + total:data_off + data_size] = bytes([pad_byte]) * pad
        with open(path, "wb") as f:
            f.write(blob[:len(blob) - file_trim])
        return path

    def bootloader_only(path, os_code_size, file_size=None):
        hdr_off = 0x100
        hs = struct.pack("<9I", 0x11C, 0x300, 0x41C, 0x420, 0x424, 0xC, 0x430,
                         0x434, 0x14)
        loadhdr = struct.pack("<5I", 0, os_code_size, 0, 0, 0)
        size = file_size if file_size is not None else os_code_size
        blob = bytearray(size)
        struct.pack_into("<6I", blob, 0, MAGIC, CONTAINER_VER, size, hdr_off, 0, 0)
        blob[hdr_off:hdr_off + len(hs)] = hs
        blob[0x434:0x434 + len(loadhdr)] = loadhdr
        with open(path, "wb") as f:
            f.write(blob)
        return path

    def case(name, path, want_code, expect=None):
        nonlocal good, bad
        try:
            report(path, expect)
            got = EXIT_OK
        except Refusal as r:
            got = r.code
            print("  %-46s refused: %s -- %s" % (name, r.what, r.why))
        ok = got == want_code
        print("  %s %-46s exit=%d want=%d" % ("PASS" if ok else "FAIL", name,
                                              got, want_code))
        if ok:
            good += 1
        else:
            bad += 1

    with tempfile.TemporaryDirectory(prefix="booter-preflight-") as td:
        print("selftest: fixtures in %s" % td)
        booter = build(os.path.join(td, "booter.bin"))
        unload = build(os.path.join(td, "unload.bin"), os_data=(0x4900, 0x4E00),
                       app_code=(0x100, 0x4800), pad=0x100)
        trunc = build(os.path.join(td, "trunc.bin"), file_trim=0x100)
        dirty = build(os.path.join(td, "pad-nonzero.bin"),
                      os_data=(0x4900, 0x4E00), app_code=(0x100, 0x4800),
                      pad=0x100, pad_byte=0x5A)
        bogus = build(os.path.join(td, "payload.bin"), magic=0xDEADBEEF)
        noload = build(os.path.join(td, "no-loadhdr.bin"), no_loadhdr=True)
        only = bootloader_only(os.path.join(td, "bootloader-only.bin"), 0x500)
        only_bad = bootloader_only(os.path.join(td, "bl-bound.bin"), 0x500,
                                   file_size=0x540)

        with open(booter, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        case("booter-class, exact tiling", booter, EXIT_OK)
        case("booter-class, pinned to its own sha", booter, EXIT_OK, digest)
        case("booter-class, pinned to a wrong sha", booter, EXIT_SHA, "00" * 32)
        case("booter-class, malformed pin", booter, EXIT_USAGE, "deadbeef")
        case("booter_unload shape, 0x100 zero pad", unload, EXIT_OK)
        case("truncated file", trunc, EXIT_GEOMETRY)
        case("non-zero padding", dirty, EXIT_GEOMETRY)
        case("not a container", bogus, EXIT_GEOMETRY)
        case("no load header", noload, EXIT_GEOMETRY)
        case("bootloader-only, bound from os_code_size", only, EXIT_OK)
        case("bootloader-only, bound mismatch", only_bad, EXIT_GEOMETRY)
        case("absent file", os.path.join(td, "missing.bin"), EXIT_ABSENT)
    print("\nselftest: %d passed, %d failed" % (good, bad))
    return 1 if bad else 0


def main(argv):
    args = list(argv[1:])
    expect = None
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--selftest":
            return selftest()
        if a in ("--expect-sha", "--expect"):
            i += 1
            if i >= len(args):
                print(__doc__)
                return EXIT_USAGE
            expect = args[i]
        elif a.startswith("--expect-sha="):
            expect = a.split("=", 1)[1]
        elif a in ("-h", "--help"):
            print(__doc__)
            return EXIT_OK
        elif a.startswith("-"):
            print("unknown option %s\n" % a, file=sys.stderr)
            print(__doc__)
            return EXIT_USAGE
        else:
            files.append(a)
        i += 1
    if len(files) != 1:
        print(__doc__)
        return EXIT_USAGE
    try:
        report(files[0], expect)
    except Refusal as r:
        print("booter REFUSED: %s -- %s" % (r.what, r.why))
        return r.code
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main(sys.argv))
