/*
 * src/user/rw_tls_dso.c — a dlopen'ed DSO with its own __thread variable.
 *
 * M2's dynamic-TLS proof.  The guest's glibc ld.so allocates this DSO's TLS
 * block and resolves `__tls_get_addr` itself (the kernel owes only a per-thread
 * FS base and mmap); the stress probe loads this at runtime with dlopen() and
 * checks that two threads see independent copies.
 *
 * The initialiser is deliberately non-zero: a fresh thread must read 7, which
 * proves the block was actually initialised per thread rather than inheriting
 * whatever happened to be in the TCB.
 */
static __thread int tls_var = 7;

int tls_get(void) { return tls_var; }

void tls_set(int v) { tls_var = v; }

int tls_initial(void) { return 7; }
