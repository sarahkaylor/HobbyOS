# Zero-boot survey — every reachable SEC2/booter/FMC/ACR artifact, ranked for R1

Card `t_9bf9bc0c` (AD102 RPC link), step 1 of the reopened ladder (`docs/nv-plan.md` §2.13.1).
**Boots spent on this run: 0.** Every row below is a file read, a hash, or a static
parse; nothing was staged, no `qm start`, no launch. The one candidate that survives is
one boot away, and that boot is not taken here.

Part under test: **AD102 `10de:2684`**, SEC2 ucode id **3**, fuse version **1**
(`0x824140` = 1, measured `t_c724a763`/`fz1a`).

Durable copy of the card's `SURVEY-R1.md`. The next boot's rig is the branch this
file lives on (`nv/wp-r1-rig`); §8 names its commit and the hash gate that stops a
boot from being spent on the wrong container.

---

## 0. The answer in one table

Four distinct AD102 `booter_load` containers exist in everything reachable; **three were
already booted and all three return `0x002B0009`**; the fourth is new, was never booted,
and is the only artifact whose static identity says it *could* pass R1.

| # | container | bytes | sha256 (16) | identity | booted? | static verdict | boot cost |
|---|---|---|---|---|---|---|---|
| **1** | **AD102 load, 580.196.07 → 620.73.04** (10 tags) | **58 488** | **`ec5bcd117e18166c`** | **1 / 0x1 / 3** | **no** | **IDENTITY-COMPATIBLE** | **1 boot (VM 300, ~6.5 min)** |
| 2 | AD102 load, 570.86.15 → 615.71.10 (83 tags), incl. 580.178.04 | 57 720 | `8b293e19b637c5e2` | 1 / 0x1 / 3 | **yes** — `v2a` | IDENTITY-COMPATIBLE, **measured `0x002B0009`** | 0 — spent |
| 3 | AD102 load, 555.58 → 565.77 (7 tags) | 57 720 | `3d8a9f22810234fe` | 1 / 0x1 / 3 | **yes** — `v3a` | IDENTITY-COMPATIBLE, **measured `0x002B0009`** | 0 — spent |
| 4 | AD102 load, linux-firmware 535.113.01 | 55 928 | `7d4ef3258da016f5` | 1 / 0x1 / 3 | **yes** — `v1a` | IDENTITY-COMPATIBLE, **measured `0x002B0009`** | 0 — spent |
| — | AD102 unload, new generation (all ten tags that carry `ec5bcd11`) | 42 872 | `0745087776b7548e` | 1 / 0x1 / 3 | no | IDENTITY-COMPATIBLE | 1 boot, *second* — teardown image, no new class |

`0x002B0009` is the boot ROM's **post-signature payload check** (`t_c724a763`): the AD102
production signature verified in all three cases and the comparison after it failed.
`0x00250009` (GA102/GA100, measured `v4a`/`v5a`) is the *signature-class* word, so the
cross-chip images are a different (and already measured) failure.

---

## 1. Reachability — measured, not assumed

| source | state on this run | consequence |
|---|---|---|
| **r1** `ssh -F ~/.ssh/config r1` (192.168.10.174, root) | **reachable** | the only host enumerated |
| r1 `/lib/firmware/nvidia` | **468 files, 127 nvfw containers** (full TSV: `r1-fw-inventory.tsv`) | the firmware tree, §2 |
| r1 guest **VM 301** disk (`local-lvm:vm-301-disk-0`, 60 G) | mounted **read-only, `-o ro,noload`, over `qemu-nbd --read-only`**; disconnected afterwards | the only guest on any reachable host with a vendor driver package (§3) |
| r1 CIFS shares `/mnt/pve/{gamingpc,flash,flash-r2,flash-r3}` | searched to depth 6 for `*.run`, `fmc-*.bin`, `booter_*.bin`, `linux-firmware*`: **0 hits** | the shares hold VM disks/ISOs, no driver packages |
| **c2, c3, pve1, r2, r3** (cluster dirs in `/etc/pve/nodes`) | from r1, `ssh root@<n>` → **connect timeout** (6 s each); `getent hosts` resolves c2/c3/c4 to the single public address `67.176.248.218` | **no route**; their `/etc/pve/nodes/*` entries carry only stale `lrm_status` files |
| **c4** LAN `192.168.0.250` / `192.168.0.251` | `.250` connect timeout; `.251` answers SSH but **`Permission denied (publickey)`** — this lane holds only `~/.ssh/id_r1` | c4's one reachable endpoint refuses the key; **not enumerated** |
| local box `/srv/nv5090/work/dl/` | `NVIDIA-Linux-x86_64-570.144.run` (375 742 470 B), `NVIDIA-Linux-x86_64-580.178.04.run` (397 214 534 B) | §3 |
| OpenRM tags (raw.githubusercontent, NVIDIA/open-gpu-kernel-modules) | **HTTP 200 for all 100 tags fetched** (555.58 → 620.73.04) | the booter source, §4 |

