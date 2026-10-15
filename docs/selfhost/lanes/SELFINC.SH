# SELFINC.SH — w12-rcut BONUS: in-OS INCREMENTAL proof after the -j8 S2
# kernel build (w10-fstime RTC timestamps; this re-cut demonstrates it on
# the REAL 59-TU kernel set):
#   1. `make -f Makefile.hobbyos /SRC/obj/arm/dhcp.o` — an up-to-date real
#      object target -> make prints "Nothing to be done for
#      '/SRC/obj/arm/dhcp.o'." (serial evidence).
#   2. `make -f Makefile.hobbyos kernel` again — the .PHONY aggregate
#      re-links, but every one of the 59 object rules sees an up-to-date
#      target, so ZERO `phase=kernel-compile` lines appear between the
#      s2-kernel-start and kernel-link-start markers (same for -j's
#      absence here: serial invocation, no -B).
# No echo/markers needed — the make output IS the evidence.
cd /SRC
/TOOLS/MAKE.BIN -f Makefile.hobbyos /SRC/obj/arm/dhcp.o
/TOOLS/MAKE.BIN -f Makefile.hobbyos kernel
