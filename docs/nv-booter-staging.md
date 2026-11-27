# Staging a booter boot: the one path, and what it asserts

**Scope.** Any boot whose verdict is about a *specific* booter container — a
candidate from the vendor bindata, a rebuilt blob, the unload image, a
bootloader-only container — goes through `tools/nv/sec2-boot-lane.sh`. Nothing
else stages a named booter, and nothing else prints the hash of the image the VM
is about to run.

Why this page exists: the round-4 review of `t_9bf9bc0c` found that the
`NV_BOOTER_FILE=… sec2-boot-lane.sh` recipe quoted in that card's survey worked
only in the lane's own worktree. The tree shipped on the integration branch had
no `tools/nv/sec2-boot-lane.sh` at all, and the copies that did exist staged the
first globbed `third_party/nvfw/ad102/gsp/booter_load-*.bin` — the 535 control.
A boot from one of those reads `0x002B0009` for a candidate it never staged, and
no reader can tell from the log.

## 1. The call

```
NV_BOOTER_FILE=<container> \
NV_EXPECT_BOOTER_SHA256=<64 hex> \
tools/nv/sec2-boot-lane.sh <count> <tag>
```

| variable | meaning |
|---|---|
| `NV_BOOTER_FILE` | the container to stage. Unset means the first globbed `third_party/nvfw/ad102/gsp/booter_load-*.bin`; the lane says so in a row, because that default is the 535 control, not necessarily the candidate. |
| `NV_EXPECT_BOOTER_SHA256` | pin. A mismatch refuses before the upload and before any `ssh` to r1. `NV_BOOTER_SHA256` is the old spelling and is still accepted; if both are set and disagree the lane refuses. |
| `<count>` | boots to run. **`0` is the dry run**: preflight, staging, and read-back verification, with no r1 call at all. |
| `<tag>` | names the serial logs `evidence/nv/<tag>{a,b,…}.log`. |

The four firmware roles are staged from the tree: `BOOTER.BIN` (the named
container), `LOADER.BIN`, `GSPRM.BIN`, `UNLOAD.BIN`. A missing optional file
prints `WARNING: <role> not staged: no file at '<path>'` instead of silently
leaving the previous boot's file in the image.

## 2. What it prints, and what each row means

Three dry runs on real files, verbatim (run from a tree whose `fixtures/` holds
a copy of `ad102/gsp/booter_load-570.144.bin`; the three `WARNING: <role> not
staged` rows for firmware this tree does not have are omitted from this block
only):

```
$ NV_BOOTER_FILE=../fixtures/booter570.bin \
  NV_EXPECT_BOOTER_SHA256=8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a \
  tools/nv/sec2-boot-lane.sh 0 rightpin
booter staged: 8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a size=57720 path=../fixtures/booter570.bin
booter pin: 8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a (NV_EXPECT_BOOTER_SHA256)
booter header: magic=0x10de v=1 bin_size=57856 data_off=0x378 data_size=0xde00 load_header=0x354+0x24 os_code=0x0+0x100 os_data=0x7e00+0x6000 napps=1
booter geometry: booter-class data_off=0x378 data_size=0xde00 imem=0x478+0x7d00 dmem=0x8178+0x6000 pad=0x0 bound=0xe178 verdict=TILES-THE-FILE
[sec2-lane] firmware staged: 1 file(s)
[sec2-lane] booter staged-verified: sha256=8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a size=57720 image=::/NVIDIA/GSP/BOOTER.BIN
[sec2-lane] vm check: skipped (count=0 dry run; r1 was not contacted)
```

`sha256sum ../fixtures/booter570.bin` is the same 64 hex digits, which is the
whole point of the first row.

* `booter staged:` is the file on disk: `sha256sum` and `stat -c %s` of
  `NV_BOOTER_FILE`. This is the row a report quotes.
* `booter pin:` is the expected sha in force, or `none`.
* `booter header:` is the parsed `nvfw_bin_hdr` + HS header + load header.
* `booter geometry:` is the bound the container's **own** load header accounts
  for. `verdict=TILES-THE-FILE` means the IMEM and DMEM extents the guest
  derives (`nv_gsp.c`: `imem = data_off + app[0].code_off`,
  `dmem = data_off + os.data_off`) meet exactly and end where the data region
  ends.
* `booter staged-verified:` is read back **out of `disk.img`** and hashed again.
  If this row is absent, nothing proved the image holds the bytes.

A refusal names both values and exits before the upload:

