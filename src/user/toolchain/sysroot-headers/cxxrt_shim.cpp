// HobbyOS toolchain closure: C++ runtime shim (lane w5-nothrow).
//
// PURPOSE
// =======
// Build-config-level fix (AD-3: zero LLVM/libc++ source patches) for the
// w4-clang wall "clang compiles a TU and then traps `brk #1` / EC=0x3C at
// operator new(std::nothrow)" (docs/selfhost/lanes/w4-clang-report.json,
// blocker b2; selfhost.md §2).  Root cause (proved in w5-nothrow):
//
//   HobbyOS's OS userland libc.a embeds src/libc/src/cxxrt.o, which defines
//   operator new/delete as STRONG functions in plain .text (deliberate OS
//   design: "the HobbyOS allocator remains the single backing for both
//   worlds" -- cxxrt.cpp header note).  A toolchain binary's link pulls
//   that member to satisfy __cxa_atexit/__dso_handle (libc++abi's
//   cxa_atexit.cpp is deliberately not built).  The pulled member's strong
//   _Znwm then beats libc++ new.o's WEAK _Znwm, and -- crucially -- lands
//   OUTSIDE libc++'s "__lcxx_override" ELF section (which its weak
//   definitions carry via _LIBCPP_OVERRIDABLE_FUNCTION).  libc++ built
//   with -fno-exceptions asserts at runtime that the throwing operator new
//   is inside that section (new.cpp: operator new(nothrow) ->
//   __is_function_overridden<&operator new>()); with _Znwm outside, the
//   assert trips -> brk #1 -> the in-OS process is killed.
//
//   Observed (broken clang-21): _Znwm T 0x100484e410 in .text; section
//   __lcxx_override [0x1004866cc0, 0x1004866d84) -- _Znwm 100 KB before the
//   section start => "overridden" => assert.  Control NPNEW.BIN (no cxxrt.o
//   in its reference graph) shows _Znwm == section start and passes.
//
// FIX (this file + toolchain-llvm-hobbyos.cmake closure edit)
// ===========================================================
//   This object is linked FIRST in the toolchain closure (before libcxx.a
//   and libc.a).  It provides the complete external surface of cxxrt.o:
//     - operator new/new[]/delete/delete[] (strong, so cxxrt.o's strong
//       copies can never win; WEAK would silently re-break if cxxrt.o were
//       ever still pulled),
//     - the new/delete that libc++'s nothrow-override check inspects are
//       declared __attribute__((section("__lcxx_override"))) so the
//       resolved _Znwm/_Znam land INSIDE libc++'s override section and the
//       runtime check passes (allocation behavior is unchanged: the
//       over-aligned allocator below is a faithful port of cxxrt.cpp),
//     - __cxa_atexit/__cxa_finalize/__dso_handle (strong where cxxrt's are
//       strong), so those undefined refs resolve here and lld never
//       extracts cxxrt.o from libc.a -- its strong plain-.text new/delete
//       never enter the toolchain binary at all.
//   The aligned operator new (align_val_t) stays libc++'s weak in-section
//   definition (its aligned_alloc backend is HobbyOS's aligned_alloc), so
//   the aligned-nothrow override checks pass too.
//
// SCOPE / SAFETY
// ==============
//   * Linked ONLY into the LLVM toolchain cross-build closure (this cmake
//     file is used by no other build).  The OS userland build (Makefile ->
//     libc.a/cxxrt.o) is untouched.
//   * Fail-safe: if a future closure/archive change ever pulls cxxrt.o
//     again, its strong new/delete would collide with this file's strong
//     definitions -> a LOUD duplicate-symbol link error instead of a
//     silently re-broken in-OS clang.
//   * OS-wide candidate (out of this lane's scope): give the new/delete in
//     src/libc/src/cxxrt.cpp the __lcxx_override section attribute too, so
//     ANY program that links both cxxrt.o and libc++ passes the nothrow
//     override check.
//
// The __cxa_* / allocator logic below is a direct port of
// src/libc/src/cxxrt.cpp (same OS); keep the two in sync when either
// changes.

