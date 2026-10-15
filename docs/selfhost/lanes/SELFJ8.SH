# SELFJ8.SH — w12-rcut: the parallel -j8 variant of the in-OS build-stage
# wrapper.  Invoked by the SELFHOST.BIN driver (stage tool SH.BIN) as
# `sh /SRC/SELFJ8.SH <target>`; runs the frozen acceptance target under the
# make port's jobserver (-j8, w11-makej; acceptance.md 9.6 caps at -j8 on
# the 8-vCPU guest).  Stability: -j8 verified on the synthetic 16-TU
# project (byte-identical outputs, safe fail propagation); this is the
# first real use on the acceptance TUs.
cd /SRC
/TOOLS/MAKE.BIN -j8 -f Makefile.hobbyos "$1"
