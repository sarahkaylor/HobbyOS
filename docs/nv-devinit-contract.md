# The devinit (golden-register) contract on AD102 — where it is in *this* VBIOS, and why this card has none to replay

Scope: settles the question left open by `docs/nv-vbios-map.md` §9 — whether the RTX 4090's VBIOS
contains a devinit register-write script the driver could replay to open the fenced
`0x110000`/`0x840000` windows — from the card's own bytes, plus the public format specification and
the two reference implementations (nouveau, NVIDIA `open-gpu-kernel-modules`).

The one-line answer, stated up front because it changes the plan:

> **There is no devinit table to replay on this card.** BIT token `0x44` (`'D'`) is not devinit at
> all — it is `BIT_TOKEN_DFP_PTRS` (flat-panel/DFP data), which is why its payload led nowhere. The
> devinit pointer token is `0x49` (`'I'`, `BIT_TOKEN_NVINIT_PTRS`), and on this VBIOS its **init
> script table is empty**: the table it points at holds two NULL entries. On Ampere, devinit is not a
> VBIOS register script executed by the driver — it is a *ucode image* (an application in the `0xE0`
> ROM partition) that the PMU runs, launched by FWSEC, before any driver exists. Consistently, **no
> dword anywhere in the 1 MiB dump equals `0x00110600` or `0x00840000`**, so nothing in this ROM
> writes the falcon/SEC2 windows even by indirection.

Second correction, about method rather than bytes: **the read-back the fence hypothesis was built on
cannot show what it was asked to show.** `0x110600`, `0x110624` and `0x11010C` are inside the
privilege-fenced half of the falcon page, so they return `0xBADF5620` *before and after* a write
regardless of whether the write landed (MEASURED, `docs/nv-gsp-reference-evidence/`). §8 gives the
one register in the same window whose read-back *is* meaningful.

| tag | meaning |
|---|---|
| **MEASURED** | read from this card: the 1 MiB dump in `docs/nv-vbios-evidence/` (sha256 `7dfb34…b863`), or the register dumps in `docs/nv-gsp-reference-evidence/` |
| **SOURCE** | stated by a named public artefact: NVIDIA's *BIOS Information Table* spec, nouveau `nvkm/subdev/bios/*`, nouveau `nvkm/subdev/devinit/*`, kernel `Documentation/gpu/nova/core/*.rst`, NVIDIA `open-gpu-kernel-modules` headers |
| **INFERRED** | my reading of the above; consistent with the bytes, not independently confirmed |
| **NOT ESTABLISHED** | no public source, no measurement. Not guessed |

No code from any driver was copied into this tree; the driver's own helpers (`vb_byte/vb_word/vb_dword`,
`nv_vbios_probe`) are the only code referenced, and only by name.

---

## 0. What the previous attempt hit, stated precisely

| # | claim | tag |
|---|---|---|
| 0.1 | `0x110000-0x110140` is **partly** readable on an uninitialised card: 60 of the first 256 dwords answer real values, the rest answer `0xBADF5620`. `0x110100` (`UC_CTRL`) reads `0x10` (halted/stopped) in both the pre-init and post-init dumps | MEASURED |
| 0.2 | `0x110600`, `0x110624`, `0x11010C` are *not* in that readable set: all three read `0xBADF5620` pre-init and real values (`0x110`, `0x110`, `0x1`) post-init | MEASURED |
| 0.3 | `0xBADF5620` is the hardware's "this aperture is not accessible to you" answer, not data; NVIDIA's own sanity checker treats it as such | SOURCE (`gpuHandleSanityCheckRegReadError_GM107`) |
| 0.4 | therefore "wrote 5, read back `0xBADF5620`" is consistent with three different worlds: write dropped, write landed but read fenced, register ignores writes | logical consequence of 0.1-0.3 |
| 0.5 | after the fence lifts (`0x110600` post-init) the register does **not** read back what was written: it reads `0x110`, which is a configuration word, not a store | MEASURED |
| 0.6 | on this card a full 32-bit `PMC_ENABLE` sweep lifts nothing (`PMC_ENABLE` = `0x40000000` = bit 30 = `PDISPLAY` only) | MEASURED (parent lane) + SOURCE for the bit name (`rnndb/bus/pmc.xml`) |

So the fence is real and per-register, and the hypothesis "devinit has not run, so replay it" was a
reasonable next guess. §1 kills the "replay" half of it; §8 replaces the unusable test.

---

## 1. Where devinit actually is in this dump

### 1.1 Token `0x44` ('D') is DFP data, not devinit

NVIDIA's BIT specification lists the token ids; `0x44` is `BIT_TOKEN_DFP_PTRS` ("DFP/Panel Data
Pointers"), and `0x49` is `BIT_TOKEN_NVINIT_PTRS` ("Initialization Table Pointers"). nouveau reads the
same token (`bit_entry(bios, 'I', …)` in `nvkm/subdev/bios/bit.c`, used as the init-table handle by
`init_table()` in `nvkm/subdev/bios/init.c`). Source: BIT spec §"BIT Tokens"; nouveau `bit_entry()`.

Measured, this dump — BIT header at `0x1B0`, 12 bytes, stride 6, 19 tokens; tokens start `0x1BC`:

| token | entry bytes at | id | ver | data size | data ptr | payload at ptr |
|---|---|---|---|---|---|---|
| `'D'` = `0x44` | `0x1CE` | `44` | `01` | `0004` | `029B` | `FE 51 00 00` |
| `'I'` = `0x49` | `0x1D4` | `49` | `01` | `0024` (36) | `029F` | `9E 40 00 00 00 00 A2 40 00 00 0E 41 …` |

