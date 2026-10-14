OBJDIR := /SRC/obj
.SUFFIXES:
$(OBJDIR)/arm/%.o: src/kernel/%.c
	@echo RULED $< $@
all: $(OBJDIR)/arm/dhcp.o
	@echo ALLDONE
