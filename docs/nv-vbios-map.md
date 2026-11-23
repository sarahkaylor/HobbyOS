# The VBIOS map on AD102 (RTX 4090, `10de:2684`, PCI 01:00.0) — where the falcon ucode table and the FWSEC descriptor actually are

Scope: this settles the last hop of `docs/nv-pmu-acr-contract.md` §1.4–1.5 — the meaning of the
`0x80DE8` value in BIT token `0x70` — from the card's own bytes, and gives a lookup the driver can
implement as six reads.

The one-line answer:

> **VBIOS-internal pointers are relative to a *logical* image that has the EFI ROM image removed.
> The EFI image occupies flash `0x0FC00–0x24A00` (85504 bytes), so any pointer `>= 0x0FC00` must be
> moved by `+0x14E00` to become a flash offset. `0x80DE8` is therefore not a flash offset; the ucode
> table is at flash `0x95BE8`.**

| tag | meaning |
|---|---|
| **MEASURED** | read from this card's flash in a throwaway Linux guest on Proxmox `r1` (VM 312, `hostpci0: 0000:01:00,pcie=1`), 2026-11-23. Byte dump and its sha256 are in `docs/nv-vbios-evidence/` |
| **SOURCE** | behaviour of a named public function (nouveau `nvbios_addr()` / `nvbios_image()` / `nvbios_pmuTe()`), read for understanding only — no code was copied into this tree |
| **INFERRED** | my reading, consistent with the bytes but not proved by an independent observer |

---

## 0. Summary of the result

| item | value | tag |
|---|---|---|
| flash size / content end | 1 MiB window; last non-`0xFF` byte at `0x95DFF` | MEASURED |
| PROM aperture read | `BAR0 + 0x300000`, 1 MiB, 1:1 with flash, shadow bit (PCI cfg `0x50` bit 0) already `0` in the guest | MEASURED |
| PCI Expansion ROM BAR | `lspci` says 512 KiB; only **150016 bytes** (`0x24A00`) are actually readable — exactly the two PCI ROM images, nothing above them | MEASURED |
| BIT table | one only, at `0x1B0` (signature `FF B8 42 49 54 00` at `0x1B0`), 12-byte header, stride 6, 19 entries, header byte-sum 0 | MEASURED |
| token `0x70` | ver 2, size 4, payload at `0x41F` = `E8 0D 08 00` → `0x00080DE8` (logical) | MEASURED |
| **falcon ucode table** | flash `0x95BE8`, header `01 06 06 10 01 30`, 16 entries of 6 bytes from `0x95BEE` | MEASURED |
| FWSEC_PROD entry | entry `[9]` at `0x95C24` = `85 07 3C F0 02 00` → desc ptr `0x0002F03C` (logical) | MEASURED |
| **FWSEC descriptor** | flash `0x43E3C`, `Hdr = 0x032C0301` → V3, header size **812 bytes**, `StoredSize` 65408, payload `0x44168–0x540E8` | MEASURED |
| `0x80DE8` read raw | `E7 8F 84 00 E7 8F 84 00 …` — a run of 4-byte records inside the last image, 336 bytes long (`0x80D18–0x80E68`); as a table header it would be ver `0xE7`/hdr `0x8F`/entry `0x84`/count `0`, i.e. nonsense | MEASURED |

---

## 1. How these bytes were obtained (and what did not work)

Two routes, both read-only, from an Ubuntu 24.04 guest with the 4090 passed through and **no NVIDIA
driver loaded** (so nothing else had touched the ROM):

| route | what happened |
|---|---|
| **A. PCI Expansion ROM BAR** (`echo 1 > /sys/.../rom; dd`) | the attribute is 524288 bytes, but the read returns **150016 bytes then EOF** (`0x24A00`). That is *not* a truncation artefact: `0x24A00` is exactly the end of the second PCI ROM image (§2), so the BAR only exposes the two legacy/EFI PCI images. **Route A can never see the ucode table.** |
| **B. PROM aperture** (`BAR0 + 0x300000`, 1 MiB) | 1048576 bytes, sha256 `7dfb34…b863`, and the route-A bytes are a byte-identical prefix. This is the image everything below refers to. |