`0x44`'s four bytes are `BIT_DFP_PTRS`: two `u16` sub-table pointers, `0x51FE` and `0x0000`. `0x51FE`
begins `10 0B D0 B4 20 90 31 10 …` — a table whose first two bytes are a **`0x10` version byte plus a
length byte**, which is exactly the shape every devinit *data* sub-table in this ROM uses (§1.5). That
is why `0x51FE` looked like a devinit header and why parsing a v1 script there could only fail: it is a
panel/DFP table, and panel tables and devinit data tables share that two-byte prefix.

**Conclusion:** the `'D'` convention needed no resolving — `'D'` is the wrong token. Nothing about
`0x51FE` is devinit.

### 1.2 Token `0x49` ('I') and the `BIT_NVINIT_PTRS` block at `0x29F`

`BIT_NVINIT_PTRS` (BIT spec) is a run of `u16` pointers, in this order:

| slot | field (spec name) | this dump | note |
|---|---|---|---|
| 0 | Init Script Table pointer | **`0x409E`** | table of per-index script offsets |
| 1 | Macro Index Table pointer | `0x0000` | absent |
| 2 | Macro Table pointer | `0x0000` | absent |
| 3 | Condition Table pointer | `0x40A2` | 12-byte records, verified below |
| 4 | I/O Condition Table pointer | `0x0000` | absent |
| 5 | I/O Flag Condition Table pointer | `0x410E` | |
| 6 | Init Function Table pointer | `0x40A2` | same target as slot 3 |
| 7 | VBIOS Private Boot Script pointer | `0x0000` | **absent** |
| 8 | Data Arrays Table pointer | `0x410E` | same target as slot 5 |
| 9 | PCIe Settings Script pointer | `0x0000` | **absent** |
| 10 | Devinit Tables pointer | `0x404A` | data segment, see §1.5 |
| 11 | Devinit Tables size | `0x0FB6` | `0x404A + 0x0FB6 = 0x5000` |
| 12 | Boot Scripts pointer | `0x0000` | **absent** |
| 13 | Boot Scripts size | `0x0000` | |
| 14 | NVLink Configuration Data pointer | `0x7B63` | inside image 0 |
| 15 | Boot Scripts Non-GC6 pointer | `0x0000` | **absent** |
| 16 | Boot Scripts Non-GC6 size | `0x0000` | |
| (17) | unused | `0x0000` | the token declares 36 bytes = 18 `u16`; the spec defines 17 |

Byte arithmetic and two self-checks that the base is right:

```
BIT header 0x1B0, hdr 12, stride 6  →  token 'I' entry at 0x1B0+12+4*6 = 0x1D4   (49 01 24 00 9F 02)
data pointer read at entry+4        →  0x029F
block                                 0x29F .. 0x2C2  (36 bytes, 18 u16)
0x29F + 36                          →  0x2C3  = the data pointer of token 'M' (0x4D)  ← exact fit
```

The exact fit against the *next* token's payload pointer (`0x4D` → `0x2C3`) is the structural
confirmation that `0x29F` is the block's base; there is no padding to argue about. First bytes, verbatim:

```
0029f  9e 40 00 00 00 00 a2 40 00 00 0e 41 a2 40 00 00 0e 41 00 00 4a 40 b6 0f 00 00 00 00 63 7b 00 00 00 00 00 00
        \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/ \__ __/
        0x409E  0x0000  0x0000  0x40A2  0x0000  0x410E  0x40A2  0x0000  0x410E  0x0000  0x404A  0x0FB6  0x0000 …
```

Two slots share a target (3≡6 at `0x40A2`, 5≡8 at `0x410E`). That is either a shared table or a
one-field difference from the newest published field order; **NOT ESTABLISHED** which. It does not
affect the answer, because the load-bearing slots — 0 (init script), 12 (boot scripts), 15 (non-GC6
boot scripts) and 7 (private boot script) — are `0x409E`, `0`, `0`, `0`, and *every* other slot value
was also tested as a possible script table (§1.4).

Slot 8 is worth a second look because it identifies itself: nouveau reads the xlat table at byte
`0x10` of this block, i.e. slot 8 = `0x410E`, and `0x410E` holds `00 01 02 03 04 05 06 07` twice — a
16-entry byte translation table, which is exactly what opcode `0x96` indexes into. So the block base
`0x29F` is confirmed by *content* (a clean index map), not only by the `0x2C3` fit.

### 1.3 The init script table at `0x409E` is empty

`init_script(bios, i)` in nouveau (`nvkm/subdev/bios/init.c`) reads `u16` at `script_table + 2*i` and
runs that as an opcode stream; the walk stops at the first NULL. Measured:

| read | value |
|---|---|
| `u16 @ 0x409E` (entry 0 = the boot script) | `0x0000` |
| `u16 @ 0x40A0` (entry 1) | `0x0000` |
| `u16 @ 0x40A2` | `0xC030` — but `0x40A2` is the **condition table**, not entry 2 |

`0x409E` is thus the two-NULL table immediately followed by the condition table. Script count = 0.
There is nothing for `nvbios_post()` to execute on this ROM, and that is normal for Turing and later:
nouveau's devinit for `TU102`/`GA100` (inherited by `GA102`) never calls `nvbios_post()` at all —
`tu102_devinit_post()` only polls `0x118128` bit 0 and `0x118234 & 0xff == 0xff` and returns
(`nvkm/subdev/devinit/tu102.c`). On earlier generations the same hook did run the tables
(`gm200_devinit_post` → `nvbios_post`), which is where the expectation comes from.

### 1.4 No slot in the block points at an opcode stream

Every non-NULL value in the block was tried as a script table. For each, entry 0 / 1 / 2 as `u16`
script offsets, and the opcode byte found there:

