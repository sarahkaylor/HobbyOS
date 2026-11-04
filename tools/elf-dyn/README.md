# tools/elf-dyn — dynamic-linking fixtures for lane D

Three modules, built for both HobbyOS targets (`aarch64-none-elf`,
`x86_64-none-elf`), that pin the relocation shapes an interpreter has to
implement.  They are the contract evidence for `docs/shared-libs.md` §4 lane D:
a later regression shows up as a diff of the golden dumps, not as a guess.

| module | shape | proves |
|---|---|---|
| `LIBDEP.SO` | `ET_DYN` DSO, no `DT_NEEDED` | `R_*_RELATIVE` for a module-local pointer (`dep_localp`), `GLOB_DAT` for an exported data object |
| `LIBUSE.SO` | `ET_DYN` DSO, `DT_NEEDED LIBDEP.SO` | `GLOB_DAT` for a data import (`dep_counter`), `JUMP_SLOT` for a function import (`dep_bump`), `__thread` TLS (`R_AARCH64_TLS_TPREL64` / `R_X86_64_TPOFF64` + `DF_STATIC_TLS`) |
| `APP.BIN` | `ET_DYN` **executable** (`-pie --no-dynamic-linker -e _start`, no `PT_INTERP`) | `DT_NEEDED LIBUSE.SO`, `JUMP_SLOT` (`use_call`), `GLOB_DAT` (`use_localp`), `RELATIVE` (`app_table[]`) |

Names are FAT16 8.3 on purpose (`LIBDEP.SO`, `LIBUSE.SO`, `APP.BIN`), and the
`DT_SONAME`/`DT_NEEDED` strings match the on-disk names, so lane D's flat-root
`open()` path is exercised with real data instead of invented names.

## Usage

```sh
tools/elf-dyn/gen_so_fixtures.sh            # rebuild + refresh golden/
tools/elf-dyn/gen_so_fixtures.sh --check    # rebuild + diff against golden/
tools/elf-dyn/gen_so_fixtures.sh --arch arm # one target only
```

Built modules: `obj/fixtures/<arch>/{LIBDEP.SO,LIBUSE.SO,APP.BIN}` (gitignored).
Golden dumps: `tools/elf-dyn/golden/<arch>/<module>.dyn.txt` (committed;
`readelf -h/-l/-d/-r/--dyn-syms`).

`gen_so_fixtures.sh` asserts the relocation matrix above per target and fails
by name if a shape goes missing — the dumps are only frozen once the assertions
pass.

## Notes for lane D

* Fixtures are compiled with plain `-fPIC -ftls-model=initial-exec`, **not**
  the Makefile's `-mcmodel=large`.  `LIBC.SO` is built with the large model
  because user images live at 64 GiB; the fixtures only need recognisable
  relocation shapes, and the small PIC model emits the classic PLT
  (`JUMP_SLOT`) that `-mcmodel=large` folds into GOT loads (`GLOB_DAT`).
  Both shapes occur in real builds — `obj/intel/libc.so.evidence.txt` shows
  140 `GLOB_DAT` / 1 `JUMP_SLOT`, the fixtures show the PLT form.
* `LIBUSE.SO` carries `.tdata` **and** `.tbss` (one initialised `__thread`
  variable each), so a TLS block builder is exercised for both the copy and
  the zero-fill path.
* `APP.BIN` has a non-zero `e_entry` and no `PT_INTERP`: the kernel never
  enters it, lane D does (or the host shim in `tools/ldso_host_test.sh`).
