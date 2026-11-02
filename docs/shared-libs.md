# Shared libraries on HobbyOS — path B (ELF-native dynamic linking)

Status: **M2 — the pilot runs on both arches at the default 8 CPUs.** All four lanes
merged; `HDYN.BIN` (ET_DYN + `PT_INTERP /LDHOBBY.SO` → `LIBC.SO`) prints its markers and
exits 0 on x86_64 and aarch64. §7 records the six integration seams and the `cpu_local`
stride bug (which used to gate the pilot at `-smp 1`, now fixed).
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

   **Done — reproduce with:**

   ```
   tools/build_libs.sh intel && tools/build_libs.sh build_interp intel
   make ARCH=intel MODE=so_test -j32 hobbyos.elf disk.img
   timeout 45 make ARCH=intel MODE=so_test run QEMU_ARGS="-display none"
   # expect on the serial log:
   #   HDYN: entered (dynamic)
   #   HDYN: libc.so symbols resolved
   #   HDYN: done
   #   [EXITPC] pid=1 ... code=0x0
   ```

   (aarch64: same with `arm`. The default 8-CPU boot is expected: the
   `cpu_local` stride bug that used to require `-smp 1` is fixed — §7.)
6. Only then convert real programs (`ls`, `nano`, `desktop`).

## 6. Deliberate non-goals (v1)

`dlopen`/`dlsym` (needs DTV + dlclose), symbol versioning, lazy binding,
prelinking, `LD_LIBRARY_PATH` (flat root only), NFS-hosted DSOs, unloading.

## 7. What the first integration measured (the seams)

Every lane was green on its own branch; these six only appear when the pieces
meet. Each is fixed in the merged tree, and each has a serial-log signature.

1. **The kernel must not relocate an interpreted executable.** `el_stage_build`
   applied the whole `DT_RELA` table, which holds symbol-bearing
   `GLOB_DAT`/`JUMP_SLOT` entries only the interpreter can resolve →
   `elf loader: relocations: unsupported relocation type`. Fix: `apply_reloc=0`
   for the exe when `PT_INTERP` is present, and leave `DT_RELASZ` intact
   (zeroing it is a static-PIE-only trick). The interpreter itself is still
   relocated by the kernel.
