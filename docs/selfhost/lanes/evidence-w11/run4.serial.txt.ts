6.503 UEFI firmware (version 2025.11-3ubuntu7 built at 11:11:58 on Mar 31 2026)
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
start_scheduler called on CPU start_scheduler called on CPU start_scheduler called on CPU 21start_scheduler called on CPU start_scheduler called on CPU 

54[KERNEL] Core 35 power-on requested.

[KERNEL] Core 
6
 power-on requested.
[KERNEL] Core 7start_scheduler called on CPU  power-on requested.
7start_scheduler called on CPU 
6
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
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
Inside process_create: acquiring lock...
Inside process_create: lock released. pid=2 block_idx=-1
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

6.704 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

7.504 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O1/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

7.705 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

7.905 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

8.506 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O1/TU02.O

8.706 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
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

8.906 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

9.708 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O1/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

9.908 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

10.108 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

10.709 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

10.909 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = TOOLS/0x00000010000260F8C
P.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O1/TU04.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=10ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

11.109 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

11.910 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O1/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

12.111 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

12.311 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

12.911 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

13.112 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O1/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

13.312 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

13.512 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

14.113 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOLS/ ELR = C0x00000010000260F8P
.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O1/TU07.O

14.313 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
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
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

14.513 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

15.114 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

15.315 [KERNEL] sys_fork: tf->regs[0] is now 4
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

15.515 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
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
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

16.316 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O1/TU09.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

16.516 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /SRC/BM4/ ELR = B0x00000010000260F8U
RN.BIN 700

16.716 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

17.517 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O1/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 4
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

17.717 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[
17.717 0]
17.917  = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

18.518 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = TOOLS0x00000010000260F8/C
P.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O1/TU11.O

18.718 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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

18.918 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

19.719 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8T
OOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O1/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped
19.719  (lazy), bytes=4
19.719 4832 p
19.719 ages=1
19.719 1 firs
19.719 t=in e
19.720 lap=0
19.720 ms

19.920 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8

20.120 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

20.721 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O1/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

20.921 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/SRC ELR = /BM/B0x00000010000260F8U
RN.BIN 700

21.121 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

21.722 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

21.922 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O1/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

22.122 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/SRC/B ELR = M0x00000010000260F8/
BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

22.323 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

22.923 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O1/TU15.O

23.123 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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

23.323 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

24.124 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O1/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

24.324 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT ser1 /SRC/BM/O1 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0

24.524 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0

24.725 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

24.925 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

25.125 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms

25.325 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

25.525 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8BM
-CMP TU12 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0

25.726 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0

25.926 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
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

26.126 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

26.927 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O1/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

27.127 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = S4R ELR = C0x00000010000260F8/
BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

27.327 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

27.928 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

28.128 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O1/TU02.O
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

28.328 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

29.129 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /T4O ELR = O0x00000010000260F8L
S/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O1/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

29.329 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8S
RC/BM/BURN.BIN 700

29.530 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

30.130 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

30.330 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O1/TU04.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

30.531 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

31.331 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOOLS/C4P ELR = .0x00000010000260F8B
IN /SRC/BM/SRC/TU05.C /SRC/BM/O1/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

31.531 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms

31.732 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

32.332 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

32.532 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O1/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

32.733 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

32.933 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

33.533 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O1/TU07.O

33.733 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
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
/SRC/BM/BURN.BIN 700

33.933 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

34.734 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O1/TU08.O
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

34.934 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

35.134 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

35.735 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Be
35.735 fore er
35.735 et
35.735 , 
35.735 tf-
35.735 >r
35.735 eg
35.735 s[
35.735 0]
35.735  =
35.735  4
35.735  
35.735 ELR
35.735  
35.935 = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O1/TU09.O
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

36.136 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

36.936 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O1/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] 
36.936 is now 5

