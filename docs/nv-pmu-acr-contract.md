# The PMU / FWSEC / SEC2 contract on AD102 (RTX 4090, 0x192, firmware 535.113.01)

Scope: everything the host must do between "BAR0 is mapped and SEC2 answers `0xBADF5620`"
and "the SEC2 window is a usable falcon" — i.e. the rung between
`docs/nvidia-driver.md` §5.7 and `docs/nv-gsp-wpr-contract.md` §4.2/§6.
Written to be implemented from; every claim is tagged.

## 0. Provenance tags, and the one correction this doc makes

| tag | meaning |
|---|---|
| **MEASURED** | read off this card (RTX 4090, AD102, PCI 2:0.0, in the HobbyOS VM) by this driver or the reference stack |
| **SOURCE** | stated by a named function/struct in a public source (nouveau, NVIDIA `open-gpu-kernel-modules`, kernel `Documentation/gpu/nova/`) |
| **INFERRED** | my reading of the above, not yet proved on this card. A wrong write here can wedge the card — treat every INFERRED row as "test on a resettable card first" |
| **NOT ESTABLISHED** | no public source, no measurement. Do not guess |

The correction, stated up front because it changes the plan:

> On AD102 with firmware 535, **the PMU is not in the boot path at all.** Neither the
> vendor driver nor nouveau sends the PMU a single command on this generation. The
> SEC2 window is not fenced because the PMU has not run a handshake; it is fenced
> because SEC2 is a **privileged** register aperture and the privilege level that
> admits the host is lowered by **FWSEC**, which runs **on the GSP falcon**, not on
> the PMU and not on SEC2. So the step the previous lane called "PMU → FWSEC/ACR → SEC2"
> is really "GSP falcon runs FWSEC-FRTS → (privilege drops) → host loads the booter
> into SEC2". §2 and §5 carry the evidence; §6 is the order to implement.

| # | claim | tag |
|---|---|---|
| 0.1 | `0x840000`..`0x841FFF` (SEC falcon + its PLM block) returns `0xBADF5620` for every read, before any host-side initialisation | MEASURED |
| 0.2 | `0xBADF5620` is a hardware signature for "this register aperture is not accessible to you right now", not data: the vendor driver has a sanity checker for exactly this word (`gpuHandleSanityCheckRegReadError_GM107`, "Possible bad register read") | SOURCE |
| 0.3 | Access to some blocks is *privilege*-gated and only opens after secure firmware "lowers the privilege level to allow CPU (LS/low-secure) access" — said of the GFW_BOOT handshake, same mechanism | SOURCE (`Documentation/gpu/nova/core/devinit.rst`, "Security and Access Control") |
| 0.4 | The SEC2/PLM register block is declared in a header whose name is literally "second falcon **privileged**": `dev_falcon_second_pri.h`, `NV_FALCON2_SEC_BASE 0x00841000` | SOURCE |
| 0.5 | Therefore the fence is a privilege fence, and the thing that lifts it is FWSEC, not `PMC_ENABLE` (which is why the 32-bit sweep found nothing) | INFERRED (this is the hypothesis §6 step 3 tests: two writes and two reads) |
| 0.6 | No public source has the host send the PMU any command on AD102/535: nouveau's `gp102_pmu_new()` returns `-ENODEV` when GSP-RM is in use, `gp102_pmu_fwif` has no firmware (`gm200_pmu_nofw`), and the vendor's `kgspBootstrap*` path never touches the PMU | SOURCE |

---

## 1. Where the ucode comes from: the VBIOS, and the BIT token that names it

### 1.1 Why the VBIOS and not a file on disk

| fact | detail | tag |
|---|---|---|
| FWSEC (the ucode this doc is about) is **not** a file in the 535 driver package on this path; it is read out of the card's own VBIOS | vendor: `kgspParseFwsecUcodeFromVbiosImg()` reads "a VBIOS image from ROM, and parses it for FWSEC" before booting GSP-RM (`kernel_gsp.c`, ~line 2321) | SOURCE |
| nouveau does the same, at boot, every boot | `nvkm_gsp_fwsec_init()` → `nvbios_pmuEp()` loop looking for `type == 0x85`, then `nvbios_pointer(bios, flcn_ucode.data)` | SOURCE |
| the PMU's own ucode also lives there (same table, different app id), which is why the PMU is already running on a card the host has never touched | kernel doc: the ROM's Boot-ROM/IFR firmware "bootstraps various microcontrollers (PMU, SEC, GSP)" before the driver loads | SOURCE |
| our card's PMU is in fact idle-but-loaded: `UC_CTRL = 0x20` = bit 5 = `SLEEPING` ("no instructions being executed, interrupts can restart execution") | MEASURED value + SOURCE bit meaning (`rnndb/falcon.xml` `uc_cpuctl`; `docs/hw/falcon/proc.rst`) | MEASURED + SOURCE |
| so "the PMU has no ucode yet" is **wrong** on this card; there is nothing for the host to upload to the PMU | consequence of the two rows above | INFERRED |

### 1.2 How the VBIOS is readable at all

Three routes exist; route A is the one to use in a VM, route B is the fallback if the
ROM BAR is not decodable.

| route | mechanism | gotcha on this card | tag |
|---|---|---|---|
| **A. PCI Expansion ROM BAR** | config `0x30`: set bit 0 (enable), read `romlen` bytes from the BAR, clear bit 0 when done. nouveau: `pcirom_init()` → `pci_enable_rom()` / `pci_map_rom()` | `lspci -vvv` on this card says **`Expansion ROM at fa000000 [virtual] [disabled] [size=512K]`** — i.e. disabled and virtualised. Enabling it is a config-space write the VM may or may not honour. Test first | SOURCE + MEASURED |
| **B. PROM aperture in BAR0** | `BAR0 + 0x300000`, a 1 MiB window onto the flash. nouveau: `nvbios_prom_init()` / `nvbios_prom_read()` | the window normally shows the *shadow* copy; the shadow must be turned off first: PCI config `0x50` bit 0 ← 0 (`nvkm_pci_rom_shadow(pci,false)`), restore to 1 afterwards. Reads must stay inside `0x300000..0x3FFFFF` | SOURCE |
| **C. VBIOS image in VRAM (PRAMIN)** | display engine reads the ROM copy out of FB | not needed, and awkward in a VM | SOURCE |

