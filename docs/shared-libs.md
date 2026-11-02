# Shared libraries on HobbyOS — path B (ELF-native dynamic linking)

Status: **M0 frozen (2026-11-01).** Executing as four parallel lanes; this file
is the contract every lane reads before its first commit.
Scope: native HobbyOS programs (not the Linux-compat track, which reaches
`ld.so` by a different route — `docs/linux-abi.md` §W5).

---

## 1. The decision

Native programs get **real dynamic linking**, the ordinary Unix shape:

* An executable is an **ELF64 `ET_DYN`** image with a **`PT_INTERP`** naming
  `LDHOBBY.SO` at the disk root (8.3: `LDHOBBY.SO` is 7+2, legal FAT16).
* `LDHOBBY.SO` is itself an `ET_DYN` static-PIE (no `DT_NEEDED`). The kernel
  maps it and jumps to it; it relocates itself with the existing
  `R_*_RELATIVE` pass.
* The interpreter maps each `DT_NEEDED` DSO with `mmap(fd, MAP_PRIVATE)`,
  resolves symbols, applies relocations, runs `DT_INIT_ARRAY`, calls
  `mprotect` for W^X/RELRO, and transfers to the executable's entry.
* **The kernel does not parse symbol tables.** It maps the executable, maps the
  interpreter, writes the startup block, and enters the interpreter. Everything
  else is userspace, exactly as on Linux — so `dlopen` later is an interpreter
  feature, not a kernel one.

Consequences that the lanes must respect:

1. **The flat `.bin` format cannot carry a dependency.** `objcopy -O binary`
   drops headers and `PT_DYNAMIC`. A dynamically linked native program is
   therefore an ELF file on disk (`NAME.BIN` holding an ELF image is allowed
   and expected — the loader already dispatches on `\x7fELF`). The flat
   path stays for every program not yet converted; nothing is deleted.
2. **Sharing comes from the page cache, not from a kernel library registry.**
   `mmap` of a regular FAT16 file creates/reuses one `VM_OBJ_FILE` object per
   `(start_cluster, file_size)`; all processes mapping it share the frames
   until a `MAP_PRIVATE` write copies. Text stays read-only shared.
3. **TLS v1 is `initial-exec` only.** Every DSO is mapped at process start
   (no lazy loading of TLS-using modules). A DSO whose TLS cannot fit the
   per-thread static block makes the load fail loudly. `DTV`/`__tls_get_addr`
   and dynamic TLS are a later phase (they exist for `dlopen` only).
4. **No symbol versioning in v1.** `DT_SONAME` is the 8.3 name
   (`LIBC.SO`, `LIBCXX.SO`); `libc.map` freezes the exported set.

## 2. Frozen interfaces (single writer: coordinator)

| Interface | Where | Frozen shape |
|---|---|---|
| Library VA slot | `src/include/vm.h` | `USER_LIB_BASE..USER_LIB_LIMIT`, 16 MiB `USER_LIB_SLOT_SIZE`, 55 slots, first-fit |
| Startup block page | `src/include/vm.h` + `src/include/startup.h` | one page at `USER_START_BASE`, `struct hb_start`, `HB_START_ABI 1` |
| File-backed object | `src/include/vm.h` | `VM_OBJ_FILE 3`, region kind `VMK_LIB 7` |
| mmap contract | `SYS_MMAP` (62) | `fd >= 0` + a regular file + `MAP_PRIVATE`/`MAP_SHARED` maps the **file**; memfd behaviour unchanged |
| DSO naming | FAT16 root | `NAME.SO`, `PT_INTERP` = `/LDHOBBY.SO`, `DT_NEEDED` = `LIBC.SO` |
| Exported symbol set | `src/libc/libc.map` | version script pinning the current ~699 globals |

Nothing else is frozen. Lanes may add private headers under their own paths.

## 3. Ownership map (one writer per file)