Nothing was inferred from an unreachable host. "c2/c3/c4 if they answer" — they do not.

---

## 2. r1 `/lib/firmware/nvidia`: the on-disk census

468 files, 127 nvfw containers (3 ELF32, 13 ELF64, 325 other/payloads). Twelve containers
declare **ucode id 3** — the only id a SEC2 booter for this part can use — and they come
from exactly three chip families:

| file | bytes | sha256 (16) | triple | hs_gate |
|---|---:|---|---|---|
| `ad102/gsp/booter_load-535.113.01.bin` | 55 928 | `7d4ef3258da016f5` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ad102/gsp/booter_load-570.144.bin` | 57 720 | `8b293e19b637c5e2` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ad102/gsp/booter_unload-535.113.01.bin` | 39 800 | `d036dfee0545c36e` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ad102/gsp/booter_unload-570.144.bin` | 41 592 | `975b85a14ded8e43` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ga102/gsp/booter_load-535.113.01.bin` | 59 768 | `3ab46bf4c70d72e8` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ga102/gsp/booter_load-570.144.bin` | 61 304 | `4497e3eff7e95c77` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ga102/gsp/booter_unload-{535,570}` | 39 544 / 41 080 | `09240c83…` / `8e63db5b…` | 1/0x1/3 | IDENTITY-COMPATIBLE |
| `ga100/gsp/booter_{load,unload}-{535,570}` | 59 128 / 60 664 / 38 904 / 40 440 | `1d11d861…` / `46bafe32…` / `0518f894…` / `33bea246…` | **0**/0x1/3 | FUSE-PRECEDENT-MISSING |

No **AD102** `sec2/`, `acr/` or `fmc/` directory exists on r1 — `nvidia/ad102/` is `gsp/`
only. The GA102 `sec2/hs_bl_sig.bin` (1 280 B, `1/0x1/5`, IDENTITY-COMPATIBLE) and the
three GA102 ACR containers (`2/0x1/1`, FUSE-PRECEDENT-MISSING) are the only non-booter
HS containers present; see §6 for why neither is an R1 candidate.

---

## 3. Vendor packages — what is actually inside them

1. **The `.run` packages hold no booter.** `sh NVIDIA-Linux-x86_64-580.178.04.run --list`
   and the same for `570.144` show exactly two firmware files each:
   `firmware/gsp_ga10x.bin` and `firmware/gsp_tu10x.bin` (75 028 464 / 30 483 544 B for
   580; 63 571 696 / 28 542 040 B for 570). Those are **GSP-RM images (ELF64/RISC-V)**,
   not SEC2 containers. The same is true of the installed 580.178.04 tree on r1's VM 301
   guest: `/usr/lib/firmware/nvidia/580.178.04/{gsp_ga10x,gsp_tu10x}.bin` and nothing else.
2. **The booter is compiled bindata, not a file.** VM 301's
   `/usr/src/nvidia-srv-580.178.04/nvidia/nv-kernel.o_binary` (112 677 456 B) contains
   **0** raw `0x10de` containers and **0** `booter`/`bindata` strings — the generated
   arrays are compressed at build time. So the installed package cannot yield the blob by
   scanning; the vendor's own generated source (the OpenRM tag) can, and does (§4).
3. **580.178.04 is the 570 container.** Rebuilding tag `580.178.04` gives
   `8b293e19…` byte-identically — the package the coordinator now has on r1 adds **no new
   AD102 booter**. The new generation starts at **580.196.07**, a later 580 point release.

---

## 4. OpenRM tag sweep: three distinct AD102 `booter_load` containers in 100 tags

Method: fetch `src/nvidia/generated/g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.c`
per tag, inflate each bindata array (raw deflate, `wbits=-15`), rebuild the nvfw container
(`bin_hdr | hs_header_v2 | sig_prod | patch_loc | patch_sig | meta | num_sigs_off |
num_sigs | load_hdr | image`), hash, then decode the triple with
`tools/nv/no-rm/hs_gate.py`.