#include <stdint.h> /* uintptr_t */
#include <stdlib.h> /* malloc/free/abort -- sysroot header (see cxxrt.cpp) */

/* print_console: HobbyOS console writer (self-contained decl, as in
 * cxxrt.cpp, so this TU needs no other HobbyOS header). */
extern "C" void print_console(const char *str);

/* ------------------------------------------------------------------ new/delete
 * Port of cxxrt.cpp's over-aligned allocator: malloc(0) may return NULL on
 * HobbyOS, so zero-size requests ask for 1 byte; storage is over-allocated
 * by CXXRT_MAX_ALIGN and 16-aligned with the raw pointer stashed 8 bytes
 * below the returned block (the HobbyOS allocator is only 8-aligned).
 */
#define CXXRT_MAX_ALIGN 16

extern "C" void cxxrt_oom(void) __attribute__((noreturn));
extern "C" void cxxrt_oom(void) {
  print_console("C++ runtime: operator new out of memory\n");
  abort();
}

static void *tc_alloc_aligned(size_t size) {
  void *raw = malloc((size ? size : 1) + CXXRT_MAX_ALIGN);
  if (!raw)
    cxxrt_oom();
  uintptr_t p = (uintptr_t)raw + sizeof(void *);
  p = (p + (CXXRT_MAX_ALIGN - 1)) & ~(uintptr_t)(CXXRT_MAX_ALIGN - 1);
  ((void **)p)[-1] = raw;
  return (void *)p;
}

static void tc_free_aligned(void *ptr) {
  if (ptr)
    free(((void **)ptr)[-1]);
}

/* The nothrow-override check inspects the THROWING forms; they must live in
 * libc++'s __lcxx_override section.  Strong: see the header comment. */
__attribute__((section("__lcxx_override"))) void *operator new(size_t size) {
  return tc_alloc_aligned(size);
}
__attribute__((section("__lcxx_override"))) void *operator new[](size_t size) {
  return tc_alloc_aligned(size);
}

void operator delete(void *ptr) noexcept { tc_free_aligned(ptr); }
void operator delete[](void *ptr) noexcept { tc_free_aligned(ptr); }
void operator delete(void *ptr, size_t) noexcept { tc_free_aligned(ptr); }
void operator delete[](void *ptr, size_t) noexcept { tc_free_aligned(ptr); }

/* Placement forms (cxxrt.h declares the same).  Inline-addressable in
 * practice; defined here so the surface is complete and `&::operator new`
 * always resolves in a closure that does not pull cxxrt.o. */
void *operator new(size_t, void *ptr) noexcept { return ptr; }
void operator delete(void *, void *) noexcept {}

/* -------------------------------------------------------------------- atexit
 * Port of cxxrt.cpp's static-destructor registration (exit() calls
 * __cxa_finalize(NULL) through a weak reference; libc.a provides that
 * exit()).  Table, LIFO, runs at most once per entry.
 */
#define TC_ATEXIT_MAX 64

struct tc_atexit_entry {
  void (*func)(void *);
  void *arg;
  void *dso;
};

static struct tc_atexit_entry tc_atexit_table[TC_ATEXIT_MAX];
static int tc_atexit_count;

extern "C" int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
  if (!func || tc_atexit_count >= TC_ATEXIT_MAX)
    return -1;
  tc_atexit_table[tc_atexit_count].func = func;
  tc_atexit_table[tc_atexit_count].arg = arg;
  tc_atexit_table[tc_atexit_count].dso = dso;
  tc_atexit_count++;
  return 0;
}

extern "C" void __cxa_finalize(void *dso) {
  for (int i = tc_atexit_count - 1; i >= 0; i--) {
    struct tc_atexit_entry *e = &tc_atexit_table[i];
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

/* Per-module handle clang passes to __cxa_atexit.  Weak (as in cxxrt.cpp): a
 * CRT that defines its own wins without a duplicate-symbol error. */
__attribute__((weak)) void *__dso_handle = (void *)&__dso_handle;