| Lane | Owns | Must not touch |
|---|---|---|
| **A `so-vm`** | `src/kernel/vm.c`, `src/kernel/vm.h`→`src/include/vm.h` (additive only, constants above are frozen), `src/kernel/vm_test.c`, new `src/kernel/file_obj.c` + `src/include/file_obj.h` | `program_loader.c`, `elf_*.c`, `Makefile`, `src/libc/**` |
| **B `so-armelf`** | `src/kernel/elf_loader.c`, `src/kernel/elf_image.c`, `src/include/elf_image.h`, `src/kernel/program_loader.c`, new `src/kernel/elf_loader_test.c` | `vm.c`, `Makefile`, `src/user/**` |
| **C `so-toolchain`** | `tools/build_libs.sh`, `tools/elf-dyn/**`, `src/libc/libc.map`, new `src/libc/linker_so.ld` | `Makefile` (coordinator wires it), any `src/kernel/**` |
| **D `so-ld`** | new `src/user/ld/**`, `src/include/ldso.h`, `tools/ldso_host_test.sh` | `Makefile`, `src/kernel/**`, `src/libc/**` (except reporting needed changes) |
| **Coordinator** | `Makefile`, `src/kernel/main.c`, `src/kernel/unit_test.c`/`.h`, `src/include/vm.h` constants, `docs/shared-libs.md` | lane-owned files |

## 4. Lane briefs

### A — `so-vm`: file-backed shared mappings (keystone)

Deliver `SYS_MMAP` for regular files:

1. New `VM_OBJ_FILE` object: page array like `VM_OBJ_MEMFD`, filled **on first
   touch** by `fat16_read_direct_pos(start_cluster, size, frame, 4096, off)`.
   A short tail read zero-fills the rest of the page.
2. A per-object cache keyed by `(start_cluster, file_size)`, bounded
   (`VM_OBJ_MAX` slots exist; refuse politely beyond that). Two processes
   mapping the same file must get **the same frames** — that is the acceptance
   property, proven by a unit test that maps one file into two address spaces
   and compares frame physaddrs.
3. `MAP_PRIVATE` writes copy-on-write: on a write fault to a `VMK_LIB`/
   `VMK_SHARED`-file page, allocate a private frame, copy, remap, drop the
   object ref for that page. `MAP_SHARED` writes go straight to the object.
4. Invalidate the object on `fat16_write`/`unlink`/`rename` to the same file
   (no stale text after a rebuild on disk); if invalidation is not reachable
   from the object layer, document the residual hazard and add the test that
   shows the new file is picked up after a fresh boot only.
5. `mprotect` already exists — make sure it can drop `PROT_WRITE` on a DSO
   text region (W^X) and that a write to it then faults cleanly.

Gate: `run_unit_tests.sh` + `run_unit_tests_intel.sh` green with new
`vm_test.c` cases; no regression in the existing suites (frame leak counters
are exact — new tests must free what they map).

### B — `so-armelf`: aarch64 ELF mapper, `PT_INTERP`, startup block

1. Make `elf_loader.c` arch-neutral (drop the `__x86_64__` body guard) and
   implement the aarch64 map (same plan → `vm_map_page` with real `p_flags`).
   The `R_AARCH64_RELATIVE` type is already in `elf_apply_relocations`.
