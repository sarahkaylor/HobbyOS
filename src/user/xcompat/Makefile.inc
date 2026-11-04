# src/user/xcompat/Makefile.inc - the compat DSO tree (lane L4).
#
# Wired into the top-level Makefile by the integrator with ONE line, placed
# after HOST_CC/HOST_CFLAGS are defined (Makefile:2965) and after the last
# `host_tests:` aggregation:
#
#     include src/user/xcompat/Makefile.inc
#
# Nothing else in the top-level file changes.
#
# Why these DSOs do not use USER_CC:
#
#   src/user/xcompat/* are **Linux-ABI shared objects**, not HobbyOS native
#   images.  The compat class runs a real glibc program whose ld.so
#   dlopen()s "libX11.so.6" / "libasound.so.2" from the game's search path,
#   so those files must be SysV ELF shared objects with the real sonames,
#   built -fPIC -shared, and they must not carry a DT_NEEDED on libc.so.6
#   (a DSO the loader maps before ld.so has finished must not drag glibc's
#   loader into its own symbol resolution).  So they are built with HOST_CC
#   (clang, x86-64 Linux), -nostdlib, and talk to the kernel through raw
#   syscalls.
#
#   The USER_CC path cannot produce them: USER_CFLAGS is
#   --target=x86_64-none-elf -ffreestanding -nostdinc (Makefile:279) and the
#   user link rules use crt0 + src/user/linker.ld + PT_INTERP /LDHOBBY.SO
#   (Makefile:319, :950), i.e. the *native* ABI class, which is exactly the
#   one a compat DSO must not use.  ARCH=aarch64 skips the DSOs entirely
#   (AD-8: compat is x86_64-only).
#
#   The host test, by contrast, is an ordinary host binary and joins
#   `make host_tests` like every other Tier-1 test.

# Standalone use (`make -f src/user/xcompat/Makefile.inc ...`) needs the host
# toolchain variables; inside the top-level Makefile these are already set
# (Makefile:2965) and ?= leaves them alone.
HOST_CC ?= clang

XCOMPAT_DIR   = src/user/xcompat
XCOMPAT_OBJ   = obj/xcompat
# -ffreestanding: this is not a hosted program, and it stops clang assuming
# it may call into libc for anything it does not inline (xrt.c supplies the
# byte primitives it emits anyway, so the DSO ends up with zero undefined
# symbols -- see the comment at the top of xrt.c).
XCOMPAT_CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -ffreestanding
XCOMPAT_DSOFLAGS = -shared -fPIC -fvisibility=hidden -nostdlib \
                   -fno-stack-protector -fno-asynchronous-unwind-tables \
                   -Wl,-z,now -Wl,--hash-style=gnu

XCOMPAT_LIBX11    = $(XCOMPAT_OBJ)/libX11.so.6
XCOMPAT_LIBASOUND = $(XCOMPAT_OBJ)/libasound.so.2
# libudev.so.1: SDL's joystick stage must be able to initialise with zero
# devices, or SDL_Init rolls the just-started video subsystem back (measured;
# docs/rimworld-launch-log.md §11).  Same rules as the rest: real soname, no
# DT_NEEDED, no libc.
XCOMPAT_LIBUDEV   = $(XCOMPAT_OBJ)/libudev.so.1
# PulseAudio is FMOD's *first* choice (libpulse-simple.so.0, which pulls
# libpulse.so.0 through DT_NEEDED on a real system).  Both sonames are built
# from the same source; see pulse_null.c for why the export set is a union.
XCOMPAT_LIBPULSE_SIMPLE = $(XCOMPAT_OBJ)/libpulse-simple.so.0
XCOMPAT_LIBPULSE        = $(XCOMPAT_OBJ)/libpulse.so.0

# The compat class is x86_64-only (docs/linux-abi.md §2, plan AD-8).
XCOMPAT_DSOS_ENABLED = $(if $(filter intel,$(ARCH)),1,0)

# ---- the DSOs ---------------------------------------------------------

$(XCOMPAT_LIBX11): $(XCOMPAT_DIR)/libx11.c $(XCOMPAT_DIR)/xcore.c \
                   $(XCOMPAT_DIR)/xrt.c $(XCOMPAT_DIR)/xabi.h \
                   $(XCOMPAT_DIR)/xcore.h
	@mkdir -p $(XCOMPAT_OBJ)
	$(HOST_CC) $(XCOMPAT_CFLAGS) $(XCOMPAT_DSOFLAGS) \
	  -Wl,-soname,libX11.so.6 -o $@ \
	  $(XCOMPAT_DIR)/libx11.c $(XCOMPAT_DIR)/xcore.c $(XCOMPAT_DIR)/xrt.c

