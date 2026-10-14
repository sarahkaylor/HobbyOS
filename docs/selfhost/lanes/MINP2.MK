.SUFFIXES:
obj/arm/%.o: src/kernel/%.c
	@echo RULED $< $@
all: obj/arm/dhcp.o
	@echo ALLDONE
