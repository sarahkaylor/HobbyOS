/*
 * src/libc/src/cxxrt.cpp — HobbyOS minimal C++ runtime (browser.md §6 F2.4).
 *
 * Policy, the static init/exit mechanism and the exact surface are
 * documented in src/libc/include/cxxrt.h — read that first.  This file is
 * the implementation: operator new/delete over the HobbyOS allocator,
 * the __cxa_guard_* static-init guards, __cxa_atexit/__cxa_finalize for
 * static destructors, and the __cxa_pure_virtual trap.
 *
 * It is compiled as freestanding C++ for both targets and, with
 * -DHOST_TEST, natively for the host unit tests (src/host/cxxrt_test.cpp);
 * the only external dependencies are malloc/free, print_console and
 * abort(), all of which exist in every one of those configurations.
 */
#include "cxxrt.h"

#include <stdlib.h> /* malloc/free/abort — sysroot header on device, glibc on host */

/* libc.h's print_console with a self-contained declaration (works with
 * both compat.c and libc.a without pulling the HOST_TEST rename dance). */
extern "C" void print_console(const char *str);

/* __dso_handle: clang passes &__dso_handle to __cxa_atexit from every TU
 * that has static destructors.  Weak so a host CRT (or a future libc++
 * crtstuff) that defines its own wins without a duplicate-symbol error. */
__attribute__((weak)) void *__dso_handle = (void *)&__dso_handle;

/* ---------------------------------------------------------------- new/delete
 *
 * Backed by the HobbyOS allocator (src/user/malloc.c, archived in libc.a).
 * That allocator returns pointers aligned to 8 bytes only (a 24-byte block
 * header placed after a 16-aligned heap base), which is below the C++
 * requirement that new return storage aligned for any fundamental type
 * (max_align_t = 16 on both targets, where FP/SIMD is live userland since
 * F1.5).  So the runtime over-allocates by 16 bytes, aligns up and stashes
 * the raw malloc pointer in the word below the returned block; delete
 * recovers it.  malloc(0) returns NULL in the HobbyOS allocator, so
 * zero-size requests ask for 1 byte to keep the "never returns NULL" new
 * contract.  Without exceptions the only honest OOM behavior is a
 * diagnostic + abort; see cxxrt.h.
 */
#define CXXRT_MAX_ALIGN 16

extern "C" void cxxrt_oom(void) __attribute__((noreturn));
extern "C" void cxxrt_oom(void) {
  print_console("C++ runtime: operator new out of memory\n");
  abort();
}

static void *cxxrt_alloc_aligned(size_t size) {
  void *raw = malloc((size ? size : 1) + CXXRT_MAX_ALIGN);
  if (!raw)
    cxxrt_oom();
  uintptr_t p = (uintptr_t)raw + sizeof(void *);
  p = (p + (CXXRT_MAX_ALIGN - 1)) & ~(uintptr_t)(CXXRT_MAX_ALIGN - 1);
  ((void **)p)[-1] = raw;
  return (void *)p;
}

static void cxxrt_free_aligned(void *ptr) {
  if (ptr)
    free(((void **)ptr)[-1]);
}

void *operator new(size_t size) { return cxxrt_alloc_aligned(size); }

void *operator new[](size_t size) { return cxxrt_alloc_aligned(size); }

void operator delete(void *ptr) noexcept { cxxrt_free_aligned(ptr); }
void operator delete[](void *ptr) noexcept { cxxrt_free_aligned(ptr); }
void operator delete(void *ptr, size_t) noexcept { cxxrt_free_aligned(ptr); }
void operator delete[](void *ptr, size_t) noexcept { cxxrt_free_aligned(ptr); }

void *operator new(size_t, void *ptr) noexcept { return ptr; }
void operator delete(void *, void *) noexcept { }

/* ------------------------------------------------------------- init guards
 *
 * clang emits, for a function-local static with a dynamic initializer:
 *     if (!(guard.byte0 & 1)) {
 *       if (__cxa_guard_acquire(&guard)) { <init>; __cxa_guard_release(&guard); }
 *     }
 * so byte 0 bit 0 is the "initialized" flag.  Byte re-use follows the
 * Itanium ABI (bit 1 = initialization in progress by another context).
 * Atomic ops keep it correct if threads arrive (P1); no spinning is
 * possible today because HobbyOS processes are single-threaded.
 */
int __cxa_guard_acquire(uint64_t *guard) {
  unsigned char *bytes = (unsigned char *)guard;
  for (;;) {
    unsigned char v = __atomic_load_n(bytes, __ATOMIC_ACQUIRE);
    if (v & 1)
      return 0; /* already initialized */
    if (!(v & 2)) {
      unsigned char expected = 0;
      if (__atomic_compare_exchange_n(bytes, &expected, 2, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 1; /* we own the initialization */
    }
    /* Another context is mid-initialization: wait for it to finish, then
     * report "already done". */
    while (!(__atomic_load_n(bytes, __ATOMIC_ACQUIRE) & 1))
      ;
    return 0;
  }
}

void __cxa_guard_release(uint64_t *guard) {
  __atomic_store_n((unsigned char *)guard, 1, __ATOMIC_RELEASE);
}

void __cxa_guard_abort(uint64_t *guard) {
  __atomic_store_n((unsigned char *)guard, 0, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ atexit
 *
 * Static destructors (and __attribute__((destructor)) handlers) land here
 * via clang's __cxa_atexit(obj_dtor, obj, &__dso_handle) calls.  Fixed
 * table, LIFO, runs at most once per entry.  exit() calls
 * __cxa_finalize(NULL) through a weak reference (src/user/libc.c), after
 * the C atexit() handlers, so C++ destructors see a live process.
 */
#define CXXRT_ATEXIT_MAX 64

struct cxxrt_atexit_entry {
  void (*func)(void *);
  void *arg;
  void *dso;
};

static struct cxxrt_atexit_entry cxxrt_atexit_table[CXXRT_ATEXIT_MAX];
static int cxxrt_atexit_count;

int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
  if (!func || cxxrt_atexit_count >= CXXRT_ATEXIT_MAX)
    return -1;
  cxxrt_atexit_table[cxxrt_atexit_count].func = func;
  cxxrt_atexit_table[cxxrt_atexit_count].arg = arg;
  cxxrt_atexit_table[cxxrt_atexit_count].dso = dso;
  cxxrt_atexit_count++;
  return 0;
}

void __cxa_finalize(void *dso) {
  for (int i = cxxrt_atexit_count - 1; i >= 0; i--) {
    struct cxxrt_atexit_entry *e = &cxxrt_atexit_table[i];
    if (!e->func)
      continue;
    if (dso && e->dso != dso)
      continue;
    void (*func)(void *) = e->func;
    void *arg = e->arg;
    e->func = 0; /* mark consumed: finalize is idempotent */
    func(arg);
  }
}

/* ------------------------------------------------------------- pure virtual
 *
 * Reached only through the vtable stub of an abstract class whose
 * constructor never ran (or similar UB).  There is no recovery.
 */
void __cxa_pure_virtual(void) {
  print_console("C++ runtime: pure virtual function call\n");
  abort();
}
