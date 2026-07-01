# Default architecture
ARCH ?= arm

# Compiler and flags
CC = /opt/homebrew/opt/llvm/bin/clang
LD = /opt/homebrew/bin/ld.lld
OBJCOPY = /opt/homebrew/opt/llvm/bin/llvm-objcopy

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
  QEMU = /opt/homebrew/bin/qemu-system-x86_64
  # Intel/AMD 64-bit compiler flags
  # Using standard bare-metal flags, disabling red zone and SSE
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include --target=x86_64-none-elf -ffreestanding -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx
  ARCH_DIR = src/kernel/arch/x64
  LDFLAGS = -T linker_x64.ld
  # QEMU parameters for x86_64: 8 cores, 3GB RAM, mounting disk.img as NVMe, booting with UEFI
  QEMU_CMD = $(QEMU) -M q35 -smp 8 -m 3072M -pflash /opt/homebrew/share/qemu/edk2-x86_64-code.fd -display cocoa -serial stdio -device pcie-root-port,id=pcie.1,bus=pcie.0,slot=1 -drive file=disk.img,format=raw,id=disk0,if=none -device nvme,drive=disk0,serial=1234,bus=pcie.1 -device edu -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 -action shutdown=poweroff $(QEMU_ARGS)
else
  # Default to ARM
  QEMU = /opt/homebrew/bin/qemu-system-aarch64
  CFLAGS = -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
  USER_CFLAGS = -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
  ARCH_DIR = src/kernel/arch/arm
  LDFLAGS = -T linker.ld
  # QEMU parameters for ARM: 8 cores, 2GB RAM, booting with UEFI
  QEMU_CMD = $(QEMU) -M virt -cpu cortex-a53 -smp 4 -m 2048M -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd -display cocoa -serial stdio -drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 -device virtio-gpu-device -device virtio-keyboard-device -device virtio-tablet-device -netdev user,id=net0 -device virtio-net-device,netdev=net0,mac=52:54:00:12:34:56 -semihosting -action shutdown=poweroff $(QEMU_ARGS)
endif

ifeq ($(MODE),test)
  CFLAGS += -DKERNEL_MODE_TEST
else ifeq ($(MODE),unit_tests)
  CFLAGS += -DKERNEL_MODE_UNIT_TEST
else ifeq ($(MODE),desktop_test)
  CFLAGS += -DKERNEL_MODE_DESKTOP_TEST
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
MEM_TEST_BIN = $(OBJ_DIR)/memtest.bin
FILE_IO_BIN = $(OBJ_DIR)/fileio_test.bin
CONSOLE_TEST_BIN = $(OBJ_DIR)/console_test.bin
SPAWN_TEST_BIN = $(OBJ_DIR)/spawntest.bin
FORK_TEST_BIN = $(OBJ_DIR)/fork_test.bin
HEAP_TEST_BIN = $(OBJ_DIR)/heap_test.bin
GRAPHICS_TEST_BIN = $(OBJ_DIR)/graphics.bin
SMP_TEST_BIN = $(OBJ_DIR)/smp_test.bin

PIPETEST_BIN = $(OBJ_DIR)/pipetest.bin
NETTEST_BIN = $(OBJ_DIR)/nettest.bin
TIMEOUT_BIN = $(OBJ_DIR)/timeout.bin
DESKTOP_BIN = $(OBJ_DIR)/desktop.bin
EDITOR_BIN = $(OBJ_DIR)/editor.bin
EDITOR_T_BIN = $(OBJ_DIR)/EDITOR_T.BIN
STRESS_TEST_BIN = $(OBJ_DIR)/stress.bin