37.137 [DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/SRC ELR = /0x00000010000260F8B
M/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

37.337 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

37.938 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

38.138 [KERNEL] sys_fork: tf->regs[0] is now 4
/[DEBUG] Before eret, tf->regs[0] = T4O ELR = O0x00000010000260F8L
S/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O1/TU11.O
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

38.338 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

39.139 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O1/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

39.339 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

39.539 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

40.140 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

40.340 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O1/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

40.540 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C/BM/
BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

41.341 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O1/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

41.541 [KERNEL] sys_fork: tf->regs[0] is now 5
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

41.742 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

42.342 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

42.543 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O1/TU15.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=10ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700

42.743 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

43.544 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOLS/CP.B ELR = I0x00000010000260F8N
 /SRC/BM/SRC/TU16.C /SRC/BM/O1/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

43.744 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT ser2 /SRC/BM/O1 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0

43.944 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0

44.145 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
BM-[DEBUG] Before eret, tf->regs[0] = C2M ELR = P0x000000100000DDF8 
TU04 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

44.345 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

44.545 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

44.745 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

44.945 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
BM-CMP TU11 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0

45.146 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0

45.346 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
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
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

45.546 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms

45.746 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=6/195136S pages=R48C first=/inB elap=M83/ms
B
U[DEBUG] Before eret, tf->regs[0] = RN.B6I ELR = N0x00000010000260F8 
700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

45.947 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now /7S
R[DEBUG] Before eret, tf->regs[0] = C7/BM/BUR ELR = N.BIN 0x00000010000260F87
00
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8

46.147 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9[DEBUG] Before eret, tf->regs[0] =  ELR = 8[DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF892187 ELR =  ELR = 
0x0000001000000AB0v2 loader: image mapped (lazy), bytes=
582240x0000001000012720 pages=[DEBUG] Before eret, tf->regs[0] = 15
 first=9in ELR =  elap=0x00000010000127201
ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AC80x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 5822410 pages= ELR = 150x0000001000012720 first=
in elap=2ms

46.347 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

46.747 [KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

46.948 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = /TOOL7S ELR = /0x00000010000260F8C
P.BIN /SRC/BM/SRC/TU04.C /SRC/BM[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
/O4/TU04.O
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O4/TU03.O
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

47.148 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4v2 loader: image mapped (lazy), bytes= ELR = 1951360x00000010000260F8 pages=/
TO48O first=LinS elap=/C72Pms
v2 loader: image mapped (lazy), bytes=.44832B pages=I11N first= in/ elap=1ms
SRC/BM/SRC/TU01.C /SRC/BM/O4/TU01.O
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5[DEBUG] Before eret, tf->regs[0] =  ELR = 80x00000010000260F8
 ELR = 0x0000001000012720

47.348 /TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O4/TU02.O
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=64ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 6
/SRC/BM/BURN.B[DEBUG] Before eret, tf->regs[0] = I1073742496N ELR =  0x000000100000400070
0
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=inv2 loader: image mapped (lazy), bytes= elap=1951360 pages=ms
48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000
47.348 001000000
47.548 AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG]
47.749  Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0[DEBUG] Before eret, tf->regs[0] = 
8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=11ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8

47.949 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
51ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=73ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

48.149 [KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

48.349 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
58224 pages=15 first=in elap=21ms
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

48.550 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O4/TU05.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

48.750 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

48.950 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O4/TU06.O

49.150 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

49.350 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O4/TU07.O
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

49.551 [KERNEL] sys_fork: tf->regs[0] is now 4/TOOLS/
C[DEBUG] Before eret, tf->regs[0] = P4. ELR = B0x00000010000260F8I
N /SRC/BM/SRC/TU08.C /SRC/BM/O4/TU08.O
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 480x0000001000000AB0 first=
in[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] =  elap=921872 ELR = ms
0x0000001000000AC892187
 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

49.751 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=66ms
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

49.951 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O4/TU09.O
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=4483258224 pages= pages=1115 first= first=inin elap= elap=5544ms
ms

50.151 [KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

50.352 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=7195136
/[DEBUG] Before eret, tf->regs[0] = S pages=748 ELR =  first=0x00000010000260F8in
 elap=R86Cms
/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

50.552 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 582248 pages= ELR = 150x00000010000127206
 ELR =  first=0x00000010000260F8in
 elap=75ms
[DEBUG] Before eret, tf->
50.552 regs[0] = 9
50.552 218
50.552 7
50.552  EL
50.552 R = 0
50.552 x000
50.752 0001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 9v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000012720
 pages=48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

50.952 [KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O4/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

51.153 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700

51.353 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

51.553 /TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O4/TU11.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O4/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

51.753 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O4/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 892187 ELR =  ELR = 0x000000100000DDF80x0000001000000AB0
v2 loader: image mapped (lazy), bytes=
195136 pages=48 first=in elap=70ms
v2 loader: image mapped (lazy), bytes=44832[DEBUG] Before eret, tf->regs[0] =  pages=[DEBUG] Before eret, tf->regs[0] = 9218711 first= ELR = in8 elap= ELR = 0x0000001000000AC80x00000010000127202

ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

51.954 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = /9S ELR = R0x0000001000012720C/BM/
BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=74ms

52.154 [KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O4/TU14.O

52.354 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

52.554 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 10v2 loader: image mapped (lazy), bytes=9 ELR = 1951360x000000100000DDF8 pages=
48 ELR =  first=0x0000001000012720in
 elap=88ms
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

52.755 v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=33ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
4 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

53.155 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

53.355 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O4/TU15.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

53.556 [KERNEL] Process 4: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/TOOL ELR = S0x00000010000260F8/
CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O4/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

53.756 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j41 /SRC/BM/O4 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0

53.956 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0

54.156 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

54.357 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=2ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

54.557 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2B ELR = M0x000000100000DDF8-CMP
 TU08 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

54.757 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

54.957 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2B ELR = M0x000000100000DDF8-
CMP TU11 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

55.157 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

55.357 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
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

55.558 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

55.758 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6v2 loader: image mapped (lazy), bytes= ELR = 0x00000010000260F8
58224 pages=15 first=in elap=72ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

55.958 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8

56.158 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1058224 ELR =  pages=0x000000100001272015
 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=v2 loader: image mapped (lazy), bytes=4858224 first= pages=in15 elap= first=11inms
 elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

56.358 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720v2 loader: image mapped (lazy), bytes=
58224 pages=15 first=in elap=1ms

56.559 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /TOO4L ELR = S0x00000010000260F8/
CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O4/TU01.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

56.759 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4/
SR[DEBUG] Before eret, tf->regs[0] = C4/ ELR = B0x00000010000260F8M
/BURN.BIN 700
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

56.959 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /S[DEBUG] Before eret, tf->regs[0] = RC/BM/SR5C[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
/TU03.C /SRC/BM/O4/TU03.O
 ELR = [KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x0000001000012720[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

57.159 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8v2 loader: image mapped (lazy), bytes= ELR = 582240x000000100000DDF8 pages=
15 first=in elap=72ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O4/TU02.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O4/TU04.O

57.360 [DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=[DEBUG] Before eret, tf->regs[0] = 1491073742496ms
 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
/SRC/BM/BURN.BIN 700

57.560 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 87 ELR = 
0x0000001000012720[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=744832 ELR =  pages=0x00000010000260F811
 first=in
 elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 10v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=
[DEBUG] Before eret, tf->regs[0] = 7410ms
 ELR = 0x000000100000DDF8

57.760 [DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1044832 ELR =  pages=0x000000100001272011
 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
/v2 loader: image mapped (lazy), bytes=S58224R pages=C15/BM first=/inB elap=1ms
URN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8

57.961 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8
OLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O4/TU05.O

58.161 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 10737424966 ELR =  ELR = 0x0000001000005BC00x0000001000012720


58.361 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 11v2 loader: image mapped (lazy), bytes=
58224[DEBUG] Before eret, tf->regs[0] =  pages=1511 ELR =  first=0x000000100000DDF8in
 elap=64ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

58.562 /SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

58.762 [KERNEL] sys_fork: tf->regs[0] is now /7T
O[DEBUG] Before eret, tf->regs[0] = O7L ELR = S0x00000010000260F8/
CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O4/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

58.962 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8

59.163 [DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms
[KERNEL] Process 6: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O4/TU08.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O4/TU07.O

59.363 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000[DEBUG] Before eret, tf->regs[0] = 
1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000005BC01073742496
 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=68ms
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

59.563 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/O4/TU09.O
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=33ms

59.763 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
195136 pages=48 first=in elap=70ms
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

59.963 /SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O4v2 loader: image mapped (lazy), bytes=/195136TU10.O
 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=9ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=9ms

60.163 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

60.364 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB08 ELR = 
0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 48v2 loader: image mapped (lazy), bytes= first=58224in pages= elap=152 first=ms
in0x0000001000000AC8 elap=
1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

60.564 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 10: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

60.764 /SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

60.964 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O4/TU12.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

61.164 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.

61.364 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O4/TU11.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = /TOO0x0000001000004000
LS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O4/TU13.O

61.565 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000005BC0 pages=
7 ELR = 0x00000010000260F848
 first=in elap=81ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = [KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

61.765 [KERNEL] sys_fork: tf->regs[0] is now 4/
TOOLS/CP.BIN[DEBUG] Before eret, tf->regs[0] =  v2 loader: image mapped (lazy), bytes=/4S44832RC pages=/11BM/SR first=C/inTU elap=141.C ms
/ ELR = S0x00000010000260F8R
C/BM/O4/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

61.965 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = /5S ELR = R0x00000010000260F8C
/B[DEBUG] Before eret, tf->regs[0] = M92187[DEBUG] Before eret, tf->regs[0] = /B ELR = 9U0x0000001000000AB0RN.BI
N  ELR = 70x00000010000127200
0
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9218758224 ELR =  pages=0x0000001000000AC815
 first= ELR = in0x000000100000DDF8 elap=
81ms
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

62.165 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 107374249692187 ELR =  ELR = 0x0000001000000AC8
0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 71073742496
 ELR = 0x0000001000005BC0[DEBUG] Before eret, tf->regs[0] = 
7 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

62.365 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

62.766 [KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O4/TU15.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

62.966 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O4/TU16.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=21ms

63.166 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j42 /SRC/BM/O4 = 0

63.366 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

63.566 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

63.766 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

63.967 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

64.167 [KERNEL] sys_fork: tf->regs[0] is now 2
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

64.367 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2B ELR = M0x000000100000DDF8-
CMP TU12 = 0
v2 loade
64.367 r: image mapped (l
64.367 azy), 
64.368 bytes=
64.368 9553
64.368 6 page
64.368 s=24
64.368  first=
64.368 in el
64.368 ap=0ms
64.368 

64.568 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0

64.768 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
  PID  PPID  STATE  N[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
AME
   1     -1      READY      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[SELFHOST] phase=bm-j42-done rc=0
[SELFHOST] phase=bm-j81-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

64.968 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=0ms

65.169 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

65.369 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8/
SRC/BM/BURN.BIN 700

65.569 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136[DEBUG] Before eret, tf->regs[0] =  pages=9218748 ELR =  first=0x0000001000000AB0in elap=
21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 10
/[DEBUG] Before eret, tf->regs[0] = S10R ELR = C0x00000010000260F8/
BM/BURN.BIN 700

65.769 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

65.970 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14v2 loader: image mapped (lazy), bytes= ELR = 582240x000000100000DDF8 pages=
15 first=in elap=94ms
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720

66.170 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=/SRC/BM58224/ pages=B15U first=RinN elap=.BIN83 ms
700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1617 ELR =  ELR = 0x00000010000127200x0000001000012720

v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=2ms

66.370 [KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 58224 pages=157 first= ELR = in elap=0x0000001000012720
6ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8TOO
LS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O8/TU01.O

66.570 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=27ms
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=18195136
 pages=[DEBUG] Before eret, tf->regs[0] = 1848 ELR =  first=0x000000100000DDF8in
 elap=84ms
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x0000001000012720
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=12ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0

66.770 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8/T
OOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O8/TU03.O
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

66.970 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O8/TU02.O
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/O8/TU04.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000A
67.171 B0
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=11195136 ELR =  pages=0x000000100000DDF848
 first=in elap=68ms
v2 loader: image mapped (lazy), bytes=/44832SRC/BM/BUR pages=11N.BIN 700
 first=in elap=4ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 11 ELR = 921870x0000001000012720 ELR = 
0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

67.371 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.

[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0[DEBUG] Before eret, tf->regs[0] = 
92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = /0x00000010000260F8T
OOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O8/TU05.O
[KERNEL] Process 18: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

67.571 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
[KERNEL] Process 15: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O8/TU06.O
/TOOLS/CP.BIN /SRC/BM/SRC/[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
TU08.C /SRC/BM/O8/TU08.O

67.771 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7v2 loader: image mapped (lazy), bytes= ELR = 58224[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 pages=0x000000100000DDF815
 first=v2 loader: image mapped (lazy), bytes=in195136 pages=48 first= elap=inv2 loader: image mapped (lazy), bytes=73 elap=ms
1ms
44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 13v2 loader: image mapped (lazy), bytes=
195136[DEBUG] Before eret, tf->regs[0] =  pages=1348 ELR =  first=0x00000010000260F8in
 elap=101ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=97ms
[DEBUG] Before eret, tf->regs[0] = /1073742496T ELR = O0x0000001000004000O
LS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O8/TU07.O

67.971 [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=79ms
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720

68.172 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB092187
 ELR = 0x0000001000000AB0
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 448320x0000001000005BC0 pages=
1192187 first= ELR = in0x0000001000000AC8 elap=
12ms
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF8/SR92187C ELR = /0x0000001000000AB0B
M
/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms

68.372 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000005BC0 pages=
48 first=in elap=2ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 107374249692187 ELR =  ELR = 0x0000001000005BC00x0000001000000AC8

v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms

68.572 [KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 5 ELR = 160x0000001000012720
 ELR = [DEBUG] Before eret, tf->regs[0] = 10737424960x000000100000DDF8 ELR = 
0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

68.773 [DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
16[DEBUG] Before eret, tf->regs[0] =  ELR = 1073742496 ELR = 0x00000010000127200x0000001000005BC0

[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.

68.973 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=67ms
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=58224 pages=15 first=4in
/TOOLS/CP.BIN /SRC/BM/S elap=R[DEBUG] Before eret, tf->regs[0] = C17/ ELR = 150Tv2 loader: image mapped (lazy), bytes=U448320 pages=911. first=Cin  elap=/2Sms
Rms
0x000000100000DC00C
/BM/O8/TU09.O
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

69.173 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=10ms
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 7
92187[DEBUG] Before eret, tf->regs[0] =  ELR = 70x0000001000000AB0 ELR = 
0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x0000001000012720

69.373 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x0000001000012720 ELR = 
0x0000001000000AC8

69.574 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=44832[DEBUG] Before eret, tf->regs[0] =  pages=9218711 ELR =  first=0x0000001000000AC8in
 elap=139ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=15ms
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.

69.774 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 592187
[DEBUG] Before eret, tf->regs[0] =  ELR = 50x0000001000000AB0
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF81073742496 ELR = 
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x00000010000040000x0000001000000AC8

v2 loader: image mapped (lazy), bytes=/58224S pages=R15C first=/inBM elap=/1Bms
URN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8

69.974 [DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=23ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = /SRC/BM/BURN.BIN 700
0x00000010000260F8
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.

70.174 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=75ms
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

70.374 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O8/TU10.O
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.

70.575 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=108ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496v2 loader: image mapped (lazy), bytes= ELR = 58224 pages=0x000000100000400015
 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 107374249692187 ELR =  ELR = 0x0000001000005BC00x0000001000000AB0

/SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF892187
 ELR = 0x0000001000000AB0

70.775 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8v2 loader: image mapped (lazy), bytes=
58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 128 ELR =  ELR = 0x00000010000127200x0000001000012720

[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=4483258224 pages= pages=1115 first= first=inin elap= elap=762ms
ms
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1316 ELR =  ELR = 0x00000010000127200x0000001000012720

[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.

70.975 [KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O8/TU12.O

71.175 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=83ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O8/TU11.O
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x00000
71.175 010
71.175 00
71.175 00
71.175 0AB
71.375 0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 8: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/CP.BIN exited unexpectedly or gracefully.

71.576 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/T[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
U[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
14.C /SRC/BM/O8/T[KERNEL] Process 15: /TOOLS/SH.BIN exited unexpectedly or gracefully.
U[KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
14.O
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/O8/TU15.O
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=5195136 ELR = v2 loader: image mapped (lazy), bytes=0x00000010000260F8195136
 pages= pages=4848 first= first=inin elap= elap=1ms
75ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O8/TU13.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms

71.776 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 892187
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB0[DEBUG] Before eret, tf->regs[0] = 8
 ELR = 1073742496 ELR = 0x000000100000DDF8
0x0000001000004000
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 448328 pages= ELR = 110x0000001000012720 first=
in elap=2ms
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

71.976 [KERNEL] sys_fork: tf->regs[0] is now [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
7[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = /92187[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 ELR = 
0x0000001000000AC81073742496
[DEBUG] Before eret, tf->regs[0] =  ELR = 70x0000001000005BC0 ELR = T0x00000010000260F8O

OLS/CP.BIN /SRC/BM/SRC/TU16.[DEBUG] Before eret, tf->regs[0] = Cv2 loader: image mapped (lazy), bytes=7 /SRC195136/ ELR =  pages=0x00000010000260F848
 first=BM/O8/TU1in6 elap=.1O
ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720

72.176 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=944832
 pages=[DEBUG] Before eret, tf->regs[0] = 119 ELR = 0x000000100000DDF8 first=
in elap=67ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=2ms
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

72.377 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j81 /SRC/BM/O8 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = B2M ELR = -0x000000100000DDF8C
MP TU01 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

72.577 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

72.777 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

72.978 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

73.178 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

73.378 [KERNEL] sys_fork: tf->regs[0] is now 2
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
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

73.578 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

73.779 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
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

73.979 [KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
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

74.179 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 5
/[DEBUG] Before eret, tf->regs[0] = S5R ELR = C/0x00000010000260F8B
M/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=6195136
 pages=[DEBUG] Before eret, tf->regs[0] = 486 first= ELR = in0x00000010000260F8 elap=
71ms

74.379 /SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 7
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 195136 pages=48 first=7in ELR =  elap=0x000000100000DDF867
ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 87 ELR = 0x00000010000260F8
/ ELR = S0x0000001000012720RC/BM/BURN.BI
N 700

74.579 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=9v2 loader: image mapped (lazy), bytes=195136
 pages=48[DEBUG] Before eret, tf->regs[0] =  first=9in ELR =  elap=0x00000010000260F877
ms
/58224S pages=R15C first=/inB elap=M/BURN.86ms
BIN 700
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

74.780 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1092187
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000000AB010
 ELR = 0x000000100000DDF8[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 
9218792187 ELR =  ELR = 0x0000001000000AB00x0000001000000AC8

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF810 ELR = 
0x0000001000012720
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 5822411 pages=15 ELR =  first=0x0000001000012720in
 elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1258224
 pages=[DEBUG] Before eret, tf->regs[0] = 1512 ELR =  first=0x00000010000260F8/inS elap=R85Cms
/
BM/BURN.BIN 700

74.980 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = v2 loader: image mapped (lazy), bytes=0x00000010000260F8/195136SR pages=C48/ first=BinM elap=/B108Ums
RN.BIN 700

v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=2ms

75.180 [KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

75.380 [KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1614
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100001272016/ ELR = S0x00000010000260F8

RC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 480x0000001000000AB0 first=
in elap=3ms
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AC80x0000001000000AC8

[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 7 ELR = 582240x0000001000012720 pages=
15v2 loader: image mapped (lazy), bytes= first=58224in pages= elap=1582 first=ms
in elap=2ms

75.581 [KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=21ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/O8/TU01.O

75.781 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
/TOOLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/O8/TU03.O

75.981 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8[DEBUG] Before eret, tf->regs[0] = 
[DEBUG] Before eret, tf->regs[0] = /107374249610T ELR = v2 loader: image mapped (lazy), bytes=O58224O pages=L0x0000001000012720S15/
C first=P. ELR = BinI elap=Nv2 loader: image mapped (lazy), bytes= 4/195136Sms
R pages=C48/ first=BinM elap=/1Sms
R0x0000001000005BC0C
/TU04.C /SRC/BM/O8/TU04.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=18ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000

76.181 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=107374249644832[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 pages= ELR = 110x0000001000005BC0 first=[DEBUG] Before eret, tf->regs[0] = in11 elap= ELR = 40x0000001000012720ms


/TOOLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/O8/TU02.O
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8

76.382 [KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] =  ELR = 1073742496 ELR = 0x00000010000260F80x0000001000004000


76.582 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8/
SRC/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [KERNEL] Process 7: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
44832 pages=11 first=in elap=1ms
[KERNEL] Process 17: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8

76.782 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=78ms
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/O8/TU05.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/O8/TU06.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/O8/TU07.O
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 16: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 12 ELR = v2 loader: image mapped (lazy), bytes=0x00000010000260F8
195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10v2 loader: image mapped (lazy), bytes= ELR = 0x00000010000260F8195136
 pages=48 first=in elap=78ms

76.982 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=44ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/O8/TU08.O
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=13195136
 pages=[DEBUG] Before eret, tf->regs[0] = 4813 first=in elap= ELR = 730x000000100000DDF8ms


77.183 [DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1344832 ELR =  pages=110x0000001000012720 first=
in elap=1ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8

77.383 /SRC/BM/BURN.BIN 700
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = [DEBUG] Before eret, tf->regs[0] = 0x0000001000005BC092187 ELR = 
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AC80x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1192187 ELR = 0x000000100000DDF8 ELR = 
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

77.583 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=44832 pages=11[DEBUG] Before eret, tf->regs[0] =  first=13in ELR = 0x0000001000012720 elap=[DEBUG] Before eret, tf->regs[0] = 292187 ELR = ms
0x0000001000000AB0

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
 ELR = 0x0000001000000AC8
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

77.783 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=5
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 5448320x0000001000004000 ELR = 
0x000000100000DDF8
 pages=11 first=in elap=78ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in[DEBUG] Before eret, tf->regs[0] =  elap=10737424962 ELR = ms
0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 51073742496 ELR =  ELR = 0x0000001000012720
 ELR = 0x0000001000005BC0
195136 pages=480x0000001000005BC0 first=
in elap=23ms
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

77.984 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1412
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100001272014
 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720

78.184 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1544832
 pages=[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1511 ELR =  first=0x000000100000DDF8in
 elap=19513690 pages=48 first=inms
 elap=90ms
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700

78.384 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

78.584 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

78.785 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=38ms
[KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEB
78.785 UG] Before 
78.785 eret
78.785 , t
78.985 f->regs[0] = 5 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8[DEBUG] Before eret, tf->regs[0] =  ELR = 50x00000010000260F8
 ELR = /0x0000001000012720TOOLS/CP.BIN /SRC/BM
/SRC/TU09.C /SRC/BM/O8/TU09.O
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8

79.185 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=/inS elap=R1Cms
/BM/BURN.BIN 700
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=30ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 19513692187 pages= ELR = 480x0000001000000AC8
 first=in elap=31ms
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6/ ELR = T0x00000010000260F8OOLS/
CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/O8/TU10.O

79.385 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in/ elap=S13Rms
C/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms

79.586 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 15 ELR = [KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x0000001000012720
[KERNEL] Process 16: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1358224
 pages=[DEBUG] Before eret, tf->regs[0] = 15 first=13 ELR = in elap=0x000000100000DDF8[DEBUG] Before eret, tf->regs[0] = 
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=8392187ms
 ELR = 582240x0000001000000AB0 pages=5822415
 first=in elap= pages=1[DEBUG] Before eret, tf->regs[0] = ms
1315 ELR = 0x0000001000012720[DEBUG] Before eret, tf->regs[0] =  first=92187
in ELR =  elap=0x0000001000000AC85
ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0[DEBUG] Before eret, tf->regs[0] = 
92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

79.786 [KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17/ ELR = T0x000000100000DDF8O
OLS/CP.BIN /SRC/BM/SRC/TU13.C /SRC/BM/O8/TU13.O
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=4ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x0000001000012720
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

79.986 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/O8/TU11.O
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720

80.186 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 16: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.

80.386 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/O8/TU14.O
[KERNEL] Process 17: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x000000100000DDF8
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 1951367[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
 pages=v2 loader: image mapped (lazy), bytes=4844832 first= pages=in11 elap= first=31in[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 elap=ms
 ELR = [KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.
0x00000010000127202
ms
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/O8/TU16.O

80.587 [KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/O8/TU12.O
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.

80.787 [KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU15.C v2 loader: image mapped (lazy), bytes=/195136S pages=R48C first=/inB elap=M/O8/TU15.O
85ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1073742496195136 ELR =  pages=0x0000001000005BC048
 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 11v2 loader: image mapped (lazy), bytes=195136 pages=
48 first=in[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] =  elap=1183 ELR = ms
0x000000100000DDF892187 ELR = 
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=9218744832 ELR =  pages=0x0000001000000AB011
 first=in elap=2ms
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 921871073742496 ELR =  ELR = 0x0000001000000AC80x0000001000004000

[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0[DEBUG] Before eret, tf->regs[0] = 
11[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x0000001000012720
 ELR = 0x0000001000000AC8

80.987 [KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 121073742496 ELR =  ELR = 0x00000010000040000x000000100000DDF8

[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 121073742496 ELR =  ELR = 0x00000010000127200x0000001000005BC0

[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=12ms
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 7
1073742496[DEBUG] Before eret, tf->regs[0] =  ELR = 0x00000010000040007
 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 7v2 loader: image mapped (lazy), bytes=44832 ELR =  pages=0x000000100001272011
 first=[DEBUG] Before eret, tf->regs[0] = in1073742496 elap= ELR = 10x0000001000005BC0ms


81.188 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=44832 pages=811 first=in elap=0ms
 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8

81.388 [DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=10ms
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.

81.589 [KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT j82 /SRC/BM/O8 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0

81.789 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8

81.989 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms

82.189 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

82.390 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

82.590 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0

82.791 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 0

82.991 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 0

83.191 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=0ms
  PID  PPID  STATE  NAME
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
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

83.391 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

84.192 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/TOOLS/CP.BIN /SRC/BM/SRC/TU01.C /SRC/BM/OF/TU01.O
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

84.392 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

84.592 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

85.193 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

85.393 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/OF/TU02.O
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

85.594 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = S0x00000010000260F8R
C/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

86.395 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU03.C /SRC/BM/OF/TU03.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

86.595 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

86.795 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

87.396 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

87.596 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8/
TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/OF/TU04.O
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

87.796 [KERNEL] sys_fork: tf->regs[0] is now /4S
R[DEBUG] Before eret, tf->regs[0] = C4/ ELR = B0x00000010000260F8M
/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720

88.597 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU05.C /SRC/BM/OF/TU05.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

88.797 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700

88.997 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

89.598 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

89.798 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU06.C /SRC/BM/OF/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

89.998 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
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
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

90.799 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/OF/TU07.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

90.999 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /S4R ELR = C0x00000010000260F8/
BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

91.200 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

91.800 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

92.001 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/OF/TU08.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

92.201 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
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

93.002 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /TOOLS/CP.B0x00000010000260F8I
N /SRC/BM/SRC/TU09.C /SRC/BM/OF/TU09.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

93.202 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8

93.402 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

94.203 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/OF/TU10.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms

94.403 [KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

94.603 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

95.204 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

95.404 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU11.C /SRC/BM/OF/TU11.O
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

95.604 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

96.405 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/ ELR = T0x00000010000260F8O
OLS/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/OF/TU12.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

96.605 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 5: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4/SRC/B ELR = M0x00000010000260F8/
BURN.BIN 300
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=0ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

96.806 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=0ms

97.006 [KERNEL] Process 5: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

97.206 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = e4x ELR = i0x00000010000260F8t
 9
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
MAKE.BIN: *** [/SRC/BM/MKFAIL.MK:76: /SRC/BM/OF/TU13.O] Error 9
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.
BM-EXIT failser /SRC/BM/OF = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0

97.406 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.

97.607 [KERNEL] sys_fork: tf->regs[0] is now 2
B[DEBUG] Before eret, tf->regs[0] = M2- ELR = C0x000000100000DDF8M
P TU03 = 0
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0

97.807 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0

98.007 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0

98.207 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU11 = 0

98.408 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU12 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU13 = 2

98.608 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU14 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU15 = 2

98.808 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU16 = 2
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=44944 pages=11 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/PS.BIN exited unexpectedly or gracefully.
  PID  PPID  STATE  NAME
   1     -1      BLOCKED      SELFHOST.BIN
   2     3      RUNNING      /TOOLS/PS.BIN
   3     1      OTHER      /TOOLS/SH.BIN
[KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
BM-UNEXPECTED-FAIL failser rc=2
[SELFHOST] phase=bm-failser-done rc=0
[SELFHOST] phase=bm-failj8-start
Loading program for scheduler (v2 AS): /TOOLS/SH.BIN
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: PID=3 entry=0x0000001000000000 sp=0x0000001800000000 resident=1 tables=3
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

99.009 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=314680 pages=77 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = /0x00000010000260F8S
RC/BM/BURN.BIN 700

99.209 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 6
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 195136/6 pages=S ELR = R480x00000010000260F8C
 first=/inB elap=M81/ms
BURN.BIN 700

99.409 [KERNEL] sys_fork: tf->regs[0] is now 7v2 loader: image mapped (lazy), bytes=
195136[DEBUG] Before eret, tf->regs[0] =  pages=748 ELR =  first=0x00000010000260F8in
 elap=71ms
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136v2 loader: image mapped (lazy), bytes= pages=19513648 pages= first=48in first= elap=in1 elap=ms
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=58224195136 pages= pages=1548 first= first=inin elap= elap=10ms
ms

99.609 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
/SRC/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8

99.810 [KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=11195136
 pages=[DEBUG] Before eret, tf->regs[0] = 4811 ELR = [DEBUG] Before eret, tf->regs[0] = 0x000000100000DDF892187 first=
in elap=67 ELR = ms
0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 11[DEBUG] Before eret, tf->regs[0] =  ELR = 921870x0000001000012720 ELR = 
0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = /13S ELR = R0x00000010000260F8C
/BM/BURN.BIN 700
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x0000001000012720

99.810 v2 lo
99.810 ader
99.810 : im
99.810 age
99.810  m
99.810 app
99.810 ed 
99.810 (la
99.810 z
100.010 y), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=105ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=2ms

100.210 [KERNEL] sys_fork: tf->regs[0] is now 15[DEBUG] Before eret, tf->regs[0] = 
14[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x00000010000260F8
 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

100.410 [KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = /4T ELR = O0x000000100000DDF8O
LS/CPv2 loader: image mapped (lazy), bytes=.B58224I[DEBUG] Before eret, tf->regs[0] =  pages=9N /15S first=RinC/BM/ elap=S83Rms
C ELR = /0x0000001000012720
TU01.C /SRC/BM/OF2/TU01.O

100.611 [KERNEL] sys_fork: tf->regs[0] is now 17
[DEBUG] Before eret, tf->regs[0] = 17 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720
[KERNEL] sys_fork: tf->regs[0] is now 18
[DEBUG] Before eret, tf->regs[0] = 18 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=93ms
[KERNEL] Process 12: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 18 ELR = [KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=22ms
[KERNEL] Process 11: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.

100.811 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=102ms
/TOOL[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
S/CP.BIN /SRC/BM/SRC/Tv2 loader: image mapped (lazy), bytes=U05.C195136  pages=/48SRC/ first=BinM elap=/3Oms
F2/TU05.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8

101.011 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
/TOOLS/CP.BIN /SRC/BM/SRC/TU04.C /SRC/BM/OF2/TU04.O
[KERNEL] Process 14: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 107374249610 ELR = 
0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = /10T ELR = O0x00000010000260F8OLS/C
P.BIN /Sv2 loader: image mapped (lazy), bytes=R195136C pages=[DEBUG] Before eret, tf->regs[0] = /1073742496B ELR = M0x0000001000005BC0/
S48R first=Cin/ elap=T2Ums
06.C /SRC/BM/OF2/TU06.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

101.212 [KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 17: /TOOLS/SH.BIN exited unexpectedly or gracefully.
1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1144832 ELR =  pages=0x000000100001272011
 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
/SRC/BM/BURN.BIN 700

101.412 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=11ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218714 ELR =  ELR = 0x0000001000000AC80x0000001000012720v2 loader: image mapped (lazy), bytes=

44832 pages=[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
11 first=in elap=[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
12ms
[KERNEL] Process 13: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0

101.612 [KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9[KERNEL] Process 4: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
 ELR = [KERNEL] Process 18: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x000000100000DDF8[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

v2 loader: image mapped (lazy), bytes=44832 pages=11[KERNEL] Process 15: /TOOLS/SH.BIN exited unexpectedly or gracefully.
 first=[DEBUG] Before eret, tf->regs[0] = in9 elap= ELR = 10x0000001000012720ms

[KERNEL] Process 9: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = [KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720

101.812 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5/T ELR = OOL0x00000010000260F8S
/CP.BIN /SRC/BM/SRC/TU02.C /SRC/BM/OF2/TU02.O
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x00000010000260F8

102.013 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms

102.213 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=/TOOLS/CP.BIN /SRC/BM/SRC195136/ pages=T48U03.C /SRC/BM/OF2/TU03.O
/TOOLS/CP.BIN /SRC/BM/SRC/TU07.C /SRC/BM/OF2/TU07. first=Oin
/TOOLS/CP.BIN /SRC/BM/SRC/TU08.C /SRC/BM/OF2/TU08.O
/SRC/BM/BURN.BIN 700
/SRC/BM/BURN.BI elap=55ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=35ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=28ms
N 700
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.

102.413 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=40ms
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=51ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=143ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 1392187
 ELR = [DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF80x0000001000000AC8


102.613 [KERNEL] sys_fork: tf->regs[0] is now 7
/[DEBUG] Before eret, tf->regs[0] = S92187R[DEBUG] Before eret, tf->regs[0] = C ELR = /0x0000001000000AB0B
v2 loader: image mapped (lazy), bytes=M44832/ pages=B11U first=RinN elap=.2Bms
IN 7[DEBUG] Before eret, tf->regs[0] = 792187 ELR =  ELR = 0x00000010000260F80x0000001000000AB0

00
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 1073742496[DEBUG] Before eret, tf->regs[0] = 921871073742496 ELR =  ELR = 0x0000001000000AC80x0000001000004000

 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 92187v2 loader: image mapped (lazy), bytes= ELR = 1951360x0000001000000AC8 pages=
481073742496 first= ELR = in0x0000001000005BC0 elap=
3ms
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 14 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

102.814 [KERNEL] sys_fork: tf->regs[0] is now 5
/[DEBUG] Before eret, tf->regs[0] = S5RC ELR = /BM0x00000010000260F8/
BURN.v2 loader: image mapped (lazy), bytes=B44832IN 300
 pages=11 first=in elap=2ms
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8

103.014 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=34ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = v2 loader: image mapped (lazy), bytes=1344832 ELR =  pages=0x000000100001272011
 first=in elap=44ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8

103.214 [DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[KERNEL] Process 4: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x00000010000260F8

103.415 [DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x0000001000012720/
TOOLS/CP.BIN /SRC/BM/SRC/TU09.C /SRC/BM/OF2/TU09.O
[KERNEL] sys_fork: tf->regs[0] is now 12
[DEBUG] Before eret, tf->regs[0] = 12 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=106ms

103.615 [KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8
[KERNEL] sys_fork: tf->regs[0] is now v2 loader: image mapped (lazy), bytes=1658224
 pages=15 first=in elap=[DEBUG] Before eret, tf->regs[0] = 1664 ELR = ms
0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720
/SRC/BM/BURN.BIN 700
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=0ms
[KERNEL] Process 14: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=45ms
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x0000001000012720

103.815 v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=121ms
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
/SRC/BM/BURN.BIN 700

104.015 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x00000010000260F8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[KERNEL] sys_fork: tf->regs[0] is now 14
[DEBUG] Before eret, tf->regs[0] = 14[DEBUG] Before eret, tf->regs[0] =  ELR = 6 ELR = 0x0000001000012720
0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = /92187S ELR = R0x0000001000000AC8C
/BM/BURN.BIN 700
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=[DEBUG] Before eret, tf->regs[0] = 19513616 pages= ELR = 480x0000001000012720 first=
inv2 loader: image mapped (lazy), bytes= elap=5822468 pages=ms
15 first=in elap=2ms
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

104.216 [DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 9218792187 ELR =  ELR = 0x0000001000000AB00x0000001000000AC8

[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[KERNEL] Process 16: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 16
[DEBUG] Before eret, tf->regs[0] = 16 ELR = 0x000000100000DDF8

104.416 v2 loader: image mapped (lazy), bytes=58224 pages=15 first=in elap=1ms
[KERNEL] Process 4: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 10: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now [DEBUG] Before eret, tf->regs[0] = 416
 ELR = [DEBUG] Before eret, tf->regs[0] = 0x00000010000127204
 ELR = /0x00000010000260F8T
OOLS/CP.BIN /SRC/BM/SRC/TU10.C /SRC/BM/OF2/TU10.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[KERNEL] sys_fork: tf->regs[0] is now 9
[DEBUG] Before eret, tf->regs[0] = 9 ELR = 0x000000100000DDF8
[KERNEL] Process 6: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.

104.616 [KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
[KERNEL] Process 15: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=e195136x pages=i48t first= in9 elap=
1ms
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6[DEBUG] Before eret, tf->regs[0] =  ELR = 90x00000010000260F8
 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=58224 pages=/15T first=OinO elap=L2Sms
/CP.BIN /SRC/BM/SRC/TU12.C /SRC/BM/OF2/TU12.O
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = [KERNEL] Process 13: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
0x0000001000000AB0
[KERNEL] Process 11: /TOOLS/SH.BIN exited unexpectedly or gracefully.

104.817 [KERNEL] sys_fork: tf->regs[0] is now 7
[DEBUG] Before eret, tf->regs[0] = 7 ELR = 0x00000010000260F8
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 92187/ ELR = T0x0000001000000AC892187
OOLS/ ELR = CP0x0000001000000AB0.
BIN /SRC/BM/SRC/TU11.C /SRC/BM/OF2/TU11.O
[KERNEL] sys_fork: tf->regs[0] is now 10
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 10 ELR = 0x0000001000012720
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=21ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8

105.017 [KERNEL] sys_fork: tf->regs[0] is now 11
[DEBUG] Before eret, tf->regs[0] = [DEBUG] Before eret, tf->regs[0] = 107374249611 ELR = v2 loader: image mapped (lazy), bytes=0x0000001000005BC058224
[DEBUG] Before eret, tf->regs[0] =  pages=v2 loader: image mapped (lazy), bytes=107374249615 ELR = 448320x0000001000004000 ELR = 
 first= pages=in11 elap= first=83inms
 elap=0x000000100000DDF82[DEBUG] Before eret, tf->regs[0] = ms
1073742496
 ELR = 0x0000001000005BC0
[DEBUG] Before eret, tf->regs[0] = 11 ELR = 0x0000001000012720
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
MAKE.BIN: *** [/SRC/BM/MKFAIL.MK:76: /SRC/BM/OF2/TU13.O] Error 9
MAKE.BIN: *** Waiting for unfinished jobs....
[KERNEL] Process 16: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 8: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = /5T ELR = O0x00000010000260F8O
LS/CP.BIN /SRC/BM/SRC/TU15.C /SRC/BM/OF2/TU15.O

105.217 [KERNEL] sys_fork: tf->regs[0] is now 8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 8 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=v2 loader: image mapped (lazy), bytes=44832195136 pages= pages=11 first=48in first= elap=in1 elap=ms
1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 13
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x000000100000DDF8

105.417 [KERNEL] sys_fork: tf->regs[0] is now 15
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 13 ELR = 0x0000001000012720v2 loader: image mapped (lazy), bytes=
44832 pages=11 first=in elap=1ms
[KERNEL] Process 15: /TOOLS/CP.BIN exited unexpectedly or gracefully.
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms
[KERNEL] Process 13: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 11: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 7: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[DEBUG] Before eret, tf->regs[0] = 15 ELR = 0x0000001000012720
[KERNEL] Process 4: /TOOLS/SH.BIN exited unexpectedly or gracefully.

105.618 [KERNEL] Process 8: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 6: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] Process 9: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 14: /TOOLS/SH.BIN exited unexpectedly or gracefully.

105.818 [KERNEL] sys_fork: tf->regs[0] is now 4
[DEBUG] Before eret, tf->regs[0] = 4 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU16.C /SRC/BM/OF2/TU16.O
[KERNEL] Process 10: /SRC/BM/BURN.BIN exited unexpectedly or gracefully.
[KERNEL] Process 12: /TOOLS/SH.BIN exited unexpectedly or gracefully.
[KERNEL] sys_fork: tf->regs[0] is now 5
[DEBUG] Before eret, tf->regs[0] = 5 ELR = 0x00000010000260F8
/TOOLS/CP.BIN /SRC/BM/SRC/TU14.C /SRC/BM/OF2/TU14.O
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
v2 loader: image mapped (lazy), bytes=195136 pages=48 first=in elap=1ms
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AB0
[DEBUG] Before eret, tf->regs[0] = 92187 ELR = 0x0000001000000AC8
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000004000
[DEBUG] Before eret, tf->regs[0] = 1073742496 ELR = 0x0000001000005BC0
[KERNEL] sys_fork: tf->regs[0] is now 6
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x000000100000DDF8
[DEBUG] Before eret, tf->regs[0] = 6 ELR = 0x0000001000012720
v2 loader: image mapped (lazy), bytes=44832 pages=11 first=in elap=1ms

106.018 [KERNEL] Process 6: /TOOLS/CP.BIN exited unexpectedly or gracefully.
[KERNEL] Process 5: /TOOLS/SH.BIN exited unexpectedly or gracefully.
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
[KERNEL] Process 2: /TOOLS/MAKE.BIN exited unexpectedly or gracefully.

106.218 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2B ELR = M0x000000100000DDF8-
EXIT failj8 /SRC/BM/OF2 = 2
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU01 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU02 = 0

106.419 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU03 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU04 = 0

106.619 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU05 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU06 = 0

106.819 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU07 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU08 = 0

107.019 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU09 = 0
[KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=1ms
[KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
BM-CMP TU10 = 0

107.220 [KERNEL] sys_fork: tf->regs[0] is now 2
[DEBUG] Before eret, tf->regs[0] = 2 ELR = 0x000000100000DDF8
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

107.420 v2 loader: image mapped (lazy), bytes=95536 pages=24 first=in elap=0ms
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

107.620 [KERNEL] Process 2: /TOOLS/CMP.BIN exited unexpectedly or gracefully.
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

107.820 [KERNEL] Process 3: /TOOLS/SH.BIN exited unexpectedly or gracefully.
BM-UNEXPECTED-FAIL failj8 rc=2
[SELFHOST] phase=bm-failj8-done rc=0
[SELFHOST] ACCEPTANCE-COMPLETE stages=8
[KERNEL] Process 1: SELFHOST.BIN exited unexpectedly or gracefully.
System halt from CPU 7.