**The rebuild is validated, not assumed:** rebuilding tag `570.144` reproduces the
linux-firmware file **byte for byte** — 57 720 B,
`8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a`. Two independent
encodings of the same new container (the NvU8 arrays in `580.196.11`/`595.112.13` and the
NvU64 arrays in `615.78.08`/`620.73.04`) rebuild to the **same** 58 488-byte blob, and
their component hashes agree exactly across all four tags — `SIG_PROD`
`140098e194272cf8…`, `IMAGE_PROD` `a41100ea9b3b5319…` — while their DBG arrays differ
(`2363aeb9…` / `417e6571…`), so the PROD identity is selected and not confused with the
debug-signed image that measured `0x250009` (`v6a`).

| sha256 (16) | bytes | triple | verdict | tags |
|---|---:|---|---|---|
| `3d8a9f22810234fe` | 57 720 | 1/0x1/3 | IDENTITY-COMPATIBLE | `555.58 555.58.02 560.28.03 560.31.02 560.35.03 565.57.01 565.77` (7) |
| `8b293e19b637c5e2` | 57 720 | 1/0x1/3 | IDENTITY-COMPATIBLE | `570.86.15 … 615.71.10` (83 tags; includes `570.144`, `575.*`, `580.65.06 … 580.178.04`, `590.48.01`, `595.44.02 … 595.99.02`, `610.*`, `615.71.09/10`) |
| **`ec5bcd117e18166c`** | **58 488** | **1/0x1/3** | **IDENTITY-COMPATIBLE** | **`580.196.07 580.196.11 595.44.16 595.104.02 595.112.07 595.112.13 615.78.08 620.70.06 620.70.10 620.73.04`** (10) |

100 tags fetched, 100 HTTP 200, 0 rebuild failures (`sweep-load.tsv`).

**The "620 set already spent" is the GB202 FMC pair, not this container.** The prior
620 work (`t_320e84e5`) ran `fmc620`/`GspRmBoot_GB202` on the 5090. The AD102
`booter_load` that the 620.70.06 → 620.73.04 tags carry is `ec5bcd11`, and no boot in this
project's history has staged it — the last AD102 sweep (`t_7c890c1e`, `v1a`–`v6a`) stopped
at tag `580.178.04`, which is the *old* container.

**The branch order is not monotonic, and that matters:** `580.196.07` introduces the new
container, `590.48.01` ships the old one again, `595.44.16` has the new one, `595.99.02`
the old one again, `615.71.10` old, `615.78.08` new. "Newest tag" is not a shortcut —
a candidate must be selected **by hash**, and `ec5bcd11` is present in the terminal
`620.73.04`.

### 4.1 What the new container actually is

| field | 570.144 (`8b293e19`) | new (`ec5bcd11`) |
|---|---|---|
| file bytes | 57 720 | 58 488 |
| `bin_hdr` | v1, bin_size 0xE200, data 0x378+0xDE00 | v1, bin_size 0xE500, data 0x378+0xE100 |
| HS header | `sig 0x3C+0x300`, meta `0x344+0xC`, numSigsOff `0x350`, load `0x354+0x24` | **identical** |
| `patch_meta` | **1 / 0x1 / 3** | **1 / 0x1 / 3** |
| patch site (`*patch_loc`) | 0x7E10 | 0x8110 |
| load header | os_code 0+0x100, os_data 0x7E00+0x6000, apps 1, app0 0x100+**0x7D00** | os_code 0+0x100, os_data 0x8100+0x6000, apps 1, app0 0x100+**0x8000** |
| prod signature (768 B = 2×384) | sha `11d6c63d7cbcf60f…` | sha `140098e194272cf8…` |
| payload sha | `83e06ccd394eb73a…` | `a41100ea9b3b5319…` |

So it is a **new signed build of the same structure**: same identity, same signature slot
count, **768 more bytes of app code** (0x7D00 → 0x8000; the patch site moves by exactly
0x300 to 0x8110). The payload differs from the first app-code byte (offset 0x100); 31 163
of the first 56 832 payload bytes are equal (55 %) — the shape of a re-keyed/re-encrypted
app image, not a new container format.

**Tiling, checked against the lane's own arithmetic** (`nv_gsp.c`, which derives
`imem_file = data_off + app[0].code_off`, `dmem_file = data_off + os.data_off`):

