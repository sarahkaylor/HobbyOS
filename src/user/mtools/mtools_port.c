/* mtools_port.c — HobbyOS port glue for GNU mtools 4.0.49.
 *
 * Upstream mtools builds ONE multicall binary ("mtools") and symlinks every
 * command (mcopy, mdel, mmd, ...) to it; mtools.c dispatches on argv[0] and
 * references ALL commands, so transcribing it would drag in the whole tree.
 * HobbyOS loads one flat .BIN per program, so this first-party glue file
 * (same role as nano's hb_curses.c) supplies:
 *
 *   1. one tiny main() per tool — compiled with -DMTOOLS_TOOL=<name>, it
 *      calls the upstream command entry directly (mcopy/mdel/mmd), after
 *      the same startup mtools.c performs: setlocale, init_privs,
 *      read_config(), setup_signal(), progname, --version printing.
 *   2. the OS-surface fallbacks the transcribed subset references but the
 *      HobbyOS sysroot does not provide yet (config.h declares them):
 *      getlogin(), alarm(), setuid(), setgid(), helpFlag().  All are
 *      honest single-user/no-timer minima for THIS port; upstream mtools
 *      sources stay byte-identical.
 *
 * The ported subset: mcopy/mdel/mmd + the shared FAT/image engine
 * (doscp/stream/plain_io/FAT/VFAT/dirCache/mk_direntry/charset/config).
 * Not ported: floppyd/xdf/scsi, and every other tool main (mformat lands
 * with the mkfs.fat port), exactly as L4's buildtools-audit §4 scoped it.
 */

#include "sysincludes.h"
#include "mtools.h"

const char *progname;

/* -- per-tool main ---------------------------------------------------- */

#if defined(MTOOLS_TOOL_mcopy)
#define MTOOLS_ENTRY mcopy
#define MTOOLS_ENTRY_TYPE 0
#elif defined(MTOOLS_TOOL_mdel)
#define MTOOLS_ENTRY mdel
#define MTOOLS_ENTRY_TYPE 0
#elif defined(MTOOLS_TOOL_mmd)
#define MTOOLS_ENTRY mmd
#define MTOOLS_ENTRY_TYPE 0
#else
#error "mtools_port.c: define MTOOLS_TOOL_mcopy/mdel/mmd"
#endif

int main(int argc, char **argv) {
  const char *name;

#ifdef HAVE_SETLOCALE
  setlocale(LC_ALL, "");
#endif
  init_privs();
  name = mt_basename(argv[0]);
  progname = argv[0];

  if (argc >= 2 &&
      (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0)) {
    printf("%s (GNU mtools) %s\n", name, mversion);
    printf("configured with the following options: ");
#ifdef USE_XDF
    printf("enable-xdf ");
#else
    printf("disable-xdf ");
#endif
#ifdef USE_RAWTERM
    printf("enable-raw-term ");
#else
    printf("disable-raw-term ");
#endif
    printf("\n");
    return 0;
  }

  read_config();
  setup_signal();
  MTOOLS_ENTRY(argc, argv, MTOOLS_ENTRY_TYPE);
  /* the tool entry points are NORETURN; reaching here is a bug */
  return 1;
}

/* -- OS-surface fallbacks (HobbyOS target only) ------------------------ */

/* helpFlag normally lives in mtools.c; the transcribed tool mains call it
 * for their --help handling.  (Needed on host parity builds too: upstream
 * mtools.c is not transcribed, so the port owns it everywhere.) */
int helpFlag(int argc, char **argv) {
  return (argc > 1 && !strcmp(argv[1], "--help"));
}

#ifdef HOBBYOS_TARGET

/* getlogin(): the sysroot is single-user with a synthetic passwd entry;
 * return the LOGNAME env var or the same "user" name getpwuid reports.
 * (Upstream only uses this to find a home directory when $HOME and
 * $LOGNAME are both absent.) */
char *getlogin(void) {
  char *n = getenv("LOGNAME");
  return (n && *n) ? n : (char *) "user";
}

/* alarm(): no POSIX timer in the sysroot; a zero-return stub keeps
 * lockdev.c's timeout machinery inert (it only ever cancels + re-arms
 * here — for image files no device lock is ever taken). */
unsigned int alarm(unsigned int seconds UNUSEDP) {
  return 0;
}

/* setuid/setgid(): single-user OS, no privilege change exists; accept and
 * succeed so privileges.c's drop/reclaim arms compile and no-op. */
int setuid(unsigned int uid UNUSEDP) {
  return 0;
}

int setgid(unsigned int gid UNUSEDP) {
  return 0;
}

/* expand(): device-name expansion (upstream expand.c).  Upstream runs
 * `$(...)` through /bin/sh when the name contains shell metacharacters;
 * the HobbyOS sysroot has no popen and the acceptance recipes use plain
 * `-i IMG` paths only, so this is the faithful no-metachar branch (the
 * implementation upstream also uses for every real device name) and the
 * shell-expansion branch is deliberately not ported.  Anything with
 * metacharacters is passed through verbatim (and will fail to open,
 * exactly as an unknown device name should). */
const char *expand(const char *input, char *ans) {
  if (input == NULL)
    return NULL;
  strncpy(ans, input, EXPAND_BUF - 1);
  ans[EXPAND_BUF - 1] = '\0';
  return ans;
}

/* precmd()/postcmd() (upstream precmd.c): run a shell command before
 * opening / after closing a device.  No HobbyOS config sets these for the
 * ported image-file subset, so the calls are honest no-ops. */
void precmd(struct device *dev) {
  (void) dev;
}

void postcmd(const char *cmd) {
  (void) cmd;
}

#endif /* HOBBYOS_TARGET */
