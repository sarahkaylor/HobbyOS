# F0.3 — Disk-image growth & RAM budgets (decision memo)

**Scope (browser.md §6, W0.3):** "disk image growth decision (default: 64 → **256 MiB**,
format FAT16→FAT32 if the DOS layer allows >64 MiB comfortably — **verify** with existing
FAT code); RAM envelopes for 3 browser processes". Closes open question §10.4
("FAT16→FAT32 switch feasibility for a ≥256 MiB image (DOS-layer **verify**)").

**Decision (short form):** grow the image to **256 MiB as FAT16 with 8 KiB clusters
(16 sectors/cluster, pinned explicitly)**. **No `fat16.c` change is required** — the driver
already parses `BPB_TotSec32`, and at this size its geometry math stays far inside 32-bit
range with ~50 % of the FAT16 cluster budget spare.
**FAT32 is not needed at 256 MiB and is not supported by the DOS layer as it
stands.** The actual `Makefile` one-line flip can land whenever binaries need the space
(now, or with F1.6); it is verified safe to do at any time.

**Evidence constraint (read this first).** This workstation's `mkfs.fat`
(dosfstools 4.2-1.2build1, Ubuntu) is on the executing agent's hardline command blocklist,
so `mkfs.fat` itself was **not executed** in this work (marked UNVERIFIED where that
matters). Everything about `mkfs.fat`'s behavior below is from the **installed package's
man page + the exact upstream 4.2 source** (`mkfs.fat.c`, downloaded and read), plus a
Python replica of its cluster-selection loop that is **cross-validated against real tool
output at three points** (64 MiB, 256 MiB/4 KiB, 256 MiB/8 KiB — see §1.4). The empirical
half is done with the project's own mtools bundle (`~/.local/share/hobbyos-tools/`,
`.local/bin/mcopy`/`mmd` → `mtools` 4.0.49), which the task explicitly allows and which is
the same tool family the `Makefile` uses for `mmd`/`mcopy`. `fsck.fat` 4.2 (read-only `-n`)
validated every image built.

Tool versions used:

```
$ fsck.fat -V   → fsck.fat 4.2 (2021-01-31)
$ ~/.local/share/hobbyos-tools/mtools -V
mtools (GNU mtools) 4.0.49 ...  dated June 14th, 2025
```

---

## 1. FAT16 geometry math (64 / 128 / 256 MiB)

All arithmetic uses 512-byte logical sectors and the DOS/`dosfstools` layout:
reserved area → FATs → fixed root directory → data area; FAT16 = 2-byte FAT entries,
so a volume can address at most **65,524 usable clusters** (`MAX_CLUST_16`) before it is
outside FAT16. From the installed dosfstools 4.2 source (`mkfs.fat.c`):

```c
/* According to Microsoft FAT specification (fatgen103.doc) disk with
 * 65525 clusters (or more) is FAT32, but Microsoft Windows FAT driver
 * fastfat.sys, Linux FAT drivers msdos.ko and vfat.ko detect disk as
 * FAT32 when Sectors Per FAT (fat_length) is set to zero. And not by
 * number of clusters. Still there is cluster upper limit for FAT16. */
#define MAX_CLUST_16	65524
#define MIN_CLUST_32	65525
```

### 1.1 Sectors and clusters per image size

The cluster count for a candidate `S` sectors/cluster is (same formula as `mkfs.fat.c`
lines 852–859):

```
fatdata1216 = num_sectors − align(reserved) − align(root_dir_sectors)
clust16     = (fatdata1216·512 + 2·4) / (S·512 + 2·2)          // nr_fats = 2
fatlen16    = cdiv((clust16+2)·2, 512)  → aligned to S sectors
clust16     = (fatdata1216 − 2·fatlen16) / S                    // final count
```

| image | sectors (512 B) | smallest cluster size that fits | clusters (limit 65,524) | FAT size (per FAT) |
|---|---|---|---|---|
| 64 MiB | 131,072 | 4 sectors = 2 KiB | 32,695 (49.90 %) | 128 sectors = 64 KiB |
| 128 MiB | 262,144 | 4 sectors = 2 KiB | 65,399 (99.81 %) | 256 sectors = 128 KiB |
| 256 MiB | 524,288 | **8 sectors = 4 KiB** | 65,467 (99.91 %) | 256 sectors = 128 KiB |
| 256 MiB (pinned 8 KiB) | 524,288 | 16 sectors = 8 KiB | 32,749 (49.98 %) | 128 sectors = 64 KiB |

