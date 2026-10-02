# HobbyOS — WebKit cross toolchain (WK-1 draft; OQ-10 / P7.3).
#
# STATUS: DRAFT for the HobbyOS repo's planned src/user/browser/webkit/
# toolchain-hobbyos.cmake (the parent lands it there). This copy is the
# WebKit-fork-side working draft, exercised by the WK-1 'jsc' build.
#
# Sysroot layout (HobbyOS repo, READ-ONLY from this fork):
#   headers : <sysroot>/src/libc/include, <sysroot>/src/include,
#             <sysroot>/src/user_include
#   libc++  : <sysroot>/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/
#             libcxx/include            (built -fno-exceptions; RTTI closure
#             members for ICU's -frtti TUs live in libcxx.a, l3-rtti lane)
#   static  : <sysroot>/obj/<arch>/{libc.a,libcxx.a,crt0.o}
#   ICU     : <sysroot>/obj/<arch>/icu/{libicuuc,libicui18n,libicudata}.a
#             headers <sysroot>/third_party/icu-78.3/src/source/{common,i18n}/unicode
#             (libicuuc.a has an undefined icudt78_dat reference, satisfied by
#              the single libicudata.a member -> no whole-archive needed)
#   link    : <sysroot>/src/user/linker.ld   (v2 image base 0x1000000000, 64 GiB)
#
# Compile model mirrors the repo's USER_CFLAGS / the l6-icu-target cross
# recipe (see third_party/icu-78.3/cross-notes.md): clang as both the C and
# C++ driver (no clang++ on the workstation), --target=<triple>, -ffreestanding,
# -fuse-ld=lld, -nostdlib at link, crt0.o/libc.a/libcxx.a supplied explicitly.
#
# Usage:
#   cmake -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=<fork>/HobbyOS/toolchain-hobbyos.cmake \
#       -DPORT=HobbyOS \
#       -DHOBBYOS_ARCH=arm|intel \
#       -DCMAKE_BUILD_TYPE=Release \
#       -DHDYOS_SYSROOT=<HobbyOS repo>   # optional, defaults below
#       <fork>
#
# Configure-time probe policy: HobbyOS cannot execute target code during
# configure, so CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY turns every
# try-compile probe into a compile-only check.

if (NOT HOBBYOS_ARCH)
    set(HOBBYOS_ARCH "arm")
endif ()

if (NOT HDYOS_SYSROOT)
    if (DEFINED HOBBYOS_SYSROOT AND NOT HOBBYOS_SYSROOT STREQUAL "")
        set(HDYOS_SYSROOT "${HOBBYOS_SYSROOT}")
    else ()
        set(HDYOS_SYSROOT "/home/sarah/Documents/GitHub/HobbyOS")
    endif ()
endif ()

# The WebKit fork hosting this toolchain (HobbyOS/../..) — hosts fork-side
# shim headers (HobbyOS/include-fenv, WK-1) and the drafted artifacts.
if (NOT WDK_FORK_DIR)
    set(WDK_FORK_DIR "/home/sarah/webkit-hobbyos")
endif ()

set(CMAKE_SYSTEM_NAME HobbyOS)