| base tried | entry values | first opcode byte | verdict |
|---|---|---|---|
| `0x409E` (slot 0) | `0x0000`, `0x0000`, `0xC030` | — / — / `0x00` | NULL,NULL → **empty** |
| `0x40A2` (slot 3/6) | `0xC030`, `0x4061`, `0x0000` | `0x00`, `0x00` | not a script table (condition records) |
| `0x410E` (slot 5/8) | `0x0100`, `0x0302`, `0x0504` | `0x00`, `0x07`, `0xE9` | not a script table (strap index bytes) |
| `0x41FC` | `0x0710`, `0x1016`, `0x6300` | `0x56`, `0x9B`, `0xC1` | not a script table (memory info block) |
| `0x404A` (slot 10) | `0x0001`, `0x0010`, `0x0F86` | `0xAA`, `0x44`, `0x59` | a segment **descriptor**, not a script |
| `0x7B63` (slot 14) | `0x0802`, `0x0101`, `0x0407` | `0xC6`, `0x00`, `0x00` | not a script table |

No value yields a byte that is a legal devinit opcode. Opcodes below `0x32` do not exist in the
instruction set (§2.4), so a parser is right to reject all of these — and that is the same test that
rejected the parent's `0x51FE`. Independently: `0x00101000` (the strap register that every devinit
script on every generation reads for RAM configuration) appears **zero** times in the whole 1 MiB.

### 1.5 What the `0x404A` segment actually is: opcode *data*

`0x404A`, size `0x0FB6` → `0x404A..0x4FFF`. It opens with a 24-byte descriptor-ish prefix and then a
`u32` directory at `0x4062`:

```
404a  01 00 10 00 86 0f 30 00 03 00 84 26 00 00 00 00 01 00 44 00 32 0f 00 00
4062  4a 40 00 00 | fc 41 00 00 | a2 40 00 00 | 00 00 00 00 | 0e 41 00 00 | 1e 41 00 00 | 07 44 00 00 | 83 43 00 00
      0x404A      | 0x41FC      | 0x40A2      | (skip)      | 0x410E      | 0x411E      | 0x4407      | 0x4383
      …            | 0x442B      | 0x4443      | 0x472B      | 0x4595      | 0x4797      | 0x489B      | 0x4A1F | 0
```

All directory entries land inside the declared segment. Every one of those sub-tables starts with the
same two-byte prefix `10 <len>` (12 of them: `0x4184 10 09`, `0x41FC 10 07`, `0x4373 10 11`,
`0x4383 10 04`, `0x442B 10 05`, `0x4445 10 01`, `0x4595 10 0D`, `0x472B 10 03`, `0x473C 10 0D`,
`0x4797 10 04`, `0x489B 10 04`, `0x4A1F 10 05`). A `10 xx` prefix followed by rows of bytes is a
*record table*: for example `0x4595` (`10 0D`, 13-byte rows beginning `34 …`) and `0x4797`
(`10 04`, rows of `u16` constants). None of them is an instruction stream, which matches the spec's
own description of slot 10 as "the contiguous segment containing tables **required by** devinit
opcodes".

The first `u32` of the segment (`0x00100001` = `u16` 1, `u16` 0x10) and the `0x30`/`0x2684` words read
like a `{version, header length, … , device id}` descriptor; decoding it further is **NOT ESTABLISHED**
and is not needed, since no slot points at a script.

### 1.6 Where devinit *does* live on Ampere: the `0xE0` partition, as ucode

| item | statement | tag |
|---|---|---|
| devinit is an interpreter + a "script" that runs on the **PMU** in light-secure mode, launched by FWSEC (GSP, heavy-secure), before the driver loads | kernel `Documentation/gpu/nova/core/devinit.rst` | SOURCE |
| the devinit ucode is *itself* another ucode stored in the same `0xE0` ROM partition as FWSEC | same doc + `fwsec.rst` ("The DEVINIT ucode is itself another ucode that is stored in this ROM partition") | SOURCE |
| nova's FWSEC layout diagrams label an application-interface entry "DEVINIT (ID 1)" with a DMEM offset | `fwsec.rst` (patch text in the nova documentation series) | SOURCE |
| this ROM's ucode table entry `[0]` is `app 0x01 / target 0x01 → 0x15C54` (logical) → flash `0x2AA54`, whose first bytes are `0C 91 00 00 0C 91 00 00 00 00 00 00 F4 00 00 00 …` | dump | MEASURED |
| that entry is the devinit image | **not claimed.** The lookup-table `app_id` space (`0x85` = FWSEC there) and the DMEM app-interface `id` space are different namespaces; nothing measured ties entry `0x01` to devinit | NOT ESTABLISHED |

Consequence for the driver: devinit's *effects* are produced by ucode running on a processor the host
does not drive, and its *inputs* (the `10 xx` tables at `0x404A`) are consumed by that interpreter. A
host-side replayer would have to (a) find the script, which does not exist in plaintext here, and
(b) already own the PMU, which it does not.

---

## 2. The table format, for whoever has to verify this (or meets another VBIOS)

Kept because the next VBIOS revision, or another board, may carry a script — and because these are the
rules that let a reader check §1 without trusting me.

### 2.1 Structure chain

```
BIT header (12 bytes, byte-sum 0)         → token list (stride 6: id, ver, size u16, ptr u16)
token 'I' data (BIT_NVINIT_PTRS)         → 17 u16 slots, absolute offsets inside the ROM image
slot 0 (init script table)               → array of u16 script offsets; NULL terminates the walk
slot 3 (condition table)                 → 12-byte records
slot 4 (I/O condition table)             → 5-byte records
slot 5 (I/O flag condition table)        → 9-byte records
slot 10/11 (devinit data segment)        → opaque `10 xx`-prefixed record tables
slot 12/15 (boot scripts)                → array of u16 script offsets (both NULL here)
```

