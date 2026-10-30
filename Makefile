# Default architecture
ARCH ?= arm

# --- OS Detection ---
# Detect whether we are on macOS (Homebrew) or Linux (system packages)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  OS := macos
else
  OS := linux
endif

# --- Tool paths ---
# On macOS, tools are in /opt/homebrew; on Linux, use system PATH
ifeq ($(OS),macos)
  CC = /opt/homebrew/opt/llvm/bin/clang
  CXX = /opt/homebrew/opt/llvm/bin/clang
  LD = /opt/homebrew/bin/ld.lld
  OBJCOPY = /opt/homebrew/opt/llvm/bin/llvm-objcopy
  AR = /opt/homebrew/opt/llvm/bin/llvm-ar
  MMD = /opt/homebrew/bin/mmd
  MCOPY = /opt/homebrew/bin/mcopy
  MKFS_FAT = /opt/homebrew/sbin/mkfs.fat
  EDK2_X86_64 = /opt/homebrew/share/qemu/edk2-x86_64-code.fd
  EDK2_AARCH64 = /opt/homebrew/share/qemu/edk2-aarch64-code.fd
  QEMU_DISPLAY = cocoa
else
  CC = clang
  CXX = clang
  LD = ld.lld
  OBJCOPY = llvm-objcopy
  AR = ar
  MMD = mmd
  MCOPY = mcopy
  MKFS_FAT = mkfs.fat
  EDK2_X86_64 = $(HOME)/.local/share/OVMF/OVMF_CODE_4M.fd
  EDK2_AARCH64 = $(HOME)/.local/share/AAVMF/AAVMF_CODE.fd
  QEMU_DISPLAY = gtk
endif

# Mode selection (desktop or test)
MODE ?= desktop
# Default core count for `make run` (browser-capable desktop; 4-8 cores is
# the standing requirement — never ship -smp 1 as the default).
# The GUEST side caps at MAX_CPUS 8 (src/include/process.h); higher test
# throughput comes from running several VMs in parallel (this host has 64
# cores — see tools/run_parallel.sh), not from raising this value.
# VM RAM (user-directed from 2026-10-05): x64 6 -> 12 GiB (verified booting);
# ARM 8 -> 16 GiB (root-caused + fixed: the static identity map was extended
# 0..9 -> 0..18 GiB in src/kernel/arch/arm/mmu.c, L1[0..17]; the 12/16 GiB
# 'Booting AArch64 OS...' hang came from Limine loading the kernel image just
# below the TOP of RAM, i.e. ~12.98/16.98 GiB, outside the old <=9 GiB map;
# verified on-device 8/12/16 GiB boot.  The frame pool stays capped at 9 GiB).
QEMU_SMP ?= 8

# GX (docs/graphics-accel.md §3-D5): host graphics-acceleration selection.
#   auto (default): use the GL path — virtio-gpu-gl device + a GL-capable
#     display backend (`gtk,gl=on` on this workstation; host GL via
#     llvmpipe counts as "accelerated") — only when a real interactive
#     display is in play: MODE=desktop AND DISPLAY set AND QEMU_ARGS does
#     not force `-display none` AND QEMU_GPU != soft.  Everything else
#     resolves to soft, so every headless tier (test/unit_tests/desktop_test
#     modes, the run_*.py E2E overrides, CI) keeps its exact current
#     behavior: plain 2D device + `-display none`.
#   gl:   force the GL path.  NOTE: combining QEMU_GPU=gl with a
#     `-display none` override fails by design — QEMU rejects
#     virtio-gpu-gl-device when the display backend has no OpenGL support
#     enabled (docs/graphics-accel.md §1).
#   soft: force the plain 2D path (virtio-gpu-device + the plain display).
# Screendump caveat (§1): QMP screendump does not work on GL windows, so the
# headless tiers stay on the plain device forever.  `make gpu-check` probes
# this host and prints which mode `auto` will pick.
QEMU_GPU ?= auto
OBJ_DIR = obj/$(ARCH)
MODE_FILE = $(OBJ_DIR)/.mode

# Flat WebProcess binary (WK-5 windowed browser shell).  The browser SOURCE is
# vendored in this repo since 2026-10-07 — third_party/webkit-hobbyos/
# (sha-verified snapshot parts + extract.sh + build.sh; see its README.md).
# Build from source with:   make browser
#   (or:  bash third_party/webkit-hobbyos/build.sh --arch intel --build-deps)
# Source order used by the browser-staged rule (a disk.img prerequisite, so
# `make run` always boots a complete OS): (1) BROWSER_BIN if present
# (explicit override or a local fork/worktree build; defaults below),
# (2) the vendored-source build output
# (third_party/webkit-hobbyos/build/<arch>/browser.bin), (3) the binary
# cache (third_party/webkit-hobbyos/cache/), (4) BUILD it from the vendored
# source — `make browser --build-deps` (cold = hours, warm = minutes; the
# result lands in $(BROWSER_BUILT) and is reused from then on).  Escape
# hatch: BROWSER_SKIP=1 restores the old best-effort behavior (ship
# without the browser).  Defaults point at known-good WINDOWED WebProcess
# builds per arch (the canonical ARM one is the pre-WK-4b renderer verified
# green by WN3; the intel one is WF1B's fresh merged-tree build — the
# canonical intel dir is stale/pre-windowed).  Override on the command
# line with:  make disk.img BROWSER_BIN=/path/to/WebProcess
ifeq ($(ARCH),arm)
BROWSER_BIN ?= $(HOME)/webkit-hobbyos/WebKitBuild/HobbyOS-arm-wk5/bin/WebProcess
else
BROWSER_BIN ?= $(HOME)/webkit-lanes/wf1b/WebKitBuild/HobbyOS-intel/bin/WebProcess
endif
# Vendored-source build output + optional binary cache (browser.md §11, 2026-10-07).
BROWSER_BUILT ?= third_party/webkit-hobbyos/build/$(ARCH)/browser.bin
# Cache files use the flat-image arch naming (browser-arm / browser-x64);
# $(ARCH) is arm|intel, so intel maps to the x64 cache name.
BROWSER_CACHE_ARCH := $(if $(filter intel,$(ARCH)),x64,$(ARCH))
BROWSER_CACHE ?= third_party/webkit-hobbyos/cache/browser-$(BROWSER_CACHE_ARCH).bin.xz
XZ ?= xz

# Build WebProcess from the vendored WebKit fork source (source-first path).
# BUILD_ARGS forwards flags:  make browser BUILD_ARGS='--arch intel --build-deps'
.PHONY: browser
browser:
	bash third_party/webkit-hobbyos/build.sh --arch $(ARCH) $(BUILD_ARGS)

# Build-if-missing command for browser-staged (runs only when NONE of the
# three sources exist).  --build-deps makes an otherwise-bare machine
# self-sufficient (cross-deps are built too; warm re-runs are minutes).
BROWSER_BUILD_CMD ?= $(MAKE) --no-print-directory browser BUILD_ARGS='--build-deps'
BROWSER_SKIP ?= 0

# ---------------------------------------------------------------------------
# `make run` completeness (2026-10-15): disk.img depends on browser-staged
# and tools-staged, so a boot ALWAYS carries a browser and the in-OS build
# toolchain (/TOOLS/{CLANG,LLD,LLVMAR,OBJCOPY}.BIN + RESDIR).  Cached copies
# are reused silently; genuinely-missing ones are BUILT once and cached.
# Escape hatches for special flows: BROWSER_SKIP=1 / TOOLS_SKIP=1 (skip;
# old warn/ship-without behavior), TOOLS_BUILD_CMD=<cmd> to override the
# LLVM dist-staging build (see docs/selfhost/llvm-bootstrap.md §6 for what
# the default script does and its cold-build cost).
# ---------------------------------------------------------------------------

# Resolve the WebProcess source chain into obj/$(ARCH)/browser.bin, building
# the browser first when there is nothing to resolve from.
.PHONY: browser-staged
browser-staged:
	@if [ ! -f "$(BROWSER_BIN)" ] && [ ! -f "$(BROWSER_BUILT)" ] && [ ! -f "$(BROWSER_CACHE)" ]; then \
	  if [ "$(BROWSER_SKIP)" = "1" ]; then \
	    echo "[browser] none of $(BROWSER_BIN) | $(BROWSER_BUILT) | $(BROWSER_CACHE) present; BROWSER_SKIP=1 - disk ships without the browser"; \
	  else \
	    echo "[browser] no cached WebProcess binary; building from the vendored source (cold = hours, warm = minutes; result cached in $(BROWSER_BUILT))..."; \
	    $(BROWSER_BUILD_CMD); \
	  fi; \
	fi; \
	if [ -f "$(BROWSER_BIN)" ]; then \
	  $(OBJCOPY) -O binary "$(BROWSER_BIN)" obj/$(ARCH)/browser.bin && echo "[browser] $(BROWSER_BIN) -> obj/$(ARCH)/browser.bin"; \
	elif [ -f "$(BROWSER_BUILT)" ]; then \
	  mkdir -p obj/$(ARCH) && cp "$(BROWSER_BUILT)" obj/$(ARCH)/browser.bin && echo "[browser] $(BROWSER_BUILT) (vendored-source build) -> obj/$(ARCH)/browser.bin"; \
	elif [ -f "$(BROWSER_CACHE)" ]; then \
	  mkdir -p obj/$(ARCH) && $(XZ) -dc "$(BROWSER_CACHE)" > obj/$(ARCH)/browser.bin && echo "[browser] $(BROWSER_CACHE) (cache) -> obj/$(ARCH)/browser.bin"; \
	elif [ "$(BROWSER_SKIP)" = "1" ]; then \
	  rm -f obj/$(ARCH)/browser.bin; \
	  echo "[browser] BROWSER_SKIP=1 - no browser staged"; \
	else \
	  echo "[browser] ERROR: still no WebProcess binary after the build - the disk would lack the browser"; \
	  exit 1; \
	fi

# Ensure the in-OS LLVM toolchain staging exists (obj/selfhost/dist-staging
# or the persistent third_party/llvm-21.1.8/dist-staging), building it when
# neither is present.  tools/selfhost-stage.sh --tools consumes it for the
# /TOOLS staging step in the disk.img recipe.
TOOLS_BUILD_CMD ?= bash tools/build-llvm-dist-staging.sh
TOOLS_SKIP ?= 0

.PHONY: tools-staged
tools-staged:
	@if [ ! -f obj/selfhost/dist-staging/usr/local/bin/clang-21 ] && \
	    [ ! -f third_party/llvm-21.1.8/dist-staging/usr/local/bin/clang-21 ]; then \
	  if [ "$(TOOLS_SKIP)" = "1" ]; then \
	    echo "[tools] no LLVM dist staging; TOOLS_SKIP=1 - /TOOLS will lack the LLVM toolchain"; \
	  else \
	    echo "[tools] no LLVM dist staging anywhere; building it (one-time; cold = hours; result cached in third_party/llvm-21.1.8/dist-staging; docs/selfhost/llvm-bootstrap.md section 6)..."; \
	    $(TOOLS_BUILD_CMD); \
	    if [ ! -f third_party/llvm-21.1.8/dist-staging/usr/local/bin/clang-21 ]; then \
	      echo "[tools] ERROR: build did not produce third_party/llvm-21.1.8/dist-staging"; \
	      exit 1; \
	    fi; \
	  fi; \
	else \
	  echo "[tools] LLVM dist staging present"; \
	fi

# The browser rule above is the first rule in the file; without an explicit
# default-goal it would make `make` (no args) run the WebKit build.  The
# documented default target is `all` (kernel ELF), as it was before the
# vendored-browser block moved above it (0cc0783).
.DEFAULT_GOAL := all

# Bundled browser font (DejaVu Sans).  The windowed HobbyOSFontManager text
# backend is DEFAULT ON since FS-R2 (retired /USE-FONT; an empty ::/NO-FONT
# marker opts out deterministically) and reads the face as /DEJAVU.TTF on the
# guest disk.  Extraction is rebuildable from the committed, sha-pinned zip
# (see third_party/fonts/README.md).
BROWSER_FONT := third_party/fonts/dejavu-2.37/ttf/DejaVuSans.ttf
$(BROWSER_FONT): third_party/fonts/dejavu-fonts-ttf-2.37.zip third_party/fonts/dejavu-2.37.sha256
	@cd third_party/fonts && tmp=$$(mktemp -d) \
	  && unzip -q dejavu-fonts-ttf-2.37.zip -d $$tmp \
	  && rm -rf dejavu-2.37 \
	  && mv $$tmp/dejavu-fonts-ttf-2.37 dejavu-2.37 \
	  && rmdir $$tmp \
	  && sha256sum -c dejavu-2.37.sha256

# Include path for the X11 support library headers (src/user/x11/include),
# added only to the rules that need it: the library and X11 apps.  Defined
# here so arm, intel and host builds all see it.
X11_INC = -Isrc/user/x11/include

# Determine if the mode has changed and update the tracker file
CURRENT_MODE := $(shell [ -f $(MODE_FILE) ] && cat $(MODE_FILE) || echo none)
ifneq ($(CURRENT_MODE),$(MODE))
$(shell mkdir -p $(OBJ_DIR) && echo $(MODE) > $(MODE_FILE))
endif

ifeq ($(ARCH),intel)
  QEMU = qemu-system-x86_64
  # Intel/AMD 64-bit compiler flags
  # Kernel stays off x87/SIMD: -mno-sse/-mno-mmx keep every kernel TU free of
  # FP/SSE codegen (the FPU state is moved only by arch/x64/fpu.c's inline
  # fxsave64/fxrstor64).  Userland, however, may use the FPU/SSE: the kernel
  # enables CR0.EM/MP/TS + CR4.OSFXSR/OSXMMEXCPT per core and carries each
  # process's 512-byte FXSAVE64 image across context switches (F1.5).
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx -fstack-protector-strong -mstack-protector-guard=global
  # Hermetic userland include path (P3).  clang's driver for x86_64-none-elf
  # appends the WORKSTATION's glibc dirs (/usr/include) because the target
  # arch matches the host's; the aarch64 target never gets aarch64 host
  # dirs, which is why only intel leaked.  Concretely: libc++'s
  # __mbstate_t.h tests __has_include(<bits/types/mbstate_t.h>), finds
  # glibc's on intel only, and its typedef collides with the sysroot's
  # ('typedef redefinition ... mbstate_t').  -nostdinc + an explicit
  # resource-dir -isystem reproduces the arm (hermetic) search path:
  # compiler freestanding headers reachable, host glibc not.
  # Keep in sync with third_party/libcxx-21.1.8/build-target.sh (STDINC).
  CLANG_RESOURCE_INCLUDE := $(shell $(CC) -print-resource-dir 2>/dev/null)/include
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -nostdinc -isystem $(CLANG_RESOURCE_INCLUDE)
  # P2.5 (S5 flip): user programs link at USER_IMG_BASE (64 GiB), which the
  # x86_64 small code model cannot address (clang emits 32-bit absolute
  # symbol references: R_X86_64_32S out of range at link).  The large model
  # emits movabs/64-bit forms for every symbol address and links cleanly at
  # any base.  aarch64 needs no equivalent (its codegen is PC-relative and
  # in-image).  Keep in sync with third_party/libcxx-21.1.8/build-target.sh
  # (TARGET_FLAGS intel branch).
  USER_CFLAGS += -mcmodel=large
  ARCH_DIR = src/kernel/arch/x64
  LDFLAGS = -T linker_x64.ld
  # QEMU parameters for x86_64: 8 cores, 6GB RAM, mounting disk.img as NVMe, booting with UEFI
  # GX (§3-D5 + lane G2, merged 9fce7ec): the accelerated path ADDS the
  # modern virtio GPU device, and only on the GL resolution (interactive
  # desktop — see the resolution block below).  Headless tiers keep their
  # exact historical std-VGA lines.  With the device up, `-vga none` makes
  # the virtio scanout console 0, so the GL window AND QMP screendump both
  # target the accelerated scanout (with VGA also present, screendump
  # captures the VGA console instead — lane-G2 finding).  GX_X64_GPU_DEV
  # is empty in every non-GL invocation.  If virtio-gpu-gl-pci ever
  # misbehaves on a host, plain `-device virtio-gpu-pci -vga none` is the
  # lane-G2-verified equivalent (the driver negotiates VIRTIO_F_VERSION_1
  # only, so the gl variant's virgl capability is simply not negotiated).
  QEMU_CMD = $(QEMU) -M q35 -smp $(QEMU_SMP) -m 12288M -pflash $(EDK2_X86_64) -display $(GX_GPU_DISPLAY) -serial stdio -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 -drive file=disk.img,format=raw,id=disk0,if=none -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 $(GX_X64_GPU_DEV) -action shutdown=poweroff $(QEMU_ARGS)
else
  # Default to ARM
  QEMU = qemu-system-aarch64
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -fstack-protector-strong -mstack-protector-guard=global
  # F1.5 (FPU): userland is free to use FP/SIMD — CPACR_EL1.FPEN=0b11 is
  # set per core in boot.s and the kernel saves/restores the FPSIMD
  # register file per process (src/kernel/arch/arm/fpu.s).  ONLY this user
  # flag set drops -mgeneral-regs-only; the kernel CFLAGS above keep it
  # (no compiler-emitted SIMD at EL1).
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53
  ARCH_DIR = src/kernel/arch/arm
  LDFLAGS = -T linker.ld
  # QEMU parameters for ARM: 8 cores, 16GB RAM, booting with UEFI (RAM x2,
  # see the VM RAM comment at the top).
  # GX (§3-D5): GX_GPU_DEV/GX_GPU_DISPLAY resolve after the MODE/QEMU_ARGS
  # block below — gl -> virtio-gpu-gl-device + `gtk,gl=on`, soft (default
  # for every headless tier) -> virtio-gpu-device + the plain display.
  QEMU_CMD = $(QEMU) -M virt -cpu cortex-a53 -smp $(QEMU_SMP) -m 16384M -accel tcg,thread=multi -bios $(EDK2_AARCH64) -display $(GX_GPU_DISPLAY) -serial stdio -drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 -device $(GX_GPU_DEV) -device virtio-keyboard-device -device virtio-tablet-device -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 -semihosting -action shutdown=poweroff $(QEMU_ARGS)
endif