2. Accept `ET_DYN` **with** `PT_INTERP`: capture the interpreter path,
   map the interpreter into the first free `USER_LIB` slot, write
   `struct hb_start` at `USER_START_BASE`, and enter at `interp_base + e_entry`
   (not the executable's entry). Zero the start page for non-interpreted
   programs.
3. Keep `ET_EXEC`/`needs_low_window` refusals exactly as they are, and keep
   the flat `.bin` path byte-identical.
4. Prove it: kernel unit test for the plan/refusal matrix (in-memory fixture),
   plus a QEMU smoke on x64 with a hand-built `ET_DYN` + fake interpreter that
   prints one marker and exits. ARM smoke if the fixture is cheap to build.

Gate: both unit tiers green; the existing `elf_image_test_host` still passes
unchanged.

### C — `so-toolchain`: PIC libc, `libc.so`, soname + version script

1. `tools/build_libs.sh <arch>`: compile `src/libc/src/*.c` + `src/user/libc.c`
   + `src/user/malloc.c` with `-fPIC -ftls-model=initial-exec` into
   `obj/<arch>/pic/`, link `obj/<arch>/libc.so` with `src/libc/linker_so.ld`
   (`-shared -soname LIBC.SO --version-script src/libc/libc.map
   -z relro -z now`), and emit `obj/<arch>/libc.so.map` evidence
   (`readelf -d`, `--dyn-syms` count).
2. Keep `libc.a` building exactly as today (two flavours, one source tree).
   No source edits to libc except what PIC genuinely requires — report those.
3. `tools/elf-dyn/gen_so_fixtures.sh`: build a 3-module chain
   (`libdep.so` ← `libuse.so` ← `app`) for **both** targets with clang 21 +
   `ld.lld -shared`, and a golden `readelf` dump per module
   (`*.dyn.txt`) so a later regression is a diff, not a guess.
4. Deliver the exact command block that produces `LDHOBBY.SO` from lane D's
   sources once they exist (the coordinator wires it).

Gate: `tools/build_libs.sh arm` and `... intel` both produce a `libc.so`
whose `readelf -d` shows `SONAME LIBC.SO`, no `TEXTREL`, and a `.dynsym`
with ≥ 400 defined globals; `libc.a` unchanged in size ±1%.

### D — `so-ld`: the interpreter

New userspace program `src/user/ld/` (`ldso.c`, `elf_dyn.c/.h`, `mmap_io.c`),
built as `LDHOBBY.SO`. v1 responsibilities:

1. Read `struct hb_start` at `USER_START_BASE`; refuse on `magic`/`abi`
   mismatch with a serial message.
2. Read the executable's `PT_DYNAMIC` (already mapped by the kernel, read-only),
   walk `DT_NEEDED` in order, `open()` each `NAME.SO` at the disk root,
   `mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0)` into the `USER_LIB` slot
   (first-fit 16 MiB sub-slots), then `mprotect` each segment to its real
   `p_flags` after copying relocation results.
3. Build a global symbol scope: executable first, then DSOs in load order;
   lookup by `.gnu.hash` when present, `.hash` otherwise, linear as a last
   resort (fixtures cover all three).
4. Apply `R_*_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS64`, `COPY`. `BIND_NOW`
   only — no lazy PLT resolution in v1; a `JUMP_SLOT` is resolved eagerly.
5. Run each DSO's `DT_INIT_ARRAY`, then the executable's, then jump to the
   executable entry with no register contract (native `_start` reads argv via
   `SYS_GETARGS`, unchanged).
6. `TLS`: initial-exec only. Compute one static TLS block = sum of every
   module's `.tdata/.tbss` (max alignment), copy `.tdata` of each, hand the
   block to `SYS_SET_TLS`; pass each module's offset via `tp - block_base`.
   Refuse a DSO with `TLS` beyond the block with a clear message.

Host-testable: everything except the actual `syscall` must run under a host
shim (`tools/ldso_host_test.sh` compiles ldso.c against a POSIX mmap/open
shim and exercises the fixtures from lane C).

Gate: host test green — resolution order, each relocation type, a missing
`DT_NEEDED` (clean error), a symbol defined in two modules (first in scope
wins), and a TLS-using DSO.

## 5. Integration order (coordinator)

1. Merge A (kernel page cache) → gate both unit tiers.
2. Merge B (aarch64 ELF + `PT_INTERP`) → gate both tiers + `make test` ARM.
3. Merge C (libc.so) → build evidence only; nothing links against it yet.
4. Merge D (interpreter) → host gate.
5. **Pilot**: convert `hello` (38 KB, no deps beyond libc) to `ET_DYN` +
   `LIBC.SO`, put `LDHOBBY.SO` + `LIBC.SO` on the disk, boot x64 and ARM,
   assert the marker on the serial console and a clean exit.
6. Only then convert real programs (`ls`, `nano`, `desktop`).

## 6. Deliberate non-goals (v1)

`dlopen`/`dlsym` (needs DTV + dlclose), symbol versioning, lazy binding,
prelinking, `LD_LIBRARY_PATH` (flat root only), NFS-hosted DSOs, unloading.
