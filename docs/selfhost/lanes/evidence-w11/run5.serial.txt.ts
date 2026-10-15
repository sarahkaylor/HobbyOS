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
[KERNEL] Core 5 power-on requested.
[KERNEL] Core 6 power-on requested.
start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU 526


3start_scheduler called on CPU 
41

[KERNEL] Core start_scheduler called on CPU 77 power-on requested.

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
v2 loader: image mapped (lazy), bytes=52656 pages=13 first=in elap=1ms
v2 loader: PID=1 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
start_scheduler called on CPU 0
[SELFHOST] DRIVER-UP script=/SRC/SELFHOST.RUN
[SELFHOST] phase=bm-ser1-start
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=1
Inside process_create: clearing memory at 0x0000000072000000
Inside process_create: kmemset done.
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=3 block_idx=-1
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=-1

6.704 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

6.904 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

7.505 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

7.705 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O1/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700

7.905 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

8.706 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O1/TU02.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

8.906 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mappe
8.906 d (lazy), bytes=1951
8.907 36 pages
8.907 =48 fir
8.907 st=in e
8.907 lap=0m
8.907 s

9.107 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

9.707 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O1/TU03.O

9.908 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

10.108 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

10.308 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

10.909 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O1/TU04.O

11.109 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

11.309 [KERNEL] sys_fork: tf->regs[0] is now 4
/SRC/[DEBUG] Before eret, tf->regs[0] = B4M ELR = /0x00000010000260F8B
URN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

12.110 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O1/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

12.310 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

12.510 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

13.111 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O1/TU06.O

13.311 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

13.512 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

14.312 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O1/TU07.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

14.513 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

14.713 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

15.313 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

15.514 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O1/TU08.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

15.714 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

16.515 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O1/TU09.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

16.715 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700

16.915 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

17.516 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

17.716 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O1/TU10.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

17.916 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

18.717 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O1/TU11.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

18.917 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

19.117 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

19.918 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O1/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

20.119 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

20.319 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

20.920 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O1/TU13.O

21.120 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

21.320 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

22.121 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O1/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

22.321 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

22.522 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

23.123 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

23.323 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O1/TU15.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

23.523 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

24.324 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O1/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

24.524 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT ser1 /SRC/BM/O1 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

24.724 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

24.924 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

25.125 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

25.325 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

25.525 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0

25.725 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

25.925 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0

26.125 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0

26.326 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
  PID  PPID  STATE  NAME
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-ser1-done rc=0
[SELFHOST] phase=bm-ser2-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

26.526 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

27.327 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O1/TU01.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

27.527 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

27.728 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

28.529 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O1/TU02.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

28.729 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

28.930 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

29.531 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

29.731 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O1/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

29.931 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

30.732 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O1/TU04.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

30.933 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700

31.133 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

31.734 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

31.934 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O1/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR =
31.934  0x0000
31.934 0
32.134 01000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

32.334 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

32.935 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O1/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

33.135 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

