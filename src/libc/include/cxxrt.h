#ifndef HOBBYOS_CXXRT_H
#define HOBBYOS_CXXRT_H

/*
 * HobbyOS minimal C++ runtime (browser.md §6, F2.4) — policy + surface.
 *
 * ===========================================================================
 * Policy (binding for userland C++ until P3 lands libc++)
 * ===========================================================================
 *
 *  - Compile user C++ with `-fno-exceptions -fno-rtti` (the Makefile's
 *    CXX_USER_FLAGS does; the #errors below enforce it for any TU that
 *    includes this header).  Rationale: without an unwinder/`<exception>`
 *    surface, exception support would link against `_Unwind_*` /
 *    `__cxa_throw` symbols no one provides — failing at link time is the
 *    desired behavior, and this header makes it fail at compile time with
 *    a readable message.  RTTI is off because `typeid`/`dynamic_cast`
 *    would need typeinfo emission and library support; plain virtual
 *    dispatch does not.
 *  - There is no C++ standard library yet (no libc++, no <new>, no
 *    <string>).  This runtime provides exactly the link-time primitives
 *    clang emits for plain-language C++: operator new/delete, static-init
 *    guards, static destructor registration, and the pure-virtual trap.
 *    P3 (libc++) replaces none of these — they stay — but adds the
 *    standard headers on top.
 *  - `thread_local` is not supported (no TLS runtime yet; P1 adds it).
 *    Plain statics and function-local statics work.
 *  - The program entry point must be declared `extern "C"`: crt0.c is C
 *    and calls the unmangled symbol `main`, but clang mangles `main` in
 *    freestanding C++ mode (the hosted "main is never mangled" special
 *    case does not apply under -ffreestanding).  So write:
 *        extern "C" int main(int argc, char **argv) { ... }
 *    The link error if you forget names the fix ("did you mean to declare
 *    main(int, char**) as extern \"C\"?").
 *
 * ===========================================================================
 * Static init/exit mechanism (crt0 + linker.ld; the loader needs no change)
 * ===========================================================================
 *
 *  1. The compiler places the address of every translation unit's static
 *     constructor thunk (`_GLOBAL__sub_I_<file>`) in a `.init_array`
 *     section, plus any `__attribute__((constructor))` functions and
 *     `init_priority` handlers (sorted by priority inside the section).
 *  2. src/user/linker.ld collects `*(.init_array*)` into one output
 *     section between `.rodata` and `.data` and brackets it with the
 *     hidden symbols `__init_array_start` / `__init_array_end`.  Being
 *     part of the contiguous image, the array is inside the flat binary
 *     that `objcopy -O binary` produces, and inside the region the kernel
 *     loader copies to USER_IMG_BASE (v2 base, 64 GiB since the S5 flip) —
 *     hence no loader
 *     change is required.  `.fini_array` is collected the same way and
 *     reserved for P3; nothing runs it yet.
 *  3. src/libc/crt0.c's `_start` walks [__init_array_start,
 *     __init_array_end) and calls each entry AFTER argv is built and
 *     BEFORE `main()` — so all static constructors have run by the time
 *     main executes, in section order (priority first, then TU order).
 *  4. Static destructors are NOT run from `.fini_array`.  clang registers
 *     each non-trivial static object's destructor through `__cxa_atexit`
 *     (unless built with -fno-use-cxa-atexit, which this tree does not
 *     use).  exit() (src/user/libc.c) makes a weak call to
 *     `__cxa_finalize(NULL)` after the C atexit handlers, which runs the
 *     registered C++ destructors in reverse registration order.  The
 *     weak reference keeps non-C++ programs linkable.
 *
 * Note on plain `_start`-style programs: they never link crt0.o, so they
 * never walk `.init_array`.  That is fine — a program that defines its own
 * `_start` is C by construction and has no static constructors.  Use
 * main() + crt0 (or link crt0.o) for C++ code.
 */

#include <stddef.h>
#include <stdint.h>

/* This header is part of the C++ runtime surface; requiring the F2.4
 * build policy here means any user C++ TU either follows it or fails with
 * a message that names the rule. */
#if defined(__cplusplus) && defined(__EXCEPTIONS)
#error "HobbyOS user C++ must be built with -fno-exceptions (F2.4 policy; see cxxrt.h)"
#endif
#if defined(__cplusplus) && defined(__GXX_RTTI)
#error "HobbyOS user C++ must be built with -fno-rtti (F2.4 policy; see cxxrt.h)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

  /* Itanium-C++-ABI entry points; also callable from C for tests. */

  /* Static-initialization guard for function-local statics (and any
   * guard-using construct).  Semantics mirror libc++abi: acquire returns 1
   * when the caller must run the initializer (and then call release), 0
   * when the object is already initialized.  Implementation is
   * single-process-correct and atomic-safe for the future threads case. */
  int __cxa_guard_acquire(uint64_t *guard);
  void __cxa_guard_release(uint64_t *guard);
  void __cxa_guard_abort(uint64_t *guard);

  /* Register a destructor to run at exit (destructors LIFO).  Returns 0 on
   * success, -1 if the fixed-size table is full (the destructor is then
   * dropped — documented limitation; raise CXXRT_ATEXIT_MAX if a program
   * ever needs more).  `dso` is the registering object's __dso_handle. */
  int __cxa_atexit(void (*func)(void *), void *arg, void *dso);

  /* Run registered destructors.  __cxa_finalize(NULL) runs all of them in
   * reverse registration order; called from exit() (weak) and available to
   * tests.  Idempotent: an entry runs at most once. */
  void __cxa_finalize(void *dso);

  /* Called through the vtable slot of an abstract class if a pure virtual
   * member is ever dispatched (object not constructed, etc.).  Prints a
   * diagnostic and aborts.  noreturn. */
  void __cxa_pure_virtual(void) __attribute__((noreturn));

  /* Per-module handle clang passes to __cxa_atexit (weak definition here so
   * hosts/CRT implementations that already define it win). */
  extern void *__dso_handle;

#ifdef __cplusplus
} /* extern "C" */

/* Placement new (the rest of <new> arrives with P3).  Declared so
 * `new (buf) T(...)` compiles without a standard library. */
void *operator new(size_t size, void *ptr) noexcept;
void operator delete(void *ptr, void *place) noexcept;
#endif

#endif /* HOBBYOS_CXXRT_H */
