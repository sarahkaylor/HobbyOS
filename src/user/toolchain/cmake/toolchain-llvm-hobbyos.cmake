# HobbyOS — LLVM self-hosting toolchain file (selfhost.md §2/AD-1..AD-6; lane L1).
#
# Cross-compiles clang/lld/llvm-ar/llvm-objcopy for the HobbyOS bare-metal
# target (aarch64-none-elf) on the workstation.  Based on the WebKit-port
# precedent (webkit-hobbyos/HobbyOS/toolchain-hobbyos.cmake +
# docs/toolchain-sysroot.md), simplified for LLVM's in-tree build.
#
# Sysroot layout (staged scratch under obj/selfhost/sysroot/; obj/ gitignored):
#   headers : obj/selfhost/sysroot/include/{libc,kernel,user}-include
#             (copies of src/libc/include, src/include, src/user_include)
#   builtins: obj/selfhost/sysroot/include/builtins   (clang resource-dir
#             include/ copied from the HOST clang; hermetic -nostdinc model)
#   libs    : obj/selfhost/sysroot/lib/{crt0.o,libc.a,libcxx.a}
#             (obj/<arch>/ outputs; crt0.o is also a libc.a member, staged
#              standalone so the link closure can name it explicitly)
#   script  : src/user/linker.ld   (v2 image base 0x1000000000, 64 GiB)
#
# Configure-time probe policy: no target code may execute on the host
# (aarch64-none-elf binaries need HobbyOS), so every try-compile is
# compile-only via CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY.
#
# Tablegen: the cross build CANNOT run target llvm-tblgen/clang-tblgen, so
# a small native (host) build of the same tree provides them and the cross
# configure passes -DLLVM_TABLEGEN / -DCLANG_TABLEGEN explicitly (AD-6, L1
# spike; see docs/selfhost/llvm-bootstrap.md §tablegen).
#
# Usage (from the extracted llvm/ source dir):
#   cmake -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<repo>/src/user/toolchain/cmake/toolchain-llvm-hobbyos.cmake \
#     -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 \
#     -DLLVM_ENABLE_PROJECTS="clang;lld" \
#     -DLLVM_TABLEGEN=<...>/llvm-tblgen -DCLANG_TABLEGEN=<...>/clang-tblgen \
#     <repo>/third_party/llvm-21.1.8/src/llvm-project-21.1.8.src/llvm

# Repo root = four levels up from this file (src/user/toolchain/cmake/).
get_filename_component(_tc_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_toolchain_root "${_tc_dir}/../../../.." ABSOLUTE)

if (NOT HOBBYOS_ARCH)
    set(HOBBYOS_ARCH "arm")
endif ()

