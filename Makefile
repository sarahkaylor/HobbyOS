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
OBJ_DIR = obj/$(ARCH)
MODE_FILE = $(OBJ_DIR)/.mode

# Determine if the mode has changed and update the tracker file
CURRENT_MODE := $(shell [ -f $(MODE_FILE) ] && cat $(MODE_FILE) || echo none)
ifneq ($(CURRENT_MODE),$(MODE))
$(shell mkdir -p $(OBJ_DIR) && echo $(MODE) > $(MODE_FILE))
endif

ifeq ($(ARCH),intel)
  QEMU = qemu-system-x86_64
  # Intel/AMD 64-bit compiler flags
  # Using standard bare-metal flags, disabling red zone and SSE
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx
  ARCH_DIR = src/kernel/arch/x64
  LDFLAGS = -T linker_x64.ld
  # QEMU parameters for x86_64: 8 cores, 3GB RAM, mounting disk.img as NVMe, booting with UEFI
  QEMU_CMD = $(QEMU) -M q35 -smp 8 -m 6144M -pflash $(EDK2_X86_64) -display $(QEMU_DISPLAY) -serial stdio -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 -drive file=disk.img,format=raw,id=disk0,if=none -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 -action shutdown=poweroff $(QEMU_ARGS)
else
  # Default to ARM
  QEMU = qemu-system-aarch64
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
  ARCH_DIR = src/kernel/arch/arm
  LDFLAGS = -T linker.ld
  # QEMU parameters for ARM: 8 cores, 2GB RAM, booting with UEFI
  QEMU_CMD = $(QEMU) -M virt -cpu cortex-a53 -smp 8 -m 8192M -accel tcg,thread=multi -bios $(EDK2_AARCH64) -display $(QEMU_DISPLAY) -serial stdio -drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 -semihosting -action shutdown=poweroff $(QEMU_ARGS)
endif

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
else
  CFLAGS += -DKERNEL_MODE_DESKTOP
endif

