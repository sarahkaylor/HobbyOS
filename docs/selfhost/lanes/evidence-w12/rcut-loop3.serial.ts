6.505 UEFI firmware (version 2025.11-3ubuntu7 built at 11:11:58 on Mar 31 2026)
[2J[001;001H[=3h[2J[001;001H[2J[001;001H[8;056;240t[2J[001;001HBdsDxe: loading Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
BdsDxe: starting Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
[001;001H[001;001H[001;001Hl[001;002Hi[001;003Hn[001;004Hu[001;005Hx[001;006H:[001;007H [001;008HL[001;009Ho[001;010Ha[001;011Hd[001;012Hi[001;013Hn[001;014Hg[001;015H [001;016Hk[001;017He[001;018Hr[001;019Hn[001;020He[001;021Hl[001;022H [001;023H`[001;024Hb[001;025Ho[001;026Ho[001;027Ht[001;028H([001;029H)[001;030H:[001;031H/[001;032Hb[001;033Ho[001;034Ho[001;035Ht[001;036H/[001;037Hh[001;038Ho[001;039Hb[001;040Hb[001;041Hy[001;042Ho[001;043Hs[001;044H.[001;045Hb[001;046Hi[001;047Hn[001;048H`[001;049H.[001;050H.[001;051H.[001;052H[001;001H[002;001HBooting AArch64 OS...
MMU Initialized: Page Tables setup securely.
[GPUDIAG] gpu wait > 7 ms: ack=3 used=4 (park off)
[GPU] virtio-gpu mmio irq=78
VirtIO GPU successfully initialized.
VirtIO Input devices successfully initialized.
[FRAME] frames=1900544 total_mib=7424 reserved=0
[KERNEL] Activating secondary cores via PSCI...
[KERNEL] Core 1 power-on requested.
[KERNEL] Core 2 power-on requested.
[KERNEL] Core 3 power-on requested.
[KERNEL] Core 4 power-on requested.
[KERNEL] Core 5 power-on requested.
start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU 25[KERNEL] Core 63

 power-on requested.
1start_scheduler called on CPU start_scheduler called on CPU 6start_scheduler called on CPU 
7[KERNEL] Core 7 power-on requested.


4

VirtIO Block successfully initialized.
FAT-16 filesystem successfully initialized.
virtio-net MAC: 0x0000000000000052:0x0000000000000054:0x0000000000000000:0x0000000000000012:0x0000000000000034:0x0000000000000056
VirtIO Network successfully initialized.
Initializing DHCP...
DHCP Offer received
DHCP Request sent
DHCP Ack received
Network configured: IP 10.0.2.15 mask=0x0000000000FFFFFF gw=0x000000000202000A
DHCP DNS server: 10.0.2.3
DHCP Discover sent
DHCP initialization complete.

--- Parallel Program Loading ---
Mode: SELFHOST - Running in-OS acceptance build driver...
Loading program for scheduler (v2 AS): SELFHOST.BIN
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=1 block_idx=-1
/SELFHOST.BIN
v2 loader: image mapped (lazy), bytes=52656 pages=13 first=in elap=1ms
v2 loader: PID=1 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
start_scheduler called on CPU 0
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
/SRC/OUT.LOG
/SRC/SELFHOST.RUN
[SELFHOST] phase=s0-check-start
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=1
Inside process_create: clearing memory at 0x0000000072000000
Inside process_create: kmemset done.
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=3 block_idx=-1
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFHOST.SH
/TOOLS/MAKE.BIN
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=-1
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

7.505 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=s0-check-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/obj/arm/check.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

9.506 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

11.506 [IMGPRF] pid=5 done: pages=22997 mapped=22485 skipped=512 batches=180 stop=0 ms=4404
/SRC/src/kernel/main.c
/SRC/obj/arm/check-81674d23.o.tmp

12.507 /SRC/src/include/fat16.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fs.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/gic.h
/SRC/src/include/mmu.h
/SRC/src/include/arch/mmu.h
/SRC/src/include/program_loader.h
/SRC/src/include/timer.h
/SRC/src/include/virtio_blk.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/virtio_gpu.h

13.507 /SRC/src/include/virtio_input.h
/SRC/src/include/net.h
/SRC/src/include/virtio_net.h
/SRC/src/include/dhcp.h
src/kernel/main.c:50:39: warning: unused parameter 'tf' [-Wunused-parameter]
   50 | void irq_handler_c(struct trap_frame *tf) {
      |                                       ^

18.508 /SRC/obj/arm/check-81674d23.o.tmp new=/SRC/obj/arm/check.o
1 warning generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s0-check-done rc=0
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s0-check-done rc=0
[SELFHOST] phase=s1-user-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFHOST.SH
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

19.508 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
phase=s1-user-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/sh.elf /SRC/obj/arm/sh.o /SRC/obj/arm/sh_aux.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

20.509 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

21.509 [IMGPRF] pid=5 done: pages=13001 mapped=12626 skipped=375 batches=102 stop=0 ms=2285
/SRC/src/user/linker.ld
/SRC/obj/arm/sh.o
/SRC/obj/arm/sh_aux.o
/SRC/obj/arm/libc.a

22.509 /SRC/obj/arm/sh.elf.tmpc5ab816
/SRC/obj/arm/sh.elf
/SRC/obj/arm/sh.elf.tmp74d2309

23.509 /SRC/obj/arm/sh.elf

26.510 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/sh.elf /SRC/obj/arm/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/OBJCOPY.BIN
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1144 skipped=73 batches=10 stop=0 ms=148
/SRC/obj/arm/sh.elf
/SRC/obj/arm/SH.BIN.temp-stream-d2309e

27.510 /SRC/obj/arm/SH.BIN.temp-stream-d2309e new=/SRC/obj/arm/SH.BIN
/SRC/obj/arm/SH.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/ls.elf /SRC/obj/arm/ls.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/LLD.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

28.510 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

29.511 [IMGPRF] pid=5 done: pages=13001 mapped=12621 skipped=380 batches=102 stop=0 ms=2137
/SRC/src/user/linker.ld
/SRC/obj/arm/ls.o
/SRC/obj/arm/libc.a

30.511 /SRC/obj/arm/ls.elf.tmp1674d23
/SRC/obj/arm/ls.elf
/SRC/obj/arm/ls.elf.tmp09efc5a

31.511 /SRC/obj/arm/ls.elf

32.511 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now /4TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/ls.elf /SRC/obj/arm/LS.BIN

[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1138 skipped=79 batches=10 stop=0 ms=175
/SRC/obj/arm/ls.elf
/SRC/obj/arm/LS.BIN.temp-stream-674d23
/SRC/obj/arm/LS.BIN.temp-stream-674d23 new=/SRC/obj/arm/LS.BIN
/SRC/obj/arm/LS.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/cat.elf /SRC/obj/arm/cat.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

34.512 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

35.512 [IMGPRF] pid=5 done: pages=13001 mapped=12606 skipped=395 batches=102 stop=0 ms=2316
/SRC/src/user/linker.ld
/SRC/obj/arm/cat.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/cat.elf.tmp09efc5a
/SRC/obj/arm/cat.elf
/SRC/obj/arm/cat.elf.tmpb81674d

36.512 /SRC/obj/arm/cat.elf

37.512 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/cat.elf /SRC/obj/arm/CAT.BIN
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/OBJCOPY.BIN0x0000001000012720

v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1134 skipped=83 batches=10 stop=0 ms=163
/SRC/obj/arm/cat.elf
/SRC/obj/arm/CAT.BIN.temp-stream-5ab816

38.513 /SRC/obj/arm/CAT.BIN.temp-stream-5ab816 new=/SRC/obj/arm/CAT.BIN
/SRC/obj/arm/CAT.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/hello.elf /SRC/obj/arm/hello.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

39.513 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

40.513 [IMGPRF] pid=5 done: pages=13001 mapped=12618 skipped=383 batches=102 stop=0 ms=2130
/SRC/src/user/linker.ld
/SRC/obj/arm/hello.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/hello.elf.tmp9efc5ab

41.514 /SRC/obj/arm/hello.elf
/SRC/obj/arm/hello.elf.tmp81674d2
/SRC/obj/arm/hello.elf

42.514 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/OBJCOPY.BIN -O binary /SRC/obj/arm/hello.elf /SRC/obj/arm/HELLO.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0

43.514 [DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1143 skipped=74 batches=10 stop=0 ms=183
/SRC/obj/arm/hello.elf
/SRC/obj/arm/HELLO.BIN.temp-stream-2309ef
/SRC/obj/arm/HELLO.BIN.temp-stream-2309ef new=/SRC/obj/arm/HELLO.BIN
/SRC/obj/arm/HELLO.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s1-user-done rc=0
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s1-user-done rc=0
[SELFHOST] phase=t25-run-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/T25.RUN
[DEBUG] Before eret, tf->regs[0] = 0 ELR = 0x0000001000002E40
/SRC/obj/arm/HELLO.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/SRC/obj/arm/HELLO.BIN
v2 loader: image mapped (lazy), bytes=37360 pages=10 first=in elap=0ms
[KERNEL] Process 2: ./HELLO.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[HELLO] entering main
[HELLO] argc=1
[HELLO] argv[0]="./HELLO.BIN"
[HELLO] main returned 0
[SELFHOST] phase=t25-run-done rc=0
[SELFHOST] phase=s2-kernel-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFHOST.SH
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

44.515 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-link-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# w6-run: lld's link argv (66 words incl. 59 objects) exceeds the
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# HobbyOS crt0 posi ELR = tional-arg0x00000010000260F8 cap (CRT0_MAX_ARGS 32) on the flat-arg
s
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# fallback, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# ELR =  0x00000010000260F8t
he response file is written at parse time by the make file-write
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

45.515 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# ELR =  0x00000010000260F8f
unction attached to the KERNEL_OBJS definition above (no shell, no
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# ELR =  0x00000010000260F8a
rgv cap) and the link line itself stays short.
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/LLD.BIN -flavor gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/LLD.BIN

v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

46.515 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

47.515 /SRC/OBJS.RSP
[IMGPRF] pid=5 done: pages=13001 mapped=12586 skipped=415 batches=102 stop=0 ms=2229
/SRC/OBJS.RSP
/SRC/linker.ld
/SRC/obj/arm/dhcp.o
/SRC/obj/arm/fat16.o
/SRC/obj/arm/fat16_test.o
/SRC/obj/arm/frame.o
/SRC/obj/arm/frame_test.o
/SRC/obj/arm/fs.o
/SRC/obj/arm/fs_test.o
/SRC/obj/arm/iommu_vtd.o
/SRC/obj/arm/ipc_proto.o
/SRC/obj/arm/locks_test.o
/SRC/obj/arm/main.o
/SRC/obj/arm/math_test.o
/SRC/obj/arm/mmu_test.o
/SRC/obj/arm/net.o
/SRC/obj/arm/net_rdma.o
/SRC/obj/arm/net_rdma_test.o
/SRC/obj/arm/net_test.o
/SRC/obj/arm/nfs.o
/SRC/obj/arm/nfs_proto.o
/SRC/obj/arm/nfs_test.o
/SRC/obj/arm/pipe.o
/SRC/obj/arm/pipe_test.o
/SRC/obj/arm/process.o
/SRC/obj/arm/process_test.o

48.516 /SRC/obj/arm/program_loader.o
/SRC/obj/arm/program_loader_test.o
/SRC/obj/arm/test_fw_cfg.o
/SRC/obj/arm/test_smp.o
/SRC/obj/arm/time.o
/SRC/obj/arm/time_test.o
/SRC/obj/arm/timer_test.o
/SRC/obj/arm/trap_test.o
/SRC/obj/arm/unit_test.o
/SRC/obj/arm/unix.o
/SRC/obj/arm/unix_test.o
/SRC/obj/arm/vfs.o
/SRC/obj/arm/virtio_blk.o
/SRC/obj/arm/virtio_blk_test.o
/SRC/obj/arm/virtio_gpu.o
/SRC/obj/arm/virtio_gpu_test.o
/SRC/obj/arm/virtio_gpu_x64.o
/SRC/obj/arm/virtio_input.o
/SRC/obj/arm/virtio_input_test.o
/SRC/obj/arm/virtio_net.o
/SRC/obj/arm/virtio_net_test.o
/SRC/obj/arm/vm.o
/SRC/obj/arm/vm_test.o
/SRC/obj/arm/cpu.o
/SRC/obj/arm/gic.o
/SRC/obj/arm/locks.o
/SRC/obj/arm/mmu.o
/SRC/obj/arm/rtc.o
/SRC/obj/arm/smp.o
/SRC/obj/arm/timer.o
/SRC/obj/arm/trap.o
/SRC/obj/arm/uart.o
/SRC/obj/arm/boot.o
/SRC/obj/arm/fpu.o
/SRC/obj/arm/setjmp.o

49.516 /SRC/hobbyos.elf.tmp4d2309e
/SRC/hobbyos.elf
/SRC/hobbyos.elf.tmpfc5ab81

53.517 /SRC/hobbyos.elf

59.518 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-link-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN -O binary /SRC/hobbyos.elf /SRC/hobbyos.bin
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/OBJCOPY.BIN

v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1137 skipped=80 batches=10 stop=0 ms=163
/SRC/hobbyos.elf

60.519 /SRC/hobbyos.bin.temp-stream-c5ab81

61.519 /SRC/hobbyos.bin.temp-stream-c5ab81 new=/SRC/hobbyos.bin
/SRC/hobbyos.bin
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-done rc=0
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-done rc=0
[SELFHOST] phase=s2-inc-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFINC.SH
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP
MAKE.BIN: '/SRC/obj/arm/dhcp.o' is up to date.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

62.519 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=s2-kernel-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-link-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# w6-run: lld's link argv (66 words incl. 59 objects) exceeds the
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = #4  ELR = H0x00000010000260F8o
bbyOS crt0 positional-arg cap (CRT0_MAX_ARGS 32) on the flat-args
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# fallback, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# the response file is written at parse time by the make file-write
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# ELR =  0x00000010000260F8f
unction attached to the KERNEL_OBJS definition above (no shell, no
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# argv cap) and the link line itself stays short.
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/LLD.BIN -flavor gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128

63.519 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

64.520 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

65.520 /SRC/OBJS.RSP
[IMGPRF] pid=5 done: pages=13001 mapped=12597 skipped=404 batches=102 stop=0 ms=2139
/SRC/OBJS.RSP
/SRC/linker.ld
/SRC/obj/arm/dhcp.o
/SRC/obj/arm/fat16.o
/SRC/obj/arm/fat16_test.o
/SRC/obj/arm/frame.o
/SRC/obj/arm/frame_test.o
/SRC/obj/arm/fs.o
/SRC/obj/arm/fs_test.o
/SRC/obj/arm/iommu_vtd.o
/SRC/obj/arm/ipc_proto.o
/SRC/obj/arm/locks_test.o
/SRC/obj/arm/main.o
/SRC/obj/arm/math_test.o
/SRC/obj/arm/mmu_test.o
/SRC/obj/arm/net.o
/SRC/obj/arm/net_rdma.o
/SRC/obj/arm/net_rdma_test.o
/SRC/obj/arm/net_test.o
/SRC/obj/arm/nfs.o
/SRC/obj/arm/nfs_proto.o
/SRC/obj/arm/nfs_test.o
/SRC/obj/arm/pipe.o
/SRC/obj/arm/pipe_test.o
/SRC/obj/arm/process.o
/SRC/obj/arm/process_test.o
/SRC/obj/arm/program_loader.o
/SRC/obj/arm/program_loader_test.o
/SRC/obj/arm/test_fw_cfg.o
/SRC/obj/arm/test_smp.o
/SRC/obj/arm/time.o
/SRC/obj/arm/time_test.o
/SRC/obj/arm/timer_test.o
/SRC/obj/arm/trap_test.o
/SRC/obj/arm/unit_test.o
/SRC/obj/arm/unix.o
/SRC/obj/arm/unix_test.o
/SRC/obj/arm/vfs.o
/SRC/obj/arm/virtio_blk.o
/SRC/obj/arm/virtio_blk_test.o
/SRC/obj/arm/virtio_gpu.o

66.520 /SRC/obj/arm/virtio_gpu_test.o
/SRC/obj/arm/virtio_gpu_x64.o
/SRC/obj/arm/virtio_input.o
/SRC/obj/arm/virtio_input_test.o
/SRC/obj/arm/virtio_net.o
/SRC/obj/arm/virtio_net_test.o
/SRC/obj/arm/vm.o
/SRC/obj/arm/vm_test.o
/SRC/obj/arm/cpu.o
/SRC/obj/arm/gic.o
/SRC/obj/arm/locks.o
/SRC/obj/arm/mmu.o
/SRC/obj/arm/rtc.o
/SRC/obj/arm/smp.o
/SRC/obj/arm/timer.o
/SRC/obj/arm/trap.o
/SRC/obj/arm/uart.o
/SRC/obj/arm/boot.o
/SRC/obj/arm/fpu.o
/SRC/obj/arm/setjmp.o
/SRC/hobbyos.elf.tmp2309efc

67.520 /SRC/hobbyos.elf
/SRC/hobbyos.elf.tmp5ab8167

70.521 /SRC/hobbyos.elf

77.522 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-link-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/hobbyos.elf /SRC/hobbyos.bin
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/OBJCOPY.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1141 skipped=76 batches=10 stop=0 ms=174
/SRC/hobbyos.elf
/SRC/hobbyos.bin.temp-stream-74d230

79.523 /SRC/hobbyos.bin.temp-stream-74d230 new=/SRC/hobbyos.bin
/SRC/hobbyos.bin
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-done rc=0
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-inc-done rc=0
[SELFHOST] phase=s3-disk-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFHOST.SH
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
SELFHOST] phase=s2-kernel-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-link-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

80.523 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# w6-run: lld's link argv (66 words incl. 59 objects) exceeds the
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# HobbyOS crt0 positional-arg cap (CRT0_MAX_ARGS 32) on the flat-args
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4# fa ELR = l0x00000010000260F8l
back, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = #0x00000010000260F8 
the response file is written at parse time by the make file-write
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8#
 function attached to the KERNEL_OBJS definition above (no shell, no
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# argv cap) and the link line itself stays short.
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOLS/LLD.B ELR = IN -fla0x00000010000260F8v
or gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

82.524 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12632 skipped=369 batches=102 stop=0 ms=2221
/SRC/OBJS.RSP
/SRC/OBJS.RSP

83.524 /SRC/linker.ld
/SRC/obj/arm/dhcp.o
/SRC/obj/arm/fat16.o
/SRC/obj/arm/fat16_test.o
/SRC/obj/arm/frame.o
/SRC/obj/arm/frame_test.o
/SRC/obj/arm/fs.o
/SRC/obj/arm/fs_test.o
/SRC/obj/arm/iommu_vtd.o
/SRC/obj/arm/ipc_proto.o
/SRC/obj/arm/locks_test.o
/SRC/obj/arm/main.o
/SRC/obj/arm/math_test.o
/SRC/obj/arm/mmu_test.o
/SRC/obj/arm/net.o
/SRC/obj/arm/net_rdma.o
/SRC/obj/arm/net_rdma_test.o
/SRC/obj/arm/net_test.o
/SRC/obj/arm/nfs.o
/SRC/obj/arm/nfs_proto.o
/SRC/obj/arm/nfs_test.o
/SRC/obj/arm/pipe.o
/SRC/obj/arm/pipe_test.o
/SRC/obj/arm/process.o
/SRC/obj/arm/process_test.o
/SRC/obj/arm/program_loader.o
/SRC/obj/arm/program_loader_test.o
/SRC/obj/arm/test_fw_cfg.o
/SRC/obj/arm/test_smp.o
/SRC/obj/arm/time.o
/SRC/obj/arm/time_test.o
/SRC/obj/arm/timer_test.o
/SRC/obj/arm/trap_test.o
/SRC/obj/arm/unit_test.o
/SRC/obj/arm/unix.o
/SRC/obj/arm/unix_test.o
/SRC/obj/arm/vfs.o
/SRC/obj/arm/virtio_blk.o
/SRC/obj/arm/virtio_blk_test.o
/SRC/obj/arm/virtio_gpu.o

84.524 /SRC/obj/arm/virtio_gpu_test.o
/SRC/obj/arm/virtio_gpu_x64.o
/SRC/obj/arm/virtio_input.o
/SRC/obj/arm/virtio_input_test.o
/SRC/obj/arm/virtio_net.o
/SRC/obj/arm/virtio_net_test.o
/SRC/obj/arm/vm.o
/SRC/obj/arm/vm_test.o
/SRC/obj/arm/cpu.o
/SRC/obj/arm/gic.o
/SRC/obj/arm/locks.o
/SRC/obj/arm/mmu.o
/SRC/obj/arm/rtc.o
/SRC/obj/arm/smp.o
/SRC/obj/arm/timer.o
/SRC/obj/arm/trap.o
/SRC/obj/arm/uart.o
/SRC/obj/arm/boot.o
/SRC/obj/arm/fpu.o
/SRC/obj/arm/setjmp.o
/SRC/hobbyos.elf.tmpefc5ab8
/SRC/hobbyos.elf

85.524 /SRC/hobbyos.elf.tmp1674d23

88.525 /SRC/hobbyos.elf

94.527 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-link-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/hobbyos.elf /SRC/hobbyos.bin
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/OBJCOPY.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0

95.527 [DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1137 skipped=80 batches=10 stop=0 ms=156
/SRC/hobbyos.elf
/SRC/hobbyos.bin.temp-stream-5ab816

96.527 /SRC/hobbyos.bin.temp-stream-5ab816 new=/SRC/hobbyos.bin
/SRC/hobbyos.bin
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=s[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
1-user-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/sh.elf /SRC/obj/arm/sh.o /SRC/obj/arm/sh_aux.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN

97.528 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

98.528 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12636 skipped=365 batches=102 stop=0 ms=1938

99.528 /SRC/src/user/linker.ld
/SRC/obj/arm/sh.o
/SRC/obj/arm/sh_aux.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/sh.elf.tmp74d2309
/SRC/obj/arm/sh.elf
/SRC/obj/arm/sh.elf.tmpefc5ab8

101.529 /SRC/obj/arm/sh.elf

103.529 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/TOOLS/O
BJCOPY.BIN -O binary /SRC/obj/arm/sh.elf /SRC/obj/arm/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/OBJCOPY.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1137 skipped=80 batches=10 stop=0 ms=135
/SRC/obj/arm/sh.elf
/SRC/obj/arm/SH.BIN.temp-stream-9efc5a

104.529 /SRC/obj/arm/SH.BIN.temp-stream-9efc5a new=/SRC/obj/arm/SH.BIN
/SRC/obj/arm/SH.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = TOO4L ELR = S/LLD.0x00000010000260F8BIN -fla
vor gnu -T src/user/linker.ld -o /SRC/obj/arm/ls.elf /SRC/obj/arm/ls.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

106.530 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12610 skipped=391 batches=102 stop=0 ms=1965
/SRC/src/user/linker.ld
/SRC/obj/arm/ls.o
/SRC/obj/arm/libc.a

107.530 /SRC/obj/arm/ls.elf.tmpb81674d
/SRC/obj/arm/ls.elf
/SRC/obj/arm/ls.elf.tmp2309efc

108.530 /SRC/obj/arm/ls.elf
[KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

109.531 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOO4L ELR = S0x00000010000260F8/
OBJCOPY.BIN -O binary /SRC/obj/arm/ls.elf /SRC/obj/arm/LS.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1130 skipped=87 batches=10 stop=0 ms=143
/SRC/obj/arm/ls.elf
/SRC/obj/arm/LS.BIN.temp-stream-b81674
/SRC/obj/arm/LS.BIN.temp-stream-b81674 new=/SRC/obj/arm/LS.BIN
/SRC/obj/arm/LS.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOO ELR = LS/LLD.BIN -flav0x00000010000260F8o
r gnu -T src/user/linker.ld -o /SRC/obj/arm/cat.elf /SRC/obj/arm/cat.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

110.531 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

111.531 [IMGPRF] pid=5 done: pages=13001 mapped=12624 skipped=377 batches=102 stop=0 ms=1989
/SRC/src/user/linker.ld
/SRC/obj/arm/cat.o
/SRC/obj/arm/libc.a

112.531 /SRC/obj/arm/cat.elf.tmp81674d2
/SRC/obj/arm/cat.elf
/SRC/obj/arm/cat.elf.tmp309efc5
/SRC/obj/arm/cat.elf

113.532 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/cat.elf /SRC/obj/arm/CAT.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/OBJCOPY.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90

114.532 [IMGPRF] pid=5 done: pages=1217 mapped=1124 skipped=93 batches=10 stop=0 ms=144
/SRC/obj/arm/cat.elf
/SRC/obj/arm/CAT.BIN.temp-stream-ab8167
/SRC/obj/arm/CAT.BIN.temp-stream-ab8167 new=/SRC/obj/arm/CAT.BIN
/SRC/obj/arm/CAT.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/hello.elf /SRC/obj/arm/hello.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

115.532 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

116.532 [IMGPRF] pid=5 done: pages=13001 mapped=12605 skipped=396 batches=102 stop=0 ms=2162
/SRC/src/user/linker.ld
/SRC/obj/arm/hello.o
/SRC/obj/arm/libc.a

117.533 /SRC/obj/arm/hello.elf.tmp1674d23
/SRC/obj/arm/hello.elf
/SRC/obj/arm/hello.elf.tmp09efc5a
/SRC/obj/arm/hello.elf

118.533 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/hello.elf /SRC/obj/arm/HELLO.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN

119.533 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1131 skipped=86 batches=10 stop=0 ms=188
/SRC/obj/arm/hello.elf
/SRC/obj/arm/HELLO.BIN.temp-stream-74d230
/SRC/obj/arm/HELLO.BIN.temp-stream-74d230 new=/SRC/obj/arm/HELLO.BIN
/SRC/obj/arm/HELLO.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s1-user-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[S[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
ELFHOST] phase=s3-disk-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/RM.BIN -f /SRC/OUT.IMG
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742448 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742448 ELR = 0x0000001000005BC0
/TOOLS/RM.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/RM.BIN
v2 loader: image mapped (lazy), bytes=44704 pages=11 first=in elap=0ms
rm: cannot remove -f
rm: cannot remove /SRC/OUT.IMG
[KERNEL] Process 5: /TOOLS/RM.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

120.533 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/MKFSFAT.BIN -F 16 -C /SRC/OUT.IMG 131072
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MKFSFAT.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/MKFSFAT.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=152120 pages=38 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000009680
mkfs.fat 4.2 (2021-01-31)
/SRC/OUT.IMG

421.584 [KERNEL] Process 5: /TOOLS/MKFSFAT.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

422.584 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/MMD.BIN -i /SRC/OUT.IMG ::/boot
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MMD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5/TOOLS/MMD.BIN ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1600 ELR = 0x000000100000AE40
/SRC/OUT.IMG
[KERNEL] Process 5: /TOOLS/MMD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/MMD.BIN -i /SRC/OUT.IMG ::/EFI
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MMD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MMD.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1600 ELR = 0x000000100000AE40
/SRC/OUT.IMG
[KERNEL] Process 5: /TOOLS/MMD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/MMD.BIN -i /SRC/OUT.IMG ::/EFI/BOOT
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MMD.BIN

423.584 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MMD.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1600 ELR = 0x000000100000AE40
/SRC/OUT.IMG
[KERNEL] Process 5: /TOOLS/MMD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOO ELR = L0x00000010000260F8S
/MMD.BIN -i /SRC/OUT.IMG ::/bin
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MMD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MMD.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1600 ELR = 0x000000100000AE40
/SRC/OUT.IMG
[KERNEL] Process 5: /TOOLS/MMD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOLS/M ELR = CO0x00000010000260F8P
Y.BIN -i /SRC/OUT.IMG /SRC/hobbyos.bin ::/boot/hobbyos.bin
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/MCOPY.BIN0x0000001000012720

v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG

424.584 /SRC/hobbyos.bin
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/MCOPY.BIN -i /SRC/OUT.IMG limine.conf ::/boot/limine.conf
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG

425.585 /SRC/limine.conf
[DEBUG] Before eret, tf->regs[0] = -4808 ELR = 0x0000001000011A80
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/MCOPY.BIN -i /SRC/OUT.IMG bootloader/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/bootloader/BOOTAA64.EFI

426.585 [KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/MCOPY.BIN -i /SRC/OUT.IMG /SRC/obj/arm/HELLO.BIN ::/HELLO.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/MCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/obj/arm/HELLO.BIN
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/MCOPY.BIN -i /SRC/OUT.IMG /SRC/obj/arm/SH.BIN ::/SH.BIN

427.585 /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/MCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/obj/arm/SH.BIN
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/MCOPY.BIN -i /SRC/OUT.IMG /SRC/obj/arm/LS.BIN ::/LS.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN

428.585 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/obj/arm/LS.BIN
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/MCOPY.BIN -i /SRC/OUT.IMG /SRC/obj/arm/CAT.BIN ::/CAT.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/obj/arm/CAT.BIN

429.585 [KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/MCOPY.BIN -i /SRC/OUT.IMG bin/DESKTOP.BIN ::/DESKTOP.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/bin/DESKTOP.BIN

430.586 [KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/MCOPY.BIN -i /SRC/OUT.IMG bin/CONSOLE.BIN ::/CONSOLE.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN

431.586 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/MCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/bin/CONSOLE.BIN
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /TOOLS/MCOPY0x00000010000260F8.
BIN -i /SRC/OUT.IMG bin/SH.BIN ::/bin/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/MCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/bin/SH.BIN

432.586 [KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/MCOPY.BIN -i /SRC/OUT.IMG /SRC/obj/arm/HELLO.BIN ::/OUTPROG.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/MCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/obj/arm/HELLO.BIN
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/MCOPY.BIN -i /SRC/OUT.IMG bin/OUTMARK.TXT ::/OUTMARK.TXT
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

433.586 /TOOLS/MCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/MCOPY.BIN
v2 loader: image mapped (lazy), bytes=186200 pages=46 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000016F50
[DEBUG] Before eret, tf->regs[0] = -1728 ELR = 0x000000100000AE40
/SRC/OUT.IMG
/SRC/bin/OUTMARK.TXT
[KERNEL] Process 5: /TOOLS/MCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s3-disk-done rc=0
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s3-disk-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=6
[KERNEL] Process 1: SELFHOST.BIN exited unexpectedly or gracefully.
System halt from CPU 5.

