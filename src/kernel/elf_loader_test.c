/* src/kernel/elf_loader_test.c — ELF mapper / PT_INTERP suite (lane B).
 *
 * M0 freeze: the suite is registered so lane B owns ONLY this file.  It is
 * empty until B lands the aarch64 mapper + interpreter path; an empty suite
 * is green by construction so the tree stays buildable while B works.
 */
#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"

void elf_loader_test_suite(void) {
  /* Lane B fills this. */
}

#endif /* KERNEL_MODE_UNIT_TEST */