2. **`PT_LOAD` segments share pages unless the link says otherwise.** lld packs
   R / R+X / RW contiguously, so the interpreter's per-segment mapping is
   unsound → `LDHOBBY: LIBC.SO: PT_LOAD segments share a page`. Fix:
   `-z separate-loadable-segments` plus page-aligned `.text`/`.data.rel.ro` in
   `src/libc/linker_so.ld`; the loader now accepts a shared page only when the
   two segments agree on protection (lld's two adjacent RW segments do).
3. **The interpreter's own slot is not free.** The kernel places `LDHOBBY.SO` in
   `USER_LIB` slot 0 and hands `interp_base` over; ld.so's first-fit then
   returned slot 0 and mapped `LIBC.SO` on top of itself → the ELF header read
   back at `base` had the interpreter's 8 program headers instead of LIBC.SO's
   9. Fix: mark the slot containing `interp_base` used before the first
   `slot_take()`.
4. **A COW write is a *present-page* fault, and the trap dispatch withheld
   those.** `vm_handle_fault()` is the only thing that can break a copy, but
   x64 called it only for `!pf_present`, and ARM only for translation faults;
   a write to a read-only object frame was therefore killed as `in=PROT`
   (measured at the first relocation write into LIBC.SO). Fix: dispatch both
   classes on both arches — `vm_handle_fault` still answers `PROT` for a real
   violation.
5. **Local TLS is symbol-less.** lld emits `R_X86_64_TPOFF64` with **symbol
   index 0** and the offset in the addend for `static __thread` objects, so the
   scope lookup has nothing to resolve → `TLS relocation against an undefined
   symbol`. Fix: `si == 0` means `S = 0`, take the offset from the addend
   (a `STB_LOCAL` defined symbol is the other legal shape).
6. **The TLS "installed" flag belongs to libc and the loader must set it.**
   `__errno_location()` re-installs a single-module block when
   `ho_tls_ready_flag` is clear, clobbering the interpreter's multi-module
   layout. ld.so now writes that exported flag after `SYS_SET_TLS` and fails
   loudly if libc does not export it.

### Blocker found and fixed during M2: `cpu_local` had two sizes

With QEMU's default 8 CPUs the first user process was unreliable: it landed
on an AP where `current_process()` read NULL (`%gs:40`), so `open` →
"cannot open DT_NEEDED", console writes → `-EFAULT`, and `exit` never
terminated (the process spun in the compiler's `noreturn` trap).

Root cause: `struct cpu_local` was defined **twice** — the real block in
`trap.c` (104 bytes, with the GB-147 frame-snapshot fields) and a six-field
mirror in `process.c` (48 bytes) for its `extern cpu_locals[]`. `cpu_locals`
is defined in `trap.c`, so `process.c` indexed it with the wrong stride:
`set_current_process_pid(cpu, pid)` wrote `base + cpu*48 + 40` while that
CPU's GS base pointed at `base + cpu*104`. CPU 0 coincides, which is why
anything on the boot core worked and every AP saw NULL.

It was **not** specific to ELF: a plain flat `HELLO.BIN` failed 2 runs in 3
with `[DEADCLAIM] ... the resume path halted that CPU`. The struct now lives
in `src/include/arch/cpu_local.h` with its offsets pinned. After the fix:
3/3 dynamic-pilot runs and 4/4 flat runs on 8 CPUs, and the pilot no longer
needs `-smp 1`.

## 8. Converting a program (validated on `ls`)

A program becomes dynamically linked by changing **only its link flavour** —
no source edit is required if it already builds:

```
# 1. same TU, position independent (the static .o is absolute and lld will
#    reject it with "relocation R_X86_64_64 cannot be used against symbol")
clang <USER_CFLAGS> -fPIC -c src/user/ls.c -o obj/<arch>/dyn/ls.o

# 2. ET_DYN + PT_INTERP, linked against the shared libc
ld.lld -pie --dynamic-linker /LDHOBBY.SO -e _start -z noexecstack -z now \
       -o obj/<arch>/LSDYN.BIN obj/<arch>/dyn/ls.o obj/<arch>/libc.so
```

Two entry-point shapes, and the difference matters:

* **legacy `_start` programs** (most ports: `ls`, `cat`, `grep`) link with no
  crt0 at all — their own `_start` is the entry, and they read argv through
  `SYS_GETARGS`. TLS stays correct because the interpreter installed the block
  and set `ho_tls_ready_flag`, so libc's lazy `__errno_location()` install
  stands down.
* **`main()`-style programs** need `crt0_dyn.o` (never `crt0.o`: it would
  recompute a single-module TLS block from the executable's own `__tls_*`).

Measured: `ls` drops from 46 KB static to **7 KB** dynamic. A program using a
symbol that `libc.map` does not export fails at *link* time with the symbol
named — that is the export set doing its job, not a loader bug.

## 9. Sharing evidence, and the cache-init race

`MODE=so_test` boots three dynamically linked programs (`HDYN.BIN`, `LSDYN.BIN`
= `ls`, `CATDYN.BIN` = `cat E2E.TXT`) at the default 8 CPUs. Each boot prints
exactly one sharing line, which is the proof that the library text exists once
in RAM rather than per process (cluster/size are the file's FAT16 identity, so
they differ per arch and per image build):

```
so cache: file (cluster=23366 size=350848) now shared by refs=6   # x86_64
```

`refs` counts the cache's own reference plus one per mapping; the same line
names `LIBC.SO` by its on-disk identity, so a reader can tie it to the file.

**The bug behind it.** With three mappers this boot crashed about one time in
three: a process died at `VA=LIBC.SO+0x40`, `rw=r`, `in=PROT`, `kind=7`, with
the last PC inside the interpreter. The cause was not the mapping but the
cache's lazy lock initialiser:

```c
if (!file_obj_ready) { spinlock_init(&file_obj_lock); file_obj_ready = 1; }
```

Three CPUs reaching the first DSO mmap together can both run that body, and
the second `spinlock_init()` **resets a lock the first CPU already holds**.
Both then enter the critical section, both see an empty table, both take the
same free slot, and the second insert overwrites the first. The orphaned
object still serves the live mapping (`refs=3`, `used=1`) but is no longer
findable by `(cluster, size)`, so its unfaulted pages can never be filled —
the fill path returns `-EIO` and the fault is reported as `PROT`.

The initialiser is now a CAS: one winner initialises, losers spin on a release
store. `vm_lock_init_once()` had the identical shape and was fixed with it
(unreachable today only because the boot core happens to win that race first).
Measured: 3 crashes in 8 boots before, 0 in 15 after.

## 10. Userland migration

The loader dispatch is per file, so migration is opt-in and reversible: a
program moves to `ET_DYN` when `tools/dyn-migrate.sh` writes `DYN/<base>.BIN`
and the disk rule copies it over the program's static 8.3 name. Nothing else
in the tree changes — the shell, the desktop and the test wave launch the
dynamic build by the same name.

```
tools/dyn-migrate.sh intel cat cp ls wc head tail sort uniq free ps uptime \
    kill mkdir mv rm touch cut_gnu head_gnu tail_gnu monitor
make ARCH=intel MODE=dyn_test -j32 fresh_disk
make ARCH=intel MODE=dyn_test run QEMU_ARGS="-display none"
```

`dyn-migrate.sh` compiles the program's TU with `-fPIC`, links it `-pie` with
`--dynamic-linker /LDHOBBY.SO` against `libc.so`, and appends each success to
`obj/<arch>/dyn_list.h`. `MODE=dyn_test` boots one process per entry, so the
converted set is exercised on every build rather than one program at a time.
A no-argument invocation is the smoke: a tool either does its work, prints
usage, or reads a closed stdin — all three paths run, and a memory fault is
the only failure mode we care about.

### 10.1 Two ways a program silently fails to link

**`main()`-style programs need `crt0_dyn.o`.** Linking a TU that has no
`_start` with `-e _start` makes `lld` print a warning and leave `e_entry` at
0. The kernel then rejects the image with `e_entry outside every executable
segment`, which reads like a segment bug and is not one. `dyn-migrate.sh`
tests the object with `nm` for `T _start` and adds `crt0_dyn.o` when it is
absent; it also refuses any link whose `readelf -h` entry is `0x0`.

**A const table of pointers needs `.data.rel.ro`.** `dyn_list.h` was first
emitted as `static const char *const names[]`, which puts a relocation in
`.data.rel.ro`. Neither kernel link script had that output section, so on the
PIE-linked aarch64 kernel the relocations were dropped and every entry read
as `NULL` — the boot printed `dyn_test: ` twenty times and loaded nothing,
while x86_64 (linked absolute) was fine. Both scripts now place
`.data.rel.ro`, and the generated list is a 2-D `char[][16]` array that needs
no relocation at all.

### 10.2 What is convertible today

The list of converted programs lives in `tools/dyn-programs.txt`; 43 of them
are green on both arches. An entry is a bare object base (`cat`) or
`name=base1,base2` when the program has more than one TU (`sh=sh,sh_aux`).

Survey the real link line, not the rule text:

```
make -n ARCH=intel -s obj/intel/<bin> | grep 'ld.lld.*-o obj/intel/<bin>.elf'
```

Reading the Makefile's `$(X_BIN):` rules undercounts badly, because most
utilities are built through variable lists (`$(TR_OBJS)`) or a shared rule —
`tr` is a single TU called `tr_gnu`, and looks multi-TU from the rule text.
By real link line, 61 programs are one own object; 43 of those are converted.

Still blocked, and why:

- **`selfhost`** converts and links but faults immediately with `PC=0` (a
  call through a NULL symbol) and takes `/TOOLS/SH.BIN` down with it. It stays
  static until that is understood; the smoke is what caught it.
- **Multi-TU programs** (`grep`, `sed`, `make`, most of the 4–47-object
  group) link many objects with per-program include trees; the PIC compile
  must reproduce each object's own flags, which `dyn-migrate.sh` does not yet
  replay.
- **C++ programs** (`cxx_smoke`, `cxx_t`, `rtti_t`) link `libcxx.a`, which is
  not built `-fPIC`; the archive needs a PIC flavour first.
- **X11 and full-screen programs** (`console`, `xsession`, `nano`, `pong`,
  `doom`) are worth converting last: hardest to smoke-test headlessly.

### 10.3 Numbers (intel)

`ls` 7056 vs 46080 static, `cat` 7536 vs 56744, `sh` 219768 vs ~1 MB. The
saving is what you would expect for small utilities; the real prize stays the
large role blobs and future `dlopen`.

With 43 dynamic processes created in one boot on a 1.25 GiB process pool
(40 blocks of 32 MiB), every one still loaded: they exit fast enough that the
loader's placement loop never runs dry. That is a property of this smoke, not
a guarantee for 43 long-lived processes.