if (HOBBYOS_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(HOBBYOS_TRIPLE aarch64-none-elf)
    set(HOBBYOS_ARCH_CFLAGS "-mcpu=cortex-a53")
    set(HOBBYOS_OBJ_DIR "${HDYOS_SYSROOT}/obj/arm")
elseif (HOBBYOS_ARCH STREQUAL "intel")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
    set(HOBBYOS_TRIPLE x86_64-none-elf)
    set(HOBBYOS_ARCH_CFLAGS "-mno-red-zone -mcmodel=large")
    set(HOBBYOS_ARCH_LDFLAGS "-Wl,-no-pie")
    set(HOBBYOS_OBJ_DIR "${HDYOS_SYSROOT}/obj/intel")
else ()
    message(FATAL_ERROR "HOBBYOS_ARCH must be 'arm' or 'intel' (got '${HOBBYOS_ARCH}')")
endif ()

# clang is both the C and C++ driver on this workstation (project convention;
# the driver selects the frontend from the source extension).
set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang")
set(CMAKE_ASM_COMPILER "clang")

# HobbyOS target code cannot execute on the configure host: all CMake
# try-compile probes build static libraries only.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Take toolchain binaries/scripts from PATH (clang, ld.lld, ruby, perl...).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

# ---------------------------------------------------------------------------
# Compiler flags (mirrors the repo USER_CFLAGS per arch, plus the libc++
# include the C++ driver needs: with --target=<none-elf> clang has no C++
# stdlib of its own).
# ---------------------------------------------------------------------------
execute_process(
    COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE HOBBYOS_CLANG_RESOURCE
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(HOBBYOS_TARGET_FLAGS "--target=${HOBBYOS_TRIPLE} -ffreestanding -fuse-ld=lld")

set(HOBBYOS_SYSROOT_INCLUDES
    "-I${HDYOS_SYSROOT}/src/libc/include -I${HDYOS_SYSROOT}/src/include -I${HDYOS_SYSROOT}/src/user_include")

if (HOBBYOS_ARCH STREQUAL "intel")
    set(HOBBYOS_FREESTANDING_INCLUDE "-nostdinc -isystem ${HOBBYOS_CLANG_RESOURCE}/include")
else ()
    set(HOBBYOS_FREESTANDING_INCLUDE "")
endif ()

set(HOBBYOS_CXX_INCLUDE
    "-nostdinc++ -I${HDYOS_SYSROOT}/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include")

# Fork-hosted sysroot-style shim headers (e.g. <fenv.h>, WK-1) — searched
# after libc++ (so libc++'s C-compat wrappers can #include_next onto them)
# and before the OS sysroot (so they win over any future sysroot additions).
set(HOBBYOS_FORK_INCLUDE "-I${WDK_FORK_DIR}/HobbyOS/include")

set(CMAKE_C_FLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} ${HOBBYOS_FREESTANDING_INCLUDE} ${HOBBYOS_FORK_INCLUDE} ${HOBBYOS_SYSROOT_INCLUDES} ${CMAKE_C_FLAGS}")
# libc++'s C-compat shims (<string.h>, <ctype.h>, <wchar.h>...) must resolve
# BEFORE the sysroot C headers (libcxx include dir first), or <cstring>
# aborts with "didn't find libc++'s <string.h> header".
set(CMAKE_CXX_FLAGS "${HOBBYOS_TARGET_FLAGS} ${HOBBYOS_ARCH_CFLAGS} ${HOBBYOS_FREESTANDING_INCLUDE} ${HOBBYOS_CXX_INCLUDE} ${HOBBYOS_FORK_INCLUDE} ${HOBBYOS_SYSROOT_INCLUDES} ${CMAKE_CXX_FLAGS}")

# ---------------------------------------------------------------------------
# Link model (AD-6: everything static).  CMAKE_CXX_STANDARD_LIBRARIES lands
# at the END of the link line (after the WebKit/ICU archives), so the
# sysroot closure is grouped there; CMAKE_EXE_LINKER_FLAGS carries the
# script/entry/nostdlib switches.  -Wl,--start-group is required because
# libc.a <-> libcxx.a members have mutual references (P3 closure).
# ---------------------------------------------------------------------------
set(HOBBYOS_LINKER_SCRIPT "${HDYOS_SYSROOT}/src/user/linker.ld")

set(HOBBYOS_SYSROOT_CLOSURE
    "-Wl,--start-group"
    "${HOBBYOS_OBJ_DIR}/crt0.o"
    "${HOBBYOS_OBJ_DIR}/libcxx.a"
    "${HOBBYOS_OBJ_DIR}/libc.a"
    "-Wl,--end-group")

# A CMake LIST must never reach LINK_LIBRARIES as-is: the Ninja generator
# writes the `;` separators literally into the link line and the shell then
# mis-splits every argument (observed: crt0.o "Exec format error" and
# libc.a read as a shell script). Join to a plain space-separated string.
list(JOIN HOBBYOS_SYSROOT_CLOSURE " " HOBBYOS_SYSROOT_CLOSURE_STR)

# WK-1 weak-`main` bridge + libc gap-filler (HobbyOS/lib/*.c): -ffreestanding
# mangles C++ `main` (clang drops the C-linkage special case; crt0.c needs
# plain `main`), and the sysroot libc lacks exp/lrint/__*ti3 (weak fallbacks
# here, superseded when the libc lane lands strong versions). Compile once per
# build dir and glue into the closure. Requires clang on PATH.
set(HOBBYOS_FORK_LIB_C
    "${WDK_FORK_DIR}/HobbyOS/lib/mainlink_shim.c"
    "${WDK_FORK_DIR}/HobbyOS/lib/gaps.c")
set(HOBBYOS_FORK_LIB_OBJS "")
foreach (HOBBYOS_FORK_LIB_SRC ${HOBBYOS_FORK_LIB_C})
    get_filename_component(_base "${HOBBYOS_FORK_LIB_SRC}" NAME_WE)
    set(_o "${CMAKE_BINARY_DIR}/hobbyos_${_base}.o")
    if (NOT EXISTS "${_o}")
        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" --target=${HOBBYOS_TRIPLE} -ffreestanding
                    -mcpu=generic -c "${HOBBYOS_FORK_LIB_SRC}" -o "${_o}"
            RESULT_VARIABLE HOBBYOS_FORK_LIB_RC)
        if (NOT HOBBYOS_FORK_LIB_RC EQUAL 0)
            message(FATAL_ERROR "Failed to build HobbyOS fork lib object: ${HOBBYOS_FORK_LIB_SRC}")
        endif ()
    endif ()
    list(APPEND HOBBYOS_FORK_LIB_OBJS "${_o}")
endforeach ()
list(JOIN HOBBYOS_FORK_LIB_OBJS " " HOBBYOS_FORK_LIB_OBJS_STR)
set(CMAKE_CXX_STANDARD_LIBRARIES "${HOBBYOS_SYSROOT_CLOSURE_STR} ${HOBBYOS_FORK_LIB_OBJS_STR}"
    CACHE STRING "HobbyOS sysroot closure (crt0 + libcxx.a + libc.a + fork libs)" FORCE)

set(CMAKE_EXE_LINKER_FLAGS "${HOBBYOS_ARCH_LDFLAGS} -nostdlib -nostartfiles -Wl,-T,${HOBBYOS_LINKER_SCRIPT} -Wl,-e,_start ${CMAKE_EXE_LINKER_FLAGS}")
set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS}")

