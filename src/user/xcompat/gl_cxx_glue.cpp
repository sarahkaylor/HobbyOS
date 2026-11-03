/*
 * gl_cxx_glue.cpp - the last few C++ runtime symbols libGL needs.
 *
 * Lane L5 (M4).  libGL contains Mesa's GLSL/NIR compilers, which are C++:
 * they use std::string, std::mutex and std::error_code.  The compat class is
 * a glibc process, and glibc does not ship a C++ runtime, so libGL must carry
 * its own.  Most of it comes from the tree's pinned libc++ 21.1.8, rebuilt
 * -fPIC into libcxx-pic.a (see tools/mesa/build-hobbyos.sh, step 4).  That
 * archive is a *freestanding* build (-fno-exceptions -fno-rtti), which is
 * exactly what Mesa wants, but three categories of symbol are still missing:
 *
 *   1. functions libc++ leaves to libc++abi when LIBCXX_BUILDING_LIBCXXABI
 *      is set (the archive build defines it for the whole class);
 *   2. functions that only exist on the exception path, which a
 *      -fno-exceptions build never takes but must still resolve;
 *   3. std::exception's out-of-line destructor/vtable, which libc++ puts in
 *      src/exception.cpp - and that TU drags in libc++abi's full exception
 *      machinery (cxa_exception.cpp + the unwinder).
 *
 * Rather than link the unwinder into a rasteriser, this file provides the
 * few symbols as fail-closed no-ops: nothing in Mesa throws, so the bodies
 * are unreachable.  Every definition names the symbol it satisfies.
 *
 * Compiled with the same flags as the rest of the C++ half:
 *   --target=x86_64-none-elf -ffreestanding -fPIC -fno-exceptions -fno-rtti
 *   -std=c++23 -nostdinc++ -I<libcxx>/include -D_LIBCPP_BUILDING_LIBRARY
 */

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <new>

/* ---- 1. libc++abi-owned pieces (never called) ---- */

/* libc++abi's cxa_exception.cpp provides these; only throw/catch paths use
 * them, and -fno-exceptions code cannot reach them. */
extern "C" void *__cxa_current_primary_exception(void) { return nullptr; }
extern "C" void __cxa_increment_exception_refcount(void *) {}
extern "C" void __cxa_decrement_exception_refcount(void *) {}
extern "C" void __cxa_rethrow_primary_exception(void *) {}
extern "C" int __cxa_uncaught_exceptions(void) { return 0; }

/* std::terminate() is required by the standard even in a no-exceptions
 * build; libc++abi's abort_message.cpp provides __abort_message, and Mesa
 * never calls this. */
namespace std {
void terminate() noexcept { abort(); }
}

/* ---- 2. exception-path-only pieces ---- */

namespace std {

/* libc++'s src/exception.cpp would define these; it is not linked (see the
 * header comment).  An empty body is correct for a build that never throws:
 * the destructor has nothing to release. */
exception::~exception() noexcept {}

/* libc++abi's stdlib_exception.cpp owns what(); it is not linked. */
const char *exception::what() const noexcept { return "std::exception"; }

} // namespace std

/* libc++abi's stdlib_exception.cpp defines this; it is not linked, and a
 * -fno-exceptions build cannot reach the call site (operator new's failure
 * path goes to the nothrow/terminate route first). */
namespace std {
[[noreturn]] void __throw_bad_alloc() { abort(); }
}

/* ---- 3. std::get_new_handler ---- */
/* libc++ 21 asks libc++abi for this when LIBCXX_BUILDING_LIBCXXABI is set
 * (src/new_handler.cpp).  The tree's libc++abi does not ship that TU, so the
 * archive leaves it undefined.  Mesa replaces operator new's failure path
 * with its own OOM handler anyway; returning nullptr means "no handler",
 * which makes operator new call std::terminate (above) if it ever fails. */
namespace std {
new_handler get_new_handler() noexcept { return nullptr; }
}