33.335 [KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = S4R ELR = C0x00000010000260F8/
BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

34.136 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O1/TU07.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

34.336 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

34.536 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

35.137 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O1/TU08.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

35.337 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

35.537 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

36.338 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /T4O ELR = O0x00000010000260F8L
S/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O1/TU09.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

36.538 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

36.739 [DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

37.339 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O1/TU10.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

37.540 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

37.740 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

38.341 [KERNEL] Proce
38.341 ss 5: /SRC/B
38.341 M/BURN.
38.341 BIN exit
38.341 ed unex
38.341 pected
38.341 ly or 
38.341 gracef
38.341 ully.
38.341 

38.541 [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O1/TU11.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=[DEBUG] Before eret, tf->regs[0] = 115 first= ELR = in0x0000001000012720 elap=
1ms

38.741 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

39.542 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O1/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

39.742 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4SR ELR = C0x00000010000260F8/
BM/BURN.BIN 700

39.942 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

40.543 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

40.743 [KERNEL] sys_fork: tf->regs[0] is now 4/
T[DEBUG] Before eret, tf->regs[0] = O4O ELR = L0x00000010000260F8S
/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O1/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

40.944 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

41.144 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

41.745 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOOLS/C4P ELR = .0x00000010000260F8B
IN /SRC/BM/SRC/TU14.C /SRC/BM/O1/TU14.O

41.945 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

42.146 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

42.946 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O1/TU15.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

43.146 v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8S
RC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

43.346 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

43.947 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

44.147 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O1/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT ser2 /SRC/BM/O1 = 0

44.347 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

44.548 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

44.748 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

44.949 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
BM-CMP TU06 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2BM- ELR = C0x000000100000DDF8M
P TU07 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

45.149 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

45.349 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0

45.549 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
BM-CMP TU14 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0

45.750 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-ser2-done rc=0
[SELFHOST] phase=bm-j41-start

45.950 Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
/SRC/BM/BURN.BIN 700

46.150 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

46.350 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=858224
 pages=[DEBUG] Before eret, tf->regs[0] = 15/8S first=R ELR = Cin0x00000010000260F8 elap=/
B75Mms
/BURN.BIN 700

46.551 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9/SR ELR = C0x00000010000260F8/
BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=24ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

46.751 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 5822411 pages= ELR = 150x0000001000012720 first=
in elap=2ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

46.951 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O4/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

47.151 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

47.351 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O4/TU02.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

47.551 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O4/TU04.O
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720

47.752 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O4/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8

47.952 /SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 6v2 loader: image mapped (lazy), bytes=944832 ELR =  pages=0x000000100001272011
 first= ELR = in0x00000010000260F8 elap=
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8

48.152 v2 loader: image mapped (lazy), bytes=44832[DEBUG] Before eret, tf->regs[0] =  pages=1011 ELR =  first=0x0000001000012720in
 elap=1ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

48.352 [KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O4/TU05.O

48.552 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=/SRC/BM/BURN.BIN 700
195136 pages=48 first=in elap=77ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=92187195136 ELR =  pages=48 first=in0x0000001000000AB0 elap=
2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9v2 loader: image mapped (lazy), bytes=58224 pages= ELR = 150x000000100000DDF8 first=
in elap=95ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0

48.752 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8[DEBUG] Before eret, tf->regs[0] = 
92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=[DEBUG] Before eret, tf->regs[0] = 1510 first= ELR = in0x0000001000012720 elap=
3ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

48.953 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

49.153 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

49.553 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5/
T[DEBUG] Before eret, tf->regs[0] = O[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
OLS/5 ELR = C0x00000010000260F8P
.BIN /SRC[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/BM/SRC/TU06.C /SRC/BM/O4/TU06.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DC00
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8

49.753 [KERNEL] sys_fork: tf->regs[0] is now 7
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 1951367 pages= ELR = 480x00000010000260F8 first=
in elap=76ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O4/TU07.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O4/TU08.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

49.953 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=70ms
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 4/
92187T[DEBUG] Before eret, tf->regs[0] = O4v2 loader: image mapped (lazy), bytes=O ELR =  ELR = 448320x00000010000260F80x0000001000000AB0

LS[DEBUG] Before eret, tf->regs[0] = /C pages=P11. first=BinI elap=N2 ms
/9S ELR = R0x0000001000012720C
/BM/SRC/TU09.C /SRC/BM/O4/TU09.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100
49.954 000
49.954 DDF8
49.954 

50.154 v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 10v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000012720 pages=
48 first=in elap=65ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

50.354 /SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

50.554 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.

[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7/ ELR = S0x00000010000260F8R
Cv2 loader: image mapped (lazy), bytes=/195136B pages=M48/ first=BURN.BinI elap=N 700
96ms

50.755 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR =  ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB09
 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=9ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=21ms

50.955 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

51.155 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=22ms

51.355 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

51.556 [KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

51.756 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O4/TU11.O
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = /61073742496T ELR = O
O0x0000001000004000L
S[DEBUG] Before eret, tf->regs[0] = /6C ELR = P.0x00000010000260F8Bv2 loader: image mapped (lazy), bytes=I195136N pages= 48/ first=SinR elap=C1/ms
B
M/SRC/TU10.C /SRC/BM/O4/TU10.O
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

51.956 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=[DEBUG] Before eret, tf->regs[0] = in10 elap= ELR = v2 loader: image mapped (lazy), bytes=0x00000010000127203
ms
44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

52.157 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

52.357 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O4/TU13.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7/TOOLS/CP.BI ELR = N0x00000010000260F8 
/SRC/BM/SRC/TU12v2 loader: image mapped (lazy), bytes=.C195136  pages=/48S first=RC/BinM elap=/O4/1Tms
U12.O
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

52.557 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 98
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000012720
9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 448329 pages= ELR = 110x0000001000012720 first=
in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

52.757 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 4483210 pages= ELR = 110x0000001000012720 first=
in elap=1ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /S4R ELR = C0x00000010000260F8/
BM/BURN.BIN 700

52.958 [KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

53.158 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

53.358 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

53.558 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O4/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

53.759 [KERNEL] Process 6: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O4/TU15.O
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

53.959 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O4/TU0x00000010000260F816.O

v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=v2 loader: image mapped (lazy), bytes=1195136ms
 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8

54.159 [KERNEL] sys_fork: tf->regs[0] is now 7[DEBUG] Before eret, tf->regs[0] = 
6[DEBUG] Before eret, tf->regs[0] = 7 ELR =  ELR = 0x000000100000DDF80x0000001000012720

v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=[DEBUG] Before eret, tf->regs[0] = 17ms
 ELR = v2 loader: image mapped (lazy), bytes=0x000000100001272044832
 pages=11 first=in elap=1ms
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8B
M-EXIT j41 /SRC/BM/O4 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

54.359 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0

54.560 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2BM ELR = -0x000000100000DDF8C
MP TU04 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

54.760 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

54.960 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

55.161 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

55.361 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
BM-CMP TU11 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
BM-CMP TU12 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

55.561 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

55.761 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-j41-done rc=0
[SELFHOST] phase=bm-j42-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

55.962 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = SRC/BM/BURN.BIN 700
0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

56.162 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/ ELR = S0x00000010000260F8RC
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

56.362 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720

56.563 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

56.763 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

56.963 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O4/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.

57.163 [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = SRC0x00000010000260F8/
BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

57.364 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O4/TU02.O
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

57.564 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O4/TU03.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 89 ELR =  ELR = 0x00000010000260F80x0000001000012720

/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O4/TU04.O
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

57.764 [KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = v2 loade
57.764 r: image mapp
57.764 ed (l
57.764 azy),
57.764  byt
57.764 es=9
57.764 19
57.964 5136 ELR =  pages=0x000000100000DDF848[DEBUG] Before eret, tf->regs[0] =  first=1073742496in ELR =  elap=0x0000001000005BC069
ms

[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 4483210 pages= ELR = 110x0000001000012720 first=
in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O4/TU05.O

58.165 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

58.365 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=30ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

58.565 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=84ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832[DEBUG] Before eret, tf->regs[0] =  pages=911 ELR =  first=0x0000001000012720in
 elap=2ms

58.766 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=60ms
[KERNEL] sys_fork: tf->regs[0] is now 4/SRC
/B[DEBUG] Before eret, tf->regs[0] = M4/ ELR = B0x00000010000260F8U
RN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=34ms

58.966 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O4/TU06.O

59.166 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=81ms
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720

59.366 v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=10ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

59.567 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O4/TU08.O

59.767 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

59.967 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O4/TU09.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=13ms

60.167 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O4/TU07.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=8195136
 pages=[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x000000100000DDF8 ELR = 
48v2 loader: image mapped (lazy), bytes=0x0000001000000AB0 first=44832
 pages=in11 elap= first=[DEBUG] Before eret, tf->regs[0] = 90in[DEBUG] Before eret, tf->regs[0] =  elap=ms
81 ELR = ms
0x000000100001272092187
 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

60.367 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 5/1073742496T ELR = 
0x0000001000005BC0O[DEBUG] Before eret, tf->regs[0] = O
LS/5C ELR = P0x00000010000260F8.
BIN /SRC/BM/SRC/TU10.C /SRC/BM/O4/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
/SRC/BM/BURN.BIN 700

60.568 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 1951369 pages= ELR = 480x0000001000012720
 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

60.768 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 101073742496
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000400010
 ELR = 0x000000100000DDF8
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 41073742496
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000005BC04
 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

60.968 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1144832 ELR =  pages=0x000000100001272011 first=
in elap=2ms

61.168 [KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

61.369 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O4/TU11.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

61.569 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

61.769 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O4/TU1[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
2.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O4/TU13.O

61.969 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 1951368 pages= ELR = 480x0000001000012720 first=
in elap=14ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136v2 loader: image mapped (lazy), bytes= pages=4483248 pages= first=11in first= elap=in1 elap=ms
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

62.170 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 5/
T[DEBUG] Before eret, tf->regs[0] = O[DEBUG] Before eret, tf->regs[0] = O5L9S ELR =  ELR = 0x00000010000260F8/
0x0000001000012720C
P.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O4/TU14.O
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8

62.370 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8v2 loader: image mapped (lazy), bytes=195136
 pages=48 first=in elap=v2 loader: image mapped (lazy), bytes=6944832ms
 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 100x0000001000000AC8 ELR = 
0x0000001000012720

62.570 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

62.770 [KERNEL] Process 4: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

62.970 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O4/TU15.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

63.171 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O4/TU16.O
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

63.371 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 4: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j42 /SRC/BM/O4 = 0

63.571 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

63.771 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

63.972 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

64.172 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

64.372 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] B
64.572 efore eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

64.772 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0

64.973 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-j42-done rc=0
[SELFHOST] phase=bm-j81-start

65.173 Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

65.373 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5/S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720

65.573 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700

65.774 [KERNEL] sys_fork: tf->regs[0] is now 9
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
58224 pages=15 first=in elap=74ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700

65.974 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=68ms
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=75ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

66.174 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=58224 pages=1315 first=
in[DEBUG] Before eret, tf->regs[0] =  elap=89ms
13 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1592187 ELR =  ELR = 0x00000010000260F80x0000001000000AB0
/
SRC/BM/BURN.BIN 700

66.374 [DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8 ELR = 0x00000010000260F8

v2 loader: image mapped (lazy), bytes=/SRC58224/ pages=B15M first=/inB elap=URN.B79Ims
N 700
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=22ms

66.575 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720v2 loader: image mapped (lazy), bytes=
58224 pages=15 first=in elap=2ms

66.775 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = /7T ELR = OOLS0x00000010000260F8/
CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O8/TU03.O
[KERNEL] Process 6: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /0x00000010000260F8T
OOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O8/TU02.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

66.975 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=6195136 ELR = 0x000000100000DDF8 pages=
48v2 loader: image mapped (lazy), bytes= first=195136v2 loader: image mapped (lazy), bytes= pages=58224in pages= elap=1573 first=ms
in48 elap= first=2in elap=ms
[DEBUG] Before eret, tf->regs[0] = 1996ms
 ELR = 0x0000001000012720
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 107374249692187 ELR =  ELR = 0x00000010000040000x0000001000000AC8

[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

67.175 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/v2 loader: image mapped (lazy), bytes=T44832U01 pages=.11C first= in/ elap=Sv2 loader: image mapped (lazy), bytes=R3C195136ms
 pages=/48B first=Min/ elap=O18ms
/TU01.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8

67.376 [DEBUG] Before eret, tf->regs[0] = /[DEBUG] Before eret, tf->regs[0] = 107374249611T ELR = OOLS/CP.BIN0x0000001000012720 /SRC/BM/SR
C ELR = /0x0000001000004000T
U04.C /SRC/BM/O8/TU04.O
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 10737424961073742496 ELR =  ELR = [KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
0x0000001000005BC0[KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.

0x0000001000004000
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=6ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000005BC092187
 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = /T1073742496O ELR = OLS/CP.0x0000001000005BC0BI7N /SRC/B
M/ ELR = SRC/TU0x00000010000260F80
5.C /SRC/BM/O8/TU05.O

67.576 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8

67.776 /TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O8/TU06.O
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] Process 15: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 16: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6v2 loader: image mapped (lazy), bytes= ELR = 1951360x000000100000DDF8 pages=
48 first=in elap=68ms
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8/TOv2 loader: image mapped (lazy), bytes=O
L44832 pages=S11/ first=CPin. elap=BIN [DEBUG] Before eret, tf->regs[0] = /v2 loader: image mapped (lazy), bytes=S78Rms
C448326/ pages=B11M first=/inS elap=R ELR = C0x0000001000012720/4Tms
U
07.C /SRC/BM/O8/TU07.O

67.976 [KERNEL] sys_fork: tf->regs[0] is now 13v2 loader: image mapped (lazy), bytes=
195136[DEBUG] Before eret, tf->regs[0] =  pages=1348 ELR =  first=0x000000100000DDF8in
 elap=69ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

68.177 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1544832
 pages=11 first=[DEBUG] Before eret, tf->regs[0] = in15 elap= ELR = 0x000000100000DDF879
ms
[DEBUG] Before eret, tf->regs[0] = /92187T ELR = O0x0000001000000AB0O
Lv2 loader: image mapped (lazy), bytes=S/195136C pages=P48. first=BinI elap=N1 ms
/SRC/BM/SRC/TU08.C /SRC/BM/O8/TU08.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

68.377 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = /1073742496S ELR = R0x0000001000004000C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

68.577 [KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5/SRC/ ELR = B0x00000010000260F8M
/BURN.BIN 700

68.777 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 644832 ELR =  pages=0x000000100001272011
 first=in elap=2ms
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8

68.977 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=195136 pages=848 ELR =  first=0x0000001000012720in
 elap=81ms
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 13[DEBUG] Before eret, tf->regs[0] =  ELR = 160x0000001000012720 ELR = 
0x0000001000012720[KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.

[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=57ms

69.178 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8SRC/v2 loader: image mapped (lazy), bytes=B
M44832/B pages=U11R first=Nin. elap=BIN 1137ms
00

69.378 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=23ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/SRC/BM/BURN.BIN 700
 ELR = 0x00000010000260F8

69.578 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

69.778 [KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in[DEBUG] Before eret, tf->regs[0] =  elap=921871 ELR = ms
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8[DEBUG] Before eret, tf->regs[0] = 
92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9218758224 ELR =  pages=0x0000001000000AC815
 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0

69.978 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
13 ELR = 0x0000001000012720

70.178 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O8/TU10.O
v2 loader: image mapped (lazy), bytes=58224 pages=[DEBUG] Before eret, tf->regs[0] = 15[DEBUG] Before eret, tf->regs[0] =  first=92187in ELR =  elap=0x0000001000000AB0
392187ms
 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AC80x0000001000000AC8

[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720

70.379 [KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/v2 loader: image mapped (lazy), bytes=SRC58224/TU09.C /SRC/BM/O8/TU09.O
 pages=15 first=in elap=78ms
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

70.579 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O8/TU11.O
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=19513658224 pages= pages=4815 first= first=inin elap= elap=3265ms
ms
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

70.779 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O8/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=51ms

70.979 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=41ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1492187 ELR = 0x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 141073742496 ELR =  ELR = 0x0000001000005BC00x000000100000DDF8[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 
0x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187[DEBUG] Before eret, tf->regs[0] =  ELR = 0x0000001000000AC892187
 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000v2 loader: image mapped (lazy), bytes=
58224 pages=15 first=in elap=2ms

71.179 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1592187 ELR =  ELR = 10737424960x0000001000000AC814v2 loader: image mapped (lazy), bytes= ELR = 448320x0000001000012720 pages=
11 ELR = 0x000000100000DDF8 first=
0x0000001000005BC0

in elap=3ms
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

71.379 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5[KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x00000010000127201073742496
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = /v2 loader: image mapped (lazy), bytes=T44832O pages=O11L first=Sin/ elap=C1073742496P2.ms
B ELR = I0x0000001000005BC0N /S
Rv2 loader: image mapped (lazy), bytes=C195136/ pages=B48M first=/inS elap=RC/T2Ums
14.C /SRC/BM/O8/TU14.O
[KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

71.580 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 10737424966 ELR =  ELR = 0x0000001000005BC00x0000001000012720
v2 loader: image mapped (lazy), bytes=
44832 pages=11 first=in elap=1ms
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8

71.780 [KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /S[DEBUG] Before eret, tf->regs[0] = R6 ELR = C0x0000001000012720/B[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
M
/SRC/TU12.C /SRC/BM/O8/TU12.O
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=448329 pages=
11[DEBUG] Before eret, tf->regs[0] =  first=9in ELR =  elap=0x00000010000260F881
ms
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.

71.980 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O8/TU15.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O8/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=10ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

72.180 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1211 ELR =  ELR = 0x00000010000127200x0000001000012720

[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=50ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=124ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully
72.180 .

72.380 [KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=200ms
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

72.580 [KERNEL] Process 4: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j81 /SRC/BM/O8 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

72.781 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

72.981 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

73.181 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

73.381 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

73.581 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0

73.781 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0

73.982 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0

74.182 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      READY      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-j81-done rc=0
[SELFHOST] phase=bm-j82-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

74.382 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

74.582 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=6195136
 pages=[DEBUG] Before eret, tf->regs[0] = 486 first= ELR = in0x00000010000260F8 elap=
74ms
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7v2 loader: image mapped (lazy), bytes= ELR = 0x000000100000DDF8
195136 pages=48 first=in elap=76ms
/SRC/BM/BURN.BIN 700

74.783 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 87
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x00000010000127208
 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now /10S
RC[DEBUG] Before eret, tf->regs[0] = /10B ELR = M0x00000010000260F8/
BURN.BIN 700

74.983 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=11ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8

75.183 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AB00x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 13/12
S ELR = [DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=13R ELR = C0x00000010000260F8/0x0000001000012720
B
M/[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 480x0000001000000AC8 first=BURN.BINin
 elap= 80700
ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

75.383 [KERNEL] sys_fork: tf->regs[0] is now /14SRC/BM/BURN.BIN 700

[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=19513658224 pages= pages=4815 first= first=in elap=in2 elap=ms
3ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218715 ELR =  ELR = 0x0000001000000AC80x0000001000012720


75.583 [KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 16 ELR = 170x0000001000012720 ELR = 
0x000000100000DDF8
[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=4ms

75.783 [KERNEL] sys_fork: tf->regs[0] is now 18
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x000000100000DDF8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=92187 ELR = 0x0000001000000AB0
58224 pages=15 first=in elap=3ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O8/TU01.O
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9218758224 ELR =  pages=0x0000001000000AC815
 first=in elap=115ms
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

75.983 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /TOOLS/CP.BIN /SRC/BM/SR0x00000010000260F8C/TU02.C /SRC/BM/O8/TU02.O

[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=195136195136 pages= pages=4848 first= first=inin elap= elap=142ms
ms
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8

76.183 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8/
TOOLS/[DEBUG] Before eret, tf->regs[0] = C7P ELR = .0x0000001000012720B
IN /SRC/BM/SRC/TU04.C /SRC/BM/O8/TU04.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9218758224 ELR =  pages=0x0000001000000AC815
 first=195136in pages= elap=4812 first=ms
in elap=3ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0

76.384 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = /[DEBUG] Before eret, tf->regs[0] = T92187O ELR = O0x0000001000000AC86
LS/CP.BI ELR = N0x00000010000260F8 
/SRC/BM/SRC/TU03.C /SRC/BM/O8/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 1073742496[DEBUG] Before eret, tf->regs[0] = 9 ELR =  ELR = 0x00000010000040000x0000001000012720


76.584 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O8/TU06.O
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.

76.784 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O8/TU05.O
[KERNEL] Process 18: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O8/TU07.O
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=223ms

76.984 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48[DEBUG] Before eret, tf->regs[0] =  first=15in ELR =  elap=0x0000001000012720
82ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5v2 loader: image mapped (lazy), bytes=
[DEBUG] Before eret, tf->regs[0] = 448325 pages=11 first= ELR = in elap=73ms
0x000000100000DDF8

77.184 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = /0x00000010000260F8T
OOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O8/TU08.O
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=/195136S pages=R48C first=/inB elap=M12/ms
BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000004000 pages=48
 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187v2 loader: image mapped (lazy), bytes=44832 ELR =  pages=0x0000001000000AC811
 first=in elap=128ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

77.384 [KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
14 ELR = 0x0000001000012720
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
/SRC/BM/BURN.BIN 700

77.584 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 92187107374249692187 ELR =  ELR = 0x0000001000000AB0 ELR = 
0x0000001000005BC00x0000001000000AB0

[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

77.784 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187/ ELR =  ELR = S0x0000001000000AC8R
C/BM/0x0000001000000AC8B
URN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

77.985 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

78.185 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=91ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

78.385 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=58224 pages=[DEBUG] Before eret, tf->regs[0] = 1592187 first= ELR = in0x0000001000000AC8 elap=
2ms
v2 loader: image mapped (lazy), bytes=58224 pages=15v2 loader: image mapped (lazy), bytes= first=44832in[DEBUG] Before eret, tf->regs[0] =  pages=10 elap= ELR = 120x0000001000012720ms

11 first=in elap=12ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720

78.585 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=2ms

78.785 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=71ms
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8

78.985 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=36ms
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=3ms

79.185 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=91ms
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O8/TU12.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

79.385 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 10 ELR = 10737424960x0000001000012720
 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 1073742496/ ELR = 0x0000001000005BC0T
OOLS/CP.B[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
IN /SRC/BM/SRC/TU11.C /SRC/BM/O8/TU11.O
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.

79.586 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9 ELR = 582240x000000100000DDF8 pages=v2 loader: image mapped (lazy), bytes=15 first=195136 pages=in48 first=
in elap=4ms
 elap=6ms
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O8/TU10.O
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8

79.786 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = /v2 loader: image mapped (lazy), bytes=TO14O58224L pages=S15/ first=CP.BIinN elap= 3 ELR = /0x000000100000DDF8ms
Sv2 loader: image mapped (lazy), bytes=R195136C pages=/48B first=Min/ elap=S1Rms
C
/TU09.C /SRC/BM/O8/TU09.O

79.986 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF892187
v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000000AB0 pages=
48 first=in elap=70ms
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 16: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] =  ELR = 0x00000010000260F8
14/v2 loader: image mapped (lazy), bytes=T44832O pages=OL11S/ first=CPin.B elap=IN 123/Sms
RC/BM/SRC/TU13.C  ELR = /SRC/BM/O8/TU13.O
0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=23ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000

80.186 [DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1073742496195136 ELR =  pages=0x0000001000005BC048
 first=in elap=29ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8

80.386 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8

80.586 [DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O8/TU15.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=644832 ELR =  pages=0x00000010000260F811
 first=[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
in elap=93ms
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000

80.786 [KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=4ms
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[KERNEL] Process 17: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.

80.987 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O8/TU16.O
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.

81.187 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O8/TU14.O
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=8ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8v2 loader: image mapped (lazy), bytes=
195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

81.387 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=[DEBUG] Before eret, tf->regs[0] = in8 elap= ELR = 10x0000001000012720ms

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

81.587 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=50ms
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

81.787 [KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j82 /SRC/BM/O8 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

81.987 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

82.187 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

82.387 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

82.588 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

82.788 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0

82.988 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0

83.188 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0

83.388 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-j82-done rc=0
[SELFHOST] phase=bm-failser-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

83.588 [KERNEL] sys_fork: tf->regs[0] is now 4
/SRC[DEBUG] Before eret, tf->regs[0] = /BM/BURN.4B ELR = I0x00000010000260F8N
 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

84.389 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/OF/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

84.589 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700

84.789 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

85.390 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

85.590 [KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = T4O ELR = O0x00000010000260F8L
S/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/OF/TU02.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

85.790 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /SR0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

85.990 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

86.591 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

86.791 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/OF/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

86.992 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8S
RC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

87.792 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/OF/TU04.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

87.993 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

88.193 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

88.794 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

88.994 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/OF/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

89.194 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

89.995 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/OF/TU06.O

90.196 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

90.396 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

91.197 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/T ELR = OOLS/CP.BIN /SRC/BM/SRC/T0x00000010000260F8U
07.C /SRC/BM/OF/TU07.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

91.397 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

91.597 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

92.198 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/OF/TU08.O

92.398 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

92.598 [KERNEL] sys_fork: tf->regs[0] is now /4S
RC[DEBUG] Before eret, tf->regs[0] = /4B ELR = M0x00000010000260F8/
BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

93.399 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/OF/TU09.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

93.599 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

93.799 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /SRC/BM/B4U ELR = R0x00000010000260F8N
.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

94.600 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/OF/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

94.800 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

95.000 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

95.601 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

95.801 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/OF/TU11.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

96.001 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

96.801 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/OF/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

97.002 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = SRC/BM/BURN.BIN 300
0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

97.202 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

97.402 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

97.602 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
exit 9
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = M2A ELR = K0x000000100000DDF8E
.BIN: *** [/SRC/BM/MKFAIL.MK:76: /SRC/BM/OF/TU13.O] Error 9
BM-EXIT faiv2 loader: image mapped (lazy), bytes=l95536s pages=e24r first= in/ elap=S1Rms
C/BM/OF = 2

97.802 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

98.002 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

98.203 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

98.403 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

98.603 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0

98.803 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 2

99.004 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

99.204 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
BM-EXPECTED-FAIL ser make_rc=2
[SELFHOST] phase=bm-failser-done rc=0
[SELFHOST] phase=bm-failj8-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

99.404 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700

99.604 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now /v2 loader: image mapped (lazy), bytes=S6R195136C
/ pages=B[DEBUG] Before eret, tf->regs[0] = M48/6B first= ELR = U0x00000010000260F8Rin
N elap=.80Bms
IN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

99.804 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720v2 loader: image mapped (lazy), bytes=
58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8

100.004 [KERNEL] sys_fork: tf->regs[0] is now 9
/[DEBUG] Before eret, tf->regs[0] = SRC/BM/BURN.BIN 700[DEBUG] Before eret, tf->regs[0] = 
89 ELR =  ELR = 0x00000010000127200x00000010000260F8

v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=12ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700

100.205 [KERNEL] sys_fork: tf->regs[0] is now 11[DEBUG] Before eret, tf->regs[0] = 
92187[DEBUG] Before eret, tf->regs[0] =  ELR = 110x0000001000000AB0 ELR = 
0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x000000100000DDF8 ELR = 
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 120x0000001000000AC8 ELR = 
0x0000001000012720

100.405 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1458224
 pages=15 first=[DEBUG] Before eret, tf->regs[0] = in14 elap= ELR = 690x000000100000DDF8ms

[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 15
[DE
100.605 BUG] Before eret, tf->regs[0] = [KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
15 ELR = 0x00000010000260F8
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=119ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=53ms

100.805 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/OF2/TU03.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

101.005 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 8 ELR = 1951360x0000001000012720 pages=v2 loader: image mapped (lazy), bytes=4858224 first= pages=in15 elap= first=12inms
 elap=
1ms
v2 loader: image mapped (lazy), bytes=195136 pages=[DEBUG] Before eret, tf->regs[0] = 4892187 first= ELR = in0x0000001000000AB0 elap=
22ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1692187
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB016
 ELR = 0x000000100000DDF8

101.205 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5[DEBUG] Before eret, tf->regs[0] = / ELR = T0x00000010000260F8O
OL92187Sv2 loader: image mapped (lazy), bytes= ELR = 582240x0000001000000AC8 pages=
15/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/OF2/TU02.O
 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

101.406 [KERNEL] sys_fork: tf->regs[0] is now 18
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x000000100000DDF8
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1814 ELR = 
0x0000001000012720[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=14
 ELR = 44832 pages=0x000000100000DDF811
 first=in elap=2ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/OF2/TU01.O

101.606 [DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
[KERNEL] Process 18: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

101.806 [KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=15ms
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/OF2/TU04.O
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8

102.006 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/OF2/TU05.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/OF2/TU06.O
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.

v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 15: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496/ ELR = S0x0000001000005BC0RC/BM/BUR[DEBUG] Before eret, tf->regs[0] = N92187.BIN 700
 ELR = 0x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

102.206 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=11195136
 pages=48 first=in elap=94[DEBUG] Before eret, tf->regs[0] = ms
11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 12v2 loader: image mapped (lazy), bytes=
195136 pages=[DEBUG] Before eret, tf->regs[0] = 4812 first= ELR = 0x000000100000DDF8in
 elap=90[DEBUG] Before eret, tf->regs[0] = ms
10737424961073742496 ELR =  ELR = 0x00000010000040000x0000001000004000

[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

102.406 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=13[DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 44832 ELR = 44832 pages=0x0000001000005BC011
0x000000100000DDF8 pages=
11 first= first=in elap=in77ms
 elap=76ms
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/OF2/TU08.O
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720

102.607 /TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/OF2/TU07.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=16195136 pages=48 first=in
 elap=[DEBUG] Before eret, tf->regs[0] = 6816ms
 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=v2 loader: image mapped (lazy), bytes=in58224 elap=[DEBUG] Before eret, tf->regs[0] = 115 pages= ELR = 150x0000001000012720 first=
ms
in elap=2ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=14ms
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720

102.807 [KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF892187
 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8v2 loader: image mapped (lazy), bytes=
44832 pages=11 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

103.007 [KERNEL] sys_fork: tf->regs[0] is now 18
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x0000001000012720
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = /0x00000010000260F8S
RC/BM/BURN.BIN 700

103.207 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=66ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [KERNEL] Process 17: /TOOLS/CP.BIN exited unexpectedly or gracefully.
0x0000001000005BC0
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

103.408 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=31ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=43ms
[KERNEL] Process 18: /TOOLS/CP.BIN exited unexpectedly or gr
103.408 ace
103.408 fu
103.408 ll
103.408 y.
103.408 

103.608 [KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=42ms
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.

103.808 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/OF2/TU09.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 921874 ELR =  ELR = 0x0000001000000AC80x00000010000260F8

[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8

104.008 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/OF2/TU10.O
/SRC/BM/BURN.BIN 300
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=12
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1058224 ELR =  pages=0x0000001000012720
1215 ELR =  first=0x000000100000DDF8in
 elap=71ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=[DEBUG] Before eret, tf->regs[0] = 112ms
 ELR = 0x0000001000012720

104.208 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = /13S[DEBUG] Before eret, tf->regs[0] = R ELR = C0x00000010000260F8/
B92187M ELR = /BURN0x0000001000000AB0.BIN 700
[DEBUG] Before eret, tf->regs[0] = 
92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1492187
[DEBUG] Before eret, tf->regs[0] =  ELR = v2 loader: image mapped (lazy), bytes=0x0000001000000AC814
 ELR = 582240x00000010000260F8 pages=
15 first=in elap=81ms

104.409 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136v2 loader: image mapped (lazy), bytes= pages=19513648 pages= first=48in first= elap=in42 elap=ms
42ms

104.609 [DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=10ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8

104.809 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1611 ELR =  ELR = 0x00000010000127200x0000001000012720

v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

105.009 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.

105.209 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1210 ELR =  ELR = 0x000000100000DDF8
0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=/58224TOOLS/ pages=CP.BI15N first= /SRin elap=C95/Bms
M/SRC/TU11.C /SRC/BM/OF2/TU11.O

105.409 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
exit 9
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=86ms
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
195136 pages=48 first=in elap=85ms
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

105.610 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/[DEBUG] Before eret, tf->regs[0] = T92187O ELR = OLS0x0000001000000AB0/
CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/OF2/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [DEBUG] Before eret, tf->regs[0] = 0x00000010000040001073742496
 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

105.810 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
MAKE.BIN: *** [/SRC/BM/MKFAIL.MK:76: /SRC/BM/OF2/TU13.O] Error 9
MAKE.BIN: *** Waiting for unfinished jobs....
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /
105.810 S
105.810 R
105.810 C/
105.810 B
105.810 M/S
106.010 RC/TU14.C /SRC/BM/OF2/TU14.O
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=944832
 pages=[DEBUG] Before eret, tf->regs[0] = 119 ELR =  first=0x00000010000260F8in[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.

 elap=81ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

106.210 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/S[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
RC/TU16.C /SRC/BM/OF2/TU16.O

106.410 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = /TOO1073742496LS/CP ELR = .0x0000001000004000BI
N /SRC/BM/SRC/TU15.C /SRC/BM/OF2/TU15.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

106.610 [DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT failj8 /SRC/BM/OF2 = 2

106.811 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

107.011 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

107.211 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

107.411 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

107.611 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

107.811 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

108.012 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

108.212 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN

108.412 [KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
BM-EXPECTED-FAIL j8 make_rc=2
[SELFHOST] phase=bm-failj8-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=8
[KERNEL] Process 1: SELFHOST.BIN exited unexpectedly or gracefully.
System halt from CPU 2.

