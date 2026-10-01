/*
 * HobbyOS l3-rtti host test support: the four leaf symbols the vendored RTTI
 * closure (private_typeinfo.o / stdlib_typeinfo.o / stdlib_exception.o)
 * references at link time and that the closed-world host link (-nostdlib++)
 * must therefore provide itself.  On-device these come from the same
 * libcxx.a set the test's scope mirrors:
 *
 *   __abort_message    libcxxabi abort_message.cpp   (class A)
 *   __cxa_pure_virtual libcxxabi cxa_virtual.cpp     (class A)
 *   __cxa_bad_cast     libcxxabi cxa_aux_runtime.cpp (class A, whose
 *                      no-exceptions branch calls std::terminate(); this
 *                      shim prints and abort()s — the same SIGABRT the
 *                      rtti_test's forked-child check asserts)
 *   operator delete(void*, size_t)  libcxx/src/new.cpp (weak) and
 *                      src/libc/src/cxxrt.cpp; the host object needs the
 *                      symbol to link, and the test never calls it (no
 *                      allocation on the failure paths)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <cstddef>

extern "C" void __abort_message(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  fputs("libc++abi abort: ", stderr);
  vfprintf(stderr, format, ap);
  va_end(ap);
  fputc('\n', stderr);
  abort();
}

extern "C" void __cxa_pure_virtual(void) {
  fputs("libc++abi: pure virtual function called\n", stderr);
  abort();
}

extern "C" void __cxa_bad_cast(void) {
  fputs("libc++abi: bad dynamic_cast (std::bad_cast)\n", stderr);
  abort();
}

void operator delete(void *ptr, std::size_t) noexcept { free(ptr); }
