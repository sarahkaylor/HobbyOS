# WPR / booter hand-off — address provenance

Scope: the booter hand-off as it sits in `nv/integ-swarm` (`2fc2d17c`) plus this
card's change: `nv_gsp_firmware_check()` in `src/kernel/nv_gsp.c` (the WPR
metadata block, the radix3 tree, the SEC2 DMA source), the helpers it drives in
`src/kernel/nv_fw.c` (`nv_fw_load_blob()`, `nv_radix3_build[_into]()`), and the
boot-path state in `src/include/nv.h`. The contract for the block itself is
`docs/nv-gsp-wpr-contract.md` §3–§4.3; the facility that separates a bus address
from a CPU physical address is `src/include/nv_dma.h`.

This is the sibling of `docs/nv-rpc-bus-provenance.md`: that document covers the
RPC transport (downstream, after GSP-RM runs), this one covers the WPR/booter
hand-off (upstream, before it). Both are gated by the same rule.

## The rule

A device-visible address is only ever the value `nv_dma_bus(&map)` returned for
the mapping that owns those bytes. `map.phys` is a CPU physical address and
never crosses to the device. `bus == phys` on this machine only because VT-d
translation is initialised but not enabled, so substituting one for the other is
invisible at runtime — a field built from `phys` points SEC2/the booter at
unmapped memory the first time translation is live, with no error path: the
booter walks a page table that is not there and the card answers a mailbox code.

Two corollaries that shaped the fix:

- **A file/pbuffer offset is not a new address.** `loader_bus + 0x6C` and
  `img_bus + sig_off` are one addition on a bus base; the addend is a byte offset
  inside that mapping.
- **A buffer a device reads must be a mapping, not a frame run.** The firmware
  blobs and the radix3 tree are now allocated with `nv_dma_alloc()`, so each has
  exactly one `bus` value, resolved once (`nv_fw_blob_bus()`, `nv_dma_bus()`).

## The table

Before this card, four values in `nv_gsp_firmware_check()` were built from
`.phys` (the assignment sites were `nv_gsp.c:4777`, `:4815`, `:4835`, `:4838`,
`:4860` at `2fc2d17c`). After it:

| # | Address the device reads | Written at | Value comes from | Verdict |
|---|---|---|---|---|
| 1 | **radix3 tree root** (`sysmem_addr_radix3_elf`) | `m.sysmem_addr_radix3_elf = tree.root_bus`, `nv_gsp.c:4857` | `struct nv_radix3_tree.root_bus` = `nv_dma_bus(&tree.map)`, resolved in `nv_radix3_build()` (`nv_fw.c:743`) from an `nv_dma_alloc()` run (`nv_fw.c:757`) | **nv_dma_bus()** |
| 2 | **radix3 pointer levels + level-2 PTEs** | `nv_radix3_build_into()`: `nv_fw.c:709` (PTEs), `:718` and `:721` (level-1/level-0 entries) | `image_bus + i * NV_RADIX3_PAGE` and `tree_bus + page offset`, where `image_bus = nv_fw_blob_bus(&rm)` (`nv_gsp.c:4798`) and `tree_bus` is the tree mapping's bus | **nv_dma_bus()** — the `phys` arguments are validation-only and are never stored |
| 3 | **bootloader data region** (`sysmem_addr_bootloader`) | `m.sysmem_addr_bootloader = … loader_bus + 0x6C`, `nv_gsp.c:4864` | `loader_bus = (ldr_rc == 0) ? nv_fw_blob_bus(&loader) : 0` (`nv_gsp.c:4711`); `0x6C` is the data region's file offset inside that mapping. Publishes 0 when the loader did not load (was `0 + 0x6C` before) | **nv_dma_bus()** + bus offset |
| 4 | **ad10x signature section** (`sysmem_addr_signature`) | `m.sysmem_addr_signature = sig_off ? (img_bus + sig_off) : 0`, `nv_gsp.c:4889` | same `img_bus`; `sig_off` is the `.fwsignature_ad10x` section's file offset, still valid after the `.fwimage` slide because the section sits after it | **nv_dma_bus()** + bus offset |
| 5 | **SEC2 xfer-engine source** (the DMA that *reads* the booter image) | `uint64_t base = d->booter_bus;`, `nv_gsp.c:4395` | `d->booter_bus = nv_fw_blob_bus(&booter)` (`nv_gsp.c:4689`), recorded where the blob is loaded | **nv_dma_bus()** — was `booter_phys` before this card |
| 6 | **WPR metadata block** itself (MAILBOX0/1 carry its address) | `nv_dma_alloc(NV_DMA_PAGE, &d->gsp_meta)` → `meta_bus = nv_dma_bus(&d->gsp_meta)` | already a mapping before this card (WP-G) | **nv_dma_bus()** |
| 7 | VRAM layout fields (`gsp_fw_wpr_start`, `frts_offset`, …), `m.verified` | same block | FB offsets/dwords computed from `nv_wpr_layout()`; no system address | **not addresses** (VRAM offsets) |

