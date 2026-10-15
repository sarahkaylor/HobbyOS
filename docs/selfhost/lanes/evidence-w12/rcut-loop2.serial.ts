6.504 UEFI firmware (version 2025.11-3ubuntu7 built at 11:11:58 on Mar 31 2026)
[2J[001;001H[=3h[2J[001;001H[2J[001;001H[8;056;240t[2J[001;001HBdsDxe: loading Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
BdsDxe: starting Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
[001;001H[001;001H[001;001Hl[001;002Hi[001;003Hn[001;004Hu[001;005Hx[001;006H:[001;007H [001;008HL[001;009Ho[001;010Ha[001;011Hd[001;012Hi[001;013Hn[001;014Hg[001;015H [001;016Hk[001;017He[001;018Hr[001;019Hn[001;020He[001;021Hl[001;022H [001;023H`[001;024Hb[001;025Ho[001;026Ho[001;027Ht[001;028H([001;029H)[001;030H:[001;031H/[001;032Hb[001;033Ho[001;034Ho[001;035Ht[001;036H/[001;037Hh[001;038Ho[001;039Hb[001;040Hb[001;041Hy[001;042Ho[001;043Hs[001;044H.[001;045Hb[001;046Hi[001;047Hn[001;048H`[001;049H.[001;050H.[001;051H.[001;052H[001;001H[002;001HBooting AArch64 OS...
MMU Initialized: Page Tables setup securely.
[GPUDIAG] gpu wait > 8 ms: ack=3 used=4 (park off)
[GPU] virtio-gpu mmio irq=78
VirtIO GPU successfully initialized.
VirtIO Input devices successfully initialized.
[FRAME] frames=1900544 total_mib=7424 reserved=0
[KERNEL] Activating secondary cores via PSCI...
[KERNEL] Core 1 power-on requested.
[KERNEL] Core 2 power-on requested.
[KERNEL] Core 3 power-on requested.
[KERNEL] Core 4 power-on requested.
[KERNEL] Core start_scheduler called on CPU start_scheduler called on CPU 12start_scheduler called on CPU 5start_scheduler called on CPU start_scheduler called on CPU  power-on requested.
34[KERNEL] Core 
6
 power-on requested.

5
[KERNEL] Core 
start_scheduler called on CPU 76start_scheduler called on CPU 
7 power-on requested.

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

7.504 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=s0-check-start
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/obj/arm/check.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

9.504 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

11.505 [IMGPRF] pid=5 done: pages=22997 mapped=22513 skipped=484 batches=180 stop=0 ms=3759
/SRC/src/kernel/main.c
/SRC/obj/arm/check-09efc5ab.o.tmp
/SRC/src/include/fat16.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fs.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h

12.505 /SRC/src/include/ipc_proto.h
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
/SRC/src/include/virtio_input.h
/SRC/src/include/net.h
/SRC/src/include/virtio_net.h
/SRC/src/include/dhcp.h
src/kernel/main.c:50:39: warning: unused parameter 'tf' [-Wunused-parameter]
   50 | void irq_handler_c(struct trap_frame *tf) {
      |                                       ^

24.508 /SRC/obj/arm/check-09efc5ab.o.tmp new=/SRC/obj/arm/check.o
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFJ8.SH

25.508 /TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
RESDIR/include -c src/user/hello.c -o /SRC/obj/arm/hello.o
/TOOLS/SH.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8/T
OOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include -c src/user/sh.c -o /SRC/obj/arm/sh.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=6 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = /TO8O ELR = L0x00000010000260F8S
/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS//TOOLS/SH.BIN
RESDIR/include -c src/user/sh_aux.c -o /SRC/obj/arm/sh_aux.o

26.508 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=25ms
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=9 start: pages=22997 batch=128
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS//TOOLS/SH.BIN
/TOOLS/CLANG.BIN
RESDIR/include -c src/user/ls.c -o /SRC/obj/arm/ls.o

27.509 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=118ms
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=128ms
/TOOLS/SH.BIN
[IMGPRF] pid=12 start: pages=22997 batch=128
RESDIR/include -c src/user/cat.c -o /SRC/obj/arm/cat.o

28.509 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=24ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

29.509 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

30.510 /TOOLS/CLANG.BIN

31.510 /TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=80ms
[IMGPRF] pid=15 start: pages=22997 batch=128

32.510 v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=100ms
[IMGPRF] pid=16 start: pages=22997 batch=128

33.511 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

41.513 [IMGPRF] pid=6 done: pages=22997 mapped=22839 skipped=158 batches=180 stop=0 ms=15530

45.514 [IMGPRF] pid=12 done: pages=22997 mapped=22886 skipped=111 batches=180 stop=0 ms=17441

46.514 [IMGPRF] pid=9 done: pages=22997 mapped=22851 skipped=146 batches=180 stop=0 ms=20290

47.514 [IMGPRF] pid=16 done: pages=22997 mapped=22901 skipped=96 batches=180 stop=0 ms=15424

49.514 /SRC/src/user/cat.c
/SRC/src/user/hello.c
/SRC/obj/arm/cat-9efc5ab8.o.tmp

50.515 /SRC/obj/arm/hello-81674d23.o.tmp
[IMGPRF] pid=15 done: pages=22997 mapped=22809 skipped=188 batches=180 stop=0 ms=18270
/SRC/src/user/sh.c
/SRC/obj/arm/sh-309efc5a.o.tmp
/SRC/src/user/sh_aux.c
/SRC/obj/arm/sh_aux-09efc5ab.o.tmp
/SRC/src/user/ls.c
/SRC/obj/arm/ls-81674d23.o.tmp
/SRC/src/user_include/libc.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/SRC/src/libc/include/locale.h
/SRC/src/user_include/libc.h
/SRC/src/libc/include/string.h

51.515 /TOOLS/RESDIR/include/stddef.h
/SRC/src/user_include/libc.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/libc/include/ctype.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
[DEBUG] Before eret, tf->regs[0] = 1075503104 ELR = 0x00000010043627C0
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/SRC/src/user_include/libc.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/user_include/malloc.h
/SRC/src/libc/include/locale.h
/SRC/src/include/errno.h
/SRC/src/libc/include/locale.h
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/ctype.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/sys/time.h
/SRC/src/libc/include/fcntl.h
/SRC/src/libc/include/ctype.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h

52.515 /TOOLS/RESDIR/include/__stddef_size_t.h
/SRC/src/libc/include/sys/types.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/SRC/src/libc/include/stdlib.h
/SRC/src/libc/include/stdint.h
[DEBUG] Before eret, tf->regs[0] = 1074116872 ELR = 0x0000001002EEF9B0
/SRC/src/libc/include/stdio.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/user_include/libc.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/libc/include/stdarg.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/SRC/src/libc/include/locale.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/SRC/src/libc/include/ctype.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/SRC/src/libc/include/locale.h
/SRC/src/user_include/malloc.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/SRC/src/libc/include/ctype.h

53.515 /TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/include/errno.h
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/libc/include/unistd.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/stdarg.h
/SRC/src/user_include/malloc.h
/TOOLS/RESDIR/include/stdarg.h
/SRC/src/include/errno.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/time.h
/SRC/src/user_include/malloc.h
/SRC/src/libc/include/sys/time.h
/SRC/src/include/syscall.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/SRC/src/include/errno.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/SRC/src/libc/include/sys/time.h
/SRC/src/user_include/resolv.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/sys/time.h
/SRC/src/libc/include/unistd.h
/SRC/src/user_include/malloc.h
/SRC/src/include/errno.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/sys/time.h

56.516 /SRC/obj/arm/cat-9efc5ab8.o.tmp new=/SRC/obj/arm/cat.o
/SRC/obj/arm/hello-81674d23.o.tmp new=/SRC/obj/arm/hello.o
[KERNEL] Process 6: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 16: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=16ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

58.516 /SRC/obj/arm/ls-81674d23.o.tmp new=/SRC/obj/arm/ls.o
[KERNEL] Process 15: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

59.517 /SRC/obj/arm/sh_aux-09efc5ab.o.tmp new=/SRC/obj/arm/sh_aux.o
[KERNEL] Process 12: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

3873.348 /SRC/obj/arm/sh-309efc5a.o.tmp new=/SRC/obj/arm/sh.o

3876.348 [KERNEL] Process 9: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now [4S
E[DEBUG] Before eret, tf->regs[0] = L4F ELR = H0x00000010000260F8O
ST] phase=s1-user-start
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
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

3877.349 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

3878.349 [IMGPRF] pid=5 done: pages=13001 mapped=12618 skipped=383 batches=102 stop=0 ms=1963
/SRC/src/user/linker.ld
/SRC/obj/arm/sh.o
/SRC/obj/arm/sh_aux.o
/SRC/obj/arm/libc.a

3879.349 /SRC/obj/arm/sh.elf.tmp2309efc
/SRC/obj/arm/sh.elf.tmp5ab8167

3880.349 /SRC/obj/arm/sh.elf

3882.350 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/sh.elf /SRC/obj/arm/SH.BIN
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
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90

3883.350 [IMGPRF] pid=5 done: pages=1217 mapped=1148 skipped=69 batches=10 stop=0 ms=170
/SRC/obj/arm/sh.elf
/SRC/obj/arm/SH.BIN.temp-stream-9efc5a
/SRC/obj/arm/SH.BIN.temp-stream-9efc5a new=/SRC/obj/arm/SH.BIN

3884.350 /SRC/obj/arm/SH.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/ls.elf /SRC/obj/arm/ls.o /SRC/obj/arm/libc.a
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

3885.350 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

3886.351 [IMGPRF] pid=5 done: pages=13001 mapped=12665 skipped=336 batches=102 stop=0 ms=1895
/SRC/src/user/linker.ld
/SRC/obj/arm/ls.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/ls.elf.tmpc5ab816
/SRC/obj/arm/ls.elf.tmp74d2309

3887.351 /SRC/obj/arm/ls.elf

3888.351 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/ls.elf /SRC/obj/arm/LS.BIN
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
[IMGPRF] pid=5 done: pages=1217 mapped=1137 skipped=80 batches=10 stop=0 ms=170
/SRC/obj/arm/ls.elf
/SRC/obj/arm/LS.BIN.temp-stream-fc5ab8
/SRC/obj/arm/LS.BIN.temp-stream-fc5ab8 new=/SRC/obj/arm/LS.BIN
/SRC/obj/arm/LS.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

3889.351 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = TOOLS/L0x00000010000260F8L
D.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/cat.elf /SRC/obj/arm/cat.o /SRC/obj/arm/libc.a
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

3890.352 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

3891.352 [IMGPRF] pid=5 done: pages=13001 mapped=12654 skipped=347 batches=102 stop=0 ms=2028
/SRC/src/user/linker.ld
/SRC/obj/arm/cat.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/cat.elf.tmpfc5ab81
/SRC/obj/arm/cat.elf.tmp674d230

3892.352 /SRC/obj/arm/cat.elf

3893.352 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/cat.elf /SRC/obj/arm/CAT.BIN
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
[IMGPRF] pid=5 done: pages=1217 mapped=1136 skipped=81 batches=10 stop=0 ms=135
/SRC/obj/arm/cat.elf
/SRC/obj/arm/CAT.BIN.temp-stream-674d23
/SRC/obj/arm/CAT.BIN.temp-stream-674d23 new=/SRC/obj/arm/CAT.BIN
/SRC/obj/arm/CAT.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/hello.elf /SRC/obj/arm/hello.o /SRC/obj/arm/libc.a
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
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

3895.353 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12642 skipped=359 batches=102 stop=0 ms=2024

3896.353 /SRC/src/user/linker.ld
/SRC/obj/arm/hello.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/hello.elf.tmpefc5ab8
/SRC/obj/arm/hello.elf.tmp1674d23

3897.353 /SRC/obj/arm/hello.elf

3898.353 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/hello.elf /SRC/obj/arm/HELLO.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1140 skipped=77 batches=10 stop=0 ms=154
/SRC/obj/arm/hello.elf
/SRC/obj/arm/HELLO.BIN.temp-stream-d2309e
/SRC/obj/arm/HELLO.BIN.temp-stream-d2309e new=/SRC/obj/arm/HELLO.BIN
/SRC/obj/arm/HELLO.BIN
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/SELFHOST.SH
/TOOLS/MAKE.BIN

3899.354 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/dhcp.c -o /SRC/ob/TOOLS/SH.BIN
j/arm/dhcp.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

3901.354 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

3902.354 [IMGPRF] pid=5 done: pages=22997 mapped=22540 skipped=457 batches=180 stop=0 ms=3328
/SRC/src/kernel/dhcp.c
/SRC/obj/arm/dhcp-4d2309ef.o.tmp

3903.355 /SRC/src/include/dhcp.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/net.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/virtio_net.h
/SRC/src/include/timer.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
src/kernel/dhcp.c:42:17: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
   42 |   for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
      |                ~^~~~~~~~~~~~
src/kernel/dhcp.c:82:17: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
   82 |   for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
      |                ~^~~~~~~~~~~~
src/kernel/dhcp.c:135:19: warning: unused variable 'udp' [-Wunused-variable]
  135 |   struct udp_hdr* udp = (struct udp_hdr*)(packet + sizeof(struct ipv4_hdr));
      |                   ^~~

3918.358 /SRC/obj/arm/dhcp-4d2309ef.o.tmp new=/SRC/obj/arm/dhcp.o
3 warnings generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fat16.c -o /SRC/obj/arm/fat16.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

3920.358 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

3922.359 [IMGPRF] pid=5 done: pages=22997 mapped=22579 skipped=418 batches=180 stop=0 ms=4031
/SRC/src/kernel/fat16.c
/SRC/obj/arm/fat16-efc5ab81.o.tmp

3923.359 /SRC/src/include/fs.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fat16.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/virtio_blk.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/errno.h

3925.359 src/kernel/fat16.c:2000:25: warning: comparison of integers of different signs: 'int' and 'uint32_t' (aka 'unsigned int') [-Wsign-compare]
 2000 |       if ((int)old_size > f->fat16.cursor) f->fat16.cursor = (int)old_size;
      |           ~~~~~~~~~~~~~ ^ ~~~~~~~~~~~~~~~

4553.500 /SRC/obj/arm/fat16-efc5ab81.o.tmp new=/SRC/obj/arm/fat16.o
1 warning generated.

4555.500 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fat16_test.c -o /TOOLS/SH.BIN/
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
SRC/obj/arm/fat16_test.o
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

4557.501 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

4558.501 [IMGPRF] pid=5 done: pages=22997 mapped=22572 skipped=425 batches=180 stop=0 ms=3206
/SRC/src/kernel/fat16_test.c
/SRC/obj/arm/fat16_test-5ab81674.o.tmp

4559.501 /SRC/obj/arm/fat16_test-5ab81674.o.tmp new=/SRC/obj/arm/fat16_test.o
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
[SELFHOST] phase=kernel-compile rc=0

4560.502 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/frame.c -o /SRC/obj/arm/frame.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

4562.502 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

4563.502 [IMGPRF] pid=5 done: pages=22997 mapped=22610 skipped=387 batches=180 stop=0 ms=3792

4564.503 /SRC/src/kernel/frame.c
/SRC/obj/arm/frame-674d2309.o.tmp
/SRC/src/include/frame.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
[DEBUG] Before eret, tf->regs[0] = 1074116792 ELR = 0x0000001002EEF9B0

4623.515 /SRC/obj/arm/frame-674d2309.o.tmp new=/SRC/obj/arm/frame.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/frame_test.c -o /SRC/obj/arm/frame_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128

4624.515 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

4626.516 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

4627.516 [IMGPRF] pid=5 done: pages=22997 mapped=22616 skipped=381 batches=180 stop=0 ms=3988

4628.516 /SRC/src/kernel/frame_test.c
/SRC/obj/arm/frame_test-fc5ab816.o.tmp

4630.517 /SRC/obj/arm/frame_test-fc5ab816.o.tmp new=/SRC/obj/arm/frame_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fs.c -o /SRC/obj//TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
arm/fs.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

4632.517 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

4634.518 [IMGPRF] pid=5 done: pages=22997 mapped=22525 skipped=472 batches=180 stop=0 ms=4078
/SRC/src/kernel/fs.c
/SRC/obj/arm/fs-fc5ab816.o.tmp

4635.518 /SRC/src/include/fs.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fat16.h

4636.518 /SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/pipe.h
/SRC/src/include/unix.h
/SRC/src/include/net.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/vfs.h
/SRC/src/include/errno.h
/SRC/src/include/timer.h
/SRC/src/include/vm.h

5507.703 /SRC/obj/arm/fs-fc5ab816.o.tmp new=/SRC/obj/arm/fs.o

5508.704 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fs_test.c -o /SRC[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

5509.704 /obj/arm/fs_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5511.704 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5512.705 [IMGPRF] pid=5 done: pages=22997 mapped=
5513.705 22598 skipped=399 batches=180 stop=0 ms=3844
/SRC/src/kernel/fs_test.c
/SRC/obj/arm/fs_test-efc5ab81.o.tmp

5514.705 /SRC/obj/arm/fs_test-efc5ab81.o.tmp new=/SRC/obj/arm/fs_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN

5515.705 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/iommu_vtd.c -o /SRC/obj/arm/iommu_vtd.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5518.706 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5519.706 [IMGPRF] pid=5 done: pages=22997 mapped=22631 skipped=366 batches=180 stop=0 ms=3979
/SRC/src/kernel/iommu_vtd.c
/SRC/obj/arm/iommu_vtd-9efc5ab8.o.tmp

5520.707 /SRC/obj/arm/iommu_vtd-9efc5ab8.o.tmp new=/SRC/obj/arm/iommu_vtd.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/ipc_proto.c -o /SRC/obj/arm/ipc_proto.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5523.707 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5524.708 [IMGPRF] pid=5 done: pages=22997 mapped=22550 skipped=447 batches=180 stop=0 ms=4191

5525.708 /SRC/src/kernel/ipc_proto.c
/SRC/obj/arm/ipc_proto-81674d23.o.tmp
/SRC/src/include/ipc_proto.h
/TOOLS/RESDIR/include/stdint.h

5526.708 /SRC/src/include/errno.h

5541.712 /SRC/obj/arm/ipc_proto-81674d23.o.tmp new=/SRC/obj/arm/ipc_proto.o

5542.712 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/locks_test.c -o /SRC/obj/arm/locks_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5545.713 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5546.713 [IMGPRF] pid=5 done: pages=22997 mapped=22638 skipped=359 batches=180 stop=0 ms=4208
/SRC/src/kernel/locks_test.c
/SRC/obj/arm/locks_test-ab81674d.o.tmp

5548.713 /SRC/obj/arm/locks_test-ab81674d.o.tmp new=/SRC/obj/arm/locks_test.o

5549.714 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/obj/arm/main.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5552.714 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5553.715 [IMGPRF] pid=5 done: pages=22997 mapped=22611 skipped=386 batches=180 stop=0 ms=3917
/SRC/src/kernel/main.c
/SRC/obj/arm/main-c5ab8167.o.tmp

5554.715 /SRC/src/include/fat16.h
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
/SRC/src/include/virtio_input.h
/SRC/src/include/net.h
/SRC/src/include/virtio_net.h
/SRC/src/include/dhcp.h
src/kernel/main.c:50:39: warning: unused parameter 'tf' [-Wunused-parameter]
   50 | void irq_handler_c(struct trap_frame *tf) {
      |                                       ^

5560.716 /SRC/obj/arm/main-c5ab8167.o.tmp new=/SRC/obj/arm/main.o
1 warning generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/math_test.c -o /S[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
RC/obj/arm/math_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128

5561.716 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5563.717 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5564.717 [IMGPRF] pid=5 done: pages=22997 mapped=22612 skipped=385 batches=180 stop=0 ms=3926

5565.717 /SRC/src/kernel/math_test.c
/SRC/obj/arm/math_test-c5ab8167.o.tmp
/SRC/obj/arm/math_test-c5ab8167.o.tmp new=/SRC/obj/arm/math_test.o

5566.718 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/mmu_test.c -o /SR/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
C/obj/arm/mmu_test.o
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

5568.718 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5569.718 [IMGPRF] pid=5 done: pages=22997 mapped=22525 skipped=472 batches=180 stop=0 ms=3463
/SRC/src/kernel/mmu_test.c
/SRC/obj/arm/mmu_test-c5ab8167.o.tmp

5571.719 /SRC/obj/arm/mmu_test-c5ab8167.o.tmp new=/SRC/obj/arm/mmu_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now /TOOLS/CLANG.4B
IN -O2 -W[DEBUG] Before eret, tf->regs[0] = a4l ELR = l0x00000010000260F8 
-Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net.c -o /SRC/obj/TOOLS/SH.BIN
/arm/net.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5573.719 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5575.720 [IMGPRF] pid=5 done: pages=22997 mapped=22556 skipped=441 batches=180 stop=0 ms=3680
/SRC/src/kernel/net.c
/SRC/obj/arm/net-9efc5ab8.o.tmp

5577.720 /SRC/src/include/net.h
/TOOLS/RESDIR/include/stdint.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/virtio_net.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/timer.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/net_rdma.h
/SRC/src/include/errno.h

5578.721 src/kernel/net.c:519:18: warning: variable 'stored' set but not used [-Wunused-but-set-variable]
  519 |         uint32_t stored = 0;
      |                  ^
src/kernel/net.c:782:21: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
  782 |   for (int i = 0; i < sizeof(struct pseudo_hdr)/2; i++) {
      |                   ~ ^ ~~~~~~~~~~~~~~~~~~~~~~~~~~~

5579.721 src/kernel/net.c:624:39: warning: unused function 'safe_print_int' [-Wunused-function]
  624 | __attribute__((noinline)) static void safe_print_int(int val) {
      |                                       ^~~~~~~~~~~~~~

5699.748 /SRC/obj/arm/net-9efc5ab8.o.tmp new=/SRC/obj/arm/net.o
3 warnings generated.

5700.748 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net_rdma.c -o /SR[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
C/obj/arm/net_rdma.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5702.749 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5704.749 [IMGPRF] pid=5 done: pages=22997 mapped=22548 skipped=449 batches=180 stop=0 ms=3688
/SRC/src/kernel/net_rdma.c
/SRC/obj/arm/net_rdma-1674d230.o.tmp

5705.750 /SRC/src/include/net_rdma.h
/TOOLS/RESDIR/include/stdint.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h

5712.751 /SRC/obj/arm/net_rdma-1674d230.o.tmp new=/SRC/obj/arm/net_rdma.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now /4T
OOLS/CLANG.BI[DEBUG] Before eret, tf->regs[0] = N4  ELR = -0x00000010000260F8O
2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net_rdma_test.c -/TOOLS/SH.BIN
o /SRC/obj/arm/net_rdma_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

5713.751 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5715.752 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5717.752 [IMGPRF] pid=5 done: pages=22997 mapped=22567 skipped=430 batches=180 stop=0 ms=4073
/SRC/src/kernel/net_rdma_test.c
/SRC/obj/arm/net_rdma_test-4d2309ef.o.tmp

5718.752 /SRC/obj/arm/net_rdma_test-4d2309ef.o.tmp new=/SRC/obj/arm/net_rdma_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net_test.c -o /SRC/obj/arm/net_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5720.753 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5722.753 [IMGPRF] pid=5 done: pages=22997 mapped=22526 skipped=471 batches=180 stop=0 ms=4199
/SRC/src/kernel/net_test.c
/SRC/obj/arm/net_test-2309efc5.o.tmp

5723.753 /SRC/obj/arm/net_test-2309efc5.o.tmp new=/SRC/obj/arm/net_test.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs.c -o /SRC/obj/arm/nfs.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

5724.754 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5725.754 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5727.754 [IMGPRF] pid=5 done: pages=22997 mapped=22490 skipped=507 batches=180 stop=0 ms=3762
/SRC/src/kernel/nfs.c
/SRC/obj/arm/nfs-c5ab8167.o.tmp

5728.755 /SRC/src/include/nfs.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/net.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h

5729.755 src/kernel/nfs.c:157:12: warning: '/*' within block comment [-Wcomment]
  157 |  * is *body/*body_len, valid until the next rpc_call (static buffer). */
      |            ^

5871.787 /SRC/obj/arm/nfs-c5ab8167.o.tmp new=/SRC/obj/arm/nfs.o
1 warning generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

5872.787 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs_proto.c -o /SRC/obj/arm/nfs_proto.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

5874.788 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

5875.789 [IMGPRF] pid=5 done: pages=22997 mapped=22532 skipped=465 batches=180 stop=0 ms=3605
/SRC/src/kernel/nfs_proto.c
/SRC/obj/arm/nfs_proto-fc5ab816.o.tmp

5876.789 /SRC/src/include/nfs.h
/TOOLS/RESDIR/include/stdint.h

6056.827 /SRC/obj/arm/nfs_proto-fc5ab816.o.tmp new=/SRC/obj/arm/nfs_proto.o

6057.827 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /TOOLS/0x00000010000260F8C
LANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs_test.c -o /SRC/obj/arm/nfs_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

6059.828 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

6061.828 [IMGPRF] pid=5 done: pages=22997 mapped=22545 skipped=452 batches=180 stop=0 ms=4012
/SRC/src/kernel/nfs_test.c
/SRC/obj/arm/nfs_test-09efc5ab.o.tmp

6063.829 /SRC/obj/arm/nfs_test-09efc5ab.o.tmp new=/SRC/obj/arm/nfs_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

6064.829 [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/pipe.c -o /SRC/obj/arm/pipe.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

6066.830 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

6068.831 [IMGPRF] pid=5 done: pages=22997 mapped=22476 skipped=521 batches=180 stop=0 ms=4503
/SRC/src/kernel/pipe.c
/SRC/obj/arm/pipe-efc5ab81.o.tmp

6069.831 /SRC/src/include/pipe.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/errno.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h

6070.831 src/kernel/pipe.c:139:7: warning: unused variable 'i' [-Wunused-variable]
  139 |   int i = 0;
      |       ^
src/kernel/pipe.c:136:68: warning: unused parameter 'tf' [-Wunused-parameter]
  136 | int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf) {
      |                                                                    ^
src/kernel/pipe.c:216:7: warning: unused variable 'i' [-Wunused-variable]
  216 |   int i = 0;
      |       ^

6080.833 /SRC/obj/arm/pipe-efc5ab81.o.tmp new=/SRC/obj/arm/pipe.o
3 warnings generated.
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/pipe_test.c -o /S/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = R-1C ELR = /0x00000010048493F0o
bj/arm/pipe_test.o

6082.834 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

6084.834 [IMGPRF] pid=5 done: pages=22997 mapped=22508 skipped=489 batches=180 stop=0 ms=3922
/SRC/src/kernel/pipe_test.c
/SRC/obj/arm/pipe_test-09efc5ab.o.tmp

6085.835 /SRC/obj/arm/pipe_test-09efc5ab.o.tmp new=/SRC/obj/arm/pipe_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

6086.835 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/process.c -o /SRC[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/obj/arm/process.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

6088.836 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

6090.836 [IMGPRF] pid=5 done: pages=22997 mapped=22466 skipped=531 batches=180 stop=0 ms=4399
/SRC/src/kernel/process.c
/SRC/obj/arm/process-9efc5ab8.o.tmp

6091.836 /SRC/src/include/process.h
/SRC/src/include/lock.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/frame.h
/SRC/src/include/vm.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/mmu.h
/SRC/src/include/arch/mmu.h
/SRC/src/include/setjmp.h
/SRC/src/include/timer.h
/SRC/src/include/errno.h
/SRC/src/include/syscall.h

6092.836 src/kernel/process.c:1169:42: warning: unused parameter 'is_yield' [-Wunused-parameter]
 1169 | void schedule(struct trap_frame *tf, int is_yield) {
      |                                          ^

6094.837 src/kernel/process.c:2997:50: warning: unused parameter 'prot' [-Wunused-parameter]
 2997 | int64_t sys_mmap(int64_t addr, uint64_t len, int prot, int flags) {
      |                                                  ^

7286.107 /SRC/obj/arm/process-9efc5ab8.o.tmp new=/SRC/obj/arm/process.o
2 warnings generated.

7288.107 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOOL4S ELR = /0x00000010000260F8C
LANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/process_test.c -o/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
 /SRC/obj/arm/process_test.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7290.108 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7293.109 [IMGPRF] pid=5 done: pages=22997 mapped=22473 skipped=524 batches=180 stop=0 ms=4858
/SRC/src/kernel/process_test.c
/SRC/obj/arm/process_test-c5ab8167.o.tmp

7294.109 /SRC/obj/arm/process_test-c5ab8167.o.tmp new=/SRC/obj/arm/process_test.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/program_loader.c -o /SRC/obj/arm/program_loader.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7296.109 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7298.110 [IMGPRF] pid=5 done: pages=22997 mapped=22524 skipped=473 batches=180 stop=0 ms=3774
/SRC/src/kernel/program_loader.c
/SRC/obj/arm/program_loader-ab81674d.o.tmp

7299.110 /SRC/src/include/program_loader.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/setjmp.h
/SRC/src/include/errno.h
/SRC/src/include/timer.h
/SRC/src/include/frame.h
/SRC/src/include/vm.h

7353.123 /SRC/obj/arm/program_loader-ab81674d.o.tmp new=/SRC/obj/arm/program_loader.o

7354.123 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=/TOOLS/SH.BINc
ortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/program_loader_tev2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
st.c -o /SRC/obj/arm/program_loader_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7356.123 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7358.124 [IMGPRF] pid=5 done: pages=22997 mapped=22497 skipped=500 batches=180 stop=0 ms=3945
/SRC/src/kernel/program_loader_test.c
/SRC/obj/arm/program_loader_test-d2309efc.o.tmp

7359.124 /SRC/obj/arm/program_loader_test-d2309efc.o.tmp new=/SRC/obj/arm/program_loader_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TO4O ELR = L0x00000010000260F8S
/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/test_fw_cfg.c -o /TOOLS/SH.BIN
/SRC/obj/arm/test_fw_cfg.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

7360.124 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7361.124 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7363.125 [IMGPRF] pid=5 done: pages=22997 mapped=22518 skipped=479 batches=180 stop=0 ms=3403
/SRC/src/kernel/test_fw_cfg.c
/SRC/obj/arm/test_fw_cfg-2309efc5.o.tmp

7364.125 /TOOLS/RESDIR/include/stdint.h

7365.125 /SRC/obj/arm/test_fw_cfg-2309efc5.o.tmp new=/SRC/obj/arm/test_fw_cfg.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/test_smp.c -o /SRC/obj/arm/test_smp.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7367.126 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7369.126 [IMGPRF] pid=5 done: pages=22997 mapped=22558 skipped=439 batches=180 stop=0 ms=3488
/SRC/src/kernel/test_smp.c
/SRC/obj/arm/test_smp-674d2309.o.tmp

7370.126 /SRC/obj/arm/test_smp-674d2309.o.tmp new=/SRC/obj/arm/test_smp.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/time.c -o /SRC/ob/TOOLS/SH.BIN
j/arm/time.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7372.127 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7375.128 [IMGPRF] pid=5 done: pages=22997 mapped=22485 skipped=512 batches=180 stop=0 ms=4522
/SRC/src/kernel/time.c
/SRC/obj/arm/time-5ab81674.o.tmp
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

7376.128 /SRC/src/include/timer.h
[DEBUG] Before eret, tf->regs[0] = 1074116888 ELR = 0x00000010043C0B64

7380.129 /SRC/obj/arm/time-5ab81674.o.tmp new=/SRC/obj/arm/time.o
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
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/time_test.c -o /S
7381.130 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
RC/obj/arm/time_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7383.131 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7385.131 [IMGPRF] pid=5 done: pages=22997 mapped=22450 skipped=547 batches=180 stop=0 ms=4261
/SRC/src/kernel/time_test.c
/SRC/obj/arm/time_test-c5ab8167.o.tmp

7386.131 /SRC/obj/arm/time_test-c5ab8167.o.tmp new=/SRC/obj/arm/time_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/timer_test.c -o /SRC/obj/arm/timer_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7388.132 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7390.132 [IMGPRF] pid=5 done: pages=22997 mapped=22506 skipped=491 batches=180 stop=0 ms=3860
/SRC/src/kernel/timer_test.c
/SRC/obj/arm/timer_test-2309efc5.o.tmp

7392.133 /SRC/obj/arm/timer_test-2309efc5.o.tmp new=/SRC/obj/arm/timer_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

7393.133 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/trap_test.c -o /S[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
RC/obj/arm/trap_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7395.134 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7397.135 [IMGPRF] pid=5 done: pages=22997 mapped=22486 skipped=511 batches=180 stop=0 ms=3978
/SRC/src/kernel/trap_test.c
/SRC/obj/arm/trap_test-74d2309e.o.tmp

7398.135 /SRC/obj/arm/trap_test-74d2309e.o.tmp new=/SRC/obj/arm/trap_test.o
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
[SELFHOST] phase=kernel-compile rc=0

7399.136 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unit_test.c -o /SRC/obj/arm/unit_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/CLANG.BIN0x0000001000012720

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7401.137 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7403.137 [IMGPRF] pid=5 done: pages=22997 mapped=22522 skipped=475 batches=180 stop=0 ms=4060
/SRC/src/kernel/unit_test.c
/SRC/obj/arm/unit_test-b81674d2.o.tmp

7404.137 /SRC/obj/arm/unit_test-b81674d2.o.tmp new=/SRC/obj/arm/unit_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unix.c -o /SRC/obj/arm/unix.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7406.138 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7408.138 [IMGPRF] pid=5 done: pages=22997 mapped=22499 skipped=498 batches=180 stop=0 ms=4114

7409.139 /SRC/src/kernel/unix.c
/SRC/obj/arm/unix-09efc5ab.o.tmp
/SRC/src/include/unix.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/ipc_proto.h

7410.139 /SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/errno.h

7492.160 /SRC/obj/arm/unix-09efc5ab.o.tmp new=/SRC/obj/arm/unix.o

7493.160 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unix_test.c -o /S/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
RC/obj/arm/unix_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7495.160 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7497.161 [IMGPRF] pid=5 done: pages=22997 mapped=22489 skipped=508 batches=180 stop=0 ms=4166
/SRC/src/kernel/unix_test.c
/SRC/obj/arm/unix_test-4d2309ef.o.tmp

7498.161 /SRC/obj/arm/unix_test-4d2309ef.o.tmp new=/SRC/obj/arm/unix_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

7499.161 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vfs.c -o /SRC/obj/arm/vfs.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7501.162 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7503.162 [IMGPRF] pid=5 done: pages=22997 mapped=22485 skipped=512 batches=180 stop=0 ms=4396
/SRC/src/kernel/vfs.c
/SRC/obj/arm/vfs-674d2309.o.tmp

7504.163 /SRC/src/include/vfs.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/nfs.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/errno.h

7523.167 /SRC/obj/arm/vfs-674d2309.o.tmp new=/SRC/obj/arm/vfs.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_blk.c -o /SRC/obj/arm/virtio_blk.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/CLANG.BIN0x0000001000012720

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7525.167 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7527.168 [IMGPRF] pid=5 done: pages=22997 mapped=22502 skipped=495 batches=180 stop=0 ms=3622
/SRC/src/kernel/virtio_blk.c
/SRC/obj/arm/virtio_blk-309efc5a.o.tmp

7528.168 /SRC/src/include/virtio_blk.h
/TOOLS/RESDIR/include/stdint.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
src/kernel/virtio_blk.c:655:12: warning: unused variable 'desc_idx' [-Wunused-variable]
  655 |   uint16_t desc_idx = 0;
      |            ^~~~~~~~

7535.170 /SRC/obj/arm/virtio_blk-309efc5a.o.tmp new=/SRC/obj/arm/virtio_blk.o
1 warning generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

7536.170 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4/TOOLS/CLANG.BIN -O2 -Wall
 -We[DEBUG] Before eret, tf->regs[0] = x4t ELR = r0x00000010000260F8a
 -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_blk_test.c/TOOLS/SH.BIN
 -o /SRC/obj/arm/virtio_blk_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7538.170 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7540.171 [IMGPRF] pid=5 done: pages=22997 mapped=22439 skipped=558 batches=180 stop=0 ms=4507
/SRC/src/kernel/virtio_blk_test.c
/SRC/obj/arm/virtio_blk_test-d2309efc.o.tmp

7543.172 /SRC/obj/arm/virtio_blk_test-d2309efc.o.tmp new=/SRC/obj/arm/virtio_blk_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu.c -o /SRC/obj/arm/virtio_gpu.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7545.172 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7547.172 [IMGPRF] pid=5 done: pages=22997 mapped=22518 skipped=479 batches=180 stop=0 ms=3809
/SRC/src/kernel/virtio_gpu.c
/SRC/obj/arm/virtio_gpu-b81674d2.o.tmp
/SRC/src/include/virtio_gpu.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/display_mode.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

7577.178 /SRC/obj/arm/virtio_gpu-b81674d2.o.tmp new=/SRC/obj/arm/virtio_gpu.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none/TOOLS/CLANG.BIN[DEBUG] Before eret, tf->regs[0] = -
e5l ELR = f0x0000001000012720 
-ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu_test.cv2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128

7578.178  -o /SRC/obj/arm/virtio_gpu_test.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7579.178 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7582.179 [IMGPRF] pid=5 done: pages=22997 mapped=22456 skipped=541 batches=180 stop=0 ms=4617
/SRC/src/kernel/virtio_gpu_test.c
/SRC/obj/arm/virtio_gpu_test-674d2309.o.tmp

7583.179 /SRC/obj/arm/virtio_gpu_test-674d2309.o.tmp new=/SRC/obj/arm/virtio_gpu_test.o
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
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu_x64.c [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
-o /SRC/obj/arm/virtio_gpu_x64.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

7584.179 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7586.180 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7588.180 [IMGPRF] pid=5 done: pages=22997 mapped=22438 skipped=559 batches=180 stop=0 ms=4828
/SRC/src/kernel/virtio_gpu_x64.c
/SRC/obj/arm/virtio_gpu_x64-2309efc5.o.tmp

7590.180 /SRC/obj/arm/virtio_gpu_x64-2309efc5.o.tmp new=/SRC/obj/arm/virtio_gpu_x64.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_input.c -o /SRC/obj/arm/virtio_input.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7592.181 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7595.181 [IMGPRF] pid=5 done: pages=22997 mapped=22456 skipped=541 batches=180 stop=0 ms=4523
/SRC/src/kernel/virtio_input.c
/SRC/obj/arm/virtio_input-fc5ab816.o.tmp

7596.182 /SRC/src/include/virtio_input.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

7611.185 /SRC/obj/arm/virtio_input-fc5ab816.o.tmp new=/SRC/obj/arm/virtio_input.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_input_test/TOOLS/SH.BIN
.c -o /SRC/obj/arm/virtio_input_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/CLANG.BIN0x0000001000012720

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7614.186 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7616.186 [IMGPRF] pid=5 done: pages=22997 mapped=22488 skipped=509 batches=180 stop=0 ms=4952

7617.187 /SRC/src/kernel/virtio_input_test.c
/SRC/obj/arm/virtio_input_test-9efc5ab8.o.tmp

7618.187 /SRC/obj/arm/virtio_input_test-9efc5ab8.o.tmp new=/SRC/obj/arm/virtio_input_test.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4/
TOOLS/C[DEBUG] Before eret, tf->regs[0] = L4A ELR = NG0x00000010000260F8.
BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_net.c -o //TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
SRC/obj/arm/virtio_net.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7620.187 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7622.188 [IMGPRF] pid=5 done: pages=22997 mapped=22546 skipped=451 batches=180 stop=0 ms=3947
/SRC/src/kernel/virtio_net.c
/SRC/obj/arm/virtio_net-efc5ab81.o.tmp

7623.188 /SRC/src/include/virtio_net.h
/TOOLS/RESDIR/include/stdint.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h

7624.188 src/kernel/virtio_net.c:699:20: warning: unused function 'reg_write8' [-Wunused-function]
  699 | static inline void reg_write8(uint32_t offset, uint8_t val) {
      |                    ^~~~~~~~~~

7651.195 /SRC/obj/arm/virtio_net-efc5ab81.o.tmp new=/SRC/obj/arm/virtio_net.o
1 warning generated.
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_net_test.c[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
 -o /SRC/obj/arm/virtio_net_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7653.195 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7656.196 [IMGPRF] pid=5 done: pages=22997 mapped=22541 skipped=456 batches=180 stop=0 ms=4451
/SRC/src/kernel/virtio_net_test.c
/SRC/obj/arm/virtio_net_test-09efc5ab.o.tmp

7657.196 /SRC/obj/arm/virtio_net_test-09efc5ab.o.tmp new=/SRC/obj/arm/virtio_net_test.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vm.c -o /SRC/obj//TOOLS/SH.BIN
arm/vm.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7659.197 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7662.197 [IMGPRF] pid=5 done: pages=22997 mapped=22439 skipped=558 batches=180 stop=0 ms=4731
/SRC/src/kernel/vm.c
/SRC/obj/arm/vm-674d2309.o.tmp
/SRC/src/include/vm.h
/TOOLS/RESDIR/include/stdint.h

7663.198 /SRC/src/include/errno.h
/SRC/src/include/fat16.h
/SRC/src/include/frame.h
/SRC/src/include/fs.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h

7959.255 /SRC/obj/arm/vm-674d2309.o.tmp new=/SRC/obj/arm/vm.o

7960.256 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

7961.256 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vm_test.c -o /SRC[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/obj/arm/vm_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7963.256 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7965.257 [IMGPRF] pid=5 done: pages=22997 mapped=22430 skipped=567 batches=180 stop=0 ms=4463
/SRC/src/kernel/vm_test.c
/SRC/obj/arm/vm_test-4d2309ef.o.tmp

7967.257 /SRC/obj/arm/vm_test-4d2309ef.o.tmp new=/SRC/obj/arm/vm_test.o

7968.257 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/cpu.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/cpu.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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

7971.258 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7973.258 [IMGPRF] pid=5 done: pages=22997 mapped=22545 skipped=452 batches=180 stop=0 ms=4623
/SRC/src/kernel/arch/arm/cpu.c
/SRC/obj/arm/cpu-c5ab8167.o.tmp
/SRC/src/include/arch/cpu.h
/TOOLS/RESDIR/include/stdint.h

7976.259 /SRC/obj/arm/cpu-c5ab8167.o.tmp new=/SRC/obj/arm/cpu.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/gic.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/gic.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

7977.259 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7978.259 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7980.260 [IMGPRF] pid=5 done: pages=22997 mapped=22526 skipped=471 batches=180 stop=0 ms=3752
/SRC/src/kernel/arch/arm/gic.c
/SRC/obj/arm/gic-9efc5ab8.o.tmp

7981.260 /SRC/src/include/gic.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/process.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h

7988.261 /SRC/obj/arm/gic-9efc5ab8.o.tmp new=/SRC/obj/arm/gic.o

7989.261 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/locks.c /TOOLS/SH.BIN
-o /SRC/obj/arm/locks.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

7991.262 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

7993.262 [IMGPRF] pid=5 done: pages=22997 mapped=22493 skipped=504 batches=180 stop=0 ms=4250
/SRC/src/kernel/arch/arm/locks.c
/SRC/obj/arm/locks-09efc5ab.o.tmp

7995.263 /SRC/src/include/lock.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h

8002.264 /SRC/obj/arm/locks-09efc5ab.o.tmp new=/SRC/obj/arm/locks.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/mmu.c -o[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
 /SRC/obj/arm/mmu.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8004.264 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8005.264 [IMGPRF] pid=5 done: pages=22997 mapped=22558 skipped=439 batches=180 stop=0 ms=3466

8006.264 /SRC/src/kernel/arch/arm/mmu.c
/SRC/obj/arm/mmu-81674d23.o.tmp
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/mmu.h
/SRC/src/include/arch/mmu.h
/SRC/src/include/process.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/frame.h
/SRC/src/include/vm.h

8077.277 /SRC/obj/arm/mmu-81674d23.o.tmp new=/SRC/obj/arm/mmu.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

8078.277 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/rtc.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/rtc.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8079.277 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8081.278 [IMGPRF] pid=5 done: pages=22997 mapped=22546 skipped=451 batches=180 stop=0 ms=3457
/SRC/src/kernel/arch/arm/rtc.c
/SRC/obj/arm/rtc-b81674d2.o.tmp

8082.278 /TOOLS/RESDIR/include/stdint.h

8083.278 /SRC/obj/arm/rtc-b81674d2.o.tmp new=/SRC/obj/arm/rtc.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/smp.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/smp.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8085.278 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8087.279 [IMGPRF] pid=5 done: pages=22997 mapped=22555 skipped=442 batches=180 stop=0 ms=3341
/SRC/src/kernel/arch/arm/smp.c
/SRC/obj/arm/smp-81674d23.o.tmp
/SRC/src/include/lock.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h

8088.279 /SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h

8090.280 /SRC/obj/arm/smp-81674d23.o.tmp new=/SRC/obj/arm/smp.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

8091.280 /TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/timer.c [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
-o /SRC/obj/arm/timer.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8093.280 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8095.281 [IMGPRF] pid=5 done: pages=22997 mapped=22553 skipped=444 batches=180 stop=0 ms=3815
/SRC/src/kernel/arch/arm/timer.c
/SRC/obj/arm/timer-4d2309ef.o.tmp

8096.281 /SRC/src/include/timer.h
/TOOLS/RESDIR/include/stdint.h

8100.282 /SRC/obj/arm/timer-4d2309ef.o.tmp new=/SRC/obj/arm/timer.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/trap.c -/TOOLS/SH.BIN
o /SRC/obj/arm/trap.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8103.282 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8105.283 [IMGPRF] pid=5 done: pages=22997 mapped=22536 skipped=461 batches=180 stop=0 ms=4220
/SRC/src/kernel/arch/arm/trap.c
/SRC/obj/arm/trap-2309efc5.o.tmp
/SRC/src/include/trap.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/trap.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h

8106.283 /SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/net.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/setjmp.h
/SRC/src/include/timer.h
/SRC/src/include/arch/timer.h
/SRC/src/include/syscall.h
/SRC/src/include/errno.h
/SRC/src/include/vfs.h
/SRC/src/include/vm.h

8107.283 src/kernel/arch/arm/trap.c:1602:16: warning: unused variable 'l2idx' [-Wunused-variable]
 1602 |       uint32_t l2idx = ((uint64_t)tf->elr >> 21) & 0x1FF;
      |                ^~~~~
src/kernel/arch/arm/trap.c:1607:22: warning: array index 512 is past the end of the array (that has type 'uint64_t[512]' (aka 'unsigned long[512]')) [-Warray-bounds]
 1607 |       uart_print_hex(l2_table_1[cp][0x200]);
      |                      ^              ~~~~~
src/kernel/arch/arm/trap.c:1601:7: note: array 'l2_table_1' declared here
 1601 |       extern uint64_t l2_table_1[][512];
      |       ^
src/kernel/arch/arm/trap.c:1609:22: warning: array index 544 is past the end of the array (that has type 'uint64_t[512]' (aka 'unsigned long[512]')) [-Warray-bounds]
 1609 |       uart_print_hex(l2_table_1[cp][0x220]);
      |                      ^              ~~~~~
src/kernel/arch/arm/trap.c:1601:7: note: array 'l2_table_1' declared here
 1601 |       extern uint64_t l2_table_1[][512];
      |       ^

8445.360 /SRC/obj/arm/trap-2309efc5.o.tmp new=/SRC/obj/arm/trap.o
3 warnings generated.

8447.360 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/uart.c -/TOOLS/SH.BIN
o /SRC/obj/arm/uart.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8450.361 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8452.361 [IMGPRF] pid=5 done: pages=22997 mapped=22547 skipped=450 batches=180 stop=0 ms=4774
/SRC/src/kernel/arch/arm/uart.c
/SRC/obj/arm/uart-74d2309e.o.tmp

8453.362 /SRC/src/include/lock.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h

8464.364 /SRC/obj/arm/uart-74d2309e.o.tmp new=/SRC/obj/arm/uart.o
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
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/boot.s -/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

8465.364 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
o /SRC/ob[DEBUG] Before eret, tf->regs[0] = j5/ ELR = a0x0000001000012720r
m/boot.o
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8467.365 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8469.365 CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/boot.s
/SRC/obj/arm/boot.o
[IMGPRF] pid=5 done: pages=22997 mapped=22400 skipped=597 batches=180 stop=0 ms=4707

8470.366 [KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/fpu.s -o /SRC/obj/arm/fpu.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/CLANG.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8472.366 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8474.366 [IMGPRF] pid=5 done: pages=22997 mapped=22540 skipped=457 batches=180 stop=0 ms=4046
CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/fpu.s
/SRC/obj/arm/fpu.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/setjmp.s/TOOLS/SH.BIN

8475.367  -o /SRC/obj/arm/setjmp.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=5 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

8477.367 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

8479.368 [IMGPRF] pid=5 done: pages=22997 mapped=22496 skipped=501 batches=180 stop=0 ms=4027
CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/setjmp.s
/SRC/obj/arm/setjmp.o
[KERNEL] Process 5: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=s2-kernel[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
SELFHOST] phase=kernel-link-start
[KERNEL] sys_fork: tf->regs[0] is now 4
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now # 4f
a[DEBUG] Before eret, tf->regs[0] = l4l ELR = b0x00000010000260F8a
ck, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

8480.368 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = #4  ELR = the 0x00000010000260F8respon
se file is written at parse time by the make file-write
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# function attached to the KERNEL_OBJS definition above (no shell, no
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = # argv ca0x00000010000260F8p
) and the link line itself stays short.
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

8481.368 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

8482.369 [IMGPRF] pid=5 done: pages=13001 mapped=12607 skipped=394 batches=102 stop=0 ms=1978
/SRC/OBJS.RSP
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

8483.369 /SRC/obj/arm/unix_test.o
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
/SRC/hobbyos.elf.tmp674d230

8484.369 /SRC/hobbyos.elf.tmp9efc5ab

8487.370 /SRC/hobbyos.elf

8493.371 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
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
/TOOLS/OBJCOPY.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90

8494.372 [IMGPRF] pid=5 done: pages=1217 mapped=1134 skipped=83 batches=10 stop=0 ms=168
/SRC/hobbyos.elf
/SRC/hobbyos.bin.temp-stream-1674d2

8495.372 /SRC/hobbyos.bin.temp-stream-1674d2 new=/SRC/hobbyos.bin
/SRC/hobbyos.bin
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
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

8496.372 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOS[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
T] phase=s2-kernel-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-link-s[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
tart
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
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# HobbyOS crt0 positional-arg cap (CRT0_MAX_ARGS 32) on the flat-args
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

8497.373 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = #4  ELR = f0x00000010000260F8a
llback, which merged the tail into one path ("cannot open /SRC/");
# the response file is written at parse time by the make file-write
# function attached to the KERNEL_OBJS definition abo/TOOLS/SH.BIN
ve (no shell, no
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# argv cap) and the link line itself stays short.
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN -flavor gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

8498.373 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

8499.374 [IMGPRF] pid=5 done: pages=13001 mapped=12639 skipped=362 batches=102 stop=0 ms=1846
/SRC/OBJS.RSP
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

8500.374 /SRC/obj/arm/virtio_gpu_test.o
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
/SRC/hobbyos.elf.tmpd2309ef
/SRC/hobbyos.elf

8501.374 /SRC/hobbyos.elf.tmpc5ab816

8504.375 /SRC/hobbyos.elf

8510.376 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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
[DEBUG] Before eret, tf->regs[0] = /TOOLS/OBJCOPY.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90

8511.376 [IMGPRF] pid=5 done: pages=1217 mapped=1135 skipped=82 batches=10 stop=0 ms=158
/SRC/hobbyos.elf
/SRC/hobbyos.bin.temp-stream-74d230

8512.376 /SRC/hobbyos.bin.temp-stream-74d230 new=/SRC/hobbyos.bin
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

8513.377 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] ph[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
ase=s2-kernel-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] pha[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
se=kernel-link-start
[KERNEL] sys_fork: tf->regs[0] is now 4
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = #4  ELR = f0x00000010000260F8a
llback, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
# the response file is written at parse time by the make file-write
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# function attached to the KERNEL_OBJS definition above (no shell, no
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
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T linker.ld -o /SRC/hobbyos.elf @/SRC/OBJS.RSP
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN

8514.377 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

8515.377 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12678 skipped=323 batches=102 stop=0 ms=1847
/SRC/OBJS.RSP
/SRC/OBJS.RSP
/SRC/linker.ld

8516.377 /SRC/obj/arm/dhcp.o
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
/SRC/hobbyos.elf.tmp9efc5ab

8517.378 /SRC/hobbyos.elf
/SRC/hobbyos.elf.tmp81674d2

8520.378 /SRC/hobbyos.elf

8526.380 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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
/TOOLS/OBJCOPY.BIN
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1075 skipped=142 batches=10 stop=0 ms=179
/SRC/hobbyos.elf

8527.380 /SRC/hobbyos.bin.temp-stream-74d230
/SRC/hobbyos.bin.temp-stream-74d230 new=/SRC/hobbyos.bin
/SRC/hobbyos.bin
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

8528.380 [KERNEL] sys_fork: tf->regs[0] is now 4
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phas[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
e=s1-user-start
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
/TOOLS/LLD.BIN
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

8529.380 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

8530.380 [IMGPRF] pid=5 done: pages=13001 mapped=12639 skipped=362 batches=102 stop=0 ms=2449
/SRC/src/user/linker.ld
/SRC/obj/arm/sh.o
/SRC/obj/arm/sh_aux.o
/SRC/obj/arm/libc.a

8531.381 /SRC/obj/arm/sh.elf.tmp74d2309
/SRC/obj/arm/sh.elf
/SRC/obj/arm/sh.elf.tmpefc5ab8

8532.381 /SRC/obj/arm/sh.elf

8535.382 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = T4O ELR = O0x00000010000260F8LS/OBJC
OPY.BIN -O binary /SRC/obj/arm/sh.elf /SRC/obj/arm/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = /TOOLS/OBJCOPY.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1126 skipped=91 batches=10 stop=0 ms=163
/SRC/obj/arm/sh.elf
/SRC/obj/arm/SH.BIN.temp-stream-674d23

8536.382 /SRC/obj/arm/SH.BIN.temp-stream-674d23 new=/SRC/obj/arm/SH.BIN
/SRC/obj/arm/SH.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/ls.elf /SRC/obj/arm/ls.o /SRC/obj/arm/libc.a
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
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

8538.383 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12607 skipped=394 batches=102 stop=0 ms=2011
/SRC/src/user/linker.ld
/SRC/obj/arm/ls.o
/SRC/obj/arm/libc.a

8539.383 /SRC/obj/arm/ls.elf.tmp09efc5a
/SRC/obj/arm/ls.elf
/SRC/obj/arm/ls.elf.tmpb81674d

8540.383 /SRC/obj/arm/ls.elf
[KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

8541.383 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/ls.elf /SRC/obj/arm/LS.BIN
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
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1125 skipped=92 batches=10 stop=0 ms=147
/SRC/obj/arm/ls.elf
/SRC/obj/arm/LS.BIN.temp-stream-efc5ab
/SRC/obj/arm/LS.BIN.temp-stream-efc5ab new=/SRC/obj/arm/LS.BIN
/SRC/obj/arm/LS.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/cat.elf /SRC/obj/arm/cat.o /SRC/obj/arm/libc.a
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

8543.384 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12685 skipped=316 batches=102 stop=0 ms=2100
/SRC/src/user/linker.ld

8544.385 /SRC/obj/arm/cat.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/cat.elf.tmp5ab8167
/SRC/obj/arm/cat.elf
/SRC/obj/arm/cat.elf.tmp4d2309e

8545.385 /SRC/obj/arm/cat.elf

8546.385 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
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
/TOOLS/OBJCOPY.BIN
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1135 skipped=82 batches=10 stop=0 ms=164
/SRC/obj/arm/cat.elf
/SRC/obj/arm/CAT.BIN.temp-stream-5ab816
/SRC/obj/arm/CAT.BIN.temp-stream-5ab816 new=/SRC/obj/arm/CAT.BIN
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
[DEBUG] Before eret, tf->regs[0] = /TOOLS/LLD.BIN5
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

8548.386 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4
[IMGPRF] pid=5 done: pages=13001 mapped=12642 skipped=359 batches=102 stop=0 ms=1938
/SRC/src/user/linker.ld
/SRC/obj/arm/hello.o
/SRC/obj/arm/libc.a

8549.386 /SRC/obj/arm/hello.elf.tmp74d2309
/SRC/obj/arm/hello.elf
/SRC/obj/arm/hello.elf.tmpefc5ab8
/SRC/obj/arm/hello.elf

8550.386 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOOL4S ELR = /0x00000010000260F8O
BJCOPY.BIN -O binary /SRC/obj/arm/hello.elf /SRC/obj/arm/HELLO.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/OBJCOPY.BIN

8551.387 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/OBJCOPY.BIN[DEBUG] Before eret, tf->regs[0] = 
5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1134 skipped=83 batches=10 stop=0 ms=156
/SRC/obj/arm/hello.elf
/SRC/obj/arm/HELLO.BIN.temp-stream-b81674
/SRC/obj/arm/HELLO.BIN.temp-stream-b81674 new=/SRC/obj/arm/HELLO.BIN
/SRC/obj/arm/HELLO.BIN
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
[SELFHOST] phase=s1-user-done rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] pha[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
se=s3-disk-start
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
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
sh: command not found: /TOOLS/RM.BIN
MAKE.BIN: *** [Makefile.hobbyos:435: disk] Error 127
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s3-disk-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s3-disk
[KERNEL] Process 1: SELFHOST.BIN exited unexpectedly or gracefully.
System halt from CPU 6.

