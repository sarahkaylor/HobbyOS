#!/usr/bin/env python3
"""Structural map of an NVIDIA VBIOS image (written for the HobbyOS nv/ada lane).

Reads a raw 1 MiB flash dump (the PROM aperture image) and prints, in order:
  * the ROM signature and the image chain (PCIR / NPDS / NPDE walk),
  * every 'BIT' token table and all of its tokens,
  * the falcon ucode table that BIT token 0x70 points at, after applying the
    "imaged" address translation,
  * every ucode descriptor and the payload range each one covers.

The address translation is the point of the tool: pointers stored in the VBIOS
are written against a logical image in which the EFI ROM image (the one whose
PCIR image type is not 0xE0) does not exist.  A pointer P >= size(image 0) has
to be moved by  (base of the first 0xE0 image) - size(image 0).

Usage: analyse_vbios.py <vbios.bin>
"""
import struct
import sys

PCIR_SIGS = (0x52494350, 0x53494752, 0x5344504E)   # 'PCIR', 'RGIS', 'NPDS'
NPDE_SIG = 0x4544504E                              # 'NPDE'
IMAGE_TYPE_FWSEC = 0xE0
IMAGE_TYPE_PMU = 0x70
APP_FWSEC_PROD = 0x85


def main(path):
    rom = open(path, "rb").read()
    print("file      %s" % path)
    print("size      %d bytes (0x%X)" % (len(rom), len(rom)))

    def u8(o):
        return rom[o]

    def u16(o):
        return struct.unpack_from("<H", rom, o)[0]

    def u32(o):
        return struct.unpack_from("<I", rom, o)[0]

    def image_at(base):
        """Decode one ROM image header; returns None when no image starts there."""
        if u16(base) not in (0xAA55, 0xBB77, 0x4E56):
            return None
        pd = u16(base + 0x18)
        if not pd:
            return None
        p = base + pd
        if u32(p) not in PCIR_SIGS:
            return None
        info = {
            "base": base,
            "pcir": p,
            "sig": rom[p:p + 4].decode("latin-1"),
            "vendor": u16(p + 4),
            "device": u16(p + 6),
            "hdr": u16(p + 0x0A),
            "size": u16(p + 0x10) * 512,
            "type": u8(p + 0x14),
            "last": bool(u8(p + 0x15) & 0x80),
            "npde": None,
        }
        if info["type"] != IMAGE_TYPE_PMU:
            q = (p + info["hdr"] + 0x0F) & ~0x0F
            if u32(q) == NPDE_SIG:
                info["npde"] = q
                info["size"] = u16(q + 8) * 512
                info["last"] = bool(u8(q + 0x0A) & 0x80)
        return info

    print("\n== image chain ==")
    images = []
    base = size = 0
    while len(images) < 16:
        base += size
        im = image_at(base)
        if im is None:
            print("  stop: no image header at 0x%05X" % base)
            break
        images.append(im)
        size = im["size"]
        print("  [%d] base 0x%05X size %6d (0x%05X) type 0x%02X  pcir@0x%05X %s"
              " npde@%s vendor 0x%04X device 0x%04X last %s"
              % (len(images) - 1, im["base"], im["size"], im["size"], im["type"],
                 im["pcir"], im["sig"],
                 "0x%05X" % im["npde"] if im["npde"] else "-",
                 im["vendor"], im["device"], im["last"]))
        if im["last"]:
            break

    image0_size = images[0]["size"]
    fwsec_base = next((i["base"] for i in images if i["type"] == IMAGE_TYPE_FWSEC), 0)
    shift = fwsec_base - image0_size if fwsec_base else 0
    print("  image0_size 0x%05X   first type-0xE0 base 0x%05X   shift 0x%05X"
          % (image0_size, fwsec_base, shift))

    def phys(ptr):
        """Translate a VBIOS pointer into a flash offset (the imaged rule)."""
        return ptr + shift if ptr >= image0_size and shift else ptr

    print("\n== BIT token tables ==")
    o = 0
    while True:
        o = rom.find(b"\xff\xb8BIT\x00", o)
        if o < 0:
            break
        hdr = rom[o:o + 12]
        hsize, tsize, count = hdr[8], hdr[9], hdr[10]
        ok = (sum(hdr) & 0xFF) == 0
        print("  table at 0x%05X hdr=%d stride=%d entries=%d bcd=0x%04X checksum=%s"
              % (o, hsize, tsize, count, u16(o + 6), "ok" if ok else "BAD"))
        for i in range(count):
            t = o + hsize + i * tsize
            tid, tver = u8(t), u8(t + 1)
            dsz, dptr = u16(t + 2), u16(t + 4)
            print("    token 0x%02X ver %d size %3d ptr 0x%05X  %s"
                  % (tid, tver, dsz, dptr,
                     rom[phys(dptr):phys(dptr) + min(dsz, 8)].hex(" ")))
        o += 6

    bit = rom.find(b"\xff\xb8BIT\x00")
    token70 = None
    if bit >= 0:
        hsize, tsize, count = rom[bit + 8], rom[bit + 9], rom[bit + 10]
        for i in range(count):
            t = bit + hsize + i * tsize
            if u8(t) == 0x70:
                token70 = (u8(t + 1), u16(t + 2), u16(t + 4))
    if token70 is None:
        print("\nno token 0x70")
        return
    ver, dsz, dptr = token70
    strap = u32(dptr)
    print("\n== token 0x70 -> ucode table ==")
    print("  token ver %d size %d payload@0x%05X = 0x%05X (logical)"
          % (ver, dsz, dptr, strap))
    tbl = phys(strap)
    print("  table flash offset = 0x%05X + 0x%05X = 0x%05X" % (strap, shift, tbl))
    tv, th, te, tc = u8(tbl), u8(tbl + 1), u8(tbl + 2), u8(tbl + 3)
    print("  header %s  ver=%d hdrsize=%d entrysize=%d entries=%d"
          % (rom[tbl:tbl + 6].hex(" "), tv, th, te, tc))
    for i in range(tc):
        e = tbl + th + i * te
        app, tgt, ptr = u8(e), u8(e + 1), u32(e + 2)
        dp = phys(ptr) if ptr else 0
        note = ""
        if app == APP_FWSEC_PROD and dp:
            h = u32(dp)
            note = "Hdr=0x%08X ver=%d hdrsize=%d" % (h, (h >> 8) & 0xFF, (h >> 16) & 0xFFFF)
        print("    [%2d] @0x%05X app 0x%02X target 0x%02X ptr 0x%05X -> 0x%05X %s"
              % (i, e, app, tgt, ptr, dp, note))

    print("\n== ucode descriptors ==")
    for i in range(tc):
        e = tbl + th + i * te
        app, ptr = u8(e), u32(e + 2)
        if not ptr:
            continue
        d = phys(ptr)
        h = u32(d)
        if not (h & 1):
            continue
        dver, dhsz = (h >> 8) & 0xFF, (h >> 16) & 0xFFFF
        if dver != 3 or d + 44 > len(rom):
            print("    app 0x%02X desc 0x%05X: version %d not decoded here" % (app, d, dver))
            continue
        stored, pkc, ifc, imemp, imeml, imemv, dmemp, dmeml = struct.unpack_from("<8I", rom, d + 4)
        eng, uid, scnt = struct.unpack_from("<HBB", rom, d + 0x24)
        sver = u16(d + 0x28)
        print("    app 0x%02X desc@0x%05X Hdr=0x%08X ver=%d hdrsize=%d stored=%d"
              % (app, d, h, dver, dhsz, stored))
        print("      pkc_off=0x%X ifc=0x%X imem_phys=0x%X imem_load=%d imem_virt=0x%X"
              % (pkc, ifc, imemp, imeml, imemv))
        print("      dmem_phys=0x%X dmem_load=%d engine_mask=0x%04X ucode_id=%d"
              " sig_count=%d sig_versions=0x%X" % (dmemp, dmeml, eng, uid, scnt, sver))
        print("      payload 0x%05X..0x%05X (%d bytes, imem+dmem=%d, %s)"
              % (d + dhsz, d + dhsz + stored, stored, imeml + dmeml,
                 "consistent" if imeml + dmeml == stored else "INCONSISTENT"))
        print("      first 16 bytes %s" % rom[d + dhsz:d + dhsz + 16].hex(" "))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "vbios.bin")