$(XCOMPAT_LIBASOUND): $(XCOMPAT_DIR)/alsa_null.c
	@mkdir -p $(XCOMPAT_OBJ)
	$(HOST_CC) $(XCOMPAT_CFLAGS) $(XCOMPAT_DSOFLAGS) \
	  -Wl,-soname,libasound.so.2 -o $@ $(XCOMPAT_DIR)/alsa_null.c \
	  $(XCOMPAT_DIR)/xrt.c

# One source, two sonames.  libpulse-simple.so.0 is what FMOD/SDL dlopen;
# libpulse.so.0 is what a real libpulse-simple would have pulled in.  Neither
# carries a DT_NEEDED edge to the other (the compat loader maps these DSOs
# before ld.so finishes), so both export the same union -- see the comment at
# the top of pulse_null.c.
$(XCOMPAT_LIBPULSE_SIMPLE): $(XCOMPAT_DIR)/pulse_null.c \
                            $(XCOMPAT_DIR)/pulse_abi.h $(XCOMPAT_DIR)/xrt.c
	@mkdir -p $(XCOMPAT_OBJ)
	$(HOST_CC) $(XCOMPAT_CFLAGS) $(XCOMPAT_DSOFLAGS) \
	  -Wl,-soname,libpulse-simple.so.0 -o $@ \
	  $(XCOMPAT_DIR)/pulse_null.c $(XCOMPAT_DIR)/xrt.c

$(XCOMPAT_LIBPULSE): $(XCOMPAT_DIR)/pulse_null.c $(XCOMPAT_DIR)/pulse_abi.h \
                     $(XCOMPAT_DIR)/xrt.c
	@mkdir -p $(XCOMPAT_OBJ)
	$(HOST_CC) $(XCOMPAT_CFLAGS) $(XCOMPAT_DSOFLAGS) \
	  -Wl,-soname,libpulse.so.0 -o $@ \
	  $(XCOMPAT_DIR)/pulse_null.c $(XCOMPAT_DIR)/xrt.c

$(XCOMPAT_LIBUDEV): $(XCOMPAT_DIR)/udev_null.c $(XCOMPAT_DIR)/udev_abi.h
	@mkdir -p $(XCOMPAT_OBJ)
	$(HOST_CC) $(XCOMPAT_CFLAGS) $(XCOMPAT_DSOFLAGS) \
	  -Wl,-soname,libudev.so.1 -o $@ $(XCOMPAT_DIR)/udev_null.c

ifeq ($(XCOMPAT_DSOS_ENABLED),1)
xcompat_dsos: $(XCOMPAT_LIBX11) $(XCOMPAT_LIBASOUND) $(XCOMPAT_LIBPULSE_SIMPLE) \
              $(XCOMPAT_LIBPULSE) $(XCOMPAT_LIBUDEV)
else
xcompat_dsos:
	@echo "xcompat: DSOs are x86_64-only (ARCH=$(ARCH)); skipped"
endif
.PHONY: xcompat_dsos

# ---- the F4 export gate ----------------------------------------------- *
# Every symbol Unity's SDL dlsyms from libX11.so.6 must be exported by the
# DSO.  required-libx11.txt is generated from the game binary (tools/
# gen_exports.py) and checked in, so this runs without the game present.
#
# A missing symbol is a hard failure: SDL's X11 driver resolves the whole
# block at once and silently falls back to another video driver if any name
# is absent, which would look like "no window" much later and much more
# confusingly.
XCOMPAT_EXPORTS_CHECK = $(XCOMPAT_OBJ)/.exports-ok

$(XCOMPAT_EXPORTS_CHECK): $(XCOMPAT_LIBX11) $(XCOMPAT_DIR)/required-libx11.txt
	@mkdir -p $(XCOMPAT_OBJ)
	@test -f $(XCOMPAT_LIBX11) || { 	  echo "xcompat: $(XCOMPAT_LIBX11) is missing (build xcompat_dsos)"; 	  exit 1; }
	@nm -D --defined-only $(XCOMPAT_LIBX11) | \
	  awk '$$2 ~ /^[TWi]$$/ {print $$3}' | sed 's/@.*//' | LC_ALL=C sort -u \
	  > $(XCOMPAT_OBJ)/libX11.exports
	@miss=$$(LC_ALL=C comm -23 $(XCOMPAT_DIR)/required-libx11.txt \
	          $(XCOMPAT_OBJ)/libX11.exports); \
	if [ -n "$$miss" ]; then \
	  echo "xcompat: libX11.so.6 is missing required symbols:"; \
	  echo "$$miss" | sed 's/^/  /'; exit 1; fi
	@echo "xcompat: libX11.so.6 exports all $$(wc -l < $(XCOMPAT_DIR)/required-libx11.txt) required symbols"
	@touch $@

