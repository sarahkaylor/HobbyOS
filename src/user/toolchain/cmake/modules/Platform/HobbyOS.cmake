# Platform/HobbyOS.cmake — CMake platform module for the HobbyOS userland
# target (selfhost.md AD-1; lane L1).
#
# CMake ships no Platform/HobbyOS module, so an unknown CMAKE_SYSTEM_NAME
# falls to Platform/Generic.cmake and the `UNIX` variable is never set
# (CMake >= 3.27 sets UNIX only in the Platform/<os>-Initialize modules).
# LLVM's HandleLLVMOptions.cmake:244 branches on `FUCHSIA OR UNIX` to choose
# the POSIX support shim (llvm/lib/Support/Unix/*.inc) — exactly what the
# HobbyOS userland needs (AD-1: plain POSIX userland binaries).  Without this
# module LLVM hard-stops: "Unable to determine platform".
#
# This module is loaded via CMAKE_MODULE_PATH from the toolchain file;
# the toolchain file itself sets CMAKE_SYSTEM_NAME=HobbyOS.
set(UNIX 1)
