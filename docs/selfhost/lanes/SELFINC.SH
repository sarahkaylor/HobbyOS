# SELFINC.SH — w12-rcut BONUS: in-OS INCREMENTAL proof after S2 (w10-fstime
# RTC timestamps; demonstrated on the REAL 59-TU kernel set in attempt 2):
#   1. `make -f Makefile.hobbyos /SRC/obj/arm/dhcp.o` — an up-to-date real
#      object target -> make prints "MAKE.BIN: '/SRC/obj/arm/dhcp.o' is up
#      to date." (single-target form of 'Nothing to be done').
#   2. `make -f Makefile.hobbyos kernel` again — the .PHONY aggregate
#      re-links, but every one of the 59 object rules sees an up-to-date
#      target, so ZERO `phase=kernel-compile` lines appear between the
#      s2-kernel-start and kernel-link-start markers (same for -j's
#      absence here: serial invocation, no -B).
# No echo/markers needed — the make output IS the evidence.
cd /SRC
/TOOLS/MAKE.BIN -f Makefile.hobbyos /SRC/obj/arm/dhcp.o
/TOOLS/MAKE.BIN -f Makefile.hobbyos kernel
