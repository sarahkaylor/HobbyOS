6.504 UEFI firmware (version 2025.11-3ubuntu7 built at 11:11:58 on Mar 31 2026)
[2J[001;001H[=3h[2J[001;001H[2J[001;001H[8;056;240t[2J[001;001HBdsDxe: loading Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
BdsDxe: starting Boot0002 "UEFI Non-Block Boot Device" from VenHw(837DCA9E-E874-4D82-B29A-23FE0E23D1E2,003E000A00000000)
[001;001H[001;001H[001;001Hl[001;002Hi[001;003Hn[001;004Hu[001;005Hx[001;006H:[001;007H [001;008HL[001;009Ho[001;010Ha[001;011Hd[001;012Hi[001;013Hn[001;014Hg[001;015H [001;016Hk[001;017He[001;018Hr[001;019Hn[001;020He[001;021Hl[001;022H [001;023H`[001;024Hb[001;025Ho[001;026Ho[001;027Ht[001;028H([001;029H)[001;030H:[001;031H/[001;032Hb[001;033Ho[001;034Ho[001;035Ht[001;036H/[001;037Hh[001;038Ho[001;039Hb[001;040Hb[001;041Hy[001;042Ho[001;043Hs[001;044H.[001;045Hb[001;046Hi[001;047Hn[001;048H`[001;049H.[001;050H.[001;051H.[001;052H[001;001H[002;001HBooting AArch64 OS...
MMU Initialized: Page Tables setup securely.
[GPUDIAG] gpu wait > 11 ms: ack=3 used=4 (park off)
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
start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU 41start_scheduler called on CPU 

3start_scheduler called on CPU [KERNEL] Core 26
 power-on requested.
start_scheduler called on CPU 5

[KERNEL] Core 67
 power-on requested.
start_scheduler called on CPU 7
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

7.505 /SRC/Makefile.hobbyos
/SRC/OBJS.RSP
[KERNEL] sys_fork: tf->regs[0] is now 4
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/ob/TOOLS/SH.BIN
j/arm/check.o
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

9.505 [DEBUG] Before eret, tf->regs[0] = 75956221 ELR = 0x000000100484D314

11.506 [IMGPRF] pid=5 done: pages=22997 mapped=22498 skipped=499 batches=180 stop=0 ms=4249
/SRC/src/kernel/main.c
/SRC/obj/arm/check-c5ab8167.o.tmp

12.506 /SRC/src/include/fat16.h
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

13.506 /SRC/src/include/virtio_net.h
/SRC/src/include/dhcp.h
src/kernel/main.c:50:39: warning: unused parameter 'tf' [-Wunused-parameter]
   50 | void irq_handler_c(struct trap_frame *tf) {
      |                                       ^

18.508 /SRC/obj/arm/check-c5ab8167.o.tmp new=/SRC/obj/arm/check.o
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
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP

19.508 /TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
RESDIR/include -c src/user/hello.c -o /SRC/obj/arm/hello.o
/TOOLS/SH.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/CLAN0x00000010000260F8G
.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS//TOOLS/SH.BIN
RESDIR/include -c src/user/sh.c -o /SRC/obj/arm/sh.o
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/ ELR = TOOL0x00000010000260F8S/C
LANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include -c src/user/sh_aux.c -o /SRC/obj/arm/sh_aux.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/user_include -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
RESDIR/include -c src/user/ls.c -o /SRC/obj/arm/ls.o
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/TOOLS/SH.BIN/user_incl/TOOLS/SH.BIN

ude -Isrc/user_include/graphics -Isrc/include -Isrc/libc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS//TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 9v2 loader: image mapped (lazy), bytes=195136 pages=
48[DEBUG] Before eret, tf->regs[0] =  first=9in elap= ELR = 760x000000100000DDF8ms

[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=9 start: pages=22997 batch=128
RESDIR/include -c src/user/cat.c -o /SRC/obj/arm/cat.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 480x0000001000000AB0 first=
in elap=27ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
v2 loader: image mapped (lazy), bytes=195136[DEBUG] Before eret, tf->regs[0] =  pages=48 first=92187in ELR =  elap=22ms
0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=195136 pages=[DEBUG] Before eret, tf->regs[0] = 9218748 ELR =  first=in elap=0x0000001000000AC837
ms

20.508 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8

21.509 /TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=13 ELR = 941923280x0000001000012720 pages=
22997 first=in elap=4ms
[IMGPRF] pid=11 start: pages=22997 batch=128
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=4ms
[IMGPRF] pid=12 start: pages=22997 batch=128
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

22.509 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=178ms
[IMGPRF] pid=13 start: pages=22997 batch=128
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=304ms
[IMGPRF] pid=16 start: pages=22997 batch=128

23.509 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

35.512 [IMGPRF] pid=9 done: pages=22997 mapped=22850 skipped=147 batches=180 stop=0 ms=16325

41.513 [IMGPRF] pid=13 done: pages=22997 mapped=22886 skipped=111 batches=180 stop=0 ms=19307

44.514 [IMGPRF] pid=12 done: pages=22997 mapped=22846 skipped=151 batches=180 stop=0 ms=23752

45.514 [IMGPRF] pid=11 done: pages=22997 mapped=22818 skipped=179 batches=180 stop=0 ms=24766
/SRC/src/user/sh.c

46.514 /SRC/src/user/sh_aux.c

47.515 [IMGPRF] pid=16 done: pages=22997 mapped=22808 skipped=189 batches=180 stop=0 ms=24875
/SRC/obj/arm/sh-9efc5ab8.o.tmp
/SRC/obj/arm/sh_aux-5ab81674.o.tmp
/SRC/src/user/hello.c
/SRC/obj/arm/hello-d2309efc.o.tmp
/SRC/src/user/cat.c
/SRC/obj/arm/cat-c5ab8167.o.tmp
/SRC/src/user/ls.c
/SRC/obj/arm/ls-d2309efc.o.tmp
/SRC/src/libc/include/string.h
/SRC/src/user_include/libc.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/user_include/libc.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/stddef.h

48.515 /SRC/src/user_include/libc.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/SRC/src/libc/include/locale.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/SRC/src/libc/include/ctype.h
[DEBUG] Before eret, tf->regs[0] = 1075503104 ELR = 0x00000010043627C0
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/SRC/src/user_include/libc.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/libc/include/string.h
/TOOLS/RESDIR/include/stddef.h
/SRC/src/user_include/malloc.h
/SRC/src/include/errno.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/SRC/src/user_include/resolv.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/libc/include/locale.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/libc/include/locale.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/locale.h
/SRC/src/libc/include/sys/time.h
/SRC/src/libc/include/ctype.h
/SRC/src/libc/include/stdlib.h
/SRC/src/libc/include/stdlib.h
/SRC/src/include/syscall.h
/TOOLS/RESDIR/include/__stddef_size_t.h

49.515 /SRC/src/libc/include/ctype.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/libc/include/ctype.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/SRC/src/libc/include/fcntl.h
/SRC/src/libc/include/unistd.h
/SRC/src/libc/include/stdlib.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/user_include/malloc.h
/SRC/src/user_include/libc.h
/SRC/src/include/errno.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/stdarg.h
/SRC/src/libc/include/stdio.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/libc/include/sys/types.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/time.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/libc/include/sys/time.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/SRC/src/libc/include/stdint.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h
/SRC/src/libc/include/locale.h
/TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/SRC/src/user_include/malloc.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/SRC/src/include/errno.h
/SRC/src/libc/include/ctype.h
/SRC/src/libc/include/stdlib.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/SRC/src/user_include/resolv.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/sys/time.h
/SRC/src/libc/include/stdio.h
/SRC/src/libc/include/sys/types.h
/SRC/src/libc/include/stdint.h
/SRC/src/libc/include/stdarg.h
/TOOLS/RESDIR/include/stdarg.h
/TOOLS/RESDIR/include/__stdarg_header_macro.h

50.516 /TOOLS/RESDIR/include/__stdarg___gnuc_va_list.h
/SRC/src/libc/include/unistd.h
/TOOLS/RESDIR/include/__stdarg_va_list.h
/TOOLS/RESDIR/include/__stdarg_va_arg.h
/TOOLS/RESDIR/include/__stdarg___va_copy.h
/TOOLS/RESDIR/include/__stdarg_va_copy.h
/SRC/src/user_include/malloc.h
/SRC/src/include/errno.h
/SRC/src/libc/include/unistd.h
/SRC/src/user_include/resolv.h
/SRC/src/user_include/malloc.h
/SRC/src/libc/include/time.h
/SRC/src/include/errno.h
/SRC/src/libc/include/sys/time.h
/SRC/src/user_include/resolv.h
/SRC/src/libc/include/time.h
/SRC/src/libc/include/sys/time.h
[DEBUG] Before eret, tf->regs[0] = 1074116872 ELR = 0x0000001002EEF9B0

51.516 /SRC/obj/arm/hello-d2309efc.o.tmp new=/SRC/obj/arm/hello.o
[KERNEL] Process 13: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

54.517 /SRC/obj/arm/cat-c5ab8167.o.tmp new=/SRC/obj/arm/cat.o
[KERNEL] Process 12: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

55.517 /SRC/obj/arm/ls-d2309efc.o.tmp new=/SRC/obj/arm/ls.o
[KERNEL] Process 16: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0
/SRC/obj/arm/sh_aux-5ab81674.o.tmp new=/SRC/obj/arm/sh_aux.o

56.517 [KERNEL] Process 11: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=user-compile rc=0

1974.920 /SRC/obj/arm/sh-9efc5ab8.o.tmp new=/SRC/obj/arm/sh.o

1977.921 [KERNEL] Process 9: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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

1978.921 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOS[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
T] phase=s1-user-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/sh.elf /SRC/obj/arm/sh.o /SRC/obj/arm/sh_aux.o /SRC/obj/arm/libc.a
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

1979.922 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

1980.922 [IMGPRF] pid=5 done: pages=13001 mapped=12680 skipped=321 batches=102 stop=0 ms=1911
/SRC/src/user/linker.ld
/SRC/obj/arm/sh.o
/SRC/obj/arm/sh_aux.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/sh.elf.tmp81674d2
/SRC/obj/arm/sh.elf.tmp309efc5

1982.922 /SRC/obj/arm/sh.elf

1984.923 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOL ELR = S0x00000010000260F8/
OBJCOPY.BIN -O binary /SRC/obj/arm/sh.elf /SRC/obj/arm/SH.BIN
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
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=0ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1134 skipped=83 batches=10 stop=0 ms=140
/SRC/obj/arm/sh.elf
/SRC/obj/arm/SH.BIN.temp-stream-9efc5a

1985.923 /SRC/obj/arm/SH.BIN.temp-stream-9efc5a new=/SRC/obj/arm/SH.BIN
/SRC/obj/arm/SH.BIN
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
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

1986.923 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

1987.923 [IMGPRF] pid=5 done: pages=13001 mapped=12650 skipped=351 batches=102 stop=0 ms=2024
/SRC/src/user/linker.ld
/SRC/obj/arm/ls.o
/SRC/obj/arm/libc.a

1988.923 /SRC/obj/arm/ls.elf.tmp09efc5a
/SRC/obj/arm/ls.elf.tmpb81674d
/SRC/obj/arm/ls.elf

1989.924 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/OBJCOPY.BIN -O binary /SRC/obj/arm/ls.elf /SRC/obj/arm/LS.BIN
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

1990.924 [IMGPRF] pid=5 done: pages=1217 mapped=1128 skipped=89 batches=10 stop=0 ms=165
/SRC/obj/arm/ls.elf
/SRC/obj/arm/LS.BIN.temp-stream-81674d
/SRC/obj/arm/LS.BIN.temp-stream-81674d new=/SRC/obj/arm/LS.BIN
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
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=1ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

1991.924 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

1992.924 [IMGPRF] pid=5 done: pages=13001 mapped=12659 skipped=342 batches=102 stop=0 ms=1986
/SRC/src/user/linker.ld
/SRC/obj/arm/cat.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/cat.elf.tmp9efc5ab

1993.924 /SRC/obj/arm/cat.elf.tmp81674d2
/SRC/obj/arm/cat.elf

1994.925 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
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
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/OBJCOPY.BIN
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90
[IMGPRF] pid=5 done: pages=1217 mapped=1153 skipped=64 batches=10 stop=0 ms=127
/SRC/obj/arm/cat.elf

1995.925 /SRC/obj/arm/CAT.BIN.temp-stream-74d230
/SRC/obj/arm/CAT.BIN.temp-stream-74d230 new=/SRC/obj/arm/CAT.BIN
/SRC/obj/arm/CAT.BIN
[KERNEL] Process 5: /TOOLS/OBJCOPY.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/LLD.BIN -flavor gnu -T src/user/linker.ld -o /SRC/obj/arm/hello.elf /SRC/obj/arm/hello.o /SRC/obj/arm/libc.a
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/LLD.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
/TOOLS/LLD.BIN
v2 loader: image mapped (lazy), bytes=53250536 pages=13001 first=in elap=0ms
[IMGPRF] pid=5 start: pages=13001 batch=128
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

1996.925 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

1997.925 [IMGPRF] pid=5 done: pages=13001 mapped=12690 skipped=311 batches=102 stop=0 ms=1931
/SRC/src/user/linker.ld
/SRC/obj/arm/hello.o
/SRC/obj/arm/libc.a
/SRC/obj/arm/hello.elf.tmp81674d2

1998.926 /SRC/obj/arm/hello.elf.tmp309efc5
/SRC/obj/arm/hello.elf

1999.926 [KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
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
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/TOOLS/OBJCOPY.BIN
v2 loader: image mapped (lazy), bytes=4982672 pages=1217 first=in elap=1ms
[IMGPRF] pid=5 start: pages=1217 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100037A9C0
[DEBUG] Before eret, tf->regs[0] = 3873611 ELR = 0x0000001000382E90

2000.926 [IMGPRF] pid=5 done: pages=1217 mapped=1148 skipped=69 batches=10 stop=0 ms=156
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
/SRC/SELFJ8.SH
/TOOLS/MAKE.BIN
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
/TOOLS/MAKE.BIN
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
/SRC/Makefile.hobbyos
/SRC/OBJS.RSP
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4TOOLS/CLANG.BIN -O2 -W ELR = all -Wextra -g -Isrc/0x00000010000260F8include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgen
eral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/dhcp.c -o /SRC/obj/arm/dhcp.o
/TOOLS/SH.BIN
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = /5TOO ELR = LS/CLANG.BIN -O2 -Wall0x00000010000260F8
 -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fat16.c -o /SRC/obj/arm/fat16.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136v2 loader: image mapped (lazy), bytes= pages=19513648 pages= first=48in first= elap=in1 elap=ms
11ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2001.926 [KERNEL] sys_fork: tf->regs[0] is now /TOOLS/CL38ANG.BIN -O
2 -Wall -Wextra -g -Isrc/i[DEBUG] Before eret, tf->regs[0] = 38n ELR = clude --targ0x00000010000260F8
et=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/f/TOOLS/CLANG.BINa
t16_test.c -o /SRC/obj/arm/fat16_test.o
[KERNEL] sys_fork: tf->regs[0] is now 39
[DEBUG] Before eret, tf->regs[0] = 39/TOOLS/CLANG.BIN - ELR = O2 -Wall -Wextra -0x00000010000260F8g /TOOLS/SH.BIN
-
Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -is/TOOLS/SH.BINy
stem /TOOLS/RESDIR/include -c src/kernel/frame.c -o /SRC/o[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
bj/arm/frame.o
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/frame_test.c -o /SRC/obj/arm/frame_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 4241
 ELR = [DEBUG] Before eret, tf->regs[0] = 420x0000001000012720 ELR = 
0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 42 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=57ms
/TOOLS/CLANG.BIN
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=29ms
[IMGPRF] pid=42 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0

2002.927 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 46
[DEBUG] Before eret, tf->regs[0] = 46 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 46 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 47
[DEBUG] Before eret, tf->regs[0] = 47 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fs.c -o /SRC/obj/arm/fs.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=45 start: pages=22997 batch=128
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=73ms
[IMGPRF] pid=46 start: pages=22997 batch=128

2003.927 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=122ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2004.927 /TOOLS/CLANG.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/fs_test.c -o /SRC/obj/arm/fs_test.o
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/TOOLS/SH.BIN
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
v2 loader: image mapped (lazy), bytes=195136[DEBUG] Before eret, tf->regs[0] =  pages=92187 ELR = 48 first=in elap=50x0000001000000AB0ms

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2005.927 [KERNEL] sys_fork: tf->regs[0] is now 51
[DEBUG] Before eret, tf->regs[0] = 51 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 51 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=53ms
[IMGPRF] pid=51 start: pages=22997 batch=128

2006.927 [KERNEL] sys_fork: tf->regs[0] is now 53
[DEBUG] Before eret, tf->regs[0] = 53 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/iommu_vtd.c -o /S/TOOLS/SH.BIN
/TOOLS/CLANG.BIN

2007.928 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=77ms
RC/obj/arm/iommu_vtd.o
[KERNEL] sys_fork: tf->regs[0] is now 54
[DEBUG] Before eret, tf->regs[0] = 54 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=45ms
[IMGPRF] pid=54 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 54 ELR = 0x0000001000012720

2008.928 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2009.928 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020/TOOLS/CLANG.BIN

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2010.928 [KERNEL] sys_fork: tf->regs[0] is now 56
[DEBUG] Before eret, tf->regs[0] = 56 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 56 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=63ms
[IMGPRF] pid=56 start: pages=22997 batch=128

2012.929 /TOOLS/CLANG.BIN

2013.929 [KERNEL] sys_fork: tf->regs[0] is now 58
[DEBUG] Before eret, tf->regs[0] = 58 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=4ms
[IMGPRF] pid=58 start: pages=22997 batch=128

2021.930 [IMGPRF] pid=51 done: pages=22997 mapped=22975 skipped=22 batches=180 stop=0 ms=15794
[DEBUG] Before eret, tf->regs[0] = 58 ELR = 0x0000001000012720

2022.931 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[IMGPRF] pid=46 done: pages=22997 mapped=22959 skipped=38 batches=180 stop=0 ms=20036

2023.931 [IMGPRF] pid=42 done: pages=22997 mapped=22945 skipped=52 batches=180 stop=0 ms=21602
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2024.931 [IMGPRF] pid=54 done: pages=22997 mapped=22975 skipped=22 batches=180 stop=0 ms=16654

2025.931 [IMGPRF] pid=45 done: pages=22997 mapped=22946 skipped=51 batches=180 stop=0 ms=22452
[IMGPRF] pid=41 done: pages=22997 mapped=22925 skipped=72 batches=180 stop=0 ms=23698

2026.932 [IMGPRF] pid=56 done: pages=22997 mapped=22960 skipped=37 batches=180 stop=0 ms=15942

2027.932 [IMGPRF] pid=58 done: pages=22997 mapped=22932 skipped=65 batches=180 stop=0 ms=14057
/SRC/src/kernel/fat16_test.c
/SRC/src/kernel/frame_test.c
/SRC/src/kernel/frame.c
/SRC/obj/arm/fat16_test-1674d230.o.tmp
/SRC/obj/arm/frame-674d2309.o.tmp
/SRC/src/kernel/fat16.c
/SRC/obj/arm/frame_test-c5ab8167.o.tmp
/SRC/obj/arm/fat16-09efc5ab.o.tmp
/SRC/src/kernel/dhcp.c
/SRC/src/kernel/fs.c
/SRC/obj/arm/dhcp-1674d230.o.tmp

2028.932 /SRC/obj/arm/fs-ab81674d.o.tmp
/SRC/src/kernel/fs_test.c
/SRC/obj/arm/fs_test-309efc5a.o.tmp
/SRC/src/kernel/iommu_vtd.c
/SRC/obj/arm/iommu_vtd-4d2309ef.o.tmp
/SRC/src/include/frame.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fs.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/dhcp.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/TOOLS/RESDIR/include/stdint.h
[DEBUG] Before eret, tf->regs[0] = 1074116792 ELR = 0x0000001002EEF9B0
/SRC/src/include/net.h
/TOOLS/RESDIR/include/stddef.h
/SRC/src/include/lock.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h

2029.932 /TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/include/fat16.h
/SRC/src/include/arch/cpu.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/include/process.h
/SRC/src/include/lock.h
/SRC/obj/arm/fat16_test-1674d230.o.tmp new=/SRC/obj/arm/fat16_test.o
/SRC/obj/arm/frame_test-c5ab8167.o.tmp new=/SRC/obj/arm/frame_test.o
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/obj/arm/fs_test-309efc5a.o.tmp new=/SRC/obj/arm/fs_test.o
/SRC/src/include/arch/cpu.h
[KERNEL] Process 45: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/src/include/process.h
/SRC/src/include/trap.h
[KERNEL] Process 51: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/src/include/arch/trap.h
[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DC00
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] Process 56: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 50: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218743 ELR =  ELR = 0x0000001000000AC80x00000010000260F8

/TOOLS/SH.BIN
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/ipc_proto.c -o /SRC/obj/arm/ipc_proto.o
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/SRC/src/include/nfs.h
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/locks_test.c -o /[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
SRC/obj/arm/locks_test.o
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/nfs.h
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=20ms
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms

2030.932 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/src/include/ipc_proto.h
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=/SRC/obj/arm/iommu_vtd-4d2309ef.o.tmp94192328 new= pages=/SRC/obj/arm/iommu_vtd.o22997
 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=3ms
[IMGPRF] pid=45 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[KERNEL] Process 58: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 53: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = /SRC/src/include/ipc_proto.h50[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 ELR = 0x00000010000260F8

[SELFHOST] phase=kernel-compile rc=0
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/TOOLS/SH.BIN

2031.933 /TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/main.c -o /SRC/ob/TOOLS/SH.BIN
/SRC/src/include/virtio_blk.h/SRC/src/include/virtio_net.h

v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
j/arm/main.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2032.933 [KERNEL] Process 50: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/math_test.c -o /SRC/obj/arm/math_test.o
/TOOLS/CLANG.BIN
/TOOLS/SH.BIN
/TOOLS/RESDIR/include/stddef.h
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
/SRC/src/include/timer.h
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 51
[DEBUG] Before eret, tf->regs[0] = 51 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 51 ELR = 0x0000001000012720

2033.933 /TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 52
[DEBUG] Before eret, tf->regs[0] = 52 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=90ms
[IMGPRF] pid=51 start: pages=22997 batch=128
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=2ms
[IMGPRF] pid=52 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 52 ELR = 0x0000001000012720
/SRC/src/include/pipe.h

2034.933 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/SRC/src/include/lock.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2036.933 /SRC/src/include/unix.h

2037.934 /SRC/src/include/arch/cpu.h

2039.934 /TOOLS/RESDIR/include/__stddef_ptrdiff_t.h

2040.934 /SRC/src/include/net.h

2042.934 src/kernel/dhcp.c:42:17: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
   42 |   for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
      |                ~^~~~~~~~~~~~

2044.935 src/kernel/dhcp.c:82:17: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
   82 |   for(int i=0; i<sizeof(buf); i++) buf[i] = 0;
      |          /TOOLS/RESDIR/include/stddef.h
      ~^~~~~~~~~~~~
[IMGPRF] pid=44 done: pages=22997 mapped=22899 skipped=98 batches=180 stop=0 ms=14727

2045.935 /TOOLS/RESDIR/include/__stddef_size_t.h
src/kernel/dhcp.c:135:19: warning: unused variable 'udp' [-Wunused-variable]
  135 |   struct udp_hdr* udp = (struct udp_hdr*)(packet + sizeof(struct ipv4_hdr));
      |                   ^~~

2048.936 /TOOLS/RESDIR/include/__stddef_header_macro.h
[IMGPRF] pid=52 done: pages=22997 mapped=22900 skipped=97 batches=180 stop=0 ms=15323
[IMGPRF] pid=45 done: pages=22997 mapped=22862 skipped=135 batches=180 stop=0 ms=18460
/TOOLS/RESDIR/include/__stddef_wchar_t.h

2049.936 /TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/TOOLS/RESDIR/include/__stddef_null.h
[IMGPRF] pid=51 done: pages=22997 mapped=22866 skipped=131 batches=180 stop=0 ms=16587
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h

2050.936 /TOOLS/RESDIR/include/__stddef_size_t.h
/SRC/src/include/errno.h
/SRC/src/kernel/ipc_proto.c
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/src/kernel/math_test.c
/SRC/obj/arm/ipc_proto-2309efc5.o.tmp
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/obj/arm/math_test-d2309efc.o.tmp
/SRC/src/kernel/locks_test.c
/SRC/src/include/vfs.h
/SRC/obj/arm/locks_test-1674d230.o.tmp
/SRC/src/include/errno.h
/SRC/src/include/timer.h
/SRC/src/include/vm.h
/SRC/src/kernel/main.c
/SRC/obj/arm/main-ab81674d.o.tmp
/SRC/src/include/ipc_proto.h
/TOOLS/RESDIR/include/stdint.h

2051.936 /SRC/src/include/errno.h
/SRC/src/include/fat16.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/fs.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/obj/arm/math_test-d2309efc.o.tmp new=/SRC/obj/arm/math_test.o
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/obj/arm/locks_test-1674d230.o.tmp new=/SRC/obj/arm/locks_test.o
[KERNEL] Process 52: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 50: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/src/include/nfs.h
[KERNEL] Process 45: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.

/SRC/src/include/ipc_proto.h
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = /45T ELR = O0x00000010000260F8O
LS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/mmu_test.c -o /SR/TOOLS/SH.BIN
C/obj/arm/mmu_test.o

2052.937 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net.c -o /SRC/obj[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/arm/net.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=29ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/src/include/gic.h
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=2ms
[IMGPRF] pid=50 start: pages=22997 batch=128

2053.937 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2054.937 /SRC/src/include/mmu.h
/SRC/src/include/arch/mmu.h

2056.938 src/kernel/fat16.c:2000:25: warning: comparison of integers of different signs: 'int' and 'uint32_t' (aka 'unsigned int') [-Wsign-compare]
 2000 |       if ((int)old_size > f->fat16.cursor) f->fat16.cursor = (int)old_size;
      |           ~~~~~~~~~~~~~ ^ ~~~~~~~~~~~~~~~
/SRC/src/include/program_loader.h

2057.938 /SRC/src/include/timer.h

2059.938 /SRC/src/include/virtio_blk.h

2060.938 /TOOLS/RESDIR/include/stddef.h

2062.939 [IMGPRF] pid=48 done: pages=22997 mapped=22785 skipped=212 batches=180 stop=0 ms=10081
/TOOLS/RESDIR/include/__stddef_header_macro.h

2063.939 /TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
[IMGPRF] pid=50 done: pages=22997 mapped=22750 skipped=247 batches=180 stop=0 ms=10693
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/kernel/mmu_test.c
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/virtio_gpu.h
/SRC/obj/arm/mmu_test-2309efc5.o.tmp
/SRC/src/include/virtio_input.h
/SRC/src/include/net.h
/SRC/src/include/virtio_net.h
/SRC/src/include/dhcp.h

2064.939 src/kernel/main.c:50:39: warning: unused parameter 'tf' [-Wunused-parameter]
   50 | void irq_handler_c(struct trap_frame *tf) {
      |                                       ^
/SRC/src/kernel/net.c
/SRC/obj/arm/net-4d2309ef.o.tmp
/SRC/obj/arm/dhcp-1674d230.o.tmp new=/SRC/obj/arm/dhcp.o
3 warnings generated.
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net_rdma.c -o /SR/TOOLS/SH.BIN
C/obj/arm/net_rdma.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2065.940 /SRC/src/include/net.h

2066.940 /SRC/obj/arm/mmu_test-2309efc5.o.tmp new=/SRC/obj/arm/mmu_test.o
/TOOLS/RESDIR/include/stdint.h

2067.940 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=61ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/RESDIR/include/stddef.h
[SEL[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
FHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/incl/TOOLS/SH.BINu
de -c src/kernel/net_rdma_test.c -o /SRC/obj/arm/net_rdma_test.o

2068.940 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=2ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2069.940 /TOOLS/RESDIR/include/__stddef_header_macro.h

2071.941 [IMGPRF] pid=41 done: pages=22997 mapped=22796 skipped=201 batches=180 stop=0 ms=7180
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h

2072.941 /SRC/obj/arm/ipc_proto-2309efc5.o.tmp new=/SRC/obj/arm/ipc_proto.o
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020

2073.941 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/RESDIR/include/__stddef_size_t.h
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8/
TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/net_test.c -o /SRC/obj/arm/net_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/RESDIR/include/__stddef_wchar_t.h

2074.942 v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=2ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2075.942 /TOOLS/RESDIR/include/__stddef_null.h

2077.942 /TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/kernel/net_rdma.c
[IMGPRF] pid=48 done: pages=22997 mapped=22808 skipped=189 batches=180 stop=0 ms=9136
/SRC/obj/arm/net_rdma-2309efc5.o.tmp

2078.943 /TOOLS/RESDIR/include/__stddef_offsetof.h

2079.943 /SRC/src/include/virtio_net.h
/SRC/src/include/lock.h

2080.943 /SRC/src/kernel/net_rdma_test.c
/SRC/src/include/arch/cpu.h
/SRC/src/include/net_rdma.h

2081.943 /SRC/src/include/timer.h
/SRC/src/include/process.h
/SRC/obj/arm/net_rdma_test-74d2309e.o.tmp

2082.943 /SRC/obj/arm/main-ab81674d.o.tmp new=/SRC/obj/arm/main.o
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/trap.h
1 warning generated.
/TOOLS/RESDIR/include/stddef.h
/SRC/src/include/arch/trap.h
[KERNEL] Process 51: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN

2083.944 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[IMGPRF] pid=44 done: pages=22997 mapped=22758 skipped=239 batches=180 stop=0 ms=8977
/TOOLS/RESDIR/include/__stddef_header_macro.h
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs.c -o /SRC/obj[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/arm/nfs.o
/TOOLS/SH.BIN
/SRC/src/include/net_rdma.h
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/include/errno.h
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
/TOOLS/RESDIR/include/__stddef_offsetof.h
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=49 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2084.944 /SRC/obj/arm/net_rdma_test-74d2309e.o.tmp new=/SRC/obj/arm/net_rdma_test.o

2085.944 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=18ms
src/kernel/net.c:519:18: warning: variable 'stored' set but not used [-Wunused-but-set-variable]
  519 |         uint32_t stored = 0;[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

      |                  ^
/SRC/src/kernel/net_test.c
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN

2086.944 /TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs_proto.c -o /SRC/obj/arm/nfs_proto.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
src/kernel/net.c:782:21: warning: comparison of integers of different signs: 'int' and 'unsigned long' [-Wsign-compare]
  782 |   for (int i = 0; i < sizeof(struct pseudo_hdr)/2; i++) {
      [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
|                   ~ ^ ~~~~~~~~~~~~~~~~~~~~~~~~~~~
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720

2087.944 /SRC/obj/arm/net_test-09efc5ab.o.tmp
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=3ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2088.945 src/kernel/net.c:624:39: warning: unused function 'safe_print_int' [-Wunused-function]
  624 | __attribute__((noinline)) static void safe_print_int(int val) {
      |                                       ^~~~~~~~~~~~~~

2090.945 [IMGPRF] pid=49 done: pages=22997 mapped=22799 skipped=198 batches=180 stop=0 ms=7469

2091.946 /SRC/obj/arm/net_test-09efc5ab.o.tmp new=/SRC/obj/arm/net_test.o

2092.946 [KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=3ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/nfs_test.c -o /SRC/obj/arm/nfs_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=17ms

2093.946 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/SRC/src/kernel/nfs.c
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=18ms
[IMGPRF] pid=44 start: pages=22997 batch=128

2094.946 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2095.947 [IMGPRF] pid=48 done: pages=22997 mapped=22796 skipped=201 batches=180 stop=0 ms=8641
/SRC/obj/arm/nfs-9efc5ab8.o.tmp

2097.947 /SRC/src/include/nfs.h

2098.947 /TOOLS/RESDIR/include/stdint.h
/SRC/src/kernel/nfs_proto.c
/SRC/obj/arm/net_rdma-2309efc5.o.tmp new=/SRC/obj/arm/net_rdma.o

2099.948 /SRC/obj/arm/nfs_proto-309efc5a.o.tmp
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=4ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/src/include/net.h

2100.948 [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/pipe.c -o /SRC/obj/arm/pipe.o
/TOOLS/RESDIR/include/stddef.h
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=20ms
[IMGPRF] pid=41 start: pages=22997 batch=128

2101.948 [IMGPRF] pid=44 done: pages=22997 mapped=22768 skipped=229 batches=180 stop=0 ms=7715
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/SRC/src/include/nfs.h
/TOOLS/RESDIR/include/__stddef_header_macro.h

2102.948 /TOOLS/RESDIR/include/stdint.h

2103.949 /TOOLS/RESDIR/include/__stddef_ptrdiff_t.h

2104.949 /TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/SRC/src/kernel/nfs_test.c
/TOOLS/RESDIR/include/__stddef_null.h

2105.949 /SRC/obj/arm/nfs_test-309efc5a.o.tmp
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/RESDIR/include/__stddef_offsetof.h

2106.949 [IMGPRF] pid=41 done: pages=22997 mapped=22779 skipped=218 batches=180 stop=0 ms=5487
/SRC/src/kernel/pipe.c
/SRC/obj/arm/pipe-b81674d2.o.tmp
src/kernel/nfs.c:157:12: warning: '/*' within block comment [-Wcomment]
  157 |  * is *body/*body_len, valid until the next rpc_call (static buffer). */
      |            ^

2107.950 /SRC/obj/arm/nfs_test-309efc5a.o.tmp new=/SRC/obj/arm/nfs_test.o
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = /43T ELR = OO0x00000010000260F8L
S/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/pipe_test.c -o /SRC/obj/arm/pipe_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2108.950 /SRC/src/include/pipe.h
/TOOLS/RESDIR/include/stdint.h

2109.950 /SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

2110.950 /SRC/src/include/process.h

2111.950 /SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
[IMGPRF] pid=44 done: pages=22997 mapped=22781 skipped=216 batches=180 stop=0 ms=4490
/SRC/src/include/errno.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h

2112.951 /SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/obj/arm/frame-674d2309.o.tmp new=/SRC/obj/arm/frame.o
/SRC/src/kernel/pipe_test.c
/SRC/obj/arm/pipe_test-309efc5a.o.tmp
[KERNEL] Process 46: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 39: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 39
[DEBUG] Before eret, tf->regs[0] = 39 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 39: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/process.c -o /SRC[KERNEL] sys_fork: tf->regs[0] is now 39
[DEBUG] Before eret, tf->regs[0] = 39 ELR = 0x00000010000260F8
/obj/arm/process.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

2113.951 [KERNEL] sys_fork: tf->regs[0] is now 46
[DEBUG] Before eret, tf->regs[0] = 46 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 46 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=46 start: pages=22997 batch=128
src/kernel/pipe.c:139:7: warning: unused variable 'i' [-Wunused-variable]
  139 |   int i = 0;
      |       ^
src/kernel/pipe.c:136:68: warning: unused parameter 'tf' [-Wunused-parameter]
  136 | int pipe_read(struct pipe *p, void *buf, int n, struct trap_frame *tf) {
      |                                                             [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
       ^
src/kernel/pipe.c:216:7: warning: unused variable 'i' [-Wunused-variable]
  216 |   int i = 0;
      |       ^

2114.951 /SRC/obj/arm/pipe_test-309efc5a.o.tmp new=/SRC/obj/arm/pipe_test.o

2115.951 [KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=77ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/process_test.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/process_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2116.952 /TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=18ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2120.952 [IMGPRF] pid=46 done: pages=22997 mapped=22719 skipped=278 batches=180 stop=0 ms=7547

2122.953 /SRC/src/kernel/process.c
/SRC/obj/arm/process-1674d230.o.tmp
[IMGPRF] pid=44 done: pages=22997 mapped=22803 skipped=194 batches=180 stop=0 ms=6306

2123.953 /SRC/src/kernel/process_test.c
/SRC/obj/arm/process_test-b81674d2.o.tmp
/SRC/src/include/process.h
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

2124.953 /SRC/obj/arm/process_test-b81674d2.o.tmp new=/SRC/obj/arm/process_test.o
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/program_loader.c [KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
-o /SRC/obj/arm/program_loader.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2126.954 src/kernel/process.c:1169:42: warning: unused parameter 'is_yield' [-Wunused-parameter]
 1169 | void schedule(struct trap_frame *tf, int is_yield) {
      |                                          ^

2128.954 [IMGPRF] pid=44 done: pages=22997 mapped=22736 skipped=261 batches=180 stop=0 ms=3654
/SRC/src/kernel/program_loader.c
/SRC/obj/arm/program_loader-674d2309.o.tmp

2129.954 /SRC/src/include/program_loader.h
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

2131.955 src/kernel/process.c:2997:50: warning: unused parameter 'prot' [-Wunused-parameter]
 2997 | int64_t sys_mmap(int64_t addr, uint64_t len, int prot, int flags) {
      |                                                  ^

2133.955 /SRC/obj/arm/pipe-b81674d2.o.tmp new=/SRC/obj/arm/pipe.o
3 warnings generated.
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/program_loader_te[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
st.c -o /SRC/obj/arm/program_loader_test.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2137.956 [IMGPRF] pid=41 done: pages=22997 mapped=22768 skipped=229 batches=180 stop=0 ms=3803
/SRC/src/kernel/program_loader_test.c

2138.956 /SRC/obj/arm/program_loader_test-09efc5ab.o.tmp

2139.957 /SRC/obj/arm/program_loader_test-09efc5ab.o.tmp new=/SRC/obj/arm/program_loader_test.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/test_fw_cfg.c -o /TOOLS/SH.BIN
/SRC/obj/arm/test_fw_cfg.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2143.958 [IMGPRF] pid=41 done: pages=22997 mapped=22781 skipped=216 batches=180 stop=0 ms=3638
/SRC/src/kernel/test_fw_cfg.c
/SRC/obj/arm/test_fw_cfg-d2309efc.o.tmp

2144.958 /TOOLS/RESDIR/include/stdint.h

2146.958 /SRC/obj/arm/test_fw_cfg-d2309efc.o.tmp new=/SRC/obj/arm/test_fw_cfg.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/test_smp.c -o /SR[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
C/obj/arm/test_smp.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2150.959 [IMGPRF] pid=41 done: pages=22997 mapped=22777 skipped=220 batches=180 stop=0 ms=3644
/SRC/src/kernel/test_smp.c
/SRC/obj/arm/test_smp-9efc5ab8.o.tmp

2151.960 /SRC/obj/arm/test_smp-9efc5ab8.o.tmp new=/SRC/obj/arm/test_smp.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/time.c -o /SRC/ob[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
j/arm/time.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

2152.960 [KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2155.960 [IMGPRF] pid=41 done: pages=22997 mapped=22760 skipped=237 batches=180 stop=0 ms=3926

2156.960 /SRC/src/kernel/time.c
/SRC/obj/arm/time-74d2309e.o.tmp
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h

2157.961 /SRC/src/include/arch/cpu.h
/SRC/src/include/timer.h
[DEBUG] Before eret, tf->regs[0] = 1074116888 ELR = 0x00000010043C0B64

2162.962 /SRC/obj/arm/time-74d2309e.o.tmp new=/SRC/obj/arm/time.o

2163.962 [KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/time_test.c -o /S[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
RC/obj/arm/time_test.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2167.963 [IMGPRF] pid=41 done: pages=22997 mapped=22780 skipped=217 batches=180 stop=0 ms=3863
/SRC/src/kernel/time_test.c
/SRC/obj/arm/time_test-74d2309e.o.tmp

2168.963 /SRC/obj/arm/time_test-74d2309e.o.tmp new=/SRC/obj/arm/time_test.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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

2169.963 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/timer_test.c -o //TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
SRC/obj/arm/timer_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2173.964 [IMGPRF] pid=41 done: pages=22997 mapped=22750 skipped=247 batches=180 stop=0 ms=4061
/SRC/src/kernel/timer_test.c
/SRC/obj/arm/timer_test-c5ab8167.o.tmp

2174.964 /SRC/obj/arm/timer_test-c5ab8167.o.tmp new=/SRC/obj/arm/timer_test.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/trap_test.c -o /S/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
RC/obj/arm/trap_test.o
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN

2175.965 [KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2178.965 [IMGPRF] pid=41 done: pages=22997 mapped=22796 skipped=201 batches=180 stop=0 ms=3777

2179.966 /SRC/src/kernel/trap_test.c
/SRC/obj/arm/trap_test-1674d230.o.tmp

2180.966 /SRC/obj/arm/trap_test-1674d230.o.tmp new=/SRC/obj/arm/trap_test.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unit_test.c -o /S[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
RC/obj/arm/unit_test.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2184.967 [IMGPRF] pid=41 done: pages=22997 mapped=22757 skipped=240 batches=180 stop=0 ms=4113

2185.967 /SRC/src/kernel/unit_test.c
/SRC/obj/arm/unit_test-1674d230.o.tmp

2186.967 /SRC/obj/arm/unit_test-1674d230.o.tmp new=/SRC/obj/arm/unit_test.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unix.c -o /SRC/obj/arm/unix.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2190.968 [IMGPRF] pid=41 done: pages=22997 mapped=22755 skipped=242 batches=180 stop=0 ms=3860
/SRC/src/kernel/unix.c
/SRC/obj/arm/unix-309efc5a.o.tmp

2191.969 /SRC/src/include/unix.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/ipc_proto.h
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h

2192.969 /SRC/src/include/errno.h

2230.978 /SRC/obj/arm/nfs-9efc5ab8.o.tmp new=/SRC/obj/arm/nfs.o
1 warning generated.
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.

2231.978 [KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/unix_test.c -o /S/TOOLS/SH.BIN
RC/obj/arm/unix_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=49 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2235.979 [IMGPRF] pid=49 done: pages=22997 mapped=22773 skipped=224 batches=180 stop=0 ms=3831
/SRC/src/kernel/unix_test.c
/SRC/obj/arm/unix_test-5ab81674.o.tmp

2236.979 /SRC/obj/arm/unix_test-5ab81674.o.tmp new=/SRC/obj/arm/unix_test.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vfs.c -o /SRC/obj/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/arm/vfs.o
/TOOLS/CLANG.BIN

2237.980 [KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=49 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2241.980 [IMGPRF] pid=49 done: pages=22997 mapped=22757 skipped=240 batches=180 stop=0 ms=4025
/SRC/src/kernel/vfs.c
/SRC/obj/arm/vfs-309efc5a.o.tmp

2242.981 /SRC/src/include/vfs.h
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
/SRC/obj/arm/program_loader-674d2309.o.tmp new=/SRC/obj/arm/program_loader.o

2243.981 [KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_blk.c -o /[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
SRC/obj/arm/virtio_blk.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2247.982 [IMGPRF] pid=44 done: pages=22997 mapped=22738 skipped=259 batches=180 stop=0 ms=3779
/SRC/src/kernel/virtio_blk.c
/SRC/obj/arm/virtio_blk-2309efc5.o.tmp

2248.982 /SRC/src/include/virtio_blk.h
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

2249.982 src/kernel/virtio_blk.c:655:12: warning: unused variable 'desc_idx' [-Wunused-variable]
  655 |   uint16_t desc_idx = 0;
      |            ^~~~~~~~

2258.984 /SRC/obj/arm/virtio_blk-2309efc5.o.tmp new=/SRC/obj/arm/virtio_blk.o
1 warning generated.
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_blk_test.c -o /SRC/obj/arm/virtio_blk_test.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2262.985 [IMGPRF] pid=44 done: pages=22997 mapped=22775 skipped=222 batches=180 stop=0 ms=4104

2263.986 /SRC/src/kernel/virtio_blk_test.c
/SRC/obj/arm/virtio_blk_test-b81674d2.o.tmp

2264.986 /SRC/obj/arm/virtio_blk_test-b81674d2.o.tmp new=/SRC/obj/arm/virtio_blk_test.o
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu.c -o /SRC/obj/arm/virtio_gpu.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2268.987 [IMGPRF] pid=44 done: pages=22997 mapped=22775 skipped=222 batches=180 stop=0 ms=3552
/SRC/src/kernel/virtio_gpu.c
/SRC/obj/arm/virtio_gpu-c5ab8167.o.tmp

2269.988 /SRC/src/include/virtio_gpu.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/display_mode.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

2274.989 /SRC/obj/arm/vfs-309efc5a.o.tmp new=/SRC/obj/arm/vfs.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

2275.989 [KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu_test.c/TOOLS/CLANG.BIN
 -o /SRC/obj/arm/virtio_gpu_test.o
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=49 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2278.990 [IMGPRF] pid=49 done: pages=22997 mapped=22779 skipped=218 batches=180 stop=0 ms=3099
/SRC/src/kernel/virtio_gpu_test.c
/SRC/obj/arm/virtio_gpu_test-309efc5a.o.tmp

2279.990 /SRC/obj/arm/virtio_gpu_test-309efc5a.o.tmp new=/SRC/obj/arm/virtio_gpu_test.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_gpu_x64.c [KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
-o /SRC/obj/arm/virtio_gpu_x64.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=49 start: pages=22997 batch=128

2280.990 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2283.991 [IMGPRF] pid=49 done: pages=22997 mapped=22774 skipped=223 batches=180 stop=0 ms=3380
/SRC/src/kernel/virtio_gpu_x64.c
/SRC/obj/arm/virtio_gpu_x64-efc5ab81.o.tmp

2285.992 /SRC/obj/arm/virtio_gpu_x64-efc5ab81.o.tmp new=/SRC/obj/arm/virtio_gpu_x64.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_input.c -o /SRC/obj/arm/virtio_input.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=49 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2289.993 [IMGPRF] pid=49 done: pages=22997 mapped=22766 skipped=231 batches=180 stop=0 ms=3876
/SRC/src/kernel/virtio_input.c
/SRC/obj/arm/virtio_input-efc5ab81.o.tmp

2290.993 /SRC/src/include/virtio_input.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h

2292.994 /SRC/obj/arm/unix-309efc5a.o.tmp new=/SRC/obj/arm/unix.o

2293.994 [KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_input_test/TOOLS/SH.BIN
.c -o /SRC/obj/arm/virtio_input_test.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720/TOOLS/CLANG.BIN

v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2298.995 [IMGPRF] pid=41 done: pages=22997 mapped=22774 skipped=223 batches=180 stop=0 ms=4997

2301.996 /SRC/src/kernel/virtio_input_test.c
/SRC/obj/arm/virtio_input_test-efc5ab81.o.tmp

2302.996 /SRC/obj/arm/virtio_input_test-efc5ab81.o.tmp new=/SRC/obj/arm/virtio_input_test.o

2303.996 [KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

2304.997 [KERNEL] sys_fork: tf->regs[0] is now 4
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_net.c -o //TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
SRC/obj/arm/virtio_net.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
/SRC/obj/arm/virtio_input-efc5ab81.o.tmp new=/SRC/obj/arm/virtio_input.o

2305.997 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2306.997 [KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2307.998 [KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/virtio_net_test.cv2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
 -o /SRC/obj/arm/virtio_net_test.o
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2308.998 /TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=2ms
[IMGPRF] pid=49 start: pages=22997 batch=128

2309.998 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2316.000 [IMGPRF] pid=41 done: pages=22997 mapped=22775 skipped=222 batches=180 stop=0 ms=10453

2319.000 /SRC/src/kernel/virtio_net.c
/SRC/obj/arm/virtio_net-309efc5a.o.tmp
[IMGPRF] pid=49 done: pages=22997 mapped=22783 skipped=214 batches=180 stop=0 ms=10332

2320.001 /SRC/src/include/virtio_net.h

2321.001 /TOOLS/RESDIR/include/stdint.h
/SRC/src/kernel/virtio_net_test.c
/TOOLS/RESDIR/include/stddef.h
/TOOLS/RESDIR/include/__stddef_header_macro.h
/SRC/obj/arm/virtio_net_test-09efc5ab.o.tmp
/TOOLS/RESDIR/include/__stddef_ptrdiff_t.h

2322.001 /TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/__stddef_wchar_t.h
/TOOLS/RESDIR/include/__stddef_null.h
/SRC/obj/arm/virtio_net_test-09efc5ab.o.tmp new=/SRC/obj/arm/virtio_net_test.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN

2323.001 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vm.c -o /SRC/obj/[KERNEL] sys_fork: tf->regs[0] is now 40
[DEBUG] Before eret, tf->regs[0] = 40 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/RESDIR/include/__stddef_offsetof.h
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
arm/vm.o
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 49
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 49 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=49 start: pages=22997 batch=128

2324.002 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/SRC/src/include/lock.h

2325.002 /SRC/src/include/arch/cpu.h
/SRC/src/include/process.h

2327.002 /SRC/src/include/trap.h

2328.003 /SRC/src/include/arch/trap.h

2330.003 src/kernel/virtio_net.c:699:20: warning: unused function 'reg_write8' [-Wunused-function]
  699 | static inline void reg_write8(uint32_t offset, uint8_t val) {
      |                    ^~~~~~~~~~

2331.003 [IMGPRF] pid=49 done: pages=22997 mapped=22769 skipped=228 batches=180 stop=0 ms=7663

2332.004 /SRC/obj/arm/nfs_proto-309efc5a.o.tmp new=/SRC/obj/arm/nfs_proto.o

2333.004 /SRC/src/kernel/vm.c
[KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/vm_test.c -o /SRC/TOOLS/SH.BIN
/obj/arm/vm_test.o

2334.004 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/obj/arm/vm-fc5ab816.o.tmp
/TOOLS/CLANG.BIN

2335.004 [KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720

2336.005 v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2338.005 /SRC/src/include/vm.h

2340.006 /TOOLS/RESDIR/include/stdint.h

2342.006 /SRC/src/include/errno.h
/SRC/src/include/fat16.h

2343.006 /SRC/src/include/frame.h
/SRC/src/include/fs.h

2344.007 [IMGPRF] pid=48 done: pages=22997 mapped=22768 skipped=229 batches=180 stop=0 ms=7767
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h
/SRC/src/kernel/vm_test.c

2345.007 /SRC/obj/arm/vm_test-c5ab8167.o.tmp
/SRC/obj/arm/virtio_gpu-c5ab8167.o.tmp new=/SRC/obj/arm/virtio_gpu.o
[KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.

2346.007 [KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/cpu.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/cpu.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/SRC/obj/arm/vm_test-c5ab8167.o.tmp new=/SRC/obj/arm/vm_test.o

2347.007 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/gic.c -o /SRC/obj/arm/gic.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=40ms

2348.008 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=5ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2354.009 [IMGPRF] pid=44 done: pages=22997 mapped=22784 skipped=213 batches=180 stop=0 ms=8301

2355.009 /SRC/obj/arm/virtio_net-309efc5a.o.tmp new=/SRC/obj/arm/virtio_net.o
1 warning generated.

2356.009 [KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=4ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/src/kernel/arch/arm/cpu.c
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/locks.c [DEBUG] Before eret, tf-
2357.010 >regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
-o /SRC/obj/arm/locks.o
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/obj/arm/cpu-74d2309e.o.tmp
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=41 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2358.010 [IMGPRF] pid=48 done: pages=22997 mapped=22786 skipped=211 batches=180 stop=0 ms=9614

2359.010 /SRC/src/include/arch/cpu.h

2360.010 /SRC/src/kernel/arch/arm/gic.c
/TOOLS/RESDIR/include/stdint.h
/SRC/obj/arm/gic-1674d230.o.tmp

2362.011 [IMGPRF] pid=41 done: pages=22997 mapped=22750 skipped=247 batches=180 stop=0 ms=5011
/SRC/src/include/gic.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/process.h
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/src/kernel/arch/arm/locks.c
/SRC/obj/arm/locks-81674d23.o.tmp

2363.011 /SRC/src/include/lock.h
/TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h

2365.012 /SRC/obj/arm/cpu-74d2309e.o.tmp new=/SRC/obj/arm/cpu.o

2366.012 [KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 43
[DEBUG] Before eret, tf->regs[0] = 43 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/mmu.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/mmu.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 44
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 44 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=44 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2367.012 /SRC/obj/arm/net-4d2309ef.o.tmp new=/SRC/obj/arm/net.o

2368.012 3 warnings generated.

2370.013 [KERNEL] Process 50: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
[IMGPRF] pid=44 done: pages=22997 mapped=22768 skipped=229 batches=180 stop=0 ms=4100
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/rtc.c -o/TOOLS/SH.BIN
 /SRC/obj/arm/rtc.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=50 start: pages=22997 batch=128
/SRC/obj/arm/locks-81674d23.o.tmp new=/SRC/obj/arm/locks.o

2371.013 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2372.013 [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/smp.c -o /SRC/obj/arm/smp.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=4ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

2373.014 /TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=4ms
[IMGPRF] pid=41 start: pages=22997 batch=128

2374.014 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2375.014 /SRC/src/kernel/arch/arm/mmu.c

2377.015 /SRC/obj/arm/mmu-674d2309.o.tmp

2379.015 [IMGPRF] pid=50 done: pages=22997 mapped=22751 skipped=246 batches=180 stop=0 ms=8927

2380.015 /TOOLS/RESDIR/include/stdint.h

2381.016 /SRC/src/include/mmu.h
/SRC/src/include/arch/mmu.h

2382.016 /SRC/src/kernel/arch/arm/rtc.c
/SRC/src/include/process.h
[IMGPRF] pid=41 done: pages=22997 mapped=22773 skipped=224 batches=180 stop=0 ms=9058
/SRC/obj/arm/rtc-9efc5ab8.o.tmp
/SRC/src/include/lock.h
/SRC/src/include/arch/cpu.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h
/SRC/obj/arm/gic-1674d230.o.tmp new=/SRC/obj/arm/gic.o
/SRC/src/include/frame.h
/SRC/src/include/vm.h
/SRC/src/kernel/arch/arm/smp.c

2383.016 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/obj/arm/smp-c5ab8167.o.tmp
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/timer.c /TOOLS/SH.BIN
-o /SRC/obj/arm/timer.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2384.016 /TOOLS/RESDIR/include/stdint.h
/SRC/src/include/lock.h

2385.017 /TOOLS/RESDIR/include/stdint.h
/SRC/src/include/arch/cpu.h

2386.017 /SRC/src/include/process.h
/SRC/src/include/trap.h
/SRC/src/include/arch/trap.h

2387.017 [IMGPRF] pid=48 done: pages=22997 mapped=22757 skipped=240 batches=180 stop=0 ms=4066
/SRC/obj/arm/rtc-9efc5ab8.o.tmp new=/SRC/obj/arm/rtc.o
[KERNEL] Process 50: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/trap.c -[KERNEL] sys_fork: tf->regs[0] is now 38
[DEBUG] Before eret, tf->regs[0] = 38 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
o /SRC/obj/arm/trap.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 50
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 50 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=50 start: pages=22997 batch=128

2388.017 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2389.018 /SRC/src/kernel/arch/arm/timer.c
/SRC/obj/arm/timer-674d2309.o.tmp

2391.018 /SRC/src/include/timer.h
/TOOLS/RESDIR/include/stdint.h

2392.019 [IMGPRF] pid=50 done: pages=22997 mapped=22771 skipped=226 batches=180 stop=0 ms=4504
/SRC/obj/arm/smp-c5ab8167.o.tmp new=/SRC/obj/arm/smp.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
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
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/uart.c -/TOOLS/SH.BIN
o /SRC/obj/arm/uart.o
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 41
[DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=41 start: pages=22997 batch=128

2393.019 [DEBUG] Before eret, tf->regs[0] = 41 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2394.019 /SRC/src/kernel/arch/arm/trap.c
/SRC/obj/arm/trap-1674d230.o.tmp

2396.020 /SRC/src/include/trap.h

2397.020 /TOOLS/RESDIR/include/stdint.h
/SRC/obj/arm/timer-674d2309.o.tmp new=/SRC/obj/arm/timer.o
[IMGPRF] pid=41 done: pages=22997 mapped=22790 skipped=207 batches=180 stop=0 ms=4457
/SRC/src/include/arch/trap.h
[KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/boot.s -/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
o /SRC/obj/arm/boot.o
/SRC/src/include/fs.h
/SRC/src/include/fat16.h
/SRC/src/include/lock.h
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
/SRC/src/include/arch/cpu.h
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=1ms
[IMGPRF] pid=48 start: pages=22997 batch=128
/SRC/src/include/process.h

2398.020 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0
/SRC/src/include/nfs.h
/SRC/src/include/ipc_proto.h

2399.021 /SRC/src/include/net.h
/SRC/src/kernel/arch/arm/uart.c
/TOOLS/RESDIR/include/stddef.h
/SRC/obj/arm/uart-ab81674d.o.tmp

2400.021 /TOOLS/RESDIR/include/__stddef_header_macro.h

2401.021 /TOOLS/RESDIR/include/__stddef_ptrdiff_t.h
/SRC/src/include/lock.h
/TOOLS/RESDIR/include/__stddef_size_t.h
/TOOLS/RESDIR/include/stdint.h

2402.021 /TOOLS/RESDIR/include/__stddef_wchar_t.h
[IMGPRF] pid=48 done: pages=22997 mapped=22785 skipped=212 batches=180 stop=0 ms=4393
/TOOLS/RESDIR/include/__stddef_null.h
/TOOLS/RESDIR/include/__stddef_max_align_t.h
/SRC/src/include/arch/cpu.h
/TOOLS/RESDIR/include/__stddef_offsetof.h
/SRC/src/include/setjmp.h
/SRC/src/include/timer.h
/SRC/src/include/arch/timer.h
/SRC/src/include/syscall.h
/SRC/src/include/errno.h
/SRC/src/include/vfs.h
/SRC/src/include/vm.h
CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/boot.s
/SRC/obj/arm/boot.o

2403.022 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/fpu.s -o /SRC/obj/arm/fpu.o
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2405.022 src/kernel/arch/arm/trap.c:1602:16: warning: unused variable 'l2idx' [-Wunused-variable]
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

2407.023 [IMGPRF] pid=48 done: pages=22997 mapped=22744 skipped=253 batches=180 stop=0 ms=4208
CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/fpu.s
/SRC/obj/arm/fpu.o

2408.023 [KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/CLANG.BIN -O2 -Wall -Wextra -g -Isrc/include --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -mgeneral-regs-only -isystem /TOOLS/RESDIR/include -c src/kernel/arch/arm/setjmp.s -o /SRC/obj/arm/setjmp.o
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CLANG.BIN
[KERNEL] sys_fork: tf->regs[0] is now 48
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x000000100000DDF8
/TOOLS/CLANG.BIN
v2 loader: image mapped (lazy), bytes=94192328 pages=22997 first=in elap=0ms
[IMGPRF] pid=48 start: pages=22997 batch=128
[DEBUG] Before eret, tf->regs[0] = 48 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x00000010048493F0

2413.024 [IMGPRF] pid=48 done: pages=22997 mapped=22773 skipped=224 batches=180 stop=0 ms=4978
CLANG: warning: argument unused during compilation: '-O2' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-ffreestanding' [-Wunused-command-line-argument]
CLANG: warning: argument unused during compilation: '-isystem /TOOLS/RESDIR/include' [-Wunused-command-line-argument]
/SRC/src/kernel/arch/arm/setjmp.s
/SRC/obj/arm/setjmp.o
[KERNEL] Process 48: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 45
[DEBUG] Before eret, tf->regs[0] = 45 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 45: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-ascompile rc=0

2416.025 /SRC/obj/arm/uart-ab81674d.o.tmp new=/SRC/obj/arm/uart.o
[KERNEL] Process 41: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.


2447.033 /SRC/obj/arm/mmu-674d2309.o.tmp new=/SRC/obj/arm/mmu.o

2448.033 [KERNEL] Process 44: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 43: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

2567.058 /SRC/obj/arm/trap-1674d230.o.tmp new=/SRC/obj/arm/trap.o

2568.058 3 warnings generated.
[KERNEL] Process 50: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 38: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kerne[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
l-compile rc=0

2569.059 /SRC/obj/arm/vm-fc5ab816.o.tmp new=/SRC/obj/arm/vm.o
[KERNEL] Process 49: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 40: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase=kernel-compile rc=0
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

2648.075 /SRC/obj/arm/fs-ab81674d.o.tmp new=/SRC/obj/arm/fs.o

2649.075 [KERNEL] Process 54: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 47: /TOOLS/SH.BIN exited unexpectedly or gracefully.

2650.076 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

2771.102 /SRC/obj/arm/fat16-09efc5ab.o.tmp new=/SRC/obj/arm/fat16.o
1 warning generated.

2773.103 [KERNEL] Process 42: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

3212.200 /SRC/obj/arm/process-1674d230.o.tmp new=/SRC/obj/arm/process.o
2 warnings generated.

3214.200 [KERNEL] Process 46: /TOOLS/CLANG.BIN exited unexpectedly or gracefully.
[KERNEL] Process 39: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=kernel-compile rc=0

3215.200 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[SELFHOST] phase[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
=s2-kernel-start
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
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
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = #4  ELR = f0x00000010000260F8a
llback, which merged the tail into one path ("cannot open /SRC/");
/TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x0000001000011020
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
# the response file is written at parse time by the make file-write
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

3216.201 [DEBUG] Before eret, tf->regs[0] = -1 ELR = 0x000000100285D7C0

3217.201 [DEBUG] Before eret, tf->regs[0] = 43450365 ELR = 0x00000010028616E4

3218.201 [IMGPRF] pid=5 done: pages=13001 mapped=12653 skipped=348 batches=102 stop=0 ms=2127
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
LLD.BIN: error: cannot open /SRC/obj/arm/net_rdma_test.o: No such/SRC/obj/arm/net_test.o 
file or directory
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

3219.201 /SRC/obj/arm/virtio_gpu_test.o
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
[KERNEL] Process 5: /TOOLS/LLD.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
MAKE.BIN: *** [Makefile.hobbyos:424: kernel] Error 1
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=s2-kernel-fail reason=tool-rc rc=512
[SELFHOST] FAILED-AT stage=s2-kernel
[KERNEL] Process 1: SELFHOST.BIN exited unexpectedly or gracefully.
System halt from CPU 3.

