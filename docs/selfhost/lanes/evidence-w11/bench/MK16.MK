    # MK16.MK — w11-makej synthetic 16-TU project (explicit rules).
    #
    # Contract (docs/selfhost/acceptance.md + w2-make-report): in-OS make
    # must use EXPLICIT rules — the dir cache is empty (no opendir on FAT),
    # so pattern/implicit rules and $(wildcard) cannot match; explicit
    # stat-based rules work.  Names are 8.3-safe.  SHELL and absolute
    # /TOOLS/... paths follow the in-OS one-command-per-line convention.
    #
    # OUTDIR is overridden on the command line so serial (-j1) and parallel
    # (-j8) runs write to distinct output dirs for byte comparison.
    # -B (--always-make) is passed by the driver so repeated runs rebuild
    # regardless of FAT timestamps (whole-second RTC granularity).
    SHELL := /TOOLS/SH.BIN
    MAKEFLAGS += -r -R

    OUTDIR := /SRC/BM/O1

    .PHONY: all
    all: $(OUTDIR)/TU01.O \
	$(OUTDIR)/TU02.O \
	$(OUTDIR)/TU03.O \
	$(OUTDIR)/TU04.O \
	$(OUTDIR)/TU05.O \
	$(OUTDIR)/TU06.O \
	$(OUTDIR)/TU07.O \
	$(OUTDIR)/TU08.O \
	$(OUTDIR)/TU09.O \
	$(OUTDIR)/TU10.O \
	$(OUTDIR)/TU11.O \
	$(OUTDIR)/TU12.O \
	$(OUTDIR)/TU13.O \
	$(OUTDIR)/TU14.O \
	$(OUTDIR)/TU15.O \
	$(OUTDIR)/TU16.O

    $(OUTDIR)/TU01.O: /SRC/BM/SRC/TU01.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C $(OUTDIR)/TU01.O

$(OUTDIR)/TU02.O: /SRC/BM/SRC/TU02.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C $(OUTDIR)/TU02.O

$(OUTDIR)/TU03.O: /SRC/BM/SRC/TU03.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C $(OUTDIR)/TU03.O

$(OUTDIR)/TU04.O: /SRC/BM/SRC/TU04.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C $(OUTDIR)/TU04.O

$(OUTDIR)/TU05.O: /SRC/BM/SRC/TU05.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C $(OUTDIR)/TU05.O

$(OUTDIR)/TU06.O: /SRC/BM/SRC/TU06.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C $(OUTDIR)/TU06.O

$(OUTDIR)/TU07.O: /SRC/BM/SRC/TU07.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C $(OUTDIR)/TU07.O

$(OUTDIR)/TU08.O: /SRC/BM/SRC/TU08.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C $(OUTDIR)/TU08.O

$(OUTDIR)/TU09.O: /SRC/BM/SRC/TU09.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C $(OUTDIR)/TU09.O

$(OUTDIR)/TU10.O: /SRC/BM/SRC/TU10.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C $(OUTDIR)/TU10.O

$(OUTDIR)/TU11.O: /SRC/BM/SRC/TU11.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C $(OUTDIR)/TU11.O

$(OUTDIR)/TU12.O: /SRC/BM/SRC/TU12.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C $(OUTDIR)/TU12.O

$(OUTDIR)/TU13.O: /SRC/BM/SRC/TU13.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C $(OUTDIR)/TU13.O

$(OUTDIR)/TU14.O: /SRC/BM/SRC/TU14.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C $(OUTDIR)/TU14.O

$(OUTDIR)/TU15.O: /SRC/BM/SRC/TU15.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C $(OUTDIR)/TU15.O

$(OUTDIR)/TU16.O: /SRC/BM/SRC/TU16.C
	/SRC/BM/BURN.BIN 700
	/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C $(OUTDIR)/TU16.O