### 1.3 The IFR header: the ROM does not start with `0xAA55`

On Kepler+ the flash image may start with an **IFR** (init-from-ROM) header, and the
real PCI Expansion ROM begins at an offset the IFR points at. Algorithm
(`Documentation/gpu/nova/core/vbios.rst`; implemented in nouveau `nvbios_shadow`/`shadowrom.c`):

| step | read | expect | tag |
|---|---|---|---|
| 1 | word at ROM+0 | `0xAA55` in the low 16 bits ⇒ no IFR, PCI ROM image is at 0. Otherwise continue | SOURCE |
| 2 | dword at ROM+0x00 (`FIXED0`) | `0x4947564E` = ASCII `"NVGI"` | SOURCE |
| 3 | dword at ROM+0x04 (`FIXED1`) | bits 15:8 = `VERSIONSW`; bits 30:16 = `FIXED_DATA_SIZE` | SOURCE |
| 4 | dword at ROM+0x08 (`FIXED2`) | bits 19:0 = `TOTAL_DATA_SIZE` (used for VERSIONSW 3) | SOURCE |
| 5a | VERSIONSW 1 or 2: dword at `ROM + FIXED_DATA_SIZE + 4` | = PCI ROM image offset | SOURCE |
| 5b | VERSIONSW 3: dword at `ROM + TOTAL_DATA_SIZE`, **+ 4096** | must read `0x44524652` = `"RFRD"` (ROM directory); dword at `dir + 8` = PCI ROM image offset | SOURCE |
| 6 | word at `ROM + rom_offset` | `0xAA55`, and `rom_offset` is 4-byte aligned and `< 0x100000` | SOURCE |

All offsets in step 5 are relative to the start of the ROM window (BAR0+0x300000), which is
the same base the BIT scan uses. From here on, "ROM" means `ROM + rom_offset`
(the PCI ROM image), and the vendor code calls that value `expansionRomOffset`.

### 1.4 The BIT table, and token `0x70`

| field | offset | type | note | tag |
|---|---|---|---|---|
| `BIT_HEADER_ID` | +0x00 | u16 | **`0xB8FF`** — scan for these two bytes | SOURCE (`kernel_gsp_fwsec.c`, 535) |
| `BIT_HEADER_SIGNATURE` | +0x02 | u32 | **`0x00544942`** = `"BIT\0"` — must match at `+2`, i.e. the scan key is `FF B8 42 49 54 00` | SOURCE |
| `BCD_Version` | +0x06 | u16 | BCD, e.g. `0x0100` | SOURCE |
| `HeaderSize` | +0x08 | u8 | **tokens start at `bit + HeaderSize`** | SOURCE |
| `TokenSize` | +0x09 | u8 | token stride (usually 6) | SOURCE |
| `TokenEntries` | +0x0A | u8 | token count | SOURCE |
| `HeaderChksum` | +0x0B | u8 | **sum of the first `HeaderSize` bytes must be `0` mod 256** — this is what makes the scan unambiguous | SOURCE |

Scan the whole ROM for the 6-byte key and accept only a candidate whose header checksum
is zero. Then walk the tokens (`bit + HeaderSize + i*TokenSize`):

| token field | offset | type | note | tag |
|---|---|---|---|---|
| `TokenId` | +0 | u8 | `0x42` = BIOS data (VBIOS version string), **`0x70` = falcon data** | SOURCE |
| `DataVersion` | +1 | u8 | must be **2** for the falcon-data token | SOURCE |
| `DataSize` | +2 | u16 | must be ≥ 4 | SOURCE |
| `DataPtr` | +4 | u16 | offset of the token payload | SOURCE |
| payload of token `0x70` | — | u32 | `FalconUcodeTablePtr` — **offset of the falcon ucode table, relative to `expansionRomOffset`**, not to the ROM start | SOURCE (vendor: `pVbiosImg->expansionRomOffset + falconData.FalconUcodeTablePtr`) |
| caveat | | | kernel doc warns the token offset "does not account for the EFI image located between the PciAt and FwSec images"; the `+ expansionRomOffset` above is the resolution | SOURCE |

Same table seen from nouveau's side: it calls this token `'p'` (ASCII 0x70) via
`bit_entry(bios, 'p', &bit_p)`, requires `version == 2 && length >= 4`, and reads a u32 at
`bit_p.offset` — identical contract, different spelling. Its per-app payload layout
(`nvbios_pmuR`) is the **legacy** PMU app shape, `{init_addr_pmu, args_addr_pmu, boot_*,
code_*, data_*}` at +0x08..+0x2C — used for PMU apps, **not** for FWSEC.

### 1.5 The falcon ucode table and the FWSEC descriptor

| structure | layout | tag |
|---|---|---|
| `FALCON_UCODE_TABLE_HDR_V1` (6 bytes) | `Version` u8 (=**1**), `HeaderSize` u8 (≥6), `EntrySize` u8 (≥6), `EntryCount` u8, `DescVersion` u8, `DescSize` u8 | SOURCE |
| `FALCON_UCODE_TABLE_ENTRY_V1` (6 bytes) | `ApplicationID` u8, `TargetID` u8, `DescPtr` u32 (relative to `expansionRomOffset`) | SOURCE |
| `FALCON_UCODE_DESC_HEADER.vDesc` (u32) | bit 0 `VERSION_AVAILABLE`(=1), bit 2 `ENCRYPTED`, bits 15:8 **version** (V1=1 … V4=4), bits 31:16 **size** | SOURCE |

App ids that matter:

| `ApplicationID` | meaning | tag |
|---|---|---|
| `0x85` | **FWSEC_PROD** — the one we boot | SOURCE |
| `0x45` | FWSEC_DBG (debug build; do not pick) | SOURCE |
| `0x05` | FIRMWARE_SEC_LIC — skipped by the FWSEC lookup, accepted by the vendor's filter | SOURCE |