All of these pointers are in the VBIOS's logical address space, i.e. the rule from
`docs/nv-vbios-map.md` §4 applies (`flash = logical + 0x14E00` for `logical >= 0x0FC00`). Every pointer
in this ROM's block is below `0x0FC00`, so no shift is applied to any of §1's values; a pointer that
lands `>= 0x0FC00` must be shifted before use.

### 2.2 Conditions: encoding, evaluation, and which ones matter

| kind | record | evaluation | source |
|---|---|---|---|
| `INIT_CONDITION` (opcode `0x75`, operand = 1-byte index) | 12 bytes: `reg u32`, `mask u32`, `value u32` at `cond_table + 12*idx` | read `reg` through the MMIO mapper, keep `mask`, compare to `value` | `init_condition_met()` |
| `INIT_IO_CONDITION` (`0x76`) | 5 bytes: `port u16`, `index u8`, `mask u8`, `value u8` | read the VGA indexed port, compare masked byte | `init_io_condition_met()` |
| `INIT_IO_FLAG_CONDITION` (`0x39`) | 9 bytes: `port u16`, `index u8`, `mask u8`, `shift u8`, `table u16`, `dmask u8`, `value u8` | read port, shift+mask to an index, look up a byte in `table`, compare | `init_io_flag_condition_met()` |
| `INIT_STRAP_CONDITION` (`0x73`) | inline 8 bytes: `mask u32`, `value u32` | `(rd32(0x101000) & mask) == value` | `init_strap_condition()` |
| `INIT_RAM_CONDITION` (`0x6d`) | inline 2 bytes: `mask u8`, `value u8` | `(rd32(0x100000) & mask) == value` | `init_ram_condition()` |

Semantics of a failed condition: execution is **suppressed, not skipped** — the interpreter clears its
"execute" flag (`init_exec_set(false)`) and keeps decoding until `INIT_RESUME` (`0x72`) or the end of
the enclosing block. A replayer must therefore parse every opcode even when not executing it, or it
will lose its place. Which ones matter for a bare-metal boot: the two that read **fuses/straps**
(`0x101000`, `0x100000`) and the two board-variant ones (`0x73`, `0x6d`) — they decide RAM
configuration and board straps. All four are absent from this ROM by construction: `0x00101000` has
**0** hits in 1 MiB and `0x00100000` has 143, only two of which are inside image 0 and both are
coincidental (`0x1EAF`, and `0x40A7` = a one-byte-shifted read of a condition mask).

Measured condition table (4 of the records at `0x40A2`), decoded:

| idx | reg | mask | value | reads as |
|---|---|---|---|---|
| 0 | `0x4061C030` | `0x10000000` | `0x00000000` | display register with OR+link mangle bits (`0x40…`), test bit 28 clear |
| 1 | `0x00612308` | `0x00000080` | `0x00000080` | bit 7 set |
| 2 | `0x00612388` | `0x00000080` | `0x00000080` | bit 7 set |
| 3 | `0x00612408` | `0x00000080` | `0x00000080` | bit 7 set |

Records 1-3 sitting 0x80 apart in `0x00612xxx` is the shape of a display/SOR-condition list, which is
what a Turings-era VBIOS keeps these tables for even when it has no scripts.

### 2.3 Macro and xlat tables

* **Macro** (`0x6f`): operand is a 1-byte index; the entry at `macro_table + 8*index` is `{addr u32,
  value u32}` and is written unconditionally (`init_macro()`). Slot 2 is `0` here → opcode `0x6f`
  would be a hard error.
* **Xlat** (`0x96`): operand is `saddr u32, sshift u8, smask u8, xlat_index u8, daddr u32, dmask u32,
  shift u8` (17 bytes); the source register is shifted+masked to a byte index, that byte is read from
  `xlat_table[xlat_index] + index`, shifted, and merged into `daddr` under `dmask` (`init_xlat()`,
  `init_xlat_()`). The xlat pointer lives at **byte `0x10` of the block** (slot 8 — the newest BIT spec
  calls this slot "Data Arrays Table"), and here it is `0x410E`, whose 16 bytes are exactly a byte
  translation table (`00 01 02 03 04 05 06 07 00 01 02 03 04 05 06 07`). That is a fourth independent
  confirmation that `0x29F` is the block base: a wrongly based read would not produce a clean `0..7`
  index map. The same `0x410E` is also cited as the memory strap translation table by token `0x4D`
  (`'M'`, `BIT_MEMORY_PTRS` v2, count `0x0E`), so the two tables are genuinely shared on this ROM.

### 2.4 Opcode (token) encoding — every instruction type, with layouts

The instruction set is a byte stream; the opcode byte selects the handler and the handler fixes the
operand length. **Opcodes below `0x32` and every value not listed are invalid** and must abort the
parse. Operand offsets are relative to the opcode byte.

Register writes — all MMIO, all absolute (see §4):

