# The booter's 0x7600 app-code region: ciphertext, not RISC-V

Status: **offline analysis complete; nothing in the launch path changed.** This
document answers the one question `SWARM-REPORT-WP-A5.md` §6 left open
("Check offline whether the file's `0x7600` app-code bytes are ciphertext or
plaintext") and the same item in `docs/nv-plan.md`. Every number below comes
from `docs/nv-booter-evidence/analyse_booter_appcode.py`; its captured output is
`analyse_booter_appcode.out` in the same directory. Nothing in
`third_party/nvfw/` is modified.

**The answer in one line:** the `0x7600` bytes are **ciphertext** — a 16-byte
block cipher in **ECB** mode (AES-128-ECB in NVIDIA's documented HS
construction), keyed by the **fused heavy-secure ucode key that only the SEC2
boot ROM can reach**. Loading them verbatim into secure IMEM is *correct*: the
ROM is supposed to decrypt them. The WP-A5 hypothesis "if they are plaintext,
the ROM decrypts garbage" is **refuted**; the remaining failure is not the image
encoding.

---

## 1. What is being asked, and which bytes are meant

The `booter_load` container is parsed with the same structure the driver and
nova-core use (`NVFW_BIN_HDR` → `NVFW_HS_UCODE_HDR` v2 →
`NVFW_HS_LOAD_HDR`):

| field | value (MEASURED) |
|---|---|
| file | `third_party/nvfw/ad102/gsp/booter_load-535.113.01.bin`, 55 928 B, sha256 `7d4ef325…b45cd6` |
| container | magic `0x10DE`, version 1, `data_off 0x378`, `data_size 0xD700`; `data_off+data_size == 0xDA78 == file size` |
| HS v2 | `sig_prod (0x3C, 0x300)`, `patch_loc 0x33C → 0x7710`, `patch_sig 0`, `meta (0x344, 0xC) = {1, 1, 3}`, `num_sig 2`, `load_hdr (0x354, 0x24)` |
| load header | `os_code (data+0x0, 0x100)`, `os_data (data+0x7700, 0x6000)`, `num_apps 1`, `app0 code (data+0x100, 0x7600)`, `app0 data (data+0x100, 0)` |
| tiling | every declared size sums to `0xD700 == data_size` |

So the region in question is **file `[0x478, 0x7A78)`**, 30 208 bytes,
sha256 `47c6b482…46db3`. It is `data_off + app0.code_off`, the app code the
portable loader DMAs to secure IMEM. The 256-byte `os_code` block before it is
all zero (MEASURED), and nova-core does not load it on GA102+ (SOURCE:
`drivers/gpu/nova-core/firmware/booter.rs`, `imem_ns_load_target = None`,
app0 → secure IMEM at address 0).

## 2. (a) Entropy: 7.9949 bits/byte, incompressible, no code statistics

| sample | bytes | H(byte) | IC | zero | zlib | note |
|---|---:|---:|---:|---:|---:|---|
| **app code** | 30 208 | **7.9949** | **0.00390** | 0.37% | 0.993 | 256/256 byte values |
| uniform random (same length, seed 0x10DE) | 30 208 | 7.9944 | 0.00390 | 0.42% | 1.001 | 1888/1888 distinct 16-B blocks |
| RM `.fwimage` @0x100000 (plaintext RISC-V) | 30 208 | 6.1275 | 0.0316 | 11.7% | 0.396 | real code, see §3 |
| RSA signature in the same file | 768 | 7.7892 | 0.0036 | — | 1.014 | known-random reference |

The index of coincidence is the uniform value to five decimal places
(0.00390 vs 1/256 = 0.00391), every byte value occurs, and re-compressing the
region *expands* it (ratio 0.993). Per-256-byte-window entropy is flat:
min 6.8058, mean 7.1721, max 7.2923 over 118 windows — the expected collision
entropy of a uniform 256-sample alphabet, with no low-entropy window anywhere.

Plaintext RISC-V is not like this. The RM image's own `.fwimage` region measures
4.39 bits/byte at its start and 6.13 at `0x100000`, compresses to 34–40%, and
has a strong byte bias. The contrast is two bits per byte, not a rounding
difference.

**MEASURED verdict (a):** the region is statistically uniform random. It is
either ciphertext or already-compressed data; nothing about it is code-like.

## 3. (b) Disassembly: it decodes as garbage, with an invalid parcel 4 bytes in

Every stream is wrapped in an ELF and decoded by `llvm-objdump` 21.1.8 for
`riscv64` with the full RV64GC feature set (`--mattr=+c,+m,+a,+f,+d`), because
the SEC2 core on this part is the RISC-V variant (`0x841668` reads
`RISCV|BRFETCH`; `SWARM-REPORT-WP-A5.md` §3). `RVC` is a dense encoding, so
about 79% of *uniform random* 16-bit parcels still decode as some instruction;
the decisive statistic is how far decoded instructions chain.

| stream | parcels | `<unknown>` | longest valid run | first invalid |
|---|---:|---:|---:|---:|
| **app code** | 11 443 | **2 428 (21.2%)** | **31** | **+0x4** |
| uniform random (control) | 11 471 | 2 450 (21.4%) | 40 | +0xA |
| plaintext RISC-V control (RM `.fwimage`) | 7 552 | **0 (0.0%)** | **7 552** | none |

The app-code numbers are **indistinguishable from uniform random**; the
plaintext control never produces an invalid instruction at all. The mnemonic
mix tells the same story: the ciphertext's top five are `sd, fsd, fld, lw, ld`
(the densest compressed encodings), exactly the mix uniform random produces,
while the plaintext control is `addi, sd, ld, li, slli` — code.

Two consequences:

* Executing the region as-is, from its first byte, hits an invalid encoding at
  offset **+0x4** (after two coincidentally-valid parcels) and cannot run for
  more than 31 consecutive instructions anywhere in the image.
* "It disassembles as RISC-V" would require a coherent program: prologues,
  linked branches, a plausible mnemonic distribution, no illegal opcodes. None
  of that is present. (Whether the intended plaintext is RISC-V or the Falcon
  `fuc5` ISA does not change the answer: no plaintext ISA shows through.)

**MEASURED verdict (b):** garbage. The bytes are not a RISC-V program and cannot
be executed undecrypted.

## 4. (c) Headers: a copy descriptor around a Heavy-Secure body

What the container *does* say:

* `nvfw_bin_hdr` is a 24-byte envelope: magic `0x10DE`, version 1, sizes and
  offsets. It has **no compression field and no uncompressed-size field**
  (unlike the VBIOS `FALCON_UCODE_DESC` V2, which has `UncompressedSize`).
* `nvfw_hs_ucode_hdr` v2 names **two RSA-3072-sized signature slots**
  (`0x300` bytes = 2 × 384), the **patch site** (`0x7710` = DMEM offset `0x10`)
  where the driver writes the fuse-selected signature, and a 12-byte meta of
  `{fuse_ver 1, engine_id 1, ucode_id 3}`.
* `nvfw_hs_load_hdr` is a **copy descriptor**: four offsets/sizes, an app count
  and per-app code/data pairs. Its regions tile `data_size` exactly. It says
  "put these bytes here", not "these bytes are encoded thus".
* The unintelligible body starts at **data+0x100** — the `0x100` bytes before it
  are all zero (MEASURED) — a multiple of 16, and the app code region is
  `0x7600` = 1888 × 16 bytes exactly. The published analysis of this container
  format puts the AES-encrypted region at the same `0x100` boundary (SOURCE).

The same file's DMEM image (`os_data`, 24 576 B) is **plaintext**: 99.3% zero,
entropy 0.096, and it contains readable ASCII — `NVRNG engine`,
`NVPKA engine [RSA mode]`, `NVPKA engine [ECC mode]`, `CCC arithmetic error`.
So the container is not uniformly encoded: only the HS code region is
unintelligible.

The headers therefore do not carry an "encrypted" bit (the VBIOS V3 descriptor's
`vDesc` bit 2 is a different format and is not present here); the "HS"
designation is the declaration. That is consistent with the implementation:

* **SOURCE** — nova-core (`firmware/booter.rs`) parses exactly these headers,
  patches the signature into the image, DMA-loads `app0` into secure IMEM and
  starts the BROM. It contains **no decryption code**. The kernel docs
  (`gpu/nova/core/falcon.html`) say the HS binary's signature is verified by
  the hardware boot ROM.
* **SOURCE** — the published analysis of the same HS v2 booter format states
  the body "is AES-encrypted and is only decryptable inside the Falcon in HS
  mode", that the encrypted region starts at `0x100` (a multiple of 16), and
  that the debug variant decrypts with NVIDIA's public AES-128-ECB test key
  (key number 37).

## 5. The block structure proves ECB, and names the key scope

Entropy alone cannot separate a stream cipher from a block cipher. The block
structure can, and it is unambiguous:

| measurement | value |
|---|---|
| 16-B blocks | 1888 total, **1864 distinct, 17 repeated values, 24 excess** |
| 32-B blocks | 944 total, 2 repeated values |
| 64-B blocks | 472 total, **0 repeated** |
| trailing identical 16-B blocks | **5** (`9dbc2efa…2478d7` at blocks 1883–1887) |
| repeated blocks, uniform-random control | **0 of 1888** |

For a uniform 128-bit block space the chance of even one repeated 16-byte block
in 1888 samples is ~5 × 10⁻³³; the app code has **17**. The repeats are exact at
16-byte granularity and vanish at 32/64 bytes except where they are runs — the
signature of a **16-byte block cipher in ECB mode** (equal plaintext blocks give
equal ciphertext blocks). CBC and CTR cannot do this; neither can a compressor,
whose output would not repeat a block five times and stay uniform everywhere
else. The five identical trailing blocks are the encryption of **80 zero
plaintext bytes** (the image's zero padding).

The cross-image comparison fixes the *key scope*:

| pair | shared 16-B ciphertext blocks |
|---|---:|
| AD102 `booter_load` vs AD102 `booter_unload` | **68** |
| GA102 `booter_load` vs GA102 `booter_unload` | **241** |
| AD102 vs GA102 (any pair) | **0** |
| zero-padding block, AD102 load & unload | `9dbc2efa…2478d7` (same) |
| zero-padding block, GA102 load & unload | `af4206eb…28be57` (same) |

Same plaintext block + same key ⇒ same ciphertext block. Within a chip family
the two images share the key (68 / 241 shared blocks, including the zero block);
across families they share **nothing**, and the zero block differs. So the key
is **per chip family**, not per image, not per device — which is exactly what a
fused family key means, and it is why one file in linux-firmware serves every
AD102.

The AD102 zero block is **not** `717D1494 EACA317F F1061952 58B38377`, the
published AES-128-ECB(key 37, 0¹⁶) value of the *debug* booter. This is a
production image under the production key; the published note that "production
blobs show a different trailing pattern" is reproduced here.

## 6. The answer to "which key source would the ROM use"

The file contains **no key material**: the header's only selectors are
`fuse_ver 1, engine_id 1, ucode_id 3`, and the booter is a public,
driver-package blob. Decryption happens after the host has finished: the SEC2
boot ROM enters HS mode, decrypts the IMEM image, and only then lets the first
instruction run.

* **Key source:** the SEC2's **fuse-backed secret key store** — the heavy-secure
  ucode key, loaded into a Falcon crypto key slot by the HS entry code
  (`csecret`/`ckeyreg` in the published GA100/Tegra reconstruction), selected by
  the same `(engine_id, ucode_id)` pair the host writes into the BROM parameter
  registers (`(1,3)` for this image; INFERRED — the family scope is measured,
  the exact fuse-to-keyslot map is not). Production parts carry a secret fused
  key; debug-fused parts carry NVIDIA's public test key number 37 instead.
* **Not** the host, **not** the driver, **not** the firmware file, and **not** a
  per-device secret. One file in linux-firmware serves every AD102, so a
  per-device key could not decrypt it; the measured family split (68/241 shared
  blocks inside a family, 0 across families, different zero blocks) puts the
  scope at the chip family, which is what a fused family key means.
* The signature check and the decryption are the same hardware path: the HS
  entry routine decrypts the image and compares a Davies-Meyer MAC/signature
  before the first instruction runs (SOURCE: published HS entry reconstruction;
  kernel docs: "verified by the hardware").

**MEASURED / SOURCE verdict (c):** the container describes an opaque HS code
blob to be copied verbatim; the blob is AES-ECB ciphertext under a family-fused
key that only the SEC2 BROM can use.

## 7. What this changes for the launch

1. **Loading the bytes verbatim is correct.** WP-A5 r5c–r5g already DMA the
   exact `[0x478,0x7A78)` region and start SEC2. There is nothing to decrypt
   host-side and nothing to "fix up"; a host that *tried* to decrypt would be
   wrong, because it does not have the key.
2. **The "plaintext image" branch is closed.** WP-A5 §6 next-step 2 proposed
   that plaintext app code would make the ROM decrypt garbage. The bytes are
   ciphertext, so that hypothesis is refuted; the failure to produce a mailbox
   result lies in the halt timing, the BROM boot state, or the remaining
   `(engid,ucode)` variants — not in the image encoding.
3. **The IMEM read-back is expected to be opaque.** Secure pages answer
   `0xDEAD5EC1`; the ciphertext is supposed to be there and the decrypted code
   is supposed to be unreadable. That is not a defect.
4. **The load layout is confirmed from an independent driver.** nova-core loads
   `app0` alone (`data+0x100`, `0x7600`) to secure IMEM address 0 on GA102+ and
   does not load the zero `os_code` block. That is the layout r5e used
   (`app @ IMEM 0`), not the `os_code + app` layout of r5c/r5d.

## 8. Proven / inferred / not established

**MEASURED**

* Entropy 7.9949 bits/byte, IC 0.00390, incompressible, flat per-window.
* 21.2% invalid RISC-V parcels, longest valid run 31, first invalid at +0x4 —
  statistically identical to uniform random (21.4%, run 40) and unlike plaintext
  code (0%, run 7552).
* 17 repeated 16-byte blocks and 5 identical trailing blocks: a 16-byte block
  cipher in ECB mode over 80 bytes of zero padding.
* Key scope: shared blocks within a family (68 / 241), none across families
  (0), family-specific zero blocks.
* DMEM in the same container is plaintext with semantic ASCII strings.
* The headers contain no compression/encryption flag; they are a copy
  descriptor plus two RSA-3072 slots and `(fuse_ver, engine, ucode) = (1,1,3)`.

**SOURCE (external, matches the measurements)**

* HS v2 booter bodies are AES-encrypted and decryptable only inside the Falcon
  in HS mode; the encrypted region starts at 0x100; debug key is the public
  AES-128-ECB test key #37 with the zero-block value `717D1494…B38377`.
* nova-core loads the image verbatim and patches a signature; it has no
  decryption path; the BROM verifies HS ucode.
* The HS entry sequence decrypts with a key from a Falcon secret key slot and
  checks a MAC/signature before executing.

**INFERRED (stated, not proven here)**

* The cipher is AES-128 (16-byte block, NVIDIA's documented HS construction);
  ECB mode is measured, the "AES" name is the documented algorithm.
* The key slot is fed by fuses and selected by `(engine_id, ucode_id)`; the
  family scoping is measured, the exact fuse register is not.
* The zero trailing blocks are padding, not a repeated 16-byte constant; the
  published "trailing zero padding" description and the cross-image agreement
  make this the only coherent reading.

**NOT established**

* The plaintext itself (no key), hence whether the decrypted ISA on AD102 is
  RISC-V or Falcon `fuc5`.
* Whether the BROM ever ran in the WP-A5 attempts (halt timing is the
  instrument for that, not this analysis).
* Any per-image key difference inside one family: load and unload share a key
  as far as 16-byte block equality can tell.

## 9. Reproduce

```sh
python3 docs/nv-booter-evidence/analyse_booter_appcode.py \
        --root third_party/nvfw > /tmp/booter.out
diff -u docs/nv-booter-evidence/analyse_booter_appcode.out /tmp/booter.out
```

Requires the pinned blobs (`tools/nv/fetch-firmware.sh`) and LLVM's
`llvm-objcopy`/`llvm-objdump` with the RISC-V target for part (b); the entropy
and block measurements need only Python 3.

## 10. Sources

| what | where |
|---|---|
| the measurement tool and its output | `docs/nv-booter-evidence/analyse_booter_appcode.{py,out}` |
| container/load-header layout, measured | `src/include/nv_fw.h`, `src/kernel/nv_fw.c`, `docs/nv-gsp-firmware.md` §3.1 |
| the launch attempt this resolves | `SWARM-REPORT-WP-A5.md` §6; `docs/nv-plan.md` |
| driver-side parsing and verbatim load | NVIDIA open-gpu-kernel-modules, `nouveau/extract-firmware-nouveau.py`; Linux `drivers/gpu/nova-core/firmware/booter.rs` |
| HS root of trust / BROM verification | `Documentation/gpu/nova/core/falcon.html` |
| AES-encrypted HS body, key 37, `0x100` start, production trailing pattern | Consensus-Protocol/cmp170hx, `docs/unlock/falcon-and-booter.md`, `docs/appendix/artifacts.md` |