Descriptor bodies (the host must read these fields, they are what gets programmed):

| `FALCON_UCODE_DESC_V2` (60 B, `"15d"`) | offset | | `FALCON_UCODE_DESC_V3` (44 B, `"9d1w2b2w"`) | offset |
|---|---|---|---|---|
| `Hdr.vDesc` | 0x00 | | `Hdr.vDesc` | 0x00 |
| `StoredSize` | 0x04 | | `StoredSize` | 0x04 |
| `UncompressedSize` | 0x08 | | `PKCDataOffset` | 0x08 |
| `VirtualEntry` | 0x0C | | `InterfaceOffset` | 0x0C |
| `InterfaceOffset` | 0x10 | | `IMEMPhysBase` | 0x10 |
| `IMEMPhysBase` | 0x14 | | `IMEMLoadSize` | 0x14 |
| `IMEMLoadSize` | 0x18 | | `IMEMVirtBase` | 0x18 |
| `IMEMVirtBase` | 0x1C | | `DMEMPhysBase` | 0x1C |
| `IMEMSecBase` | 0x20 | | `DMEMLoadSize` | 0x20 |
| `IMEMSecSize` | 0x24 | | `EngineIdMask` u16 | 0x24 |
| `DMEMOffset` | 0x28 | | `UcodeId` u8 | 0x26 |
| `DMEMPhysBase` | 0x2C | | `SignatureCount` u8 | 0x27 |
| `DMEMLoadSize` | 0x30 | | `SignatureVersions` u16 | 0x28 |
| `altIMEMLoadSize` | 0x34 | | `Reserved` u16 | 0x2A |
| `altDMEMLoadSize` | 0x38 | | | |

**Which version on this card:** GA102 is documented with a `FalconUCodeDescV3` for FWSEC
(`Documentation/gpu/nova/core/vbios.rst`, the GA102 layout diagram), and 535 parses V2 or V3
and rejects anything else. So: expect **V3, size 44** — but read the header, do not assume.
Tag: SOURCE for the shape, INFERRED for V3-on-this-card.

### 1.6 The checklist for a read-only VBIOS dump of *this* card

Implement these as assertions in a dump tool (no writes, nothing to wedge):

| # | check | expect | if it fails |
|---|---|---|---|
| 1 | `ROM[0..3]` | `"NVGI"` or a `0xAA55` at 0 | wrong aperture / shadow still on (route B) |
| 2 | IFR → `rom_offset`, then `ROM[rom_offset]` low16 | `0xAA55` | ROM BAR truncated |
| 3 | `PCIR` structure present at `rom + romhdr.pci_data_struct_offset`, `PCIR` signature, `image_len` in 512-byte units | present | it is not a PCI ROM image |
| 4 | scan for `FF B8 "BIT\0"`, header checksum ≡ 0 mod 256 | exactly one candidate | shadow/PROM confusion |
| 5 | token `0x70`, `DataVersion == 2`, `DataSize >= 4` | present | VBIOS is not Ampere-shaped or the BIT is a different build |
| 6 | ucode table at `rom_offset + FalconUcodeTablePtr`: `Version == 1`, `HeaderSize >= 6`, `EntrySize >= 6`, `EntryCount` 3..20 | | pointer arithmetic (did you forget `rom_offset`?) |
| 7 | an entry with `ApplicationID == 0x85` | exactly one | wrong VBIOS for a GSP build |
| 8 | that entry's `vDesc`: bit0 = 1, version byte = 3, size ≥ 44 | V3/44 | if V2/60, use the V2 table above |
| 9 | V3 `EngineIdMask & 0x0400` | non-zero (HS falcon engine class) | signature will not validate; `ga102_gsp_fwsec_signature()` refuses if the bit is clear |
| 10 | V3 `UcodeId` in 1..8, `SignatureVersions != 0`, and `(1 << fls(rd32(0x8241C0 + (UcodeId-1)*4))) & SignatureVersions != 0` | non-zero | the fused key version and the descriptor disagree — nothing you write will authenticate |
| 11 | count of `0xE0`-type ROM images | **2** (first FwSec, second FwSec) | not a blocker, but the doc says Ampere has two |
| 12 | also log token `0x42` payload → VBIOS version dword | e.g. `0x95xxxxxx` + OEM byte | this is your only positive ID of the image |

The concrete byte offsets for *this* card are **not established** — nobody has dumped its
VBIOS (see §8, dump D1). Items 1–12 are the acceptance test for whichever dump arrives.

---

## 2. The host ↔ PMU command interface (and why there is nothing to send)

### 2.1 What the interface is, on the generations where it is used

| element | address | contract | tag |
|---|---|---|---|
| command word | `0x10A040` (`PDAEMON_SCRATCH0` = PMU falcon `MAILBOX0`) | one dword: command id in the low bits, status in the top nibble. Status 1 = NEW, 2 = PENDING, 3 = COMPLETE. Caveat: the only public description ORs the status in as `0x01` yet then masks `0xF0000000`, so the packing is stated inconsistently there — **NOT ESTABLISHED** in detail, and moot per §2.2 | SOURCE (envytools `nvrm/pmu/ucode-cmds`) + NOT ESTABLISHED |
| status/error word | `0x10A044` (`SCRATCH1`/`MAILBOX1`) | low 31 bits = error code (0 = no error), bit 31 = "log available" | SOURCE |
| ring | host writes the packet into `0x10A040`, PMU firmware polls it | no doorbell register, no interrupt required | SOURCE |
| prepare step | `0x022408` bit 1 set before first command | "PUNITS_UNK008" — meaning not documented | SOURCE (envytools pseudocode only) |
| opcode table | `NV_UCODE_CMD_COMMAND_EID/ESI/ERD/EWR/RRD/RWR/…` (0x02..0x18 in the 390-era ucode) | EEPROM/VBIOS/register access, license verify. **No opcode boots SEC2** | SOURCE |
| PMU falcon queue registers | `0x10A4A0`/`0x10A4B0` (cmd), `0x10A4C8`/`0x10A4CC` (msg) | nouveau `gp102_pmu_flcn` declares a cmdq/msgq pair there; field semantics not published | SOURCE + NOT ESTABLISHED |

