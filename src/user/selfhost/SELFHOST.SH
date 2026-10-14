# SELFHOST.SH — in-OS build-stage wrapper (lane w4-src, wave 4).
#
# Runs one `make -f Makefile.hobbyos <target>` stage with cwd=/SRC.  The
# SELFHOST.BIN driver (src/user/selfhost/selfhost.c) spawns stage tools
# from cwd "/" (all HobbyOS processes start with cwd=/), so the frozen
# acceptance command `make -f Makefile.hobbyos ...` — whose src/... paths
# are relative to the repo root — must be launched from /SRC.  This script
# (invoked as `sh /SRC/SELFHOST.SH <target>`) does that; sh script mode,
# the `cd` builtin and external execution are proven in-OS (sh.c).
#
# Positional $1 = the make target (check|user|kernel|disk), forwarded by
# SELFHOST.RUN.
cd /SRC
/TOOLS/MAKE.BIN -f Makefile.hobbyos "$1"
