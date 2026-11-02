/* tools/elf-dyn/src/dep.c — bottom of the fixture chain (LIBDEP.SO).
 *
 * Defines one data object and one function, both default-visibility, so the
 * module above it (LIBUSE.SO) must resolve an imported *object* (GLOB_DAT)
 * and an imported *function* (JUMP_SLOT) out of this DSO.
 *
 * dep_localp is a module-local pointer to a module-local object: it can only
 * be filled in once the module has a load address, so it is the fixture's
 * guaranteed R_*_RELATIVE.
 *
 * No libc, no TLS, no dependencies: this module's DT_NEEDED list is empty.
 */

int dep_counter = 7; /* imported as GLOB_DAT by LIBUSE.SO */

static int dep_local = 3;          /* static: non-preemptible, so its
                                    * address is a link-time constant +
                                    * the load base → R_*_RELATIVE */
int *dep_localp = &dep_local; /* R_*_RELATIVE: local pointer, needs the base */

const char dep_msg[] = "dep"; /* .rodata survives into the mapped image */

int dep_bump(int x) { /* imported as JUMP_SLOT by LIBUSE.SO */
  dep_counter += x;
  return dep_counter;
}
