#ifndef HB_STRTAB_H
#define HB_STRTAB_H

/*
 * Small string/hash/error-buffer helpers for LDHOBBY.SO.
 *
 * The interpreter has no libc (docs/shared-libs.md §4 lane D), so the handful
 * of primitives it needs live here.  The copy loops use volatile accesses on
 * purpose: with a plain loop clang recognises the idiom and emits a call to
 * memcpy, which the freestanding link cannot satisfy.
 */

#include <stdint.h>

#include "ldso.h"

/* The GNU and SysV ELF hash functions, byte-for-byte as ld.so computes them:
 * a symbol only resolves if the hash built here matches the one in the
 * module's table, so these are ABI, not a convenience. */
uint32_t hb_ldso_gnu_hash(const char *name);
uint32_t hb_ldso_sysv_hash(const char *name);

/* Bounded, always-NUL-terminated string copy. */
void hb_ldso_strcpy(char *dst, const char *src, uint32_t cap);

/* Error-buffer composition.  err_reset then any number of err_add*; each call
 * truncates instead of overflowing, so a message is always a valid string. */
void hb_ldso_err_reset(char *err, uint32_t cap);
void hb_ldso_err_add(char *err, uint32_t cap, const char *s);
void hb_ldso_err_add_hex(char *err, uint32_t cap, uint64_t v);
void hb_ldso_err_add_dec(char *err, uint32_t cap, uint64_t v);

#endif /* HB_STRTAB_H */