| op | name (nouveau handler) | operands | length | effect |
|---|---|---|---|---|
| `0x7A` | `init_zm_reg` | `addr u32`, `data u32` | 9 | `R[addr] = data` |
| `0x6E` | `init_nv_reg` | `addr u32`, `mask u32`, `data u32` | 13 | `R[addr] = (R[addr] & mask) | data` — *keep* `mask` bits, force `data` bits |
| `0x77` | `init_zm_reg16` | `addr u32`, `data u16` | 7 | `R[addr] = data` (16-bit value, 32-bit store) |
| `0x91` | `init_zm_reg_group` | `addr u32`, `count u8`, `count × u32` | 6+4n | n sequential dwords all written to the **same** `addr` (a FIFO-style port), value advancing through the operand list |
| `0x58` | `init_zm_reg_sequence` | `base u32`, `count u8`, `count × u32` | 6+4n | same, `base + 4i` |
| `0x47` | `init_andn_reg` | `addr u32`, `mask u32` | 9 | `R[addr] &= ~mask` |
| `0x48` | `init_or_reg` | `addr u32`, `mask u32` | 9 | `R[addr] |= mask` |
| `0x97` | `init_zm_mask_add` | `addr u32`, `mask u32`, `add u32` | 13 | `(R & mask) | ((R + add) & ~mask)` |
| `0x96` | `init_xlat` | see §2.3 | 17 | translated register modify |
| `0x6F` | `init_macro` | `index u8` | 2 | write from macro table |
| `0x90` | `init_copy_zm_reg` | `src u32`, `dst u32` | 9 | `R[dst] = R[src]` |

Conditionals and polls (no side effect on the device; they gate execution):

| op | name | operands | length | notes |
|---|---|---|---|---|
| `0x75` | `init_condition` | `idx u8` | 2 | 12-byte record lookup |
| `0x76` | `init_io_condition` | `idx u8` | 2 | 5-byte record lookup |
| `0x39` | `init_io_flag_condition` | `idx u8` | 2 | 9-byte record lookup |
| `0x3A` | `init_generic_condition` | `idx u8` | 2 | generic table |
| `0x73` | `init_strap_condition` | `mask u32`, `value u32` | 9 | `0x101000` |
| `0x6D` | `init_ram_condition` | `mask u8`, `value u8` | 3 | `0x100000` |
| `0x38` | `init_not` | — | 1 | inverts the execute flag |

Delays: `0x74` `init_time` (`u16` µs, 3 bytes; ≥1000 → ms), `0x57` `init_ltime` (`u16` ms, 3 bytes),
`0x56` `init_condition_time` (`cond u8`, `retry u8`, 3 bytes — poll a condition-table entry with a
bounded number of 20 ms retries, then suppress execution).

Flow: `0x71` `init_done` (end of script → stop), `0x72` `init_resume` (re-enable execution),
`0x5C` `init_jump` (`u16` absolute target, 3 bytes), `0x33` `init_repeat` (`count u8`) with
`0x36` `init_end_repeat` (loop body delimited, not counted), `0x6B` `init_sub`, `0x5B`
`init_sub_direct`, `0x8C`/`0x8D` `init_reset_begun`/`init_reset_end` (markers around a reset),
`0x92`/`0xAA` reserved/no-op.

Index-based and port I/O — **not MMIO**, so irrelevant to the falcon question but part of the format:
`0x78` `init_index_io` (`port u16`, `index u8`, `mask u8`, `data u8`; 6 bytes; read-modify-write
through the VGA indexed port pair), `0x62` `init_zm_index_io`, `0x69` `init_io`, `0x32`
`init_io_restrict_prog`, `0x34`/`0x4A` `init_io_restrict_pll{,2}`, `0x3B`/`0x3C` `init_io_mask_or`/
`init_io_or`, `0x49` `init_idx_addr_latched`, `0x52`/`0x53`/`0x54` `init_cr`/`zm_cr`/`zm_cr_group`,
`0x51` `init_cr_idx_adr_latch`. These touch CRTC/attribute/DAC registers and DAC-attached I²C.

I²C / AUX / sideband: `0x4C` `init_i2c_byte`, `0x4D` `init_zm_i2c_byte`, `0x4E` `init_zm_i2c`,
`0x5E` `init_i2c_if`, `0x9A` `init_i2c_long_if`, `0x98`/`0x99` `init_{,zm_}auxch`, `0x8E`
`init_gpio`, `0xA9` `init_gpio_ne`, `0x4F`/`0x50` `init_tmds`/`init_zm_tmds_group`.

PLL and memory/clock programming (the genuinely dangerous ones): `0x79` `init_pll` (`reg u32`,
`freq u16` × 10 kHz, 7 bytes), `0x4B` `init_pll2`, `0x59` `init_pll_indirect`, `0x87`
`init_ram_restrict_pll`, `0x63` `init_compute_mem`, `0x66` `init_configure_mem`, `0x67`
`init_configure_clk`, `0x68` `init_configure_preinit`, `0x65` `init_reset` (`reg u32`, `data1 u32`,
`data2 u32`, 13 bytes — writes `data1`, waits 10 µs, writes `data2`, around a PCI `0x184C`/`0x1850`
side-dance), `0x8F` `init_ram_restrict_zm_reg_group`.

A parser should implement the length table for **all** opcodes even when it intends to execute only a
subset, because unknown opcodes and wrong lengths are indistinguishable from data.

---

## 3. What the driver must have done before — and after — "replaying devinit"

Since there is nothing to replay, this section is the honest version: the preconditions for touching
the window at all, and what must follow instead.