```
$ NV_BOOTER_FILE=../fixtures/booter570.bin \
  NV_EXPECT_BOOTER_SHA256=0000000000000000000000000000000000000000000000000000000000000000 \
  tools/nv/sec2-boot-lane.sh 0 wrongpin
booter staged: 8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a size=57720 path=../fixtures/booter570.bin
booter pin: 0000000000000000000000000000000000000000000000000000000000000000 (NV_EXPECT_BOOTER_SHA256)
booter header: magic=0x10de v=1 bin_size=57856 data_off=0x378 data_size=0xde00 load_header=0x354+0x24 os_code=0x0+0x100 os_data=0x7e00+0x6000 napps=1
booter geometry: booter-class data_off=0x378 data_size=0xde00 imem=0x478+0x7d00 dmem=0x8178+0x6000 pad=0x0 bound=0xe178 verdict=TILES-THE-FILE
booter REFUSED: staged sha256 8b293e19b637c5e22c87a2428d1c71bb13e0904e8a88ac6b3c6c1f2679c6e37a -- != NV_EXPECT_BOOTER_SHA256 0000000000000000000000000000000000000000000000000000000000000000 -- refusing to launch on the wrong image
[sec2-lane] FATAL: the staged image is not the pinned candidate; refusing to spend a boot on it
$ echo $?
4
```

## 3. The geometry rule

`tools/nv/booter_preflight.py` is the gate, and it derives the bound from the
container rather than from a constant. For a container with a data region:

```
bound = data_off + data_size                 must equal the file's size
regions of the load header                    os_code, app[i].code, os_data,
                                              app[i].data must tile [0, total)
                                              contiguously, from offset 0
data_size - total                             is padding: <= 0x100 B, dword-
                                              aligned, all zero (booter_unload
                                              has 0x100)
imem = data_off + app[0].code_off             imem + imem_len == dmem
dmem = data_off + os.data_off                 dmem + dmem_len == data_off+total
```

For a **bootloader-only** container (`data_size == 0`, the case
`docs/nv-no-rm-path.md` §6 names) the bound is its own `os_code` extent:

```
bound = os_code_off + os_code_size            must equal the file's size
```

`0x7700 + 0x6000` is the 535-generation booter's split, not a property of the
format; a lane that uses it refuses a legitimate bootloader-only image and — on a
different generation — accepts a file it then DMA-reads past the end of. Both
directions are refused here.

## 4. Refusals

| exit | row says | when |
|---|---|---|
| 2 | `booter REFUSED: booter <path> -- does not exist` | absent or unreadable `NV_BOOTER_FILE`, or no default glob |
| 3 | `booter REFUSED: <field> -- <why>` | not a container, no load header, bound ≠ file size, regions do not tile, non-zero padding, IMEM/DMEM do not meet |
| 4 | `booter REFUSED: staged sha256 <got> -- != NV_EXPECT_BOOTER_SHA256 <want>` | pin mismatch, before the upload |
| 5 | `booter REFUSED: NV_EXPECT_BOOTER_SHA256='…' -- is not a 64-digit hex sha256` | a malformed pin must not read as a match; also two disagreeing pins |

Every one of these happens before the first `r1()` call, so a refusal costs no
VM and no upload. `count=0` never calls r1 even when it accepts.

## 5. What this does not check

* **Identity compatibility** (`fuse_ver`/`engine_id`/`ucode_id` against the
  part's fuses) is `tools/nv/no-rm/hs_gate.py`'s job. Run it in addition; it is a
  candidate filter, not a launch gate.
* **The signature class.** No static gate can see which fused key a container
  was signed with: a GA102-signed booter is measured to fail at the signature
  (`0x250009`) on this AD102 while an AD102-signed one gets past it and fails
  after (`0x002B0009`). Identity compatibility is necessary, never sufficient.

## 6. Reproducing the acceptance without a GPU

```
python3 tools/nv/booter_preflight.py --selftest      # 12/12 fixtures, no hardware
tools/nv/sec2-boot-lane.sh 0 <tag>                   # the dry run above
```

The selftest builds its own containers in a temp directory (booter-class,
`0x100`-padded unload, truncated, non-zero padding, no load header, bootloader-
only, and a wrong pin) so the gate is checkable on a tree with no firmware
staged. The lane's `count=0` path needs a `disk.img` to prove the staging step;
without one it prints `booter stage: skipped (no disk.img; preflight only)` and
still asserts the file.

## 7. Where the rig lives

`tools/nv/sec2-boot-lane.sh` and `tools/nv/booter_preflight.py` are tracked
files on `origin/nv/integ-swarm`; `git log --oneline -1 -- tools/nv/sec2-boot-lane.sh`
names the commit that carries them. `tools/nv/deploy.sh` also stages a
`BOOTER.BIN`, from the same first-glob default and with no hash row — it is the
general VM/boot harness, not a booter test, and it cannot be pinned.
