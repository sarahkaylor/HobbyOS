# build_kernel.sh — build the HobbyOS kernel FROM THE SOURCE TAR on this disk.
#
# Run inside HobbyOS (from the desktop shell or any console):
#   SH.BIN /BUILD_KERNEL.SH [mode]
#
# Modes:
#   (none) / all — if /SRC has no source tree yet, extract /SRC.TAR into it,
#                  then build check+user+kernel+disk from it with
#                  /TOOLS/MAKE.BIN -f Makefile.hobbyos, then install the
#                  freshly built kernel as the boot kernel
#   untar        — force re-extract /SRC.TAR into /SRC (refresh the tree)
#   check        — one-TU compile smoke (src/kernel/main.c)
#   user         — the acceptance user set (HELLO SH LS CAT)
#   kernel       — all kernel TUs -> /SRC/hobbyos.elf -> /SRC/hobbyos.bin
#   disk         — assemble the bootable image /SRC/OUT.IMG (kernel +
#                  limine + BOOTAA64.EFI + user set)
#   install      — copy /SRC/hobbyos.bin to /boot/hobbyos.bin (the kernel
#                  limine boots); the previous kernel is kept at
#                  /boot/hobbyos.old, and the limine menu's "previous
#                  kernel" entry boots it
#
# The build itself is the in-OS make driver Makefile.hobbyos (staged into
# /SRC by tools/selfhost-src-tar.sh); this script only drives it.  The
# heavy lifting is all in-OS: /TOOLS/CLANG.BIN compiles, /TOOLS/LLD.BIN
# links, /TOOLS/OBJCOPY.BIN flattens, /TOOLS/MKFSFAT.BIN + /TOOLS/MCOPY.BIN
# assemble OUT.IMG.  Expect a full fresh build to take hours under TCG
# (sh.c alone is ~1 h); once /SRC/obj exists, make re-runs are incremental
# (FAT timestamps) and a re-prove is minutes.
#
# Every command path is absolute, so the script works from any cwd.

echo "[BUILD] build-kernel start"
mode="$1"
if [ "x$mode" = "x" ]; then
  mode="all"
fi
echo "[BUILD] mode=$mode"

if [ "$mode" = "untar" ]; then
  echo "[BUILD] extract /SRC.TAR into /SRC"
  /TOOLS/TAR.BIN xf /SRC.TAR /SRC
  rc="$?"
  if [ "x$rc" = "x0" ]; then
    echo "[BUILD] untar done"
  else
    echo "[BUILD] untar failed rc=$rc"
  fi
  exit "$rc"
fi

if [ "$mode" = "install" ]; then
  if [ -f /SRC/hobbyos.bin ]; then
    echo "[BUILD] backup /boot/hobbyos.bin -> /boot/hobbyos.old"
    /CP.BIN /boot/hobbyos.bin /boot/hobbyos.old
    echo "[BUILD] install /SRC/hobbyos.bin -> /boot/hobbyos.bin"
    /CP.BIN /SRC/hobbyos.bin /boot/hobbyos.bin
    rc="$?"
    echo "[BUILD] install rc=$rc"
    exit "$rc"
  else
    echo "[BUILD] install failed: /SRC/hobbyos.bin not found (build first)"
    exit 1
  fi
fi

# build modes: make sure a source tree exists, then drive the make driver
if [ -f /SRC/src/kernel/main.c ]; then
  echo "[BUILD] /SRC source tree present"
else
  echo "[BUILD] extract /SRC.TAR into /SRC"
  /TOOLS/TAR.BIN xf /SRC.TAR /SRC
  rc="$?"
  if [ "x$rc" = "x0" ]; then
    echo "[BUILD] extract ok"
  else
    echo "[BUILD] extract failed rc=$rc"
    exit "$rc"
  fi
fi

cd /SRC
echo "[BUILD] make -f Makefile.hobbyos $mode"
/TOOLS/MAKE.BIN -f Makefile.hobbyos "$mode"
rc="$?"
echo "[BUILD] make rc=$rc"

if [ "x$rc" = "x0" ]; then
  if [ "$mode" = "all" ]; then
    echo "[BUILD] backup /boot/hobbyos.bin -> /boot/hobbyos.old"
    /CP.BIN /boot/hobbyos.bin /boot/hobbyos.old
    echo "[BUILD] install /SRC/hobbyos.bin -> /boot/hobbyos.bin"
    /CP.BIN /SRC/hobbyos.bin /boot/hobbyos.bin
    rc="$?"
    echo "[BUILD] install rc=$rc"
  fi
  echo "[BUILD] build-kernel done"
else
  echo "[BUILD] build failed (make rc=$rc) - kernel not installed"
fi
exit "$rc"
