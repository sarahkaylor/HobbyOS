# Lane W3-Tools — /TOOLS staging + F8 curation decision (evidence, 2026-10-14)

Wave-3 tools lane. Gates: `make disk.img` green; mtools `mdir` proof of the
staged F1 `/TOOLS` layout; `docs/selfhost/lanes/w3-tools-report.json`.

## 1. F8 decision numbers (Task A)

Measured on `obj/selfhost/dist-staging/usr/local/lib/clang/21/include` (the
wave-2 `install-distribution` RESDIR):

```
272 files, 14,727,254 bytes, 6 wrapper subdirs
  (cuda_wrappers llvm_libc_wrappers llvm_offload_wrappers openmp_wrappers
   ppc_wrappers zos_wrappers)
221 basenames violate 8.3 (205 of 239 top-level files)
  5 basenames exceed 31 chars (the read_dir DISPLAY cap — cosmetic only):
    __clang_cuda_libdevice_declares.h          (33)
    __clang_hip_libdevice_declares.h           (32)
    __clang_cuda_texture_intrinsics.h          (33)
    __clang_cuda_math_forward_declares.h       (36)
    openmp_wrappers/__clang_openmp_device_functions.h (36)
19 name families collide under 8.3 truncation (e.g. __CLANG_.H, __STDDEF.H)
max path depth 3, no symlinks, all-ASCII
```

Decision: **LFN suffices → stage faithfully, no curated tree** (see
`docs/selfhost/llvm-bootstrap.md` §6.2).  Key in-tree proofs:

- `src/kernel/fat16.c` — LFN match on open spans the full 255-char name
  space (`lfn_add_chunk`/`lfn_matches`; `st->name` unbounded past 31); the
  31-char cap is confined to `lfn_entry_display` (read_dir listing).
- `src/user/lfn_test.c:100` — merged wave-1 in-OS test opens the 36-char
  `llvm-project-21.1.8.src-EXAMPLE.c` by long name and reads content
  (`read_expect` PASS). That is the same length class as the longest RESDIR
  basenames.

## 2. Scratch-image staging proof (Task B, host side)

```
$ truncate -s 700M /tmp/w3-tools-scratch.img && mformat -i /tmp/w3-tools-scratch.img ::
$ tools/selfhost-stage.sh --tools /tmp/w3-tools-scratch.img
selfhost-stage: staging in-OS toolchain to ::/TOOLS (F1 layout)
... (4 ported tools WARN-skipped: obj/arm/*.bin not built yet at first run)
selfhost-stage: staged 276 files, 237116238 bytes into ::/TOOLS
  --- ::/TOOLS (mdir) ---
CLANG    BIN  126060320 2026-10-14   2:21
LLD      BIN  72651120  2026-10-14   2:21
LLVMAR   BIN  13614528  2026-10-14   2:21
OBJCOPY  BIN  10063016  2026-10-14   2:21
RESDIR       <DIR>      2026-10-14   2:21
  --- ::/TOOLS/RESDIR/include count ---
      295 files          14 727 254 bytes
```

Byte-fidelity spot checks (mtype pipe → sha256, compared to staging source):

```
OK  __clang_cuda_math_forward_declares.h     (36-char LFN, include root)
OK  __clang_hip_libdevice_declares.h         (32-char LFN)
OK  openmp_wrappers/__clang_openmp_device_functions.h   (subdir LFN)
OK  module.modulemap
CLANG.BIN sha256 == obj/selfhost/dist-staging/usr/local/bin/clang-21  (match)
```

Idempotency: re-run `--tools` rc=0; `--tools -f` deletes and re-stages
`::/TOOLS` cleanly (mdir + sha256 re-verified after force).

## 3. Startup-image disk.img (Task B, gate)

- `make disk.img` green on branch `selfhost/w3-tools` (rc=0; full recipe
  incl. the new `tools/selfhost-stage.sh --tools disk.img` step; 281 files /
  238,128,534 bytes staged into ::/TOOLS).
- Final `mdir` of `::/TOOLS` (12 top-level entries = 9 tools + RESDIR dir):

```
MCOPY    BIN    184664   (preserved w2-mtools line)
MDEL     BIN    184664   (preserved w2-mtools line)
MMD      BIN    184664   (preserved w2-mtools line)
CLANG    BIN  126060320  == clang-21 ELF, sha256 identical to staging
LLD      BIN   72651120
LLVMAR   BIN   13614528
OBJCOPY  BIN   10063016
MAKE     BIN     307720
MKFSFAT  BIN     150584
RESDIR       <DIR>
        12 files         223 401 280 bytes
                        696 090 624 bytes free   (1 GiB image, fits)
```

- RESDIR long names present on the real image (mdir shows the 8.3 short
  alias + the VFAT long name side by side, e.g. `__CLAN~1 H  __clang_cuda_math.h`);
  byte-fidelity confirmed on the real disk.img by mtype+sha256 for the
  36-char names, `openmp_wrappers/__clang_openmp_device_functions.h`,
  CLANG.BIN and MAKE.BIN — all identical to their staging sources.
- Layout staged by the recipe:
  `tools/selfhost-stage.sh --tools disk.img` (soft dependency: warns +
  skips when `obj/selfhost/dist-staging` is absent, so other trees stay
  green).  STAGE_TOOLS=0 disables the step entirely.

## 4. In-OS visibility run (SELFHOST.BIN harness boot)

Ran `tools/run_selfhost_build_test.py --stage --image disk.img` — booted the
staged startup image once, launched SELFHOST.BIN from the Apps menu via QMP
keystrokes.  Serial proof that `/TOOLS` is visible and resolvable in-OS
(committed excerpt: `w3-tools-harness-serial-excerpt.log`):

```
Booting AArch64 OS...
[LAUNCH] SELFHOST.BIN
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=s0-check-start
Loading program for scheduler (v2 AS): /TOOLS/MAKE.BIN
MAKE.BIN: Makefile.hobbyos: No such file or directory
MAKE.BIN: *** No rule to make target 'Makefile.hobbyos'.  Stop.
[SELFHOST] phase=s0-check-fail reason=tool-rc rc=512
```

The driver's /TOOLS-first tool resolution found `/TOOLS/MAKE.BIN` and the
kernel loaded + executed it in-OS (307,720 B); make ran and failed cleanly
only on the absent `Makefile.hobbyos` (a later wave), never on tool-missing
(`abort` marker count = 0).  Harness verdict `stage_failure` is the
acceptance's expected clean-staged-failure shape for this wave.

## 5. Files touched

- `Makefile` — disk.img recipe: F1 /TOOLS staging invocation (sole Makefile
  writer this wave).
- `tools/selfhost-stage.sh` — new `--tools` mode + `stage_tools()` (F1 /TOOLS
  layout, idempotent, soft dependency on the LLVM distribution staging).
- `docs/selfhost/llvm-bootstrap.md` — §6.2 F8 decision & evidence (this lane).
- `docs/selfhost/lanes/w3-tools-*` — this evidence + report JSON.
