#!/usr/bin/env python3
"""vbios_ucode.py -- read-only parse of the committed RTX 4090 VBIOS dump.

Answers, from the ROM bytes alone (no card, no writes):
  * where the PCI ROM image starts (IFR walk),
  * the BIT token list and the falcon-data token 0x70,
  * every FALCON_UCODE_TABLE_ENTRY_V1 and its descriptor, decoded:
    app id, target id, descriptor version/size, StoredSize, IMEM/DMEM load
    sizes, EngineIdMask, UcodeId, SignatureCount, SignatureVersions,
    PKC (signature) offset, and whether ENCRYPTED is set.

That is exactly the evidence a host-side bring-up path needs: which engines'
ucode this ROM carries at all, and which of NVIDIA's fused keys would have to
sign it. Nothing here writes to hardware.

Usage: vbios_ucode.py evidence/vbios.bin [--json out.json]
"""
import json
import struct
import sys

ROM_SIZE = 0x100000

APP_NAMES = {
    0x01: "PMU?/DEVINIT? (unclaimed, see nv-devinit-contract.md S9)",
    0x05: "FIRMWARE_SEC_LIC",
    0x10: "FECS? (not asserted)",
    0x20: "FECS (legacy id space)",
    0x21: "GPCCS (legacy id space)",
    0x45: "FWSEC_DBG",
    0x85: "FWSEC_PROD",
    0x86: "GSP? (not asserted)",
}


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def find_ifr_rom_offset(rom):
    """Return (rom_offset, how). Mirrors the IFR walk in nv-pmu-acr-contract.md S1.3."""
    if u16(rom, 0) == 0xAA55:
        return 0, "no IFR: PCI ROM image at 0 (0xAA55)"
    if u32(rom, 0) != 0x4947564E:  # 'NVGI'
        raise SystemExit("no IFR: ROM[0..3] = %08x, not 'NVGI' and not 0xAA55" % u32(rom, 0))
    fixed1 = u32(rom, 0x04)
    versionsw = (fixed1 >> 8) & 0xFF
    fixed_data_size = (fixed1 >> 16) & 0x7FFF
    fixed2 = u32(rom, 0x08)
    total_data_size = fixed2 & 0xFFFFF
    if versionsw in (1, 2):
        off = u32(rom, fixed_data_size + 4)
        how = "IFR VERSIONSW=%d: dword at FIXED_DATA_SIZE(0x%x)+4" % (versionsw, fixed_data_size)
    elif versionsw == 3:
        d = total_data_size
        if u32(rom, d + 4096) != 0x44524652:  # 'RFRD'
            raise SystemExit("IFR v3: no RFRD at 0x%x+0x1000" % d)
        off = u32(rom, d + 4096 + 8)
        how = "IFR VERSIONSW=3: RFRD at 0x%x, dword at +8" % (d + 4096)
    else:
        raise SystemExit("IFR VERSIONSW=%d not handled" % versionsw)
    if u16(rom, off) != 0xAA55:
        raise SystemExit("rom_offset 0x%x does not start with 0xAA55" % off)
    if off % 4 or off >= ROM_SIZE:
        raise SystemExit("rom_offset 0x%x not aligned/in range" % off)
    return off, how


def find_bit(rom, limit=0x100000):
    """Scan for the 6-byte key FF B8 'BIT\\0' and require header checksum 0."""
    key = b"\xff\xb8BIT\x00"
    out = []
    i = 0
    while True:
        i = rom.find(key, i)
        if i < 0 or i >= limit:
            break
        hdr_size = u8(rom, i + 0x08)
        if 6 <= hdr_size <= 0x40 and sum(rom[i:i + hdr_size]) % 256 == 0:
            out.append((i, hdr_size, u8(rom, i + 0x09), u8(rom, i + 0x0A),
                        u16(rom, i + 0x06)))
        i += 1
    return out


def parse_tokens(rom, bit, hdr_size, tok_size, tok_count):
    toks = []
    for n in range(tok_count):
        o = bit + hdr_size + n * tok_size
        toks.append({
            "index": n,
            "id": u8(rom, o),
            "version": u8(rom, o + 1),
            "size": u16(rom, o + 2),
            "ptr": u16(rom, o + 4),
        })
    return toks


def desc_ok(rom, d):
    if not (0 <= d < ROM_SIZE - 8):
        return False
    v = u32(rom, d)
    ver = (v >> 8) & 0xFF
    size = (v >> 16) & 0xFFFF
    return bool(v & 1) and 1 <= ver <= 4 and 0x20 <= size <= 0x7C