| # | precondition | why | tag |
|---|---|---|---|
| 1 | BAR0 mapped, 16 MiB, at the address the kernel assigned; `resource0` size is the authority | everything below is BAR0-relative | MEASURED (`lspci -vvv`: `Region 0 … [size=16M]`) |
| 2 | PROM shadow off before reading the flash: PCI config `0x50` bit 0 ← 0, restored afterwards | otherwise the aperture shows a stale RAM copy | SOURCE (`nvkm_pci_rom_shadow`) |
| 3 | The image chain walked and `shift` computed before *any* VBIOS pointer is dereferenced | pointers address the logical image | SOURCE + MEASURED (`docs/nv-vbios-map.md` §4) |
| 4 | `PMC_ENABLE` read and logged (`0x40000000` = `PDISPLAY` only on this card) | it is the only "engine enable" state a driver can see; it is *not* the fence | MEASURED |
| 5 | Recognise that on TU102/GA100 the devinit hook is a **poll**, not a script run: `0x118128` bit 0 and `0x118234 & 0xff == 0xff` | this is what "devinit has finished" means to the only open driver that supports the part | SOURCE (`tu102_devinit_wait`) |
| 6 | Know the status of the x86 devinit heuristic: VGA CRTC index `0x00` and `0x1A` both zero ⇒ "adaptor not initialised" (i.e. the option ROM's devinit never ran on this card) | the compiler of this heuristic is nouveau itself, and it is the *only* public way to ask the question without writing | SOURCE (`nv50_devinit_preinit`) |
| 7 | **Do not** write the `0x404A..0x4FFF` segment, do not treat `0x51FE` as a table, and do not add a `devinit_replay()` path | §1: no script exists; those bytes are interpreter input, and writing them back as registers would map arbitrary constants onto `0x0xxxxx` registers | INFERRED from §1 |

After: nothing to unwind, because nothing was written. What must happen *instead* of a replay is the
FWSEC launch already specified in `docs/nv-pmu-acr-contract.md` §3.3 (GSP falcon: reset → DMA config →
IMEM/DMEM load → DMEM boot descriptor → `0x040 ← 0`, `0x104 ← 0`, `0x100 ← 2` → wait `HALTED` →
`0x040 == 0` **and** `0x1438 >> 16 == 0`), then the privilege-drop gate `0x840100 != 0xBADF5620`,
then the SEC2 booter. devinit is what FWSEC's child does on the PMU; it is not a host step.

---

## 4. How a register write in devinit is addressed

Absolute MMIO offset, 32-bit, BAR0-relative — with three mangling bits that are display-only:

| step | rule | source |
|---|---|---|
| 1 | `reg &= ~0x3` — low two bits select I/O-port access on ancient parts; the binary driver always uses the NV register address | `init_nvreg()` |
| 2 | if `reg` bit 31 set: `reg += head*0x800`, clear bit 31 (per-CRTC register block) | same |
| 3 | if `reg` bit 30 set: `reg += or*0x800`, clear bit 30; and if then bit 29 set: `reg += link*0x80`, clear bit 29 (per-output/per-link) | same |
| 4 | any remaining bit above `0x00FFFFFC` is bogus → warn (do not write) | same |
| 5 | pass through the chip's MMIO mapper; **for TU102/GA100/GA102 no mapper is installed** (`tu102_devinit`, `ga100_devinit` func tables have no `.mmio` hook), so the address is used verbatim | `nvkm_devinit_mmio()` + the two func tables |

Worked example on this card: a `0x7A` with `addr = 0x00110600` lands at BAR0 `0x110600` — the GSP
falcon's DMA-port register, i.e. inside the `NV_PGSP` aperture (`0x113FFF:0x110000`, NVIDIA
`dev_gsp.h`), 0x600 into the primary falcon block. `0x00840000` would land on the SEC aperture
(`NV_PSEC` `0x843FFF:0x840000`, `dev_sec_pri.h`). Nothing engine-relative, nothing indexed: devinit
addresses are flat. The only indexed forms are the *data* tables (`0x91`, `0x58`, `0x8F`), which vary
the *address* arithmetically from a base, and `0x96`/`0x90`, which use the xlat table — none of which
can reach `0x11xxxx` here, because their base constants would have to appear in the ROM and `0x00110000`
appears **nowhere in image 0** (six hits exist in the whole 1 MiB, all at `0x7EEB4` or above, inside
the `0xE0` partition's data).

---

## 5. The complete list of this card's devinit writes into the falcon/GSP/SEC2 windows

**Empty.** Both windows, no writes, in order or otherwise — because there is no script. The negative rests
on three independent checks, all repeatable against the committed dump:

| check | method | result |
|---|---|---|
| declared scripts | `u16 @ 0x409E` (init script table entry 0), `slot 12`, `slot 15`, `slot 7` | `0`, `0`, `0`, `0` — nothing to execute |
| any write to `0x110600` | every 4-byte-aligned dword in the 1 MiB compared against `0x00110600`: opcode `0x7A`/`0x6E`/`0x77`/`0x47`/`0x48`/`0x97` all carry the address literally | **0 hits** |
| any write to `0x840000` / `0x840100` | same scan for `0x00840000` / `0x00840100` | **0 hits** |
| any base that could reach the GSP window | same scan for `0x00110000` | 6 hits, **none inside image 0**; all inside the `0xE0` partition's payload area (`0x7EEB4`, `0x7EEF8`, `0x7F56B`, `0x7F6EB`, `0x81A9F`, `0x94B59`), each surrounded by unrelated bytes (`00 00 00 00 … 00 11 00 … 00 00 00 00`) — coincidental |
| any strap read at all (`0x00101000`) | same scan | **0 hits** |

The residual caveat, stated so nobody over-reads the table: a write whose address is *computed*
(`0x91` from a base, `0x96` through xlat) needs its base in the ROM, which is covered by the same scan
for `0x00110000`; a write inside the signed ucode image in the `0xE0` partition would not be visible as
plaintext, and the PMU-resident devinit ucode is exactly that case (**NOT ESTABLISHED** what it writes —
it is not host-replayable either way).

So the "handful of writes to test the fence with" does not exist. The right handful is §8's one write,
or the FWSEC launch sequence, which is host code and already written down.

---

## 6. Risk: what a blind replay would have touched, and how to bound an attempt

Even though this ROM has no script, the risk list matters the moment anyone wires a generic
"replay the golden tables" path into the driver, because it will run on some other card's ROM:

| class | opcodes | why unsafe on a live card |
|---|---|---|
| display pipeline | `0x49`, `0x51`-`0x54`, `0x78`, `0x62`, `0x4F`, `0x50` | CRTC/attribute/DAC state; can blank or corrupt the console the driver is running on |
| straps / RAM config | `0x73`, `0x6D`, `0x63`, `0x66`, `0x8F`, `0x87` | memory-controller timings; a wrong value wedges the FB and needs a full reset, not a retry |
| clocks | `0x79`, `0x4B`, `0x59`, `0x34`, `0x4A`, `0x67` | PLL programming with a bad divider can over-clock or hang the link |
| reset | `0x65`, `0x8C`, `0x8D` | writes `data1`, waits 10 µs, writes `data2`, and pokes PCI `0x184C`/`0x1850`; a mistimed pulse is unrecoverable without a host reset |
| PMU-visible state | anything in the `0x10A000`/`0x118000` blocks | the PMU owns those; the host writing them races the firmware |
| I²C / AUX | `0x4C`-`0x4E`, `0x5E`, `0x98`-`0x9A` | talks to the board's regulators and retimers; a wrong write is a hardware event, not a register error |

Bounding rule for a first attempt, if a future ROM *does* carry scripts: execute only instructions whose
decoded address is inside `0x110000-0x113FFF` or `0x840000-0x843FFF`, treat every other opcode as
"skip" **while still advancing the offset by its length**, and refuse to execute any `0x65`/`0x67`/
`0x66`/`0x73`/`0x6D` at all. Never execute a script at all on a card that is serving a display without
a "this will blank the screen" acknowledgement.

---

## 7. Implementation order, each step with a read-back

Steps 1-2 are pure reads and are the whole "devinit" deliverable. Step 3 is the experiment that decides
the real question. Nothing here writes a VBIOS-sourced value to a register.

| # | step | writes | read-back / pass condition | on failure |
|---|---|---|---|---|
| 1 | **Parse token `0x49`** in `nv_vbios_probe()` (new `vb_nvinit()` beside `vb_byte/vb_word/vb_dword`): walk the token list for id `0x49`, read its 36-byte payload at `0x29F`. Log all 17 slots. | none | entry bytes are `49 01 24 00 9F 02`; `0x29F + size == ` the next token's payload offset (`0x2C3`) | the BIT geometry is not what §1.1 says; stop, do not guess an offset |
| 2 | **Assert "no script"**: `u16 @ slot0 == 0`, `slot 7 == 0`, `slot 12 == 0`, `slot 15 == 0`; and `slot10 + slot11 <= 0x100000`. | none | all four are zero (this card); log the negative explicitly so a VBIOS update that *does* carry scripts is visible in the log | a non-zero script pointer means the ROM changed: re-run §1.4 before trusting anything |
| 3 | **The fence probe** (§8). | 2 writes into `0x110040`, 2 restores | `rd32(0x110040)` returns what was written, twice, and returns `0` after the restore | see §8's decision table |
| 4 | If step 3 says "writes land": **FWSEC launch** per `docs/nv-pmu-acr-contract.md` §3.3, unchanged. | GSP falcon | `0x110100` bit 4 (`HALTED`) ≤ 2 s, then `0x110040 == 0` and `0x1438 >> 16 == 0` | FWSEC error code in `0x1438`; stop, do not retry in a loop |
| 5 | **Privilege-drop gate**: read SEC2. | none | `0x840100 != 0xBADF5620` within 100 ms | the fence story is wrong; dump `0x840100`, `0x84012C`, `0x841000` and stop |
| 6 | Only now the SEC2 booter (`docs/nv-gsp-wpr-contract.md` §4.2). | SEC2 | `0x840100` bit 4 = 1, then `0x840040 == 0` | booter error code |
| 7 | **Never** add `devinit_replay()`. If a future ROM needs it, it needs §2's full opcode table, the condition tables, and §6's bounding rule first. | — | — | — |

---

## 8. The single experiment that decides "devinit is required" vs "something else fences the window"

**Test register: `0x110040`, `NV_PGSP_FALCON_MAILBOX0`** (NVIDIA `dev_gsp.h`). Why this one:

* it is **outside** the fenced subset — it reads `0x00000000` in the pre-init dump, in the post-init dump,
  and in the HobbyOS log (MEASURED, three independent sources), so its read-back is *data*, not a fence word;
* it is a plain read/write 32-bit register whose value the driver already has to prefill to `0` before
  FWSEC (the contract's step 7), so writing it is on the normal path anyway;
* its expected value is unambiguous: whatever was written, or `0` if the write was dropped.

The experiment (two writes, two restores, four reads):

```
1.  a = rd32(0x110040);  b = rd32(0x110044)          // baseline; expect 0 / 0 on this card
2.  wr32(0x110040, 0xA5A51234)
3.  r1 = rd32(0x110040)                              // ← the discriminator
4.  wr32(0x110044, 0x5A5A4321); r2 = rd32(0x110044)  // independent second sample, same window
5.  wr32(0x110040, 0); wr32(0x110044, 0)             // MUST restore: 0x110040 is the FWSEC
                                                     // verdict word, and a leftover value would
                                                     // make a later FWSEC run read as "failed"
6.  c = rd32(0x110040);  d = rd32(0x110044)          // expect 0 / 0
```

| result | means | next move |
|---|---|---|
| `r1 == 0xA5A51234` (and `r2 == 0x5A5A4321`) | **writes to the GSP falcon page are accepted before FWSEC.** The `0xBADF5620` the parent saw is a *read*-side privilege mask, not a write fence. devinit is irrelevant to this blocker — nothing needs unfencing, FWSEC can be launched now | go to §7 step 4 |
| `r1 == 0` while `a == 0` | the write was dropped (or `MAILBOX0` is write-gated like the rest). The window is genuinely write-fenced | re-run after a **cold** start of the VM with no FLR to separate "FLR state" from "power-on state"; if still 0, FWSEC itself is blocked and the escalation is the PMU/SEC2 question in `docs/nv-pmu-acr-contract.md` §8, not devinit |
| `r1 == 0xBADF5620` | impossible in principle (a fence word on a register that is not fenced) — treat as "the register moved", re-check the aperture base | stop; the BAR or the chip revision is not what the doc assumes |

Two zero-cost companion reads, for the log only (they cannot decide anything): `0x110008` (expect
`0x10`), `0x11005C` (expect `0x0000FFF8`), `0x1103C0` (expect `0`) — if these do not match the
reference dump's pre-init values, the VM's card is in a different state than the reference card was and
the experiment's baseline is void.

**Why not `0x110600` (the obvious choice):** it is one of the fenced registers. Pre-init it answers
`0xBADF5620` no matter what; post-init it answers `0x110`, which is a configuration word, not the
value written. Both outcomes are uninformative, which is exactly how the current hypothesis survived
contact with the hardware.

**Why this is the cheapest discriminator overall:** it is 2 writes + 6 reads, it cannot wedge anything
(FWSEC is not running, so the mailbox is inert; `0x110040`/`0x110044` are the only two falcon registers
the contract already writes), it needs no VRAM, no DMA, and no new parsing — and its two outcomes lead
to *disjoint* next steps, which is what the previous test failed to do.

---

## 9. What this document does not establish

* **What the PMU's devinit ucode writes.** The devinit "script" for Ampere is inside the `0xE0`
  partition's ucode images; no plaintext instruction stream with MMIO addresses exists in the 1 MiB, so
  either the script is not in plaintext or it is not in this ROM at all. Not host-replayable either way.
* **Which lookup-table `app_id` is devinit.** `0x01` is the only unclaimed entry and nova's FWSEC doc
  labels a *different* id space ("DEVINIT (ID 1)"); the identification is not made here.
* **Slot-order drift.** Slots 3≡6 and 5≡8 share targets in this ROM's 36-byte block; whether that is a
  shared table or an older field order is not established, and the newest published 17-field list is
  what §1.2 is keyed against.
* **Whether Turing's CRTC heuristic works in this VM.** nouveau's "did x86 devinit run" test reads VGA
  CRTC `0x00`/`0x1A`; with no VGA decode in the guest those reads may return nothing at all. Untested.
* **Post-init register semantics.** `0x110600 = 0x110` post-init is measured, not explained.
* **Any other VBIOS revision.** Rule is generic; every offset in §1 (`0x409E`, `0x404A`, `0x0FB6`,
  `0x29F`) is this ROM only.

---

## 10. Sources

| artefact | used for |
|---|---|
| `docs/nv-vbios-evidence/rtx4090-ad102-vbios-1MiB.bin.gz` (sha256 `7dfb34…b863`) | every offset and byte in §1, §5 |
| NVIDIA, *BIOS Information Table* specification (`open-gpu-doc/BIOS-Information-Table`) | BIT header/token layout; token ids `0x44`=DFP, `0x49`=NVINIT; the 17-field `BIT_NVINIT_PTRS` list; the `0x7A`-style absolute addressing note |
| nouveau `nvkm/subdev/bios/bit.c` — `bit_entry()` | token stride/count/field order confirmed against a real parser |
| nouveau `nvkm/subdev/bios/init.c` — `init_table()`, `init_script()`, `init_script_table()`, `init_table_()`, `init_condition_met()`, `init_io_condition_met()`, `init_io_flag_condition_met()`, `init_xlat_()`, `init_nvreg()`, `init_rd32()/init_wr32()/init_mask()`, `nvbios_exec()`, `nvbios_post()`, `init_opcode[]` | §2 in full (structure chain, conditions, operand layouts, opcode inventory, execution model) |
| nouveau `nvkm/subdev/devinit/tu102.c` — `tu102_devinit_post()`, `tu102_devinit_wait()`; `ga100.c`, `gv100.c`, `gm200.c` func tables | TU102/GA100 devinit is a poll, not a script run; no `.mmio` mapper |
| nouveau `nvkm/subdev/devinit/nv50.c` — `nv50_devinit_preinit()`, `nv50_devinit_init()` | the CRTC `0x00`/`0x1A` "has devinit run" heuristic |
| kernel `Documentation/gpu/nova/core/devinit.rst`, `fwsec.rst` | devinit as PMU ucode launched by FWSEC; DEVINIT in the `0xE0` partition |
| NVIDIA `open-gpu-kernel-modules` headers `dev_gsp.h`, `dev_sec_pri.h`, `falcon_second_pri.h` | `NV_PGSP`/`NV_PSEC` apertures, `MAILBOX0/1` = `0x110040`/`0x110044`, `ENGINE` = `0x1103C0`, the "second/privileged" falcon naming |
| `docs/nv-gsp-reference-evidence/dump-{pre,post}init-gsp-sweep.txt`, `-named.txt` | which falcon offsets are readable pre-init (60 of the first 256 dwords); `0x110600`/`0x110624`/`0x11010C` fenced; `0x110040` = 0 in both states |
| `docs/nv-gsp-reference.md` §3.1-§3.4 | the three `0xBADF` patterns and their meanings |
| `docs/nv-pmu-acr-contract.md` §3.3-§3.4, §5 | the FWSEC launch the driver must do *instead* of a replay; the read-back limitation of fenced blocks |
| `docs/nv-vbios-map.md` §4 | the logical→flash pointer rule applied in §2.1 |
