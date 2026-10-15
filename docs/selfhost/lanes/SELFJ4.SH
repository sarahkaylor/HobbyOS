# SELFJ4.SH — w12-rcut: parallel -j4 variant of the in-OS build-stage wrapper
# (fallback after the -j8 S2 attempt tore a dirent under 8-way concurrent
# rename-while-open; mission fallback ladder: -j8 -> -j4 -> serial).
# Invoked as `sh /SRC/SELFJ4.SH <target>`; runs the frozen acceptance target
# under make's jobserver capped at -j4 (w11-makej; acceptance.md 9.6).
cd /SRC
/TOOLS/MAKE.BIN -j4 -f Makefile.hobbyos "$1"