```
imem 0x478 + 0x8000 = 0x8478 = dmem
dmem 0x8478 + 0x6000 = 0xE478 = 58488 = file size        -> TILES-THE-FILE
bin_size 0xE500 - data_size 0xE100 = 0x400               -> container parse accepts
len 58488 >= 0x7700 + 0x6000 (55040)                     -> the lane's length gate accepts
```

No driver or lane change is needed to stage it; the A12 rig's `NV_BOOTER_FILE=`
override (branch `nv/wp-r1-rig`, §8) takes it as-is, and `NV_BOOTER_SHA256=` makes
the lane refuse if the staged file is not this hash.

---

## 5. Ranked survivors, with the boot each would cost

1. **`ec5bcd11` — AD102 `booter_load` 58 488 B (tags 580.196.07 → 620.73.04).**
   **1 boot.** Static identity is the measured-compatible class (ucode 3, fuse 1, word
   0x1, engine 0x1), it is an AD102 production image from the vendor's own bindata, and
   it is the only AD102 container that has never been through the BROM. Run it from
   branch `nv/wp-r1-rig` (§8), which stages this exact file and refuses on a hash
   mismatch:
   `NV_BOOTER_FILE=<rebuilt blob> NV_BOOTER_SHA256=ec5bcd11… tools/nv/sec2-boot-lane.sh 1 r1new`
   (count 1, ~6.5 min; `0 r1pre` is the same staging preflight with no VM start).
   Decision table from `docs/nv-no-rm-path.md` §6: `0x002B0009` → close R1 for this
   generation; anything else with `INTR` not `EXIT` → the next card; `0x0` + metadata
   `verified` written → SEC2 authenticated the image, and the round-trip half then needs
   the transport tree `nv/wp-rpc-final` (`c58b1295`) merged into this rig (§8 names
   both conflicted files; branch `nv/wp-r1-merged` carries both halves, so one boot
   answers both) — a pass here
   is the SEC2 word first, and the round trip only if the peer then answers.
2. **`07450877` — AD102 `booter_unload` 42 872 B (all ten tags that carry `ec5bcd11`).**
   1 boot, *after* #1 and only as a second signature-generation sample: the unload path is
   a teardown image, and the 535-generation unload already returned `0x002B0009`. It
   shares #1's generator, so it adds no independent class unless #1 is refused before the
   payload check.
3. **`975b85a1` — AD102 `booter_unload` 41 592 B (570/580/590/610).** 1 boot, low value:
   same branch as the measured `8b293e19` load; not the boot path.
4. Everything else: **0 boots** — §6.

---

## 6. Statically ruled out, and why

| artifact | why no boot |
|---|---|
| AD102 `booter_load` `8b293e19` (570.86.15 → 615.71.10, incl. 580.178.04) | **already measured**: `v2a`, `0x002B0009` at +4.163 ms, `INTR=0x10 EXIT` |
| AD102 `booter_load` `3d8a9f22` (555.58 → 565.77) | **already measured**: `v3a`, `0x002B0009` at +4.176 ms |
| AD102 `booter_load` `7d4ef325` (535.113.01) | **already measured**: `v1a`, `0x002B0009` at +4.208 ms (the control) |
| AD102 `booter_unload` `d036dfee` (535) | **already measured**: same `0x002B0009` (`t_c724a763`) |
| GA102 `booter_load` `3ab46bf4` / `4497e3ef` | **measured `0x250009`** (`v4a`): signature class — this AD102 does not verify a GA102-signed container, and no AD102-signed non-GSP ucode is published |
| GA100 `booter_load` `1d11d861` / `46bafe32` | **measured `0x250009`** (`v5a`); also `fuse_ver=0` against this part's fuse word 1 |
| TU102/TU116 `booter_load` | `hs_gate`: `UCODE-ID-NOT-FOR-THIS-PART` — ucode id **13**, not 3; a Turing booter cannot serve this part |
| AD102 `bootloader-{535,570}.bin` (32 876 / 36 972 B) | nvfw containers but **not SEC2 images**: ucode id 5, engine 0x8000/0x9000, 100+ load-header entries — the GSP-RM RISC-V bootloader |
| GB202/GB100 `bootloader-570.144.bin`, `gb202`/`gh100` `fmc-*.bin` | FMC is ELF32 (not an nvfw container); the bootloader is the RISC-V loader. These are **R2** (GB202) artifacts, not R1 |
| `gsp-*.bin` (38–75 MB) | ELF64/EM_RISCV GSP-RM images — the peer's firmware, not a SEC2 container |
| GA102 `sec2/hs_bl_sig.bin` (1 280 B, `1/0x1/5`) | the SEC2 HS **bootloader**, not a GSP booter: N3n (non-GSP chain) is closed in class (`docs/nv-games-ladder.md` §2.1), and it is GA102-signed — the cross-chip risk measured at `0x250009` applies |
| GA102 ACR `ucode_ahesasc/asb/unload` (`2/0x1/1`) | `hs_gate`: FUSE-PRECEDENT-MISSING (declared fuse 2 has no measured precedent on this part) **and** GA102-signed; no AD102 ACR is published |
| GA102 `sec2/{image,sig,desc}.bin`, `ga102/gr/*` | payloads/descriptors, not standalone nvfw containers (no `0x10de`); the GR/NET blobs are not SEC2 images |
| `*/nvdec/scrubber.bin`, `tegra*`, `gp10*` | ucode id ≠ 3 (14/58/…), `hs_gate` drops them by id |