ifneq ($(MODE),desktop)
  QEMU_ARGS += -display none
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
USER_HDRS = src/user_include/*.h src/user_include/graphics/*.h src/libc/include/*.h
MEM_TEST_BIN = $(OBJ_DIR)/memtest.bin
FILE_IO_BIN = $(OBJ_DIR)/fileio_test.bin
CONSOLE_BIN = $(OBJ_DIR)/console.bin
SPAWN_TEST_BIN = $(OBJ_DIR)/spawntest.bin
FORK_TEST_BIN = $(OBJ_DIR)/fork_test.bin
HEAP_TEST_BIN = $(OBJ_DIR)/heap_test.bin
GRAPHICS_TEST_BIN = $(OBJ_DIR)/graphics.bin
SMP_TEST_BIN = $(OBJ_DIR)/smp_test.bin

PIPETEST_BIN = $(OBJ_DIR)/pipetest.bin
NETTEST_BIN = $(OBJ_DIR)/nettest.bin
TIMEOUT_BIN = $(OBJ_DIR)/timeout.bin
NFSTEST_BIN = $(OBJ_DIR)/nfstest.bin
DESKTOP_BIN = $(OBJ_DIR)/desktop.bin
EDITOR_BIN = $(OBJ_DIR)/editor.bin
EDITOR_T_BIN = $(OBJ_DIR)/EDITOR_T.BIN
APPS_T_BIN = $(OBJ_DIR)/APPS_T.BIN
PONG_T_BIN = $(OBJ_DIR)/PONG_T.BIN
STRESS_TEST_BIN = $(OBJ_DIR)/stress.bin
ERRNO_TEST_BIN = $(OBJ_DIR)/errtest.bin
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
CP_BIN = $(OBJ_DIR)/cp.bin
RM_BIN = $(OBJ_DIR)/rm.bin
MV_BIN = $(OBJ_DIR)/mv.bin
TOUCH_BIN = $(OBJ_DIR)/touch.bin
WC_BIN = $(OBJ_DIR)/wc.bin
SED_BIN = $(OBJ_DIR)/sed.bin
# GNU sed 4.8 port: seven core sources + the vendored gnulib subset they
# pull in (src/user/sed/config.h is the port's generated config header).
SED_CORE_NAMES = sed compile debug execute mbcs regexp utils
SED_GNULIB_NAMES = acl-errno-valid acl-internal basename-lgpl c-ctype c-strcasecmp c-strncasecmp close-stream closeout copy-acl dfa dirname-lgpl exitfail get-permissions localcharset localeinfo obstack progname qcopy-acl qset-acl quotearg set-acl set-permissions strverscmp version-etc version-etc-fsf xalloc-die xmalloc
SED_OBJS = $(addprefix $(OBJ_DIR)/sed_,$(addsuffix .o,$(SED_CORE_NAMES))) $(addprefix $(OBJ_DIR)/sedg_,$(addsuffix .o,$(SED_GNULIB_NAMES)))
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
MD5SUM_BIN = $(OBJ_DIR)/md5sum.bin
MD5SUM_T_BIN = $(OBJ_DIR)/md5sum_test.bin
TAC_BIN = $(OBJ_DIR)/tac.bin
TACTEST_BIN = $(OBJ_DIR)/tac_test.bin
CMP_BIN = $(OBJ_DIR)/cmp.bin
CMPTEST_BIN = $(OBJ_DIR)/cmp_test.bin
TAILTEST_BIN = $(OBJ_DIR)/tail_test.bin
PROCCHLD_BIN = $(OBJ_DIR)/proc_child.bin
PROCTEST_BIN = $(OBJ_DIR)/proc_test.bin
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

# --- New desktop applications (10 GUI apps) ---
FILES_BIN = $(OBJ_DIR)/files.bin
CALC_BIN = $(OBJ_DIR)/calc.bin
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
# Combine the objects to create the ELF binary
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
ifeq ($(ARCH),intel)
	$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $@
endif

# Rule to compile .c files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c src/include/*.h $(MODE_FILE)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

$(OBJ_DIR)/user_graphics.o: src/user/graphics/graphics.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_window.o: src/user/graphics/window.c $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/desktop.o: src/user/desktop.c $(USER_LIBC) src/user_include/*.h src/user_include/graphics/*.h
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

$(SMP_TEST_BIN): $(OBJ_DIR)/smp_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/smp_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/smp_test.elf $(SMP_TEST_BIN)

$(PIPETEST_BIN): $(OBJ_DIR)/user_pipe_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/pipe_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/pipe_test.elf $(PIPETEST_BIN)

$(STRESS_TEST_BIN): $(OBJ_DIR)/stress_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/stress.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/stress.elf $(STRESS_TEST_BIN)

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

$(OBJ_DIR)/libc.a: $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/crt0.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/libc_ctype.o $(OBJ_DIR)/libc_stdlib.o $(OBJ_DIR)/libc_stdio.o $(OBJ_DIR)/libc_file.o $(OBJ_DIR)/libc_getopt.o $(OBJ_DIR)/libc_error.o $(OBJ_DIR)/libc_stat.o $(OBJ_DIR)/libc_signal.o $(OBJ_DIR)/libc_mman.o $(OBJ_DIR)/libc_regex.o $(OBJ_DIR)/libc_langinfo.o $(OBJ_DIR)/libc_wchar.o $(OBJ_DIR)/libc_wctype.o $(OBJ_DIR)/libc_locale.o $(OBJ_DIR)/libc_selinux.o $(OBJ_DIR)/libc_dirent.o
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

$(DESKTOP_BIN): $(OBJ_DIR)/desktop.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/desktop_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/desktop_test.elf $(DESKTOP_BIN)

$(EDITOR_BIN): $(OBJ_DIR)/editor.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_dialog.o $(OBJ_DIR)/user_filedialog.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/editor.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/editor.elf $(EDITOR_BIN)

$(EDITOR_T_BIN): $(OBJ_DIR)/user_editor_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
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

$(APPS_T_BIN): $(OBJ_DIR)/user_apps_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics_stats.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/apps_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/apps_test.elf $(APPS_T_BIN)

FILEDIALOG_ARROW_T_BIN = $(OBJ_DIR)/FILEDIAL.BIN
$(OBJ_DIR)/user_filedialog_arrow_test.o: src/user/filedialog_arrow_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
$(FILEDIALOG_ARROW_T_BIN): $(OBJ_DIR)/user_filedialog_arrow_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/filedialog_arrow_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/filedialog_arrow_test.elf $(FILEDIALOG_ARROW_T_BIN)

DIALOG_TEST_BIN = $(OBJ_DIR)/DIALOG_T.BIN
$(DIALOG_TEST_BIN): $(OBJ_DIR)/user_dialog_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_dialog.o $(OBJ_DIR)/user_filedialog.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/dialog_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/dialog_test.elf $(DIALOG_TEST_BIN)

$(OBJ_DIR)/user_pong_test.o: src/user/pong_test.c $(USER_LIBC) $(USER_HDRS)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -DPONG_TEST_WRAPPER -c $< -o $@

$(PONG_T_BIN): $(OBJ_DIR)/user_pong_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
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

$(SH_BIN): $(OBJ_DIR)/sh.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/libc_string.o
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

disk.img: $(TARGET) $(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(SPAWN_TEST_BIN) $(GRAPHICS_TEST_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(TIMEOUT_BIN) $(NFSTEST_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(DIALOG_TEST_BIN) $(PONG_T_BIN) $(STRESS_TEST_BIN) $(ERRNO_TEST_BIN) $(HELLO_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(PS_BIN) $(FREE_BIN) $(UPTIME_BIN) $(KILL_BIN) $(CP_BIN) $(RM_BIN) $(MV_BIN) $(TOUCH_BIN) $(WC_BIN) $(SED_BIN) $(HEDGNU_BIN) $(WCTEST_BIN) $(CUTTEST_BIN) $(TR_BIN) $(TRTEST_BIN) $(PASTE_BIN) $(PASTE_T_BIN) $(FOLD_BIN) $(FOLDTEST_BIN) $(NL_BIN) $(NLTEST_BIN) $(COMM_BIN) $(COMMTEST_BIN) $(TSORT_BIN) $(TSORT_T_BIN) $(EXPAND_BIN) $(EXPAND_T_BIN) $(UNEXPAND_BIN) $(UNEXPAND_T_BIN) $(CKSUM_BIN) $(CKSUM_T_BIN) $(MD5SUM_BIN) $(MD5SUM_T_BIN) $(TAC_BIN) $(TACTEST_BIN) $(CMP_BIN) $(CMPTEST_BIN) $(REGTEST_BIN) $(SEDTEST_BIN) $(GREPTEST_BIN) $(SUBPRB_BIN) $(PIPEPROBE_BIN) $(HEDTEST_BIN) $(TAILGN_BIN) $(CUT_BIN) $(TAILTEST_BIN) $(PROCCHLD_BIN) $(PROCTEST_BIN) $(LKSTEST_BIN) $(SORT_BIN) $(UNIQ_BIN) $(PING_BIN) $(NC_BIN) $(IFCONFIG_BIN) $(SHELL_TEST2_BIN) $(MKDIR_BIN) $(SHELL_TEST3_BIN) $(PONG_BIN) $(MILLIPEDE_BIN) $(FILEDIALOG_ARROW_T_BIN) $(MONITOR_BIN) $(MONITOR_TEST_BIN) $(DESKTOP_APP_BINS) $(APPS_T_BIN) $(MODE_FILE)
	dd if=/dev/zero of=disk.img bs=1M count=64
	$(MKFS_FAT) -F 16 disk.img 
	$(MMD) -i disk.img ::/EFI
	$(MMD) -i disk.img ::/EFI/BOOT
	$(MMD) -i disk.img ::/boot
	$(MMD) -i disk.img ::/home
	$(MMD) -i disk.img ::/nfs
	$(MMD) -i disk.img ::/mnt
	$(MMD) -i disk.img ::/TESTDIR
	$(MMD) -i disk.img ::/tmp
	$(MCOPY) -i disk.img tests/fixtures/E2E.TXT ::/E2E.TXT
	$(MCOPY) -i disk.img tests/fixtures/DRAGME.TXT ::/DRAGME.TXT
	$(MCOPY) -i disk.img bootloader/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(MCOPY) -i disk.img bootloader/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
ifeq ($(ARCH),arm)
	@echo "timeout: 0" > obj/$(ARCH)/limine.conf
	@echo "default_entry: 1" >> obj/$(ARCH)/limine.conf
	@echo "" >> obj/$(ARCH)/limine.conf
	@echo "/HobbyOS (ARM AArch64)" >> obj/$(ARCH)/limine.conf
	@echo "protocol: linux" >> obj/$(ARCH)/limine.conf
	@echo "path: boot():/boot/hobbyos.bin" >> obj/$(ARCH)/limine.conf
	$(MCOPY) -i disk.img obj/$(ARCH)/limine.conf ::/boot/limine.conf
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
	$(MCOPY) -i disk.img $(SMP_TEST_BIN) ::/SMPTEST.BIN
	$(MCOPY) -i disk.img $(PIPETEST_BIN) ::/PIPETEST.BIN
	$(MCOPY) -i disk.img $(NETTEST_BIN) ::/NETTEST.BIN
	$(MCOPY) -i disk.img $(TIMEOUT_BIN) ::/TIMEOUT.BIN
	$(MCOPY) -i disk.img $(NFSTEST_BIN) ::/NFSTEST.BIN
	$(MCOPY) -i disk.img $(DESKTOP_BIN) ::/DESKTOP.BIN
	$(MCOPY) -i disk.img $(EDITOR_BIN) ::/EDITOR.BIN
	$(MCOPY) -i disk.img $(EDITOR_T_BIN) ::/EDITOR_T.BIN
	$(MCOPY) -i disk.img $(APPS_T_BIN) ::/APPS_T.BIN
	$(MCOPY) -i disk.img $(FILEDIALOG_ARROW_T_BIN) ::/FILEDIAL.BIN
	$(MCOPY) -i disk.img $(DIALOG_TEST_BIN) ::/DIALOG_T.BIN
	$(MCOPY) -i disk.img $(PONG_T_BIN) ::/PONG_T.BIN
	$(MCOPY) -i disk.img $(STRESS_TEST_BIN) ::/STRESS.BIN
	$(MCOPY) -i disk.img $(ERRNO_TEST_BIN) ::/ERRTEST.BIN
	$(MCOPY) -i disk.img $(HELLO_BIN) ::/HELLO.BIN
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
	$(MCOPY) -i disk.img $(CP_BIN) ::/CP.BIN
	$(MCOPY) -i disk.img $(RM_BIN) ::/RM.BIN
	$(MCOPY) -i disk.img $(MV_BIN) ::/MV.BIN
	$(MCOPY) -i disk.img $(TOUCH_BIN) ::/TOUCH.BIN
	$(MCOPY) -i disk.img $(WC_BIN) ::/WC.BIN
	$(MCOPY) -i disk.img $(SED_BIN) ::/SED.BIN
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
	$(MCOPY) -i disk.img $(FILES_BIN) ::/FILES.BIN
	$(MCOPY) -i disk.img $(CALC_BIN) ::/CALC.BIN
	$(MCOPY) -i disk.img $(CLOCK_BIN) ::/CLOCK.BIN
	$(MCOPY) -i disk.img $(SYSMON_BIN) ::/SYSMON.BIN
	$(MCOPY) -i disk.img $(HEX_BIN) ::/HEX.BIN
	$(MCOPY) -i disk.img $(TASKS_BIN) ::/TASKS.BIN
	$(MCOPY) -i disk.img $(FIND_BIN) ::/FIND.BIN
	$(MCOPY) -i disk.img $(DIFF_BIN) ::/DIFF.BIN
	$(MCOPY) -i disk.img $(NOTES_BIN) ::/NOTES.BIN
	$(MCOPY) -i disk.img $(UNIT_BIN) ::/UNIT.BIN
	$(MCOPY) -i disk.img $(MONITOR_BIN) ::/MONITOR.BIN
	$(MCOPY) -i disk.img $(MONITOR_TEST_BIN) ::/MONITORT.BIN
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
	rm -f SHTEST.TXT TEST1.TXT TEST2.TXT TEST3.TXT TEST4.TXT SORT.TXT

# Target to run the OS inside QEMU
run: $(TARGET) disk.img
	$(QEMU_CMD)

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

test:
	$(MAKE) MODE=test run

unit_tests:
	$(MAKE) MODE=unit_tests run

desktop_test_run:
	$(MAKE) MODE=desktop_test run

pong_test_run:
	$(MAKE) MODE=pong_test run

filedialog_test_run:
	$(MAKE) MODE=filedialog_test run

filedialog_test:
	python3 ./run_filedialog_test.py

desktop_test:
	python3 ./run_desktop_test.py

desktop_apps_test:
	python3 ./run_desktop_apps_test.py

files_nav_test:
	python3 ./run_files_nav_test.py

apps_test_run:
	$(MAKE) MODE=apps_test run

apps_test:
	python3 ./run_apps_test.py

# --- Host Compatibility Build Targets ---
HOST_CC = clang
HOST_CFLAGS = -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -DHOST_TEST

obj/host_%.o: src/host/%.c src/user_include/*.h src/user_include/graphics/*.h
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

# Cross-application host suite: every desktop app in one binary, in-process
# checks plus forked end-to-end runs on pipes. Single TU (includes the app
# sources), so it is built directly from src/host/apps_suite_test.c.
# Run from a scratch cwd: tasks/notes persist TODO.TXT / NOTES.TXT there.
APPS_SUITE_TEST = apps_suite_test_host
$(APPS_SUITE_TEST): src/host/apps_suite_test.c src/host/compat.c src/user/gui.c src/user/dialog.c src/user/filedialog.c
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/host/apps_suite_test.c src/host/compat.c src/user/gui.c src/user/dialog.c src/user/filedialog.c -o $@

# NFS protocol codecs against real nfsd fixtures (src/host/nfs_fixtures.h,
# regenerated with tools/capture_nfs_fixtures.py).  Single TU with the kernel
# codec source: no kernel dependencies.
NFS_PROTO_TEST = nfs_proto_test_host
$(NFS_PROTO_TEST): src/host/nfs_proto_test.c src/kernel/nfs_proto.c src/host/nfs_fixtures.h
	$(HOST_CC) -Isrc/include $(HOST_CFLAGS) src/kernel/nfs_proto.c src/host/nfs_proto_test.c -o $@

# Console app host tests: both roles (interactive shell handoff + smoke).
CONSOLE_APP_TEST = console_test_host
$(CONSOLE_APP_TEST): obj/host_console_test.o obj/host_compat.o obj/host_user_gui.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$(HOST_CC) -o $@ $^

PONG_TEST_BIN = pong_test_host
$(PONG_TEST_BIN): obj/host_pong_test.o obj/host_user_graphics_graphics.o obj/host_compat.o
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

# The ported wc built for the host: ours except getopt_long (hb_* via our
# getopt.h) and error() (host_hb_error.o); everything else resolves to
# glibc.  wc_host is raced byte-for-byte against coreutils in
# src/host/wc_parity.sh.
WC_HOST = obj/wc_host
obj/host_wc.o: src/user/wc.c $(USER_HDRS) src/libc/include/*.h
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/libc/include -c $< -o $@
$(WC_HOST): obj/host_wc.o obj/host_hb_getopt.o obj/host_hb_error.o
	$(HOST_CC) -o $@ $^

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

# Each app host test includes its app's .c directly and links the toolkit,
# dialogs and host compatibility layer.
define HOST_APP_TEST_RULE
$(1)_test_host: obj/host_$(1)_test.o obj/host_compat.o obj/host_user_gui.o obj/host_user_dialog.o obj/host_user_filedialog.o
	$$(HOST_CC) -o $$@ $$^
endef

$(foreach app,$(DESKTOP_APP_NAMES),$(eval $(call HOST_APP_TEST_RULE,$(app))))

HOST_APP_TEST_BINS = $(foreach app,$(DESKTOP_APP_NAMES),$(app)_test_host)

# Wrap host test execution so a hung test fails the build instead of hanging
# it. On macOS without coreutils this falls back to an unwrapped run.
HOST_RUN = @sh -c 'if command -v timeout >/dev/null 2>&1; then exec timeout 40 "$$@"; else exec "$$@"; fi' sh

host_tests: $(EDITOR_HOST) $(EDITOR_TEST_BIN) $(DESKTOP_MENU_TEST) $(DESKTOP_DRAG_TEST) $(DESKTOP_DAMAGE_TEST) $(APPS_SUITE_TEST) $(NFS_PROTO_TEST) $(CONSOLE_APP_TEST) $(PONG_TEST_BIN) $(DIALOG_ARROW_TEST) $(GUI_TEST) $(ERRNO_TEST) $(GRAPHICS_LIB_TEST) $(WINDOW_DAMAGE_TEST) $(WINDOW_TEXT_TEST) $(STRING_TEST) $(CTYPE_TEST) $(STDLIB_TEST) $(REALLOC_TEST) $(PRINTF_TEST) $(HEADERS_TEST) $(GETOPT_TEST) $(REGEX_TEST) $(LANGINFO_TEST) $(WC_PARITY) $(HEAD_PARITY) $(TAIL_PARITY) $(CUT_PARITY) $(TR_PARITY) $(PASTE_PARITY) $(FOLD_PARITY) $(NL_PARITY) $(COMM_PARITY) $(TSORT_PARITY) $(EXPAND_PARITY) $(UNEXPAND_PARITY) $(CKSUM_PARITY) $(MD5SUM_PARITY) $(TAC_PARITY) $(CMP_PARITY_STRICT) $(HOST_APP_TEST_BINS)
	$(HOST_RUN) ./$(EDITOR_TEST_BIN)
	$(HOST_RUN) ./$(DESKTOP_MENU_TEST)
	$(HOST_RUN) ./$(DESKTOP_DRAG_TEST)
	$(HOST_RUN) ./$(DESKTOP_DAMAGE_TEST)
	$(HOST_RUN) ./$(NFS_PROTO_TEST)
	$(HOST_RUN) ./$(CONSOLE_APP_TEST)
	$(HOST_RUN) ./$(DIALOG_ARROW_TEST)
	$(HOST_RUN) ./$(PONG_TEST_BIN)
	$(HOST_RUN) ./$(GUI_TEST)
	$(HOST_RUN) ./$(ERRNO_TEST)
	$(HOST_RUN) ./$(GRAPHICS_LIB_TEST)
	$(HOST_RUN) ./$(WINDOW_DAMAGE_TEST)
	$(HOST_RUN) ./$(WINDOW_TEXT_TEST)
	$(HOST_RUN) ./$(STRING_TEST)
	$(HOST_RUN) ./$(CTYPE_TEST)
	$(HOST_RUN) ./$(STDLIB_TEST)
	$(HOST_RUN) ./$(REALLOC_TEST)
	$(HOST_RUN) ./$(PRINTF_TEST)
	$(HOST_RUN) ./$(HEADERS_TEST)
	$(HOST_RUN) ./$(GETOPT_TEST)
	$(HOST_RUN) ./$(REGEX_TEST)
	$(HOST_RUN) ./$(LANGINFO_TEST)
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

# --- Architecture Specific Targets ---

run_arm:
	$(MAKE) ARCH=arm run

run_intel:
	$(MAKE) ARCH=intel run

test_arm:
	$(MAKE) ARCH=arm MODE=test run

test_intel:
	$(MAKE) ARCH=intel MODE=test run

unit_tests_arm:
	$(MAKE) ARCH=arm MODE=unit_tests run

unit_tests_intel:
	$(MAKE) ARCH=intel MODE=unit_tests run

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

.PHONY: all clean run memtest fileio_test fork_test tests test unit_tests desktop_test host_tests run_arm run_intel test_arm test_intel unit_tests_arm unit_tests_intel desktop_test_arm desktop_test_intel files_nav_test deploy_intel deploy_run_intel format check-format style-diff style-test
