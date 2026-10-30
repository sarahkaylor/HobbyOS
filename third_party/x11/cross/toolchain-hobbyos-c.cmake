# CMake toolchain for C/C++ deps of the HobbyOS X11 stack.
#
#   . third_party/x11/cross/env.sh <arch> <prefix>     # generates the wrappers
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<repo>/third_party/x11/cross/toolchain-hobbyos-c.cmake \
#         -DHOBBYOS_ARCH=arm|intel -DHOBBYOS_X11_TOOLS=$HB_TOOLS -DHOBBYOS_X11_PREFIX=$HB_PREFIX ...
#
# HOBBYOS_X11_TOOLS / HOBBYOS_X11_PREFIX fall back to $ENV{HB_TOOLS} /
# $ENV{HB_PREFIX} (set by env.sh).  Compiler = env.sh's hobbyos-cc/-c++
# wrappers (same flags as the Makefile's USER_CFLAGS; link mode adds the
# flat-image closure), so try_compile/try_run-free probes behave.
# Modelled on ~/webkit-hobbyos/HobbyOS/toolchain-hobbyos.cmake.

if (NOT HOBBYOS_ARCH)
    set(HOBBYOS_ARCH "$ENV{HB_ARCH}")
endif ()
if (NOT HOBBYOS_X11_TOOLS)
    set(HOBBYOS_X11_TOOLS "$ENV{HB_TOOLS}")
endif ()
if (NOT HOBBYOS_X11_PREFIX)
    set(HOBBYOS_X11_PREFIX "$ENV{HB_PREFIX}")
endif ()
if (NOT HOBBYOS_X11_TOOLS OR NOT EXISTS "${HOBBYOS_X11_TOOLS}/hobbyos-cc")
    message(FATAL_ERROR "HOBBYOS_X11_TOOLS must point at cross/env.sh's tools dir (got '${HOBBYOS_X11_TOOLS}')")
endif ()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES HOBBYOS_ARCH HOBBYOS_X11_TOOLS HOBBYOS_X11_PREFIX)

set(CMAKE_SYSTEM_NAME Generic)
if (HOBBYOS_ARCH STREQUAL "arm")
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
elseif (HOBBYOS_ARCH STREQUAL "intel")
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
else ()
    message(FATAL_ERROR "HOBBYOS_ARCH must be 'arm' or 'intel' (got '${HOBBYOS_ARCH}')")
endif ()

set(CMAKE_C_COMPILER "${HOBBYOS_X11_TOOLS}/hobbyos-cc")
set(CMAKE_CXX_COMPILER "${HOBBYOS_X11_TOOLS}/hobbyos-c++")
set(CMAKE_AR "llvm-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "llvm-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "llvm-nm" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "llvm-objcopy" CACHE FILEPATH "" FORCE)
find_program(_hb_llvm_ar NAMES llvm-ar PATHS /usr/lib/llvm-21/bin)
if (_hb_llvm_ar)
    set(CMAKE_AR "${_hb_llvm_ar}" CACHE FILEPATH "" FORCE)
    get_filename_component(_hb_llvm_dir "${_hb_llvm_ar}" DIRECTORY)
    set(CMAKE_RANLIB "${_hb_llvm_dir}/llvm-ranlib" CACHE FILEPATH "" FORCE)
    set(CMAKE_NM "${_hb_llvm_dir}/llvm-nm" CACHE FILEPATH "" FORCE)
    set(CMAKE_OBJCOPY "${_hb_llvm_dir}/llvm-objcopy" CACHE FILEPATH "" FORCE)
endif ()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-fno-common -I${HOBBYOS_X11_PREFIX}/include")
set(CMAKE_CXX_FLAGS_INIT "-std=c++17 -I${HOBBYOS_X11_PREFIX}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${HOBBYOS_X11_PREFIX}/lib")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(CMAKE_DL_LIBS "")

set(CMAKE_FIND_ROOT_PATH "${HOBBYOS_X11_PREFIX}")
set(CMAKE_PREFIX_PATH "${HOBBYOS_X11_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(PKG_CONFIG_EXECUTABLE "${HOBBYOS_X11_TOOLS}/hobbyos-pkg-config" CACHE FILEPATH "" FORCE)