Packet "format": there isn't one beyond the 32-bit word — the payload (address, length) is
passed in DMEM followed by the PMU ucode, on the generations that use this interface. There
is no header, no alignment rule, no sequence number, and no completion queue: **completion
is the same word changing state**, polled by the host.

### 2.2 Why none of that applies here

| # | observation | tag |
|---|---|---|
| 1 | nouveau's `gp102_pmu_new()` returns `-ENODEV` when `nvkm_gsp_rm(device->gsp)` — the whole PMU subdev is skipped in the GSP-RM configuration, on GP102 through Ada | SOURCE |
| 2 | `gp102_pmu_fwif` has no firmware of any kind (`gm200_pmu_nofw`): on GP102+ the **host never uploads PMU ucode** | SOURCE |
| 3 | the vendor's boot path (`kgspBootstrap_TU102` / `kgspBootstrapRiscvOSEarly_TU102`, `kernel_gsp_tu102.c`) contains no PMU transaction: reset GSP → FWSEC-FRTS → reset GSP → pre-queue two RPCs → SEC2 booter → GSP-RM | SOURCE |
| 4 | our PMU is `SLEEPING` with valid `UC_CAPS` (v6) — firmware already resident, so there is nothing for a "PMU handshake" to accomplish | MEASURED |
| 5 | the fence on SEC2 does **not** lift when any `PMC_ENABLE` bit is set | MEASURED |

**Verdict:** the ordered command list that ends in "SEC2 is initialised" contains **zero PMU
commands**. Do not build a PMU command driver for this card. The interface in §2.1 is
recorded here only so that nobody re-derives it and mistakes it for the answer.
Tag: SOURCE + MEASURED.

---

## 3. FWSEC / ACR: what runs where, and what FRTS is

### 3.1 Who runs what (this is where the previous lane's model was off by one processor)