xcompat_exports_check: $(XCOMPAT_EXPORTS_CHECK)
.PHONY: xcompat_exports_check

# ---- the PulseAudio export gate --------------------------------------- *
# Same rule as libX11: the set is measured from the game binary
# (required-pulse.txt, 21 names) and checked in, so this runs with no game
# present.  Both sonames must carry the full union: FMOD dlopens
# libpulse-simple.so.0 and resolves the async names through it, which on a
# real system happens via that DSO's DT_NEEDED on libpulse.so.0 -- an edge
# these shims deliberately do not have.
#
# The same rule also asserts the no-DT_NEEDED contract on both pulse DSOs: the
# loader maps them before ld.so has finished, so a dependency edge would drag
# glibc's own resolution into a DSO that must stand alone.
XCOMPAT_PULSE_CHECK = $(XCOMPAT_OBJ)/.pulse-exports-ok

$(XCOMPAT_PULSE_CHECK): $(XCOMPAT_LIBPULSE_SIMPLE) $(XCOMPAT_LIBPULSE) \
                        $(XCOMPAT_DIR)/required-pulse.txt
	@mkdir -p $(XCOMPAT_OBJ)
	@for so in $(XCOMPAT_LIBPULSE_SIMPLE) $(XCOMPAT_LIBPULSE); do \
	  nm -D --defined-only $$so | \
	    awk '$$2 ~ /^[TWi]$$/ {print $$3}' | sed 's/@.*//' | LC_ALL=C sort -u \
	    > $$so.exports; \
	  miss=$$(LC_ALL=C comm -23 $(XCOMPAT_DIR)/required-pulse.txt $$so.exports); \
	  if [ -n "$$miss" ]; then \
	    echo "xcompat: $$so is missing required pulse symbols:"; \
	    echo "$$miss" | sed 's/^/  /'; exit 1; fi; \
	  if readelf -d $$so | grep -q '(NEEDED)'; then \
	    echo "xcompat: $$so has a DT_NEEDED edge; compat DSOs must be free of them"; \
	    exit 1; fi; \
	  echo "xcompat: $$(basename $$so) exports all $$(wc -l < $(XCOMPAT_DIR)/required-pulse.txt) required pulse symbols, 0 DT_NEEDED"; \
	done
	@touch $@

xcompat_pulse_check: $(XCOMPAT_PULSE_CHECK)
.PHONY: xcompat_pulse_check

# ---- the libudev export gate ------------------------------------------ *
# Same rule again, and one extra assertion that matters more here than
# anywhere else: the SONAME must be exactly libudev.so.1.  SDL dlopens that
# name and falls back to libudev.so.0; a DSO whose SONAME drifted would load
# by path but not by name, which looks like "the shim did nothing".
XCOMPAT_UDEV_CHECK = $(XCOMPAT_OBJ)/.udev-exports-ok