`config[0x50]` read `0x00` in the guest *before* any write (bit 0 already clear), was written back as
`0x00`, and restored to `0x00`. So on this guest the shadow toggle was a no-op; the dump is therefore
the flash itself, corroborated by route A agreeing byte for byte. On the HobbyOS path the same bit is
read and cleared by the driver, which is equally fine.

Offsets in this document are **flash offsets** = file offsets in
`docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.gz` = offsets from `BAR0 + 0x300000`.

---

## 2. The flash is a chain of four images

Walking the chain the way the image headers describe themselves (u16 signature at *base*, PCIR/`NPDS`
pointer at `base+0x18`, `image_size` in 512-byte units at `pcir+0x10`, type at `pcir+0x14`, "last" bit
at `pcir+0x15` bit 7, then an optional `NPDE` which can override size/last):

| # | base | size | type | header | what it is |
|---|---|---|---|---|---|
| 0 | `0x00000` | `0x0FC00` (64512) | `0x00` | `PCIR` @ `0x170`, `NPDE` @ `0x190` | legacy x86 PCI ROM image; contains the BIT table |
| 1 | `0x0FC00` | `0x14E00` (85504) | `0x03` | `PCIR` @ `0x0FC1C`, `NPDE` @ `0x0FC40` | EFI PCI ROM image |
| 2 | `0x24A00` | `0x06000` (24576) | **`0xE0`** | `NPDS` @ `0x24B60`, `NPDE` @ `0x24B80` | first FwSec-typed image (no BIT table, no ucode table, no app `0x85`) |
| 3 | `0x2AA00` | `0x6B400` (439296) | **`0xE0`**, last | `NPDS` @ `0x2AA20`, `NPDE` @ `0x2AA40` | second FwSec-typed image: all four ucode descriptors + payloads + the ucode table |
| — | `0x95E00` | — | — | — | end of content; `0xFF` to `0xFFFFF` |

Note: images 2 and 3 begin with the two bytes `56 4E` (u16 `0x4E56`, "NV"), which the PCIR walker
accepts as a valid image signature; their "PCI data structure" is a `NPDS` record whose field layout is
the PCIR layout (`image_size` blocks at `+0x10`, type at `+0x14`, last at `+0x15`), followed by an
`NPDE` at the next 16-byte boundary. `NPDS`/`NPDE` for image 3:
`NPDS @0x2AA20`: vendor `0x10DE`, device `0x2680`, hdr len `0x18`, image_size `0x035A` blocks (=439296),
type `0xE0`, last bit set; `NPDE @0x2AA40` repeats size `0x035A` blocks.

**The walk only reaches images 2 and 3 if you honour the NPDE override on image 1.** Image 1's PCIR has
`0x15 = 0x80` ("last image"), but its `NPDE @0x0FC40` has `0x0A = 0x00`, which clears that flag. A walker
that stops at image 1 (or that ignores NPDE) will never see the `0xE0` images, and therefore can neither
find the ucode table nor compute `shift`.

ASCII map (each cell 16 KiB, `#` = inside the content area):

```
0x00000 image0 (64K, x86)  ...... 0x0FC00 image1 (83.4K, EFI) ...... 0x24A00 image2 (24K, E0) .. 0x2AA00 image3 (429K, E0) ............ 0x95E00  end
       |____ BIT 0x1B0, tokens 0x22E..0x2A0 ____|                                            |__ descs 0x33B90/0x43E3C/0x63E84/0x6A5D8, table 0x95BE8 __|
```

---

## 3. The BIT table at `0x1B0` (the driver already parses this correctly)

Header `FF B8 42 49 54 00 00 01 0C 06 13 44`: id `0xB8FF`, signature `"BIT\0"`, BCD `0x0100`,
header size 12, token stride 6, 19 entries, checksum byte `0x44` → **sum of the 12 header bytes = 0**.
Tokens start at `0x1BC`:

| id | ver | size | ptr | payload (first bytes) | note |
|---|---|---|---|---|---|
| `0x32` | 1 | 4 | `0x23E` | `00 00 00 00` | |
| `0x42` | 2 | 37 | `0x24A` | `40 18 02 95 4C 00 00 00` | VBIOS version string (`95.02.18.40`-ish) |
| `0x43` | 2 | 44 | `0x26F` | `E0 50 00 00 …` | |
| `0x44` | 1 | 4 | `0x29B` | `FE 51 00 00` | |
| `0x49` | 1 | 36 | `0x29F` | `9E 40 00 00 …` | |
| `0x4D` | 2 | 41 | `0x2C3` | `0E 0E 41 FC …` | |
| `0x4E` | 0 | 0 | `0x000` | — | empty |
| `0x50` | 2 | 252 | `0x2EC` | `B9 26 07 00 …` | mode tables |
| `0x53` | 2 | 24 | `0x3E8` | `68 00 50 B9 …` | |
| `0x54` | 1 | 2 | `0x400` | `B2 52` | |
| `0x55` | 1 | 5 | `0x40A` | `BF 50 00 00 00` | |
| `0x56` | 1 | 6 | `0x40F` | `2B 47 95 45 00 00` | |
| `0x78` | 1 | 8 | `0x415` | `00 00 01 01 00 00 00 00` | |
| `0x64` | 1 | 2 | `0x41D` | `50 77` | |
| **`0x70`** | **2** | **4** | **`0x41F`** | **`E8 0D 08 00`** | falcon data → `0x00080DE8` |
| `0x75` | 1 | 17 | `0x423` | `00 …` | |
| `0x69` | 2 | 110 | `0x434` | `40 18 02 95 …` | |
| `0x45` | 1 | 4 | `0x402` | `00 00 00 00` | |
| `0x73` | 1 | 4 | `0x406` | `00 00 00 00` | |

All token payload pointers are `< 0x0FC00`, so no address translation applies to any of them — the
tokens locate their payloads *inside image 0*.

**Careful:** token `0x70`'s payload field is at flash `0x41F`, which is **not dword aligned**. An
aligned read of `0x41C` gives `0xE8775000`; the value only appears when the four bytes `0x41F..0x422`
are assembled (`0x420` = `0x0000080D` supplies the top three bytes). The driver's `vb_dword()` already
does this correctly.

---

## 4. The address-space rule — why `0x80DE8` is not an offset

The flash has an 85504-byte EFI image wedged between image 0 and the two `0xE0` images. The VBIOS's
internal pointers were **not** rebased when that image was inserted; they address a *logical* image
made of image 0 followed directly by image 2 and image 3:

```
logical : [ image0 0x00000..0x0FC00 ][ image2 0x0FC00..0x15C00 ][ image3 0x15C00..0x81000 ]
flash   : [ image0 0x00000..0x0FC00 ][ image1 0x0FC00..0x24A00 ][ image2 0x24A00..0x2AA00 ][ image3 0x2AA00..0x95E00 ]
                                                  ^^^ 0x14E00 bytes of EFI image, absent from the logical space
```

Translation (SOURCE: this is exactly what nouveau's `nvbios_addr()` does for *every* ROM read; the two
constants are computed by walking the image chain, §2):

```
shift = base(first image with type 0xE0) - size(image 0) = 0x24A00 - 0x0FC00 = 0x14E00
flash_offset(p) = p                    if p <  0x0FC00     (i.e. inside image 0, no change)
                = p + 0x14E00           if p >= 0x0FC00
```

Worked arithmetic with the measured bytes:

| hop | logical | + shift | flash | bytes there |
|---|---|---|---|---|
| token `0x70` payload | `0x80DE8` | `+0x14E00` | **`0x95BE8`** | `01 06 06 10 01 30` — ucode table header |
| app `0x85` desc pointer | `0x2F03C` | `+0x14E00` | **`0x43E3C`** | `01 03 2C 03 …` — V3 FWSEC descriptor |
| app `0x45` desc pointer | `0x1ED90` | `+0x14E00` | `0x33B90` | V3 descriptor (FWSEC debug build) |
| app `0x49` desc pointer | `0x4F084` | `+0x14E00` | `0x63E84` | V3 descriptor (22.5 KiB of IMEM) |
| app `0x89` desc pointer | `0x557D8` | `+0x14E00` | `0x6A5D8` | V3 descriptor |
| table end | `0x81000` | `+0x14E00` | `0x95E00` | end of content (all four pointers land in-range) |