---

## 7. What this survey does **not** establish

- **No R1 claim.** `ec5bcd11` is *statically* compatible; the signature class cannot be
  checked without the fused key, and three identity-compatible AD102 production images
  already failed the payload check. Only a boot moves the row.
- **Static identity is necessary, not sufficient** — `hs_gate` says so in its own row, and
  the measured history is the proof.
- **c2/c3/c4 were not enumerated** (§1): no route from r1, and the one answering endpoint
  (`192.168.0.251`) refuses this lane's key. If they hold a *different* vendor package, it
  is not in this table.
- **No GB202 work was done here.** The GB202 FMC pair (`fmc-570.144.bin` 199 820 B,
  `FMC620.BIN` 287 884 B) and the 580.178.04 GB202-signed RM image belong to R2 and cost
  VM-313 boots, not this card's.
- **No message was sent and no doorbell rung.** The RPC transport state is unchanged from
  attempt 3: publish + 129 pages + pre-boot 72/73 + SEPARATE-WORDS canary, `linked=0`.

---

## 8. Reproduce (all read-only), and the next boot

**The rig is one branch.** Everything below runs from branch **`nv/wp-r1-rig`**,
which is `nv/wp-sec2-auth-verdict` (`fde3f41f`) plus the commit that carries this
file — `git log -1 --format=%H -- docs/nv-r1-survey.md`. That branch is the only
tree in this repo whose lane stages a booter by name:

```
tools/nv/sec2-boot-lane.sh:  NV_BOOTER_FILE=<path>  NV_BOOTER_SHA256=<hex>
                             count=0 -> staging preflight, no VM start
```

Every other `sec2-boot-lane.sh` in the tree (including the one on
`nv/wp-rpc-final`) ignores `NV_BOOTER_FILE` and stages
`third_party/nvfw/ad102/gsp/booter_load-535.113.01.bin` — the measured control —
so a boot from one of those reads `0x002B0009` for `ec5bcd11` without ever
staging it. The gate in this branch prints the row below and aborts **before the
upload** on a mismatch:

```
[sec2-lane] booter staged: path=rebuilt-load-620.73.04.bin sha256=ec5bcd117e18166c… size=58488
[sec2-lane] FATAL: booter sha256 <got> != NV_BOOTER_SHA256 <want> -- refusing to spend a boot on the wrong image
```

The three survey scripts are card artifacts, not repo files: take
`inventory.py`, `sweep_tags.py`, `rebuild_booter.py` from
`ARTIFACTS-MANIFEST.txt` (hashes there) and put them beside a `bindata/` cache.

```
# r1 census (root; writes nothing)
scp inventory.py hs_gate.py r1:/root/nvsurvey/
ssh r1 'python3 /root/nvsurvey/inventory.py /lib/firmware/nvidia' > r1-fw-inventory.tsv

# vendor package contents
sh NVIDIA-Linux-x86_64-580.178.04.run --list | grep -i firmware

# VM 301 guest, read-only, no boot
ssh r1 'qemu-nbd --read-only --connect=/dev/nbd1 /dev/pve/vm-301-disk-0
        mount -o ro,noload /dev/nbd1p1 /mnt/nv301ro'
ssh r1 'find /mnt/nv301ro/usr/lib/firmware -ipath "*nvidia*" -type f -printf "%s %p\n"'
ssh r1 'umount /mnt/nv301ro; qemu-nbd --disconnect /dev/nbd1'

# tag sweep + rebuild + decode.  bindata is the vendor's own generated source:
curl -fsSL -o bindata/g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.620.73.04.c \
  https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/620.73.04/src/nvidia/generated/g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.c
python3 sweep_tags.py $(cat tags.txt) > sweep-load.tsv
python3 rebuild_booter.py \
  bindata/g_bindata_kgspGetBinArchiveBooterLoadUcode_AD102.620.73.04.c \
  rebuilt-load-620.73.04.bin                     # 58488 B, ec5bcd117e18166c…
python3 tools/nv/no-rm/hs_gate.py rebuilt-load-620.73.04.bin   # IDENTITY-COMPATIBLE
```

