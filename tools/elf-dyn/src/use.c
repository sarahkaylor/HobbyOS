/* tools/elf-dyn/src/use.c — middle of the fixture chain (LIBUSE.SO).
 *
 * Imports LIBDEP.SO's data and function, so it carries
 *   DT_NEEDED  LIBDEP.SO        (a module with a dependency)
 *   GLOB_DAT   dep_counter      (data import)
 *   JUMP_SLOT  dep_bump         (function import)
 * and defines its own TLS, which is the module the interpreter's initial-exec
 * path has to account for:
 *   __thread int use_tls;       (R_AARCH64_TLS_TPREL64 / R_X86_64_TPOFF64
 *                                into .got, plus DT_FLAGS STATIC_TLS)
 *
 * use_localp is the local pointer that must become R_*_RELATIVE.
 */

extern int dep_counter;      /* GLOB_DAT */
extern int dep_bump(int x);  /* JUMP_SLOT */

static int use_local = 5;
int *use_localp = &use_local; /* R_*_RELATIVE */

__thread int use_tls;              /* .tbss: initial-exec TLS */
__thread int use_tls_init = 0x1234; /* .tdata: copied by the loader */

int use_call(int x) {
  use_tls += x;
  return dep_bump(x) + dep_counter + use_tls_init;
}

int use_get_tls(void) { return use_tls; }
int *use_get_localp(void) { return use_localp; }