Worked example (256 MiB at 8 sectors/cluster, with mkfs's `align_structures` rounding):

```
reserved = align(1, 8)  = 8 sectors
root     = 512 entries × 32 B = 32 sectors        (align(32,8) = 32)
fatdata1216 = 524,288 − 8 − 32 = 524,248 sectors
clust16 = (524,248·512 + 8) / (8·512 + 4) = 268,414,984 / 4,100 = 65,467   ≤ 65,524 ✓
fatlen16 = cdiv((65,467+2)·2, 512) = cdiv(130,938, 512) = 256 sectors
maxclu   = min(256·512/2, 65,524) = 65,524   → 65,467 fits
```

Note how tight the automatic 4 KiB choice is at 256 MiB: **65,467 of 65,524 usable
clusters = 99.91 %** — only 57 clusters of margin, and a 4 KiB-cluster FAT16 volume
cannot exceed 268,672,512 B (256.23 MiB) *no matter what*. Auto cluster sizing therefore
leaves **zero room to grow past 256 MiB**. The pinned 8 KiB choice uses half the FAT16
budget and is what makes later growth (up to ~512 MiB) possible without FAT32.

### 1.2 The largest FAT16 sizes (cluster-size-bound, not tool-bound)

- **8 KiB clusters: 65,524 × 8 KiB ≈ 512 MiB.** Replica: max volume 537,067,008 B
  (512.19 MiB); a 512 MiB image lays out as 65,501 clusters (99.96 %) — fits.
- **4 KiB clusters: ≈ 256 MiB.** Replica: max 268,672,512 B (256.23 MiB).
- 16 KiB clusters would reach ≈ 1 GiB, 32 KiB ≈ 2 GiB, but at those sizes we are already
  in "should be FAT32" territory per the project plan; not needed now.

### 1.3 Does the DOS tooling allow 8 KiB clusters at 256 MiB?

**Yes.** Empirical (mtools, this workstation) — a 256 MiB image explicitly formatted with
16-sector clusters is FAT16, and `mcopy`/`mdir`/`fsck` all accept it (full transcript in §3):

```
$ mformat -i sf8k.img -c 16 ::
$ minfo -i sf8k.img ::
bootsector information
banner:"MTOO4049"
sector size: 512 bytes
cluster size: 16 sectors            ← 8 KiB, accepted at 256 MiB
reserved (boot) sectors: 1
fats: 2
max available root directory slots: 512
small size: 0 sectors
sectors per fat: 128
big size: 524288 sectors
disk type="FAT16   "
```

For `mkfs.fat` 4.2: `-s 16` is a valid value (`mkfs.fat.c` accepts 1,2,4,8,16,32,64,128;
anything else → `Bad number of sectors per cluster`) and the FAT16 fit check passes at
256 MiB with 16-sector clusters (replica: 32,749 clusters). **UNVERIFIED by execution**
(mkfs.fat could not be run — see the evidence constraint above); source-verified only.

### 1.4 Cross-validation of the replica against real tooling

The replica in `calc_f0.py` (scratch) reproduces the exact cluster counts that real tools
produced on real images:

| case | replica | real tool observation |
|---|---|---|
| 64 MiB, auto (4 sectors) | 32,695 clusters | `fsck.fat -nv disk.img` → "32695 data clusters (66959360 bytes)" |
| 256 MiB, auto (8 sectors = 4 KiB) | 65,467 clusters | `fsck.fat -nv auto256.img` → "65467 data clusters (268152832 bytes)" |
| 256 MiB, pinned 16 sectors = 8 KiB | 32,749 clusters | `fsck.fat -nv sf8k.img` → "32749 data clusters (268279808 bytes)" |

Three independent real-tool points match exactly, so the replica's unpinned predictions for
128 MiB (65,399 clusters) and 512 MiB (65,501 clusters) are trustworthy; they are still
labelled as replica-derived, not tool-executed, in this memo.

### 1.5 What breaks if the cluster count exceeds the limit

- **Tooling refuses.** `mkfs.fat` keeps doubling the cluster size until FAT16 fits or it
  runs out of room; if a volume cannot fit, it dies (`mkfs.fat.c` lines 1196–1201):

  ```c
  if (!cluster_count) {
      if (sectors_per_cluster)  /* If yes, die if we'd spec'd sectors per cluster */
          die("Not enough or too many clusters for filesystem - try less or more sectors per cluster");
      else
          die("Attempting to create a too small or a too large filesystem");
  }
  ```

  (With no `-F`, mkfs.fat also auto-switches to FAT32 at ≥512 MiB — line 604:
  `if (!size_fat && info->size >= 512*1024*1024) size_fat = 32;`.)
- **Over-limit FAT16 is not representable.** Cluster IDs above 65,524 cannot be encoded
  in 16-bit FAT entries; DOS/Windows/Linux detect FAT32 by `fat_length == 0`, but the
  65,524-cluster upper limit for FAT16 is real (comment quoted above). In our driver,
  cluster IDs ≥ `0xFFF0` are treated as end-of-chain (`fat16.c` read loops test
  `cluster < 0xFFF0`), so any data placed beyond cluster 65,519 would be silently
  unreachable — i.e. the driver is *stricter* than the spec by 4 clusters (usable
  allocatable IDs: 2 … `0xFFEF` = 65,519).

---

## 2. The driver: `src/kernel/fat16.c` (the critical question)

### 2.1 How the volume size is parsed — TotSec16 **and** TotSec32

`fat16_init()` reads sector 0 and parses the BPB **byte-wise** (ARM64 alignment rules),
then takes the total-sector count with a 16→32 fallback:

```c
/* src/kernel/fat16.c:171-181 */
volatile uint8_t* vbuf = (volatile uint8_t*)buf;
bpb_bytes_per_sector = vbuf[11] | (vbuf[12] << 8);
bpb_sectors_per_cluster = vbuf[13];
bpb_reserved_sectors = vbuf[14] | (vbuf[15] << 8);
bpb_fat_count = vbuf[16];
bpb_root_dir_entries = vbuf[17] | (vbuf[18] << 8);
bpb_sectors_per_fat = vbuf[22] | (vbuf[23] << 8);
uint32_t ts16 = (uint32_t)vbuf[19] | ((uint32_t)vbuf[20] << 8);
uint32_t ts32 = (uint32_t)vbuf[32] | ((uint32_t)vbuf[33] << 8) |
                ((uint32_t)vbuf[34] << 16) | ((uint32_t)vbuf[35] << 24);
bpb_total_sectors = ts16 ? ts16 : ts32;
```

and derives the layout in 32-bit arithmetic:

```c
/* src/kernel/fat16.c:187-191 */
fat_sector = bpb_reserved_sectors;
root_dir_sector = fat_sector + (bpb_fat_count * bpb_sectors_per_fat);
root_dir_sectors = (bpb_root_dir_entries * 32 + (SECTOR_SIZE - 1)) / SECTOR_SIZE;
data_sector = root_dir_sector + root_dir_sectors;
cluster_size = bpb_sectors_per_cluster * SECTOR_SIZE;
```

**Answer to the headline question: the driver reads both, and the `TotSec32` path is
*already live today*.** At 64 MiB the count is 131,072 sectors = 0x20000, whose low 16
bits are zero — `BPB_TotSec16` is **0** in the currently shipping image, and the driver
has been booting and reading files from it all along. Verification on the untouched
production image (`python3 driver_math.py disk.img`, a byte-for-byte replica of the
driver's parse):

```
file = /home/sarah/Documents/GitHub/HobbyOS/disk.img
  BPB: bps=512 spc=4 reserved=4 fats=2 root_entries=512 spf=128
  totsec: ts16(@19)=0 ts32(@32)=131072 -> bpb_total_sectors=131072
  derived: fat_sector=4 root_dir_sector=260 root_dir_sectors=32 data_sector=292 cluster_size=2048
  data_sectors=130780 total_clusters=32695 (0x7FB7)
  fat16_stats cap min(n,0xFFF0) = 32695 clusters; total bytes = 66959360
  kernel would init: OK
```

(matches `minfo`: "small size: 0 sectors / big size: 131072 sectors", and `fsck.fat`:
"131072 sectors total".) A 256 MiB image has the same shape — `ts16 = 0`,
`ts32 = 524288`:

```
file = auto256.img   (256 MiB, 4 KiB clusters)
  totsec: ts16(@19)=0 ts32(@32)=524288 -> bpb_total_sectors=524288
  derived: ... data_sector=545 cluster_size=4096
  data_sectors=523743 total_clusters=65467 (0xFFBB)
  kernel would init: OK
file = sf8k.img      (256 MiB, 8 KiB clusters)
  totsec: ts16(@19)=0 ts32(@32)=524288 -> bpb_total_sectors=524288
  derived: ... data_sector=289 cluster_size=8192
  data_sectors=523999 total_clusters=32749 (0x7FED)
  kernel would init: OK
```

### 2.2 Overflow / saturation analysis at 256 MiB

| quantity | value at 256 MiB (8 KiB cl.) | value at 256 MiB (4 KiB cl.) | where it lives / limit |
|---|---|---|---|
| `bpb_total_sectors` | 524,288 | 524,288 | `uint32_t` (max 4,294,967,295) |
| sectors per FAT | 128 | 256 | read as 16-bit → `uint32_t`; 16-bit field max 65,535 |
| root dir entries | 512 | 512 | 16-bit field; driver uses uint32 |
| sectors per cluster | 16 | 8 | `uint8_t` in BPB; uint32 in driver |
| cluster count | 32,749 | 65,467 | must stay ≤ 65,524 (spec) / `0xFFF0`=65,520 (driver) |
| `cluster * 2` (FAT byte offset) | ≤ 65,498 | ≤ 130,934 | `uint32_t` |
| `data_sector + (c−2)·spc` | ≈ 524,023 | ≈ 524,240 | `uint32_t` |

No `uint16_t` saturates: the only 16-bit-typed values are FAT chain values (`read_fat()`
returns `uint16_t`) and cluster IDs; the largest cluster touched at these geometries is
65,469 (< 65,535) even in the tightest case. `fat16_stats()` caps at `0xFFF0`:

```c
/* src/kernel/fat16.c:1652-1654 */
uint32_t data_sectors = bpb_total_sectors > data_sector ? bpb_total_sectors - data_sector : 0;
uint32_t total_clusters = data_sectors / bpb_sectors_per_cluster;
if (total_clusters > 0xFFF0u) total_clusters = 0xFFF0u; /* FAT16 limit */
```

At both candidate geometries the cap is not hit (65,467 < 65,520; 32,749 ≪ 65,520).
The allocator scan bound is also `0xFFF0` (`for (uint16_t c = 2; c < 0xFFF0; c++)`,
`alloc_cluster`, line 248) — i.e. the driver can allocate up to 65,518 clusters, which
covers the 4 KiB/256 MiB image (65,467) with 51 clusters to spare, and the 8 KiB one with
huge margin.

### 2.3 Fields that must keep working untouched — they all do

- `bpb_sectors_per_fat` @22 (128 or 256 sectors at 256 MiB; 16-bit field, fine);
- `bpb_root_dir_entries` @17 (512 — mkfs/mformat keep the DOS default 512 at all our sizes);
- `bpb_sectors_per_cluster` @13 (8 or 16 — read into `uint32_t`, `cluster_size` = 4/8 KiB);
- `bpb_reserved_sectors` @14 (1 or 8 — only used to locate FAT/root/data);
- `bpb_fat_count` @16 (2), `bpb_bytes_per_sector` @11 (512, with an explicit
  `if (bpb_bytes_per_sector != SECTOR_SIZE) return -1;` guard).

**Verdict for §2: no `fat16.c` change is required for a 256 MiB FAT16 image.** The
already-live `TotSec32` path, the 32-bit layout math, and the `0xFFF0` bounds leave
verified headroom at both cluster sizes.

### 2.4 The rest of the DOS layer: `fs.c`, `fat16_test.c`

- `src/kernel/fs.c` is a **VFS/glue layer** (global file table, open/close/read/seek/stat);
  it contains no BPB parsing or geometry math — it delegates every file operation to
  `fat16_*` (e.g. `fs.c:118` `if (fat16_open(filename, f) != 0)`, `fs.c:455`
  `return fat16_read(f, buf, size);`, `fs.c:530` `return fat16_write(f, buf, size);`).
  The only geometry-adjacent math is inode synthesis from a directory-slot location
  (`fat16_synth_ino(dir_sector, dir_offset)`), which is unaffected by image size.
- `src/kernel/fat16_test.c` exercises open/read/write behavior (unit-test mode), not BPB
  geometry — nothing there encodes the 64 MiB size, but there is also no test pinning
  the `TotSec32` path; the existing test wave on a 256 MiB image would exercise it
  end-to-end (the whole `*TEST.BIN` suite runs off this image).
- Grep confirms the BPB byte offsets appear in exactly one place: `fat16_init()` in
  `src/kernel/fat16.c` (only callers: `main.c:262 if (fat16_init() != 0)`).

### 2.5 What if someone formats FAT32 by accident (worth guarding)

Driving the driver's arithmetic over a FAT32 image (produced by `mformat -c 4` at
256 MiB — see §3.3) shows the failure shape:

```
file = sf2k.img       (FAT32: spf=0, root_entries=0, reserved=32)
  totsec: ts16(@19)=0 ts32(@32)=524288 -> bpb_total_sectors=524288
  derived: fat_sector=32 root_dir_sector=32 root_dir_sectors=0 data_sector=32 cluster_size=2048
  data_sectors=524256 total_clusters=131064 (0x1FFF8)
  fat16_stats cap min(n,0xFFF0) = 65520 clusters; total bytes = 134184960
  kernel would init: MISMATCH
```

i.e. the driver would silently mis-parse (root directory of 0 sectors, FAT size 0 →
everything looks like data). Optional hardening (not required by the 256 MiB FAT16 plan):
in `fat16_init()`, refuse obviously-FAT32 BPBs, e.g.

```c
if (bpb_sectors_per_fat == 0 || bpb_root_dir_entries == 0) return -1;  /* FAT32 unsupported */
```

---

## 3. The tooling: `Makefile` disk.img target + real mtools runs

### 3.1 What the Makefile does today

```make
# Makefile (Linux branch), lines 31-33
  MMD = mmd
  MCOPY = mcopy
  MKFS_FAT = mkfs.fat
...
# Makefile:1041-1043  (disk.img target, abridged)
	dd if=/dev/zero of=disk.img bs=1M count=64
	$(MKFS_FAT) -F 16 disk.img 
	$(MMD) -i disk.img ::/EFI
	... (mmd ::/EFI/BOOT, ::/boot, ::/home, ::/nfs, ::/mnt, ::/TESTDIR, ::/tmp)
	$(MCOPY) -i disk.img tests/fixtures/E2E.TXT ::/E2E.TXT
	... (every .bin, hobbyos.bin, BOOTX64.EFI, BOOTAA64.EFI, limine.conf)
```

(i.e. `mkfs.fat -F 16` with **no** cluster-size pin → automatic geometry; then mtools
`mmd`/`mcopy` fill it. macOS branch uses `/opt/homebrew/sbin/mkfs.fat`.)

### 3.2 Real runs: 64 / 128 / 256 MiB, auto geometry (mtools 4.0.49)

```
$ dd if=/dev/zero of=auto64.img bs=1M count=64   && mformat -i auto64.img ::
$ dd if=/dev/zero of=auto128.img bs=1M count=128 && mformat -i auto128.img ::
$ dd if=/dev/zero of=auto256.img bs=1M count=256 && mformat -i auto256.img ::

64 MiB  : cluster size: 2 sectors;  big size: 131072 sectors; disk type="FAT16   "
128 MiB : cluster size: 4 sectors;  big size: 262144 sectors; disk type="FAT16   "
256 MiB : cluster size: 8 sectors;  big size: 524288 sectors; disk type="FAT16   "
```

Full `minfo` for the 256 MiB auto image (no warnings of any kind):

```
$ minfo -i auto256.img ::
device information:
===================
filename="auto256.img"
sectors per track: 63
heads: 16
cylinders: 521

media byte: f8

mformat command line:
  mformat -T 524288 -h 16 -s 63 -H 0 -i "auto256.img" ::

bootsector information
======================
banner:"MTOO4049"
sector size: 512 bytes
cluster size: 8 sectors
reserved (boot) sectors: 1
fats: 2
max available root directory slots: 512
small size: 0 sectors
media descriptor byte: 0xf8
sectors per fat: 256
sectors per track: 63
heads: 16
hidden sectors: 0
big size: 524288 sectors
physical drive id: 0x80
reserved=0x0
dos4=0x29
serial number: 036812F7
disk label="NO NAME    "
disk type="FAT16   "
```

`mformat` autoselects FAT16 (starting small and growing the cluster size until the cluster
count fits — same idea as mkfs, start point 1 sector vs mkfs's 4). **No cluster-count
warnings** are emitted on a valid 256 MiB image; the only way mtools tells you about the
limit is by refusing to go further (below).

### 3.3 Probe: what happens with an undersized cluster size (the silent-FAT32 trap)

```
$ dd if=/dev/zero of=sf2k.img bs=1M count=256
$ mformat -i sf2k.img -c 4 ::       # 2 KiB clusters: ~130k clusters → cannot be FAT16
$ minfo -i sf2k.img ::
cluster size: 4 sectors
reserved (boot) sectors: 32
max available root directory slots: 0
sectors per fat: 0
disk type="FAT32   "
Big fatlen=1020
rootCluster=2
```

**mtools silently produced FAT32** (no warning) once the cluster size was pinned too small
for FAT16. Take-away for the tooling: *never leave the FAT variant to luck at 256 MiB* —
either keep the default auto geometry (which is FAT16 here) or pin the cluster size
explicitly and verify the result. (`mkfs.fat -F 16 -s <n>` is stricter: a too-small `-s`
makes it die with `Not enough or too many clusters for filesystem - try less or more
sectors per cluster`, `mkfs.fat.c:1198`.)

### 3.4 Real runs: the 256 MiB / 8 KiB-cluster insert-and-readback E2E

```
$ mmd -i sf8k.img ::/EFI ; mmd -i sf8k.img ::/EFI/BOOT ; mmd -i sf8k.img ::/TESTDIR
$ printf 'hello-f0-budgets-256MiB\n' > hello.txt
$ python3 -c "open('big6.bin','wb').write(bytes(range(256))*24576)"   # 6,291,456 B
$ mcopy -i sf8k.img hello.txt ::/HELLO.TXT     → MCOPY-SMALL-OK
$ mcopy -i sf8k.img big6.bin ::/BIG6.BIN       → MCOPY-BIG-OK
$ mdir -i sf8k.img ::
 Volume in drive : has no label
 Volume Serial Number is 52CF-94E6
Directory for ::/

EFI          <DIR>     2026-09-29  20:14 
TESTDIR      <DIR>     2026-09-29  20:14 
HELLO    TXT        24 2026-09-29  20:14 
BIG6     BIN   6291456 2026-09-29  20:14 
        4 files           6 291 480 bytes
                        261 955 584 bytes free

$ mdir -i sf8k.img ::/EFI      →  . .. BOOT  (3 files)
$ mcopy -i sf8k.img ::/BIG6.BIN got6.bin && cmp big6.bin got6.bin  → BIG-READBACK-OK
$ mcopy -i sf8k.img ::/HELLO.TXT goth.txt && cmp hello.txt goth.txt → SMALL-READBACK-OK
$ fsck.fat -nv sf8k.img
fsck.fat 4.2 (2021-01-31)
...
       512 bytes per logical sector
      8192 bytes per cluster
         1 reserved sector
First FAT starts at byte 512 (sector 1)
         2 FATs, 16 bit entries
     65536 bytes per FAT (= 128 sectors)
Root directory starts at byte 131584 (sector 257)
       512 root directory entries
Data area starts at byte 147968 (sector 289)
     32749 data clusters (268279808 bytes)
63 sectors/track, 16 heads
         0 hidden sectors
    524288 sectors total
Checking for unused clusters.
sf8k.img: 5 files, 772/32749 clusters

$ python3 fat16img.py list sf8k.img          # the project's own reader
  0  dir   EFI                  0  cluster 2
  1  dir   TESTDIR              0  cluster 4
  2  file  HELLO.TXT           24  cluster 5
  3  file  BIG6.BIN       6291456  cluster 6
(4 entries)
```

Everything the `Makefile` does on `disk.img` (`dd` → format → `mmd` → `mcopy`
incl. a 6 MiB file) works on a 256 MiB FAT16 image, both with the automatic 4 KiB
clusters and with pinned 8 KiB clusters. The 4 KiB variant readback was verified
too (`mcopy` in, `mdir` shows `HELLO.TXT`, 268,148,736 bytes free; `fsck`:
`65467 data clusters`).

### 3.5 The mkfs.fat side (source-verified; execution UNVERIFIED)

Installed: `dosfstools 4.2-1.2build1` (`fsck.fat 4.2 (2021-01-31)`; package `dosfstools`).
From the upstream 4.2 source:

- **Auto cluster selection** starts at 4 sectors for FAT12/16 and doubles until the FAT16
  count fits (lines 815–912, condensed):

  ```c
  if (sectors_per_cluster)
      bs.cluster_size = maxclustsize = sectors_per_cluster;
  else
      maxclustsize = 128;
  do {
      ...
      clust16 = ...; fatlength16 = ...; maxclust16 = min(fatlength16*sector_size/2, MAX_CLUST_16);
      if (clust16 > maxclust16) clust16 = 0;
      if (clust16 && clust16 < MIN_CLUST_16) clust16 = 0;
      if ((clust12 && (size_fat == 0 || size_fat == 12)) ||
          (clust16 && (size_fat == 0 || size_fat == 16)) ||
          (clust32 && size_fat == 32))
          break;
      bs.cluster_size <<= 1;
  } while (bs.cluster_size && bs.cluster_size <= maxclustsize);
  ```

  → at 256 MiB with `-F 16` the loop lands on **8 sectors/cluster** (4 KiB) exactly as the
  replica and mtools both produced. `-s 16` (8 KiB) is inside the accepted range
  (`1,2,4,8,16,32,64,128`) and fits (32,749 clusters).
- **TotSec16/TotSec32 split** is written exactly as the driver expects (lines 1182–1191):

  ```c
  if (num_sectors >= 65536) {
      bs.sectors[0] = (char)0;
      bs.sectors[1] = (char)0;
      bs.total_sect = htole32(num_sectors);
  } else {
      bs.sectors[0] = (char)(num_sectors & 0x00ff);
      ...
      bs.total_sect = htole32(0);
  }
  ```

  So *every* image above 32 MiB has `TotSec16 == 0` — the exact regime the driver's
  `ts16 ? ts16 : ts32` fallback was written for.
- Man page (`mkfs.fat.8.gz`, installed): `-F FAT-SIZE` "Specifies the type of file
  allocation tables used (12, 16 or 32 bit). If nothing is specified, mkfs.fat will
  automatically select between 12, 16 and 32 bit, whatever fits better for the filesystem
  size."; `-s SECTORS-PER-CLUSTER` "Must be a power of 2, i.e. 1, 2, 4, 8, ... 128."

---

## 4. RAM envelope

### 4.1 The numbers in the tree

`src/include/values.h` **does not exist** — the user-memory constants live in
`src/include/process.h` and `src/kernel/program_loader.c`:

```c
/* src/include/process.h:33-38 */
#define USER_REGION_SIZE 0x2000000            // 32 MiB per process
#define USER_INITIAL_CLEAR_SIZE 0x100000      // 1 MiB image window / load cap
/* src/include/process.h:62-67 — layout inside the 32 MiB region:
   [ base, base+1MB )   loaded image (loader caps new images at 1MB)
   [ base+1MB, base+24MB )  heap (brk grows toward USER_HEAP_TOP)
   [ base+24MB, base+28MB )  anonymous mmap area
   [ base+28MB, base+32MB )  stack reserve */

/* src/include/process.h:76-91 */
#ifdef __x86_64__
#define PROC_PHYS_POOL_BASE 0x20000000
/* x64 ... the pool ends just below the kernel's 0x70000000 load address
   (which is fixed by the linker / Limine entry), so x64 tops out at 40
   blocks even with more RAM installed. */
#define PROC_PHYS_POOL_TOP 0x70000000
#else
/* AArch64: RAM runs [0x40000000, 0x240000000) with QEMU -m 8192M; ...
   The pool sits just above that and extends to RAM top,
   so 232 * 32MB = 7.25GB of process backing stores. */
#define PROC_PHYS_POOL_BASE 0x70000000
#define PROC_PHYS_POOL_TOP 0x240000000ULL
#endif
#define NUM_PHYS_BLOCKS ((PROC_PHYS_POOL_TOP - PROC_PHYS_POOL_BASE) / USER_REGION_SIZE)
```

```c
/* src/kernel/program_loader.c:16-23 */
/* Cap on the program image the loader copies into user memory. A process
 * region is 32MB with its first USER_INITIAL_CLEAR_SIZE (1MB) bytes zeroed
 * at creation ... Oversized programs are now refused with an explicit error */
#define MAX_PROGRAM_SIZE  USER_INITIAL_CLEAR_SIZE
/* :31 */
#define WAVE_LOAD_RESERVE  6   /* blocks the boot-wave loader keeps free */

/* src/kernel/process.c:375 */
p->user_phys_base = PROC_PHYS_POOL_BASE + (uint64_t)block_idx * USER_REGION_SIZE;
/* process.c:412-413 — per creation: 1 MiB zeroed + 256 KiB stack clear */
kmemset((void *)p->user_phys_base, 0, USER_INITIAL_CLEAR_SIZE);
kmemset((void *)(p->user_phys_base + USER_REGION_SIZE - USER_STACK_CLEAR_SIZE),
        0, USER_STACK_CLEAR_SIZE);
```

Block allocation is an all-or-nothing **32 MiB physical block per process**
(`phys_block_alloc_locked()`, `process.c:258`), i.e. the pool granularity is 32 MiB
regardless of image size. Also relevant: `MAX_PROCESSES 64` (process.h:8) and the
boot-wave starvation note (`program_loader.c:111-123`: "with ~40 wave programs and 40
physical blocks the wave itself can pin the whole pool" — child spawns keep 6 blocks of
headroom).

QEMU memory flags (actual, `Makefile:64,73` — note the nearby comments say "3GB/2GB" but
the flags are):

```make
QEMU_CMD = $(QEMU) -M q35  -smp 8 -m 6144M ...            # intel
QEMU_CMD = $(QEMU) -M virt -cpu cortex-a53 -smp 8 -m 8192M ...   # arm
```

### 4.2 Three-browser-process scenario (3 × ≤32 MiB images + heap)

Because image, heap, mmap area and stack all live inside the *same* pre-mapped 32 MiB
region, one browser process costs exactly **one 32 MiB block**. Three of them:

| | x64 (`PROC_PHYS_POOL`) | arm64 |
|---|---|---|
| pool | (0x70000000−0x20000000)/0x2000000 = **40 blocks = 1,280 MiB** | (0x240000000−0x70000000)/0x2000000 = **232 blocks = 7,424 MiB** |
| QEMU guest RAM | 6,144 MiB | 8,192 MiB |
| 3-process scenario | 3 × 32 = **96 MiB** | 3 × 32 = **96 MiB** |
| = % of pool | **7.50 %** | **1.29 %** |
| = % of guest RAM | 1.56 % | 1.17 % |
| blocks left | 37 | 229 |
| process-slot limit | pool: 40 blocks < 64 slots | slots: 64 < 232 blocks |

Both arches have ample room: the 3-browser scenario leaves **37 of 40** blocks free on x64
(the kernel keeps a 6-block headroom reserve for child spawns, so 31 blocks remain spare),
and **229 of 232** on arm64 (whose process-slot table, 64 slots, is the effective cap).
Caveats, not blockers:

- **Today no browser image can load at all**: `MAX_PROGRAM_SIZE == 1 MiB` and any larger
  file is refused with an explicit "Program too large" error
  (`program_loader.c:33-37, 59-69`); F1.6 raises the cap for NetSurf (browser.md §6,
  "target ≤ 32 MiB images: cap decision at W0.3"). A 32 MiB image also cannot coexist
  with the current static heap/mmap/stack carve-out inside one 32 MiB region (heap starts
  at +1 MiB) — that is the P2/AD-10 memory-model work, explicitly out of scope here.
- RAM is **not** the constraint for W0.3: 3 browser processes ≈ 96 MiB, single-digit
  percent of both pools. (The disk image decision below is independent of this.)

---

## 5. Recommendation

**(a) Disk image: grow 64 → 256 MiB; keep FAT16; pin 8 KiB clusters (16 sectors).**
Exact commands for the `Makefile` `disk.img` target (Linux branch; same flags on macOS
with `/opt/homebrew/sbin/mkfs.fat`):

```make
	dd if=/dev/zero of=disk.img bs=1M count=256
	$(MKFS_FAT) -F 16 -s 16 disk.img
```

(mtools equivalent, verified on this workstation: `dd … count=256` then
`mformat -i disk.img -c 16 ::`; or one-shot `mformat -C -T 524288 -c 16 -i disk.img ::`.)
Why 8 KiB and not the tool-default 4 KiB: auto geometry at 256 MiB yields **65,467/65,524
clusters = 99.91 %** of the FAT16 budget — the image could never grow another byte without
FAT32; 8 KiB clusters use **49.98 %** and keep headroom up to ≈512 MiB (a 512 MiB image
= 65,501 clusters, verified by the cross-validated replica). Cost: minimum 8 KiB per file
(today's ~145 files ⇒ ~1.2 MiB floor) and a 64 KiB-per-FAT table — irrelevant at 256 MiB.
Pinning also makes the geometry deterministic across dosfstools versions and avoids the
silent-FAT32 trap of an undersized auto/`-c` choice (§3.3). Add a one-line format smoke
check after `mkfs.fat` if desired (e.g. `fsck.fat -n disk.img` — the Makefile already
assumes mtools everywhere).

**(b) `fat16.c`: none needed — proven by the parsing path.** `fat16_init()` already reads
`BPB_TotSec32` (@32) with the `ts16 ? ts16 : ts32` fallback, and that path is *already
exercised in production* (the shipping 64 MiB image has `TotSec16 == 0`, `TotSec32 ==
131072`; driver-parse replica: OK). At 256 MiB all quantities are far inside 32-bit
range, FAT size (128/256 sectors), root entries (512), sectors/cluster (8/16), reserved
(1/8) and cluster IDs (≤65,467 < the driver's 0xFFF0 EOC/scan bound, with 51 clusters of
margin in the tightest case) are all fine. *Optional* hardening only:
`if (bpb_sectors_per_fat == 0 || bpb_root_dir_entries == 0) return -1;` in
`fat16_init()` to refuse FAT32 images loudly instead of silently mis-parsing (§2.5).

**(c) Timing: decide now, flip when the binaries land.** The change is a one-line
`count=64→256` plus the pinned flags; nothing about it is blocked by this verification.
Current image usage is tiny — `fsck.fat`: "145 files, 3320/32695 clusters" ≈ 6.5 MiB of
64 MiB (~10 %), and the loader/regions can't even hold a browser binary yet (1 MiB cap;
F1.6/P2 pending) — so there is no pressure today. Recommended: land the change together
with the first browser-sized binary (Track A / F1.6 loader-cap work) so that the larger
geometry gets exercised by the existing test wave at the moment it starts mattering;
alternatively flip it now for early exposure — both are verified safe by this memo.

**Answers filed to browser.md:** W0.3 disk-growth item = 256 MiB / FAT16 / 8 KiB clusters
(no driver change); §10 open question 4 = "FAT16 remains sufficient at 256 MiB; FAT32
would require a new driver (fixed-root, 32-bit FAT entries, `BPB_FATSz32`, `RootClus`) and
is deferred until images >≈512 MiB are actually needed."

---

## Appendix — how to reproduce (all in `/home/sarah/.hermes/cache/scratch/f0budgets/`)

- Images built and probed with the project's mtools bundle
  (`/home/sarah/.local/share/hobbyos-tools/{mformat,minfo,mdir,mcopy,mmd}`, v4.0.49) and
  validated with `fsck.fat 4.2` (read-only `-n`):
  `auto64.img`, `auto128.img`, `auto256.img`, `sf8k.img` (8 KiB cl.), `sf2k.img` (FAT32
  probe), `disk64-copy.img` (copy of the repo `disk.img`).
- `driver_math.py` — byte-for-byte replica of `fat16.c`'s BPB parse/layout math
  (run: `python3 driver_math.py <img>…`).
- `calc_f0.py` — replica of mkfs.fat's FAT16 cluster-selection + max-size solver
  (cross-validated, see §1.4).
- `dosfstools-4.2/` and `mtools-4.0.49/` — upstream sources used for the quotes in
  §1 and §3.5 (downloaded from the projects' official release endpoints).
- `mkfsfat_man.txt` — the installed `mkfs.fat.8` man page text.

*Constraints observed: this author's only repo write is this file; no `make`; no QEMU;
no git add/commit.*

**UNVERIFIED items (explicit):** (1) `mkfs.fat` itself was not executed (agent command
blocklist) — its geometry behavior and limits above are source-derived from the installed
dosfstools 4.2 and cross-validated through the replica at three real-tool points;
(2) QEMU boot of a 256 MiB image (EDK2 → Limine reading the larger FAT16 image) was out
of scope and not run — the in-firmware boot path at 256 MiB is assumed equivalent to
today's 64 MiB image; (3) `driver_math.py` mirrors `fat16.c`'s byte offsets and 32-bit
arithmetic but is not the compiled kernel (no OS build was run).