No intentional exceptions exist in this path.

## What changed in the code

- `struct nv_fw_blob` (`src/include/nv_fw.h`) carries `struct nv_dma_map map`
  instead of a bare `phys`. `map.phys` remains for `phys_to_virt()` and the log
  row; the device-visible base is obtained through **one** accessor,
  `nv_fw_blob_bus(&blob)` → `nv_dma_bus(&blob->map)`. There is no stored copy,
  so there is no second source of truth.
- `nv_fw_load_blob()` allocates its frames with `nv_dma_alloc()` (`nv_fw.c:827`)
  instead of `frame_alloc_contig()`. Under translation it now fails with
  `-ENOTSUP` at load time instead of handing the boot path a `phys` that only
  looks like an address.
- `nv_radix3_build_into()` takes **both** ends of both mappings
  (`tree_phys, tree_bus, image_phys, image_bus, size`). The entries hold
  `tree_bus`/`image_bus`; `tree_phys`/`image_phys` exist for validation (page
  alignment) and for the caller's `phys_to_virt()`. `NV_DMA_BUS_INVALID` is
  page-unaligned by construction, so "no honest bus address" is refused rather
  than written into a PTE.
- `nv_radix3_build()` allocates the tree with `nv_dma_alloc()` and returns
  `struct nv_radix3_tree { struct nv_dma_map map; uint64_t root_bus; }`; release
  is `nv_radix3_release()` → `nv_dma_free()`.
- `struct nv_dev` holds `uint64_t booter_bus` (was `booter_phys`) and
  `struct nv_radix3_tree radix3` (was `radix3_root` + `radix3_pages`).
- The five boot-path fields in `struct nv_info` are **dead ABI space** — 
  `nv_fill_info()` never writes them — so they were left in place (their names
  are now marked legacy) instead of moving the ABI.

## The regression guard

`tools/nv/check_wpr_bus_provenance.py` (stdlib only, ~0.1 s). `make
wpr_bus_provenance` runs it alone; it is a prerequisite of `make host_tests`.
It strips comments and string literals, then checks:

| Rule | What it proves |
|---|---|
| A | in `nv_gsp_firmware_check()`, no `m.sysmem_addr_*` assignment is built from `.phys`, and every one spells a bus value (`_bus` / `.root_bus`) |
| B | every `.phys` in that region is one of three CPU-side uses (`phys_to_virt`, a log row, the `img_phys` local that only the log uses) |
| C | no device-facing call (`nv_radix3_build`, `nv_gsp_sec2_start_booter_ex`, `falcon_dma_block`, `nv_wr32`/`nv_rd32`) takes an argument containing `.phys` |
| D | positive: the seam still reads `nv_fw_blob_bus(&rm)` / `(&loader)`, publishes `tree.root_bus`, `loader_bus + 0x6C`, `img_bus + sig_off`, and records `d->booter_bus`; the SEC2 source is `d->booter_bus` |
| E | `nv_fw.c`: no `fw_put64()` writes a `phys` expression into the tree; the PTEs use `image_bus` and both pointer levels `tree_bus` |
| F | `nv_fw_load_blob()` allocates through `nv_dma_alloc()` (no `frame_alloc_contig`), `struct nv_fw_blob` has no bare `phys` member, and `struct nv_dev` carries `booter_bus` / the tree mapping |

A violation exits 1 with `file:line` and the offending text; a moved/renamed
marker exits 2 so the guard is updated deliberately rather than silently
bypassed. Demonstrated by two one-line mutations — the signature field built
from `rm.map.phys`, and a level-2 PTE built from `image_phys` — which produce
five violations and a red `make wpr_bus_provenance`; reverted, green
(`evidence/nv/wpr-bus-guard-demo.txt`, reproducible with
`bash tools/nv/demo-wpr-bus-guard.sh`).

## Residual items (named, not silently left)

- The SEC2 helpers' parameter is still spelled `meta_phys` in
  `nv_gsp_sec2_start_booter_ex()`/`sec2_attempt()` even though the value is a
  bus address. It is a name, not a provenance defect (the value already comes
  from `nv_dma_bus()`), and those functions are the ones the r1 rig branch
  rewrites; renaming them here would add merge conflicts to a card that already
  has 15 hunks in `nv_gsp.c`. Tracked for the rig merge.
- `nv_fw_load()` (the pointer-only convenience form) hands back a buffer whose
  mapping is not kept; it is released with `nv_fw_release_buf()`. That is fine
  for the parse-and-verify path, which publishes nothing, and the header now
  says so. Any future caller that needs an address must use
  `nv_fw_load_blob()`.
- This guard is structural. It proves what the code names; it cannot prove the
  card accepts the addresses. The runtime evidence that `bus` reaches the
  device is the boot log's `[NV-DMA] map phys=… bus=… identity=1` rows plus the
  `[NV-GSP] radix3 root_bus=…` / `meta … block_bus=…` rows on a boot, and the
  unit tier's `test_nv_radix3_build*` cases (which are built with
  `bus != phys` on purpose).