| stage | actually runs on | loaded by | delivered as | tag |
|---|---|---|---|---|
| Boot-ROM / IFR firmware, devinit | **PMU** (light-secure) | fused boot ROM | VBIOS | SOURCE |
| **FWSEC** (this doc's subject) | **GSP falcon** (`0x110000`), heavy-secure | **host**, every boot | DMA from the VBIOS-sourced ucode buffer | SOURCE (vendor `Documentation/gpu/nova/core/vbios.rst`: "typically runs directly on the GSP … as of Ampere, it is the GSP"; nouveau `nvkm_falcon_fw_ctor(..., &gsp->falcon, fw)`) |
| FWSEC's own child ucodes (devinit, PMU/SEC2 pre-OS apps) | PMU, SEC2 | FWSEC | — | SOURCE (same doc) |
| booter (`booter_load`, 55 928 B) | **SEC2** | **host** | IMEM+DMEM DMA, BROM registers | SOURCE (`docs/nv-gsp-wpr-contract.md` §4.2) |
| bootloader, GSP-RM | GSP (RISC-V) | booter | radix3 into VRAM | SOURCE (same doc) |

So "ACR" on this part is not a separate binary the host loads: the role ACR played on
Maxwell/Pascal (an HS blob that carved the WPR and bootstrapped engines) is split between
**FWSEC-FRTS** (carves WPR2, drops privileges) and the **SEC2 booter** (places the
bootloader/RM and starts the GSP). nouveau encodes exactly this by refusing the ACR subdev
whenever GSP-RM is in use (`ga102_acr_new()` → `-ENODEV`).
Tag: SOURCE.

### 3.2 What FRTS is, and what the host must supply

FRTS = the power-management data blob FWSEC writes into WPR2; the region must exist *before*
the booter places GSP-RM, because both live inside the same write-protected region.

| item | value / rule | tag |
|---|---|---|
| region size on this chip | `0x100000` (1 MiB). GA100 (`chipset 0x170`) is the exception with size 0 | SOURCE (nouveau `tu102_gsp_oneinit`) |
| region address | `ALIGN_DOWN(bios_workspace_addr, 0x20000) - size`, i.e. the top of the WPR2 stack, directly under the BIOS/VGA workspace. For our computed layout: **`0x5FE300000`, size `0x100000`** | SOURCE for the rule, MEASURED for our own layout arithmetic (`docs/nv-gsp-wpr-contract.md` §3.2) |
| what the host hands FWSEC | the DMEM `FRTS` command struct: `frts_region.addr = frtsOffset >> 12`, `.size = frtsSize >> 12`, `.type = 2` (FB), preceded by a `read_vbios` struct `{ver=1, hdr=sizeof, addr=0, size=0, flags=2}` | SOURCE (nouveau `nvkm_gsp_fwsec_patch`, `nvfw_fwsec_frts_cmd`) |
| units | **4 KiB pages**, not bytes — the only place in this chain with page units | SOURCE |
| how the host selects the command | `init_cmd` word in the DMEM-mapper app interface: `0x15` = FRTS, `0x19` = SB | SOURCE (same file, `NVFW_FALCON_APPIF_DMEMMAPPER_CMD_*`) |
| where those words go | at `DMEM + interface_offset` there is an appif header `{ver=1, hdr, len, cnt}` + `cnt` app entries `{id, dmem_base}`; the entry with `id == 4` (DMEMMAPPER) gives the DMEM offset of the mapper block, whose `cmd_in_buffer_offset` is where the two structs above are written | SOURCE |
| WPR2 pair after success | `0x1FA824`/`0x1FA828` should read `start == frtsOffset>>12` and a non-zero end; vendor reads them back as a post-check (SOURCE, `kern_vbios_tu102.c`) — but **on this card those two words are read-protected** (`0x1FA7C8` bit 0 = 1), so they are not usable as an oracle here | SOURCE, but MEASURED conflict on this card, see §7 |

### 3.3 The FWSEC launch itself (GSP falcon, `0x110000`)

Order, and nothing here is optional:

| # | action | register | value | tag |
|---|---|---|---|---|
| 1 | reset the GSP falcon | falcon block | `0x048 &= ~3`, `0x014 = 0xffffffff`, then engine-reset pulse `0x3C0.0 = 1`, 10 µs, `0x3C0.0 = 0`, then wait `0x0F4` bit 31 set (≤150 µs) and bit 12 clear (≤20 ms) | SOURCE (nouveau `gm200_flcn_disable`, `ga102_flcn_reset_prep`, `ga102_flcn_reset_wait_mem_scrubbing`) |
| 2 | let the DMA see sysmem, no context | `0x624` bit 7 set (`ALLOW_PHYS_NO_CTX`); `0x10C = 0` | | SOURCE (`ga102_flcn_fw_load`) |
| 3 | route the DMA port to non-coherent physical sysmem | `0x600` ← `0x00000005` (ctx 0, TYPE 5 = SYSRAM, port valid) | | SOURCE (same) |
| 4 | load IMEM | 256-byte blocks: `0x110 = phys>>8`, `0x128 = 0`, `0x114 = imem_base`, `0x11C = img_off`, `0x118` = size 256B (`6` in bits 10:8), IMEM bit 4 set, SEC bit 3:2 set; then poll `0x118` bit 1 | | SOURCE (`ga102_flcn_dma_init/xfer/done`) |
| 5 | load DMEM (`sec = false` for FWSEC's DMEM) | same loop, `IMEM` bit clear | | SOURCE |
| 6 | write the DMEM boot descriptor | `tu102_gsp_fwsec_load_bld`: `flcn_bl_dmem_desc_v2{ctx_dma=PHYS_SYS_NCOH, code_dma_base=fw phys, non_sec_code_off/size, sec_code_off/size, entry=0, data_dma_base=fw phys + dmem_base_img, data_size}` pushed by PIO to DMEM offset 0; and `0x600 + ctx*4` ← 5 | | SOURCE |
| 7 | prefill mailbox, boot vector, start | `0x040` ← 0 (FRTS: expected success value), `0x104` ← 0, `0x100` ← 2 | | SOURCE (`gm200_flcn_fw_boot`) |
| 8 | wait for halt | poll `0x100` bit 4 (`HALTED`) ≤ **2 s** | | SOURCE |
| 9 | read verdict | `0x040` must be **0**; also read `0x044` | | SOURCE |
| 10 | read the FWSEC status word | BAR0 **`0x1438`**: **high 16 bits = FRTS status**, **low 16 = SB status**; non-zero = error code | | SOURCE (nouveau `nvkm_gsp_fwsec_frts` / `_sb`); the packing is measured by those two reads and **not** documented elsewhere, so treat the split as INFERRED-but-consistent |
| 11 | reset the GSP again, then switch it to RISC-V | `0x111668 ← 0x111` = VALID (bit 0) + CORE_SELECT=RISCV (bit 4) + BRFETCH (bit 8); read it back immediately | | SOURCE |

Note the asymmetry that trips people: **Turing loads FWSEC by PIO (`gm200_flcn_fw_load`),
GA102 loads it by DMA (`ga102_flcn_fw_load`)**. Steps 2–5 are GA102-only. Tag: SOURCE.

### 3.4 What SEC2 looks like after FWSEC

| observation | before FWSEC | after FWSEC-FRTS (expected) | tag |
|---|---|---|---|
| any read in `0x840000..0x841FFF` | `0xBADF5620` | real register contents | MEASURED before / INFERRED after |
| `0x840100` (`CPUCTL`) | fence word | `0x10` = `HALTED` (bit 4) — reset-into-falcon state, not running | INFERRED from SEC2 post-reset reads on Turing (`0x10`) reported by the `gpu-unlock` issue #242 thread, GA102 comment "until the launch resets the engine" |
| `0x841000+0x668` (`BCR_CTRL` of the PLM) | fence word | `0x0` or `0x1` depending on core select | NOT ESTABLISHED |
| write acceptance | unknown — a read-back cannot tell you, because reads return the fence word either way | same as before: unproven | MEASURED (the read-back limitation itself); the consequence is that **SEC2 bring-up must be done blind and judged by MAILBOX0**, exactly as the vendor does (INFERRED) |

---

## 4. First proof of life, per step

Every stage names itself. Each row is one bounded wait with one exact expectation, and one
log line that makes a timeout diagnosable without re-running.

| # | stage | proof (exact) | bound | on timeout / mismatch, log |
|---|---|---|---|---|
| 1 | VBIOS route works | first 4 bytes of the ROM window = `"NVGI"` or `0xAA55` | none (read-only) | which route (ROM BAR / PROM), the 4 bytes, `romlen`, and PCI `0x30` + `0x50` raw values |
| 2 | BIT found | 6-byte key at `bit`, checksum byte sums to 0 mod 256 | none | every candidate offset tried, and each candidate's header bytes |
| 3 | FWSEC blob found | `AppID == 0x85` entry exists; `vDesc` version = 3, size = 44 | none | the whole ucode table header + all entries (`AppID`, `TargetID`, `DescPtr`) |
| 4 | GSP falcon reset | `0x1100F4` (HWCFG2) bit 31 = 1, then bit 12 = 0 | 150 µs / 20 ms | `0x1100F4`, `0x11010C`, `0x110100` |
| 5 | FWSEC image DMA landed | last block: `0x110118` bit 1 (IDLE) = 1 | 2 s total | block index, `0x110`, `0x110114`, `0x110118`, `0x11011C` |
| 6 | FWSEC ran | `0x110100` bit 4 (`HALTED`) = 1 | **2 s** | `0x110100`, `0x110104`, `0x110040/44`, plus the 5 DMEM words at the appif header |
| 7 | FWSEC verdict | `0x110040 == 0` **and** `rd32(0x1438) >> 16 == 0` | immediate | the raw `0x1438`, the raw `0x110040`, and "this is a FWSEC error code, not a HobbyOS bug" |
| 8 | privileges dropped / SEC2 awake | **`0x840100 != 0xBADF5620`** | 100 ms, 5 tries | `0x840100`, `0x84012C`, `0x841000`, `0x8400F4`, and the comparison word `0xBADF5620` |
| 9 | booter loaded | readbacks: `0x840110`, `0x840128`, `0x840114`, `0x84011C`, `0x840104`, `0x840100` bit 4 still 1, and the four BROM words `0x841180` (`MOD_SEL`), `0x841198` (`CURR_UCODE_ID`), `0x84119C` (`ENGIDMASK`), `0x841210` (`PARAADDR(0)`) | none | full 19-step trace of what was written (the WPR doc's own advice) |
| 10 | booter verdict | `0x840100` bit 4 = 1 within 2 s, then `0x840040 == 0` | 2 s | `0x840040/44` raw, plus step 9's trace |
| 11 | GSP came up | `0x111668` reads `0x111` right after the write | immediate | `0x111668`, `0x111100`, `0x110040/44` |
| 12 | RM is serving | first `VRPC` element on the status queue, `0x03000000`, function `0x1001`, `rpc_result == 0` | 4 s | queue `writePtr`/`readPtr`, block phys, first 64 bytes at each queue head |

Rules that come out of the measurements, not taste:

- **Do not gate any decision on a read of a fenced block.** Step 8 is the *only* gate that
  reads SEC2, and its pass condition is "not the fence word", not any particular value.
- **`MAILBOX0 == 0` is the ground truth for both launches** (FWSEC on GSP, booter on SEC2).
  Everything else is circumstantial. Tag: SOURCE.
- Log the fence word itself (`0xBADF5620`) in every failure line, so a later reader can tell
  "fenced" from "read as zero" at a glance.

---

## 5. What may be skipped, and what the fence actually is

| question | answer | tag |
|---|---|---|
| Can the host boot GSP directly via SEC2 without the PMU? | **Yes — that is the only path on AD102.** The PMU is not involved before, during, or after | SOURCE (§2.2) + MEASURED |
| Is FWSEC-SB needed before the booter? | **No.** SB runs at *teardown* (unload), to put pre-OS apps back: vendor `kgspExecuteFwsecSb_HAL()` is called from the unload path (~line 2582 of `kernel_gsp.c`), and nouveau builds the SB blob at oneinit but only boots it in `tu102_gsp_fini`. The boot path runs **FRTS only** | SOURCE |
| Is FWSEC-FRTS needed? | Yes on AD102, because `frtsSize > 0`. A boot with `frtsSize = 0` is legal on GA100 only | SOURCE |
| Can FRTS be skipped and the WPR2 pair written by hand? | Not on this card: `0x1FA7C8` bit 0 is set, the WPR2 pair reads as a name rather than a state, and the whole region is only writable by heavy-secure ucode | MEASURED + SOURCE (`docs/nv-gsp-wpr-contract.md` §2.4) |
| What is the fence? | A privilege mask on the privileged register aperture: reads return `0xBADF5620` until the secure firmware lowers the privilege level. NVIDIA's own header calls this block "second_pri" (second falcon, *privileged*) | INFERRED, with three independent supports: nova `devinit.rst` ("only accessible after secure firmware lowers the privilege level to allow CPU access"), `dev_falcon_second_pri.h`, and the vendor's `gpuHandleSanityCheckRegReadError_GM107` treating `0xbadf5620` as "possible bad register read" |
| Does `PMC_ENABLE` lift it? | No: one bit set on this card (`0x40000000`), a 32-bit single-bit sweep lifts nothing. `PMC_ENABLE` is engine *enable*, and on GA102 the falcon funcs used by SEC2 have no `reset_pmc` hook at all | MEASURED + SOURCE (`ga102_sec2_flcn` has no `reset_pmc`) |
| Does the engine-reset pulse (`0x3C0`) lift it? | **Unknown, and it is cheap to test.** Third-party logs on GA102 say `CPUCTL` "reads `0xbadf5620` until the launch resets the engine", which would mean the pulse alone suffices; but those launches follow FWSEC, so the two explanations have never been separated on a card in our state | NOT ESTABLISHED — this is §6 step 3, and it is the single most valuable experiment in this document |
| Can we at least attempt the booter load blind if step 8 fails? | Yes, and it is what the vendor's flow tolerates: the SEC2 `HALTED` poll is *not* a gate on some parts (`gpu-unlock` #242 had to tolerate it), and completion is proved by `MAILBOX0 == 0`. But a blind load onto a truly write-fenced window is a long shot and must be done on a resettable card | INFERRED |

---

## 6. Implementation order, each step with its own read-back

Do these in order. Each one lands on its own; none needs the next to be interpretable.
Steps 1–2 are pure reads and parsing. Step 3 is the only write made to a fenced block
before it is known to be open.

| # | step | writes | read-back / pass condition | failure means |
|---|---|---|---|---|
| 1 | **VBIOS acquisition, read-only.** ROM BAR first (enable `0x30` bit 0, read `romlen`, disable), then PROM (`0x300000`, after clearing PCI `0x50` bit 0). Parse IFR → `rom_offset`; assert `0xAA55`. | PCI config only | `"NVGI"` or `0xAA55` at the front; `0xAA55` at `rom_offset`; `PCIR` found | wrong aperture; fix before anything else |
| 2 | **BIT + FWSEC descriptor, pure parsing.** Run §1.6 items 4–12 and log all of it. No hardware writes. | none | item 7 (an `AppID 0x85` entry) and item 8 (`V3`, size ≥44) hold | the VBIOS is not what we think; stop |
| 3 | **The fence experiment, two writes, highest information per unit of risk.** On a resettable card: `0x8403C0 ← 1`, wait 10 µs, `0x8403C0 ← 0`, then read `0x840100` and `0x841000`. | 2 writes | `0x840100 != 0xBADF5620` ⇒ the fence is reset-state, not FWSEC-gated, and §3.3 can be skipped for SEC2 purposes | still the fence word ⇒ the fence is privilege-gated; continue at step 4 |
| 4 | **FWSEC-FRTS on the GSP falcon** (§3.3, steps 1–8), with `frtsOffset = 0x5FE300000>>12`, `frtsSize = 0x100000>>12`, `type = 2`. | GSP falcon only | `0x110040 == 0` **and** `0x1438 >> 16 == 0` | FWSEC rejected the command or the image; print `0x1438` verbatim and stop (do not retry in a loop) |
| 5 | **Re-test the fence** | none | `0x840100 != 0xBADF5620` | the privilege story is wrong; go to §8 dump D4 before writing anything else |
| 6 | **GSP into RISC-V mode** | `0x111668 ← 0x111` | immediate read-back = `0x111` | the switch did not take; nothing after it can work |
| 7 | **Booter load, no start** (`docs/nv-gsp-wpr-contract.md` §4.2 steps 1–16) | SEC2 IMEM/DMEM + BROM | the register read-backs listed in §4 row 9 | a DMA/BROM programming error, caught before anything can run |
| 8 | **Start SEC2, collect the verdict** | `0x840100 ← 2` | `HALTED` ≤2 s, then `0x840040 == 0` | booter error code; log and stop |
| 9 | **GSP mailboxes + `FALCON_OS`, wait for the init RPC** | `0x110040/44`, `0x110080` | `VRPC` element, `0x1001`, `rpc_result == 0` | queue plumbing, not the booter |

Step 3 is deliberately before step 4: it costs two writes and it decides whether the
expensive step is needed at all. If it passes, the doc's central INFERRED claim is dead and
everything gets simpler — that is a good outcome, not a wasted step.

---

## 7. Where sources and measurement disagree

| # | disagreement | resolution | tag |
|---|---|---|---|
| 1 | `0x1FA824/828` as a WPR2 liveness oracle (vendor) vs `0x1FA7C8` bit 0 = 1 and both words reading as a name, not a state, on this card | prefer the measurement: use the booter's `MAILBOX0` and FWSEC's `0x1438` as the oracles, never the WPR2 pair | SOURCE vs MEASURED |
| 2 | "FWSEC-SB runs before the booter" (implied by the SB/FRTS pair in nouveau's `fwsec.c`) vs SB being teardown-only in both stacks | prefer the call sites: SB is unload, FRTS is boot | SOURCE |
| 3 | `nova` doc says FWSEC "loads other firmware ucodes onto the PMU and SEC2 … before the driver loads", which sounds like it happens at power-on; our card shows a `SLEEPING` PMU, consistent with that | consistent, not a conflict — but it means the PMU state we measured is *pre-host* state and must not be read as "waiting for us" | SOURCE + MEASURED |
| 4 | Turing PIO vs GA102 DMA for the FWSEC image | generation difference, not a conflict; GA102 = DMA | SOURCE |
| 5 | FWSEC descriptor v2 vs v3 | generation difference; GA102 documented as V3, but the code accepts both — **read the header** | SOURCE |

---

## 8. Read-only dumps that would settle the INFERRED items

All of these are reads (plus, for D1, a PCI config ROM-enable that is undone). None of them
needs a VM restart, and none changes GPU state that survives the read.

| id | what | how | expected pattern / what it settles |
|---|---|---|---|
| **D1** | **VBIOS image, 512 KiB** | ROM BAR (config `0x30` bit 0 set, read, clear) or PROM at BAR0 `0x300000` with PCI `0x50` bit 0 cleared, then restored | front = `"NVGI"`; `0xAA55` at `rom_offset`; `FF B8 42 49 54 00` with a zero-sum header; token `0x70` v2; table `01 06 06 xx`; entry `85 xx` + desc ptr; `vDesc` = `0x002C0301` (V3, 44 B) — settles §1.6 all of it, including "does this VBIOS even carry a prod FWSEC" |
| **D2** | **`0x8241C0..0x8241DF`** (fuse-version bitmask per ucode id) | 8 dwords | one bit set per live id; AND with `SignatureVersions` from D1 must be non-zero. Settles item 10 |
| **D3** | **the 0x1400 block, `0x1400..0x143F`** | 16 dwords, before and after any FWSEC run | `0x1438` should read `0x00000000` before, and `(hi16, lo16)` = `(0,0)` on success. Confirms the SB/FRTS packing — and whether that block is readable pre-boot at all is itself INFERRED, so dump 1 answers it |
| **D4** | **SEC2 window, `0x8400F0..0x84012F` and `0x841000..0x84102F`**, sampled before step 3, right after step 3, and after step 4 | 3 × 32 dwords | before: all `0xBADF5620`. After the pulse (step 3): either real values (fence = reset-state) or still the fence word (fence = privilege). After FWSEC: `0x840100 = 0x10`. This single dump decides §5's central question |
| **D5** | **`0x1100F4`, `0x11010C`, `0x110100`, `0x11012C`** on the GSP falcon, fresh | 4 dwords | `HWCFG2` bit 31/12, `DMACTL` scrub bits, `CPUCTL` halted, `CAPS2` version. Confirms the GSP is a v4 falcon in the state §3.3 assumes |
| **D6** | **PMU window, `0x10A000..0x10A0FF` + `0x10A4A0..0x10A4CF`** | 96 dwords | `UC_CTRL` bit 5 (SLEEPING), `CAPS2` v6, and whether the queue registers are non-zero (which would indicate ucode still driving a queue). Settles whether the measured PMU state is "idle firmware" or something else |
| **D7** | **`0x1FA7C8`, `0x1FA824`, `0x1FA828`** before/after step 4 | 3 dwords × 2 | documents the privilege mask and whether FRTS changes it — the cheapest possible confirmation of the privilege story, independent of SEC2 |

---

## 9. Sources read (named so each claim is checkable)

NVIDIA `open-gpu-kernel-modules`, tag **535.113.01** (the firmware version on this card):

- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp_fwsec.c` — `BIT_HEADER_V1_00`,
  `BIT_TOKEN_V1_00`, `BIT_TOKEN_FALCON_DATA` (0x70), `FALCON_UCODE_TABLE_HDR_V1` /
  `_ENTRY_V1`, `FALCON_UCODE_ENTRY_APPID_FWSEC_PROD` (0x85) / `_DBG` (0x45) /
  `FIRMWARE_SEC_LIC` (0x05), `FALCON_UCODE_DESC_V2` (60 B) / `V3` (44 B),
  `s_vbiosFindBitHeader()`, `s_vbiosParseFwsecUcodeDescFromBit()` (the
  `expansionRomOffset +` arithmetic).
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c` —
  `kgspBootstrapRiscvOSEarly_TU102`: `kflcnReset_HAL` → `kgspExecuteFwsecFrts_HAL(frtsOffset)`
  → `kflcnReset_HAL` → pre-queued `SET_SYSTEM_INFO` / `SET_REGISTRY`.
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` — the FRTS rationale comment (~2321), the SB
  call in the unload path (~2582).
- `src/common/inc/swref/published/ampere/ga102/dev_falcon_v4.h` — every falcon offset and
  bit used in §3.3 (`CPUCTL 0x100`, `BOOTVEC 0x104`, `HWCFG2 0x0F4` bits 10/12/31,
  `DMACTL 0x10C` bits 0/1/2, `DMATRF* 0x110/114/118/11C/128`, `IMEMC/D/T`, `DMEMC/D`,
  `MAILBOX0/1 0x040/044`, `OS 0x080`, `RM 0x084`).
- `src/common/inc/swref/published/ampere/ga102/dev_falcon_second_pri.h` —
  `NV_FALCON2_SEC_BASE 0x00841000`, `NV_FALCON2_GSP_BASE 0x00111000`, `MOD_SEL 0x180`,
  `BROM_CURR_UCODE_ID 0x198`, `BROM_ENGIDMASK 0x19C`, `BROM_PARAADDR(i) 0x210+4i`.
- `dev_gsp.h` — `NV_PGSP_QUEUE_HEAD(i) = 0x110C00 + i*8`, `NV_PGSP_FALCON_MAILBOX0/1`.

Linux `Documentation/gpu/nova/`:

- `core/vbios.rst` — IFR header, `RFRD`, the VBIOS image list, the `0xE0` FwSec images, the
  BIT token 0x70 → PMU lookup table → `FalconUCodeDescV3` chain, "FWSEC typically runs
  directly on the GSP".
- `core/devinit.rst` — devinit on the PMU, FWSEC on the GSP, `GFW_BOOT`, and the sentence
  about secure firmware lowering the privilege level for CPU access.

nouveau, in the Linux tree (`drivers/gpu/drm/nouveau/nvkm/`):

- `subdev/gsp/fwsec.c` — `nvkm_gsp_fwsec_init/_v2/_v3`, `nvkm_gsp_fwsec_patch` (the FRTS
  DMEM command incl. `>>12` units and `type=2`), `nvkm_gsp_fwsec_frts` (status at
  `0x001400 + 0xe*4` = `0x1438`, high half), `nvkm_gsp_fwsec_sb` (same word, low half).
- `subdev/gsp/tu102.c` — `tu102_gsp_oneinit` (FRTS size 0x100000, address rule, FRTS before
  reset), `tu102_gsp_init` (booter on `device->sec2->falcon`), `tu102_gsp_fini` (SB at
  teardown).
- `subdev/gsp/ga102.c` — `ga102_gsp_reset` (`0x1668 ← 0x111`), `ga102_gsp_fwsec_signature`
  (`0x8241C0 + (ucode_id-1)*4`, `engine_id & 0x400`).
- `engine/sec2/ga102.c` — `addr = 0x840000`, `ga102_sec2_flcn` (no `reset_pmc`).
- `subdev/acr/acr.c`, `acr/gp102.c`, `acr/ga102.c` — `ga102_acr_new()` returns `-ENODEV`
  under GSP-RM; the ACR HSF runs on SEC2 (gp102) / is unused (ga102).
- `subdev/pmu/gp102.c` — `gp102_pmu` has no firmware and `-ENODEV` under GSP-RM; the
  `cmdq/msgq` register tuples.
- `falcon/gm200.c`, `falcon/ga102.c`, `falcon/gp102.c`, `falcon/base.c` —
  `gm200_flcn_fw_boot` (mailbox prefill, `0x104`, `0x100←2`, 2 s halt, `mbox0` check),
  `ga102_flcn_fw_load` (DMA, `0x624` bit 7, `0x10C=0`, `0x600`), `gp102_flcn_reset_eng`
  (`0x3C0` pulse), `ga102_flcn_reset_prep` / `_reset_wait_mem_scrubbing`.
- `subdev/bios/{base,bit,pmu,shadowrom,shadowpci}.c` — BIT scan key `FF B8 "BIT"`,
  `bit_entry()`, `nvbios_pmuEp/Rm`, IFR walk, PROM window, `nvkm_pci_rom_shadow` (PCI `0x50`
  bit 0).

envytools (register/tooling documentation, not driver code):

- `docs/nvrm/pmu/ucode-cmds.rst` — the `0x10A040`/`0x10A044` scratch command interface, its
  statuses and error table (390-era ucode, i.e. explicitly *not* an AD102 contract).
- `rnndb/falcon.xml` — `uc_cpuctl` bit meanings (`STOPPED` 4, `SLEEPING` 5), `XFER_CTRL`,
  `CAPS2`.
- `docs/hw/falcon/proc.rst` — the RUNNING/STOPPED/SLEEPING state machine.

Local, measured:

- `docs/nvidia-driver.md` §5.7 (the fence measurement, `PMC_ENABLE = 0x40000000`).
- `docs/nv-gsp-reference.md` and `docs/nv-gsp-reference-evidence/` (the same card under the
  vendor driver: `lspci -vvv` showing the disabled virtual option ROM, `nvidia-smi`).
- `docs/nv-gsp-wpr-contract.md` §3.2 (our WPR2 layout, `frts @ 0x5FE300000`, 1 MiB), §4.2
  (the SEC2 register sequence this doc builds on), §4.4 (what the boot ROM validates).

Third-party reverse-engineering, used only for the "what does `0xbadf5620`/`0x10` look like
in practice" rows and clearly marked as such: `Zagitta/gpu-unlock` issue #242 comments
(GA102: "SEC2 `CPUCTL` reads `0xbadf5620` until the launch resets the engine"; Turing: SEC2
`CPUCTL` = `0x10` after reset; `0x1FA7CC` fenced pre-booter).
