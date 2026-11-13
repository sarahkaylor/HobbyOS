#!/usr/bin/env python3
"""Wrap a FAT filesystem image in a GPT disk image with one EFI System Partition.

    mkgpt.py <fat.img> <out.img> [size_mib]

Parallels' efi-arm64 firmware wants a partitioned disk (a bare "superfloppy" FAT
image is not guaranteed to be offered as a boot device), so the probe's ESP is
wrapped in a protective MBR + GPT, primary and backup, the way a real installer
would lay it out.  The FAT image starts at 1 MiB.
"""
import struct
import sys
import zlib

ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")  # C12A7328-F81F-11D2-BA4B-00A0C93EC93B
SECTOR = 512
START_LBA = 2048  # 1 MiB


def guid_le(s: str) -> bytes:
    """'C12A7328-F81F-11D2-BA4B-00A0C93EC93B' -> 16 raw bytes in EFI order."""
    a, b, c, d, e = s.split("-")
    return (struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16))
            + bytes.fromhex(d) + bytes.fromhex(e))


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    fat_path, out_path = sys.argv[1], sys.argv[2]
    size_mib = int(sys.argv[3]) if len(sys.argv) > 3 else 128

    with open(fat_path, "rb") as f:
        fat = f.read()

    total = size_mib * 1024 * 1024
    if START_LBA * SECTOR + len(fat) > total:
        print(f"ESP does not fit: need {(START_LBA * SECTOR + len(fat)) / 1048576:.1f} MiB")
        return 1

    total_sectors = total // SECTOR
    num_entries = 128
    entry_size = 128
    entries_sectors = num_entries * entry_size // SECTOR
    first_usable = 2 + entries_sectors
    last_usable = total_sectors - 2 - entries_sectors

    esp_first = START_LBA
    esp_last = esp_first + (len(fat) + SECTOR - 1) // SECTOR - 1

    # --- partition entry ---
    entry = bytearray(entry_size)
    entry[0:16] = ESP_TYPE
    entry[16:32] = guid_le("5A1B2C3D-4E5F-4061-8273-849506172839")
    entry[32:40] = struct.pack("<Q", esp_first)
    entry[40:48] = struct.pack("<Q", esp_last)
    entry[48:56] = struct.pack("<Q", 0)  # attributes
    name = "EFI System Partition".encode("utf-16-le")
    entry[56:56 + len(name)] = name
    entries = bytes(entry) + bytes(entry_size * (num_entries - 1))
    entries_crc = crc32(entries)

    disk_guid = guid_le("3F2504E0-4F89-41D3-9A0C-0305E82C3301")

    def gpt_header(current_lba: int, alt_lba: int, entries_lba: int) -> bytes:
        hdr = bytearray(SECTOR)
        hdr[0:8] = b"EFI PART"
        struct.pack_into("<I", hdr, 8, 0x00010000)      # revision 1.0
        struct.pack_into("<I", hdr, 12, 92)             # header size
        struct.pack_into("<I", hdr, 16, 0)              # crc32 placeholder
        struct.pack_into("<I", hdr, 20, 0)              # reserved
        struct.pack_into("<Q", hdr, 24, current_lba)
        struct.pack_into("<Q", hdr, 32, alt_lba)
        struct.pack_into("<Q", hdr, 40, first_usable)
        struct.pack_into("<Q", hdr, 48, last_usable)
        hdr[56:72] = disk_guid
        struct.pack_into("<Q", hdr, 72, entries_lba)
        struct.pack_into("<I", hdr, 80, num_entries)
        struct.pack_into("<I", hdr, 84, entry_size)
        struct.pack_into("<I", hdr, 88, entries_crc)
        struct.pack_into("<I", hdr, 16, crc32(bytes(hdr[0:92])))
        return bytes(hdr)

    img = bytearray(total)

    # --- protective MBR ---
    mbr = bytearray(SECTOR)
    struct.pack_into("<B", mbr, 446, 0x00)         # not bootable
    struct.pack_into("<B", mbr, 447, 0x00)         # start CHS
    struct.pack_into("<B", mbr, 448, 0x02)
    struct.pack_into("<B", mbr, 449, 0x00)
    struct.pack_into("<B", mbr, 450, 0xEE)         # GPT protective type
    struct.pack_into("<B", mbr, 451, 0xFF)         # end CHS
    struct.pack_into("<B", mbr, 452, 0xFF)
    struct.pack_into("<B", mbr, 453, 0xFF)
    struct.pack_into("<I", mbr, 454, 1)            # start LBA
    struct.pack_into("<I", mbr, 458, min(total_sectors - 1, 0xFFFFFFFF))
    struct.pack_into("<H", mbr, 510, 0xAA55)
    img[0:SECTOR] = mbr

    # --- primary GPT ---
    img[1 * SECTOR:2 * SECTOR] = gpt_header(1, total_sectors - 1, 2)
    img[2 * SECTOR:(2 + entries_sectors) * SECTOR] = entries

    # --- backup GPT ---
    backup_entries_lba = total_sectors - 1 - entries_sectors
    img[backup_entries_lba * SECTOR:(backup_entries_lba + entries_sectors) * SECTOR] = entries
    img[(total_sectors - 1) * SECTOR:total_sectors * SECTOR] = gpt_header(
        total_sectors - 1, 1, backup_entries_lba)

    # --- ESP payload ---
    img[START_LBA * SECTOR:START_LBA * SECTOR + len(fat)] = fat

    with open(out_path, "wb") as f:
        f.write(img)
    print(f"{out_path}: {size_mib} MiB GPT, ESP lba {esp_first}..{esp_last} "
          f"({len(fat) / 1048576:.1f} MiB FAT32)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
