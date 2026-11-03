/* tools/elf-dyn/src/app.c — top of the fixture chain (APP.BIN).
 *
 * An ET_DYN executable (linked -pie --no-dynamic-linker -e _start): exactly
 * the shape a HobbyOS native program has after docs/shared-libs.md §1, minus
 * PT_INTERP (the fixture is never entered by the kernel; it exists so the
 * interpreter's loader path can be exercised under a host shim).
 *
 * It carries DT_NEEDED LIBUSE.SO, a JUMP_SLOT for use_call (function import),
 * a GLOB_DAT for use_localp (data import) and a RELATIVE for app_table
 * (module-local pointer).
 */

extern int use_call(int x);
extern int *use_localp;

static int app_local = 9;
int *app_table[] = {&app_local, &app_local}; /* R_*_RELATIVE */

int app_main(void) { return use_call(1) + *use_localp + *app_table[0]; }

/* The entry point.  A real program reads argv via SYS_GETARGS; the fixture
 * only has to exist so e_entry is non-zero and the ELF is a legal ET_DYN
 * executable. */
void _start(void) {
  (void)app_main();
  for (;;) {
  }
}
