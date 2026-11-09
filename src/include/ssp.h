/* SEC-4 (GB-246): the stack-canary panic report.
 *
 * Split out of ssp.c so the formatter and the frame-layout decoder can be
 * unit-tested on the host: the panic path itself halts, so the only way to
 * prove "it reports rather than crashes silently" is to drive these two pure
 * functions with a synthetic smashed frame.
 */
#ifndef HOBBYOS_SSP_H
#define HOBBYOS_SSP_H

#include <stdint.h>

/* Parse a function prologue that begins with push %rbp; mov %rsp,%rbp and
 * return the byte offset of the compiler's stack canary slot from %rbp
 * (8 * (saved-register count + 1)), or 0 if `code` is not such a prologue.
 * x86_64 only; returns 0 elsewhere. */
int ssp_prologue_canary_offset(const unsigned char *code, int len);

/* Kernel side: scan backwards from a return address inside the faulting
 * function for its prologue and return the canary offset from %rbp, or 0. */
uint64_t ssp_pc_canary_offset(uint64_t pc);

/* Format the panic report into `out` (NUL-terminated), returning its length.
 * `have_found` distinguishes a decoded canary slot from "unresolved". */
int ssp_format_report(char *out, int cap, uint64_t pc, uint64_t sp, uint64_t fp,
                      uint64_t guard, uint64_t found, int have_found);

#endif