def decode_desc(rom, rom_off, desc_ptr):
    """Decode a FALCON_UCODE_DESC header + the V2 (60B) / V3 (44B) body.

    desc_ptr names the EFI-stripped logical image when it lies past 0xFC00, so
    resolve by which offset actually holds a valid vDesc."""
    d = rom_off + desc_ptr
    rule = "raw"
    if not desc_ok(rom, d) and desc_ptr > 0xFC00 and desc_ok(rom, d + 0x14E00):
        d += 0x14E00
        rule = "logical+0x14E00"
    vdesc = u32(rom, d)
    version = (vdesc >> 8) & 0xFF
    size = (vdesc >> 16) & 0xFFFF
    flags = vdesc & 0xFF
    rec = {
        "desc_ptr": desc_ptr,
        "desc_off": d,
        "desc_rule": rule,
        "vdesc": vdesc,
        "version_available": bool(flags & 1),
        "encrypted": bool(flags & 4),
        "version": version,
        "size": size,
        "stored_size": u32(rom, d + 0x04),
    }
    if version == 2 and size >= 60:
        rec.update({
            "uncompressed_size": u32(rom, d + 0x08),
            "virtual_entry": u32(rom, d + 0x0C),
            "interface_offset": u32(rom, d + 0x10),
            "imem_phys_base": u32(rom, d + 0x14),
            "imem_load_size": u32(rom, d + 0x18),
            "imem_virt_base": u32(rom, d + 0x1C),
            "imem_sec_base": u32(rom, d + 0x20),
            "imem_sec_size": u32(rom, d + 0x24),
            "dmem_offset": u32(rom, d + 0x28),
            "dmem_phys_base": u32(rom, d + 0x2C),
            "dmem_load_size": u32(rom, d + 0x30),
        })
    elif version == 3 and size >= 44:
        rec.update({
            "pkc_data_offset": u32(rom, d + 0x08),
            "interface_offset": u32(rom, d + 0x0C),
            "imem_phys_base": u32(rom, d + 0x10),
            "imem_load_size": u32(rom, d + 0x14),
            "imem_virt_base": u32(rom, d + 0x18),
            "dmem_phys_base": u32(rom, d + 0x1C),
            "dmem_load_size": u32(rom, d + 0x20),
            "engine_id_mask": u16(rom, d + 0x24),
            "ucode_id": u8(rom, d + 0x26),
            "signature_count": u8(rom, d + 0x27),
            "signature_versions": u16(rom, d + 0x28),
            # payload: PKC header (sig) starts at desc + pkc_data_offset
            "pkc_off": d + u32(rom, d + 0x08),
        })
    return rec


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "evidence/vbios.bin"
    rom = open(path, "rb").read()
    if len(rom) != ROM_SIZE:
        raise SystemExit("expected 1 MiB, got %d" % len(rom))
    rep = {"file": path, "size": len(rom)}
    rom_off, how = find_ifr_rom_offset(rom)
    rep["rom_offset"] = rom_off
    rep["rom_offset_how"] = how
    bits = find_bit(rom)
    rep["bit_candidates"] = [{"off": b[0], "hdr_size": b[1], "tok_size": b[2],
                              "tok_count": b[3], "bcd": b[4]} for b in bits]
    if len(bits) != 1:
        print(json.dumps(rep, indent=2))
        raise SystemExit("expected exactly one BIT candidate, got %d" % len(bits))
    bit, hdr_size, tok_size, tok_count, bcd = bits[0]
    toks = parse_tokens(rom, bit, hdr_size, tok_size, tok_count)
    rep["bcd_version"] = "0x%04x" % bcd
    rep["tokens"] = toks
    rep["tokens_str"] = " ".join("%02x" % t["id"] for t in toks)
    fp = [t for t in toks if t["id"] == 0x70]
    rep["falcon_token"] = fp[0] if fp else None
    if fp:
        assert fp[0]["version"] == 2 and fp[0]["size"] >= 4, "token 0x70 shape"
        tbl_ptr = u32(rom, fp[0]["ptr"])
        tbl = rom_off + tbl_ptr

        def hdr_ok(off):
            if not (0 <= off < ROM_SIZE - 8):
                return False
            v, hs, es, ec = rom[off], rom[off + 1], rom[off + 2], rom[off + 3]
            return v == 1 and 6 <= hs <= 0x40 and 6 <= es <= 0x40 and 1 <= ec <= 0x40

        # docs/nv-vbios-map.md: pointers above the 0xFC00 boundary name the
        # EFI-stripped logical image, so the flash offset is ptr + 0x14E00.
        # Resolve by which one actually holds a table header, and record it.
        table_rule = "raw"
        if not hdr_ok(tbl):
            if tbl_ptr > 0xFC00 and hdr_ok(tbl_ptr + 0x14E00):
                tbl = tbl_ptr + 0x14E00
                table_rule = "logical+0x14E00 (EFI ROM stripped)"
            else:
                raise SystemExit("falcon table ptr 0x%06x does not resolve" % tbl_ptr)
        rep["falcon_table"] = {
            "ptr": tbl_ptr, "off": tbl, "rule": table_rule, "version": u8(rom, tbl),
            "hdr_size": u8(rom, tbl + 1), "entry_size": u8(rom, tbl + 2),
            "entry_count": u8(rom, tbl + 3), "desc_version": u8(rom, tbl + 4),
            "desc_size": u8(rom, tbl + 5),
        }
        entries = []
        esz = rep["falcon_table"]["entry_size"] or 6
        for n in range(rep["falcon_table"]["entry_count"]):
            o = tbl + rep["falcon_table"]["hdr_size"] + n * esz
            app = u8(rom, o)
            tgt = u8(rom, o + 1)
            dptr = u32(rom, o + 2)
            e = {"n": n, "app_id": app, "app_name": APP_NAMES.get(app, "-"),
                 "target_id": tgt, "desc_ptr": dptr}
            try:
                e.update(decode_desc(rom, rom_off, dptr))
            except Exception as ex:  # noqa: BLE001 - report, never guess
                e["desc_error"] = str(ex)
            entries.append(e)
        rep["entries"] = entries
    rep["version_string_token"] = next((t for t in toks if t["id"] == 0x42), None)
    if rep.get("version_string_token"):
        t = rep["version_string_token"]
        rep["vbios_version_dword"] = "0x%08x" % u32(rom, t["ptr"])
    print(json.dumps(rep, indent=2))
    if len(sys.argv) > 3 and sys.argv[2] == "--json":
        json.dump(rep, open(sys.argv[3], "w"), indent=2)


if __name__ == "__main__":
    main()
