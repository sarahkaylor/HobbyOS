/*
 * HobbyOS host test aid: glibc-side regex ABI facts.
 *
 * This file is compiled WITHOUT the sysroot include paths, so it sees the
 * system <regex.h>. src/host/libc_regex_test.c computes the same numbers
 * against the sysroot's regex.h and fails fast if the vendored header has
 * drifted from the host ABI: the hb_* regex implementation (compiled from
 * src/libc/src/regex.c) is linked into the same binary as glibc's regex,
 * so the shared regex_t/regmatch_t/re_registers layouts must agree
 * exactly or the race would be meaningless (or crash).
 *
 * Only sizes and the plainly-named member offsets are compared; members
 * whose names are macro-renamed by the private/public header split are
 * intentionally not referenced here.
 */
#define _GNU_SOURCE 1 /* struct re_registers is GNU-only in <regex.h> */

#include <stddef.h>
#include <regex.h>

const unsigned long glibc_regex_layout[] = {
  sizeof(regex_t),
  sizeof(struct re_pattern_buffer),
  sizeof(struct re_registers),
  sizeof(regmatch_t),
  sizeof(regoff_t),
  offsetof(struct re_registers, num_regs),
  offsetof(struct re_registers, start),
  offsetof(struct re_registers, end),
  offsetof(regmatch_t, rm_so),
  offsetof(regmatch_t, rm_eo),
};

/* 1 if regoff_t is signed (it must be: -1 marks non-participating groups). */
const int glibc_regoff_signed = (((regoff_t) -1) < 0);
