# w10-fstime synthetic 5-TU project (P2).  Explicit rules only (FAT16
# uppercase short names break make pattern rules); SHELL must be the in-OS sh.
SHELL := /SH.BIN

.PHONY: all
all: B.O C.O D.O E.O F.O
	@echo P2-BUILD-ALL

B.O: B.C
	@echo P2-COMPILING-B
	/CP.BIN B.C B.O

C.O: C.C
	@echo P2-COMPILING-C
	/CP.BIN C.C C.O

D.O: D.C
	@echo P2-COMPILING-D
	/CP.BIN D.C D.O

E.O: E.C
	@echo P2-COMPILING-E
	/CP.BIN E.C E.O

F.O: F.C
	@echo P2-COMPILING-F
	/CP.BIN F.C F.O
