# GB-062 — the intel-only FATBIG tail checksum: a signed-`char` comparison, not the block device

**Repro (from the ticket):** `[FATBIG] FAIL tail checksum clean (got=0x0000000000000800 want=0x0000000000000000)`
in `evidence/gb-057/t3intel_probe16_gb060_release_fired.log`; the same test passes on ARM in the same night's run.

## What FATBIG actually checks

`src/user/fatbig_test.c` (FATBIG_T.BIN):
- creates `/BIGFILE.BIN`, writes **2 MiB** of the position-dependent byte pattern `(p>>8) ^ (p&0xFF)` in 4 KiB chunks,
- reopens and asserts `lseek(SEEK_END) == 2 MiB` (passes),
- reads the **tail 4 KiB** and counts mismatches `bad`; the failing check is `bad == 0x0` (got `bad = 0x800` = 2048).

## The cause

`src/user/fatbig_test.c:95` compared:

```c
if ((chunk[i] & 0xFF) != (char)((p >> 8) ^ (p & 0xFF))) bad++;
```

The left side is an `int` in `0..255`. The right side is `(char)`, which is **signed on x86_64** and
**unsigned on ARM** — the ABI, not the device, is the intel-vs-ARM delta. A `(char)` with the high bit set
sign-extends to `-128..-1`, so it can never equal the `128..255` on the left. The tail pattern
`(p>>8) ^ (p&0xFF)` has bit 7 set in **exactly half** its bytes, which is why the count is `0x800` — half of
4096 — and why the file *size* is correct: nothing was corrupted on disk, the comparison was wrong.

**Verified on a host, all three readings matching the ticket:**

```
char is SIGNED   (x86_64) : as written in fatbig_test.c : bad=2048 (0x800) of 4096
                            with (unsigned char) cast  : bad=0 (0x0)
char is unsigned (ARM ABI): as written in fatbig_test.c : bad=0 (0x0)
```

## The fix

Compare as unsigned bytes on both sides:

```c
if ((unsigned char)chunk[i] != (unsigned char)((p >> 8) ^ (p & 0xFF))) bad++;
```

## What this doc previously said, and why it is wrong

An earlier version of this file concluded the defect was in the **intel NVMe block-device layer**
(`virtio_blk.c`'s `__x86_64__` branch, single-page `bounce_buf`, per-sector NVMe writes), reasoning that the
shared FAT16 code and user test are arch-agnostic. That reasoning was sound but the conclusion was not: an
NVMe data-path fault cannot produce **exactly half** of a tail, cannot be deterministic across runs, and
cannot explain why the same bytes pass on ARM. The half-tail number is the signature of a sign bit, not a
DMA boundary.

Kept as a record because the wrong version was merged and read; the correction is the useful part.

## A separate symptom, not part of this bug

The ticket also recorded `[FATBIG] FAIL short` in another run. That is a *different* symptom and is not
explained by this comparison: a short write points at the storage path and remains open.