Sanity check that the shift is right and not a coincidence: **all four** descriptor pointers resolve to
a dword with `bit0 = 1`, `version = 3`, `size = 812`, and their `IMEMLoadSize + DMEMLoadSize` equals
their `StoredSize` exactly. With the shift omitted, all four dwords are noise (`0xB0B3020A`,
`0x5B101983`, `0xB87381E6`, `0x209C4920`).

### 4.1 What is at `0x80DE8` when you skip the shift

`0x80DE8` is inside image 3 (image 3 spans `0x2AA00–0x95E00`; `0x80DE8` is `0x563E8` into it), between the
end of the last ucode payload (`0x70D2C`) and the ucode table (`0x95BE8`). It sits in a run of 4-byte
records `E7 8F xx 00` spanning `0x80D18–0x80E68`:

```
080de0   e7 8f 84 00  e7 8f 84 00  e7 8f 84 00  e7 8f 84 00
080df0   e7 8f 84 00  e7 8f 84 00  e7 8f 60 00  e7 8f 60 00
```

Read as a ucode table header that is `ver=0xE7, hdrsize=0x8F, entrysize=0x84, entries=0x00` — and with
`entries = 0` every downstream check fails. This region is ordinary image content (it is 0x14E00 below
the table in the logical space, i.e. the same object seen through the wrong base), not a table and not
a pointer indirection.

### 4.2 Why the driver's structural scan found nothing

