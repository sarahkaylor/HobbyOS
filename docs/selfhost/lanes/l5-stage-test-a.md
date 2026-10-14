# Lane L5 — staging tool: test A transcript + verification (2026-10-14)

Host: workstation, 64 cores. Tool: `tools/selfhost-stage.sh` (branch
`selfhost/l5-acceptance-design`).

## Setup

Scratch image created WITHOUT the (agent-blocked) `mkfs.fat` binary — used
mtools `mformat` instead (same mtools family as the staging tool; verified a
real FAT16 BPB afterwards: bps=512 spc=8 fatsize=256 → FAT16).

```text
truncate -s 256M stage-test-a.img
mformat -i stage-test-a.img ::
```

The libcxx include subset is a gitignored extraction — not present in the
lane worktree — so it was copied read-only from the workstation repo
(`~/Documents/GitHub/HobbyOS/third_party/libcxx-21.1.8/.../include`) into
scratch, and the script's `LIBCXX_INC_DIR` override pointed at the mirrored
path.

## Run (full transcript, raw, abridged for the committed copy)

```text
$ LIBCXX_INC_DIR=$SCR/libcxx-tree/.../include tools/selfhost-stage.sh -f stage-test-a.img
selfhost-stage: replacing existing /SRC on .../stage-test-a.img
selfhost-stage: WARNING: Makefile.hobbyos absent (lands in a later wave) — staging without it

selfhost-stage: staged payload into .../stage-test-a.img
  payload: 2322 files, 30108339 bytes
  8.3-oversized basenames in payload: 1362 (informational for F8; LFN is proven)
  --- image report (mdir totals) ---
Total files listed:
      2726 files          30 108 302 bytes
                        232 194 048 bytes free

selfhost-stage: done
```

## Verification

### 1. LFN tree arrival (mdir recursive)

`mdir -i img -/ ::/SRC` walks the full tree. Structure and deep long-name
directories confirmed, e.g.:

```text
Directory for ::/SRC/src
Directory for ::/SRC/src/kernel           (files present: main.c, program_loader.c, virtio_blk.c, ...)
Directory for ::/SRC/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include
Directory for ::/SRC/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include/__bit
...
```

### 2. Content-integrity spot checks (mtype → sha256 vs source)

| staged file | on-image sha256 | source sha256 | result |
|---|---|---|---|
| `src/kernel/program_loader.c` | e00bd74c6a87 | e00bd74c6a87 | MATCH |
| `src/user/sh.c` | 2d0a8434fa24 | 2d0a8434fa24 | MATCH |
| `src/host/build_tu21_cksum_ref.sh` | fafa014f07eb | fafa014f07eb | MATCH |
| `.../libcxx/include/__config` | d7f72170b00d | d7f72170b00d | MATCH |
| `.../libcxx/include/__algorithm/copy_move_common.h` | 42e625eef508 | 42e625eef508 | MATCH |
| `tools/selfhost-seed-worktree.sh` | 15468dbc4e06 | 15468dbc4e06 | MATCH |
| `limine.conf` | (prefix check) | `timeout: 0\n...` | MATCH |

### 3. Count reconciliation

`find src -type f | wc -l` in the lane worktree = **613**; payload total
2322 = 613 (src) + 26 (tools) + 1 (BOOTAA64.EFI) + 1 (limine.conf) + 1681
(libcxx include) — exact arithmetic match. The 615-figure in the lane brief
counts the workstation tree, which carries two extra entries:
- `src/user/actual_qemu.ppm` — untracked screenshot byproduct (correctly
  absent from the git tree; must NOT be staged),
- `src/user_include/graphics/theme.h` — tracked on the workstation's newer
  `main` tip (0a3d073), absent from the wave-1 seed (64093f6-era). Version
  skew between the seed commit and workstation main — expected for this
  wave; parent owns the merge.

### 4. Idempotence

Re-run without `-f` on an image that already has `/SRC` → aborts with
"already contains /SRC — pass -f to replace it". With `-f` → `mdeltree` +
fresh copy → same payload totals. Both paths exercised; both exit codes
verified (1 and 0).

## Notable behaviors recorded for the harness/port lanes

- mtools 4.0.49 `mmd` does **not** create parent directories; `mcopy` does
  **not** create the destination's parent chain. The tool therefore walks
  path components itself (`mmkdir_p`).
- mtools `mdel -r` does not exist; recursive delete is `mdeltree`.
- `mcopy -s <dir> <dst>/./` copies the directory *itself* under its
  basename (`<dst>/<basename>`); giving a zero-length `mcopy -s` (some
  payloads) is fine.
- 1362 of 2322 staged basenames exceed FAT 8.3 → LFN write is required and
  demonstrably works (arrival shown above). Informational for F8; lane L3
  owns the in-OS read side.

## Raw artifacts

Uncommitted raw transcript: `~/.hermes/cache/scratch/l5/stage-test-a.transcript.txt`
(kept out of git per program evidence policy).