if (HOBBYOS_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(HOBBYOS_TRIPLE aarch64-none-elf)
    set(HOBBYOS_ARCH_CFLAGS "-mcpu=cortex-a53")
elseif (HOBBYOS_ARCH STREQUAL "intel")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    set(HOBBYOS_TRIPLE x86_64-none-elf)
    set(HOBBYOS_ARCH_CFLAGS "-mno-red-zone -mcmodel=large")
else ()
    message(FATAL_ERROR "HOBBYOS_ARCH must be 'arm' or 'intel' (got '${HOBBYOS_ARCH}')")
endif ()

set(CMAKE_SYSTEM_NAME HobbyOS)

# Provide our own Platform/HobbyOS.cmake module (sets UNIX=1 so LLVM's
# HandleLLVMOptions compiles the POSIX support shim; without it LLVM stops
# with "Unable to determine platform").  Must precede project().
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/modules")

# clang is both the C and C++ driver on this workstation (project
# convention; the driver picks the frontend from the source extension).
set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang")
set(CMAKE_ASM_COMPILER "clang")
# LLD comes via -fuse-ld=lld in HOBBYOS_TARGET_FLAGS.  Do NOT also set
# CMAKE_LINKER_TYPE LLD: CMake rejects it for unknown systems
# ("LINKER_TYPE 'LLD' is unknown or not supported by this toolchain",
# L1 spike run 4) and it is redundant with the driver flag.

# No target code may run at configure time.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Toolchain binaries/scripts come from PATH (clang, ld.lld, ...).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

# ---------------------------------------------------------------------------
# Compiler flags.  Hermetic include model (Makefile:145-156 + AD-5): -nostdinc
# plus an explicit -isystem of the HOST clang resource dir (freestanding
# stdarg.h/stddef.h/... stay reachable; host glibc never leaks in) and the
# HobbyOS sysroot headers.
# ---------------------------------------------------------------------------
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE HOBBYOS_CLANG_RESOURCE
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(HOBBYOS_SYSROOT "${_toolchain_root}/obj/selfhost/sysroot")
set(HOBBYOS_TARGET_FLAGS "--target=${HOBBYOS_TRIPLE} -ffreestanding -fuse-ld=lld")

set(HOBBYOS_FREESTANDING_INCLUDE
    "-nostdinc -isystem ${HOBBYOS_CLANG_RESOURCE}/include")
set(HOBBYOS_SYSROOT_INCLUDES
    "-isystem ${HOBBYOS_SYSROOT}/include/libc-include"
    "-isystem ${HOBBYOS_SYSROOT}/include/kernel-include"
    "-isystem ${HOBBYOS_SYSROOT}/include/user-include"
    "-isystem ${_toolchain_root}/src/user/toolchain/sysroot-headers"
    # Force-include the missing-declarations shim on every TU: LLVM's Unix
    # shim compiles socket/bind/listen/accept/connect/pipe + alarm/
    # gethostname/getsid/strsignal unconditionally, and HobbyOS lacks the
    # declarations.  -include is build-config (no LLVM source patch); the
    # BODIES are declared but never defined in this lane, so the LINK step
    # reports each missing symbol verbatim (L2's blocker list,
    # l1-dep-surface.txt B).  -include on a path resolves relative to the
    # include dirs, so give the full path.
    "-include ${_toolchain_root}/src/user/toolchain/sysroot-headers/_missing.h")
# A CMake LIST must never reach *.cxx_flags / a compile line as-is: the Ninja
# generator writes `;` separators literally and the shell mis-splits them
# (observed: "/bin/sh: 1: -isystem: not found" in the L1 spike). Join to a
# plain space-separated string (the same trap the WebKit toolchain hit).
list(JOIN HOBBYOS_SYSROOT_INCLUDES " " HOBBYOS_SYSROOT_INCLUDES_STR)

set(CMAKE_C_FLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} ${HOBBYOS_FREESTANDING_INCLUDE} ${HOBBYOS_SYSROOT_INCLUDES_STR} ${CMAKE_C_FLAGS}")
# LLVM is C++17; RTTI/exceptions handled by LLVM_ENABLE_EH=OFF/
# LLVM_ENABLE_RTTI=OFF (AD-10).  -nostdinc++ keeps host C++ headers out.
# The libcxx include MUST be -I (lies before clang's builtin dir: -isystem
# would sort after the resource dir and libc++ fails on <stddef.h>).
set(CMAKE_CXX_FLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} ${HOBBYOS_FREESTANDING_INCLUDE} -nostdinc++ -I${_toolchain_root}/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include ${HOBBYOS_SYSROOT_INCLUDES_STR} ${CMAKE_CXX_FLAGS}")
set(CMAKE_ASM_FLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} ${CMAKE_ASM_FLAGS}")

# ---------------------------------------------------------------------------
# Link model (AD-1/AD-4, F1): everything static; the closure lands at the END
# of the link line via CMAKE_CXX_STANDARD_LIBRARIES.  -Wl,--start-group is
# required (libc.a <-> libcxx.a mutual references, P3 closure).  crt0.o
# defines _start; the ELF entry is pointed there.
# ---------------------------------------------------------------------------
set(HOBBYOS_LINKER_SCRIPT "${_toolchain_root}/src/user/linker.ld")

set(HOBBYOS_SYSROOT_CLOSURE
    "-Wl,--start-group"
    "${HOBBYOS_SYSROOT}/lib/crt0.o"
    "${HOBBYOS_SYSROOT}/lib/libcxx.a"
    "${HOBBYOS_SYSROOT}/lib/libc.a"
    "-Wl,--end-group")
list(JOIN HOBBYOS_SYSROOT_CLOSURE " " HOBBYOS_SYSROOT_CLOSURE_STR)

set(CMAKE_CXX_STANDARD_LIBRARIES "${HOBBYOS_SYSROOT_CLOSURE_STR}"
    CACHE STRING "HobbyOS sysroot closure (crt0 + libcxx.a + libc.a)" FORCE)
set(CMAKE_C_STANDARD_LIBRARIES "${HOBBYOS_SYSROOT_CLOSURE_STR}"
    CACHE STRING "HobbyOS sysroot closure (crt0 + libc.a)" FORCE)

set(CMAKE_EXE_LINKER_FLAGS
    "-nostdlib -nostartfiles -Wl,-T,${HOBBYOS_LINKER_SCRIPT} -Wl,-e,_start -L${HOBBYOS_SYSROOT}/lib/stubs"
    CACHE STRING "HobbyOS exe link flags" FORCE)

# ---------------------------------------------------------------------------
# HobbyOS sysroot gap-filler object (WebKit-fork gaps.c precedent; lane L1
# spike).  The LLVM tools link a POSIX surface HobbyOS's libc does not
# provide (l1-dep-surface.txt B); these stubs make the toolchain's own link
# closure go end-to-end.  Every stub is a documented L2 work item.  Compiled
# once per build dir into the closure, like the WebKit fork-lib loop.
# ---------------------------------------------------------------------------
set(HOBBYOS_GAPS_SRC "${_toolchain_root}/src/user/toolchain/sysroot-headers/gaps.c")
if (NOT EXISTS "${HOBBYOS_GAPS_SRC}")
  message(FATAL_ERROR "missing HobbyOS gaps.c: ${HOBBYOS_GAPS_SRC}")
endif ()
set(HOBBYOS_GAPS_OBJ "${CMAKE_BINARY_DIR}/hobbyos_gaps.o")
if (NOT EXISTS "${HOBBYOS_GAPS_OBJ}" OR "${HOBBYOS_GAPS_SRC}" IS_NEWER_THAN "${HOBBYOS_GAPS_OBJ}")
  # Gap object flags as a REAL argv list (space-joined string -> split):
  # a CMake LIST would pass `;` into --target and clang rejects the triple.
  set(HOBBYOS_GAPS_CFLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} -ffreestanding ${HOBBYOS_FREESTANDING_INCLUDE} ${HOBBYOS_SYSROOT_INCLUDES_STR}")
  separate_arguments(HOBBYOS_GAPS_CFLAGS_LIST NATIVE_COMMAND "${HOBBYOS_GAPS_CFLAGS}")
  execute_process(
    COMMAND "${CMAKE_C_COMPILER}" ${HOBBYOS_GAPS_CFLAGS_LIST} "${HOBBYOS_GAPS_SRC}" -c -o "${HOBBYOS_GAPS_OBJ}"
    RESULT_VARIABLE HOBBYOS_GAPS_RC
    OUTPUT_VARIABLE HOBBYOS_GAPS_OUT
    ERROR_VARIABLE HOBBYOS_GAPS_ERR)
  if (NOT HOBBYOS_GAPS_RC EQUAL 0)
    message(FATAL_ERROR "gaps.c build failed: ${HOBBYOS_GAPS_OUT} ${HOBBYOS_GAPS_ERR}")
  endif ()
endif ()
set(HOBBYOS_GAPS_IN_CLOSURE "${HOBBYOS_GAPS_OBJ} ${HOBBYOS_SYSROOT_CLOSURE_STR}")
set(CMAKE_CXX_STANDARD_LIBRARIES "${HOBBYOS_GAPS_IN_CLOSURE}"
    CACHE STRING "HobbyOS sysroot closure (gaps + crt0 + libcxx.a + libc.a)" FORCE)
set(CMAKE_C_STANDARD_LIBRARIES "${HOBBYOS_GAPS_IN_CLOSURE}"
    CACHE STRING "HobbyOS sysroot closure (gaps + crt0 + libc.a)" FORCE)

# No dynamic linking anywhere.
set(CMAKE_DL_LIBS "")
set(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

# LLVM-specific: the built artifacts are target binaries, never run on the
# host.  Tell CMake the compiler target so LLVM_HOST_TRIPLE defaults match.
set(CMAKE_C_COMPILER_TARGET "${HOBBYOS_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${HOBBYOS_TRIPLE}")