SH_BIN = $(OBJ_DIR)/sh.bin
LS_BIN = $(OBJ_DIR)/ls.bin
CAT_BIN = $(OBJ_DIR)/cat.bin
GREP_BIN = $(OBJ_DIR)/grep.bin
LESS_BIN = $(OBJ_DIR)/less.bin
TAIL_BIN = $(OBJ_DIR)/tail.bin
HEAD_BIN = $(OBJ_DIR)/head.bin
SHELL_TEST_BIN = $(OBJ_DIR)/shtest.bin

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
$(OBJ_DIR)/user_%.o: src/user/%.c src/user_include/*.h src/include/*.h
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/mem_test.o: src/user/mem_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR) 
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/file_io_test.o: src/user/file_io_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR) 
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/console_test.o: src/user/console_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/fork_test.o: src/user/fork_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/spawn_test.o: src/user/spawn_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/heap_test.o: src/user/heap_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/graphics_test.o: src/user/graphics_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/smp_test.o: src/user/smp_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_pipe_test.o: src/user/pipe_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/stress_test.o: src/user/stress_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_graphics.o: src/user/graphics/graphics.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_window.o: src/user/graphics/window.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/desktop.o: src/user/desktop.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/editor.o: src/user/editor.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_editor_test.o: src/user/editor_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_net_test.o: src/user/net_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/user_desktop_test_wrapper.o: src/user/desktop.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -Dmain=desktop_main -DDESKTOP_TEST_WRAPPER -c $< -o $@

$(MEM_TEST_BIN): $(OBJ_DIR)/mem_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/memtest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/memtest.elf $(MEM_TEST_BIN)

$(FILE_IO_BIN): $(OBJ_DIR)/file_io_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/fileio_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/fileio_test.elf $(FILE_IO_BIN)

$(CONSOLE_TEST_BIN): $(OBJ_DIR)/console_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/console_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/console_test.elf $(CONSOLE_TEST_BIN)

$(FORK_TEST_BIN): $(OBJ_DIR)/fork_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/fork_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/fork_test.elf $(FORK_TEST_BIN)

$(HEAP_TEST_BIN): $(OBJ_DIR)/heap_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/heap_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/heap_test.elf $(HEAP_TEST_BIN)

$(SPAWN_TEST_BIN): $(OBJ_DIR)/spawn_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/spawn_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/spawn_test.elf $(SPAWN_TEST_BIN)

$(GRAPHICS_TEST_BIN): $(OBJ_DIR)/graphics_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_graphics.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/graphics_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/graphics_test.elf $(GRAPHICS_TEST_BIN)

$(SMP_TEST_BIN): $(OBJ_DIR)/smp_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/smp_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/smp_test.elf $(SMP_TEST_BIN)

$(PIPETEST_BIN): $(OBJ_DIR)/user_pipe_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/pipe_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/pipe_test.elf $(PIPETEST_BIN)

$(STRESS_TEST_BIN): $(OBJ_DIR)/stress_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/stress.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/stress.elf $(STRESS_TEST_BIN)

$(NETTEST_BIN): $(OBJ_DIR)/user_net_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/net_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/net_test.elf $(NETTEST_BIN)

$(OBJ_DIR)/user_net_timeout_test.o: src/user/net_timeout_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(TIMEOUT_BIN): $(OBJ_DIR)/user_net_timeout_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/net_timeout_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/net_timeout_test.elf $(TIMEOUT_BIN)

$(DESKTOP_BIN): $(OBJ_DIR)/desktop.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/desktop_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/desktop_test.elf $(DESKTOP_BIN)

$(EDITOR_BIN): $(OBJ_DIR)/editor.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/editor.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/editor.elf $(EDITOR_BIN)

$(EDITOR_T_BIN): $(OBJ_DIR)/user_editor_test.o $(OBJ_DIR)/user_desktop_test_wrapper.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/user_graphics.o $(OBJ_DIR)/user_window.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/editor_test.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/editor_test.elf $(EDITOR_T_BIN)

$(OBJ_DIR)/sh.o: src/user/sh.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/ls.o: src/user/ls.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/cat.o: src/user/cat.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/grep.o: src/user/grep.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/less.o: src/user/less.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/tail.o: src/user/tail.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(OBJ_DIR)/head.o: src/user/head.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SH_BIN): $(OBJ_DIR)/sh.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/sh.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/sh.elf $(SH_BIN)

$(LS_BIN): $(OBJ_DIR)/ls.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/ls.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/ls.elf $(LS_BIN)

$(CAT_BIN): $(OBJ_DIR)/cat.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/cat.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/cat.elf $(CAT_BIN)

$(GREP_BIN): $(OBJ_DIR)/grep.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/grep.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/grep.elf $(GREP_BIN)

$(LESS_BIN): $(OBJ_DIR)/less.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/less.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/less.elf $(LESS_BIN)

$(TAIL_BIN): $(OBJ_DIR)/tail.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/tail.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/tail.elf $(TAIL_BIN)

$(HEAD_BIN): $(OBJ_DIR)/head.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/head.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/head.elf $(HEAD_BIN)

$(OBJ_DIR)/shell_test.o: src/user/shell_test.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SHELL_TEST_BIN): $(OBJ_DIR)/shell_test.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/shtest.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/shtest.elf $(SHELL_TEST_BIN)


disk.img: $(TARGET) $(MEM_TEST_BIN) $(FILE_IO_BIN) $(CONSOLE_TEST_BIN) $(FORK_TEST_BIN) $(HEAP_TEST_BIN) $(SPAWN_TEST_BIN) $(GRAPHICS_TEST_BIN) $(SMP_TEST_BIN) $(PIPETEST_BIN) $(NETTEST_BIN) $(TIMEOUT_BIN) $(DESKTOP_BIN) $(EDITOR_BIN) $(EDITOR_T_BIN) $(STRESS_TEST_BIN) $(SH_BIN) $(LS_BIN) $(CAT_BIN) $(GREP_BIN) $(LESS_BIN) $(TAIL_BIN) $(HEAD_BIN) $(SHELL_TEST_BIN) $(MODE_FILE)
	dd if=/dev/zero of=disk.img bs=1M count=64
	/opt/homebrew/sbin/mkfs.fat -F 16 disk.img 
	/opt/homebrew/bin/mmd -i disk.img ::/EFI
	/opt/homebrew/bin/mmd -i disk.img ::/EFI/BOOT
	/opt/homebrew/bin/mmd -i disk.img ::/boot
	/opt/homebrew/bin/mcopy -i disk.img bootloader/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	/opt/homebrew/bin/mcopy -i disk.img bootloader/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
ifeq ($(ARCH),arm)
	@echo "timeout: 0" > obj/$(ARCH)/limine.conf
	@echo "default_entry: 1" >> obj/$(ARCH)/limine.conf
	@echo "" >> obj/$(ARCH)/limine.conf
	@echo "/HobbyOS (ARM AArch64)" >> obj/$(ARCH)/limine.conf
	@echo "protocol: linux" >> obj/$(ARCH)/limine.conf
	@echo "path: boot():/boot/hobbyos.bin" >> obj/$(ARCH)/limine.conf
	/opt/homebrew/bin/mcopy -i disk.img obj/$(ARCH)/limine.conf ::/boot/limine.conf
	$(OBJCOPY) -O binary $(TARGET) hobbyos.bin
	/opt/homebrew/bin/mcopy -i disk.img hobbyos.bin ::/boot/hobbyos.bin
else
	@echo "timeout: 0" > obj/$(ARCH)/limine.conf
	@echo "default_entry: 1" >> obj/$(ARCH)/limine.conf
	@echo "" >> obj/$(ARCH)/limine.conf
	@echo "/HobbyOS (Intel x86_64)" >> obj/$(ARCH)/limine.conf
	@echo "protocol: multiboot1" >> obj/$(ARCH)/limine.conf
	@echo "path: boot():/boot/hobbyos.elf" >> obj/$(ARCH)/limine.conf
	/opt/homebrew/bin/mcopy -i disk.img obj/$(ARCH)/limine.conf ::/boot/limine.conf
	/opt/homebrew/bin/mcopy -i disk.img $(TARGET) ::/boot/hobbyos.elf
endif
	/opt/homebrew/bin/mcopy -i disk.img $(MEM_TEST_BIN) ::/MEMTEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(FILE_IO_BIN) ::/FILEIO.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(CONSOLE_TEST_BIN) ::/CONSOLE.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(FORK_TEST_BIN) ::/FORKTEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(HEAP_TEST_BIN) ::/HEAPTEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(SPAWN_TEST_BIN) ::/SPAWN.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(GRAPHICS_TEST_BIN) ::/GRAPHICS.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(SMP_TEST_BIN) ::/SMPTEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(PIPETEST_BIN) ::/PIPETEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(NETTEST_BIN) ::/NETTEST.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(TIMEOUT_BIN) ::/TIMEOUT.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(DESKTOP_BIN) ::/DESKTOP.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(EDITOR_BIN) ::/EDITOR.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(EDITOR_T_BIN) ::/EDITOR_T.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(STRESS_TEST_BIN) ::/STRESS.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(SH_BIN) ::/SH.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(LS_BIN) ::/LS.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(CAT_BIN) ::/CAT.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(GREP_BIN) ::/GREP.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(LESS_BIN) ::/LESS.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(TAIL_BIN) ::/TAIL.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(HEAD_BIN) ::/HEAD.BIN
	/opt/homebrew/bin/mcopy -i disk.img $(SHELL_TEST_BIN) ::/SHTEST.BIN
	echo "HobbyOS Terminal Test File" > SHTEST.TXT
	echo "This is line number two." >> SHTEST.TXT
	echo "Line three is right here." >> SHTEST.TXT
	echo "Fourth line has some interesting keywords like hello world." >> SHTEST.TXT
	echo "Line five is the last line of this small test document." >> SHTEST.TXT
	touch TEST1.TXT TEST2.TXT TEST3.TXT TEST4.TXT
	/opt/homebrew/bin/mcopy -i disk.img SHTEST.TXT ::/SHTEST.TXT
	/opt/homebrew/bin/mcopy -i disk.img TEST1.TXT ::/TEST1.TXT
	/opt/homebrew/bin/mcopy -i disk.img TEST2.TXT ::/TEST2.TXT
	/opt/homebrew/bin/mcopy -i disk.img TEST3.TXT ::/TEST3.TXT
	/opt/homebrew/bin/mcopy -i disk.img TEST4.TXT ::/TEST4.TXT
	rm -f SHTEST.TXT TEST1.TXT TEST2.TXT TEST3.TXT TEST4.TXT

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

desktop_test:
	python3 ./run_desktop_test.py

# --- Host Compatibility Build Targets ---
HOST_CC = clang
HOST_CFLAGS = -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -DHOST_TEST

obj/host_%.o: src/host/%.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

obj/host_user_desktop.o: src/user/desktop.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -Dmain=desktop_main -c $< -o $@

obj/host_user_%.o: src/user/%.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

obj/host_user_graphics_%.o: src/user/graphics/%.c
	@mkdir -p obj
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

EDITOR_HOST = EDITOR.BIN_host

$(EDITOR_HOST): obj/host_user_editor.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

EDITOR_TEST_BIN = editor_test_host
$(EDITOR_TEST_BIN): obj/host_editor_test.o obj/host_user_desktop.o obj/host_user_graphics_graphics.o obj/host_user_graphics_window.o obj/host_compat.o
	$(HOST_CC) -o $@ $^

host_tests: $(EDITOR_HOST) $(EDITOR_TEST_BIN)
	./$(EDITOR_TEST_BIN)

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

.PHONY: all clean run memtest fileio_test fork_test tests test unit_tests desktop_test host_tests run_arm run_intel test_arm test_intel unit_tests_arm unit_tests_intel desktop_test_arm desktop_test_intel deploy_intel deploy_run_intel
