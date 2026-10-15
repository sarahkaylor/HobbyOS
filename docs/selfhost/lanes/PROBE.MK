SHELL := /TOOLS/SH.BIN
MAKEFLAGS += -r -R
.PHONY: check
check:
	/ARGVPROBE.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/obj/arm/check.o
	@echo "[SELFHOST] probe-make-done rc=$$?"