set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

# CMake's dl flags are meaningless here (no dynamic linking).
set(CMAKE_DL_LIBS "")

# ---------------------------------------------------------------------------
# ICU: point the find_package(ICU 70.1 ...) probe at the OS repo's cross
# static archives.  ICU_INCLUDE_DIR needs a merged unicode/ view because the
# common (utypes.h, ucal.h, ubrk.h...) and i18n (ucol.h, unum.h, udat.h...)
# headers live in separate trees with zero basename collisions.
# Overridable per build dir via -DICU_* if a different layout is wanted.
# ---------------------------------------------------------------------------
set(HOBBYOS_ICU_SRC "${HDYOS_SYSROOT}/third_party/icu-78.3/src/source")
set(HOBBYOS_ICU_INC "${HDYOS_SYSROOT}/obj/icu-include")

file(MAKE_DIRECTORY "${HOBBYOS_ICU_INC}/unicode")
file(GLOB HOBBYOS_ICU_COMMON_HDRS "${HOBBYOS_ICU_SRC}/common/unicode/*.h")
file(GLOB HOBBYOS_ICU_I18N_HDRS "${HOBBYOS_ICU_SRC}/i18n/unicode/*.h")
foreach (_hdr ${HOBBYOS_ICU_COMMON_HDRS} ${HOBBYOS_ICU_I18N_HDRS})
    get_filename_component(_hdr_name "${_hdr}" NAME)
    if (NOT EXISTS "${HOBBYOS_ICU_INC}/unicode/${_hdr_name}")
        file(CREATE_LINK "${_hdr}" "${HOBBYOS_ICU_INC}/unicode/${_hdr_name}")
    endif ()
endforeach ()

set(ICU_INCLUDE_DIR "${HOBBYOS_ICU_INC}" CACHE PATH "HobbyOS ICU merged include dir" FORCE)
# FindICU resolves each component through <COMPONENT>_LIBRARY_RELEASE and
# synthesizes the plain var via select_library_configurations(); set the
# _RELEASE forms so the search is bypassed entirely.
set(ICU_UC_LIBRARY_RELEASE "${HOBBYOS_OBJ_DIR}/icu/libicuuc.a" CACHE FILEPATH "HobbyOS ICU uc" FORCE)
set(ICU_I18N_LIBRARY_RELEASE "${HOBBYOS_OBJ_DIR}/icu/libicui18n.a" CACHE FILEPATH "HobbyOS ICU i18n" FORCE)
set(ICU_DATA_LIBRARY_RELEASE "${HOBBYOS_OBJ_DIR}/icu/libicudata.a" CACHE FILEPATH "HobbyOS ICU data" FORCE)
set(ICU_VERSION "78.3" CACHE STRING "HobbyOS ICU version" FORCE)

# ---------------------------------------------------------------------------
# Options for the Find module: never let CMake search the host for ICU.
# ---------------------------------------------------------------------------
set(ICU_ROOT "${HDYOS_SYSROOT}/obj/icu-root" CACHE PATH "unused; keeps FindICU quiet" FORCE)