# --- F2.4 (browser.md §6): userland C++ build policy ---------------------
# The minimal C++ runtime (src/libc/src/cxxrt.cpp) and C++ user programs
# build with the SAME per-arch flags as C (USER_CFLAGS) plus the language
# and the two policy switches the runtime is designed for:
#   -fno-exceptions / -fno-rtti  (no unwinder and no typeinfo runtime
#                                 exists; src/libc/include/cxxrt.h #errors
#                                 if either is missing)
#   -std=c++17                   (sized delete, static-init guards,
#                                 Itanium C++ ABI — the F2.4 baseline;
#                                 P3 revisits with libc++)
#   -nostdinc++                  (there is no C++ standard library yet:
#                                 includes must resolve to the sysroot)
# Compiled with $(CXX) (= clang, not clang++ — this workstation has no
# clang++ binary; the driver picks the C++ frontend from the .cpp
# extension).
CXX_USER_FLAGS = $(USER_CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -nostdinc++

# P3 (browser.md §6): the vendored libc++ + libc++abi.  Programs that use
# the standard library compile with the vendored headers (-nostdinc++ keeps
# every other C++ header out) and link $(OBJ_DIR)/libcxx.a ahead of libc.a.
LIBCXX_VENDOR = third_party/libcxx-21.1.8
LIBCXX_ROOT = $(LIBCXX_VENDOR)/src/llvm-project-21.1.8.src
LIBCXX_INCLUDE = $(LIBCXX_ROOT)/libcxx/include
# The vendored include dir must precede the sysroot: libc++'s ctype.h/
# wchar.h/... wrappers are found first and pull the C headers with
# #include_next (the upstream "C++ headers before C headers" rule).
CXX_LIBCXX_USER_FLAGS = -I$(LIBCXX_INCLUDE) $(USER_CFLAGS) -std=c++23 -fno-exceptions -fno-rtti -nostdinc++
# l3-rtti (browser.md §6/L6): RTTI ON for the RTTI acceptance program — the
# exact consumption mode the ICU cross build uses (-fno-exceptions -frtti).
# The archive-wide -fno-rtti policy is unchanged for every other program; the
# per-TU split lives in the vendor build (sources.txt class B).
CXX_RTTI_USER_FLAGS = -I$(LIBCXX_INCLUDE) $(USER_CFLAGS) -std=c++23 -fno-exceptions -frtti -nostdinc++

ifeq ($(MODE),test)
  CFLAGS += -DKERNEL_MODE_TEST
  # Userland sees the same fact: CONSOLE.BIN ships as the boot smoke in
  # test mode and as the interactive console in every other mode.
  USER_CFLAGS += -DKERNEL_MODE_TEST
else ifeq ($(MODE),unit_tests)
  CFLAGS += -DKERNEL_MODE_UNIT_TEST
else ifeq ($(MODE),desktop_test)
  CFLAGS += -DKERNEL_MODE_DESKTOP_TEST
  USER_CFLAGS += -DDESKTOP_TEST_AUTO_LAUNCH
else ifeq ($(MODE),apps_test)
  CFLAGS += -DKERNEL_MODE_APPS_TEST
else ifeq ($(MODE),pong_test)
  CFLAGS += -DKERNEL_MODE_PONG_TEST
  USER_CFLAGS += -DDESKTOP_TEST_AUTO_LAUNCH
else ifeq ($(MODE),filedialog_test)
  CFLAGS += -DKERNEL_MODE_FILEDIALOG_TEST
  USER_CFLAGS += -DDESKTOP_TEST_AUTO_LAUNCH
else ifeq ($(MODE),jsc)
  # WK-1 M3 (draft 0003): WebKit jsc smoke — single-program boot.  The fork
  # side assembles a minimal boot disk (kernel + JSC.BIN + SMOKE.JS only)
  # and boots it; see the fork's HobbyOS/continuation/README.md for the
  # runner (this tree stays self-contained — nothing here references the
  # fork's build outputs).
  CFLAGS += -DKERNEL_MODE_JSC
else ifeq ($(MODE),bigload)
  # L8 x64-loadfix lane: single-v2-load repro for the 30 MB image kernel
  # page fault under KVM.  Boots ONLY BIG.BIN (a synthetic ~30 MB image)
  # through the v2 loader before the scheduler; used for the KVM repro.
  CFLAGS += -DKERNEL_MODE_BIGLOAD
else ifeq ($(MODE),webproc)
  # WK-3 (draft 0003): headless WebProcess single-program boot.  The fork
  # side assembles the boot disk (kernel + WEBPROC.BIN + FIX01.HTM + probe
  # files) and boots it; see the fork's HobbyOS/continuation/README.md
  # WK-3 section (this tree stays self-contained -- nothing here
  # references the fork's build outputs).
  CFLAGS += -DKERNEL_MODE_WEBPROC
else ifeq ($(MODE),selfhost)
  # Self-hosting (docs/selfhost/acceptance.md §5.2/5.3): single-program
  # boot that launches the in-OS acceptance build driver SELFHOST.BIN
  # WITHOUT the desktop.  fd 1/2 are unset -> the console-fd fallback
  # (fs.c file_write 0002 -> console_write_user) relays the driver's
  # stdout/stderr to the SERIAL console, so every [SELFHOST] phase=
  # marker lands in the serial capture with zero keystroke fragility.
  # The driver script path is passed as argv[1] (main.c KERNEL_MODE_SELFHOST
  # branch); the driver's own default is /SRC/SELFHOST.RUN.
  CFLAGS += -DKERNEL_MODE_SELFHOST
else ifeq ($(MODE),soak)
  # P8.2 (browser.md §6): the soak — the standard test wave plus TORTURE.BIN
  # running its bounded soak loop (28-minute cap; main.c's KERNEL_SOAK_WAVE
  # branch passes the "soak" argument).  Bound the whole run externally:
  #   timeout 2100 make soak
  CFLAGS += -DKERNEL_MODE_TEST -DKERNEL_SOAK_WAVE
  USER_CFLAGS += -DKERNEL_MODE_TEST
else
  CFLAGS += -DKERNEL_MODE_DESKTOP
endif

# W4-kernel (selfhost): in-OS MKFSFAT TCG-timing probe (Mission B).  Set
# W4_MKFS_PROBE=1 to build the unit tier with the [W4-PROBE] ftruncate
# timings (fat16_test.c test_fat16_extend_timing_probe) so a probe run
# reuses the normal unit harness; off by default so stock unit runs stay
# quick.  Keep the define OUT of USER_CFLAGS: it is kernel-test-only.
ifneq ($(W4_MKFS_PROBE),)
  CFLAGS += -DW4_MKFS_TIMING_PROBE
endif

ifneq ($(MODE),desktop)
  QEMU_ARGS += -display none
endif

# KVM acceleration is the default for ARCH=intel on hosts that have
# /dev/kvm (the swarm farm VMs expose nested SVM).  Without it a TCG intel
# wave is tens of minutes; with -enable-kvm it is minutes, and the ELF-compat
# track (intel-only by design, gaming.md §2.1) depends on fast intel cycles.
# The mode follows /dev/kvm's presence — an environmental fact, so the same
# host always runs the same mode (reproducibility holds), and a host can
# force the slow path for an A/B with SKIP_KVM=1.  macOS has no /dev/kvm, so
# this block is a no-op there; ARM is unaffected (no aarch64 KVM on x86 host).
# Tier wrappers (tools/t3_tier.sh, run_unit_tests_intel.sh) may already pass
# -enable-kvm in QEMU_ARGS; the findstring guard keeps it from duplicating.
ifeq ($(ARCH),intel)
  KVM_DEV := $(shell test -e /dev/kvm && echo 1 || echo 0)
  ifeq ($(KVM_DEV),1)
    ifneq ($(SKIP_KVM),1)
      ifeq ($(findstring -enable-kvm,$(QEMU_ARGS)),)
        QEMU_ARGS += -enable-kvm
      endif
    endif
  endif
endif

# --- GX (docs/graphics-accel.md §3-D5): resolve QEMU_GPU to the concrete
# display/device pair referenced by the per-arch QEMU_CMD lines above.
# This block must run AFTER the `-display none` append: non-desktop modes
# (and any QEMU_ARGS=-display-none override) resolve to soft, keeping every
# headless/CI path on the plain 2D device + `none` exactly as before.
ifeq ($(QEMU_GPU),auto)
  ifeq ($(MODE),desktop)
    ifneq ($(DISPLAY),)
      ifeq ($(findstring -display none,$(QEMU_ARGS)),)
        GX_GPU_GL := 1
      endif
    endif
  endif
else ifeq ($(QEMU_GPU),gl)
  GX_GPU_GL := 1
endif

ifeq ($(GX_GPU_GL),1)
  # `$(QEMU_DISPLAY),gl=on` resolves to the §1-proven `gtk,gl=on` on Linux;
  # the macOS branch derives `cocoa,gl=on` (legacy path, untested — fall
  # back with QEMU_GPU=soft if that backend rejects it).
  GX_GPU_DISPLAY := $(QEMU_DISPLAY),gl=on
  GX_GPU_DEV := virtio-gpu-gl-device
else
  GX_GPU_DISPLAY := $(QEMU_DISPLAY)
  GX_GPU_DEV := virtio-gpu-device
endif

# x64 accelerated-path device (see the x64 QEMU_CMD comment): additive and
# GL-only, paired with `-vga none`.  Empty in every non-GL invocation, so
# test/unit/soak/desktop_test waves keep their exact historical qemu lines.
ifeq ($(GX_GPU_GL),1)
  GX_X64_GPU_DEV := -device virtio-gpu-gl-pci -vga none
else
  GX_X64_GPU_DEV :=
endif
# Per-arch note appended by `make gpu-check` so x64 invocations don't print
# the ARM device names.
GX_X64_NOTE_GL :=
GX_X64_NOTE_SOFT :=
ifeq ($(ARCH),intel)
  GX_X64_NOTE_GL := (x64: $(GX_X64_GPU_DEV))
  GX_X64_NOTE_SOFT := (x64: no GPU device -- std VGA)
endif

# Target and objects
TARGET = hobbyos.elf
SRC_DIR = src/kernel

# Find all .c and .s files in both src/kernel and the architecture directory
C_SRCS = $(wildcard $(SRC_DIR)/*.c) $(wildcard $(ARCH_DIR)/*.c)
ASM_SRCS = $(wildcard $(SRC_DIR)/*.s) $(wildcard $(ARCH_DIR)/*.s)

# Object files corresponding to the source files
C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(wildcard $(SRC_DIR)/*.c)) \
         $(patsubst $(ARCH_DIR)/%.c, $(OBJ_DIR)/%.o, $(wildcard $(ARCH_DIR)/*.c))
ASM_OBJS = $(patsubst $(SRC_DIR)/%.s, $(OBJ_DIR)/%.o, $(wildcard $(SRC_DIR)/*.s)) \
           $(patsubst $(ARCH_DIR)/%.s, $(OBJ_DIR)/%.o, $(wildcard $(ARCH_DIR)/*.s))
OBJS = $(ASM_OBJS) $(C_OBJS)

USER_LIBC = src/user/libc.c
# All user-visible headers (a change to any of these must rebuild user objects)
# src/include/errno.h + syscall.h feed every userland TU that touches errno
# or the syscall ABI (and every lane edits them), so they belong in the
# userland header dependency set; without them, a header ABI change links
# against stale objects.
USER_HDRS = src/user_include/*.h src/user_include/graphics/*.h src/libc/include/*.h src/include/syscall.h src/include/errno.h
MEM_TEST_BIN = $(OBJ_DIR)/memtest.bin
FILE_IO_BIN = $(OBJ_DIR)/fileio_test.bin
CONSOLE_BIN = $(OBJ_DIR)/console.bin
SPAWN_TEST_BIN = $(OBJ_DIR)/spawntest.bin
FORK_TEST_BIN = $(OBJ_DIR)/fork_test.bin
HEAP_TEST_BIN = $(OBJ_DIR)/heap_test.bin
GRAPHICS_TEST_BIN = $(OBJ_DIR)/graphics.bin
WSRF_T_BIN = $(OBJ_DIR)/wsurf_test.bin
SMP_TEST_BIN = $(OBJ_DIR)/smp_test.bin

PIPETEST_BIN = $(OBJ_DIR)/pipetest.bin
NETTEST_BIN = $(OBJ_DIR)/nettest.bin
# l8-netfix: on-device TX/console + RX-NUL repro and regression app
# (src/user/netfix_test.c).  Booted in MODE=webproc as WEBPROC.BIN by the
# lane harness; stays on disk.img for the wave as NETFIX.BIN (inert unless
# a loader entry selects it).
NETFIX_BIN = $(OBJ_DIR)/netfix.bin
TIMEOUT_BIN = $(OBJ_DIR)/timeout.bin
NFSTEST_BIN = $(OBJ_DIR)/nfstest.bin
DESKTOP_BIN = $(OBJ_DIR)/desktop.bin
EDITOR_BIN = $(OBJ_DIR)/editor.bin
EDITOR_T_BIN = $(OBJ_DIR)/EDITOR_T.BIN
APPS_T_BIN = $(OBJ_DIR)/APPS_T.BIN
PONG_T_BIN = $(OBJ_DIR)/PONG_T.BIN
STRESS_TEST_BIN = $(OBJ_DIR)/stress.bin
# F1.5 (browser.md): floating-point bring-up test.  FPU_T.BIN is 8.3-safe
# (the FAT16 reader only resolves 8-char base names, see the run-tests
# skill's pitfall #1).
FPU_T_BIN = $(OBJ_DIR)/fpu_test.bin
# GX (docs/graphics-accel.md): framebuffer present benchmark — full-flush
# vs damage-rect latency; the GFXBENCH lines are the GX lanes' raw
# before/after evidence.
GFXBENCH_BIN = $(OBJ_DIR)/gfxbench.bin
# P3.2 (browser.md): sysroot transcendental acceptance.  MATH_T.BIN links
# crt0 + libc.a like HELLO/THRD_T, so it proves the public sin/cos/.../
# expf symbols resolve from the archive on-device (the same symbols the
# ICU link probe wanted); 8.3-safe name.
MATH_T_BIN = $(OBJ_DIR)/math_test.bin
# P1 (docs/browser/p1-threads-design.md section 7): kernel threads +
# futex-lite + libpthread acceptance.  THRD_T and TLS_T link crt0 + libc.a
# (pthread.c is archived there) so they exercise the crt0 TLS bootstrap.
THRD_T_BIN = $(OBJ_DIR)/thrd_test.bin
TLS_T_BIN = $(OBJ_DIR)/tls_test.bin
# F2.2/F2.3: DNS resolver acceptance over the real network stack.
DNSTST_BIN = $(OBJ_DIR)/dns_test.bin
# F2.4 (browser.md): minimal C++ runtime acceptance.  CXXSMOKE.BIN is
# 8.3-safe (8-char base name) and built as C++ per CXX_USER_FLAGS above.
CXXSMOKE_BIN = $(OBJ_DIR)/cxx_smoke.bin
# P3 (browser.md §6): libc++ acceptance (8.3-safe).
CXX_T_BIN = $(OBJ_DIR)/cxx_t.bin
# l3-rtti (browser.md §6/L6): libc++abi RTTI acceptance (8.3-safe
# RTTI_T.BIN).  Compiled -frtti; links libcxx.a's class-B RTTI closure.
CXX_RTTI_T_BIN = $(OBJ_DIR)/rtti_t.bin
# P4 (docs/browser/p4-ipc-design.md section 8): AF_UNIX socketpair/poll/
# SCM_RIGHTS acceptance (8.3-safe IPC_T.BIN).
IPC_T_BIN = $(OBJ_DIR)/ipc_test.bin
# P2.2 (S2, docs/browser/p2-vm-design.md section 8): v2 (AS_V2) userland
# acceptance.  MMTEST.BIN is 8.3-safe, linked with linker_v2.ld at
# USER_IMG_BASE and loaded by loader v2 (no crt0, like MEMTEST).
MMTEST_T_BIN = $(OBJ_DIR)/mmtest.bin
# P8.1 (browser.md §6): "POSIX torture" acceptance — 64-thread
# mutex/condvar churn, socketpair SCM_RIGHTS fd-pass loops, mmap/fault/
# free storms, exec/wait cycles, poll on many fds, memory high-water.
# TORTURE.BIN is 8.3-safe; MODE=soak (P8.2) feeds it the soak argument.
TORTURE_T_BIN = $(OBJ_DIR)/torture_test.bin
# L8 x64-loadfix lane (repro): a synthetic ~30 MB flat image (30,167,040
# bytes = 7365 x 4 KiB pages, just over the M4 JSC.BIN size) generated at
# build time.  BIG.BIN never executes; the v2 loader must be able to read
# + map it on x64 KVM without a kernel page fault.
BIG_BIN = $(OBJ_DIR)/big.bin
$(BIG_BIN):
	mkdir -p $(dir $@)
	python3 -c "import sys; sys.stdout.buffer.write(b'HO_BIGLOAD_30MB\n' * 1885440)" > $@
	@test -s $@ && echo "built $(BIG_BIN): $$(wc -c < $@) bytes"
# l3-icu-wire (browser L6): ICU 78.3 target acceptance.  ICUSMK.BIN is
# 8.3-safe and links the target ICU archive set data-FREE (libicuuc.a only:
# u_strlen + UTF iteration + ASCII u_tolower + u_errorName + u_getVersion;
# see docs/browser/icu-usage-note.md).  The archive is produced on demand
# by third_party/icu-78.3/build-target.sh (self-healing fetch -> host tools
# -> target configure/make -> link probe -> MANIFEST).
ICU_SMOKE_BIN = $(OBJ_DIR)/icusmk.bin
ERRNO_TEST_BIN = $(OBJ_DIR)/errtest.bin
# Phase F1 (browser.md): socket/select acceptance + deterministic select
# checks + SYS_GETRANDOM entropy (see main.c's KERNEL_MODE_TEST wave).
SOCK2TST_BIN = $(OBJ_DIR)/SOCK2TST.BIN
POLLTST_BIN = $(OBJ_DIR)/POLLTST.BIN
RANDTST_BIN = $(OBJ_DIR)/RANDTST.BIN
# blk-cache (docs/blk-cache.md): in-OS integration/benchmark test for the
# block-level read cache — integrity, hit accounting (sysinfo 9),
# concurrent readers+writer, warm/cold timing, memory pressure.
BLKTEST_BIN = $(OBJ_DIR)/BLKTEST.BIN
HELLO_BIN = $(OBJ_DIR)/hello.bin

SH_BIN = $(OBJ_DIR)/sh.bin
LS_BIN = $(OBJ_DIR)/ls.bin
CAT_BIN = $(OBJ_DIR)/cat.bin
GREP_BIN = $(OBJ_DIR)/grep.bin
# GNU grep 2.5.4 port: four core sources + the vendored lib subset they pull
# in (src/user/grep/config.h is the port's generated config header).
GREP_CORE_NAMES = grep search dfa kwset
GREP_GNULIB_NAMES = alloca closeout error exclude fnmatch hard-locale isdir obstack quotearg regex savedir stpcpy xmalloc xstrtol xstrtoumax
GREP_OBJS = $(addprefix $(OBJ_DIR)/grep_,$(addsuffix .o,$(GREP_CORE_NAMES))) $(addprefix $(OBJ_DIR)/grepg_,$(addsuffix .o,$(GREP_GNULIB_NAMES)))
LESS_BIN = $(OBJ_DIR)/less.bin
TAIL_BIN = $(OBJ_DIR)/tail.bin
HEAD_BIN = $(OBJ_DIR)/head.bin
SHELL_TEST_BIN = $(OBJ_DIR)/shtest.bin

PS_BIN = $(OBJ_DIR)/ps.bin
FREE_BIN = $(OBJ_DIR)/free.bin
UPTIME_BIN = $(OBJ_DIR)/uptime.bin
KILL_BIN = $(OBJ_DIR)/kill.bin
BASENAME_BIN = $(OBJ_DIR)/basename.bin
DIRNAME_BIN = $(OBJ_DIR)/dirname.bin
SEQ_BIN = $(OBJ_DIR)/seq.bin
EXPR_BIN = $(OBJ_DIR)/expr.bin
TESTGNU_BIN = $(OBJ_DIR)/testgnu.bin
CP_BIN = $(OBJ_DIR)/cp.bin
RM_BIN = $(OBJ_DIR)/rm.bin
# [inos-build-kernel] in-OS ustar extractor: /BUILD_KERNEL.SH unpacks the
# ::/SRC.TAR source payload with /TOOLS/TAR.BIN (src/user/tar.c).
TAR_BIN = $(OBJ_DIR)/tar.bin
MV_BIN = $(OBJ_DIR)/mv.bin
TOUCH_BIN = $(OBJ_DIR)/touch.bin
WC_BIN = $(OBJ_DIR)/wc.bin
SED_BIN = $(OBJ_DIR)/sed.bin
# GNU sed 4.8 port: seven core sources + the vendored gnulib subset they
# pull in (src/user/sed/config.h is the port's generated config header).
SED_CORE_NAMES = sed compile debug execute mbcs regexp utils
SED_GNULIB_NAMES = acl-errno-valid acl-internal basename-lgpl c-ctype c-strcasecmp c-strncasecmp close-stream closeout copy-acl dfa dirname-lgpl exitfail get-permissions localcharset localeinfo obstack progname qcopy-acl qset-acl quotearg set-acl set-permissions strverscmp version-etc version-etc-fsf xalloc-die xmalloc
SED_OBJS = $(addprefix $(OBJ_DIR)/sed_,$(addsuffix .o,$(SED_CORE_NAMES))) $(addprefix $(OBJ_DIR)/sedg_,$(addsuffix .o,$(SED_GNULIB_NAMES)))
# GNU nano 8.7 port: the editor's own sources (byte-identical to upstream
# except two HobbyOS-marked touch points) plus the curses shim that renders
# it into a desktop terminal window.  src/user/nano/curses.h documents the
# wire protocol; src/user/nano/config.h pins the port's feature set.
NANO_SRC_NAMES = browser chars color cut files global help history move nano prompt rcfile search text utils winio
NANO_OBJS = $(addprefix $(OBJ_DIR)/nano_,$(addsuffix .o,$(NANO_SRC_NAMES))) $(OBJ_DIR)/nano_hb_curses.o
NANO_BIN = $(OBJ_DIR)/nano.bin
XCRYPT_BIN = $(OBJ_DIR)/xcrypt.bin
# Compact NES emulator (GB-035): HobbyOS shell (main.c) + pure-C core (nes.c).
NES_BIN = $(OBJ_DIR)/nes.bin
# GNU make 4.4.1 port: the upstream transcription in src/user/make/
# (byte-identical except the hand-pinned src/user/make/config.h +
# src/user/make/hb_stubs.c glue; see docs/selfhost/buildtools-audit.md S3
# and docs/gnu-ports.md).  MAKE_SRC_NAMES = the ported src/*.c core +
# the bundled glob/fnmatch + the glue.
MAKE_SRC_NAMES = ar arscan commands default dir expand file function getopt getopt1 guile hash implicit job load loadapi main misc output posixos read remake remote-stub rule shuffle signame strcache variable version vpath glob fnmatch hb_stubs
MAKE_OBJS = $(addprefix $(OBJ_DIR)/make_,$(addsuffix .o,$(MAKE_SRC_NAMES)))
MAKE_BIN = $(OBJ_DIR)/make.bin

# Selfhost W2 (mtools port, lane w2-mtools): GNU mtools 4.0.49 transcribed
# subset for the mcopy/mdel/mmd build-tools (L4 buildtools-audit §4).  The
# engine objects are the shared FAT/image closure both commands link; the
# per-tool main lives in mtools_port.c (upstream's single multicall main in
# mtools.c is deliberately NOT transcribed — it dispatches on argv[0] and
# references every tool, which does not fit HobbyOS's one-.BIN model).
MTOOLS_ENGINE_NAMES = buffer charsetConv codepages config copyfile device devices dirCache directory direntry dos2unix fat fat_free file file_name force_io hash init lba llong lockdev mainloop match mcopy mdel misc missFuncs mk_direntry mmd offset old_dos open_image partition patchlevel plain_io privileges remap signal stream streamcache strtonum swap tty unix2dos unixdir vfat
MTOOLS_ENGINE_OBJS = $(addprefix $(OBJ_DIR)/mtools_,$(addsuffix .o,$(MTOOLS_ENGINE_NAMES)))
MCOPY_BIN = $(OBJ_DIR)/mcopy.bin
MDEL_BIN = $(OBJ_DIR)/mdel.bin
MMD_BIN = $(OBJ_DIR)/mmd.bin

# GNU dosfstools 4.2 mkfs.fat port: four transcribed core sources plus the
# port's first-party blkdev/termios glue.  src/user/mkfs/config.h pins the
# HAVE_* feature set (docs/selfhost/buildtools-audit.md §5) and is force-
# included on every object so the upstream sources stay byte-identical.
MKFSFAT_SRC_NAMES = mkfs.fat common charconv device_info hb_blkdev
MKFSFAT_OBJS = $(addprefix $(OBJ_DIR)/mkfs_,$(addsuffix .o,$(MKFSFAT_SRC_NAMES)))
MKFSFAT_BIN = $(OBJ_DIR)/mkfsfat.bin
HEDGNU_BIN = $(OBJ_DIR)/hedgnu.bin
WCTEST_BIN = $(OBJ_DIR)/wc_test.bin
REGTEST_BIN = $(OBJ_DIR)/regex_test.bin
SEDTEST_BIN = $(OBJ_DIR)/sed_test.bin
GREPTEST_BIN = $(OBJ_DIR)/grep_test.bin
PIPEPROBE_BIN = $(OBJ_DIR)/pipeprobe.bin
SUBPRB_BIN = $(OBJ_DIR)/subprobe.bin
HEDTEST_BIN = $(OBJ_DIR)/head_test.bin
TAILGN_BIN = $(OBJ_DIR)/tailgnu.bin
CUT_BIN = $(OBJ_DIR)/cut.bin
CUTTEST_BIN = $(OBJ_DIR)/cut_test.bin
TR_BIN = $(OBJ_DIR)/tr.bin
TRTEST_BIN = $(OBJ_DIR)/tr_test.bin
PASTE_BIN = $(OBJ_DIR)/paste.bin
PASTE_T_BIN = $(OBJ_DIR)/paste_test.bin
FOLD_BIN = $(OBJ_DIR)/fold.bin
FOLDTEST_BIN = $(OBJ_DIR)/fold_test.bin
NL_BIN = $(OBJ_DIR)/nl.bin
NLTEST_BIN = $(OBJ_DIR)/nl_test.bin
COMM_BIN = $(OBJ_DIR)/comm.bin
COMMTEST_BIN = $(OBJ_DIR)/comm_test.bin
TSORT_BIN = $(OBJ_DIR)/tsort.bin
TSORT_T_BIN = $(OBJ_DIR)/tsort_test.bin
EXPAND_BIN = $(OBJ_DIR)/expand.bin
EXPAND_T_BIN = $(OBJ_DIR)/expand_test.bin
UNEXPAND_BIN = $(OBJ_DIR)/unexpand.bin
UNEXPAND_T_BIN = $(OBJ_DIR)/unexpand_test.bin
CKSUM_BIN = $(OBJ_DIR)/cksum.bin
CKSUM_T_BIN = $(OBJ_DIR)/cksum_test.bin
FATBIG_T_BIN = $(OBJ_DIR)/fatbig_test.bin
STAGE4K_T_BIN = $(OBJ_DIR)/stage4k_test.bin
LFN_T_BIN = $(OBJ_DIR)/lfn_test.bin
MD5SUM_BIN = $(OBJ_DIR)/md5sum.bin
MD5SUM_T_BIN = $(OBJ_DIR)/md5sum_test.bin
TAC_BIN = $(OBJ_DIR)/tac.bin
TACTEST_BIN = $(OBJ_DIR)/tac_test.bin
CMP_BIN = $(OBJ_DIR)/cmp.bin
CMPTEST_BIN = $(OBJ_DIR)/cmp_test.bin
TAILTEST_BIN = $(OBJ_DIR)/tail_test.bin
PROCCHLD_BIN = $(OBJ_DIR)/proc_child.bin
PROCTEST_BIN = $(OBJ_DIR)/proc_test.bin
# P5 (S3/S5): the signal-delivery/SIGRETURN/spawn_ex acceptance test and
# the spawn_ex child probe it spawns (BIN names: SIG_T.BIN, SPAWNEX.BIN).
SIGTEST_BIN = $(OBJ_DIR)/sig_test.bin
SPAWNEXCHILD_BIN = $(OBJ_DIR)/spawnex_child.bin
LKSTEST_BIN = $(OBJ_DIR)/lseek_test.bin
SORT_BIN = $(OBJ_DIR)/sort.bin
UNIQ_BIN = $(OBJ_DIR)/uniq.bin
PING_BIN = $(OBJ_DIR)/ping.bin
NC_BIN = $(OBJ_DIR)/nc.bin
IFCONFIG_BIN = $(OBJ_DIR)/ifconfig.bin
MONITOR_BIN = $(OBJ_DIR)/monitor.bin
MONITOR_TEST_BIN = $(OBJ_DIR)/monitort.bin
SHELL_TEST2_BIN = $(OBJ_DIR)/shtest2.bin
MKDIR_BIN = $(OBJ_DIR)/mkdir.bin
SHELL_TEST3_BIN = $(OBJ_DIR)/shtest3.bin
PONG_BIN = $(OBJ_DIR)/pong.bin
MILLIPEDE_BIN = $(OBJ_DIR)/millipede.bin
DOOM_BIN = $(OBJ_DIR)/doom.bin
STARDUST_BIN = $(OBJ_DIR)/stardust.bin

# --- New desktop applications (10 GUI apps) ---
FILES_BIN = $(OBJ_DIR)/files.bin
CALC_BIN = $(OBJ_DIR)/calc.bin
XCALC_BIN = $(OBJ_DIR)/xcalc.bin
ANTFARM_BIN = $(OBJ_DIR)/antfarm.bin
XEYES_BIN = $(OBJ_DIR)/xeyes.bin
XNIBBLER_BIN = $(OBJ_DIR)/xnibbler.bin
CLOCK_BIN = $(OBJ_DIR)/clock.bin
SYSMON_BIN = $(OBJ_DIR)/sysmon.bin
HEX_BIN = $(OBJ_DIR)/hex.bin
TASKS_BIN = $(OBJ_DIR)/tasks.bin
FIND_BIN = $(OBJ_DIR)/find.bin
DIFF_BIN = $(OBJ_DIR)/diff.bin
NOTES_BIN = $(OBJ_DIR)/notes.bin
UNIT_BIN = $(OBJ_DIR)/unit.bin
DESKTOP_APP_NAMES = files calc clock sysmon hex tasks find diff notes unit
DESKTOP_APP_BINS = $(FILES_BIN) $(CALC_BIN) $(CLOCK_BIN) $(SYSMON_BIN) $(HEX_BIN) $(TASKS_BIN) $(FIND_BIN) $(DIFF_BIN) $(NOTES_BIN) $(UNIT_BIN)
# Common link objects for GUI applications (libc + allocator + toolkit + dialogs)
GUI_APP_OBJS = $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_gui.o $(OBJ_DIR)/user_dialog.o $(OBJ_DIR)/user_filedialog.o

# Default rule: build the target
all: $(TARGET)

# The final linking step
# Combine the objects to create the ELF binary.  The kernel ELF lives in the
# repo root under the shared name hobbyos.elf for BOTH arches, so a build
# switch (arm -> intel or back) would leave a stale cross-arch ELF that the
# disk.img recipe then objcopy's onto the wrong disk ("limine: invalid kernel
# image magic" at boot).  FORCE_ARCH makes this rule always run; the stamp
# check removes the stale ELF first, then we relink for the current ARCH.
.PHONY: FORCE_ARCH
FORCE_ARCH:
# .EXTRA_PREREQS (GNU make >= 4.3) keeps FORCE_ARCH out of `$^` (a plain
# prerequisite would be fed to ld.lld as an input file -- same trap as the
# linker-script dependency).
$(TARGET): .EXTRA_PREREQS = FORCE_ARCH
$(TARGET): $(OBJS)
	@if [ "$$(cat .arch-stamp 2>/dev/null)" != "$(ARCH)" ]; then rm -f $@; printf '%s' "$(ARCH)" > .arch-stamp; fi
	$(LD) $(LDFLAGS) -o $@ $^
ifeq ($(ARCH),intel)
	$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $@
endif

# Rule to compile .c files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c src/include/*.h $(MODE_FILE)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Generated constant: the on-image size of the UNEXPAND_T.BIN fixture.
# fat16_test asserts this size to prove a long 8.3-truncating name resolves to
# the REAL test binary and not to the shorter UNEXPAND.BIN.  The size moves
# with the toolchain and the architecture, so it is generated from the actual
# artifact at build time — the hardcoded constant rotted this way three times
# (each userland change that shifted the binary turned one or both arches red).
UNEXPAND_SIZE_HDR = $(OBJ_DIR)/unexpand_size_gen.h
$(UNEXPAND_SIZE_HDR): $(UNEXPAND_T_BIN)
	@mkdir -p $(OBJ_DIR)
	@printf '#define UNEXPAND_T_BIN_SIZE %s\n' "$$(wc -c < $< | tr -d ' ')" > $@
$(OBJ_DIR)/fat16_test.o: $(UNEXPAND_SIZE_HDR)
$(OBJ_DIR)/fat16_test.o: CFLAGS += -I$(OBJ_DIR)

# Rule to compile .s files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.s src/include/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to compile .c files from arch into .o files
$(OBJ_DIR)/%.o: $(ARCH_DIR)/%.c src/include/*.h src/include/arch/*.h $(MODE_FILE)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to compile .s files from arch into .o files
$(OBJ_DIR)/%.o: $(ARCH_DIR)/%.s src/include/*.h src/include/arch/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# --- F1.5 (FPU): the deliberate -mgeneral-regs-only exception -----------
# arch/arm/fpu.s is the ONE kernel translation unit allowed to touch the
# FP/SIMD register file: saving/restoring q0-q31 + FPCR + FPSR is not
# expressible without it, and clang's assembler rejects those instructions
# outright when the restriction is in effect ("instruction requires:
# fp-armv8").  The file is lowercase .s, so the ASM_OBJS wildcard compiles
# and links it into the kernel like boot.s/setjmp.s; this explicit target
# (which GNU make prefers over the pattern rule above) is the ONLY build
# path that drops the flag — and it drops it for hand-written assembly
# only, so no kernel C codegen gains FP/SIMD access.
ifeq ($(ARCH),arm)
$(OBJ_DIR)/fpu.o: $(ARCH_DIR)/fpu.s src/include/*.h src/include/arch/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -c $< -o $@
endif

# Rule to compile user objects
$(OBJ_DIR)/user_%.o: src/user/%.c src/user_include/*.h src/user_include/graphics/*.h src/include/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/mem_test.o: src/user/mem_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR) 
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/file_io_test.o: src/user/file_io_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR) 
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/console.o: src/user/console.c $(USER_LIBC) $(USER_HDRS) $(MODE_FILE)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/fork_test.o: src/user/fork_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/spawn_test.o: src/user/spawn_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/heap_test.o: src/user/heap_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/graphics_test.o: src/user/graphics_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/smp_test.o: src/user/smp_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_pipe_test.o: src/user/pipe_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/stress_test.o: src/user/stress_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# F1.5: floating-point bring-up test (src/user/fpu_test.c) — built like the
# other *_test binaries; USER_CFLAGS no longer restrict user FP/SIMD on
# either arch (ARM drops -mgeneral-regs-only; intel drops the -mno-sse
# family), so float/double ops compile to real FP instructions.
$(OBJ_DIR)/fpu_test.o: src/user/fpu_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# GX (docs/graphics-accel.md): the framebuffer present benchmark.
$(OBJ_DIR)/gfxbench.o: src/user/gfxbench.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# GB-045: pure burst-per-flush stats shared by GFXBENCH and its host test.
$(OBJ_DIR)/gfx_stats.o: src/user/gfx_stats.c src/user/gfx_stats.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# P3.2: transcendental acceptance (crt0 + libc.a, the sysroot link path the
# ICU probe uses).  USER_CFLAGS allows FP in user code since F1.5.
# NOTE: user_math_test.o (not math_test.o) -- src/kernel/math_test.c owns
# obj/<arch>/math_test.o via the kernel wildcard, and one path, one recipe.
$(OBJ_DIR)/user_math_test.o: src/user/math_test.c $(OBJ_DIR)/libc.a $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MATH_T_BIN): $(OBJ_DIR)/user_math_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/math_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/math_test.elf $(MATH_T_BIN)

# Phase F1 user acceptance programs (browser.md).
$(OBJ_DIR)/sock2_test.o: src/user/sock2_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/poll_test.o: src/user/poll_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/rand_test.o: src/user/rand_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/blktest.o: src/user/blktest.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_graphics.o: src/user/graphics/graphics.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_window.o: src/user/graphics/window.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# X11 support library (src/user/x11/): shared by XCALC.BIN and future
# X11 apps.  The library's own headers live under src/user/x11/include.
$(OBJ_DIR)/x11_%.o: src/user/x11/%.c src/user/x11/*.h src/user/x11/include/X11/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@

$(OBJ_DIR)/desktop.o: src/user/desktop.c $(USER_LIBC) src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/games_scan.o: src/user/games_scan.c $(USER_LIBC) src/user_include/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/editor.o: src/user/editor.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_dialog.o: src/user/dialog.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_filedialog.o: src/user/filedialog.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_editor_test.o: src/user/editor_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_net_test.o: src/user/net_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# F2.2/F2.3 resolver + DNSTST: OBJECT-ONLY rules (no .bin/disk.img/main.c
# wiring yet).  DNSTST.BIN is wired at integration once the net lane's
# syscalls + sysinfo(4).dns land; src/user/dns_test.c's header comment
# carries the exact steps.
$(OBJ_DIR)/user_resolv.o: src/user/resolv.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_dns_test.o: src/user/dns_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_desktop_test_wrapper.o: src/user/desktop.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Dmain=desktop_main -DDESKTOP_TEST_WRAPPER -c $< -o $@

$(MEM_TEST_BIN): $(OBJ_DIR)/mem_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/memtest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/memtest.elf $(MEM_TEST_BIN)

$(FILE_IO_BIN): $(OBJ_DIR)/file_io_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/fileio_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/fileio_test.elf $(FILE_IO_BIN)

$(CONSOLE_BIN): $(OBJ_DIR)/console.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/console.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/console.elf $(CONSOLE_BIN)

$(FORK_TEST_BIN): $(OBJ_DIR)/fork_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/fork_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/fork_test.elf $(FORK_TEST_BIN)

$(HEAP_TEST_BIN): $(OBJ_DIR)/heap_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_mman.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/heap_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/heap_test.elf $(HEAP_TEST_BIN)

$(SPAWN_TEST_BIN): $(OBJ_DIR)/spawn_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/spawn_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/spawn_test.elf $(SPAWN_TEST_BIN)

$(GRAPHICS_TEST_BIN): $(OBJ_DIR)/graphics_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_graphics.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/graphics_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/graphics_test.elf $(GRAPHICS_TEST_BIN)

# rp-w (docs/browser/render-pipeline.md Design 1): the wsurf window-surface
# render-confinement tests.  src/user/wsurf_test.c shares one source between
# the host build (-DHOST_TEST) and this device bin; it needs only libc (for
# print/exit) + the wsurf object -- no real framebuffer, it tests the layer
# against a private canary buffer.
$(OBJ_DIR)/wsurf_test.o: src/user/wsurf_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_wsurf.o: src/user/graphics/wsurf.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(WSRF_T_BIN): $(OBJ_DIR)/wsurf_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_wsurf.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/wsurf_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/wsurf_test.elf $(WSRF_T_BIN)

$(SMP_TEST_BIN): $(OBJ_DIR)/smp_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/smp_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/smp_test.elf $(SMP_TEST_BIN)

$(PIPETEST_BIN): $(OBJ_DIR)/user_pipe_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/pipe_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/pipe_test.elf $(PIPETEST_BIN)

$(STRESS_TEST_BIN): $(OBJ_DIR)/stress_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/stress.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/stress.elf $(STRESS_TEST_BIN)

$(FPU_T_BIN): $(OBJ_DIR)/fpu_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/fpu_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/fpu_test.elf $(FPU_T_BIN)

$(GFXBENCH_BIN): $(OBJ_DIR)/gfxbench.o $(OBJ_DIR)/gfx_stats.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/gfxbench.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/gfxbench.elf $(GFXBENCH_BIN)

$(SOCK2TST_BIN): $(OBJ_DIR)/sock2_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/sock2_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/sock2_test.elf $(SOCK2TST_BIN)

$(POLLTST_BIN): $(OBJ_DIR)/poll_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/poll_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/poll_test.elf $(POLLTST_BIN)

# GB-086: negative control variant of the POLLTST select-timeout check, built
# with -DPOLLTST_BROKEN_SELECT (the 100ms timeout select is replaced by a
# zero-timeout select that returns immediately, ~0ms).  The >=85ms elapsed
# check MUST therefore FAIL — this proves the gate cannot be painted green by a
# broken select.  wabi: measured in-guest, ARM T3_SMP=1 → elapsed_ms=0, FAIL.
# The DOS name is POLLBRK.BIN (8.3-legal; POLLTSTBRK.BIN is 11 chars and FAT
# truncates it to a 0-byte POLLTSTB.BIN on disk.img — see GB-086-V RED).
POLLTSTBRK_BIN = $(OBJ_DIR)/POLLBRK.BIN
$(OBJ_DIR)/poll_test_brk.o: src/user/poll_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DPOLLTST_BROKEN_SELECT -c src/user/poll_test.c -o $@
$(POLLTSTBRK_BIN): $(OBJ_DIR)/poll_test_brk.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/poll_test_brk.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/poll_test_brk.elf $(POLLTSTBRK_BIN)

$(RANDTST_BIN): $(OBJ_DIR)/rand_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/rand_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/rand_test.elf $(RANDTST_BIN)

$(BLKTEST_BIN): $(OBJ_DIR)/blktest.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/blktest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/blktest.elf $(BLKTEST_BIN)

$(DNSTST_BIN): $(OBJ_DIR)/user_dns_test.o $(OBJ_DIR)/user_resolv.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/dns_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/dns_test.elf $(DNSTST_BIN)

# l8-netfix repro app (TX/console + RX-NUL); see src/user/netfix_test.c.
$(OBJ_DIR)/user_netfix_test.o: src/user/netfix_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NETFIX_BIN): $(OBJ_DIR)/user_netfix_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/netfix_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/netfix_test.elf $(NETFIX_BIN)

# --- F2.4 (browser.md §6): minimal C++ runtime + smoke -------------------
# cxxrt.cpp is the runtime (operator new/delete, __cxa_guard_*, __cxa_atexit
# /__cxa_finalize, __cxa_pure_virtual, __dso_handle); it is archived into
# libc.a below, so crt0-style C++ programs pick it up on demand.  The smoke
# test proves the whole path (crt0 walks .init_array, static ctors run,
# guards fire, delete goes through the runtime) on both arches.
$(OBJ_DIR)/cxxrt.o: src/libc/src/cxxrt.cpp src/libc/include/cxxrt.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXX_USER_FLAGS) -c $< -o $@

$(OBJ_DIR)/cxx_smoke.o: src/user/cxx_smoke.cpp $(USER_LIBC) $(USER_HDRS) src/libc/include/cxxrt.h
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXX_USER_FLAGS) -c $< -o $@

$(CXXSMOKE_BIN): $(OBJ_DIR)/cxx_smoke.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/cxx_smoke.elf $(OBJ_DIR)/cxx_smoke.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/cxx_smoke.elf $(CXXSMOKE_BIN)

# --- P3 (browser.md §6): libc++ acceptance -------------------------------
# cxx_t.cpp uses <vector> <string> <map> <unordered_map> <algorithm>
# <memory> <atomic> <thread> <mutex> <condition_variable> <chrono>
# <sstream> from the vendored libc++, threads/mutex/cond from libc.a's
# pthread.c, and prints "name : PASS" lines per check.
# These TUs compile against the vendored headers, so their prerequisite is a
# real file of the fetched tree (not the bare .fetched-ok marker): a deleted
# tree under an up-to-date marker would otherwise pass make's timestamp check
# and the compile would hit the missing <algorithm>/<typeinfo> headers.  The
# $(LIBCXX_INCLUDE)/% guard below shares the marker-invalidation recipe with
# the source-pattern guards further down.
$(OBJ_DIR)/cxx_t.o: src/user/cxx_t.cpp $(USER_LIBC) $(USER_HDRS) $(LIBCXX_INCLUDE)/algorithm $(LIBCXX_VENDOR)/sources.txt
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXX_LIBCXX_USER_FLAGS) -c $< -o $@

$(CXX_T_BIN): $(OBJ_DIR)/cxx_t.o $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/cxx_t.elf $(OBJ_DIR)/cxx_t.o $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/cxx_t.elf $(CXX_T_BIN)

# l3-rtti (browser.md §6/L6): RTTI acceptance for the libc++abi cast
# machinery.  The program compiles -frtti (the ICU consumption mode) and
# links libcxx.a, whose class-B objects define the four symbols the ICU link
# probe reported (__dynamic_cast + the __*_class_type_info vtables).  See
# src/user/cxx_rtti_t.cpp for the battery; RTTI_T.BIN is 8.3-safe.
$(OBJ_DIR)/cxx_rtti_t.o: src/user/cxx_rtti_t.cpp $(USER_LIBC) $(USER_HDRS) $(LIBCXX_INCLUDE)/typeinfo $(LIBCXX_VENDOR)/sources.txt
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXX_RTTI_USER_FLAGS) -c $< -o $@

$(CXX_RTTI_T_BIN): $(OBJ_DIR)/cxx_rtti_t.o $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/rtti_t.elf $(OBJ_DIR)/cxx_rtti_t.o $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/rtti_t.elf $(CXX_RTTI_T_BIN)

# The vendored libc++ + libc++abi static archive (one per arch).  Built by
# third_party/libcxx-21.1.8/build-target.sh from sources.txt with the
# committed __config_site; the extracted upstream tree is fetched by
# fetch.sh (Skia vendor recipe) and is gitignored.
$(OBJ_DIR)/libcxx.a: $(LIBCXX_VENDOR)/build-target.sh $(LIBCXX_VENDOR)/sources.txt $(LIBCXX_VENDOR)/config/__config_site
	@mkdir -p $(OBJ_DIR)
	bash $(LIBCXX_VENDOR)/build-target.sh --arch $(ARCH) --objdir $(OBJ_DIR)/libcxx --out $@

# --- l3-icu-wire (browser L6): ICU 78.3 target archives + smoke ----------
# build-target.sh is the single entry point (fetch -> host tools -> target
# configure/make -> nm spot-check -> link probe -> MANIFEST).  It self-heals
# every gitignored piece (tarball, extracted src/, host buildroot, objdir),
# so the committed scripts are the ONLY prerequisites a fresh checkout
# needs; MANIFEST.txt is the evidence file (probe status + archive hashes).
# Additive by design: this lane touches no other build step.
ICU_DIR = third_party/icu-78.3
ICU_MANIFEST = $(OBJ_DIR)/icu/MANIFEST.txt

$(ICU_MANIFEST): $(ICU_DIR)/build-target.sh $(ICU_DIR)/fetch.sh $(ICU_DIR)/build-host.sh $(ICU_DIR)/config/mh-unknown $(ICU_DIR)/data-filter-en.json $(ICU_DIR)/probe/icu_target_probe.cpp
	@mkdir -p $(OBJ_DIR)/icu
	bash $(ICU_DIR)/build-target.sh --arch $(ARCH)

# Guard against a hand-deleted archive under an up-to-date MANIFEST: the
# script re-stages the archives and rewrites the manifest (~1s warm).
$(OBJ_DIR)/icu/libicuuc.a: $(ICU_MANIFEST)
	@test -f $@ || bash $(ICU_DIR)/build-target.sh --arch $(ARCH)

# The smoke TU compiles against the extracted ICU headers (gitignored,
# created by the manifest rule) with the house user flags;
# -ffunction-sections + the link's --gc-sections keep the image far below
# the 1 MB user-image loader cap (program_loader.c MAX_PROGRAM_SIZE).
# The TU's prerequisite is a real header of the fetched tree, not the bare
# MANIFEST evidence file: a hand-deleted src/ tree under an up-to-date
# MANIFEST would otherwise skip the rebuild and the compile would hit the
# missing <unicode/utypes.h>.  The guard invalidates the MANIFEST so
# build-target.sh self-heals (fetch -> extract -> reconfigure -> rewrite).
$(ICU_DIR)/src/source/common/unicode/utypes.h: $(ICU_MANIFEST)
	@test -f $@ || { rm -f $(ICU_MANIFEST); bash $(ICU_DIR)/build-target.sh --arch $(ARCH); test -f $@; }

$(OBJ_DIR)/icu_smoke.o: src/user/icu_smoke.c $(USER_LIBC) $(USER_HDRS) $(ICU_MANIFEST) $(ICU_DIR)/src/source/common/unicode/utypes.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -ffunction-sections -I$(ICU_DIR)/src/source/common -I$(ICU_DIR)/src/source/i18n -c $< -o $@

$(ICU_SMOKE_BIN): $(OBJ_DIR)/icu_smoke.o $(OBJ_DIR)/crt0.o $(OBJ_DIR)/icu/libicuuc.a $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start --gc-sections -o $(OBJ_DIR)/icusmk.elf $(OBJ_DIR)/icu_smoke.o $(OBJ_DIR)/crt0.o $(OBJ_DIR)/icu/libicuuc.a $(OBJ_DIR)/libcxx.a $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/icusmk.elf $(ICU_SMOKE_BIN)

$(ERRNO_TEST_BIN): $(OBJ_DIR)/user_errno_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/errno_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/errno_test.elf $(ERRNO_TEST_BIN)

# --- libc sysroot (Phase 0, posix.md §2.3) ------------------------------
# crt0.o provides the main(argc, argv) entry convention; libc.a is the
# archive new POSIX-style ports link against (crt0 + libc + malloc).
$(OBJ_DIR)/crt0.o: src/libc/crt0.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# Phase 1: src/libc/src/*.c are archived into libc.a (pattern rule, both
# arches since OBJ_DIR varies). Private headers (src/libc/src/*.h) are
# included in the dependencies so internal changes rebuild the objects.
$(OBJ_DIR)/libc_%.o: src/libc/src/%.c $(USER_HDRS) src/libc/src/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# math.c's double-double kernels (Dekker/Veltkamp two_product, the
# compensated division, the Cody-Waite reduction residuals) are only
# exact under strictly-rounded operations: a fused multiply-add in any of
# those expressions breaks the error term.  Scope the flag to this one
# object rather than changing USER_CFLAGS for every libc TU.
$(OBJ_DIR)/libc_math.o: src/libc/src/math.c $(USER_HDRS) src/libc/src/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -ffp-contract=off -c $< -o $@

# The P3.2 gap-fills use long double (strtold/strtof's widening casts, the
# %Lf scanner path): on aarch64 clang lowers those to the binary128
# compiler-rt builtins.  libc.a carries exactly the three that its members
# reference so plain C programs (basename, sed, ...) link; libcxx.a carries
# the full set for the C++ side.  x86_64 lowers long double to x87 and the
# objects are simply unreferenced there.  Sources come from the pinned
# vendor tree (fetch.sh self-heals if it is missing).
LIBC_BUILTIN_SRCS = extenddftf2.c trunctfdf2.c trunctfsf2.c
LIBC_BUILTIN_OBJS = $(addprefix $(OBJ_DIR)/builtins/,$(LIBC_BUILTIN_SRCS:.c=.o))
$(LIBCXX_VENDOR)/.fetched-ok: $(LIBCXX_VENDOR)/fetch.sh $(LIBCXX_VENDOR)/anchors.sha256
	@bash $(LIBCXX_VENDOR)/fetch.sh

# Header guard (mirror of the source guards below): any TU that includes
# vendored libc++ headers depends on a concrete file of the fetched tree.
# The marker is only trusted once a real extracted header exists under it, so
# a hand-deleted tree (which would otherwise leave .fetched-ok stale and make
# skip the fetch) is restored here instead of failing the later #include.
$(LIBCXX_INCLUDE)/%: $(LIBCXX_VENDOR)/.fetched-ok
	@test -f $@ || { rm -f $(LIBCXX_VENDOR)/.fetched-ok; bash $(LIBCXX_VENDOR)/fetch.sh; test -f $@; }

# The header pattern builds the TUs' prerequisites; like the source patterns
# below, keep the extracted headers in place across builds.
.SECONDARY: $(LIBCXX_INCLUDE)/algorithm $(LIBCXX_INCLUDE)/typeinfo

# On a fresh checkout the source does not exist yet.  make only accepts a
# pattern rule as "makeable" when it has a recipe (an empty-recipe pattern is
# rejected during the implicit-rule search, before fetch.sh could run -- it
# reports "No rule to make target 'obj/<arch>/builtins/<x>.o', needed by
# libc.a").  So the source pattern carries a guard recipe.  The marker is
# invalidated first: fetch.sh early-exits when the marker + tree verify, so a
# tree that is merely missing one file would otherwise never be restored (and
# the recipe would succeed silently without creating it).  The trailing
# test -f turns that into a loud failure.
$(LIBCXX_ROOT)/compiler-rt/lib/builtins/%.c: $(LIBCXX_VENDOR)/.fetched-ok
	@test -f $@ || { rm -f $(LIBCXX_VENDOR)/.fetched-ok; bash $(LIBCXX_VENDOR)/fetch.sh; test -f $@; }

# The fetched sources are built via the pattern above, so make classifies them
# as intermediate files and deletes them after use; .SECONDARY keeps them in
# place so a later rebuild does not re-extract the tree.
.SECONDARY: $(addprefix $(LIBCXX_ROOT)/compiler-rt/lib/builtins/,$(LIBC_BUILTIN_SRCS))

# Same guard for the libcxxabi sources the HOST RTTI test compiles
# (obj/host_rtti_*.o below): on a fresh checkout the file does not exist yet,
# so the pattern rule must carry a recipe, invalidate the marker BEFORE
# fetching and fail loudly (see the note above).  .SECONDARY again keeps the
# fetched sources in place across builds.
$(LIBCXX_ROOT)/libcxxabi/src/%.cpp: $(LIBCXX_VENDOR)/.fetched-ok
	@test -f $@ || { rm -f $(LIBCXX_VENDOR)/.fetched-ok; bash $(LIBCXX_VENDOR)/fetch.sh; test -f $@; }

RTTI_HOST_VENDOR_SRCS = libcxxabi/src/private_typeinfo.cpp libcxxabi/src/stdlib_typeinfo.cpp libcxxabi/src/stdlib_exception.cpp
.SECONDARY: $(addprefix $(LIBCXX_ROOT)/,$(RTTI_HOST_VENDOR_SRCS))

$(OBJ_DIR)/builtins/%.o: $(LIBCXX_ROOT)/compiler-rt/lib/builtins/%.c $(LIBCXX_VENDOR)/.fetched-ok
	@mkdir -p $(OBJ_DIR)/builtins
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/libc.a: $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/crt0.o $(OBJ_DIR)/cxxrt.o $(OBJ_DIR)/libc_pthread.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_stdlib.o $(OBJ_DIR)/libc_stdio.o $(OBJ_DIR)/libc_file.o $(OBJ_DIR)/libc_getopt.o $(OBJ_DIR)/libc_error.o $(OBJ_DIR)/libc_stat.o $(OBJ_DIR)/libc_signal.o $(OBJ_DIR)/libc_mman.o $(OBJ_DIR)/libc_regex.o $(OBJ_DIR)/libc_langinfo.o $(OBJ_DIR)/libc_wchar.o $(OBJ_DIR)/libc_wctype.o $(OBJ_DIR)/libc_locale.o $(OBJ_DIR)/libc_selinux.o $(OBJ_DIR)/libc_dirent.o $(OBJ_DIR)/libc_time.o $(OBJ_DIR)/libc_time_math.o $(OBJ_DIR)/libc_libgen.o $(OBJ_DIR)/libc_realpath.o $(OBJ_DIR)/libc_xlocale.o $(OBJ_DIR)/libc_strtod.o $(OBJ_DIR)/libc_strftime.o $(OBJ_DIR)/libc_math.o $(OBJ_DIR)/libc_pwd.o $(OBJ_DIR)/libc_setjmp.o $(OBJ_DIR)/libc_dlfcn.o $(OBJ_DIR)/libc_resource.o $(OBJ_DIR)/libc_statvfs.o $(OBJ_DIR)/libc_syscall.o $(OBJ_DIR)/libc_ioctl.o $(LIBC_BUILTIN_OBJS)
	$(AR) rcs $@ $^

# --- HELLO demo (Phase 0 gate): a main(argc, argv) program built against
# crt0.o + libc.a, proving the sysroot link path end to end. -e _start is
# redundant with linker.ld's ENTRY(_start) but pins the archive pull. ---
$(HELLO_BIN): $(OBJ_DIR)/hello.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/hello.elf $(OBJ_DIR)/hello.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/hello.elf $(HELLO_BIN)

$(OBJ_DIR)/hello.o: src/user/hello.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# --- SELFHOST driver (self-hosting wave-2 harness, acceptance.md §5): the
# in-OS acceptance build driver.  Sources live in src/user/selfhost/; links
# crt0 + libc.a like the hello demo.  Installed on the boot disk root as
# SELFHOST.BIN (see the disk.img recipe). ---
SELFHOST_BIN = $(OBJ_DIR)/selfhost.bin
$(OBJ_DIR)/selfhost.o: src/user/selfhost/selfhost.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(SELFHOST_BIN): $(OBJ_DIR)/selfhost.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/selfhost.elf $(OBJ_DIR)/selfhost.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/selfhost.elf $(SELFHOST_BIN)

# Fresh coreutils-parity utilities: basename, dirname, seq, expr, test.
# These are hand-written POSIX programs (not vendored GNU sources); they
# link crt0 + libc.a like the hello smoke test.
define parity_bin
$(1)_BIN = $$(OBJ_DIR)/$(2).bin
$$(OBJ_DIR)/$(2).o: src/user/$(2).c $$(USER_LIBC) $$(USER_HDRS) src/libc/crt0.c
	@mkdir -p $$(OBJ_DIR)
	$$(CC) $$(USER_CFLAGS) -c $$< -o $$@
$$(OBJ_DIR)/$(2).bin: $$(OBJ_DIR)/$(2).o $$(OBJ_DIR)/libc.a
	$$(LD) -T src/user/linker.ld -e _start -o $$(OBJ_DIR)/$(2).elf $$(OBJ_DIR)/$(2).o $$(OBJ_DIR)/libc.a
	$$(OBJCOPY) -O binary $$(OBJ_DIR)/$(2).elf $$@
endef
$(eval $(call parity_bin,BASENAME,basename))
$(eval $(call parity_bin,DIRNAME,dirname))
$(eval $(call parity_bin,SEQ,seq))
$(eval $(call parity_bin,EXPR,expr))
$(eval $(call parity_bin,TESTGNU,testgnu))

# P1: threads + TLS acceptance (crt0 + libc.a; late wave positions).  The
# crt0 dependency keeps the TLS bootstrap rebuild honest.
$(OBJ_DIR)/thrd_test.o: src/user/thrd_test.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(THRD_T_BIN): $(OBJ_DIR)/thrd_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/thrd_test.elf $(OBJ_DIR)/thrd_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/thrd_test.elf $(THRD_T_BIN)
$(OBJ_DIR)/tls_test.o: src/user/tls_test.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(TLS_T_BIN): $(OBJ_DIR)/tls_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/tls_test.elf $(OBJ_DIR)/tls_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/tls_test.elf $(TLS_T_BIN)

# L8 WK-1 (WebKit JSC shell libc/OS prerequisites): same crt0 + libc.a link
# as THRD_T/TLS_T.  WK1C_T.BIN asserts the clocks (SYS_GETTIME row 87),
# getentropy, sched_yield, sched_get_priority_min/max, isatty and the
# pthread stack-bounds exposure (pthread_getattr_np over SYS_GETSTACK
# row 88).
WK1C_T_BIN = $(OBJ_DIR)/WK1C_T.BIN
$(OBJ_DIR)/wk1c_test.o: src/user/wk1c_test.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(WK1C_T_BIN): $(OBJ_DIR)/wk1c_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/wk1c_test.elf $(OBJ_DIR)/wk1c_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/wk1c_test.elf $(WK1C_T_BIN)

# P4: AF_UNIX IPC acceptance (crt0 + libc.a like THRD_T; forks children, so
# it runs late in the wave).
$(OBJ_DIR)/ipc_test.o: src/user/ipc_test.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(IPC_T_BIN): $(OBJ_DIR)/ipc_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/ipc_test.elf $(OBJ_DIR)/ipc_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/ipc_test.elf $(IPC_T_BIN)

# P2.2 (S2): v2 userland acceptance.  Deliberately linked at USER_IMG_BASE
# with linker_v2.ld (no crt0, no libc objects): the kernel's loader v2 maps
# this flat .bin into fresh 4 KiB frames in the AS_V2 IMAGE region.  The
# libc.a path is avoided on purpose — its TLS bootstrap's __tls_align
# absolute-symbol idiom is unreachable by PC-relative relocations from a
# 64 GiB image (a TLS-rework item, out of S2 scope).
#
# x64 needs -mcmodel=large for a 64 GiB image: the default small model
# emits R_X86_64_32 absolute relocations for .rodata, which cannot
# encode an address above 4 GiB (the v1 layout at 0x44000000 could).
ifeq ($(ARCH),intel)
MMTEST_USER_CFLAGS = $(USER_CFLAGS) -mcmodel=large
else
MMTEST_USER_CFLAGS = $(USER_CFLAGS)
endif
$(OBJ_DIR)/mmtest.o: src/user/mmtest.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(MMTEST_USER_CFLAGS) -c $< -o $@
$(MMTEST_T_BIN): $(OBJ_DIR)/mmtest.o
	$(LD) -T src/user/linker_v2.ld -e _start -o $(OBJ_DIR)/mmtest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/mmtest.elf $(MMTEST_T_BIN)

# P8.1 (browser.md §6): POSIX torture acceptance (crt0 + libc.a like
# THRD_T/IPC_T).  Runs LAST in the wave: it threads 64-wide and forks, so
# the earlier suites should have drained first.
$(OBJ_DIR)/torture_test.o: src/user/torture_test.c $(USER_LIBC) $(USER_HDRS) src/libc/crt0.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(TORTURE_T_BIN): $(OBJ_DIR)/torture_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/torture_test.elf $(OBJ_DIR)/torture_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/torture_test.elf $(TORTURE_T_BIN)

$(NETTEST_BIN): $(OBJ_DIR)/user_net_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/net_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/net_test.elf $(NETTEST_BIN)

$(OBJ_DIR)/user_net_timeout_test.o: src/user/net_timeout_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(TIMEOUT_BIN): $(OBJ_DIR)/user_net_timeout_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/net_timeout_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/net_timeout_test.elf $(TIMEOUT_BIN)

$(OBJ_DIR)/user_nfs_test.o: src/user/nfs_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NFSTEST_BIN): $(OBJ_DIR)/user_nfs_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/nfs_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/nfs_test.elf $(NFSTEST_BIN)

$(DESKTOP_BIN): $(OBJ_DIR)/desktop.o $(OBJ_DIR)/games_scan.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/desktop_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/desktop_test.elf $(DESKTOP_BIN)

$(EDITOR_BIN): $(OBJ_DIR)/editor.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_dialog.o $(OBJ_DIR)/user_filedialog.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/editor.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/editor.elf $(EDITOR_BIN)

$(EDITOR_T_BIN): $(OBJ_DIR)/user_editor_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/games_scan.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/editor_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/editor_test.elf $(EDITOR_T_BIN)

$(OBJ_DIR)/user_apps_test.o: src/user/apps_test.c $(USER_HDRS) $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DPAINT_STATS -c $< -o $@

# Same graphics library, built with the test-only paint counter the APPS_T
# harness reads (see graphics.c).  Production binaries link user_graphics.o.
$(OBJ_DIR)/user_graphics_stats.o: src/user/graphics/graphics.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DPAINT_STATS -c $< -o $@

$(APPS_T_BIN): $(OBJ_DIR)/user_apps_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/games_scan.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics_stats.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/apps_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/apps_test.elf $(APPS_T_BIN)

FILEDIALOG_ARROW_T_BIN = $(OBJ_DIR)/FILEDIAL.BIN
$(OBJ_DIR)/user_filedialog_arrow_test.o: src/user/filedialog_arrow_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(FILEDIALOG_ARROW_T_BIN): $(OBJ_DIR)/user_filedialog_arrow_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/games_scan.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/filedialog_arrow_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/filedialog_arrow_test.elf $(FILEDIALOG_ARROW_T_BIN)

DIALOG_TEST_BIN = $(OBJ_DIR)/DIALOG_T.BIN
$(DIALOG_TEST_BIN): $(OBJ_DIR)/user_dialog_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_dialog.o $(OBJ_DIR)/user_filedialog.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/dialog_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/dialog_test.elf $(DIALOG_TEST_BIN)

$(OBJ_DIR)/user_pong_test.o: src/user/pong_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DPONG_TEST_WRAPPER -c $< -o $@

$(PONG_T_BIN): $(OBJ_DIR)/user_pong_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/games_scan.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/pong_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/pong_test.elf $(PONG_T_BIN)

$(OBJ_DIR)/sh.o: src/user/sh.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/ls.o: src/user/ls.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/cat.o: src/user/cat.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/less.o: src/user/less.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/tail.o: src/user/tail.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/head.o: src/user/head.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/sh_aux.o: src/user/sh_aux.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SH_BIN): $(OBJ_DIR)/sh.o $(OBJ_DIR)/sh_aux.o $(OBJ_DIR)/crt0.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/sh.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/sh.elf $(SH_BIN)

$(LS_BIN): $(OBJ_DIR)/ls.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/ls.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/ls.elf $(LS_BIN)

$(CAT_BIN): $(OBJ_DIR)/cat.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/cat.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/cat.elf $(CAT_BIN)

$(LESS_BIN): $(OBJ_DIR)/less.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/less.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/less.elf $(LESS_BIN)

$(TAIL_BIN): $(OBJ_DIR)/tail.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/tail.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/tail.elf $(TAIL_BIN)

$(HEAD_BIN): $(OBJ_DIR)/head.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/head.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/head.elf $(HEAD_BIN)

$(OBJ_DIR)/shell_test.o: src/user/shell_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHELL_TEST_BIN): $(OBJ_DIR)/shell_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/shtest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/shtest.elf $(SHELL_TEST_BIN)

$(OBJ_DIR)/ps.o: src/user/ps.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(PS_BIN): $(OBJ_DIR)/ps.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/ps.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/ps.elf $(PS_BIN)

$(OBJ_DIR)/free.o: src/user/free.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(FREE_BIN): $(OBJ_DIR)/free.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/free.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/free.elf $(FREE_BIN)

$(OBJ_DIR)/uptime.o: src/user/uptime.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UPTIME_BIN): $(OBJ_DIR)/uptime.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/uptime.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/uptime.elf $(UPTIME_BIN)

$(OBJ_DIR)/kill.o: src/user/kill.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(KILL_BIN): $(OBJ_DIR)/kill.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/kill.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/kill.elf $(KILL_BIN)

$(OBJ_DIR)/cp.o: src/user/cp.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CP_BIN): $(OBJ_DIR)/cp.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/cp.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/cp.elf $(CP_BIN)

$(OBJ_DIR)/rm.o: src/user/rm.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(RM_BIN): $(OBJ_DIR)/rm.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/rm.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/rm.elf $(RM_BIN)

# [inos-build-kernel] TAR.BIN — the in-OS source-tar extractor.  Port-style
# (main(argc, argv) via the crt0 inside libc.a), like the textutils ports;
# the same source is also the host test's subject (src/host/tar_test.c).
$(OBJ_DIR)/tar.o: src/user/tar.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(TAR_BIN): $(OBJ_DIR)/tar.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/tar.elf $(OBJ_DIR)/tar.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/tar.elf $(TAR_BIN)

$(OBJ_DIR)/mv.o: src/user/mv.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MV_BIN): $(OBJ_DIR)/mv.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/mv.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/mv.elf $(MV_BIN)

$(OBJ_DIR)/touch.o: src/user/touch.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(TOUCH_BIN): $(OBJ_DIR)/touch.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/touch.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/touch.elf $(TOUCH_BIN)

$(OBJ_DIR)/wc.o: src/user/wc.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# wc links against the Phase-2 sysroot assembly (crt0 + libc.a): stdio,
# getopt_long, error, stat stubs all resolve from the archive.
$(WC_BIN): $(OBJ_DIR)/wc.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/wc.elf $(OBJ_DIR)/wc.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/wc.elf $(WC_BIN)

$(OBJ_DIR)/head_gnu.o: src/user/head_gnu.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# The GNU head port links against the Phase-2/3 sysroot assembly, named
# HEDGNU.BIN so it does not collide with the shell's legacy HEAD.BIN.
$(HEDGNU_BIN): $(OBJ_DIR)/head_gnu.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/hedgnu.elf $(OBJ_DIR)/head_gnu.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/hedgnu.elf $(HEDGNU_BIN)

$(OBJ_DIR)/cut_gnu.o: src/user/cut_gnu.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# The GNU cut port links against the Phase-2/3 sysroot assembly (crt0 +
# libc.a: stdio, getopt_long, error all resolve from the archive).
$(CUT_BIN): $(OBJ_DIR)/cut_gnu.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/cut.elf $(OBJ_DIR)/cut_gnu.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/cut.elf $(CUT_BIN)

# The textutils batch-1 ports (tr, paste, fold, nl) link the same way.
define TU21_PORT_RULE
$(OBJ_DIR)/$(1)_gnu.o: src/user/$(1)_gnu.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $$< -o $$@

$(2): $(OBJ_DIR)/$(1)_gnu.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/$(1).elf $(OBJ_DIR)/$(1)_gnu.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/$(1).elf $(2)
endef

$(eval $(call TU21_PORT_RULE,tr,$(TR_BIN)))
$(eval $(call TU21_PORT_RULE,paste,$(PASTE_BIN)))
$(eval $(call TU21_PORT_RULE,fold,$(FOLD_BIN)))
$(eval $(call TU21_PORT_RULE,nl,$(NL_BIN)))
$(eval $(call TU21_PORT_RULE,comm,$(COMM_BIN)))
$(eval $(call TU21_PORT_RULE,tsort,$(TSORT_BIN)))
$(eval $(call TU21_PORT_RULE,expand,$(EXPAND_BIN)))
$(eval $(call TU21_PORT_RULE,unexpand,$(UNEXPAND_BIN)))
$(eval $(call TU21_PORT_RULE,cksum,$(CKSUM_BIN)))
$(eval $(call TU21_PORT_RULE,md5sum,$(MD5SUM_BIN)))
$(eval $(call TU21_PORT_RULE,tac,$(TAC_BIN)))
$(eval $(call TU21_PORT_RULE,cmp,$(CMP_BIN)))

# --- GNU nano 8.7 (multi-object port; objects named nano_*) -------------
# -DHAVE_CONFIG_H + -Isrc/user/nano make the port's pinned config.h the one
# weirdness: nano's definitions.h includes <config.h> only when told to.
$(OBJ_DIR)/nano_%.o: src/user/nano/%.c src/user/nano/config.h src/user/nano/revision.h src/user/nano/curses.h src/user/nano/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/nano -DHAVE_CONFIG_H -c $< -o $@

$(NANO_BIN): $(NANO_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/nano.elf $(NANO_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/nano.elf $(NANO_BIN)

# --- xcrypt: the roguelike on the curses port (GB-036) -------------------
# An original compact roguelike rendered through hb_curses.c (the curses
# shim nano ships), tiling as a desktop terminal window.  Board logic is
# host-testable (src/host/xcrypt_test.c).  Links the curses shim + libc.a.
$(OBJ_DIR)/xcrypt.o: src/user/rogue/xcrypt.c src/user/nano/curses.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/nano -c $< -o $@
$(XCRYPT_BIN): $(OBJ_DIR)/xcrypt.o $(OBJ_DIR)/nano_hb_curses.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/xcrypt.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/xcrypt.elf $(XCRYPT_BIN)

# --- NES emulator (GB-035) ---------------------------------------------
$(OBJ_DIR)/nescore.o: src/user/nes/nes.c src/user/nes/nes.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/nes -c $< -o $@
$(OBJ_DIR)/nesmain.o: src/user/nes/main.c src/user/nes/nes.h src/user/nes/homebrew_rom.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/nes -c $< -o $@
$(NES_BIN): $(OBJ_DIR)/nesmain.o $(OBJ_DIR)/nescore.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/nes.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/nes.elf $(NES_BIN)

# --- GNU sed 4.8 (multi-object GNU port; objects named sed_/sedg_) ------
$(OBJ_DIR)/sed_%.o: src/user/sed/%.c src/user/sed/config.h src/user/sed/*.h src/user/sed/gnulib/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/sed -Isrc/user/sed/gnulib -c $< -o $@

$(OBJ_DIR)/sedg_%.o: src/user/sed/gnulib/%.c src/user/sed/config.h src/user/sed/*.h src/user/sed/gnulib/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/sed -Isrc/user/sed/gnulib -c $< -o $@

$(SED_BIN): $(SED_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/sed.elf $(SED_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/sed.elf $(SED_BIN)

# --- GNU make 4.4.1 (multi-object port; objects named make_*) -----------
# -DHAVE_CONFIG_H mirrors the configure build (config.h is only pulled by
# the sources that #ifdef HAVE_CONFIG_H first, e.g. lib/glob.c).
$(OBJ_DIR)/make_%.o: src/user/make/%.c src/user/make/config.h src/user/make/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DHAVE_CONFIG_H -Isrc/user/make -c $< -o $@

$(MAKE_BIN): $(MAKE_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/make.elf $(MAKE_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/make.elf $(MAKE_BIN)

# --- GNU mtools 4.0.49 (multi-object port; objects named mtools_*) -------
# Shared FAT/image engine, transcribed byte-identical from upstream except
# config.h (the pinned port config; HobbyOS glue gated by HOBBYOS_TARGET).
# -DSYSCONFDIR=\"/etc\" mirrors upstream's build-time define; no mtools.conf
# exists in the OS root, so read_config() parses nothing and -i/-c drive the
# recipes exactly as the acceptance requires.
$(OBJ_DIR)/mtools_%.o: src/user/mtools/%.c src/user/mtools/config.h src/user/mtools/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/mtools -DHAVE_CONFIG_H -DHOBBYOS_TARGET -DSYSCONFDIR=\"/etc\" -c $< -o $@

# One tiny main() per tool (mtools_port.c, compiled with the tool selected)
# replaces upstream's multicall mtools.c; each .BIN links the shared engine.
# Port-main objects use the mtoolsm_ prefix so they cannot collide with the
# engine objects above (mcopy.o would otherwise map to mtools_mcopy.o twice).
define MTOOLS_PORT_RULE
$(OBJ_DIR)/mtoolsm_$(1).o: src/user/mtools/mtools_port.c src/user/mtools/config.h src/user/mtools/*.h $(USER_HDRS)
	@mkdir -p $$(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/mtools -DHAVE_CONFIG_H -DHOBBYOS_TARGET -DMTOOLS_TOOL_$(1) -DSYSCONFDIR=\"/etc\" -c $$< -o $$@

$(OBJ_DIR)/$(1).bin: $(OBJ_DIR)/mtoolsm_$(1).o $(MTOOLS_ENGINE_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/$(1).elf $(OBJ_DIR)/mtoolsm_$(1).o $(MTOOLS_ENGINE_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/$(1).elf $$@
endef
$(eval $(call MTOOLS_PORT_RULE,mcopy))
$(eval $(call MTOOLS_PORT_RULE,mdel))
$(eval $(call MTOOLS_PORT_RULE,mmd))

# --- GNU dosfstools 4.2 mkfs.fat (MKFSFAT.BIN; -C is ftruncate-only) -----
# Objects named mkfs_<src>; config.h is -include'd so the transcribed
# sources stay byte-identical to third_party/dosfstools-4.2/src/.  The
# -Wno-* flags mirror upstream's own src/Makefile.am AM_CFLAGS.
$(OBJ_DIR)/mkfs_%.o: src/user/mkfs/%.c src/user/mkfs/config.h src/user/mkfs/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Isrc/user/mkfs -include src/user/mkfs/config.h \
	      -Wno-sign-compare -Wno-missing-field-initializers -c $< -o $@

$(MKFSFAT_BIN): $(MKFSFAT_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/mkfsfat.elf $(MKFSFAT_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/mkfsfat.elf $(MKFSFAT_BIN)

$(OBJ_DIR)/wc_test.o: src/user/wc_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(WCTEST_BIN): $(OBJ_DIR)/wc_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/wc_test.elf $(OBJ_DIR)/wc_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/wc_test.elf $(WCTEST_BIN)

$(OBJ_DIR)/cut_test.o: src/user/cut_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(CUTTEST_BIN): $(OBJ_DIR)/cut_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/cut_test.elf $(OBJ_DIR)/cut_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/cut_test.elf $(CUTTEST_BIN)

# In-OS acceptance tests for the batch-1 ports.
define TU21_TEST_RULE
$(OBJ_DIR)/$(1)_test.o: src/user/$(1)_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $$< -o $$@

$(2): $(OBJ_DIR)/$(1)_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/$(1)_test.elf $(OBJ_DIR)/$(1)_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/$(1)_test.elf $(2)
endef

$(eval $(call TU21_TEST_RULE,tr,$(TRTEST_BIN)))
$(eval $(call TU21_TEST_RULE,paste,$(PASTE_T_BIN)))
$(eval $(call TU21_TEST_RULE,fold,$(FOLDTEST_BIN)))
$(eval $(call TU21_TEST_RULE,nl,$(NLTEST_BIN)))
$(eval $(call TU21_TEST_RULE,comm,$(COMMTEST_BIN)))
$(eval $(call TU21_TEST_RULE,tsort,$(TSORT_T_BIN)))
$(eval $(call TU21_TEST_RULE,expand,$(EXPAND_T_BIN)))
$(eval $(call TU21_TEST_RULE,unexpand,$(UNEXPAND_T_BIN)))
$(eval $(call TU21_TEST_RULE,fatbig,$(FATBIG_T_BIN)))
$(eval $(call TU21_TEST_RULE,stage4k,$(STAGE4K_T_BIN)))
$(eval $(call TU21_TEST_RULE,lfn,$(LFN_T_BIN)))
$(eval $(call TU21_TEST_RULE,cksum,$(CKSUM_T_BIN)))
$(eval $(call TU21_TEST_RULE,md5sum,$(MD5SUM_T_BIN)))
$(eval $(call TU21_TEST_RULE,tac,$(TACTEST_BIN)))
$(eval $(call TU21_TEST_RULE,cmp,$(CMPTEST_BIN)))

$(OBJ_DIR)/regex_test.o: src/user/regex_test.c src/user/regex_test_cases.h $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# In-OS acceptance test for the sysroot GNU regex.  Its expectation table
# (src/user/regex_test_cases.h) is re-verified against glibc by the host
# suite, so a pass on both sides means GNU-identical behavior.
$(REGTEST_BIN): $(OBJ_DIR)/regex_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/regex_test.elf $(OBJ_DIR)/regex_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/regex_test.elf $(REGTEST_BIN)

$(OBJ_DIR)/sed_test.o: src/user/sed_test.c src/user/sed_test_cases.h $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# In-OS acceptance test for the ported GNU sed: golden cases generated by
# tools/gen_sed_tests.py from native GNU sed 4.8 (the same sources the port
# vendors), checked for byte-exact stdout, exit status, and file effects.
# --- GNU grep 2.5.4 (multi-object GNU port; objects named grep_/grepg_) --
# The port's own headers (-I first) must win over src/libc/include: grep
# 2.5.4 matches with the regex.c it ships, not the sysroot's.
$(OBJ_DIR)/grep_%.o: src/user/grep/%.c src/user/grep/config.h src/user/grep/*.h src/user/grep/gnulib/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) -Isrc/user/grep -Isrc/user/grep/gnulib $(USER_CFLAGS) -DHAVE_CONFIG_H -c $< -o $@

$(OBJ_DIR)/grepg_%.o: src/user/grep/gnulib/%.c src/user/grep/config.h src/user/grep/*.h src/user/grep/gnulib/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) -Isrc/user/grep -Isrc/user/grep/gnulib $(USER_CFLAGS) -DHAVE_CONFIG_H -c $< -o $@

$(GREP_BIN): $(GREP_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/grep.elf $(GREP_OBJS) $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/grep.elf $(GREP_BIN)

$(SEDTEST_BIN): $(OBJ_DIR)/sed_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/sed_test.elf $(OBJ_DIR)/sed_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/sed_test.elf $(SEDTEST_BIN)

# In-OS acceptance test for the ported GNU grep: golden cases generated by
# tools/gen_grep_tests.py from native GNU grep 2.5.4 (the same sources the
# port vendors); GREP.BIN itself is built by the rules below.
$(OBJ_DIR)/grep_test.o: src/user/grep_test.c src/user/grep_test_cases.h $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(GREPTEST_BIN): $(OBJ_DIR)/grep_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/grep_test.elf $(OBJ_DIR)/grep_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/grep_test.elf $(GREPTEST_BIN)

$(OBJ_DIR)/pipeprobe.o: src/user/pipeprobe.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(OBJ_DIR)/subprobe.o: src/user/subprobe.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SUBPRB_BIN): $(OBJ_DIR)/subprobe.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/subprobe.elf $(OBJ_DIR)/subprobe.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/subprobe.elf $(SUBPRB_BIN)

$(PIPEPROBE_BIN): $(OBJ_DIR)/pipeprobe.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/pipeprobe.elf $(OBJ_DIR)/pipeprobe.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/pipeprobe.elf $(PIPEPROBE_BIN)

$(OBJ_DIR)/head_test.o: src/user/head_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(HEDTEST_BIN): $(OBJ_DIR)/head_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/head_test.elf $(OBJ_DIR)/head_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/head_test.elf $(HEDTEST_BIN)

$(OBJ_DIR)/tail_gnu.o: src/user/tail_gnu.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# The GNU tail port links against the Phase-2/3 sysroot assembly, named
# TAILGN.BIN to stay clear of the shell's legacy TAIL.BIN.
$(TAILGN_BIN): $(OBJ_DIR)/tail_gnu.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/tail_gnu.elf $(OBJ_DIR)/tail_gnu.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/tail_gnu.elf $(TAILGN_BIN)

$(OBJ_DIR)/tail_test.o: src/user/tail_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(TAILTEST_BIN): $(OBJ_DIR)/tail_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/tail_test.elf $(OBJ_DIR)/tail_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/tail_test.elf $(TAILTEST_BIN)

# Phase 3 (processes): PROCCHLD.BIN is the exec target, PROCTEST.BIN the
# fork/exec/waitpid acceptance test.
$(OBJ_DIR)/proc_child.o: src/user/proc_child.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(PROCCHLD_BIN): $(OBJ_DIR)/proc_child.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/proc_child.elf $(OBJ_DIR)/proc_child.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/proc_child.elf $(PROCCHLD_BIN)

$(OBJ_DIR)/proc_test.o: src/user/proc_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(PROCTEST_BIN): $(OBJ_DIR)/proc_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/proc_test.elf $(OBJ_DIR)/proc_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/proc_test.elf $(PROCTEST_BIN)

# P5 (S3/S5, docs/browser/p5-exec-signals-design.md): signal delivery +
# SIGRETURN acceptance, and the spawn_ex child probe.
$(OBJ_DIR)/spawnex_child.o: src/user/spawnex_child.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(SPAWNEXCHILD_BIN): $(OBJ_DIR)/spawnex_child.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/spawnex_child.elf $(OBJ_DIR)/spawnex_child.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/spawnex_child.elf $(SPAWNEXCHILD_BIN)

$(OBJ_DIR)/sig_test.o: src/user/sig_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(SIGTEST_BIN): $(OBJ_DIR)/sig_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/sig_test.elf $(OBJ_DIR)/sig_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/sig_test.elf $(SIGTEST_BIN)

$(OBJ_DIR)/lseek_test.o: src/user/lseek_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(LKSTEST_BIN): $(OBJ_DIR)/lseek_test.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/lseek_test.elf $(OBJ_DIR)/lseek_test.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/lseek_test.elf $(LKSTEST_BIN)

$(OBJ_DIR)/sort.o: src/user/sort.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SORT_BIN): $(OBJ_DIR)/sort.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/sort.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/sort.elf $(SORT_BIN)

$(OBJ_DIR)/uniq.o: src/user/uniq.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(UNIQ_BIN): $(OBJ_DIR)/uniq.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/uniq.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/uniq.elf $(UNIQ_BIN)

$(OBJ_DIR)/ping.o: src/user/ping.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(PING_BIN): $(OBJ_DIR)/ping.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/ping.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/ping.elf $(PING_BIN)

$(OBJ_DIR)/nc.o: src/user/nc.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(NC_BIN): $(OBJ_DIR)/nc.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/nc.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/nc.elf $(NC_BIN)

$(OBJ_DIR)/ifconfig.o: src/user/ifconfig.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(IFCONFIG_BIN): $(OBJ_DIR)/ifconfig.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/ifconfig.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/ifconfig.elf $(IFCONFIG_BIN)

$(OBJ_DIR)/monitor.o: src/user/monitor.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MONITOR_BIN): $(OBJ_DIR)/monitor.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/monitor.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/monitor.elf $(MONITOR_BIN)

$(OBJ_DIR)/monitor_test.o: src/user/monitor_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MONITOR_TEST_BIN): $(OBJ_DIR)/monitor_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/monitor_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/monitor_test.elf $(MONITOR_TEST_BIN)

$(OBJ_DIR)/shell_test2.o: src/user/shell_test2.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHELL_TEST2_BIN): $(OBJ_DIR)/shell_test2.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/shtest2.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/shtest2.elf $(SHELL_TEST2_BIN)

$(OBJ_DIR)/mkdir.o: src/user/mkdir.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MKDIR_BIN): $(OBJ_DIR)/mkdir.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/mkdir.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/mkdir.elf $(MKDIR_BIN)

$(OBJ_DIR)/shell_test3.o: src/user/shell_test3.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHELL_TEST3_BIN): $(OBJ_DIR)/shell_test3.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/shtest3.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/shtest3.elf $(SHELL_TEST3_BIN)

$(OBJ_DIR)/pong.o: src/user/pong.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(PONG_BIN): $(OBJ_DIR)/pong.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/pong.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/pong.elf $(PONG_BIN)

$(OBJ_DIR)/millipede.o: src/user/millipede.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(MILLIPEDE_BIN): $(OBJ_DIR)/millipede.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/millipede.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/millipede.elf $(MILLIPEDE_BIN)

# --- DOOM (doomgeneric) — S0 native-track game port -------------------
# Full-screen software raycaster as a user-space process on the HobbyOS ABI.
# Engine = vendored doomgeneric (third_party/doomgeneric, GPLv2+); backend =
# src/user/doom/doomgeneric_hobbyos.c (VirtIO GPU + input).  Linked against
# the userland libc.a so stdio/printf/malloc resolve.  Installed on disk.img
# as DOOM.BIN (launcher slot 12, right after PONG/MILLIPED).  Needs a
# license-clean IWAD at runtime (see tools/fetch_doom_wad.sh + provenance).
DOOM_CFLAGS = $(USER_CFLAGS) -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
  -D_DEFAULT_SOURCE -include unistd.h -Isrc/user/doom -Ithird_party/doomgeneric
# Engine sources = the upstream doomgeneric SRC_DOOM set (self-contained; the
# platform backends and optional Allegro/OPL ports are deliberately excluded).
DOOM_ENGINE_SRC = third_party/doomgeneric/am_map.c third_party/doomgeneric/doomdef.c \
  third_party/doomgeneric/doomstat.c third_party/doomgeneric/dstrings.c \
  third_party/doomgeneric/d_event.c third_party/doomgeneric/d_items.c \
  third_party/doomgeneric/d_iwad.c third_party/doomgeneric/d_loop.c \
  third_party/doomgeneric/d_main.c third_party/doomgeneric/d_mode.c \
  third_party/doomgeneric/d_net.c third_party/doomgeneric/f_finale.c \
  third_party/doomgeneric/f_wipe.c third_party/doomgeneric/g_game.c \
  third_party/doomgeneric/hu_lib.c third_party/doomgeneric/hu_stuff.c \
  third_party/doomgeneric/info.c third_party/doomgeneric/i_cdmus.c \
  third_party/doomgeneric/i_endoom.c third_party/doomgeneric/i_joystick.c \
  third_party/doomgeneric/i_scale.c third_party/doomgeneric/i_sound.c \
  third_party/doomgeneric/i_system.c third_party/doomgeneric/i_timer.c \
  third_party/doomgeneric/memio.c third_party/doomgeneric/m_argv.c \
  third_party/doomgeneric/m_bbox.c third_party/doomgeneric/m_cheat.c \
  third_party/doomgeneric/m_config.c third_party/doomgeneric/m_controls.c \
  third_party/doomgeneric/m_fixed.c third_party/doomgeneric/m_menu.c \
  third_party/doomgeneric/m_misc.c third_party/doomgeneric/m_random.c \
  third_party/doomgeneric/p_ceilng.c third_party/doomgeneric/p_doors.c \
  third_party/doomgeneric/p_enemy.c third_party/doomgeneric/p_floor.c \
  third_party/doomgeneric/p_inter.c third_party/doomgeneric/p_lights.c \
  third_party/doomgeneric/p_map.c third_party/doomgeneric/p_maputl.c \
  third_party/doomgeneric/p_mobj.c third_party/doomgeneric/p_plats.c \
  third_party/doomgeneric/p_pspr.c third_party/doomgeneric/p_saveg.c \
  third_party/doomgeneric/p_setup.c third_party/doomgeneric/p_sight.c \
  third_party/doomgeneric/p_spec.c third_party/doomgeneric/p_switch.c \
  third_party/doomgeneric/p_telept.c third_party/doomgeneric/p_tick.c \
  third_party/doomgeneric/p_user.c third_party/doomgeneric/r_bsp.c \
  third_party/doomgeneric/r_data.c third_party/doomgeneric/r_draw.c \
  third_party/doomgeneric/r_main.c third_party/doomgeneric/r_plane.c \
  third_party/doomgeneric/r_segs.c third_party/doomgeneric/r_sky.c \
  third_party/doomgeneric/r_things.c third_party/doomgeneric/sha1.c \
  third_party/doomgeneric/sounds.c third_party/doomgeneric/statdump.c \
  third_party/doomgeneric/st_lib.c third_party/doomgeneric/st_stuff.c \
  third_party/doomgeneric/s_sound.c third_party/doomgeneric/tables.c \
  third_party/doomgeneric/v_video.c third_party/doomgeneric/wi_stuff.c \
  third_party/doomgeneric/w_checksum.c third_party/doomgeneric/w_file.c \
  third_party/doomgeneric/w_main.c third_party/doomgeneric/w_wad.c \
  third_party/doomgeneric/z_zone.c third_party/doomgeneric/w_file_stdc.c \
  third_party/doomgeneric/i_input.c third_party/doomgeneric/i_video.c \
  third_party/doomgeneric/doomgeneric.c third_party/doomgeneric/dummy.c
DOOM_ENGINE_OBJ = $(patsubst third_party/doomgeneric/%.c,$(OBJ_DIR)/doom_engine/%.o,$(DOOM_ENGINE_SRC))
$(OBJ_DIR)/doom_engine/%.o: third_party/doomgeneric/%.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)/doom_engine
	$(CC) $(DOOM_CFLAGS) -c $< -o $@
$(OBJ_DIR)/doom.o: src/user/doom/doom.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@
$(OBJ_DIR)/doom_hobbyos.o: src/user/doom/doomgeneric_hobbyos.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(DOOM_CFLAGS) -c $< -o $@
$(DOOM_BIN): $(OBJ_DIR)/doom.o $(OBJ_DIR)/doom_hobbyos.o $(DOOM_ENGINE_OBJ) $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/doom.elf \
	  $(OBJ_DIR)/doom.o $(OBJ_DIR)/doom_hobbyos.o $(DOOM_ENGINE_OBJ) \
	  $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/doom.elf $(DOOM_BIN)
# --- STARDUST (GB-038, original arcade dodge game) ---
$(OBJ_DIR)/stardust.o: src/user/stardust.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(STARDUST_BIN): $(OBJ_DIR)/stardust.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/stardust.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/stardust.elf $(STARDUST_BIN)

STARDUST_TEST = stardust_test_host
$(STARDUST_TEST): obj/host_stardust_test.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# --- New desktop applications: compile + link (see DESKTOP_APP_NAMES) ---
# Each app links the GUI toolkit and dialog libraries in addition to the
# standard libc + malloc. Binary lands on disk as /<NAME>.BIN (8.3).
define DESKTOP_APP_RULE
$(OBJ_DIR)/$(1).o: src/user/$(1).c $(USER_HDRS) $(USER_LIBC)
	@mkdir -p $$(OBJ_DIR)
	$$(CC) $$(USER_CFLAGS) -c $$< -o $$@

$(OBJ_DIR)/$(1).bin: $(OBJ_DIR)/$(1).o $$(GUI_APP_OBJS)
	$$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/$(1).elf $$^
	$$(OBJCOPY) -O binary $(OBJ_DIR)/$(1).elf $(OBJ_DIR)/$(1).bin
endef

$(foreach app,$(DESKTOP_APP_NAMES),$(eval $(call DESKTOP_APP_RULE,$(app))))

# --- xcalc: the X11 support library's reference application -------------
# Links the library (x11_xlib_*.o, see the rule above) instead of the GUI
# toolkit; binary lands on disk as /XCALC.BIN (8.3).
X11_LIB_OBJS = $(OBJ_DIR)/x11_xlib_display.o $(OBJ_DIR)/x11_xlib_window.o $(OBJ_DIR)/x11_xlib_draw.o $(OBJ_DIR)/x11_xlib_event.o

$(OBJ_DIR)/xcalc_main.o: src/user/x11/apps/xcalc/main.c src/user/x11/*.h src/user/x11/include/X11/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@

$(XCALC_BIN): $(OBJ_DIR)/xcalc_main.o $(X11_LIB_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/xcalc.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/xcalc.elf $(XCALC_BIN)

# --- antfarm: the X11 support library's second application --------------
# The xantfarm port (simulation, timers, XBM tiles and clip masks);
# binary lands on disk as /ANTFARM.BIN (8.3).
$(OBJ_DIR)/antfarm_main.o: src/user/x11/apps/antfarm/main.c src/user/x11/*.h src/user/x11/include/X11/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@

$(ANTFARM_BIN): $(OBJ_DIR)/antfarm_main.o $(X11_LIB_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/antfarm.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/antfarm.elf $(ANTFARM_BIN)

# --- xeyes: the X11 support library's third application -----------------
# The xeyes port (ellipse fills, pointer-paced pupil tracking); binary
# lands on disk as /XEYES.BIN (8.3).
$(OBJ_DIR)/xeyes_main.o: src/user/x11/apps/xeyes/main.c src/user/x11/*.h src/user/x11/include/X11/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@

$(XEYES_BIN): $(OBJ_DIR)/xeyes_main.o $(X11_LIB_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/xeyes.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/xeyes.elf $(XEYES_BIN)

# --- xnibbler: the X11 support library's fourth application -------------
# The original nibbler/Snake arcade game (clean-room implementation of the
# 1982-1990s X11 genre classic); board logic host-tested, entry point forks
# + pipes into xnibbler_main.  Linked against the X11 lib like the others.
$(OBJ_DIR)/xnibbler_main.o: src/user/x11/apps/xnibbler/main.c src/user/x11/*.h src/user/x11/include/X11/*.h $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(X11_INC) -c $< -o $@
$(XNIBBLER_BIN): $(OBJ_DIR)/xnibbler_main.o $(X11_LIB_OBJS) $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/xnibbler.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/xnibbler.elf $(XNIBBLER_BIN)

# --- P6.2 (browser.md §6): SQLite target port ---------------------------
# The pinned amalgamation (3.49.2.0) built for both arches with
# SQLITE_OS_OTHER=1 — src/user/sqlite_os.c is the whole OS port: one VFS
# ("hobby") over the libc file API + mutex methods over libpthread, so
# SQLITE_THREADSAFE=1 is real on this box.  WAL is compiled out initially
# (SQLITE_OMIT_WAL); sections are garbage-collected to keep SQLTEST.BIN
# well under the 1 MB program cap.  The extracted tree is gitignored and
# rebuilt from the checksummed zip on demand.
SQLITE_DIR = third_party/sqlite-amalgamation-3490200
SQLITE_DEFS = -DSQLITE_THREADSAFE=1 -DSQLITE_OS_OTHER=1 \
              -DSQLITE_OMIT_WAL -DSQLITE_OMIT_LOAD_EXTENSION \
              -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_CACHE_SIZE=512
SQLTEST_BIN = $(OBJ_DIR)/sqltest.bin
# GX fix (2026-10-04): relink every user binary when the linker script
# changes.  make cannot see the script through the $(LD) recipe, so a
# changed src/user/linker.ld silently left every existing .bin stale
# (observed: the x64 large-model .lbss fix did not propagate).
# .EXTRA_PREREQS (GNU make >= 4.3) keeps it out of $^ -- a plain
# prerequisite would be passed to ld.lld AS AN INPUT FILE by the
# recipes' $^.  Names undefined in this config expand to nothing.
$(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(SPAWN_TEST_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(GRAPHICS_TEST_BIN) $(WSRF_T_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(NETFIX_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(APPS_T_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(FPU_T_BIN) $(GFXBENCH_BIN) $(MATH_T_BIN) $(THRD_T_BIN) $(TLS_T_BIN) $(DNSTST_BIN) $(CXXSMOKE_BIN) $(CXX_T_BIN) $(CXX_RTTI_T_BIN) $(IPC_T_BIN) $(MMTEST_T_BIN) $(TORTURE_T_BIN) $(BIG_BIN) $(ICU_SMOKE_BIN) $(ERRNO_TEST_BIN) $(SOCK2TST_BIN) $(POLLTST_BIN) $(POLLTSTBRK_BIN) $(BLKTEST_BIN) $(RANDTST_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(BASENAME_BIN) $(DIRNAME_BIN) $(SEQ_BIN) $(EXPR_BIN) $(TESTGNU_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(NANO_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(PIPEPROBE_BIN) $(SUBPRB_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(FATBIG_T_BIN) $(STAGE4K_T_BIN) $(LFN_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(SIGTEST_BIN) $(SPAWNEXCHILD_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(DOOM_BIN) $(FILES_BIN) $(CALC_BIN) $(XCALC_BIN) $(ANTFARM_BIN) $(XEYES_BIN) $(XNIBBLER_BIN) $(CLOCK_BIN) $(SYSMON_BIN) $(HEX_BIN) $(TASKS_BIN) $(FIND_BIN) $(DIFF_BIN) $(NOTES_BIN) $(UNIT_BIN) $(WK1C_T_BIN) $(FILEDIALOG_ARROW_T_BIN) $(DIALOG_TEST_BIN) $(SQLTEST_BIN): .EXTRA_PREREQS += src/user/linker.ld $(XCRYPT_BIN)
$(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(SPAWN_TEST_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(GRAPHICS_TEST_BIN) $(WSRF_T_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(NETFIX_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(APPS_T_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(FPU_T_BIN) $(GFXBENCH_BIN) $(MATH_T_BIN) $(THRD_T_BIN) $(TLS_T_BIN) $(DNSTST_BIN) $(CXXSMOKE_BIN) $(CXX_T_BIN) $(CXX_RTTI_T_BIN) $(IPC_T_BIN) $(MMTEST_T_BIN) $(TORTURE_T_BIN) $(BIG_BIN) $(ICU_SMOKE_BIN) $(ERRNO_TEST_BIN) $(SOCK2TST_BIN) $(POLLTST_BIN) $(POLLTSTBRK_BIN) $(BLKTEST_BIN) $(RANDTST_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(BASENAME_BIN) $(DIRNAME_BIN) $(SEQ_BIN) $(EXPR_BIN) $(TESTGNU_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(NANO_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(PIPEPROBE_BIN) $(SUBPRB_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(FATBIG_T_BIN) $(STAGE4K_T_BIN) $(LFN_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(SIGTEST_BIN) $(SPAWNEXCHILD_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(STARDUST_BIN) $(FILES_BIN) $(CALC_BIN) $(XCALC_BIN) $(ANTFARM_BIN) $(XEYES_BIN) $(XNIBBLER_BIN) $(CLOCK_BIN) $(SYSMON_BIN) $(HEX_BIN) $(TASKS_BIN) $(FIND_BIN) $(DIFF_BIN) $(NOTES_BIN) $(UNIT_BIN) $(WK1C_T_BIN) $(FILEDIALOG_ARROW_T_BIN) $(DIALOG_TEST_BIN) $(SQLTEST_BIN): .EXTRA_PREREQS += src/user/linker.ld $(XCRYPT_BIN)


$(SQLITE_DIR)/sqlite3.c:
	cd third_party && sha256sum -c sqlite-amalgamation-3490200.zip.sums && \
	  unzip -oq sqlite-amalgamation-3490200.zip -d .

# Side effect of the extraction above: sqlite3.h lands next to sqlite3.c.
# Declared so a pristine tree resolves the header (make would otherwise stop
# with "No rule to make target ...sqlite3.h" on the first disk.img build).
$(SQLITE_DIR)/sqlite3.h: $(SQLITE_DIR)/sqlite3.c

$(OBJ_DIR)/sqlite3.o: $(SQLITE_DIR)/sqlite3.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Os -ffunction-sections -fdata-sections \
	  -Wno-unused-parameter $(SQLITE_DEFS) -I$(SQLITE_DIR) \
	  -c $< -o $@

$(OBJ_DIR)/sqlite_os.o: src/user/sqlite_os.c $(USER_HDRS) $(SQLITE_DIR)/sqlite3.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(SQLITE_DEFS) -I$(SQLITE_DIR) -c $< -o $@

$(OBJ_DIR)/sqlite_test.o: src/user/sqlite_test.c $(USER_HDRS) $(SQLITE_DIR)/sqlite3.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) $(SQLITE_DEFS) -I$(SQLITE_DIR) -c $< -o $@

$(SQLTEST_BIN): $(OBJ_DIR)/sqlite_test.o $(OBJ_DIR)/sqlite_os.o $(OBJ_DIR)/sqlite3.o $(OBJ_DIR)/libc.a
	$(LD) -T src/user/linker.ld -e _start --gc-sections -o $(OBJ_DIR)/sqltest.elf $(OBJ_DIR)/sqlite_test.o $(OBJ_DIR)/sqlite_os.o $(OBJ_DIR)/sqlite3.o $(OBJ_DIR)/libc.a
	$(OBJCOPY) -O binary $(OBJ_DIR)/sqltest.elf $(SQLTEST_BIN)

# GNU make port: stay out of the giant prerequisite lists above (they are
# merge-owner territory) and hang MAKE.BIN off linker.ld + the disk recipe
# with target-specific rules of its own.
$(MAKE_BIN): .EXTRA_PREREQS += src/user/linker.ld
disk.img: $(MAKE_BIN)

# [inos-build-kernel] the in-OS tar extractor rides the disk too (staged
# into /TOOLS by tools/selfhost-stage.sh --tools).
$(TAR_BIN): .EXTRA_PREREQS += src/user/linker.ld
disk.img: $(TAR_BIN)

# [inos-build-kernel] `make run` completeness (2026-10-15): every disk
# ships a browser and the in-OS build toolchain; each is built here when no
# cached copy exists (rules above; see the completeness block near the
# browser rules).
disk.img: browser-staged tools-staged

# The browser's TLS trust store is a disk input, not a build artefact: if the
# bundle is missing the disk must not be built silently-https-broken.
disk.img: certs/cacert.pem

disk.img: $(TARGET) $(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(SPAWN_TEST_BIN) $(GRAPHICS_TEST_BIN) $(WSRF_T_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(DIALOG_TEST_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(FPU_T_BIN) $(GFXBENCH_BIN) $(MATH_T_BIN) $(ERRNO_TEST_BIN) $(SOCK2TST_BIN) $(POLLTST_BIN) $(POLLTSTBRK_BIN) $(BLKTEST_BIN) $(RANDTST_BIN) $(DNSTST_BIN) $(NETFIX_BIN) $(CXXSMOKE_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(BASENAME_BIN) $(DIRNAME_BIN) $(SEQ_BIN) $(EXPR_BIN) $(TESTGNU_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(FATBIG_T_BIN) $(STAGE4K_T_BIN) $(LFN_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(SUBPRB_BIN) $(PIPEPROBE_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(STARDUST_BIN) $(FILEDIALOG_ARROW_T_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(DESKTOP_APP_BINS) $(XCALC_BIN) $(ANTFARM_BIN) $(XEYES_BIN) $(XNIBBLER_BIN) $(NANO_BIN) $(APPS_T_BIN) $(THRD_T_BIN) $(TLS_T_BIN) $(WK1C_T_BIN) $(CXX_T_BIN) $(CXX_RTTI_T_BIN) $(MMTEST_T_BIN) $(IPC_T_BIN) $(ICU_SMOKE_BIN) $(SQLTEST_BIN) $(SIGTEST_BIN) $(SPAWNEXCHILD_BIN) $(TORTURE_T_BIN) $(BIG_BIN) $(SELFHOST_BIN) $(MODE_FILE) $(XCRYPT_BIN) $(NES_BIN) $(DOOM_BIN)

# w2-mtools disk
disk.img: $(TARGET) $(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(SPAWN_TEST_BIN) $(GRAPHICS_TEST_BIN) $(WSRF_T_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(DIALOG_TEST_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(FPU_T_BIN) $(GFXBENCH_BIN) $(MATH_T_BIN) $(ERRNO_TEST_BIN) $(SOCK2TST_BIN) $(POLLTST_BIN) $(POLLTSTBRK_BIN) $(BLKTEST_BIN) $(RANDTST_BIN) $(DNSTST_BIN) $(NETFIX_BIN) $(CXXSMOKE_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(BASENAME_BIN) $(DIRNAME_BIN) $(SEQ_BIN) $(EXPR_BIN) $(TESTGNU_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(FATBIG_T_BIN) $(STAGE4K_T_BIN) $(LFN_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(SUBPRB_BIN) $(PIPEPROBE_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(STARDUST_BIN) $(FILEDIALOG_ARROW_T_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(DESKTOP_APP_BINS) $(XCALC_BIN) $(ANTFARM_BIN) $(XEYES_BIN) $(XNIBBLER_BIN) $(NANO_BIN) $(APPS_T_BIN) $(THRD_T_BIN) $(TLS_T_BIN) $(WK1C_T_BIN) $(CXX_T_BIN) $(CXX_RTTI_T_BIN) $(MMTEST_T_BIN) $(IPC_T_BIN) $(ICU_SMOKE_BIN) $(SQLTEST_BIN) $(SIGTEST_BIN) $(SPAWNEXCHILD_BIN) $(TORTURE_T_BIN) $(BIG_BIN) $(MCOPY_BIN) $(MDEL_BIN) $(MMD_BIN) $(MODE_FILE) $(XCRYPT_BIN) # w2-mtools deps (union: MAKE_BIN + SELFHOST_BIN ride their own lines above)
disk.img: $(TARGET) $(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(SPAWN_TEST_BIN) $(GRAPHICS_TEST_BIN) $(WSRF_T_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(DIALOG_TEST_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(FPU_T_BIN) $(GFXBENCH_BIN) $(MATH_T_BIN) $(ERRNO_TEST_BIN) $(SOCK2TST_BIN) $(POLLTST_BIN) $(POLLTSTBRK_BIN) $(BLKTEST_BIN) $(RANDTST_BIN) $(DNSTST_BIN) $(NETFIX_BIN) $(CXXSMOKE_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(BASENAME_BIN) $(DIRNAME_BIN) $(SEQ_BIN) $(EXPR_BIN) $(TESTGNU_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(FATBIG_T_BIN) $(STAGE4K_T_BIN) $(LFN_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(SUBPRB_BIN) $(PIPEPROBE_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(STARDUST_BIN) $(FILEDIALOG_ARROW_T_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(DESKTOP_APP_BINS) $(XCALC_BIN) $(ANTFARM_BIN) $(XEYES_BIN) $(XNIBBLER_BIN) $(NANO_BIN) $(MKFSFAT_BIN) $(APPS_T_BIN) $(THRD_T_BIN) $(TLS_T_BIN) $(WK1C_T_BIN) $(CXX_T_BIN) $(CXX_RTTI_T_BIN) $(MMTEST_T_BIN) $(IPC_T_BIN) $(ICU_SMOKE_BIN) $(SQLTEST_BIN) $(SIGTEST_BIN) $(SPAWNEXCHILD_BIN) $(TORTURE_T_BIN) $(BIG_BIN) $(MODE_FILE) $(XCRYPT_BIN) # w2-mkfs deps

	# Startup disk: 1 GiB (user-requested floor; FAT16 tops out at 2 GiB).
	# mkfs.fat auto-picks the cluster size (32 KiB @ 1 GiB); the kernel
	# fat16.c layout math is BPB data-driven (bpb_sectors_per_cluster), so
	# no driver change is needed for the larger disk/clusters.
	dd if=/dev/zero of=disk.img bs=1M count=1024
	$(MKFS_FAT) -F 16 disk.img
	$(MMD) -i disk.img ::/EFI
	$(MMD) -i disk.img ::/EFI/BOOT
	$(MMD) -i disk.img ::/boot
	$(MMD) -i disk.img ::/home
	# CA roots for the browser's TLS stack (libcurl/mbedTLS): the driver pins
	# /CERTS/CA.PEM (WebKit/WebProcess/hobbyos/WK5WindowDriver.cpp).  Without
	# this every https:// navigation fails at certificate verification and the
	# user sees the previous page with no explanation.
	$(MMD) -i disk.img ::/CERTS
	$(MCOPY) -i disk.img certs/cacert.pem ::/CERTS/CA.PEM
	$(MMD) -i disk.img ::/nfs
	$(MMD) -i disk.img ::/mnt
	$(MMD) -i disk.img ::/TESTDIR
	$(MMD) -i disk.img ::/tmp
	# Selfhost W2 (mtools port): the in-OS acceptance invokes the ported
	# build-tools at their frozen /TOOLS/ paths (Makefile.hobbyos sketch).
	$(MMD) -i disk.img ::/TOOLS
	# L3 (selfhost W-A): VFAT long-name build-payload fixture dirs (mtools-written).
	$(MMD) -i disk.img ::/src
	$(MMD) -i disk.img ::/src/kernel
	$(MMD) -i disk.img ::/src/user_include
	$(MMD) -i disk.img ::/src/user_include/graphics
	$(MMD) -i disk.img ::/third_party
	$(MMD) -i disk.img ::/third_party/libcxx-21.1.8
	$(MMD) -i disk.img ::/third_party/libcxx-21.1.8/src
	$(MMD) -i disk.img ::/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src
	$(MMD) -i disk.img ::/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx
	$(MMD) -i disk.img ::/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include
	$(MCOPY) -i disk.img tests/fixtures/E2E.TXT ::/E2E.TXT
	$(MCOPY) -i disk.img tests/fixtures/DRAGME.TXT ::/DRAGME.TXT
	$(MCOPY) -i disk.img bootloader/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY) -i disk.img bootloader/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
ifeq ($(ARCH),arm)
	# [inos-build-kernel] canonical ARM boot config from the repo file
	# (single source of truth; also what --src staging copies to
	# /SRC/limine.conf and what SRC.TAR carries).  Two entries: the boot
	# kernel + the previous one, which /BUILD_KERNEL.SH's install step
	# keeps at /boot/hobbyos.old.
	$(MCOPY) -i disk.img bootloader/limine-arm.conf ::/boot/limine.conf
	$(OBJCOPY) -O binary $(TARGET) hobbyos.bin
	$(MCOPY) -i disk.img hobbyos.bin ::/boot/hobbyos.bin
else
	@echo "timeout: 0" > obj/$(ARCH)/limine.conf
	@echo "default_entry: 1" >> obj/$(ARCH)/limine.conf
	@echo "" >> obj/$(ARCH)/limine.conf
	@echo "/HobbyOS (Intel x86_64)" >> obj/$(ARCH)/limine.conf
	@echo "protocol: multiboot1" >> obj/$(ARCH)/limine.conf
	@echo "path: boot():/boot/hobbyos.elf" >> obj/$(ARCH)/limine.conf
	$(MCOPY) -i disk.img obj/$(ARCH)/limine.conf ::/boot/limine.conf
	$(MCOPY) -i disk.img $(TARGET) ::/boot/hobbyos.elf
endif
	$(MCOPY) -i disk.img $(MEM_TEST_BIN) ::/MEMTEST.BIN
	$(MCOPY) -i disk.img $(FILE_IO_BIN) ::/FILEIO.BIN
	$(MCOPY) -i disk.img $(CONSOLE_BIN) ::/CONSOLE.BIN
	$(MCOPY) -i disk.img $(FORK_TEST_BIN) ::/FORKTEST.BIN
	$(MCOPY) -i disk.img $(HEAP_TEST_BIN) ::/HEAPTEST.BIN
	$(MCOPY) -i disk.img $(SPAWN_TEST_BIN) ::/SPAWN.BIN
	$(MCOPY) -i disk.img $(GRAPHICS_TEST_BIN) ::/GRAPHICS.BIN
	$(MCOPY) -i disk.img $(WSRF_T_BIN) ::/WSRF_T.BIN
	$(MCOPY) -i disk.img $(SMP_TEST_BIN) ::/SMPTEST.BIN
	$(MCOPY) -i disk.img $(PIPETEST_BIN) ::/PIPETEST.BIN
	$(MCOPY) -i disk.img $(NETTEST_BIN) ::/NETTEST.BIN
	$(MCOPY) -i disk.img $(TIMEOUT_BIN) ::/TIMEOUT.BIN
	$(MCOPY) -i disk.img $(NFSTEST_BIN) ::/NFSTEST.BIN
	$(MCOPY) -i disk.img $(DESKTOP_BIN) ::/DESKTOP.BIN
	@if [ -f obj/$(ARCH)/browser.bin ]; then \
	  echo "browser: obj/$(ARCH)/browser.bin (resolved by the browser-staged completeness rule; installed as ::/BROWSER.BIN)"; \
	  $(MCOPY) -i disk.img obj/$(ARCH)/browser.bin ::/BROWSER.BIN; \
	  $(MCOPY) -i disk.img tests/fixtures/browser/HOME.HTM ::/HOME.HTM; \
	  $(MCOPY) -i disk.img tests/fixtures/browser/TALL.HTM ::/TALL.HTM; \
	  [ -f "$(BROWSER_FONT)" ] || $(MAKE) --no-print-directory $(BROWSER_FONT); \
	  $(MCOPY) -i disk.img $(BROWSER_FONT) ::/DEJAVU.TTF; \
	else \
	  echo "browser: no browser.bin resolved (BROWSER_SKIP) - Apps menu ships without browser"; \
	fi
	$(MCOPY) -i disk.img $(EDITOR_BIN) ::/EDITOR.BIN
	$(MCOPY) -i disk.img $(EDITOR_T_BIN) ::/EDITOR_T.BIN
	$(MCOPY) -i disk.img $(APPS_T_BIN) ::/APPS_T.BIN
	$(MCOPY) -i disk.img $(FILEDIALOG_ARROW_T_BIN) ::/FILEDIAL.BIN
	$(MCOPY) -i disk.img $(DIALOG_TEST_BIN) ::/DIALOG_T.BIN
	$(MCOPY) -i disk.img $(PONG_T_BIN) ::/PONG_T.BIN
	$(MCOPY) -i disk.img $(STRESS_TEST_BIN) ::/STRESS.BIN
	$(MCOPY) -i disk.img $(FPU_T_BIN) ::/FPU_T.BIN
	$(MCOPY) -i disk.img $(GFXBENCH_BIN) ::/GFXBENCH.BIN
	$(MCOPY) -i disk.img $(MATH_T_BIN) ::/MATH_T.BIN
	$(MCOPY) -i disk.img $(ERRNO_TEST_BIN) ::/ERRTEST.BIN
	$(MCOPY) -i disk.img $(SOCK2TST_BIN) ::/SOCK2TST.BIN
	$(MCOPY) -i disk.img $(POLLTST_BIN) ::/POLLTST.BIN
	$(MCOPY) -i disk.img $(POLLTSTBRK_BIN) ::/POLLBRK.BIN
	$(MCOPY) -i disk.img $(BLKTEST_BIN) ::/BLKTEST.BIN
	$(MCOPY) -i disk.img $(RANDTST_BIN) ::/RANDTST.BIN
	$(MCOPY) -i disk.img $(DNSTST_BIN) ::/DNSTST.BIN
	$(MCOPY) -i disk.img $(NETFIX_BIN) ::/NETFIX.BIN
	$(MCOPY) -i disk.img $(CXXSMOKE_BIN) ::/CXXSMOKE.BIN
	$(MCOPY) -i disk.img $(THRD_T_BIN) ::/THRD_T.BIN
	$(MCOPY) -i disk.img $(TLS_T_BIN) ::/TLS_T.BIN
	$(MCOPY) -i disk.img $(WK1C_T_BIN) ::/WK1C_T.BIN
	$(MCOPY) -i disk.img $(CXX_T_BIN) ::/CXX_T.BIN
	$(MCOPY) -i disk.img $(CXX_RTTI_T_BIN) ::/RTTI_T.BIN
	$(MCOPY) -i disk.img $(IPC_T_BIN) ::/IPC_T.BIN
	$(MCOPY) -i disk.img $(MMTEST_T_BIN) ::/MMTEST.BIN
	$(MCOPY) -i disk.img $(SQLTEST_BIN) ::/SQLTEST.BIN
	$(MCOPY) -i disk.img $(TORTURE_T_BIN) ::/TORTURE.BIN
	$(MCOPY) -i disk.img $(BIG_BIN) ::/BIG.BIN
	$(MCOPY) -i disk.img $(HELLO_BIN) ::/HELLO.BIN
	$(MCOPY) -i disk.img $(SELFHOST_BIN) ::/SELFHOST.BIN
	$(MCOPY) -i disk.img $(SH_BIN) ::/SH.BIN
	$(MCOPY) -i disk.img $(LS_BIN) ::/LS.BIN
	$(MCOPY) -i disk.img $(CAT_BIN) ::/CAT.BIN
	$(MCOPY) -i disk.img $(GREP_BIN) ::/GREP.BIN
	$(MCOPY) -i disk.img $(LESS_BIN) ::/LESS.BIN
	$(MCOPY) -i disk.img $(TAIL_BIN) ::/TAIL.BIN
	$(MCOPY) -i disk.img $(HEAD_BIN) ::/HEAD.BIN
	$(MCOPY) -i disk.img $(SHELL_TEST_BIN) ::/SHTEST.BIN
	$(MCOPY) -i disk.img $(PS_BIN) ::/PS.BIN
	$(MCOPY) -i disk.img $(FREE_BIN) ::/FREE.BIN
	$(MCOPY) -i disk.img $(UPTIME_BIN) ::/UPTIME.BIN
	$(MCOPY) -i disk.img $(KILL_BIN) ::/KILL.BIN
	$(MCOPY) -i disk.img $(BASENAME_BIN) ::/BASENAME.BIN
	$(MCOPY) -i disk.img $(DIRNAME_BIN) ::/DIRNAME.BIN
	$(MCOPY) -i disk.img $(SEQ_BIN) ::/SEQ.BIN
	$(MCOPY) -i disk.img $(EXPR_BIN) ::/EXPR.BIN
	$(MCOPY) -i disk.img $(TESTGNU_BIN) ::/TEST.BIN
	$(MCOPY) -i disk.img $(CP_BIN) ::/CP.BIN
	$(MCOPY) -i disk.img $(RM_BIN) ::/RM.BIN
	$(MCOPY) -i disk.img $(MV_BIN) ::/MV.BIN
	$(MCOPY) -i disk.img $(TOUCH_BIN) ::/TOUCH.BIN
	$(MCOPY) -i disk.img $(WC_BIN) ::/WC.BIN
	$(MCOPY) -i disk.img $(SED_BIN) ::/SED.BIN
	$(MCOPY) -i disk.img $(MAKE_BIN) ::/MAKE.BIN
	$(MCOPY) -i disk.img $(HEDGNU_BIN) ::/HEDGNU.BIN
	$(MCOPY) -i disk.img $(TAILGN_BIN) ::/TAILGN.BIN
	$(MCOPY) -i disk.img $(CUT_BIN) ::/CUT.BIN
	$(MCOPY) -i disk.img $(TR_BIN) ::/TR.BIN
	$(MCOPY) -i disk.img $(PASTE_BIN) ::/PASTE.BIN
	$(MCOPY) -i disk.img $(FOLD_BIN) ::/FOLD.BIN
	$(MCOPY) -i disk.img $(NL_BIN) ::/NL.BIN
	$(MCOPY) -i disk.img $(COMM_BIN) ::/COMM.BIN
	$(MCOPY) -i disk.img $(TSORT_BIN) ::/TSORT.BIN
	$(MCOPY) -i disk.img $(EXPAND_BIN) ::/EXPAND.BIN
	$(MCOPY) -i disk.img $(UNEXPAND_BIN) ::/UNEXPAND.BIN
	$(MCOPY) -i disk.img $(CKSUM_BIN) ::/CKSUM.BIN
	$(MCOPY) -i disk.img $(MD5SUM_BIN) ::/MD5SUM.BIN
	$(MCOPY) -i disk.img $(PROCCHLD_BIN) ::/PROCCHLD.BIN
	$(MCOPY) -i disk.img $(SPAWNEXCHILD_BIN) ::/SPAWNEX.BIN
	$(MCOPY) -i disk.img $(SIGTEST_BIN) ::/SIG_T.BIN
	$(MCOPY) -i disk.img $(HEDTEST_BIN) ::/HEDTEST.BIN
	$(MCOPY) -i disk.img $(TAILTEST_BIN) ::/TAILTEST.BIN
	$(MCOPY) -i disk.img $(PROCTEST_BIN) ::/PROCTEST.BIN
	$(MCOPY) -i disk.img $(WCTEST_BIN) ::/WCTEST.BIN
	$(MCOPY) -i disk.img $(CUTTEST_BIN) ::/CUTTEST.BIN
	$(MCOPY) -i disk.img $(TRTEST_BIN) ::/TRTEST.BIN
	$(MCOPY) -i disk.img $(PASTE_T_BIN) ::/PASTE_T.BIN
	$(MCOPY) -i disk.img $(FOLDTEST_BIN) ::/FOLDTEST.BIN
	$(MCOPY) -i disk.img $(NLTEST_BIN) ::/NLTEST.BIN
	$(MCOPY) -i disk.img $(COMMTEST_BIN) ::/COMMTEST.BIN
	$(MCOPY) -i disk.img $(TSORT_T_BIN) ::/TSORT_T.BIN
	$(MCOPY) -i disk.img $(EXPAND_T_BIN) ::/EXPAND_T.BIN
	$(MCOPY) -i disk.img $(UNEXPAND_T_BIN) ::/UNEXPAND_T.BIN
	$(MCOPY) -i disk.img $(FATBIG_T_BIN) ::/FATBIG_T.BIN
	$(MCOPY) -i disk.img $(STAGE4K_T_BIN) ::/STAGE4K_T.BIN
	# L3 (selfhost W-A): VFAT long-name build-payload fixtures.  Content
	# markers must stay in sync with src/user/lfn_test.c.
	$(MCOPY) -i disk.img tests/fixtures/lfn/program_loader.c ::/src/kernel/program_loader.c
	$(MCOPY) -i disk.img tests/fixtures/lfn/window.h ::/src/user_include/graphics/window.h
	$(MCOPY) -i disk.img tests/fixtures/lfn/llvm-project-21.1.8.src-EXAMPLE.c ::/src/llvm-project-21.1.8.src-EXAMPLE.c
	$(MCOPY) -i disk.img tests/fixtures/lfn/upper_bound.h ::/third_party/libcxx-21.1.8/src/llvm-project-21.1.8.src/libcxx/include/upper_bound.h
	$(MCOPY) -i disk.img tests/fixtures/lfn/longfilename.txt ::/longfilename.txt
	$(MCOPY) -i disk.img tests/fixtures/lfn/longfilename2.txt ::/longfilename2.txt
	$(MCOPY) -i disk.img tests/fixtures/lfn/longfil~1.txt ::/longfil~1.txt
	$(MCOPY) -i disk.img $(LFN_T_BIN) ::/LFN_T.BIN
	$(MCOPY) -i disk.img $(CKSUM_T_BIN) ::/CKSUM_T.BIN
	$(MCOPY) -i disk.img $(MD5SUM_T_BIN) ::/MD5SUM_T.BIN
	$(MCOPY) -i disk.img $(TAC_BIN) ::/TAC.BIN
	$(MCOPY) -i disk.img $(TACTEST_BIN) ::/TACTEST.BIN
	$(MCOPY) -i disk.img $(CMP_BIN) ::/CMP.BIN
	$(MCOPY) -i disk.img $(CMPTEST_BIN) ::/CMPTEST.BIN
	$(MCOPY) -i disk.img $(REGTEST_BIN) ::/REGTEST.BIN
	$(MCOPY) -i disk.img $(SEDTEST_BIN) ::/SEDTEST.BIN
	$(MCOPY) -i disk.img $(GREPTEST_BIN) ::/GREPTEST.BIN
	$(MCOPY) -i disk.img $(SUBPRB_BIN) ::/SUBPRB.BIN
	$(MCOPY) -i disk.img $(PIPEPROBE_BIN) ::/PIPEPRB.BIN
	$(MCOPY) -i disk.img $(LKSTEST_BIN) ::/LKSTEST.BIN
	$(MCOPY) -i disk.img $(SORT_BIN) ::/SORT.BIN
	$(MCOPY) -i disk.img $(UNIQ_BIN) ::/UNIQ.BIN
	$(MCOPY) -i disk.img $(PING_BIN) ::/PING.BIN
	$(MCOPY) -i disk.img $(NC_BIN) ::/NC.BIN
	$(MCOPY) -i disk.img $(IFCONFIG_BIN) ::/IFCONFIG.BIN
	$(MCOPY) -i disk.img $(SHELL_TEST2_BIN) ::/SHTEST2.BIN
	$(MCOPY) -i disk.img $(MKDIR_BIN) ::/MKDIR.BIN
	$(MCOPY) -i disk.img $(SHELL_TEST3_BIN) ::/SHTEST3.BIN
	$(MCOPY) -i disk.img $(PONG_BIN) ::/PONG.BIN
	$(MCOPY) -i disk.img $(MILLIPEDE_BIN) ::/MILLIPED.BIN
	$(MCOPY) -i disk.img $(DOOM_BIN) ::/DOOM.BIN
	@if [ -f doom1.wad ]; then $(MCOPY) -i disk.img doom1.wad ::/DOOM1.WAD && echo "[disk] IWAD doom1.wad -> ::/DOOM1.WAD"; else echo "[disk] note: no doom1.wad (run tools/fetch_doom_wad.sh) — DOOM will lack an IWAD"; fi
	$(MCOPY) -i disk.img $(STARDUST_BIN) ::/STARDUST.BIN
	$(MCOPY) -i disk.img $(FILES_BIN) ::/FILES.BIN
	$(MCOPY) -i disk.img $(CALC_BIN) ::/CALC.BIN
	$(MCOPY) -i disk.img $(XCALC_BIN) ::/XCALC.BIN
	$(MCOPY) -i disk.img $(ANTFARM_BIN) ::/ANTFARM.BIN
	$(MCOPY) -i disk.img $(XEYES_BIN) ::/XEYES.BIN
	$(MCOPY) -i disk.img $(XNIBBLER_BIN) ::/XNIBBLER.BIN
	$(MCOPY) -i disk.img $(CLOCK_BIN) ::/CLOCK.BIN
	$(MCOPY) -i disk.img $(SYSMON_BIN) ::/SYSMON.BIN
	$(MCOPY) -i disk.img $(HEX_BIN) ::/HEX.BIN
	$(MCOPY) -i disk.img $(TASKS_BIN) ::/TASKS.BIN
	$(MCOPY) -i disk.img $(FIND_BIN) ::/FIND.BIN
	$(MCOPY) -i disk.img $(DIFF_BIN) ::/DIFF.BIN
	$(MCOPY) -i disk.img $(NOTES_BIN) ::/NOTES.BIN
	$(MCOPY) -i disk.img $(UNIT_BIN) ::/UNIT.BIN
	$(MCOPY) -i disk.img $(NANO_BIN) ::/NANO.BIN
	$(MCOPY) -i disk.img $(XCRYPT_BIN) ::/XCRYPT.BIN
	$(MCOPY) -i disk.img $(NES_BIN) ::/NES.BIN
	# Selfhost W2 (mtools port): in-OS /TOOLS copies of the ported
	# build-tools (8.3 names, the frozen F1 layout).
	$(MCOPY) -i disk.img $(MCOPY_BIN) ::/TOOLS/MCOPY.BIN
	$(MCOPY) -i disk.img $(MDEL_BIN) ::/TOOLS/MDEL.BIN
	$(MCOPY) -i disk.img $(MMD_BIN) ::/TOOLS/MMD.BIN

	# Selfhost W2 (mkfs port): in-OS MKFSFAT (F1 layout).
	$(MCOPY) -i disk.img $(MKFSFAT_BIN) ::/MKFSFAT.BIN
	# Selfhost W3 (tools): the in-OS toolchain on the startup image (frozen
	# F1 layout, selfhost.md §7.3).  tools/selfhost-stage.sh --tools stages
	# CLANG.BIN/LLD.BIN/LLVMAR.BIN/OBJCOPY.BIN from the wave-2 LLVM
	# distribution staging (obj/selfhost/dist-staging, or the persistent
	# third_party/llvm-21.1.8/dist-staging copy that survives `make clean`),
	# the ported build tools (MAKE/MCOPY/MDEL/MMD/MKFSFAT), and the RESDIR
	# header tree under /TOOLS — staged FAITHFULLY with long names (F8
	# decision, see docs/selfhost/llvm-bootstrap.md §6.2).  Soft dependency:
	# an absent dist-staging warns + skips, so trees without the LLVM
	# install still build a bootable image (build_kernel.sh then refuses
	# with instructions).  Override with STAGE_TOOLS=0 to skip.
ifneq ($(STAGE_TOOLS),0)
	tools/selfhost-stage.sh --tools disk.img
	# Selfhost W4 (src): the in-OS build-required source subset on the
	# startup image (frozen F2 /SRC flavor): kernel+user sources, headers,
	# Makefile.hobbyos, limine/EFI assets, the carried build products, and
	# the SELFHOST.RUN/SELFHOST.SH stage scripts.  tools/selfhost-stage.sh
	# --src; disable together with the tools step via STAGE_TOOLS=0.
	tools/selfhost-stage.sh --src disk.img
	# [inos-build-kernel] the source tarball + the in-OS build script:
	# SRC.TAR (ustar, assembled by tools/selfhost-src-tar.sh from the real
	# tree) lands at ::/SRC.TAR; build_kernel.sh at ::/BUILD_KERNEL.SH.
	# In-OS: `SH.BIN /BUILD_KERNEL.SH` extracts the tar and builds the
	# kernel from it (Makefile.hobbyos `all`) + assembles OUT.IMG.
	tools/selfhost-src-tar.sh obj/selfhost/SRC.TAR
	$(MCOPY) -i disk.img obj/selfhost/SRC.TAR ::/SRC.TAR
	$(MCOPY) -i disk.img src/user/selfhost/build_kernel.sh ::/BUILD_KERNEL.SH
endif
	$(MCOPY) -i disk.img $(MONITOR_BIN) ::/MONITOR.BIN
	$(MCOPY) -i disk.img $(MONITOR_TEST_BIN) ::/MONITORT.BIN
	$(MCOPY) -i disk.img $(ICU_SMOKE_BIN) ::/ICUSMK.BIN
	echo "HobbyOS Terminal Test File" > SHTEST.TXT
	echo "This is line number two." >> SHTEST.TXT
	echo "Line three is right here." >> SHTEST.TXT
	echo "Fourth line has some interesting keywords like hello world." >> SHTEST.TXT
	echo "Line five is the last line of this small test document." >> SHTEST.TXT
	touch TEST1.TXT TEST2.TXT TEST3.TXT TEST4.TXT
	echo "orange" > SORT.TXT
	echo "apple" >> SORT.TXT
	echo "orange" >> SORT.TXT
	$(MCOPY) -i disk.img SHTEST.TXT ::/SHTEST.TXT
	$(MCOPY) -i disk.img SHTEST.TXT ::/home/SHTEST.TXT
	$(MCOPY) -i disk.img TEST1.TXT ::/TEST1.TXT
	$(MCOPY) -i disk.img TEST2.TXT ::/TEST2.TXT
	$(MCOPY) -i disk.img TEST3.TXT ::/TEST3.TXT
	$(MCOPY) -i disk.img TEST4.TXT ::/TEST4.TXT
	$(MCOPY) -i disk.img SORT.TXT ::/SORT.TXT
	# GB-067 fixture: 4096 bytes, byte[i] = i % 251 (the pattern STAGE4K_T.BIN
	# byte-compares).  Staged HERE, by the host, because the DoD under test is
	# "a host-staged 4 KiB file reads back byte-exact in the guest" -- if the
	# guest wrote it, the test would be exercising the writer.  Until this
	# landed the fixture came from a hand-run `T3_STAGE` probe, so every CI
	# wave failed `[STAGE4K] FAIL open /STAGE4K.BIN` while passing locally
	# for whoever remembered to stage it.
	perl -e 'print chr($$_ % 251) for (0..4095)' > STAGE4K.BIN
	$(MCOPY) -i disk.img STAGE4K.BIN ::/STAGE4K.BIN
	rm -f SHTEST.TXT TEST1.TXT TEST2.TXT TEST3.TXT TEST4.TXT SORT.TXT STAGE4K.BIN

# Target to run the OS inside QEMU
# RUN_STAGE: optional staging hook, run AFTER disk.img is (re)built and BEFORE
# QEMU starts -- the exact contract of t3_tier.sh's T3_STAGE (GB-118).  A fixture
# (or wave manifest) the guest must see is placed on the image here, after the
# build that would have wiped it.  The recipe sets ROOT and hands the command to
# the shell via $$RUN_STAGE (GNU make would otherwise eat `$ROOT` -> `$R`+`OOT`
# in the value -- GB-118-V RED, 2026-10-28), the same `ROOT="$ROOT" sh -c "$T3_STAGE"`
# shape t3_tier.sh uses.  A failing stage is fatal rather than booting an image
# without the fixture:
#   RUN_STAGE='mcopy -o -i "$ROOT/disk.img" /tmp/pie_hello ::HELLO.BIN' make run
# Off by default -- an unset/empty RUN_STAGE changes nothing.
# The evidence rig shares this same hook (evidence_rig.py --stage), so the rig and
# the T3 tier put a fixture on the image by one mechanism, not two (GB-118).
run: $(TARGET) disk.img
	$(if $(RUN_STAGE),@echo "=== run: stage ==="; ROOT="$(CURDIR)" $(SHELL) -c "$$RUN_STAGE" || { echo "run: STAGE FAILED - refusing to boot a stale image"; exit 2; },)
	$(QEMU_CMD)

# GX (docs/graphics-accel.md §3-D5): probe this host for a working
# accelerated QEMU configuration and print which mode `auto` will pick.
# Bounded (each probe <= 7 s) and harmless without DISPLAY — it prints
# "no DISPLAY -> soft" and skips the live GL probes.  The probes are
# device-free -S starts, so they never touch disk.img and never run a guest.
.PHONY: gpu-check
gpu-check:
	@echo "== gpu-check: host GL probe (docs/graphics-accel.md §1 / §3-D5) =="
	@TO=""; if command -v timeout >/dev/null 2>&1; then TO=timeout; elif command -v gtimeout >/dev/null 2>&1; then TO=gtimeout; fi; \
	if [ -z "$$DISPLAY" ]; then \
	  echo "gpu-check: no DISPLAY -> soft (GL probes skipped)"; \
	elif [ -z "$$TO" ]; then \
	  echo "gpu-check: no timeout(1)/gtimeout on this host -> live probes skipped"; \
	else \
	  probe() { label="$$1"; shift; log=$$(mktemp); \
	    "$$TO" 7 "$$@" >"$$log" 2>&1; rc=$$?; \
	    if [ "$$rc" -eq 124 ]; then echo "  ok        $$label"; return 0; fi; \
	    echo "  excluded  $$label (rc=$$rc)"; sed -n '1,2p' "$$log" | sed 's/^/            /'; return 1; }; \
	  GLARM=0; GLX64=0; \
	  echo "-- ARM (qemu-system-aarch64) --"; \
	  if command -v qemu-system-aarch64 >/dev/null 2>&1; then \
	    probe "gtk,gl=on + virtio-gpu-gl-device (the GL config)" \
	      qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 1 -m 512M -accel tcg \
	      -display gtk,gl=on -device virtio-gpu-gl-device -S -monitor none -serial none -nodefaults && GLARM=1; \
	    probe "egl-headless,gl=on + virtio-gpu-gl-device (why it is excluded)" \
	      qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 1 -m 512M -accel tcg \
	      -display egl-headless,gl=on -device virtio-gpu-gl-device -S -monitor none -serial none -nodefaults || true; \
	    probe "-display none + virtio-gpu-gl-device (why headless stays soft)" \
	      qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 1 -m 512M -accel tcg \
	      -display none -device virtio-gpu-gl-device -S -monitor none -serial none -nodefaults || true; \
	    probe "gtk + virtio-gpu-device (soft fallback)" \
	      qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 1 -m 512M -accel tcg \
	      -display gtk -device virtio-gpu-device -S -monitor none -serial none -nodefaults || true; \
	  else echo "  (qemu-system-aarch64 not on PATH)"; fi; \
	  if [ "$$GLARM" = "1" ]; then \
	    echo "  => ARM verdict: GL OK — auto picks virtio-gpu-gl-device + gtk,gl=on for interactive desktop boots"; \
	  else \
	    echo "  => ARM verdict: GL unavailable — auto falls back to soft (virtio-gpu-device + plain display)"; \
	  fi; \
	  echo "-- x64 (qemu-system-x86_64) --"; \
	  if command -v qemu-system-x86_64 >/dev/null 2>&1; then \
	    probe "gtk,gl=on + virtio-gpu-gl-pci + -vga none (the GL config)" \
	      qemu-system-x86_64 -M q35 -m 512M -display gtk,gl=on -device virtio-gpu-gl-pci -vga none -S -monitor none -serial none -nodefaults && GLX64=1 || true; \
	  else echo "  (qemu-system-x86_64 not on PATH)"; fi; \
	  if [ "$$GLX64" = "1" ]; then \
	    echo "  => x64 verdict: GL OK — auto picks virtio-gpu-gl-pci + -vga none + gtk,gl=on for interactive desktop boots"; \
	  else \
	    echo "  => x64 verdict: gtk,gl=on unavailable — auto falls back to plain gtk (no virtio device; guest uses BGA/std-VGA)"; \
	  fi; \
	fi
	@if [ "$(GX_GPU_GL)" = "1" ]; then \
	  echo "gpu-check: this invocation (QEMU_GPU=$(QEMU_GPU) MODE=$(MODE)) resolves to GL -> -display $(GX_GPU_DISPLAY) + -device $(GX_GPU_DEV) $(GX_X64_NOTE_GL)"; \
	else \
	  echo "gpu-check: this invocation (QEMU_GPU=$(QEMU_GPU) MODE=$(MODE)) resolves to soft -> -display $(GX_GPU_DISPLAY) + -device $(GX_GPU_DEV) $(GX_X64_NOTE_SOFT)"; \
	fi

# Rebuild the boot disk image from scratch (fresh mkfs + full mcopy).
# Unit-test suites WRITE to the disk (fat16 tests create/move files), so a
# re-run in the same tree needs a fresh image or stateful failures appear
# (fat16 move-across-dirs).  Touch the kernel so the image recipe re-runs —
# the recipe itself does dd+mkfs, so no deletions are involved.
.PHONY: fresh_disk
fresh_disk: $(TARGET)
	touch $(TARGET)
	$(MAKE) disk.img

# Clean rule to remove build artifacts
clean:
	-pkill -f qemu-system
	rm -rf obj $(TARGET) hobbyos disk.img disk_guest.img *.bin *.elf *.log *.BIN *_test_host *.BIN_host
	rm -f actual_qemu.ppm

memtest: $(MEM_TEST_BIN)

fileio_test: $(FILE_IO_BIN) 

fork_test: $(FORK_TEST_BIN)

tests: memtest fileio_test fork_test
	@echo "All test programs compiled"

# GB-057: the T3 tier is a GATE, not a boot.  `make test` = policed ARM run
# via tools/t3_tier.sh: it fails on any FAIL line in the console and on a
# run that never reaches its halt line.  The raw invocation is still
# `make MODE=test run` (that is what the wrapper calls).
test:
	bash tools/t3_tier.sh arm

# Raw, unpoliced boots (diagnostics only — never cite these as a tier).
test_run_raw:
	$(MAKE) MODE=test run

# P8.2 (browser.md §6): the 30-minute soak — a dedicated combined run.  The
# whole standard wave runs while TORTURE.BIN loops its combined groups for a
# 28-minute wall cap; "System halt" lands when the soak ends.  Bound it:
#   timeout 2100 make soak
soak:
	$(MAKE) MODE=soak run

unit_tests:
	$(MAKE) MODE=unit_tests run

desktop_test_run:
	$(MAKE) MODE=desktop_test run

pong_test_run:
	$(MAKE) MODE=pong_test run

filedialog_test_run:
	$(MAKE) MODE=filedialog_test run

# WK-1 M3 (draft 0003): build the kernel in jsc (single-program) mode.
# Booting needs the fork-side minimal disk; see the fork's
# HobbyOS/continuation/README.md for the full runner.
jsc_run:
	$(MAKE) MODE=jsc run

# WK-3 (draft 0003): build the kernel in webproc (single-program) mode.
# Booting needs the fork-side minimal disk; see the fork's
# HobbyOS/continuation/README.md for the full runner.
webproc_run:
	$(MAKE) MODE=webproc run

# Self-hosting (docs/selfhost/acceptance.md §5.2): MODE=selfhost boots the
# in-OS acceptance build driver SELFHOST.BIN (no desktop) with stdout/stderr
# on the serial console.  Needs SELFHOST.BIN + /SRC/SELFHOST.RUN on the boot
# disk (tools/selfhost-stage.sh or the disk.img recipe stage them).
selfhost_run:
	$(MAKE) MODE=selfhost run

filedialog_test:
	python3 ./run_filedialog_test.py

desktop_test:
	python3 ./run_desktop_test.py

# Build everything `desktop_test` boots (kernel, userland apps, disk.img) without
# starting QEMU.  run_desktop_test.py runs this first so DESKTOP_TEST_TIMEOUT
# measures GUEST time only: the timeout is anchored at the harness start, and on
# a cold copy the build alone can exceed it -- the tier then reports "Timeout
# waiting for SCREENSHOT_READY" for a guest that never booted (linuxci,
# 2026-10-26: 120 s budget, still compiling ICU when it expired).
desktop_test_build:
	$(MAKE) MODE=desktop_test disk.img

desktop_apps_test:
	python3 ./run_desktop_apps_test.py

xcalc_test:
	python3 ./run_xcalc_test.py

antfarm_test:
	python3 ./run_antfarm_test.py

xeyes_test:
	python3 ./run_xeyes_test.py

files_nav_test:
	python3 ./run_files_nav_test.py

apps_test_run:
	$(MAKE) MODE=apps_test run

apps_test:
	python3 ./run_apps_test.py

# --- Host Compatibility Build Targets ---
HOST_CC = clang
HOST_CFLAGS = -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -DHOST_TEST

# GB-085 make-dependency fix: every host test object and binary that links
# compat (the shared filesystem-mock layer) must rebuild when compat.c or
# the shared scratch-dir header changes -- a stale obj/host_compat.o was the
# second cause of the recurring "phantom 11" failures (see GB-085).
HOST_TEST_COMMON_DEPS = src/host/compat.c src/host/ho_scratch.h

obj/host_%.o: src/host/%.c src/user_include/*.h src/user_include/graphics/*.h $(HOST_TEST_COMMON_DEPS)
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

obj/host_user_desktop.o: src/user/desktop.c src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Dmain=desktop_main -c $< -o $@

obj/host_user_%.o: src/user/%.c src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

obj/host_user_graphics_%.o: src/user/graphics/%.c src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# X11 support library, host build (tests run it against the mock fb).
obj/host_user_x11_%.o: src/user/x11/%.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -c $< -o $@

# xcalc: the ported X11 calculator -- core, layout, real pixels and the
# real entry point (fork + pipes into xcalc_main).
XCALC_TEST = xcalc_test_host
obj/host_xcalc_test.o: src/host/xcalc_test.c src/user/x11/apps/xcalc/main.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -Isrc/user/x11 -c $< -o $@
$(XCALC_TEST): obj/host_xcalc_test.o obj/host_user_x11_xlib_display.o obj/host_user_x11_xlib_window.o obj/host_user_x11_xlib_draw.o obj/host_user_x11_xlib_event.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# antfarm: the xantfarm port -- world setup, the simulation, the
# checkpoint format and the real entry point (fork + pipes into
# antfarm_main).
ANTFARM_TEST = antfarm_test_host
obj/host_antfarm_test.o: src/host/antfarm_test.c src/user/x11/apps/antfarm/main.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -Isrc/user/x11 -c $< -o $@
$(ANTFARM_TEST): obj/host_antfarm_test.o obj/host_user_x11_xlib_display.o obj/host_user_x11_xlib_window.o obj/host_user_x11_xlib_draw.o obj/host_user_x11_xlib_event.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# xeyes: the xeyes port -- the transform, the pupil math, real arc
# pixels and the real entry point (fork + pipes into xeyes_main).
XEYES_TEST = xeyes_test_host
obj/host_xeyes_test.o: src/host/xeyes_test.c src/user/x11/apps/xeyes/main.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -Isrc/user/x11 -c $< -o $@
$(XEYES_TEST): obj/host_xeyes_test.o obj/host_user_x11_xlib_display.o obj/host_user_x11_xlib_window.o obj/host_user_x11_xlib_draw.o obj/host_user_x11_xlib_event.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# xnibbler: the nibbler/Snake port -- the board logic (reset / step / turn /
# food / wall / self-collision), driven directly with no X or display.
XNIBBLER_TEST = xnibbler_test_host
obj/host_xnibbler_test.o: src/host/xnibbler_test.c src/user/x11/apps/xnibbler/main.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -Isrc/user/x11 -c $< -o $@
$(XNIBBLER_TEST): obj/host_xnibbler_test.o obj/host_user_x11_xlib_display.o obj/host_user_x11_xlib_window.o obj/host_user_x11_xlib_draw.o obj/host_user_x11_xlib_event.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

obj/host_x11_lib_test.o: src/host/x11_lib_test.c src/user/x11/*.h src/user/x11/include/X11/*.h src/user_include/*.h src/user_include/graphics/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(X11_INC) -Isrc/user/x11 -c $< -o $@

# X11 support library: the desktop protocol in both directions, the
# shadow/backing store and event decoding (src/user/x11/), plus the
# outbound byte capture and mock-fb pixel checks.
X11_LIB_TEST = x11_lib_test_host
$(X11_LIB_TEST): obj/host_x11_lib_test.o obj/host_user_x11_xlib_display.o obj/host_user_x11_xlib_window.o obj/host_user_x11_xlib_draw.o obj/host_user_x11_xlib_event.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

EDITOR_HOST = EDITOR.BIN_host

$(EDITOR_HOST): obj/host_user_editor.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

EDITOR_TEST_BIN = editor_test_host
$(EDITOR_TEST_BIN): obj/host_editor_test.o obj/host_user_desktop.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# Menu ordering + display-label unit tests (link desktop.c like editor_test).
DESKTOP_MENU_TEST = desktop_menu_test_host
$(DESKTOP_MENU_TEST): obj/host_desktop_menu_test.o obj/host_user_desktop.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# Desktop drag & drop plumbing + ESC ] R run-in-window helpers (desktop.c).
DESKTOP_DRAG_TEST = desktop_drag_test_host
$(DESKTOP_DRAG_TEST): obj/host_desktop_drag_test.o obj/host_user_desktop.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# S2 game-launcher /GAMES scan + menu ordering (games_scan.c, GB-039): pure
# classify/order host test; links host_compat for libc stubs only.
GAMES_SCAN_TEST = games_scan_test_host
$(GAMES_SCAN_TEST): obj/host_games_scan_test.o obj/host_user_games_scan.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# GB-045: GFXBENCH burst-per-flush stats host golden (gfx_stats.c is pure).
GFX_STATS_TEST = gfx_stats_test_host
$(GFX_STATS_TEST): obj/host_gfx_stats_test.o obj/host_user_gfx_stats.o
	$(HOST_CC) -o $@ $^

# Desktop keyboard input: Ctrl+letter control bytes and the software key
# auto-repeat (map_key_char/key_may_repeat/key_repeat_tick in desktop.c).
# The test includes src/user/desktop.c directly (file-scope state), so it
# must NOT link host_user_desktop.o (that would duplicate every symbol).
DESKTOP_INPUT_TEST = desktop_input_test_host
$(DESKTOP_INPUT_TEST): obj/host_desktop_input_test.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# Desktop terminal surface: the ESC ] V / ] S handshake and the character
# grid a curses program (nano) draws on (window.c wm_term_* + the CSI
# dispatch in desktop.c).  Also includes desktop.c directly, same reason.
DESKTOP_TERM_TEST = desktop_term_test_host
$(DESKTOP_TERM_TEST): obj/host_desktop_term_test.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# Desktop pixel-mode surface: the ESC ] X / ] G handshake, repair requests
# (ESC [ E), the wallpaper carve and pixel input -- what the X11 support
# library (src/user/x11/) rides on.  Includes desktop.c directly, same
# reason as the term test.
DESKTOP_PIXEL_TEST = desktop_pixel_test_host
$(DESKTOP_PIXEL_TEST): obj/host_desktop_pixel_test.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# nano <-> desktop integration: the curses shim (a separate host object,
# linked like a program would) driven against the real desktop terminal
# surface, over the wire protocol (hb_set_io transport).
NANO_TERM_TEST = nano_term_test_host
$(NANO_TERM_TEST): obj/host_nano_term_test.o obj/nano_hb_curses_host.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

# The shim compiled for the host (the port's own file, host build).
obj/nano_hb_curses_host.o: src/user/nano/hb_curses.c src/user/nano/curses.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/user/nano -c $< -o $@

# xcrypt: the roguelike on the curses port -- the board logic (dungeon gen,
# move, mob AI, combat, pickup, victory) driven directly, no curses needed.
XCRYPT_TEST = xcrypt_test_host
obj/host_xcrypt_test.o: src/host/xcrypt_test.c src/user/rogue/xcrypt.c src/user/nano/curses.h src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/user/nano -Isrc/user_include -DHOST_TEST -c $< -o $@
$(XCRYPT_TEST): obj/host_xcrypt_test.o obj/nano_hb_curses_host.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# The two desktop tests that #include src/user/desktop.c directly must
# rebuild when it (or the window layer) changes.
obj/host_desktop_input_test.o obj/host_desktop_term_test.o obj/host_nano_term_test.o obj/host_desktop_pixel_test.o: src/user/desktop.c src/user/graphics/window.c

# Cross-application host suite: every desktop app in one binary, in-process
# checks plus forked end-to-end runs on pipes. Single TU (includes the app
# sources), so it is built directly from src/host/apps_suite_test.c.
# Run from a scratch cwd: tasks/notes persist TODO.TXT / NOTES.TXT there.
APPS_SUITE_TEST = apps_suite_test_host
$(APPS_SUITE_TEST): src/host/apps_suite_test.c src/host/compat.c src/user/gui.c src/user/dialog.c src/user/filedialog.c $(HOST_TEST_COMMON_DEPS)
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/host/apps_suite_test.c src/host/compat.c src/user/gui.c src/user/dialog.c src/user/filedialog.c -o $@

# NFS protocol codecs against real nfsd fixtures (src/host/nfs_fixtures.h,
# regenerated with tools/capture_nfs_fixtures.py).  Single TU with the kernel
# codec source: no kernel dependencies.
NFS_PROTO_TEST = nfs_proto_test_host
$(NFS_PROTO_TEST): src/host/nfs_proto_test.c src/kernel/nfs_proto.c src/host/nfs_fixtures.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/nfs_proto.c src/host/nfs_proto_test.c -o $@

# GB-026 Linux fb ABI surface: pure single-TU host golden tests (T1/T4)
# for src/kernel/fb_ioctl.c, the device-free FBIOGET_VSCREENINFO/
# FSCREENINFO fill logic whose struct layouts + request numbers are locked
# against <linux/fb.h> on the host (the nfs_proto/ipc_proto/evdev pattern).
FB_IOCTL_TEST = fb_ioctl_test_host
$(FB_IOCTL_TEST): src/host/fb_ioctl_test.c src/kernel/fb_ioctl.c src/include/linux_fb.h src/include/display_mode.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/fb_ioctl.c src/host/fb_ioctl_test.c -o $@
# B8G8R8A8 framebuffer pixel packing + geometry lock (GB-025/026): T1 host
# golden for src/include/fb_pixel.h — the concrete byte order ([blue,green,red,alpha])
# and pixel/row offset math under fb_ioctl_test's struct-field checks
# (docs/linux-abi.md §9; single-header precedent like linux_input.h).
FB_PIXEL_TEST = fb_pixel_test_host
$(FB_PIXEL_TEST): src/host/fb_pixel_test.c src/include/fb_pixel.h src/include/display_mode.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/host/fb_pixel_test.c -o $@
# M2 device-surface contract (GB-033): T1/T4 host tie-together of the full
# Linux device-ABI surface a static-musl SDL fbcon+evdev app depends on —
# fb_var<->pixel byte-order consistency, smem geometry, FBIOGET_*/FBIOBLANK/
# EVIOCGVERSION vs <linux/{fb,input}.h>, gamepad-code space (docs §9).
# Links the device-free fb TUs (fb_ioctl.c + fb_mmap.c) with the test.
M2_CONTRACT_TEST = m2_dev_contract_test_host
$(M2_CONTRACT_TEST): src/host/m2_dev_contract_test.c src/kernel/fb_ioctl.c src/kernel/fb_mmap.c src/include/linux_fb.h src/include/fb_ioctl.h src/include/fb_mmap.h src/include/linux_input.h src/include/linux_joystick.h src/include/fb_pixel.h src/include/display_mode.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/fb_ioctl.c src/kernel/fb_mmap.c src/host/m2_dev_contract_test.c -o $@
# PS/2 -> evdev decode: pure single-TU host golden tests (T1) for
# src/kernel/evdev.c, the device-free translator shared with the real
# virtio-input driver (the nfs_proto/ipc_proto host-test precedent).
EVDEV_TEST = evdev_test_host
# GB-042 gamepad/joystick evdev event-code ABI lock: T1/T4 golden + differential
# for src/include/linux_joystick.h (face/bumper BTN_*, stick/trigger/d-pad ABS_*)
# vs <linux/input.h> / <linux/input-event-codes.h> when present — the axis/button
# register surface SDL's gamecontroller layer consumes (docs/linux-abi.md §9).
JOYPAD_TEST = joypad_test_host
$(JOYPAD_TEST): src/host/joypad_test.c src/include/linux_joystick.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/host/joypad_test.c -o $@
$(EVDEV_TEST): src/host/evdev_test.c src/kernel/evdev.c src/include/evdev.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/evdev.c src/host/evdev_test.c -o $@
# nanosleep/clock_gettime/gettimeofday parity + timer precision (GB-028): T4
# differential host test that RUNS real Linux clock_gettime/gettimeofday/
# nanosleep and asserts the numeric ABI (CLOCK_* values, timespec/timeval
# 16 B layout) + the timing behaviour the compat layer must match for SDL
# (CLOCK_MONOTONIC monotonicity, nanosleep never under-sleeps and stays
# within an overshoot tolerance, wall clock advances).
CLOCKABI_TEST = clockabi_test_host
$(CLOCKABI_TEST): src/host/clockabi_test.c
	$(HOST_CC) $(HOST_CFLAGS) src/host/clockabi_test.c -o $@

# GB-060 stale-owner lock steal: T1 host test that compiles the REAL x64 lock
# implementation and asserts the rule that makes the mechanism safe -- a lock with
# a live owner is never freed (no matter how long anyone waits), a lock whose
# owner is a recorded-halted CPU is freed and re-acquired normally, and a CPU
# never frees its own lock.
LOCKSTEAL_TEST = locksteal_test_host
$(LOCKSTEAL_TEST): src/host/locksteal_test.c src/kernel/arch/x64/locks.c src/include/lock.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/arch/x64/locks.c src/host/locksteal_test.c -o $@

# memfd_create + shared-memory ABI surface (GB-030): T4 differential test that
# RUNS the real Linux memfd_create/fcntl(F_ADD_SEALS)/mmap(MAP_SHARED) on the
# host and proves the concrete behaviour + numeric ABI the compat layer must
# match (syscall 319, MFD_* flags, F_ADD_SEALS/F_GET_SEALS, F_SEAL_*, and
# two-process MAP_SHARED write visibility after fork — the SDL-surface
# requirement).  Skips where memfd_create is unavailable (non-Linux).
MEMFD_TEST = memfd_test_host
$(MEMFD_TEST): src/host/memfd_test.c
	$(HOST_CC) $(HOST_CFLAGS) src/host/memfd_test.c -o $@

# Linux evdev / input ABI lock (GB-027): header-only T1/T4 golden test for
# src/include/linux_input.h (struct input_event/input_absinfo layout, EV_*, BTN_*,
# KEY_*, ABS_*, REL_* codes, EVIOCG* request numbers) vs <linux/input.h> when the
# Linux headers are present — the input half of the /dev/input/event* contract
# (docs/linux-abi.md §9).  Same single-header pattern as linux_fb.h + fb_ioctl.
LINUX_INPUT_TEST = linux_input_test_host
$(LINUX_INPUT_TEST): src/host/linux_input_test.c src/include/linux_input.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/host/linux_input_test.c -o $@
# GB-027 /dev/input/event* node logic: device-free T1/T4 golden + differential
# for src/kernel/input_node.c (event->Linux input_event translation, EVIOCG*
# ioctl route + arg size) vs <linux/input.h> when present — the read/probe
# half of the compat input node (docs/linux-abi.md §9).
INPUT_NODE_TEST = input_node_test_host
$(INPUT_NODE_TEST): src/host/input_node_test.c src/kernel/input_node.c src/include/input_node.h src/include/linux_input.h src/include/evdev.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/input_node.c src/host/input_node_test.c -o $@

# GB-051 audio assert: device-free compat audio-surface decision (GB-029) —
# /dev/dsp (OSS) open -> ENODEV + SDL dummy silent-safe path. Single-TU host
# golden (src/kernel/audio_stub.c), T1 differential vs <errno.h>; runs in
# host_tests (the in-OS T5 playthrough leg is the separately wall-gated gate).
AUDIO_STUB_TEST = audio_stub_test_host
$(AUDIO_STUB_TEST): src/host/audio_stub_test.c src/kernel/audio_stub.c src/include/audio_stub.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/audio_stub.c src/host/audio_stub_test.c -o $@

# /dev/fb0 mmap translation + fb blank/activate ABI surface: pure single-TU
# host golden tests (T1) for src/kernel/fb_mmap.c, the device-free mmap half
# of the compat /dev/fb0 node (GB-025).  When <linux/fb.h> and a real /dev/fb0
# exist on the host it also differentially locks the request number + mmap
# window rules (the fb_ioctl/evdev/nfs_proto single-TU precedent).
FB_MMAP_TEST = fb_mmap_test_host
$(FB_MMAP_TEST): src/host/fb_mmap_test.c src/kernel/fb_mmap.c src/kernel/fb_ioctl.c src/include/fb_mmap.h src/include/fb_ioctl.h src/include/linux_fb.h src/include/display_mode.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/fb_mmap.c src/kernel/fb_ioctl.c src/host/fb_mmap_test.c -o $@

# GB-032 SDL test app: one C file drawing on the fb surface + blitting a 2D
# buffer + echoing evdev/gamepad input.  Built (a) as the x86_64 static-PIE
# image the GB-013 loader maps (same flags the loader accepts: ET_DYN, no
# PT_INTERP), and (b) into the host golden suite as the T1/T4 gate.  T4 parses
# the REAL static-PIE binary with src/kernel/elf_image.c and cross-checks the
# type against readelf — a differential, and it runs inside host_tests.
SDL_APP_PIE = obj/sdl_test_app_pie
$(SDL_APP_PIE): tools/sdl/sdl_test_app.c src/include/fb_pixel.h src/include/linux_input.h src/include/display_mode.h
	@mkdir -p obj
	clang --target=x86_64-linux-musl -fPIE -static-pie -Isrc/include tools/sdl/sdl_test_app.c -o $@

SDL_TEST_APP_HOST = sdl_test_app_test_host
$(SDL_TEST_APP_HOST): src/host/sdl_test_app_host.c tools/sdl/sdl_test_app.c src/kernel/elf_image.c src/include/fb_pixel.h src/include/linux_input.h src/include/linux_joystick.h src/include/elf_image.h src/include/display_mode.h $(SDL_APP_PIE)
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/elf_image.c src/host/sdl_test_app_host.c -o $@

# F2.2 resolver codecs: src/user/resolv.c compiled with -DHOST_TEST (its
# pure functions land as hb_*) in one command with the test; inet_pton/ntop/
# aton race glibc on the same inputs (see src/host/resolv_test.c).
RESOLV_TEST = resolv_test_host
$(RESOLV_TEST): src/host/resolv_test.c src/user/resolv.c src/user_include/resolv.h
	$(HOST_CC) $(HOST_CFLAGS) src/user/resolv.c src/host/resolv_test.c -o $@

# F2.3 calendar math: src/libc/src/time_math.c compiled natively (hb_* via
# the obj/host_hb_%.o pattern) and raced against glibc's timegm/gmtime_r on
# a hardcoded case table + normalization cases (src/host/time_math_test.c).
TIME_MATH_TEST = time_math_test_host
$(TIME_MATH_TEST): obj/host_time_math_test.o obj/host_hb_time_math.o
	$(HOST_CC) -o $@ $^

# Console app host tests: both roles (interactive shell handoff + smoke).
CONSOLE_APP_TEST = console_test_host
$(CONSOLE_APP_TEST): obj/host_console_test.o obj/host_compat.o obj/host_user_gui.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

PONG_TEST_BIN = pong_test_host
$(PONG_TEST_BIN): obj/host_pong_test.o obj/host_user_graphics_graphics.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# NES emulator core (GB-035): single-TU host golden (nes.c included inline).
NES_TEST = nes_test_host
$(NES_TEST): obj/host_nes_test.o
	$(HOST_CC) -o $@ $^

DIALOG_ARROW_TEST = dialog_arrow_test_host
$(DIALOG_ARROW_TEST): obj/host_dialog_arrow_test.o obj/host_user_dialog.o obj/host_user_filedialog.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# --- GUI toolkit + new desktop app host tests ---
GUI_TEST = gui_test_host
$(GUI_TEST): obj/host_gui_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# Phase 0: POSIX errno convention — the ho_* mocks + Linux errno must match
# the kernel's negative-errno contract (errno.h uses Linux numbering).
ERRNO_TEST = errno_test_host
$(ERRNO_TEST): obj/host_errno_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# GB-013 (gaming.md §2.2 #1): the ELF parser is compiled from the SAME source the
# kernel links (src/kernel/elf_image.c) and fed generated fixtures
# (tools/linux-abi/gen_test_elf.py -> tests/fixtures/elf/).  The gate that decides
# whether a Linux binary may load is proven on the host, before any QEMU run.
# The fixtures are generated, not committed: .gitignore ignores *.elf repo-wide
# (no accidental ELF blobs in the tree), and regenerating is deterministic.
ELF_FIXTURES = tests/fixtures/elf/.generated
$(ELF_FIXTURES): tools/linux-abi/gen_test_elf.py
	python3 tools/linux-abi/gen_test_elf.py
	@touch $@

# GB-014/GB-015: the compat ABI tables (syscall numbers, errno) are locked against
# goldens measured on a real Linux box (tools/linux-abi/gen_abi_golden.py).
LINUX_ABI_TEST = linux_abi_test_host
$(LINUX_ABI_TEST): obj/host_linux_abi_test.o
	$(HOST_CC) -o $@ $^
obj/host_linux_abi_test.o: src/host/linux_abi_test.c src/include/linux_syscall.h src/include/errno.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@

# GB-016 (host T4 value differential): the kernel uname shim's VALUES must equal
# real Linux uname() (sysname, machine, NUL-padding).  The existing host probe's
# `uname sysname=Linux` line is real-musl vs real-glibc on the same host — both
# are Linux, so it cannot catch a shim returning the wrong identity.  This runs
# the merged linux_fill_utsname against the host's real uname() byte-for-byte.
UNAME_PARITY = uname_parity_host
$(UNAME_PARITY): obj/host_linux_uname_parity.o obj/host_linux_uname.o
	$(HOST_CC) -o $@ $^
obj/host_linux_uname_parity.o: src/host/linux_uname_parity.c src/include/linux_uname.h src/include/linux_layout.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@
obj/host_linux_uname.o: src/kernel/linux_uname.c src/include/linux_uname.h src/include/linux_layout.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@

# GB-015/014: struct-layout parity lock + L_* constant lock — asserted against
# the same tests/fixtures/abi goldens (linux_layouts.golden, linux_constants.golden),
# the "silent corruption" class a wrong layout/constant causes.  Kernel headers
# feed it; host-compiled here.
LINUX_LAYOUT_TEST = linux_layout_test_host
$(LINUX_LAYOUT_TEST): obj/host_linux_layout_test.o
	$(HOST_CC) -o $@ $^
obj/host_linux_layout_test.o: src/host/linux_layout_test.c src/include/linux_layout.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@

LINUX_ABI_CONSTANTS_TEST = linux_abi_constants_test_host
$(LINUX_ABI_CONSTANTS_TEST): obj/host_linux_abi_constants_test.o
	$(HOST_CC) -o $@ $^
obj/host_linux_abi_constants_test.o: src/host/linux_abi_constants_test.c src/include/linux_syscall.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@

ELF_IMAGE_TEST = elf_image_test_host
# .generated is an ORDER-ONLY prerequisite: it orders fixture generation before
# the link but must never appear in $^ (Apple ld hard-errors "file is empty";
# GNU ld silently ignores it — the macOS symptom is a link failure naming
# tests/fixtures/elf/.generated).
$(ELF_IMAGE_TEST): obj/host_elf_image_test.o obj/host_elf_image.o | $(ELF_FIXTURES)
	$(HOST_CC) -o $@ $^
obj/host_elf_image_test.o: src/host/elf_image_test.c src/include/elf_image.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@
obj/host_elf_image.o: src/kernel/elf_image.c src/include/elf_image.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/include -c $< -o $@

# P1: the thread/TLS tests share their source with the device bins and run
# the same logic on glibc pthreads (-DHOST_TEST), validating the test
# itself (futex-only cases are device-only).
THRD_TEST_HOST = thrd_test_host
$(THRD_TEST_HOST): obj/host_thrd_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^
TLS_TEST_HOST = tls_test_host
$(TLS_TEST_HOST): obj/host_tls_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^
# L8 WK-1: clocks/entropy/sched/stack-bounds logic validated on glibc
# (pthread_getattr_np + attr_getstack/setstack are glibc extensions; the
# test defines _GNU_SOURCE itself for the host build).
WK1C_TEST_HOST = wk1c_test_host
$(WK1C_TEST_HOST): obj/host_wk1c_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

obj/host_thrd_test.o: src/user/thrd_test.c src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -pthread -include src/host/host_pthread_compat.h -c $< -o $@
obj/host_tls_test.o: src/user/tls_test.c src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -pthread -c $< -o $@
obj/host_wk1c_test.o: src/user/wk1c_test.c src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -pthread -include src/host/host_pthread_compat.h -c $< -o $@

# P4: the AF_UNIX acceptance test also runs its logic on glibc
# (-DHOST_TEST; it includes native headers, not libc.h), and the pure
# SCM_RIGHTS codec gets the nfs_proto-style host suite.
IPC_TEST_HOST = ipc_test_host
$(IPC_TEST_HOST): obj/host_ipc_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^
obj/host_ipc_test.o: src/user/ipc_test.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# P8.1: the POSIX torture acceptance runs its logic natively on glibc too
# (-DHOST_TEST; it re-execs itself via /proc/self/exe for the exec/wait
# child mode).
TORTURE_TEST_HOST = torture_test_host
$(TORTURE_TEST_HOST): obj/host_torture_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^
obj/host_torture_test.o: src/user/torture_test.c src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -pthread -c $< -o $@

IPC_PROTO_TEST = ipc_proto_test_host
$(IPC_PROTO_TEST): src/host/ipc_proto_test.c src/kernel/ipc_proto.c src/include/ipc_proto.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/ipc_proto.c src/host/ipc_proto_test.c -o $@

# Phase 1: HobbyOS sysroot subset compiled for the host (as hb_*) and
# property-tested byte-exact against glibc on literal + randomized inputs.
obj/host_hb_%.o: src/libc/src/%.c src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

STRING_TEST = libc_string_test_host
$(STRING_TEST): obj/host_libc_string_test.o obj/host_hb_string.o
	$(HOST_CC) -o $@ $^

CTYPE_TEST = libc_ctype_test_host
$(CTYPE_TEST): obj/host_libc_ctype_test.o obj/host_hb_ctype.o
	$(HOST_CC) -o $@ $^

STDLIB_TEST = libc_stdlib_test_host
$(STDLIB_TEST): obj/host_libc_stdlib_test.o obj/host_hb_stdlib.o
	$(HOST_CC) -o $@ $^

# selfhost W2-2: setjmp/longjmp — the real handlers run on the host (x86-64
# path) via the hb_* renames; every device build links the same object into
# libc.a, so aarch64 coverage comes from the in-OS compile + disassembly.
SETJMP_TEST = libc_setjmp_test_host
$(SETJMP_TEST): obj/host_libc_setjmp_test.o obj/host_hb_setjmp.o
	$(HOST_CC) -o $@ $^

# malloc.c's realloc compiled natively (hb_* names): free-list allocator
# logic is pure, so it runs against a fake heap on the host.
PRINTF_TEST = libc_printf_test_host
$(PRINTF_TEST): obj/host_libc_printf_test.o obj/host_hb_stdio.o
	$(HOST_CC) -o $@ $^

REALLOC_TEST = libc_realloc_test_host
obj/host_malloc_hb.o: src/user/malloc.c src/user_include/malloc.h
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(REALLOC_TEST): obj/host_libc_realloc_test.o obj/host_malloc_hb.o
	$(HOST_CC) -o $@ $^

# getopt_long raced against glibc (hb_ impl from src/libc/src/getopt.c).
GETOPT_TEST = libc_getopt_test_host
$(GETOPT_TEST): obj/host_libc_getopt_test.o obj/host_hb_getopt.o
	$(HOST_CC) -o $@ $^

# GNU regex ported into the sysroot (src/libc/src/regex.c, which includes
# the regcomp/regexec/regex_internal parts): compiled for the host renamed
# to hb_* and raced byte-exact against glibc's regex in the C locale
# (see src/host/libc_regex_test.c).  Needs -Isrc/libc/include so the
# sysroot's regex.h/intprops.h/verify.h win over system copies.
REGEX_TEST = libc_regex_test_host
obj/host_hb_regex.o: src/libc/src/regex.c src/libc/include/*.h src/libc/src/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
obj/host_libc_regex_test.o: src/host/libc_regex_test.c src/user/regex_test_cases.h src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
obj/glibc_regex_layout.o: src/host/glibc_regex_layout.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(REGEX_TEST): obj/host_libc_regex_test.o obj/host_hb_regex.o obj/glibc_regex_layout.o
	$(HOST_CC) -o $@ $^

# langinfo: the two C-locale facts the GNU ports rely on (CODESET is
# "ANSI_X3.4-1968", MB_CUR_MAX is 1) checked against glibc.
LANGINFO_TEST = libc_langinfo_test_host
obj/host_hb_langinfo.o: src/libc/src/langinfo.c src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
obj/host_libc_langinfo_test.o: src/host/libc_langinfo_test.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
$(LANGINFO_TEST): obj/host_libc_langinfo_test.o obj/host_hb_langinfo.o
	$(HOST_CC) -o $@ $^

# P3.2 gap-fills: the scanner + wide formatter, compiled from the bare-metal
# sources with -DHOST_TEST (colliding symbols are hb_* there) and raced
# byte-for-byte against glibc (src/host/libc_scanf_wprintf_test.c).
SCANFW_PARITY = libc_scanf_wprintf_parity_run
obj/host_hb_stdio_sp.o: src/libc/src/stdio.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_hb_wchar_sp.o: src/libc/src/wchar.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_libc_scanf_wprintf_test.o: src/host/libc_scanf_wprintf_test.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(SCANFW_PARITY): obj/host_libc_scanf_wprintf_test.o obj/host_hb_stdio_sp.o obj/host_hb_wchar_sp.o
	$(HOST_CC) -o $@ $^

# P3.1 numeric parity: strtod / strtol / strtold and the math front-ends
# compiled from the bare-metal sources with -DHOST_TEST, raced against glibc
# (src/host/libc_num_parity_test.c; the acceptance window is the ulp bound
# documented in src/libc/src/strtod.c).
NUM_PARITY = libc_num_parity_run
obj/host_hb_math_np.o: src/libc/src/math.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -ffp-contract=off -c $< -o $@
obj/host_hb_strtod_np.o: src/libc/src/strtod.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_libc_num_parity_test.o: src/host/libc_num_parity_test.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(NUM_PARITY): obj/host_libc_num_parity_test.o obj/host_hb_math_np.o obj/host_hb_strtod_np.o
	$(HOST_CC) -o $@ $^ -lm

# P3.2 transcendental host race: the math.c slice (hb_*) against glibc
# over ~1M deterministic values plus live bit-exact special-value checks
# (src/host/math_transcend_test.c).
MATH_TX = math_transcend_parity_run
obj/host_math_transcend_test.o: src/host/math_transcend_test.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(MATH_TX): obj/host_math_transcend_test.o obj/host_hb_math_np.o
	$(HOST_CC) -o $@ $^ -lm

# P3.2/P3.3 wide parity: wchar / wctype / strftime / xlocale compiled from
# the bare-metal sources with -DHOST_TEST (colliding symbols are hb_* there)
# and raced against glibc (src/host/libc_wide_parity_test.c).
WIDE_PARITY = libc_wide_parity_run
obj/host_hb_wchar_wp.o: src/libc/src/wchar.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_hb_wctype_wp.o: src/libc/src/wctype.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_hb_strftime_wp.o: src/libc/src/strftime.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_hb_xlocale_wp.o: src/libc/src/xlocale.c src/libc/include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
obj/host_libc_wide_parity_test.o: src/host/libc_wide_parity_test.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(WIDE_PARITY): obj/host_libc_wide_parity_test.o obj/host_hb_wchar_wp.o obj/host_hb_wctype_wp.o obj/host_hb_strftime_wp.o obj/host_hb_xlocale_wp.o
	$(HOST_CC) -o $@ $^

# The ported wc built for the host: ours except getopt_long (hb_* via our
# getopt.h) and error() (host_hb_error.o); everything else resolves to
# glibc.  wc_host is raced byte-for-byte against coreutils in
# src/host/wc_parity.sh.
WC_HOST = obj/wc_host
obj/host_wc.o: src/user/wc.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
$(WC_HOST): obj/host_wc.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $@ $^

# [inos-build-kernel] tar extractor host test: the test TU includes
# src/user/tar.c directly and runs it over glibc with REAL file I/O (no
# compat mocks — the extractor is port-style main(argc, argv)).  Two
# archive sources: a synthetic edge-case archive built in-test (dirs, "./"
# roots, 512/odd sizes, GNU 'L' longname, ustar prefix) and a real
# `tar --format=ustar` archive of tests/fixtures/tar, compared
# byte-for-byte against the source tree.
TAR_TEST = tar_test_run
obj/host_tar_test.o: src/host/tar_test.c src/user/tar.c
	@mkdir -p obj
	$(HOST_CC) -O2 -g -Wall -Wextra -c src/host/tar_test.c -o $@

TAR_FIXTURE_FILES = tests/fixtures/tar/GREET.TXT tests/fixtures/tar/EMPTY.TXT \
                    tests/fixtures/tar/DATA1200.DAT tests/fixtures/tar/MiXeD.TxT \
                    tests/fixtures/tar/SUB/NESTED.TXT tests/fixtures/tar/SUB/DEEPER/LEAF.TXT
obj/tar_fixture.tar: $(TAR_FIXTURE_FILES)
	@mkdir -p obj
	tar --format=ustar -cf $@ -C tests/fixtures/tar .

$(TAR_TEST): obj/host_tar_test.o obj/tar_fixture.tar
	$(HOST_CC) -o obj/tar_test obj/host_tar_test.o
	./obj/tar_test obj/tar_fixture.tar tests/fixtures/tar

WC_PARITY = wc_parity_run
$(WC_PARITY): $(WC_HOST)
	@bash src/host/wc_parity.sh $(WC_HOST)

# Strict byte-exact parity against the original GNU textutils-2.1 wc
# (same lineage as the port). Builds the reference, then compares.
WC_PARITY_STRICT = wc_parity_strict
$(WC_PARITY_STRICT): $(WC_HOST)
	@bash src/host/build_tu21_wc_ref.sh $(OBJ_DIR)/tu21_wc_ref
	@bash src/host/wc_parity.sh $(WC_HOST) $(OBJ_DIR)/tu21_wc_ref

# The ported head built for the host (head_host): ours except getopt_long
# (hb_* via our getopt.h) and error() (host_hb_error.o).  head_host is
# raced byte-for-byte against GNU head in src/host/head_parity.sh.
HEAD_HOST = obj/head_host
obj/host_head.o: src/user/head_gnu.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
$(HEAD_HOST): obj/host_head.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $@ $^

HEAD_PARITY = head_parity_run
$(HEAD_PARITY): $(HEAD_HOST)
	@bash src/host/head_parity.sh $(HEAD_HOST)

# Strict byte-exact parity against textutils-2.1's original head.
HEAD_PARITY_STRICT = head_parity_strict
$(HEAD_PARITY_STRICT): $(HEAD_HOST)
	@bash src/host/build_tu21_head_ref.sh $(OBJ_DIR)/tu21_head_ref
	@bash src/host/head_parity.sh $(HEAD_HOST) $(OBJ_DIR)/tu21_head_ref

# The ported tail built for the host (tail_host): ours except getopt_long
# (hb_* via our getopt.h) and error() (host_hb_error.o).  tail_host is
# raced byte-for-byte against GNU tail in src/host/tail_parity.sh.
TAIL_HOST = obj/tail_host
obj/host_tail.o: src/user/tail_gnu.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
$(TAIL_HOST): obj/host_tail.o obj/host_hb_assert.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $@ $^

TAIL_PARITY = tail_parity_run
$(TAIL_PARITY): $(TAIL_HOST)
	@bash src/host/tail_parity.sh $(TAIL_HOST)

# Strict byte-exact parity against textutils-2.1's original tail
# (extra gnulib surface: argmatch.c + human.c, so its own build recipe).
TAIL_PARITY_STRICT = tail_parity_strict
$(TAIL_PARITY_STRICT): $(TAIL_HOST)
	@bash src/host/build_tu21_tail_ref.sh $(OBJ_DIR)/tu21_tail_ref
	@bash src/host/tail_parity.sh $(TAIL_HOST) $(OBJ_DIR)/tu21_tail_ref

# The ported cut built for the host (cut_host): ours except getopt_long
# (hb_* via our getopt.h) and error() (host_hb_error.o).  cut_host is
# raced byte-for-byte against GNU cut in src/host/cut_parity.sh.
CUT_HOST = obj/cut_host
obj/host_cut.o: src/user/cut_gnu.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
obj/host_hb_assert.o: src/host/hb_assert.c
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
$(CUT_HOST): obj/host_cut.o obj/host_hb_assert.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $@ $^

CUT_PARITY = cut_parity_run
$(CUT_PARITY): $(CUT_HOST)
	@bash src/host/cut_parity.sh $(CUT_HOST)

# Strict byte-exact parity against textutils-2.1's original cut
# (getstr.c is vendored, so its own reference build recipe).
CUT_PARITY_STRICT = cut_parity_strict
$(CUT_PARITY_STRICT): $(CUT_HOST)
	@bash src/host/build_tu21_cut_ref.sh $(OBJ_DIR)/tu21_cut_ref
	@bash src/host/cut_parity.sh $(CUT_HOST) $(OBJ_DIR)/tu21_cut_ref

# The batch-1 textutils ports follow the identical host pattern: a
# host build racing GNU on the same inputs (loose vs modern GNU, strict
# vs the textutils-2.1 reference this source came from).
define TU21_PARITY_RULES
$(3)_HOST = obj/$(1)_host
obj/host_$(1).o: src/user/$(1)_gnu.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $$< -o $$@
$$($(3)_HOST): obj/host_$(1).o obj/host_hb_assert.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $$@ $$^

$(3)_PARITY = $(1)_parity_run
$$($(3)_PARITY): $$($(3)_HOST)
	@bash src/host/$(1)_parity.sh $$($(3)_HOST)

$(3)_PARITY_STRICT = $(1)_parity_strict
$$($(3)_PARITY_STRICT): $$($(3)_HOST)
	@bash src/host/build_tu21_$(1)_ref.sh $(OBJ_DIR)/tu21_$(1)_ref
	@bash src/host/$(1)_parity.sh $$($(3)_HOST) $(OBJ_DIR)/tu21_$(1)_ref
endef

$(eval $(call TU21_PARITY_RULES,tr,tr,TR))
$(eval $(call TU21_PARITY_RULES,paste,paste,PASTE))
$(eval $(call TU21_PARITY_RULES,fold,fold,FOLD))
$(eval $(call TU21_PARITY_RULES,nl,nl,NL))
$(eval $(call TU21_PARITY_RULES,comm,comm,COMM))
$(eval $(call TU21_PARITY_RULES,tsort,tsort,TSORT))
$(eval $(call TU21_PARITY_RULES,expand,expand,EXPAND))
$(eval $(call TU21_PARITY_RULES,unexpand,unexpand,UNEXPAND))
$(eval $(call TU21_PARITY_RULES,cksum,cksum,CKSUM))
$(eval $(call TU21_PARITY_RULES,md5sum,md5sum,MD5SUM))
$(eval $(call TU21_PARITY_RULES,tac,tac,TAC))

# --- GNU diffutils 2.8.1 (cmp).  Same shape as the textutils family,
# but the reference build comes from the diffutils tarball.  -----------
define DIFFUTILS_PARITY_RULES
$(3)_HOST = obj/$(1)_host
obj/host_$(1).o: src/user/$(1)_gnu.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $$< -o $$@
$$($(3)_HOST): obj/host_$(1).o obj/host_hb_assert.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $$@ $$^

$(3)_PARITY = $(1)_parity_run
$$($(3)_PARITY): $$($(3)_HOST)
	@bash src/host/$(1)_parity.sh $$($(3)_HOST)

$(OBJ_DIR)/diffutils_$(1)_ref:
	@bash src/host/build_diffutils_$(1)_ref.sh $$@

$(3)_PARITY_STRICT = $(1)_parity_strict
$$($(3)_PARITY_STRICT): $$($(3)_HOST) $(OBJ_DIR)/diffutils_$(1)_ref
	@bash src/host/$(1)_parity.sh $$($(3)_HOST) $(OBJ_DIR)/diffutils_$(1)_ref
endef

$(eval $(call DIFFUTILS_PARITY_RULES,cmp,cmp,CMP))

# Header-only sysroot set (stdarg/limits/stdbool/inttypes): compiled with
# -Isrc/libc/include FIRST so the HobbyOS headers (not glibc's) resolve.
HEADERS_TEST = libc_headers_test_host
obj/host_libc_headers_test.o: src/host/libc_headers_test.c src/libc/include/stdarg.h src/libc/include/limits.h src/libc/include/stdbool.h src/libc/include/inttypes.h
	$(HOST_CC) -Isrc/libc/include $(HOST_CFLAGS) -c $< -o $@
$(HEADERS_TEST): obj/host_libc_headers_test.o
	$(HOST_CC) -o $@ $^

# --- F2.4/F2.5 (browser.md §6): C++ runtime + C++-clean sysroot ----------
# cxxrt_test drives the real runtime object (src/libc/src/cxxrt.cpp built
# for the host) through the Itanium entry points a C++ compiler emits.
# Both compile as C++17 with the shipped policy flags.  cxxrt.cpp keeps
# -Isrc/libc/include so its "cxxrt.h" and <stdlib.h> resolve to the
# sysroot exactly as they do on-device; the test TU deliberately omits it
# so <stdio.h>/<sys/wait.h> stay glibc's and cxxrt.h is reached by
# relative include.
# w6-run: obj/host_malloc_hb.o (host build of src/user/malloc.c) rides in the
# link — cxxrt's delete now calls free(ptr) with the RAW pointer, a contract
# defined over the HobbyOS allocator's BLOCK_MAGIC check, not glibc's free.
# (See the test TU's header note; the hb_* wrappers shadow libc's malloc.)
CXX_TEST_FLAGS = -std=c++17 -fno-exceptions -fno-rtti
CXXRT_TEST = cxxrt_test_host
obj/host_cxxrt_test.o: src/host/cxxrt_test.cpp src/libc/include/cxxrt.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(CXX_TEST_FLAGS) -c $< -o $@
obj/host_cxxrt.o: src/libc/src/cxxrt.cpp src/libc/include/cxxrt.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(CXX_TEST_FLAGS) -Isrc/libc/include -c $< -o $@
$(CXXRT_TEST): obj/host_cxxrt_test.o obj/host_cxxrt.o obj/host_compat.o obj/host_malloc_hb.o
	$(HOST_CC) -o $@ $^

# cxx_headers_test: every public sysroot header in one C++ TU, compiled
# with the sysroot include path FIRST so the HobbyOS headers are what get
# tested — the F2.5 regression gate (see the test's header comment).
CXX_HEADERS_TEST = cxx_headers_test_host
obj/host_cxx_headers_test.o: src/host/cxx_headers_test.cpp src/libc/include/*.h src/libc/include/sys/*.h $(USER_HDRS)
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) $(CXX_TEST_FLAGS) -Isrc/libc/include -Isrc/include -c $< -o $@
$(CXX_HEADERS_TEST): obj/host_cxx_headers_test.o
	$(HOST_CC) -o $@ $^

# --- l3-rtti (browser.md §6/L6): libc++abi RTTI closure, host --------------
# The host test compiles the SHIPPED vendor RTTI objects natively with the
# same hermetic flags the target class-B build uses (-nostdinc++ + the
# vendored libc++ headers; -frtti for the closure only) and drives them with
# real compiler-emitted dynamic_cast/typeid calls.  The link is closed-world:
# -nostdlib++ + the four objects + the support shim leave no host C++ runtime
# in ldd(), so a call can never fall through to a system implementation.
# The vendor sources are files of the fetched tree, so their prerequisite is
# the guarded pattern rule above (fresh-checkout discipline).
VENDOR_HOST_CXXFLAGS = -O2 -g -std=c++23 -fno-exceptions -frtti -nostdinc++ \
  -I$(LIBCXX_INCLUDE) -I$(LIBCXX_ROOT)/libcxx/src -I$(LIBCXX_ROOT)/libcxx/src/include \
  -I$(LIBCXX_ROOT)/libcxxabi/include \
  -D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES \
  -DLIBCXX_BUILDING_LIBCXXABI -D_LIBCXXABI_BUILDING_LIBRARY
RTTI_TEST = rtti_test_host
obj/host_rtti_test.o: src/host/rtti_test.cpp
	@mkdir -p obj
	$(HOST_CC) -Wall -Wextra -g $(VENDOR_HOST_CXXFLAGS) -c $< -o $@
obj/host_rtti_private_typeinfo.o: $(LIBCXX_ROOT)/libcxxabi/src/private_typeinfo.cpp $(LIBCXX_VENDOR)/.fetched-ok
	@mkdir -p obj
	$(HOST_CC) $(VENDOR_HOST_CXXFLAGS) -c $< -o $@
obj/host_rtti_stdlib_typeinfo.o: $(LIBCXX_ROOT)/libcxxabi/src/stdlib_typeinfo.cpp $(LIBCXX_VENDOR)/.fetched-ok
	@mkdir -p obj
	$(HOST_CC) $(VENDOR_HOST_CXXFLAGS) -c $< -o $@
obj/host_rtti_stdlib_exception.o: $(LIBCXX_ROOT)/libcxxabi/src/stdlib_exception.cpp $(LIBCXX_VENDOR)/.fetched-ok
	@mkdir -p obj
	$(HOST_CC) $(VENDOR_HOST_CXXFLAGS) -c $< -o $@
obj/host_rtti_test_support.o: src/host/rtti_test_support.cpp
	@mkdir -p obj
	$(HOST_CC) -Wall -Wextra -g $(VENDOR_HOST_CXXFLAGS) -c $< -o $@
$(RTTI_TEST): obj/host_rtti_test.o obj/host_rtti_private_typeinfo.o obj/host_rtti_stdlib_typeinfo.o obj/host_rtti_stdlib_exception.o obj/host_rtti_test_support.o
	$(HOST_CC) -nostdlib++ -o $@ $^

# WM damage bookkeeping: line-level window repair + the base (damage) clip
# (window.c + graphics.c are included into the test's single TU).
WINDOW_DAMAGE_TEST = window_damage_test_host
$(WINDOW_DAMAGE_TEST): obj/host_window_damage_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# Window text capture: the sliding terminal-tail helpers (window.c included
# into the test's single TU, same as window_damage_test).
WINDOW_TEXT_TEST = window_text_test_host
$(WINDOW_TEXT_TEST): obj/host_window_text_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# Both of the above include window.c/graphics.c directly, so their objects
# must rebuild when those sources change (the generic pattern rule cannot
# see inside the includes).
obj/host_window_damage_test.o: src/user/graphics/window.c src/user/graphics/graphics.c
obj/host_window_text_test.o: src/user/graphics/window.c src/user/graphics/graphics.c

# The desktop's chrome diff (desktop_damage()), linked against the real
# desktop.c like desktop_menu_test/desktop_drag_test.
DESKTOP_DAMAGE_TEST = desktop_damage_test_host
$(DESKTOP_DAMAGE_TEST): obj/host_desktop_damage_test.o obj/host_user_desktop.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

GRAPHICS_LIB_TEST = graphics_lib_test_host
$(GRAPHICS_LIB_TEST): obj/host_graphics_lib_test.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

# rp-w (docs/browser/render-pipeline.md Design 1): wsurf tests share the
# device source (src/user/wsurf_test.c, -DHOST_TEST) and link the wsurf
# object built by the host_user_graphics_% pattern; wsurf.c and the test
# need no host compat (only stdio), so the object pair is self-contained.
WSURF_TEST_HOST = wsurf_test_host
$(WSURF_TEST_HOST): obj/host_wsurf_test.o obj/host_user_graphics_wsurf.o
	$(HOST_CC) -o $@ $^
obj/host_wsurf_test.o: src/user/wsurf_test.c src/user_include/*.h
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# Each app host test includes its app's .c directly and links the toolkit,
# dialogs and host compatibility layer.
define HOST_APP_TEST_RULE
$(1)_test_host: obj/host_$(1)_test.o obj/host_compat.o obj/host_user_gui.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$$(HOST_CC) -o $$@ $$^
endef

$(foreach app,$(DESKTOP_APP_NAMES),$(eval $(call HOST_APP_TEST_RULE,$(app))))

HOST_APP_TEST_BINS = $(foreach app,$(DESKTOP_APP_NAMES),$(app)_test_host)

# --- T4 differential compat-syscall parity (gaming swarm, ABI worker) ---
# src/host/t4_parity.sh races the same probe (src/host/t4_compat_probe.c) as
# a static-musl Linux binary (leg A) vs the HobbyOS host-compat surface (leg
# B, glibc ho_* mocks over our Linux-numbered errno.h), diffing stdout +
# exit status + errno byte-for-byte. The in-QEMU device leg is gated on the
# kernel ELF loader (Coordinator ticket GB-013 [DEEP]); until then the host
# differential is the gate. Needs musl (apt: musl musl-tools); if absent the
# harness skips leg A and reports it, never a fake pass.
T4_COMPAT_PARITY = t4_compat_parity
$(T4_COMPAT_PARITY): src/host/t4_parity.sh src/host/t4_compat_probe.c
	@bash src/host/t4_parity.sh

# GB-083: leg B must be LIVE — flipping a ho_* shim must FAIL the differential.
T4_COMPAT_SELFTEST = t4_compat_selftest
$(T4_COMPAT_SELFTEST): src/host/t4_parity.sh src/host/t4_compat_probe.c
	@bash src/host/t4_parity.sh --self-test

# GB-126: x86 exception stubs must push the right frame shape — EXCEPTION_ERR for
# #CP/#VC/#SX (21/29/30), not EXCEPTION_NO_ERR. Table-driven over the dispatcher,
# so a future exception cannot get the wrong macro in silence. Host-side, no QEMU.
VECTOR_SHAPE_SCAN = vector_shape_scan
$(VECTOR_SHAPE_SCAN): tools/gate/vector_shape_scan.py
	@python3 tools/gate/vector_shape_scan.py

# GB-084: open() must always pass its mode when O_CREAT — the in-suite scan
# FAILs if any two-argument O_CREAT open() is (re)introduced.
OPEN_MODE_SCAN = open_mode_scan
$(OPEN_MODE_SCAN): tools/gate/open_mode_scan.py
	@python3 tools/gate/open_mode_scan.py

# GB-066: T4 device-leg instrument.  src/user/t4_probe.c is the W0 row-set probe
# (standard C + write(), so the SAME musl-built static ELF is leg A on Linux and
# leg B inside HobbyOS-intel via the real ELF loader + linux dispatch — never a
# host shim).  tools/gate/t4_device.sh is the gate: it asserts the probe's own
# arrival in the guest, diffs rc/errno/out per row, FAILS on a wrong shim, and
# FAILS (never SKIPs) on an absent probe.  The in-guest leg B run is wired to the
# T3 device tier and is gated on GB-014's full linux dispatch (coordinator); the
# instrument itself — probe + gate + self-test proving it can fail — is host-side
# and runs here.
#
# GB-066-V2 (re-gate): the probe must be a static-PIE (ET_DYN).  The v2 loader
# refuses a fixed low-VA ET_EXEC (elf_loader.c:120, "ET_EXEC linked below the
# user window; a -static-pie build is required"), and musl-gcc's default `-static`
# produces exactly that (entry ~0x401cc1 < USER_VA_BASE).  Build with -static-pie
# and ASSERT the result is ET_DYN (readelf -h Type: DYN) so a regression to
# -static fails the build loudly instead of silently shipping a probe the guest
# rejects (the GB-066-V2 RED's clause-1 failure).
T4_DEVICE_GATE = t4_device_gate
T4_DEVICE_SELFTEST = t4_device_selftest
T4_DEVICE_CHECK = t4_device_check
T4_LEGA_OUT = $(OBJ_DIR)/t4_legA.out
T4_PROBE_ELF = $(OBJ_DIR)/t4_probe.elf
$(T4_PROBE_ELF): src/user/t4_probe.c
	@musl-gcc -static-pie -fPIE -O2 -Wall src/user/t4_probe.c -o $(T4_PROBE_ELF)
	@readelf -h $(T4_PROBE_ELF) | grep -q 'Type: *DYN' || ( echo "t4_probe: build is not ET_DYN/static-PIE — the v2 ELF loader rejects it (GB-066-V2)"; readelf -h $(T4_PROBE_ELF) | head -4; exit 1 )
$(T4_LEGA_OUT): $(T4_PROBE_ELF)
	@./$(T4_PROBE_ELF) > $(T4_LEGA_OUT) && echo "[t4-device] leg A captured -> $(T4_LEGA_OUT)"
$(T4_DEVICE_GATE): tools/gate/t4_device.sh $(T4_LEGA_OUT)
	@T4_LEGA_OUT=$(T4_LEGA_OUT) bash tools/gate/t4_device.sh
$(T4_DEVICE_SELFTEST): tools/gate/t4_device.sh
	@bash tools/gate/t4_device.sh --self-test
# GB-073: a --check that the device probe and host model agree on the row-name
# set the M1 -V DoDs name — FAILs on disagreement or a deleted row (a smaller
# diff is never a pass).  Lightweight (source + leg A only, no guest needed), so
# it runs in host_tests directly.
$(T4_DEVICE_CHECK): tools/gate/t4_device.sh $(T4_LEGA_OUT)
	@T4_LEGA_OUT=$(T4_LEGA_OUT) bash tools/gate/t4_device.sh --check

# GB-070: W0-corpus differential gate.  The M1 sentence names a corpus (hello /
# cat / sh / coreutils-static) that the intel wave's exited-programs set must
# carry; measured, only HELLO.BIN is a W0 member of the boot wave.  t4_w0.sh is
# the gate over that set: asserts every W0 member arrived in the guest, compares
# output to the Linux leg, FAILs (never skips) on absent/broken members.  The
# self-test proves it can report both PASS and FAIL from synthetic logs.
T4_W0_GATE = t4_w0_gate
T4_W0_SELFTEST = t4_w0_selftest
$(T4_W0_GATE): tools/gate/t4_w0.sh
	@bash tools/gate/t4_w0.sh --serial $(T3_LOG)
$(T4_W0_SELFTEST): tools/gate/t4_w0.sh
	@bash tools/gate/t4_w0.sh --self-test

# GB-072: manifest-driven-wave gate.  The boot wave is a hardcoded list, so a
# program staged on disk.img (T4PROBE.BIN) is never dispatched — five -V tickets
# are RED on this single ground.  t4_wave.sh is the gate over that dispatch:
# asserts every manifest-named program produced its arrival/exit line in an
# intel/arm wave serial (proving it ran), FAILs (never skips) when a
# manifest-named file is absent from the image, and leaves the no-manifest
# default covered (same wave, no coverage lost).  Self-test proves PASS+FAIL.
T4_WAVE_GATE = t4_wave_gate
T4_WAVE_SELFTEST = t4_wave_selftest
$(T4_WAVE_GATE): tools/gate/t4_wave.sh
	@bash tools/gate/t4_wave.sh --serial $(T3_LOG) --manifest "$(WAVE_MANIFEST)"
$(T4_WAVE_SELFTEST): tools/gate/t4_wave.sh
	@bash tools/gate/t4_wave.sh --self-test

# GB-082 glued-console-line counter (whole-line console atomicity observability).
T4_GLUED_SELFTEST = t4_glued_selftest
$(T4_GLUED_SELFTEST): tools/gate/t4_glued.sh
	@bash tools/gate/t4_glued.sh --self-test

# GB-122 clause-5 gate: the resume-copy #PF invariant.  Host-only self-test; the
# gate itself takes a T3 serial path (`bash tools/gate/t3_resume_page.sh <log>`),
# deliberately NOT wired into t3_tier.sh in this change.
T3_RESUME_PAGE_SELFTEST = t3_resume_page_selftest
$(T3_RESUME_PAGE_SELFTEST): tools/gate/t3_resume_page.sh
	@bash tools/gate/t3_resume_page.sh --self-test

# GB-086 clause-3 gate: boot the broken-select control (POLLBRK.BIN) and assert
# the >=85ms select-timeout check FAILs (elapsed_ms=0, POLLTST FAILED) with the
# guest halted.  A control that PASSES fails the gate — proves the gate cannot
# be painted green.  Drives the real tier with a POLLBRK-only manifest.
# Usage: make t3_pollbrk [ARCH=arm|intel]
T3_POLLBRK = t3_pollbrk
$(T3_POLLBRK): tools/gate/t3_pollbrk.sh
	@bash tools/gate/t3_pollbrk.sh $(ARCH)

# GB-089 null-entry control gate: boot a single-program WAVE.CFG manifest at
# T3_SMP=1 and assert the GB-087 outcome for the given state.
#   STATE=fixed   (a53f26e; default)  -> the run reaches halted=1, exit 0
#   STATE=unfixed (plain main)        -> the run provokes the null-entry fault
#       (intel 'vector=13', ARM 'EL1'/'ELR=0') and exits non-zero
# The gate detects the tree's actual state, so a mislabeled STATE is refused,
# not trusted (a mislabel that makes the number fall is the GB-066/079 class).
# Usage: make t3_nullentry [ARCH=arm|intel] [STATE=fixed|unfixed]
T3_NULLENTRY = t3_nullentry
$(T3_NULLENTRY): tools/gate/t3_nullentry.sh
	@bash tools/gate/t3_nullentry.sh $(ARCH) $(STATE)

# Wrap host test execution so a hung test fails the build instead of hanging
# it. On macOS without coreutils this falls back to an unwrapped run.
HOST_RUN = @sh -c 'if command -v timeout >/dev/null 2>&1; then exec timeout 40 "$$@"; else exec "$$@"; fi' sh

# GB-006 shared build cache: wrap the compiler in ccache and measure the DoD
# "cold vs cached build time".  cache_test.sh (T1) proves a real Miss->Hit
# transition and emits cold/warm ms.
# ---- GB-006: opt-in read/write build cache for the REAL build ----
# A plain `make` is unchanged (a fresh clone builds with no surprises and the
# cache can never fail a correctness gate).  A worker/farm that wants the build
# cache exports HOBBYOS_USE_CCACHE=1; the C/C++ recipes call `clang`/`clang++`
# by short name, so prepending /usr/lib/ccache on PATH routes them through the
# ccache shim.  CCACHE_BASEDIR is set to the tree so the same tree on two VMs
# hashes identically (that is what makes a seeded cache usable on farm-2/farm-3,
# not just the box that built it).  A cache miss is a normal, correct build.
ifdef HOBBYOS_USE_CCACHE
  CCACHE_DIR ?= $(HOME)/.cache/hobbyos-ccache
  CCACHE_BASEDIR ?= $(CURDIR)
  export CCACHE_DIR CCACHE_BASEDIR
  PATH := /usr/lib/ccache:$(PATH)
  export PATH
  $(info HOBBYOS_USE_CCACHE=1: real build routed through /usr/lib/ccache (CCACHE_DIR=$(CCACHE_DIR) CCACHE_BASEDIR=$(CCACHE_BASEDIR)))
endif
cache_measure:
	@bash tools/cache/measure_build.sh
cache_measure_negative:
	@bash tools/cache/measure_build.sh --negative
cache_setup:
	@bash tools/cache/setup_ccache.sh
cold_vs_cached:
	@bash tools/cache/cold_vs_cached.sh /tmp/hobbyos_cvc.o tools/cache/probe.c clang
test_cache:
	@bash tools/cache/cache_test.sh

# GB-007 differential harness skeleton: build/capture/compare the `hello`
# probe's {stdout, exit, errno} contract.  DoD: "hello diverges / lie detector
# works" — the T1 self-test plants divergences and requires DIVERGE, never a
# false pass.  (The HobbyOS-intel / QEMU target leg reuses the same harness.)
DIFF_RUN = python3 tools/differential/diff_harness_test.py
test_diff_harness:
	$(HOST_RUN) $(DIFF_RUN)

host_tests: $(EDITOR_HOST) $(EDITOR_TEST_BIN) test_cache test_diff_harness $(GAMES_SCAN_TEST) $(GFX_STATS_TEST) $(DESKTOP_MENU_TEST) $(DESKTOP_DRAG_TEST) $(DESKTOP_DAMAGE_TEST) $(DESKTOP_INPUT_TEST) $(DESKTOP_TERM_TEST) $(DESKTOP_PIXEL_TEST) $(X11_LIB_TEST) $(XCALC_TEST) $(ANTFARM_TEST) $(XEYES_TEST) $(XNIBBLER_TEST) $(NANO_TERM_TEST) $(APPS_SUITE_TEST) $(NFS_PROTO_TEST) $(RESOLV_TEST) $(TIME_MATH_TEST) $(CONSOLE_APP_TEST) $(PONG_TEST_BIN) $(STARDUST_TEST) $(DIALOG_ARROW_TEST) $(GUI_TEST) $(ERRNO_TEST) $(ELF_IMAGE_TEST) $(LINUX_ABI_TEST) $(THRD_TEST_HOST) $(TLS_TEST_HOST) $(WK1C_TEST_HOST) $(IPC_PROTO_TEST) $(IPC_TEST_HOST) $(TORTURE_TEST_HOST) $(WSURF_TEST_HOST) $(GRAPHICS_LIB_TEST) $(WINDOW_DAMAGE_TEST) $(WINDOW_TEXT_TEST) $(STRING_TEST) $(CTYPE_TEST) $(STDLIB_TEST) $(SETJMP_TEST) $(REALLOC_TEST) $(PRINTF_TEST) $(HEADERS_TEST) $(CXXRT_TEST) $(CXX_HEADERS_TEST) $(RTTI_TEST) $(GETOPT_TEST) $(REGEX_TEST) $(LANGINFO_TEST) $(SCANFW_PARITY) $(NUM_PARITY) $(MATH_TX) $(WIDE_PARITY) $(WC_PARITY) $(HEAD_PARITY) $(TAIL_PARITY) $(CUT_PARITY) $(TR_PARITY) $(PASTE_PARITY) $(FOLD_PARITY) $(NL_PARITY) $(COMM_PARITY) $(TSORT_PARITY) $(EXPAND_PARITY) $(UNEXPAND_PARITY) $(CKSUM_PARITY) $(MD5SUM_PARITY) $(TAC_PARITY) $(CMP_PARITY_STRICT) $(TAR_TEST) $(HOST_APP_TEST_BINS) $(FB_IOCTL_TEST) $(FB_PIXEL_TEST) $(T4_COMPAT_PARITY) $(EVDEV_TEST) $(XCRYPT_TEST) $(JOYPAD_TEST) $(CLOCKABI_TEST) $(MEMFD_TEST) $(NES_TEST) $(LINUX_LAYOUT_TEST) $(LINUX_INPUT_TEST) $(INPUT_NODE_TEST) $(FB_MMAP_TEST) $(LINUX_ABI_CONSTANTS_TEST) $(LOCKSTEAL_TEST) $(M2_CONTRACT_TEST) $(T4_DEVICE_SELFTEST) $(T4_DEVICE_CHECK) $(T4_W0_SELFTEST) $(T4_WAVE_SELFTEST) $(T4_GLUED_SELFTEST) $(T4_COMPAT_SELFTEST) $(OPEN_MODE_SCAN) $(VECTOR_SHAPE_SCAN) $(SDL_TEST_APP_HOST) $(UNAME_PARITY) $(AUDIO_STUB_TEST)
	$(HOST_RUN) ./$(EDITOR_TEST_BIN)
	$(HOST_RUN) ./$(GAMES_SCAN_TEST)
	$(HOST_RUN) ./$(GFX_STATS_TEST)
	$(HOST_RUN) ./$(DESKTOP_MENU_TEST)
	$(HOST_RUN) ./$(DESKTOP_DRAG_TEST)
	$(HOST_RUN) ./$(DESKTOP_DAMAGE_TEST)
	$(HOST_RUN) ./$(DESKTOP_INPUT_TEST)
	$(HOST_RUN) ./$(DESKTOP_TERM_TEST)
	$(HOST_RUN) ./$(DESKTOP_PIXEL_TEST)
	$(HOST_RUN) ./$(X11_LIB_TEST)
	$(HOST_RUN) ./$(XCALC_TEST)
	$(HOST_RUN) ./$(ANTFARM_TEST)
	$(HOST_RUN) ./$(XEYES_TEST)
	$(HOST_RUN) ./$(XNIBBLER_TEST)
	$(HOST_RUN) ./$(NANO_TERM_TEST)
	$(HOST_RUN) ./$(XCRYPT_TEST)
	$(HOST_RUN) ./$(NFS_PROTO_TEST)
	$(HOST_RUN) ./$(RESOLV_TEST)
	$(HOST_RUN) ./$(TIME_MATH_TEST)
	$(HOST_RUN) ./$(CONSOLE_APP_TEST)
	$(HOST_RUN) ./$(DIALOG_ARROW_TEST)
	$(HOST_RUN) ./$(PONG_TEST_BIN)
	$(HOST_RUN) ./$(STARDUST_TEST)
	@echo "[host] self-registering python host tests (GB-064)"
	python3 tools/t5/run_t5_tests.py
	$(HOST_RUN) ./$(NES_TEST)
	$(HOST_RUN) ./$(GUI_TEST)
	$(HOST_RUN) ./$(ERRNO_TEST)
	$(HOST_RUN) ./$(ELF_IMAGE_TEST)
	$(HOST_RUN) ./$(LINUX_ABI_TEST)
	$(HOST_RUN) ./$(UNAME_PARITY)
	$(HOST_RUN) ./$(LINUX_LAYOUT_TEST)
	$(HOST_RUN) ./$(LINUX_ABI_CONSTANTS_TEST)
	$(HOST_RUN) ./$(THRD_TEST_HOST)
	$(HOST_RUN) ./$(TLS_TEST_HOST)
	$(HOST_RUN) ./$(WK1C_TEST_HOST)
	$(HOST_RUN) ./$(IPC_PROTO_TEST)
	$(HOST_RUN) ./$(IPC_TEST_HOST)
	$(HOST_RUN) ./$(TORTURE_TEST_HOST)
	$(HOST_RUN) ./$(GRAPHICS_LIB_TEST)
	$(HOST_RUN) ./$(WSURF_TEST_HOST)
	$(HOST_RUN) ./$(FB_IOCTL_TEST)
	$(HOST_RUN) ./$(FB_PIXEL_TEST)
	$(HOST_RUN) ./$(M2_CONTRACT_TEST)
	$(HOST_RUN) ./$(EVDEV_TEST)
	$(HOST_RUN) ./$(JOYPAD_TEST)
	$(HOST_RUN) ./$(CLOCKABI_TEST)
	$(HOST_RUN) ./$(LOCKSTEAL_TEST)
	$(HOST_RUN) ./$(MEMFD_TEST)
	$(HOST_RUN) ./$(LINUX_INPUT_TEST)
	$(HOST_RUN) ./$(INPUT_NODE_TEST)
	$(HOST_RUN) ./$(AUDIO_STUB_TEST)
	$(HOST_RUN) ./$(FB_MMAP_TEST)
	$(HOST_RUN) ./$(SDL_TEST_APP_HOST)
	$(HOST_RUN) ./$(WINDOW_DAMAGE_TEST)
	$(HOST_RUN) ./$(WINDOW_TEXT_TEST)
	$(HOST_RUN) ./$(STRING_TEST)
	$(HOST_RUN) ./$(CTYPE_TEST)
	$(HOST_RUN) ./$(STDLIB_TEST)
	$(HOST_RUN) ./$(SETJMP_TEST)
	$(HOST_RUN) ./$(REALLOC_TEST)
	$(HOST_RUN) ./$(PRINTF_TEST)
	$(HOST_RUN) ./$(HEADERS_TEST)
	$(HOST_RUN) ./$(CXXRT_TEST)
	$(HOST_RUN) ./$(CXX_HEADERS_TEST)
	$(HOST_RUN) ./$(RTTI_TEST)
	$(HOST_RUN) ./$(GETOPT_TEST)
	$(HOST_RUN) ./$(REGEX_TEST)
	$(HOST_RUN) ./$(LANGINFO_TEST)
	$(HOST_RUN) ./$(SCANFW_PARITY)
	$(HOST_RUN) ./$(NUM_PARITY)
	$(HOST_RUN) ./$(MATH_TX)
	$(HOST_RUN) ./$(WIDE_PARITY)
	$(HOST_RUN) ./files_test_host
	$(HOST_RUN) ./calc_test_host
	$(HOST_RUN) ./clock_test_host
	$(HOST_RUN) ./sysmon_test_host
	$(HOST_RUN) ./hex_test_host
	$(HOST_RUN) ./tasks_test_host
	$(HOST_RUN) ./find_test_host
	$(HOST_RUN) ./diff_test_host
	$(HOST_RUN) ./notes_test_host
	$(HOST_RUN) ./unit_test_host
	@mkdir -p obj/host_scratch
	@cd obj/host_scratch && sh -c 'if command -v timeout >/dev/null 2>&1; then exec timeout 120 "../../$(APPS_SUITE_TEST)"; else exec "../../$(APPS_SUITE_TEST)"; fi'

# GB-085 concurrency gate: two `make host_tests` at once must both pass
# (per-process scratch dirs), and a shared-path override must FAIL it.
host_concurrency_gate:
	python3 tools/host/host_concurrency_gate.py
host_concurrency_gate_negative:
	python3 tools/host/host_concurrency_gate.py --negative

# --- Architecture Specific Targets ---

run_arm:
	$(MAKE) ARCH=arm run

run_intel:
	$(MAKE) ARCH=intel run

test_arm:
	bash tools/t3_tier.sh arm

test_intel:
	bash tools/t3_tier.sh intel

unit_tests_arm:
	$(MAKE) ARCH=arm MODE=unit_tests run

unit_tests_intel:
	$(MAKE) ARCH=intel MODE=unit_tests run

bigload_intel:
	$(MAKE) ARCH=intel MODE=bigload run

desktop_test_arm:
	ARCH=arm python3 ./run_desktop_test.py

desktop_test_intel:
	ARCH=intel python3 ./run_desktop_test.py

deploy_intel:
	$(MAKE) ARCH=intel disk.img
	@echo "Copying disk image to Proxmox server..."
	scp -C -o StrictHostKeyChecking=no -i ~/.ssh/mac_to_r1 disk.img root@192.168.10.174:/root/disk_205.raw
	@echo "Recreating VM 205 on Proxmox..."
	ssh -o StrictHostKeyChecking=no -i ~/.ssh/mac_to_r1 root@192.168.10.174 '\
		qm stop 205 || true; \
		qm destroy 205 --purge || true; \
		qm create 205 --name HobbyOSIntel --cores 8 --memory 4096 --net0 virtio,bridge=vmbr0 --machine q35; \
		qm importdisk 205 /root/disk_205.raw local-lvm; \
		qm set 205 --bios ovmf --efidisk0 local-lvm:0; \
		qm set 205 --serial0 socket; \
		qm set 205 --cpu host,phys-bits=host; \
		DISK_VOL=$$(grep "unused0:" /etc/pve/qemu-server/205.conf | grep -o "vm-205-disk-[0-9]\+"); \
		sed -i "/unused0:/d" /etc/pve/qemu-server/205.conf; \
		echo "args: -drive file=/dev/pve/$${DISK_VOL},if=none,id=disk0,format=raw -device nvme,drive=disk0,serial=1234,bootindex=1" >> /etc/pve/qemu-server/205.conf; \
		rm -f /root/disk_205.raw; \
		qm start 205'

deploy_run_intel: deploy_intel
	ssh -tt -o StrictHostKeyChecking=no -i ~/.ssh/mac_to_r1 root@192.168.10.174 "qm terminal 205"

# --- C style (see STYLE.md; enforced by tools/cstyle.py) ---
CSTYLE = python3 tools/cstyle.py

format:
	$(CSTYLE) format src

check-format:
	$(CSTYLE) check src

style-diff:
	$(CSTYLE) diff src

style-test:
	python3 tools/test_cstyle.py

# --- Merge gate (GB-005): one command to run the branch/PR gate -----------
# merge_gate.sh aggregates the test tiers (T1 host golden always; heavy QEMU
# T2/T3 tiers enabled via GATE_CI=1 / GATE_T2=1 / GATE_T3=1) into a single
# verdict; merge_gate_test.sh proves the DoD "a dummy branch fails -> passes
# the gate" (T3-verdict fail->pass + optional real T1 run).
merge_gate:
	bash tools/gate/merge_gate.sh

# --- Merge PR guard (GB-106): the production merge path calls the fail-closed
# guard before merging.  `make merge-pr PR=204` = run
# tools/board/merge_pr.sh (guard --merge; refuses to merge a PR whose GREEN graded
# sha is absent or != its current head, unless a written override is recorded and
# posted to the card).
.PHONY: merge-pr
merge-pr: FORCE_ARCH
	bash tools/board/merge_pr.sh $(PR)

merge_gate_ci:
	GATE_CI=1 bash tools/gate/merge_gate.sh

test_merge_gate:
	bash tools/gate/merge_gate_test.sh

# --- Two-arch parity CI (GB-044) ---
# Runs the per-arch check on ARCH=arm AND ARCH=intel, requires both green
# (gaming.md §2.3 parity), and enforces the GB-044 DoD "run window < best"
# via a PARITY_BUDGET_S baseline (fails when the window exceeds it). The
# self-test proves the verdict + budget (make test_parity_ci).
parity_ci:
	bash tools/gate/parity_ci.sh

test_parity_ci:
	bash tools/gate/parity_ci_test.sh

.PHONY: all clean run memtest fileio_test fork_test tests test test_run_raw unit_tests desktop_test desktop_test_build host_tests run_arm run_intel test_arm test_intel unit_tests_arm unit_tests_intel desktop_test_arm desktop_test_intel files_nav_test deploy_intel deploy_run_intel format check-format style-diff style-test merge_gate merge_gate_ci test_merge_gate parity_ci test_parity_ci cache_setup cold_vs_cached test_cache test_diff_harness bigload_intel t4_compat_parity t4_device_gate t4_device_selftest t4_device_check t4_w0_gate t4_w0_selftest t4_wave_gate t4_wave_selftest t4_glued_selftest t4_compat_selftest open_mode_scan uname_parity_host t3_pollbrk t3_nullentry
