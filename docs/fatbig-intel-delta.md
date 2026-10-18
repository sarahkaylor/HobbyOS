# GB-062 — intel-vs-ARM delta in the FAT write path (FATBIG tail checksum)

**Repro (from the ticket):** `[FATBIG] FAIL tail checksum clean (got=0x0000000000000800 want=0x0000000000000000)` in `evidence/gb-057/t3intel_probe16_gb060_release_fired.log`; the same test passes on ARM in the same night's run.

## What FATBIG actually checks

`src/user/fatbig_test.c` (FATBIG_T.BIN):
- creates `/BIGFILE.BIN`, writes **2 MiB** of the position-dependent byte pattern `(p>>8) ^ (p&0xFF)` in 4 KiB chunks,
- reopens and asserts `lseek(SEEK_END) == 2 MiB` (passes),
- reads the **tail 4 KiB** and counts mismatches `bad`; the failing check is `bad == 0x0` (got `bad = 0x800` = 2048).

So exactly **half the tail read** (2048 of 4096 bytes) differs from the written pattern on intel, and the file *size* is correct — this is data corruption in the write/read-back, **not** a size/look-up defect (the >1 MiB directory-entry bug the test was created for is long fixed).

## The intel-vs-ARM delta — explained

The user test and the shared FAT filesystem code are **arch-agnostic**:
- `src/user/fatbig_test.c` — same on both arches (pure userspace writes/reads).
- `src/kernel/fat16.c` (the create/write/reopen path the test exercises) — shared C, no `__x86_64__`/arch branches in the write path.

The block device **beneath** FAT is where the arches diverge, in `src/kernel/virtio_blk.c`:

| arch | block backend | write path |
|---|---|---|
| **ARM** | VirtIO-blk **MMIO** (`#else` branch, `reg_write32`, virtqueue) | passes (`bad == 0`) |
| **intel (x86_64)** | **homemade NVMe driver on PCI** (`#ifdef __x86_64__` branch, `outl`/`inl`, `pci_write_config`, `nvme_...`) | 2048/4096 tail bytes wrong |

Concretely the intel `virtio_blk_write_raw` (virtio_blk.c:411) copies each of `count` 512-byte sectors into a **single-page `bounce_buf`** and issues a per-sector NVMe write (`cdw12 = 0` = one block), polling `submit_io_cmd` per sector. The ARM MMIO branch issues the same logical 512-byte requests through the virtio virtqueue.

## Conclusion / locus

The shared FAT16 + user-test logic is **not** the culprit — the same code passes on ARM. The defect is isolated to the **intel NVMe block-device layer** (`virtio_blk.c` `__x86_64__` branch): a >1 MiB tail write on NVMe corrupting ~half the bytes (the 2 MiB boundary is exactly where a single-PRP / data-page issue would surface). Because the delta is the device backend, the **fix and the intel T3 verification belong on the intel storage/ABI track (GB-057's intel-runtime lane), not in FAT16 or the user test.**

A host golden (src/host/fatbig_tail_test.c) locks the user-test + pattern round-trip on the host so any future intel regression is proven to be device-layer, not test/FAT logic.
