# w10-fstime synthetic 2-TU project (P1).
# Explicit rules ONLY: in-OS GNU make pattern rules (%.o: %.c) do not match
# FAT16's uppercase short names.  SHELL must resolve to the in-OS sh.
SHELL := /SH.BIN

.PHONY: all
all: A.O
	@echo P1-BUILD-ALL

A.O: A.C
	@echo P1-COMPILING-A
	/CP.BIN A.C A.O