$(XCOMPAT_UDEV_CHECK): $(XCOMPAT_LIBUDEV) $(XCOMPAT_DIR)/required-udev.txt
	@mkdir -p $(XCOMPAT_OBJ)
	@nm -D --defined-only $(XCOMPAT_LIBUDEV) | \
	  awk '$$2 ~ /^[TWi]$$/ {print $$3}' | sed 's/@.*//' | LC_ALL=C sort -u \
	  > $(XCOMPAT_OBJ)/libudev.exports
	@miss=$$(LC_ALL=C comm -23 $(XCOMPAT_DIR)/required-udev.txt \
	          $(XCOMPAT_OBJ)/libudev.exports); \
	if [ -n "$$miss" ]; then \
	  echo "xcompat: libudev.so.1 is missing required symbols:"; \
	  echo "$$miss" | sed 's/^/  /'; exit 1; fi
	@if readelf -d $(XCOMPAT_LIBUDEV) | grep -q '(NEEDED)'; then \
	  echo "xcompat: libudev.so.1 has a DT_NEEDED edge; compat DSOs must be free of them"; \
	  exit 1; fi
	@son=$$(readelf -d $(XCOMPAT_LIBUDEV) | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p'); \
	if [ "$$son" != "libudev.so.1" ]; then \
	  echo "xcompat: libudev SONAME is '$$son', must be libudev.so.1"; exit 1; fi
	@echo "xcompat: libudev.so.1 exports all $$(wc -l < $(XCOMPAT_DIR)/required-udev.txt) required udev symbols, 0 DT_NEEDED, SONAME ok"
	@touch $@

xcompat_udev_check: $(XCOMPAT_UDEV_CHECK)
.PHONY: xcompat_udev_check

# ---- host test (Tier 1) ----------------------------------------------- *
# The ABI differential needs the host's own X11 headers.  They are optional:
# without <X11/Xlib.h> the reference TU is compiled out and the test prints
# SKIP for that group (the pure-core tests still run).
#
# The probe is `-include <header>`, not a `#include` piped into the shell: a
# '#' inside a make variable starts a comment, and the old '\#' spelling
# reached the compiler as a non-directive (so the test was always "yes").
XCOMPAT_HAVE_X11 := $(shell echo | $(HOST_CC) -E -include X11/Xlib.h - \
                      >/dev/null 2>&1 && echo 1 || echo 0)
# ...and the PulseAudio ABI differential needs <pulse/...> (libpulse-dev).
# Optional for the same reason: the shim carries its own pulse_abi.h, and the
# reference rows are only an extra proof that the two agree.
XCOMPAT_HAVE_PULSE := $(shell echo | $(HOST_CC) -E -include pulse/sample.h - \
                        >/dev/null 2>&1 && echo 1 || echo 0)
# ...and the libudev opaque-type check needs <libudev.h> (libudev-dev).  It is
# *only* a differential on the opaque tags and dev_t: libudev has no public
# structures to diff, so without the header the group prints SKIP and the
# shim's own static asserts still run.
XCOMPAT_HAVE_UDEV := $(shell echo | $(HOST_CC) -E -include libudev.h - \
                       >/dev/null 2>&1 && echo 1 || echo 0)

XCOMPAT_TEST = xcompat_test_host
XCOMPAT_TEST_SRCS = src/host/xcompat_test.c src/host/xcompat_abi_mine.c \
                    src/user/xcompat/xcore.c
XCOMPAT_TEST_DEFS = -DXCOMPAT_HAVE_X11_REF=$(XCOMPAT_HAVE_X11) \
                    -DXCOMPAT_HAVE_PULSE_REF=$(XCOMPAT_HAVE_PULSE) \
                    -DXCOMPAT_HAVE_UDEV_REF=$(XCOMPAT_HAVE_UDEV) \
                    -DXCOMPAT_HAVE_REQUIRED=1 \
                    -Isrc/host -I$(XCOMPAT_DIR)
ifeq ($(XCOMPAT_HAVE_X11),1)
XCOMPAT_TEST_SRCS += src/host/xcompat_abi_ref.c
endif
ifeq ($(XCOMPAT_HAVE_PULSE),1)
XCOMPAT_TEST_SRCS += src/host/pulse_abi_ref.c
endif
ifeq ($(XCOMPAT_DSOS_ENABLED),1)
XCOMPAT_TEST_DEFS += -DXCOMPAT_DSO_PATH='"$(XCOMPAT_LIBX11)"' \
                     -DXCOMPAT_ALSA_PATH='"$(XCOMPAT_LIBASOUND)"' \
                     -DXCOMPAT_PULSE_SIMPLE_PATH='"$(XCOMPAT_LIBPULSE_SIMPLE)"' \
                     -DXCOMPAT_PULSE_PATH='"$(XCOMPAT_LIBPULSE)"' \
                     -DXCOMPAT_UDEV_PATH='"$(XCOMPAT_LIBUDEV)"'
XCOMPAT_TEST_DEPS  = $(XCOMPAT_LIBX11) $(XCOMPAT_LIBASOUND) \
                     $(XCOMPAT_LIBPULSE_SIMPLE) $(XCOMPAT_LIBPULSE) \
                     $(XCOMPAT_LIBUDEV)
else
XCOMPAT_TEST_DEPS  =
endif

$(XCOMPAT_TEST): $(XCOMPAT_TEST_SRCS) src/host/xcompat_abi_probe.h \
                 src/host/xcompat_abi_row.h $(XCOMPAT_DIR)/xcore.h \
                 $(XCOMPAT_DIR)/xabi.h $(XCOMPAT_DIR)/required-libx11.h \
                 $(XCOMPAT_DIR)/required-alsa.h $(XCOMPAT_DIR)/required-pulse.h \
                 $(XCOMPAT_DIR)/required-udev.h $(XCOMPAT_DIR)/udev_abi.h \
                 $(XCOMPAT_DIR)/pulse_abi.h $(XCOMPAT_TEST_DEPS)
	$(HOST_CC) -Wall -Wextra -g $(XCOMPAT_TEST_DEFS) -o $@ $(XCOMPAT_TEST_SRCS) -ldl

xcompat_test_run: $(XCOMPAT_TEST)
	./$(XCOMPAT_TEST)
host_tests: xcompat_test_run
.PHONY: xcompat_test_run
