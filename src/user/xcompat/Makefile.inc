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

ifeq ($(XCOMPAT_DSOS_ENABLED),1)
xcompat_dsos: $(XCOMPAT_LIBX11) $(XCOMPAT_LIBASOUND)
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

# ---- host test (Tier 1) ----------------------------------------------- *
# The ABI differential needs the host's own X11 headers.  They are optional:
# without <X11/Xlib.h> the reference TU is compiled out and the test prints
# SKIP for that group (the pure-core tests still run).
XCOMPAT_HAVE_X11 := $(shell echo '\#include <X11/Xlib.h>' | \
                      $(HOST_CC) -E - >/dev/null 2>&1 && echo 1 || echo 0)

XCOMPAT_TEST = xcompat_test_host
XCOMPAT_TEST_SRCS = src/host/xcompat_test.c src/host/xcompat_abi_mine.c \
                    src/user/xcompat/xcore.c
XCOMPAT_TEST_DEFS = -DXCOMPAT_HAVE_X11_REF=$(XCOMPAT_HAVE_X11) \
                    -DXCOMPAT_HAVE_REQUIRED=1 \
                    -Isrc/host -I$(XCOMPAT_DIR)
ifeq ($(XCOMPAT_HAVE_X11),1)
XCOMPAT_TEST_SRCS += src/host/xcompat_abi_ref.c
endif
ifeq ($(XCOMPAT_DSOS_ENABLED),1)
XCOMPAT_TEST_DEFS += -DXCOMPAT_DSO_PATH='"$(XCOMPAT_LIBX11)"' \
                     -DXCOMPAT_ALSA_PATH='"$(XCOMPAT_LIBASOUND)"'
XCOMPAT_TEST_DEPS  = $(XCOMPAT_LIBX11) $(XCOMPAT_LIBASOUND)
else
XCOMPAT_TEST_DEPS  =
endif

$(XCOMPAT_TEST): $(XCOMPAT_TEST_SRCS) src/host/xcompat_abi_probe.h \
                 src/host/xcompat_abi_row.h $(XCOMPAT_DIR)/xcore.h \
                 $(XCOMPAT_DIR)/xabi.h $(XCOMPAT_DIR)/required-libx11.h \
                 $(XCOMPAT_DIR)/required-alsa.h $(XCOMPAT_TEST_DEPS)
	$(HOST_CC) -Wall -Wextra -g $(XCOMPAT_TEST_DEFS) -o $@ $(XCOMPAT_TEST_SRCS) -ldl

xcompat_test_run: $(XCOMPAT_TEST)
	./$(XCOMPAT_TEST)
host_tests: xcompat_test_run
.PHONY: xcompat_test_run
