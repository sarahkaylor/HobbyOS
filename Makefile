# Compiler and flags
CC = /opt/homebrew/opt/llvm/bin/clang
QEMU = /opt/homebrew/bin/qemu-system-aarch64

# Use --target=aarch64-none-elf to instruct clang to build for an AArch64 bare-metal target.
# -ffreestanding confirms that we don't have a standard library underneath us.
CFLAGS = -Wall -Wextra -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only

# -nostdlib prevents linking against stdlib startup files and libc.
# -T linker.ld points to our custom linker script.
# -fuse-ld=lld forces the use of the LLD linker installed alongside Clang.
LDFLAGS = -fuse-ld=lld -T linker.ld -nostdlib

# Target and objects
TARGET = hobbyos.elf
SRC_DIR = src/kernel
OBJ_DIR = obj

# Find all .c and .s files in the src/ directory
C_SRCS = $(wildcard $(SRC_DIR)/*.c)
ASM_SRCS = $(wildcard $(SRC_DIR)/*.s)

# Object files corresponding to the source files
C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(C_SRCS))
ASM_OBJS = $(patsubst $(SRC_DIR)/%.s, $(OBJ_DIR)/%.o, $(ASM_SRCS))
OBJS = $(ASM_OBJS) $(C_OBJS)

USER_CFLAGS = -Wall -Wextra -Isrc/user_include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only
USER_C = src/user/main.c src/user/libc.c
USER_OBJ = obj/user_main.o obj/user_libc.o
USER_BIN = user.bin

# Default rule: build the target
all: $(TARGET)

# Use explicit linker executable rather than via clang
LD = /opt/homebrew/bin/ld.lld
LDFLAGS = -T linker.ld

# The final linking step
# Combine the objects to create the ELF binary
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Rule to compile .c files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to compile .s files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.s
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to compile user objects
obj/user_%.o: src/user/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BIN): $(USER_OBJ)
	/opt/homebrew/bin/ld.lld -T src/user/linker.ld -o user.elf $(USER_OBJ)
	/opt/homebrew/opt/llvm/bin/llvm-objcopy -O binary user.elf $(USER_BIN)

disk.img: $(TARGET) $(USER_BIN)
	dd if=/dev/zero of=disk.img bs=1M count=512
	/opt/homebrew/sbin/mkfs.fat -F 16 disk.img
	/opt/homebrew/bin/mcopy -i disk.img $(USER_BIN) ::/USER.BIN

# Target to run the OS inside QEMU
run: disk.img
	$(QEMU) -M virt -cpu cortex-a53 -m 128M -kernel $(TARGET) -nographic -append "console=ttyAMA0" -drive if=none,file=disk.img,format=raw,id=hd0 -device virtio-blk-device,drive=hd0

# Target to exit QEMU properly
# Note: Ctrl+A, X exists QEMU nographic mode.

# Clean rule to remove build artifacts
clean:
	rm -rf $(OBJ_DIR) $(TARGET) hobbyos disk.img user.elf $(USER_BIN)

.PHONY: all clean run