My own scan, using the documented filter ("ver=1, hdr/entry 6 or 8, **non-zero app ids**, desc ptrs in
`[0x400, size)`" and requiring ≥80 % of entries to pass), also rejects the real table: of its 16 entries
only **7** have a non-zero app id (`0x01 0x07 0x08 0x45 0x85 0x49 0x89`); the other 9 are zero-filled
padding. So the table is invisible to that filter *and* the pointer never pointed at it. Both had to be
fixed; either one alone leaves the scan empty.

---

## 5. The falcon ucode table at `0x95BE8`

```
95be8  01 06 06 10 01 30      <- Version=1, HeaderSize=6, EntrySize=6, EntryCount=16, DescVersion=1, (byte 5 = 0x30)
95bee  01 01 54 5c 01 00      [0]  app 0x01 target 0x01 -> 0x15C54 (logical) -> 0x2AA54  (not a ucode descriptor)
95bf4  00 00 00 00 00 00      [1..4, 7, 12..15] zero-filled
...
95c0c  07 06 e8 f2 03 00      [5]  app 0x07 target 0x06 -> 0x3F2E8 -> 0x540E8  (= end of the 0x85 payload)
95c12  08 01 2c bf 05 00      [6]  app 0x08 target 0x01 -> 0x5BF2C -> 0x70D2C  (= end of the 0x49 payload)
95c1e  45 07 90 ed 01 00      [8]  app 0x45 target 0x07 -> 0x1ED90 -> 0x33B90  V3 desc, FWSEC debug
95c24  85 07 3c f0 02 00      [9]  app 0x85 target 0x07 -> 0x2F03C -> 0x43E3C  V3 desc, FWSEC prod  <-- the one we need
95c2a  49 05 84 f0 04 00      [10] app 0x49 target 0x05 -> 0x4F084 -> 0x63E84  V3 desc
95c30  89 05 d8 57 05 00      [11] app 0x89 target 0x05 -> 0x557D8 -> 0x6A5D8  V3 desc
95c36  00 ...                 [12..15] zero-filled; table ends at 0x95C4E
```

Facts a driver may rely on:

* The table has **no checksum**: the six header bytes sum to `0x4E`, not 0. Validate by shape
  (`ver==1, hdr>=6, entry>=6, count>=1`, entry app id `0x85`, descriptor `Hdr` sane), never by checksum.
* Entries are 6 bytes, not 8. `EntrySize` in the header is authoritative (`6` here).
* `EntryCount` counts *slots*, not descriptors: 9 of 16 slots are zero. Scan all slots, skip `app==0`,
  and take the **first** `app==0x85`. There is exactly one in the whole 1 MiB (verified by brute force
  over all 6-byte-aligned `0x85` bytes with descriptor-pointer validation).
* Apps `0x45` (FWSEC debug), `0x49`, `0x89` are present as well; `0x45` must not be picked.
* Both `0x45` and `0x85` carry `UcodeId = 9` and `SignatureVersions = 0x3`; `0x49`/`0x89` carry
  `UcodeId = 10`, `EngineIdMask = 0x0001` and are 22.5 KiB in IMEM — the SEC/ACR side, not GSP FWSEC.
* The descriptor pointers are in the *same* logical space, so the shift applies to them too (§4).

---

## 6. The FWSEC descriptor (app `0x85`) at flash `0x43E3C`

44 bytes of header, `Hdr.size = 812` (the header is 812 bytes long because it carries the signature
block; the *struct* is 44 bytes, the *payload offset* is 812):

```
43e3c  01 03 2c 03   Hdr = 0x032C0301 -> VERSION_AVAILABLE=1, version=3, size=812
43e40  80 ff 00 00   StoredSize       = 65408
43e44  24 0b 00 00   PKCDataOffset    = 0x0B24   (relative to DMEM base image)
43e48  1c 00 00 00   InterfaceOffset  = 0x1C     (appif table inside the payload)
43e4c  00 00 00 00   IMEMPhysBase     = 0
43e50  00 f2 00 00   IMEMLoadSize     = 61952 (0xF200)
43e54  00 00 00 00   IMEMVirtBase     = 0
43e58  00 00 00 00   DMEMPhysBase     = 0
43e5c  80 0d 00 00   DMEMLoadSize     = 3456  (0xD80)
43e60  00 04 09 02   EngineIdMask     = 0x0400  UcodeId = 9  SignatureCount = 2
43e64  03 00 49 80   SignatureVersions= 0x0003  Reserved = 0x8049
```

| check | expected | why it matters |
|---|---|---|
| `Hdr & 1` | 1 | "version available"; a driver that skips this will happily parse garbage |
| `(Hdr >> 8) & 0xFF` | 3 (V3) | selects the 44-byte field layout |
| `(Hdr >> 16) & 0xFFFF` | 812 | **payload starts at desc + 812**, not desc + 44 |
| `IMEMLoadSize + DMEMLoadSize` | `== StoredSize` (61952+3456 = 65408) | cheap, decisive sanity check |
| `EngineIdMask & 0x0400` | non-zero | HS falcon engine class; signature will not validate otherwise |
| `UcodeId`, `SignatureVersions` | 9, `0x0003` | must be checked against the fused key version (`0x8241C0 + (UcodeId-1)*4`) |

**Payload** (`desc + 812`): `0x44168 .. 0x540E8`, 65408 bytes, entropy 7.95 bits/byte, ending in zeros.
It is not plaintext falcon code — it is the signed/encrypted blob the bootloader (ACR `bl` + FWSEC)
decrypts with the fused key. What a driver must hand over is `desc` and `StoredSize`; it must not
"fix up" the payload. (MEASURED entropy; INFERRED that it is the bootloader that decrypts, matching the
V3 layout's `PKCDataOffset`.)

Neighbouring descriptors, for contrast (all `Hdr = 0x032C0301`, all internally consistent):

| app | desc | StoredSize | IMEMLoad | DMEMLoad | EngineIdMask | UcodeId | payload |
|---|---|---|---|---|---|---|---|
| `0x45` FWSEC_DBG | `0x33B90` | 65408 | 61952 | 3456 | `0x0400` | 9 | `0x33EBC–0x43E3C` |
| **`0x85` FWSEC_PROD** | **`0x43E3C`** | **65408** | **61952** | **3456** | **`0x0400`** | **9** | **`0x44168–0x540E8`** |
| `0x49` | `0x63E84` | 25640 | 22528 | 3112 | `0x0001` | 10 | `0x641B0–0x6A5D8` |
| `0x89` | `0x6A5D8` | 25640 | 22528 | 3112 | `0x0001` | 10 | `0x6A904–0x70D2C` |

Note they are laid out back to back: `0x45`'s payload ends exactly where `0x85`'s descriptor begins
(`0x33EBC + 65408 = 0x43E3C`), and `0x85`'s descriptor + payload (`0x43E3C + 812 + 65408 = 0x540E8`)
ends on a 16-byte boundary followed by a small table of repeated `0x166DC` dwords and code; the next
descriptor (`0x49`) starts at `0x63E84`, i.e. the four descriptors are packed into the first half of
image 3 and the tail (`0x70D2C–0x95E00`) is the trailing data plus the ucode table.

---

## 7. Byte-level recipe for the driver

All offsets are below `0x100000`, so every one of them is reachable with the existing
`nv_rd32(d, 0x300000 + off)` path. Six steps, with the values this card returns:

```
 1. p = vb_dword(0x41F)                      // 0x00080DE8   (bytes E8 0D 08 00; misaligned on purpose)
 2. img0   = vb_word(0x170 + 0x10) * 512     // 0x7E  * 512 = 0x0000FC00   (image 0 size)
    pcir1  = 0x0FC00 + vb_word(0xFC18)       // 0x0FC00 + 0x1C  = 0x0FC1C
    img1   = vb_word(pcir1 + 0x10) * 512     // 0xA7  * 512 = 0x00014E00   (EFI image size)
    b2     = 0x0FC00 + img1                  // 0x00024A00
    pcir2  = b2 + vb_word(b2 + 0x18)         // 0x24A00 + 0x160 = 0x24B60
    type2  = vb_byte(pcir2 + 0x14)           // 0xE0  <-- first 0xE0 image
    shift  = b2 - img0                       // 0x00014E00
 3. t = p + shift                            // 0x00095BE8
    hdr = vb_byte(t), vb_byte(t+1), vb_byte(t+2), vb_byte(t+3)   // 01 06 06 10
 4. for i in 0..cnt-1:  e = t + hdr + i*entry
        if vb_byte(e) == 0x85: ptr = vb_dword(e+2)               // 0x0002F03C
 5. d = ptr + shift                          // 0x00043E3C
    H = vb_dword(d)                          // 0x032C0301
 6. ucode = d + ((H >> 16) & 0xFFFF)         // 0x00044168
    size  = vb_dword(d + 4)                  // 65408
```

Validation ladder — reject the ROM (do not "try another base") if any of these fails:

| # | assertion | this card |
|---|---|---|
| 1 | `vb_word(0x000) == 0xAA55` (or an `NVGI` IFR chain, §1.3 of the contract) | `0xAA55` |
| 2 | `vb_word(0x0FC00) == 0xAA55` and `vb_word(0x24A00) == 0x4E56` | holds |
| 3 | `type2 == 0xE0` and `shift == 0x14E00` and `img1 == 0x14E00` | holds |
| 4 | `t + 0x10 < 0x100000` and `vb_byte(t) == 1` and `vb_byte(t+1) >= 6` and `vb_byte(t+2) >= 6` | `0x95BE8`, 1/6/6 |
| 5 | `d + 44 < 0x100000` and `(H & 1)` and `((H>>8)&0xFF) == 3` and `((H>>16)&0xFFFF) in [44, 0x1000]` | `0x43E3C`, V3/812 |
| 6 | `vb_dword(d+0x14) + vb_dword(d+0x20) == vb_dword(d+4)` | `61952 + 3456 == 65408` |
| 7 | `d + hdrsize + size <= 0x100000` | `0x540E8 <= 0x100000` |

A hard-coded `shift = 0x14E00` also works on this exact VBIOS, but computing it from steps 2 costs four
reads and survives a VBIOS update; the assertions in rows 2–3 make a wrong `shift` fail loudly rather
than silently parsing noise.

The change to `nv_vbios_probe()` is therefore small: after the existing token-`0x70` read, add the
image-chain walk (four reads), add `shift` to the table pointer and to the entry's descriptor pointer,
and replace the "structural scan for a V1 table" with "walk `EntryCount` slots, take `app == 0x85`".

---

## 8. What this contradicts in `docs/nv-pmu-acr-contract.md` §1.3–1.5

| contract text | measured | correction |
|---|---|---|
| §1.4: token `0x70` payload is "offset of the falcon ucode table, relative to `expansionRomOffset`" | `expansionRomOffset` is `0` here (no IFR), yet `0x80DE8` lands nowhere | the base is the *logical* image with the EFI image removed: `flash = logical + 0x14E00` for `logical >= 0x0FC00` |
| §1.4 caveat row: "the token offset does not account for the EFI image … the `+ expansionRomOffset` above is the resolution" | right instinct, wrong resolution | the EFI image *is* the missing 0x14E00 bytes; `expansionRomOffset` does not help when it is 0 |
| §1.5: "expect V3, size 44" and checklist #8 "`vDesc` size ≥ 44" | `Hdr.size` is **812**, not the 44-byte struct | `Hdr.size` is the *payload offset*, and it includes the 2×384-byte signature block; use it, never `sizeof` |
| §1.5 `FALCON_UCODE_TABLE_HDR_V1` row: `DescVersion`, `DescSize` | header is `01 06 06 10 01 30`, i.e. `DescSize = 0x30` while the descriptors are 812-byte V3 headers | `DescSize` is not usable for these descriptors; take the version/size from each descriptor's own `Hdr` |
| §1.6 checklist #6: `EntryCount` 3..20 and (implicitly) non-zero app ids | `EntryCount = 16` with 9 zero-filled slots | walk every slot and skip `app == 0`; a "non-zero app ids" filter rejects the real table (§4.2) |
| §1.6 checklist #10: "`UcodeId` in 1..8" | `UcodeId = 9` | widen to 1..16 (or just `!= 0` and `< 32`) |
| §1.2 route A: "`romlen` bytes from the BAR" (512 KiB) | only `0x24A00` bytes are readable; that is the two PCI images | route A is structurally incapable of reaching the `0xE0` images; route B is mandatory |
| §1.6 checklist #11: "two `0xE0`-type ROM images" | two, at `0x24A00` (24 KiB) and `0x2AA00` (429 KiB); only the second carries the ucode table | confirmed, but the *first* `0xE0` image is the one that defines the pointer base and contains nothing we need |
| §1.3 step 1 ("`0xAA55` ⇒ no IFR") | `0xAA55` at 0, no `NVGI` anywhere in the dump | confirmed |

Unchanged and confirmed by the dump: the BIT scan key `FF B8 "BIT\0"`, the zero-sum header rule, token
stride 6, token `0x41F` payload for `0x70`, `DataVersion == 2`, `DataSize == 4`, `EngineIdMask & 0x0400`
non-zero, and the existence of app `0x45` (debug) next to `0x85` (prod).

---

## 9. What this document does not establish

* **Decryption.** The 65408-byte payload is not plaintext and this lane did not decrypt it. Whoever
  boots FWSEC must go through the ACR/bootloader path with the fused key; nothing here changes that.
* **Whether the vendor stack uses the same two hops.** The vendor's own parse
  (`kgspParseFwsecUcodeFromVbiosImg`) works on an image the RM has already normalised; the offsets it
  prints will not match these flash offsets. Only nouveau's `nvbios_addr()` semantics were used here,
  and they are consistent with all four descriptors in the dump.
* **Other VBIOS revisions.** Every number is this card (VBIOS version string token `0x42` =
  `95.02.18.40…`). The *rule* is generic; `0x14E00`, `0x95BE8`, `0x43E3C` are not.
* **The 24 KiB `0xE0` image at `0x24A00`** — it is walked as an image and skipped; its contents were not
  interpreted beyond "no BIT table, no ucode table, no app `0x85`".

---

## 10. Evidence

| file | what |
|---|---|
| `docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.gz` | the 1 MiB dump from `BAR0+0x300000`, gzipped (403686 bytes) |
| `docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.sha256` | sizes and sha256 of the raw and gzipped files |
| `docs/nv-vbios-evidence/analyse_vbios.py` | the analysis tool: image chain, BIT tokens, ucode table, descriptors (run on the gunzipped dump) |
| `docs/nv-vbios-evidence/analyse_vbios.out` | its output for this dump |
| `docs/nv-vbios-evidence/dump-and-verify.txt` | transcript: ROM BAR size/EOF, PROM aperture sha256, landmark scan, and the §7 recipe executed as dword reads in the guest |

Reproduce: `gunzip -c rtx4090-ad102-vbios-1MiB.bin.gz > /tmp/v.bin && python3 analyse_vbios.py /tmp/v.bin`.