**The next boot, in two steps — preflight first, then the one boot.** Build
`disk.img` from branch **`nv/wp-r1-merged`** — the rig plus the transport, in one
tree (the merge is described at the end of this section) — then:

```
export NV_BOOTER_FILE=$PWD/rebuilt-load-620.73.04.bin
export NV_BOOTER_SHA256=ec5bcd117e18166c1f5e0336b62239457d9048e68b7e8d8a93ea628394724bc5
tools/nv/sec2-boot-lane.sh 0 r1pre     # stages, asserts, touches no VM
tools/nv/sec2-boot-lane.sh 1 r1new     # the one authorized boot (~6.5 min)
```

Scope of that boot: it answers **R1 (the SEC2 word)** and, because the transport
rides in the same tree, the round-trip rows too if SEC2 authenticates the image.
The merge is `nv/wp-rpc-final` (`c58b1295`) into `nv/wp-r1-rig` (`5b64dc0d`) and
it conflicts in **two files**, both of which have to be resolved on the rig's
side:

- **`src/kernel/nv_gsp.c` — 15 content hunks.** Every one is the rig's copy of
  the SEC2 code superseding an older copy of the same functions (`sec2_arm`
  grew the `struct sec2_auth` argument and a tag, `sec2_reset_fixup_variant`
  gained `break_sig`/`facts`, and the auth-bisect compare and the
  `SEC2_IMEM_OSCODE` switch exist only on the rig side). The transport's own
  additions — publish, the pre-boot 72/73, stage 8/9 and their call sites in
  `nv_gsp_boot` — merge cleanly and are not in any hunk.
- **`tools/nv/sec2-boot-lane.sh` — a 2-hunk add/add.** This one is material:
  `c58b1295`'s lane predates `NV_BOOTER_FILE`/`NV_BOOTER_SHA256`, so a merge
  resolved toward the transport side would compile the rig's kernel and then
  stage the **first globbed booter — `booter_load-535.113.01.bin`, the measured
  control** — and the boot would read `0x002B0009` for `ec5bcd11` without ever
  staging it. Take the rig's lane.

Artifacts beside this file: `r1-fw-inventory.tsv`, `sweep-load.tsv`,
`sweep-unload.tsv`, `hs_gate-all.txt`, `tags.txt`,
`rebuilt-load-620.73.04.bin` (the candidate), `rebuilt-570.144.bin` (the
validated rebuild), the four independent rebuilds that prove the two generated
encodings agree (`580.196.11`, `595.112.13`, `615.78.08`, plus the candidate),
and the three scripts (`inventory.py`, `rebuild_booter.py`, `sweep_tags.py`)
with `diff_containers.py`, `dump_hs.py`, `prod_vs_dbg.py`, `summarise.py`
(the per-claim witnesses). `survey/rebuilt-load-620.70.10.bin` was a pre-fix
58 504-byte artifact; it contradicted `sweep-load.tsv:99` and has been deleted.

## 9. One tool nit, found on the way and fixed here

`hs_gate.py` used to label the 7th dword of `nvfw_hs_header_v2` as `num_sig` and
print `num_sig=848` for every booter. That dword is an **offset** (0x350); the
count lives at that offset (value 2; `sig_prod_size 0x300 / 2 = 0x180` per slot).
The label is fixed in this branch's `tools/nv/no-rm/hs_gate.py` (`num_sigs_off=0x350
num_sigs=2`) and the verdict is unchanged — it always came from `patch_meta`, which
is why every row above stands. The copy of `hs_gate.py` beside this file **is that
fixed tool** (sha256 `8df90328…`) and `hs_gate-all.txt` was regenerated with it: the
five rows read `num_sigs_off=0x350 num_sigs=2`, the verdicts are the same five
`IDENTITY-COMPATIBLE`, and the pre-fix file's trailing `FileNotFoundError` (a sixth,
absent input) is gone.
