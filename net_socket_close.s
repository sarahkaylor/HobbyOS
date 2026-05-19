00000000400870e0 <net_socket_close>:
400870e0: d100c3ff     	sub	sp, sp, #0x30
400870e4: a9027bfd     	stp	x29, x30, [sp, #0x20]
400870e8: 910083fd     	add	x29, sp, #0x20
400870ec: f81f83a0     	stur	x0, [x29, #-0x8]
400870f0: f85f83a8     	ldur	x8, [x29, #-0x8]
400870f4: b5000068     	cbnz	x8, 0x40087100 <net_socket_close+0x20>
400870f8: 14000001     	b	0x400870fc <net_socket_close+0x1c>
400870fc: 14000021     	b	0x40087180 <net_socket_close+0xa0>
40087100: f85f83a8     	ldur	x8, [x29, #-0x8]
40087104: b9400508     	ldr	w8, [x8, #0x4]
40087108: 71001908     	subs	w8, w8, #0x6
4008710c: 54000201     	b.ne	0x4008714c <net_socket_close+0x6c>
40087110: 14000001     	b	0x40087114 <net_socket_close+0x34>
40087114: f85f83a8     	ldur	x8, [x29, #-0x8]
40087118: b9401908     	ldr	w8, [x8, #0x18]
4008711c: 71000908     	subs	w8, w8, #0x2
40087120: 54000161     	b.ne	0x4008714c <net_socket_close+0x6c>
40087124: 14000001     	b	0x40087128 <net_socket_close+0x48>
40087128: f85f83a9     	ldur	x9, [x29, #-0x8]
4008712c: 52800068     	mov	w8, #0x3                // =3
40087130: b9001928     	str	w8, [x9, #0x18]
40087134: f85f83a0     	ldur	x0, [x29, #-0x8]
40087138: 52800221     	mov	w1, #0x11               // =17
4008713c: aa1f03e2     	mov	x2, xzr
40087140: 2a1f03e3     	mov	w3, wzr
40087144: 97fffcbb     	bl	0x40086430 <send_tcp_segment>
40087148: 14000001     	b	0x4008714c <net_socket_close+0x6c>
4008714c: d0000c20     	adrp	x0, 0x4020d000 <net_lock>
40087150: 91000000     	add	x0, x0, #0x0
40087154: f90007e0     	str	x0, [sp, #0x8]
40087158: 97fff52a     	bl	0x40084600 <spinlock_acquire_irqsave>
4008715c: aa0003e8     	mov	x8, x0
40087160: f94007e0     	ldr	x0, [sp, #0x8]
40087164: f9000be8     	str	x8, [sp, #0x10]
40087168: f85f83a9     	ldur	x9, [x29, #-0x8]
4008716c: 2a1f03e8     	mov	w8, wzr
40087170: b9000128     	str	w8, [x9]
40087174: f9400be1     	ldr	x1, [sp, #0x10]
40087178: 97fff532     	bl	0x40084640 <spinlock_release_irqrestore>
4008717c: 14000001     	b	0x40087180 <net_socket_close+0xa0>
40087180: a9427bfd     	ldp	x29, x30, [sp, #0x20]
40087184: 9100c3ff     	add	sp, sp, #0x30
40087188: d65f03c0     	ret
4008718c: d503201f     	nop

0000000040087190 <net_arp_request>:
40087190: d10183ff     	sub	sp, sp, #0x60
40087194: a9057bfd     	stp	x29, x30, [sp, #0x50]
40087198: 910143fd     	add	x29, sp, #0x50
4008719c: b81fc3a0     	stur	w0, [x29, #-0x4]
400871a0: 91008be8     	add	x8, sp, #0x22
400871a4: f9000fe8     	str	x8, [sp, #0x18]
400871a8: 91003908     	add	x8, x8, #0xe
400871ac: f9000be8     	str	x8, [sp, #0x10]
400871b0: 2a1f03e8     	mov	w8, wzr
400871b4: b9000fe8     	str	w8, [sp, #0xc]
400871b8: 14000001     	b	0x400871bc <net_arp_request+0x2c>
400871bc: b9400fe8     	ldr	w8, [sp, #0xc]
400871c0: 71001508     	subs	w8, w8, #0x5
400871c4: 5400024c     	b.gt	0x4008720c <net_arp_request+0x7c>
400871c8: 14000001     	b	0x400871cc <net_arp_request+0x3c>
400871cc: f9400fe9     	ldr	x9, [sp, #0x18]
400871d0: b9800fea     	ldrsw	x10, [sp, #0xc]
400871d4: 52801fe8     	mov	w8, #0xff               // =255
400871d8: 382a6928     	strb	w8, [x9, x10]
400871dc: b9800fea     	ldrsw	x10, [sp, #0xc]
400871e0: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
400871e4: 910f1108     	add	x8, x8, #0x3c4
400871e8: 386a6908     	ldrb	w8, [x8, x10]
400871ec: f9400fe9     	ldr	x9, [sp, #0x18]
400871f0: 8b0a0129     	add	x9, x9, x10
400871f4: 39001928     	strb	w8, [x9, #0x6]
400871f8: 14000001     	b	0x400871fc <net_arp_request+0x6c>
400871fc: b9400fe8     	ldr	w8, [sp, #0xc]
40087200: 11000508     	add	w8, w8, #0x1
40087204: b9000fe8     	str	w8, [sp, #0xc]
40087208: 17ffffed     	b	0x400871bc <net_arp_request+0x2c>
4008720c: 528100c0     	mov	w0, #0x806              // =2054
40087210: 97fff800     	bl	0x40085210 <htons>
40087214: f9400fe8     	ldr	x8, [sp, #0x18]
40087218: 79001900     	strh	w0, [x8, #0xc]
4008721c: 52800020     	mov	w0, #0x1                // =1
40087220: b90007e0     	str	w0, [sp, #0x4]
40087224: 97fff7fb     	bl	0x40085210 <htons>
40087228: f9400be8     	ldr	x8, [sp, #0x10]
4008722c: 79000100     	strh	w0, [x8]
40087230: 52810000     	mov	w0, #0x800              // =2048
40087234: 97fff7f7     	bl	0x40085210 <htons>
40087238: 2a0003e8     	mov	w8, w0
4008723c: b94007e0     	ldr	w0, [sp, #0x4]
40087240: f9400be9     	ldr	x9, [sp, #0x10]
40087244: 79000528     	strh	w8, [x9, #0x2]
40087248: f9400be9     	ldr	x9, [sp, #0x10]
4008724c: 528000c8     	mov	w8, #0x6                // =6
40087250: 39001128     	strb	w8, [x9, #0x4]
40087254: f9400be9     	ldr	x9, [sp, #0x10]
40087258: 52800088     	mov	w8, #0x4                // =4
4008725c: 39001528     	strb	w8, [x9, #0x5]
40087260: 97fff7ec     	bl	0x40085210 <htons>
40087264: f9400be8     	ldr	x8, [sp, #0x10]
40087268: 79000d00     	strh	w0, [x8, #0x6]
4008726c: 2a1f03e8     	mov	w8, wzr
40087270: b9000be8     	str	w8, [sp, #0x8]
40087274: 14000001     	b	0x40087278 <net_arp_request+0xe8>
40087278: b9400be8     	ldr	w8, [sp, #0x8]
4008727c: 71001508     	subs	w8, w8, #0x5
40087280: 5400026c     	b.gt	0x400872cc <net_arp_request+0x13c>
40087284: 14000001     	b	0x40087288 <net_arp_request+0xf8>
40087288: b9800bea     	ldrsw	x10, [sp, #0x8]
4008728c: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
40087290: 910f1108     	add	x8, x8, #0x3c4
40087294: 386a6908     	ldrb	w8, [x8, x10]
40087298: f9400be9     	ldr	x9, [sp, #0x10]
4008729c: 8b0a0129     	add	x9, x9, x10
400872a0: 39002128     	strb	w8, [x9, #0x8]
400872a4: f9400be8     	ldr	x8, [sp, #0x10]
400872a8: b9800be9     	ldrsw	x9, [sp, #0x8]
400872ac: 8b090109     	add	x9, x8, x9
400872b0: 2a1f03e8     	mov	w8, wzr
400872b4: 39004928     	strb	w8, [x9, #0x12]
400872b8: 14000001     	b	0x400872bc <net_arp_request+0x12c>
400872bc: b9400be8     	ldr	w8, [sp, #0x8]
400872c0: 11000508     	add	w8, w8, #0x1
400872c4: b9000be8     	str	w8, [sp, #0x8]
400872c8: 17ffffec     	b	0x40087278 <net_arp_request+0xe8>
400872cc: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
400872d0: b943cd08     	ldr	w8, [x8, #0x3cc]
400872d4: f9400be9     	ldr	x9, [sp, #0x10]
400872d8: b800e128     	stur	w8, [x9, #0xe]
400872dc: b85fc3a8     	ldur	w8, [x29, #-0x4]
400872e0: f9400be9     	ldr	x9, [sp, #0x10]
400872e4: b9001928     	str	w8, [x9, #0x18]
400872e8: 91008be0     	add	x0, sp, #0x22
400872ec: 52800541     	mov	w1, #0x2a               // =42
400872f0: 94001720     	bl	0x4008cf70 <virtio_net_send>
400872f4: a9457bfd     	ldp	x29, x30, [sp, #0x50]
400872f8: 910183ff     	add	sp, sp, #0x60
400872fc: d65f03c0     	ret

0000000040087300 <arp_cache_update>:
40087300: d10083ff     	sub	sp, sp, #0x20
40087304: b9001fe0     	str	w0, [sp, #0x1c]
40087308: f9000be1     	str	x1, [sp, #0x10]
4008730c: 2a1f03e8     	mov	w8, wzr
40087310: b9000fe8     	str	w8, [sp, #0xc]
40087314: 14000001     	b	0x40087318 <arp_cache_update+0x18>
40087318: b9400fe8     	ldr	w8, [sp, #0xc]
4008731c: 71003d08     	subs	w8, w8, #0xf
40087320: 540005ac     	b.gt	0x400873d4 <arp_cache_update+0xd4>
40087324: 14000001     	b	0x40087328 <arp_cache_update+0x28>
40087328: b9800fe9     	ldrsw	x9, [sp, #0xc]
4008732c: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
40087330: 910b1108     	add	x8, x8, #0x2c4
40087334: 8b091108     	add	x8, x8, x9, lsl #4
40087338: b9400d08     	ldr	w8, [x8, #0xc]
4008733c: 34000428     	cbz	w8, 0x400873c0 <arp_cache_update+0xc0>
40087340: 14000001     	b	0x40087344 <arp_cache_update+0x44>
40087344: b9800fe8     	ldrsw	x8, [sp, #0xc]
40087348: d37ced09     	lsl	x9, x8, #4
4008734c: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
40087350: 910b1108     	add	x8, x8, #0x2c4
40087354: b8696908     	ldr	w8, [x8, x9]
40087358: b9401fe9     	ldr	w9, [sp, #0x1c]
4008735c: 6b090108     	subs	w8, w8, w9
40087360: 54000301     	b.ne	0x400873c0 <arp_cache_update+0xc0>
40087364: 14000001     	b	0x40087368 <arp_cache_update+0x68>
40087368: 2a1f03e8     	mov	w8, wzr
4008736c: b9000be8     	str	w8, [sp, #0x8]
40087370: 14000001     	b	0x40087374 <arp_cache_update+0x74>
40087374: b9400be8     	ldr	w8, [sp, #0x8]
40087378: 71001508     	subs	w8, w8, #0x5
4008737c: 5400020c     	b.gt	0x400873bc <arp_cache_update+0xbc>
40087380: 14000001     	b	0x40087384 <arp_cache_update+0x84>
40087384: f9400be8     	ldr	x8, [sp, #0x10]
40087388: b9800bea     	ldrsw	x10, [sp, #0x8]
4008738c: 386a6908     	ldrb	w8, [x8, x10]
40087390: b9800feb     	ldrsw	x11, [sp, #0xc]
40087394: d0000c69     	adrp	x9, 0x40215000 <sockets+0x7ffc>
40087398: 910b1129     	add	x9, x9, #0x2c4
4008739c: 8b0b1129     	add	x9, x9, x11, lsl #4
400873a0: 8b0a0129     	add	x9, x9, x10
400873a4: 39001128     	strb	w8, [x9, #0x4]
400873a8: 14000001     	b	0x400873ac <arp_cache_update+0xac>
400873ac: b9400be8     	ldr	w8, [sp, #0x8]
400873b0: 11000508     	add	w8, w8, #0x1
400873b4: b9000be8     	str	w8, [sp, #0x8]
400873b8: 17ffffef     	b	0x40087374 <arp_cache_update+0x74>
400873bc: 1400003b     	b	0x400874a8 <arp_cache_update+0x1a8>
400873c0: 14000001     	b	0x400873c4 <arp_cache_update+0xc4>
400873c4: b9400fe8     	ldr	w8, [sp, #0xc]
400873c8: 11000508     	add	w8, w8, #0x1
400873cc: b9000fe8     	str	w8, [sp, #0xc]
400873d0: 17ffffd2     	b	0x40087318 <arp_cache_update+0x18>
400873d4: 2a1f03e8     	mov	w8, wzr
400873d8: b90007e8     	str	w8, [sp, #0x4]
400873dc: 14000001     	b	0x400873e0 <arp_cache_update+0xe0>
400873e0: b94007e8     	ldr	w8, [sp, #0x4]
400873e4: 71003d08     	subs	w8, w8, #0xf
400873e8: 5400060c     	b.gt	0x400874a8 <arp_cache_update+0x1a8>
400873ec: 14000001     	b	0x400873f0 <arp_cache_update+0xf0>
400873f0: b98007e9     	ldrsw	x9, [sp, #0x4]
400873f4: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
400873f8: 910b1108     	add	x8, x8, #0x2c4
400873fc: 8b091108     	add	x8, x8, x9, lsl #4
40087400: b9400d08     	ldr	w8, [x8, #0xc]
40087404: 35000488     	cbnz	w8, 0x40087494 <arp_cache_update+0x194>
40087408: 14000001     	b	0x4008740c <arp_cache_update+0x10c>
4008740c: b9401fe8     	ldr	w8, [sp, #0x1c]
40087410: b98007e9     	ldrsw	x9, [sp, #0x4]
40087414: d37ced2a     	lsl	x10, x9, #4
40087418: d0000c69     	adrp	x9, 0x40215000 <sockets+0x7ffc>
4008741c: 910b1129     	add	x9, x9, #0x2c4
40087420: b82a6928     	str	w8, [x9, x10]
40087424: 2a1f03e8     	mov	w8, wzr
40087428: b90003e8     	str	w8, [sp]
4008742c: 14000001     	b	0x40087430 <arp_cache_update+0x130>
40087430: b94003e8     	ldr	w8, [sp]
40087434: 71001508     	subs	w8, w8, #0x5
40087438: 5400020c     	b.gt	0x40087478 <arp_cache_update+0x178>
4008743c: 14000001     	b	0x40087440 <arp_cache_update+0x140>
40087440: f9400be8     	ldr	x8, [sp, #0x10]
40087444: b98003ea     	ldrsw	x10, [sp]
40087448: 386a6908     	ldrb	w8, [x8, x10]
4008744c: b98007eb     	ldrsw	x11, [sp, #0x4]
40087450: d0000c69     	adrp	x9, 0x40215000 <sockets+0x7ffc>
40087454: 910b1129     	add	x9, x9, #0x2c4
40087458: 8b0b1129     	add	x9, x9, x11, lsl #4
4008745c: 8b0a0129     	add	x9, x9, x10
40087460: 39001128     	strb	w8, [x9, #0x4]
40087464: 14000001     	b	0x40087468 <arp_cache_update+0x168>
40087468: b94003e8     	ldr	w8, [sp]
4008746c: 11000508     	add	w8, w8, #0x1
40087470: b90003e8     	str	w8, [sp]
40087474: 17ffffef     	b	0x40087430 <arp_cache_update+0x130>
40087478: b98007e9     	ldrsw	x9, [sp, #0x4]
4008747c: d0000c68     	adrp	x8, 0x40215000 <sockets+0x7ffc>
40087480: 910b1108     	add	x8, x8, #0x2c4
40087484: 8b091109     	add	x9, x8, x9, lsl #4
40087488: 52800028     	mov	w8, #0x1                // =1
4008748c: b9000d28     	str	w8, [x9, #0xc]
40087490: 14000006     	b	0x400874a8 <arp_cache_update+0x1a8>
40087494: 14000001     	b	0x40087498 <arp_cache_update+0x198>
40087498: b94007e8     	ldr	w8, [sp, #0x4]
4008749c: 11000508     	add	w8, w8, #0x1
400874a0: b90007e8     	str	w8, [sp, #0x4]
400874a4: 17ffffcf     	b	0x400873e0 <arp_cache_update+0xe0>
400874a8: 910083ff     	add	sp, sp, #0x20
400874ac: d65f03c0     	ret

00000000400874b0 <pipes_init>:
400874b0: d10083ff     	sub	sp, sp, #0x20
400874b4: a9017bfd     	stp	x29, x30, [sp, #0x10]
400874b8: 910043fd     	add	x29, sp, #0x10
400874bc: d0000c60     	adrp	x0, 0x40215000 <sockets+0x7ffc>
400874c0: 910f6000     	add	x0, x0, #0x3d8
400874c4: 97fff433     	bl	0x40084590 <spinlock_init>
400874c8: 2a1f03e8     	mov	w8, wzr
400874cc: b81fc3a8     	stur	w8, [x29, #-0x4]
400874d0: 14000001     	b	0x400874d4 <pipes_init+0x24>
400874d4: b85fc3a8     	ldur	w8, [x29, #-0x4]
400874d8: 7100fd08     	subs	w8, w8, #0x3f
400874dc: 540002ec     	b.gt	0x40087538 <pipes_init+0x88>
400874e0: 14000001     	b	0x400874e4 <pipes_init+0x34>
400874e4: b89fc3a8     	ldursw	x8, [x29, #-0x4]
400874e8: 52804409     	mov	w9, #0x220              // =544
400874ec: 2a0903e0     	mov	w0, w9
400874f0: 2a0003e9     	mov	w9, w0
400874f4: d0000c6a     	adrp	x10, 0x40215000 <sockets+0x7ffc>
400874f8: 910f714a     	add	x10, x10, #0x3dc
400874fc: 9b29290b     	smaddl	x11, w8, w9, x10
40087500: 2a1f03e8     	mov	w8, wzr
40087504: b9021168     	str	w8, [x11, #0x210]
40087508: b89fc3ab     	ldursw	x11, [x29, #-0x4]
4008750c: 9b29296b     	smaddl	x11, w11, w9, x10
40087510: b9021568     	str	w8, [x11, #0x214]
40087514: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40087518: 9b292908     	smaddl	x8, w8, w9, x10
4008751c: 91083100     	add	x0, x8, #0x20c
40087520: 97fff41c     	bl	0x40084590 <spinlock_init>
40087524: 14000001     	b	0x40087528 <pipes_init+0x78>
40087528: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008752c: 11000508     	add	w8, w8, #0x1
40087530: b81fc3a8     	stur	w8, [x29, #-0x4]
40087534: 17ffffe8     	b	0x400874d4 <pipes_init+0x24>
40087538: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008753c: 910083ff     	add	sp, sp, #0x20
40087540: d65f03c0     	ret
40087544: d503201f     	nop
40087548: d503201f     	nop
4008754c: d503201f     	nop

0000000040087550 <pipe_alloc>:
40087550: d10143ff     	sub	sp, sp, #0x50
40087554: a9047bfd     	stp	x29, x30, [sp, #0x40]
40087558: 910103fd     	add	x29, sp, #0x40
4008755c: f81f03a0     	stur	x0, [x29, #-0x10]
40087560: f81e83a1     	stur	x1, [x29, #-0x18]
40087564: d0000c60     	adrp	x0, 0x40215000 <sockets+0x7ffc>
40087568: 910f6000     	add	x0, x0, #0x3d8
4008756c: 97fff425     	bl	0x40084600 <spinlock_acquire_irqsave>
40087570: f90013e0     	str	x0, [sp, #0x20]
40087574: 2a1f03e8     	mov	w8, wzr
40087578: b9001fe8     	str	w8, [sp, #0x1c]
4008757c: 14000001     	b	0x40087580 <pipe_alloc+0x30>
40087580: b9401fe8     	ldr	w8, [sp, #0x1c]
40087584: 7100fd08     	subs	w8, w8, #0x3f
40087588: 54000b0c     	b.gt	0x400876e8 <pipe_alloc+0x198>
4008758c: 14000001     	b	0x40087590 <pipe_alloc+0x40>
40087590: b9801fe8     	ldrsw	x8, [sp, #0x1c]
40087594: 52804409     	mov	w9, #0x220              // =544
40087598: 2a0903e0     	mov	w0, w9
4008759c: 2a0003e9     	mov	w9, w0
400875a0: d0000c6a     	adrp	x10, 0x40215000 <sockets+0x7ffc>
400875a4: 910f714a     	add	x10, x10, #0x3dc
400875a8: 9b292908     	smaddl	x8, w8, w9, x10
400875ac: b9421108     	ldr	w8, [x8, #0x210]
400875b0: 35000928     	cbnz	w8, 0x400876d4 <pipe_alloc+0x184>
400875b4: 14000001     	b	0x400875b8 <pipe_alloc+0x68>
400875b8: b9801fe8     	ldrsw	x8, [sp, #0x1c]
400875bc: 52804409     	mov	w9, #0x220              // =544
400875c0: 2a0903e0     	mov	w0, w9
400875c4: 2a0003e9     	mov	w9, w0
400875c8: d0000c6a     	adrp	x10, 0x40215000 <sockets+0x7ffc>
400875cc: 910f714a     	add	x10, x10, #0x3dc
400875d0: 9b292908     	smaddl	x8, w8, w9, x10
400875d4: b9421508     	ldr	w8, [x8, #0x214]
400875d8: 350007e8     	cbnz	w8, 0x400876d4 <pipe_alloc+0x184>
400875dc: 14000001     	b	0x400875e0 <pipe_alloc+0x90>
400875e0: b9801fe8     	ldrsw	x8, [sp, #0x1c]
400875e4: 52804409     	mov	w9, #0x220              // =544
400875e8: 2a0903e0     	mov	w0, w9
400875ec: 2a0003ea     	mov	w10, w0
400875f0: b9000fea     	str	w10, [sp, #0xc]
400875f4: d0000c6b     	adrp	x11, 0x40215000 <sockets+0x7ffc>
400875f8: 910f716b     	add	x11, x11, #0x3dc
400875fc: f9000beb     	str	x11, [sp, #0x10]
40087600: 9b2a2d09     	smaddl	x9, w8, w10, x11
40087604: 52800028     	mov	w8, #0x1                // =1
40087608: b9021128     	str	w8, [x9, #0x210]
4008760c: b9801fe9     	ldrsw	x9, [sp, #0x1c]
40087610: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087614: b9021528     	str	w8, [x9, #0x214]
40087618: b9801fe9     	ldrsw	x9, [sp, #0x1c]
4008761c: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087620: 2a1f03ec     	mov	w12, wzr
40087624: b9001bec     	str	w12, [sp, #0x18]
40087628: b902012c     	str	w12, [x9, #0x200]
4008762c: b9801fe9     	ldrsw	x9, [sp, #0x1c]
40087630: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087634: b902052c     	str	w12, [x9, #0x204]
40087638: b9801fe9     	ldrsw	x9, [sp, #0x1c]
4008763c: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087640: b902092c     	str	w12, [x9, #0x208]
40087644: b9801fe9     	ldrsw	x9, [sp, #0x1c]
40087648: 9b2a2d29     	smaddl	x9, w9, w10, x11
4008764c: b902192c     	str	w12, [x9, #0x218]
40087650: b9801fe9     	ldrsw	x9, [sp, #0x1c]
40087654: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087658: b9021d2c     	str	w12, [x9, #0x21c]
4008765c: f85f03a9     	ldur	x9, [x29, #-0x10]
40087660: f940012d     	ldr	x13, [x9]
40087664: 52800049     	mov	w9, #0x2                // =2
40087668: b90001a9     	str	w9, [x13]
4008766c: b9801fed     	ldrsw	x13, [sp, #0x1c]
40087670: 9b2a2dad     	smaddl	x13, w13, w10, x11
40087674: f85f03ae     	ldur	x14, [x29, #-0x10]
40087678: f94001ce     	ldr	x14, [x14]
4008767c: f90009cd     	str	x13, [x14, #0x10]
40087680: f85f03ad     	ldur	x13, [x29, #-0x10]
40087684: f94001ad     	ldr	x13, [x13]
40087688: b90019ac     	str	w12, [x13, #0x18]
4008768c: f85e83ac     	ldur	x12, [x29, #-0x18]
40087690: f940018c     	ldr	x12, [x12]
40087694: b9000189     	str	w9, [x12]
40087698: b9801fe9     	ldrsw	x9, [sp, #0x1c]
4008769c: 9b2a2d29     	smaddl	x9, w9, w10, x11
400876a0: f85e83aa     	ldur	x10, [x29, #-0x18]
400876a4: f940014a     	ldr	x10, [x10]
400876a8: f9000949     	str	x9, [x10, #0x10]
400876ac: f85e83a9     	ldur	x9, [x29, #-0x18]
400876b0: f9400129     	ldr	x9, [x9]
400876b4: b9001928     	str	w8, [x9, #0x18]
400876b8: f94013e1     	ldr	x1, [sp, #0x20]
400876bc: d0000c60     	adrp	x0, 0x40215000 <sockets+0x7ffc>
400876c0: 910f6000     	add	x0, x0, #0x3d8
400876c4: 97fff3df     	bl	0x40084640 <spinlock_release_irqrestore>
400876c8: b9401be8     	ldr	w8, [sp, #0x18]
400876cc: b81fc3a8     	stur	w8, [x29, #-0x4]
400876d0: 1400000d     	b	0x40087704 <pipe_alloc+0x1b4>
400876d4: 14000001     	b	0x400876d8 <pipe_alloc+0x188>
400876d8: b9401fe8     	ldr	w8, [sp, #0x1c]
400876dc: 11000508     	add	w8, w8, #0x1
400876e0: b9001fe8     	str	w8, [sp, #0x1c]
400876e4: 17ffffa7     	b	0x40087580 <pipe_alloc+0x30>
400876e8: f94013e1     	ldr	x1, [sp, #0x20]
400876ec: d0000c60     	adrp	x0, 0x40215000 <sockets+0x7ffc>
400876f0: 910f6000     	add	x0, x0, #0x3d8
400876f4: 97fff3d3     	bl	0x40084640 <spinlock_release_irqrestore>
400876f8: 12800008     	mov	w8, #-0x1               // =-1
400876fc: b81fc3a8     	stur	w8, [x29, #-0x4]
40087700: 14000001     	b	0x40087704 <pipe_alloc+0x1b4>
40087704: b85fc3a0     	ldur	w0, [x29, #-0x4]
40087708: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008770c: 910143ff     	add	sp, sp, #0x50
40087710: d65f03c0     	ret
40087714: d503201f     	nop
40087718: d503201f     	nop
4008771c: d503201f     	nop

0000000040087720 <pipe_reopen>:
40087720: d100c3ff     	sub	sp, sp, #0x30
40087724: a9027bfd     	stp	x29, x30, [sp, #0x20]
40087728: 910083fd     	add	x29, sp, #0x20
4008772c: f81f83a0     	stur	x0, [x29, #-0x8]
40087730: b81f43a1     	stur	w1, [x29, #-0xc]
40087734: f85f83a8     	ldur	x8, [x29, #-0x8]
40087738: 91083100     	add	x0, x8, #0x20c
4008773c: 97fff3b1     	bl	0x40084600 <spinlock_acquire_irqsave>
40087740: f90007e0     	str	x0, [sp, #0x8]
40087744: b85f43a8     	ldur	w8, [x29, #-0xc]
40087748: 350000e8     	cbnz	w8, 0x40087764 <pipe_reopen+0x44>
4008774c: 14000001     	b	0x40087750 <pipe_reopen+0x30>
40087750: f85f83a9     	ldur	x9, [x29, #-0x8]
40087754: b9421128     	ldr	w8, [x9, #0x210]
40087758: 11000508     	add	w8, w8, #0x1
4008775c: b9021128     	str	w8, [x9, #0x210]
40087760: 14000006     	b	0x40087778 <pipe_reopen+0x58>
40087764: f85f83a9     	ldur	x9, [x29, #-0x8]
40087768: b9421528     	ldr	w8, [x9, #0x214]
4008776c: 11000508     	add	w8, w8, #0x1
40087770: b9021528     	str	w8, [x9, #0x214]
40087774: 14000001     	b	0x40087778 <pipe_reopen+0x58>
40087778: f85f83a8     	ldur	x8, [x29, #-0x8]
4008777c: 91083100     	add	x0, x8, #0x20c
40087780: f94007e1     	ldr	x1, [sp, #0x8]
40087784: 97fff3af     	bl	0x40084640 <spinlock_release_irqrestore>
40087788: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008778c: 9100c3ff     	add	sp, sp, #0x30
40087790: d65f03c0     	ret
40087794: d503201f     	nop
40087798: d503201f     	nop
4008779c: d503201f     	nop

00000000400877a0 <pipe_close>:
400877a0: d100c3ff     	sub	sp, sp, #0x30
400877a4: a9027bfd     	stp	x29, x30, [sp, #0x20]
400877a8: 910083fd     	add	x29, sp, #0x20
400877ac: f81f83a0     	stur	x0, [x29, #-0x8]
400877b0: b81f43a1     	stur	w1, [x29, #-0xc]
400877b4: f85f83a8     	ldur	x8, [x29, #-0x8]
400877b8: 91083100     	add	x0, x8, #0x20c
400877bc: 97fff391     	bl	0x40084600 <spinlock_acquire_irqsave>
400877c0: f90007e0     	str	x0, [sp, #0x8]
400877c4: b85f43a8     	ldur	w8, [x29, #-0xc]
400877c8: 35000508     	cbnz	w8, 0x40087868 <pipe_close+0xc8>
400877cc: 14000001     	b	0x400877d0 <pipe_close+0x30>
400877d0: f85f83a8     	ldur	x8, [x29, #-0x8]
400877d4: b9421108     	ldr	w8, [x8, #0x210]
400877d8: 340000e8     	cbz	w8, 0x400877f4 <pipe_close+0x54>
400877dc: 14000001     	b	0x400877e0 <pipe_close+0x40>
400877e0: f85f83a9     	ldur	x9, [x29, #-0x8]
400877e4: b9421128     	ldr	w8, [x9, #0x210]
400877e8: 71000508     	subs	w8, w8, #0x1
400877ec: b9021128     	str	w8, [x9, #0x210]
400877f0: 14000001     	b	0x400877f4 <pipe_close+0x54>
400877f4: f85f83a8     	ldur	x8, [x29, #-0x8]
400877f8: b9421108     	ldr	w8, [x8, #0x210]
400877fc: 35000348     	cbnz	w8, 0x40087864 <pipe_close+0xc4>
40087800: 14000001     	b	0x40087804 <pipe_close+0x64>
40087804: 2a1f03e8     	mov	w8, wzr
40087808: b90007e8     	str	w8, [sp, #0x4]
4008780c: 14000001     	b	0x40087810 <pipe_close+0x70>
40087810: b94007e8     	ldr	w8, [sp, #0x4]
40087814: 71007d08     	subs	w8, w8, #0x1f
40087818: 5400024c     	b.gt	0x40087860 <pipe_close+0xc0>
4008781c: 14000001     	b	0x40087820 <pipe_close+0x80>
40087820: f85f83a8     	ldur	x8, [x29, #-0x8]
40087824: b9421d08     	ldr	w8, [x8, #0x21c]
40087828: b94007e9     	ldr	w9, [sp, #0x4]
4008782c: 2a0903e0     	mov	w0, w9
40087830: 2a0003e9     	mov	w9, w0
40087834: 1ac92508     	lsr	w8, w8, w9
40087838: 360000a8     	tbz	w8, #0x0, 0x4008784c <pipe_close+0xac>
4008783c: 14000001     	b	0x40087840 <pipe_close+0xa0>
40087840: b94007e0     	ldr	w0, [sp, #0x4]
40087844: 9400077f     	bl	0x40089640 <process_wakeup>
40087848: 14000001     	b	0x4008784c <pipe_close+0xac>
4008784c: 14000001     	b	0x40087850 <pipe_close+0xb0>
40087850: b94007e8     	ldr	w8, [sp, #0x4]
40087854: 11000508     	add	w8, w8, #0x1
40087858: b90007e8     	str	w8, [sp, #0x4]
4008785c: 17ffffed     	b	0x40087810 <pipe_close+0x70>
40087860: 14000001     	b	0x40087864 <pipe_close+0xc4>
40087864: 14000027     	b	0x40087900 <pipe_close+0x160>
40087868: f85f83a8     	ldur	x8, [x29, #-0x8]
4008786c: b9421508     	ldr	w8, [x8, #0x214]
40087870: 340000e8     	cbz	w8, 0x4008788c <pipe_close+0xec>
40087874: 14000001     	b	0x40087878 <pipe_close+0xd8>
40087878: f85f83a9     	ldur	x9, [x29, #-0x8]
4008787c: b9421528     	ldr	w8, [x9, #0x214]
40087880: 71000508     	subs	w8, w8, #0x1
40087884: b9021528     	str	w8, [x9, #0x214]
40087888: 14000001     	b	0x4008788c <pipe_close+0xec>
4008788c: f85f83a8     	ldur	x8, [x29, #-0x8]
40087890: b9421508     	ldr	w8, [x8, #0x214]
40087894: 35000348     	cbnz	w8, 0x400878fc <pipe_close+0x15c>
40087898: 14000001     	b	0x4008789c <pipe_close+0xfc>
4008789c: 2a1f03e8     	mov	w8, wzr
400878a0: b90003e8     	str	w8, [sp]
400878a4: 14000001     	b	0x400878a8 <pipe_close+0x108>
400878a8: b94003e8     	ldr	w8, [sp]
400878ac: 71007d08     	subs	w8, w8, #0x1f
400878b0: 5400024c     	b.gt	0x400878f8 <pipe_close+0x158>
400878b4: 14000001     	b	0x400878b8 <pipe_close+0x118>
400878b8: f85f83a8     	ldur	x8, [x29, #-0x8]
400878bc: b9421908     	ldr	w8, [x8, #0x218]
400878c0: b94003e9     	ldr	w9, [sp]
400878c4: 2a0903e0     	mov	w0, w9
400878c8: 2a0003e9     	mov	w9, w0
400878cc: 1ac92508     	lsr	w8, w8, w9
400878d0: 360000a8     	tbz	w8, #0x0, 0x400878e4 <pipe_close+0x144>
400878d4: 14000001     	b	0x400878d8 <pipe_close+0x138>
400878d8: b94003e0     	ldr	w0, [sp]
400878dc: 94000759     	bl	0x40089640 <process_wakeup>
400878e0: 14000001     	b	0x400878e4 <pipe_close+0x144>
400878e4: 14000001     	b	0x400878e8 <pipe_close+0x148>
400878e8: b94003e8     	ldr	w8, [sp]
400878ec: 11000508     	add	w8, w8, #0x1
400878f0: b90003e8     	str	w8, [sp]
400878f4: 17ffffed     	b	0x400878a8 <pipe_close+0x108>
400878f8: 14000001     	b	0x400878fc <pipe_close+0x15c>
400878fc: 14000001     	b	0x40087900 <pipe_close+0x160>
40087900: f85f83a8     	ldur	x8, [x29, #-0x8]
40087904: 91083100     	add	x0, x8, #0x20c
40087908: f94007e1     	ldr	x1, [sp, #0x8]
4008790c: 97fff34d     	bl	0x40084640 <spinlock_release_irqrestore>
40087910: a9427bfd     	ldp	x29, x30, [sp, #0x20]
40087914: 9100c3ff     	add	sp, sp, #0x30
40087918: d65f03c0     	ret
4008791c: d503201f     	nop

0000000040087920 <pipe_read>:
40087920: d10203ff     	sub	sp, sp, #0x80
40087924: a9077bfd     	stp	x29, x30, [sp, #0x70]
40087928: 9101c3fd     	add	x29, sp, #0x70
4008792c: f81f03a0     	stur	x0, [x29, #-0x10]
40087930: f81e83a1     	stur	x1, [x29, #-0x18]
40087934: b81e43a2     	stur	w2, [x29, #-0x1c]
40087938: f81d83a3     	stur	x3, [x29, #-0x28]
4008793c: f85f03a8     	ldur	x8, [x29, #-0x10]
40087940: b50000a8     	cbnz	x8, 0x40087954 <pipe_read+0x34>
40087944: 14000001     	b	0x40087948 <pipe_read+0x28>
40087948: 12800008     	mov	w8, #-0x1               // =-1
4008794c: b81fc3a8     	stur	w8, [x29, #-0x4]
40087950: 1400009b     	b	0x40087bbc <pipe_read+0x29c>
40087954: f85e83a8     	ldur	x8, [x29, #-0x18]
40087958: f81d03a8     	stur	x8, [x29, #-0x30]
4008795c: 2a1f03e8     	mov	w8, wzr
40087960: b81cc3a8     	stur	w8, [x29, #-0x34]
40087964: 940001bf     	bl	0x40088060 <current_process>
40087968: f9001be0     	str	x0, [sp, #0x30]
4008796c: 14000001     	b	0x40087970 <pipe_read+0x50>
40087970: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087974: b85e43a9     	ldur	w9, [x29, #-0x1c]
40087978: 6b090108     	subs	w8, w8, w9
4008797c: 540011aa     	b.ge	0x40087bb0 <pipe_read+0x290>
40087980: 14000001     	b	0x40087984 <pipe_read+0x64>
40087984: f85f03a8     	ldur	x8, [x29, #-0x10]
40087988: 91083100     	add	x0, x8, #0x20c
4008798c: 97fff31d     	bl	0x40084600 <spinlock_acquire_irqsave>
40087990: f90017e0     	str	x0, [sp, #0x28]
40087994: f85f03a8     	ldur	x8, [x29, #-0x10]
40087998: b9420908     	ldr	w8, [x8, #0x208]
4008799c: 34000a68     	cbz	w8, 0x40087ae8 <pipe_read+0x1c8>
400879a0: 14000001     	b	0x400879a4 <pipe_read+0x84>
400879a4: 14000001     	b	0x400879a8 <pipe_read+0x88>
400879a8: b85cc3a9     	ldur	w9, [x29, #-0x34]
400879ac: b85e43aa     	ldur	w10, [x29, #-0x1c]
400879b0: 2a1f03e8     	mov	w8, wzr
400879b4: 6b0a0129     	subs	w9, w9, w10
400879b8: b90017e8     	str	w8, [sp, #0x14]
400879bc: 5400010a     	b.ge	0x400879dc <pipe_read+0xbc>
400879c0: 14000001     	b	0x400879c4 <pipe_read+0xa4>
400879c4: f85f03a8     	ldur	x8, [x29, #-0x10]
400879c8: b9420908     	ldr	w8, [x8, #0x208]
400879cc: 71000108     	subs	w8, w8, #0x0
400879d0: 1a9f07e8     	cset	w8, ne
400879d4: b90017e8     	str	w8, [sp, #0x14]
400879d8: 14000001     	b	0x400879dc <pipe_read+0xbc>
400879dc: b94017e8     	ldr	w8, [sp, #0x14]
400879e0: 360002a8     	tbz	w8, #0x0, 0x40087a34 <pipe_read+0x114>
400879e4: 14000001     	b	0x400879e8 <pipe_read+0xc8>
400879e8: f85f03a8     	ldur	x8, [x29, #-0x10]
400879ec: b9420509     	ldr	w9, [x8, #0x204]
400879f0: 38696908     	ldrb	w8, [x8, x9]
400879f4: f85d03a9     	ldur	x9, [x29, #-0x30]
400879f8: b89cc3aa     	ldursw	x10, [x29, #-0x34]
400879fc: 382a6928     	strb	w8, [x9, x10]
40087a00: f85f03a9     	ldur	x9, [x29, #-0x10]
40087a04: b9420528     	ldr	w8, [x9, #0x204]
40087a08: 11000508     	add	w8, w8, #0x1
40087a0c: 12002108     	and	w8, w8, #0x1ff
40087a10: b9020528     	str	w8, [x9, #0x204]
40087a14: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087a18: 11000508     	add	w8, w8, #0x1
40087a1c: b81cc3a8     	stur	w8, [x29, #-0x34]
40087a20: f85f03a9     	ldur	x9, [x29, #-0x10]
40087a24: b9420928     	ldr	w8, [x9, #0x208]
40087a28: 71000508     	subs	w8, w8, #0x1
40087a2c: b9020928     	str	w8, [x9, #0x208]
40087a30: 17ffffde     	b	0x400879a8 <pipe_read+0x88>
40087a34: f85f03a8     	ldur	x8, [x29, #-0x10]
40087a38: b9420908     	ldr	w8, [x8, #0x208]
40087a3c: 7107fd08     	subs	w8, w8, #0x1ff
40087a40: 54000461     	b.ne	0x40087acc <pipe_read+0x1ac>
40087a44: 14000001     	b	0x40087a48 <pipe_read+0x128>
40087a48: 2a1f03e8     	mov	w8, wzr
40087a4c: b90027e8     	str	w8, [sp, #0x24]
40087a50: 14000001     	b	0x40087a54 <pipe_read+0x134>
40087a54: b94027e8     	ldr	w8, [sp, #0x24]
40087a58: 71007d08     	subs	w8, w8, #0x1f
40087a5c: 5400036c     	b.gt	0x40087ac8 <pipe_read+0x1a8>
40087a60: 14000001     	b	0x40087a64 <pipe_read+0x144>
40087a64: f85f03a8     	ldur	x8, [x29, #-0x10]
40087a68: b9421d08     	ldr	w8, [x8, #0x21c]
40087a6c: b94027e9     	ldr	w9, [sp, #0x24]
40087a70: 2a0903e0     	mov	w0, w9
40087a74: 2a0003e9     	mov	w9, w0
40087a78: 1ac92508     	lsr	w8, w8, w9
40087a7c: 360001c8     	tbz	w8, #0x0, 0x40087ab4 <pipe_read+0x194>
40087a80: 14000001     	b	0x40087a84 <pipe_read+0x164>
40087a84: b94027e0     	ldr	w0, [sp, #0x24]
40087a88: 940006ee     	bl	0x40089640 <process_wakeup>
40087a8c: b94027e8     	ldr	w8, [sp, #0x24]
40087a90: 2a0803e0     	mov	w0, w8
40087a94: 2a0003e9     	mov	w9, w0
40087a98: 52800028     	mov	w8, #0x1                // =1
40087a9c: 1ac9210a     	lsl	w10, w8, w9
40087aa0: f85f03a9     	ldur	x9, [x29, #-0x10]
40087aa4: b9421d28     	ldr	w8, [x9, #0x21c]
40087aa8: 0a2a0108     	bic	w8, w8, w10
40087aac: b9021d28     	str	w8, [x9, #0x21c]
40087ab0: 14000001     	b	0x40087ab4 <pipe_read+0x194>
40087ab4: 14000001     	b	0x40087ab8 <pipe_read+0x198>
40087ab8: b94027e8     	ldr	w8, [sp, #0x24]
40087abc: 11000508     	add	w8, w8, #0x1
40087ac0: b90027e8     	str	w8, [sp, #0x24]
40087ac4: 17ffffe4     	b	0x40087a54 <pipe_read+0x134>
40087ac8: 14000001     	b	0x40087acc <pipe_read+0x1ac>
40087acc: f85f03a8     	ldur	x8, [x29, #-0x10]
40087ad0: 91083100     	add	x0, x8, #0x20c
40087ad4: f94017e1     	ldr	x1, [sp, #0x28]
40087ad8: 97fff2da     	bl	0x40084640 <spinlock_release_irqrestore>
40087adc: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087ae0: b81fc3a8     	stur	w8, [x29, #-0x4]
40087ae4: 14000036     	b	0x40087bbc <pipe_read+0x29c>
40087ae8: f85f03a8     	ldur	x8, [x29, #-0x10]
40087aec: b9421508     	ldr	w8, [x8, #0x214]
40087af0: 35000128     	cbnz	w8, 0x40087b14 <pipe_read+0x1f4>
40087af4: 14000001     	b	0x40087af8 <pipe_read+0x1d8>
40087af8: f85f03a8     	ldur	x8, [x29, #-0x10]
40087afc: 91083100     	add	x0, x8, #0x20c
40087b00: f94017e1     	ldr	x1, [sp, #0x28]
40087b04: 97fff2cf     	bl	0x40084640 <spinlock_release_irqrestore>
40087b08: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087b0c: b81fc3a8     	stur	w8, [x29, #-0x4]
40087b10: 1400002b     	b	0x40087bbc <pipe_read+0x29c>
40087b14: f9401be8     	ldr	x8, [sp, #0x30]
40087b18: b40003e8     	cbz	x8, 0x40087b94 <pipe_read+0x274>
40087b1c: 14000001     	b	0x40087b20 <pipe_read+0x200>
40087b20: f9401be8     	ldr	x8, [sp, #0x30]
40087b24: b9400108     	ldr	w8, [x8]
40087b28: 2a0803e0     	mov	w0, w8
40087b2c: 2a0003e9     	mov	w9, w0
40087b30: 52800028     	mov	w8, #0x1                // =1
40087b34: 1ac9210a     	lsl	w10, w8, w9
40087b38: f85f03a9     	ldur	x9, [x29, #-0x10]
40087b3c: b9421928     	ldr	w8, [x9, #0x218]
40087b40: 2a0a0108     	orr	w8, w8, w10
40087b44: b9021928     	str	w8, [x9, #0x218]
40087b48: d0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40087b4c: 912f8000     	add	x0, x0, #0xbe0
40087b50: f90007e0     	str	x0, [sp, #0x8]
40087b54: 97fff2ab     	bl	0x40084600 <spinlock_acquire_irqsave>
40087b58: aa0003e8     	mov	x8, x0
40087b5c: f94007e0     	ldr	x0, [sp, #0x8]
40087b60: f9000fe8     	str	x8, [sp, #0x18]
40087b64: f9401be9     	ldr	x9, [sp, #0x30]
40087b68: 528000a8     	mov	w8, #0x5                // =5
40087b6c: b9000528     	str	w8, [x9, #0x4]
40087b70: f9400fe1     	ldr	x1, [sp, #0x18]
40087b74: 97fff2b3     	bl	0x40084640 <spinlock_release_irqrestore>
40087b78: f85f03a8     	ldur	x8, [x29, #-0x10]
40087b7c: 91083100     	add	x0, x8, #0x20c
40087b80: f94017e1     	ldr	x1, [sp, #0x28]
40087b84: 97fff2af     	bl	0x40084640 <spinlock_release_irqrestore>
40087b88: 12800028     	mov	w8, #-0x2               // =-2
40087b8c: b81fc3a8     	stur	w8, [x29, #-0x4]
40087b90: 1400000b     	b	0x40087bbc <pipe_read+0x29c>
40087b94: f85f03a8     	ldur	x8, [x29, #-0x10]
40087b98: 91083100     	add	x0, x8, #0x20c
40087b9c: f94017e1     	ldr	x1, [sp, #0x28]
40087ba0: 97fff2a8     	bl	0x40084640 <spinlock_release_irqrestore>
40087ba4: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087ba8: b81fc3a8     	stur	w8, [x29, #-0x4]
40087bac: 14000004     	b	0x40087bbc <pipe_read+0x29c>
40087bb0: b85cc3a8     	ldur	w8, [x29, #-0x34]
40087bb4: b81fc3a8     	stur	w8, [x29, #-0x4]
40087bb8: 14000001     	b	0x40087bbc <pipe_read+0x29c>
40087bbc: b85fc3a0     	ldur	w0, [x29, #-0x4]
40087bc0: a9477bfd     	ldp	x29, x30, [sp, #0x70]
40087bc4: 910203ff     	add	sp, sp, #0x80
40087bc8: d65f03c0     	ret
40087bcc: d503201f     	nop

0000000040087bd0 <pipe_write>:
40087bd0: d101c3ff     	sub	sp, sp, #0x70
40087bd4: a9067bfd     	stp	x29, x30, [sp, #0x60]
40087bd8: 910183fd     	add	x29, sp, #0x60
40087bdc: f81f03a0     	stur	x0, [x29, #-0x10]
40087be0: f81e83a1     	stur	x1, [x29, #-0x18]
40087be4: b81e43a2     	stur	w2, [x29, #-0x1c]
40087be8: f81d83a3     	stur	x3, [x29, #-0x28]
40087bec: f85f03a8     	ldur	x8, [x29, #-0x10]
40087bf0: b50000a8     	cbnz	x8, 0x40087c04 <pipe_write+0x34>
40087bf4: 14000001     	b	0x40087bf8 <pipe_write+0x28>
40087bf8: 12800008     	mov	w8, #-0x1               // =-1
40087bfc: b81fc3a8     	stur	w8, [x29, #-0x4]
40087c00: 14000089     	b	0x40087e24 <pipe_write+0x254>
40087c04: f85e83a8     	ldur	x8, [x29, #-0x18]
40087c08: f9001be8     	str	x8, [sp, #0x30]
40087c0c: 2a1f03e8     	mov	w8, wzr
40087c10: b9002fe8     	str	w8, [sp, #0x2c]
40087c14: 94000113     	bl	0x40088060 <current_process>
40087c18: f90013e0     	str	x0, [sp, #0x20]
40087c1c: 14000001     	b	0x40087c20 <pipe_write+0x50>
40087c20: b9402fe8     	ldr	w8, [sp, #0x2c]
40087c24: b85e43a9     	ldur	w9, [x29, #-0x1c]
40087c28: 6b090108     	subs	w8, w8, w9
40087c2c: 54000f6a     	b.ge	0x40087e18 <pipe_write+0x248>
40087c30: 14000001     	b	0x40087c34 <pipe_write+0x64>
40087c34: f85f03a8     	ldur	x8, [x29, #-0x10]
40087c38: 91083100     	add	x0, x8, #0x20c
40087c3c: 97fff271     	bl	0x40084600 <spinlock_acquire_irqsave>
40087c40: f9000fe0     	str	x0, [sp, #0x18]
40087c44: f85f03a8     	ldur	x8, [x29, #-0x10]
40087c48: b9421108     	ldr	w8, [x8, #0x210]
40087c4c: 35000128     	cbnz	w8, 0x40087c70 <pipe_write+0xa0>
40087c50: 14000001     	b	0x40087c54 <pipe_write+0x84>
40087c54: f85f03a8     	ldur	x8, [x29, #-0x10]
40087c58: 91083100     	add	x0, x8, #0x20c
40087c5c: f9400fe1     	ldr	x1, [sp, #0x18]
40087c60: 97fff278     	bl	0x40084640 <spinlock_release_irqrestore>
40087c64: 12800008     	mov	w8, #-0x1               // =-1
40087c68: b81fc3a8     	stur	w8, [x29, #-0x4]
40087c6c: 1400006e     	b	0x40087e24 <pipe_write+0x254>
40087c70: f85f03a8     	ldur	x8, [x29, #-0x10]
40087c74: b9420908     	ldr	w8, [x8, #0x208]
40087c78: 7107fd08     	subs	w8, w8, #0x1ff
40087c7c: 540007e8     	b.hi	0x40087d78 <pipe_write+0x1a8>
40087c80: 14000001     	b	0x40087c84 <pipe_write+0xb4>
40087c84: f9401be8     	ldr	x8, [sp, #0x30]
40087c88: b9802fe9     	ldrsw	x9, [sp, #0x2c]
40087c8c: 38696908     	ldrb	w8, [x8, x9]
40087c90: f85f03a9     	ldur	x9, [x29, #-0x10]
40087c94: b942012a     	ldr	w10, [x9, #0x200]
40087c98: 382a6928     	strb	w8, [x9, x10]
40087c9c: f85f03a9     	ldur	x9, [x29, #-0x10]
40087ca0: b9420128     	ldr	w8, [x9, #0x200]
40087ca4: 11000508     	add	w8, w8, #0x1
40087ca8: 12002108     	and	w8, w8, #0x1ff
40087cac: b9020128     	str	w8, [x9, #0x200]
40087cb0: b9402fe8     	ldr	w8, [sp, #0x2c]
40087cb4: 11000508     	add	w8, w8, #0x1
40087cb8: b9002fe8     	str	w8, [sp, #0x2c]
40087cbc: f85f03a9     	ldur	x9, [x29, #-0x10]
40087cc0: b9420928     	ldr	w8, [x9, #0x208]
40087cc4: 11000508     	add	w8, w8, #0x1
40087cc8: b9020928     	str	w8, [x9, #0x208]
40087ccc: f85f03a8     	ldur	x8, [x29, #-0x10]
40087cd0: b9420908     	ldr	w8, [x8, #0x208]
40087cd4: 71000508     	subs	w8, w8, #0x1
40087cd8: 54000461     	b.ne	0x40087d64 <pipe_write+0x194>
40087cdc: 14000001     	b	0x40087ce0 <pipe_write+0x110>
40087ce0: 2a1f03e8     	mov	w8, wzr
40087ce4: b90017e8     	str	w8, [sp, #0x14]
40087ce8: 14000001     	b	0x40087cec <pipe_write+0x11c>
40087cec: b94017e8     	ldr	w8, [sp, #0x14]
40087cf0: 71007d08     	subs	w8, w8, #0x1f
40087cf4: 5400036c     	b.gt	0x40087d60 <pipe_write+0x190>
40087cf8: 14000001     	b	0x40087cfc <pipe_write+0x12c>
40087cfc: f85f03a8     	ldur	x8, [x29, #-0x10]
40087d00: b9421908     	ldr	w8, [x8, #0x218]
40087d04: b94017e9     	ldr	w9, [sp, #0x14]
40087d08: 2a0903e0     	mov	w0, w9
40087d0c: 2a0003e9     	mov	w9, w0
40087d10: 1ac92508     	lsr	w8, w8, w9
40087d14: 360001c8     	tbz	w8, #0x0, 0x40087d4c <pipe_write+0x17c>
40087d18: 14000001     	b	0x40087d1c <pipe_write+0x14c>
40087d1c: b94017e0     	ldr	w0, [sp, #0x14]
40087d20: 94000648     	bl	0x40089640 <process_wakeup>
40087d24: b94017e8     	ldr	w8, [sp, #0x14]
40087d28: 2a0803e0     	mov	w0, w8
40087d2c: 2a0003e9     	mov	w9, w0
40087d30: 52800028     	mov	w8, #0x1                // =1
40087d34: 1ac9210a     	lsl	w10, w8, w9
40087d38: f85f03a9     	ldur	x9, [x29, #-0x10]
40087d3c: b9421928     	ldr	w8, [x9, #0x218]
40087d40: 0a2a0108     	bic	w8, w8, w10
40087d44: b9021928     	str	w8, [x9, #0x218]
40087d48: 14000001     	b	0x40087d4c <pipe_write+0x17c>
40087d4c: 14000001     	b	0x40087d50 <pipe_write+0x180>
40087d50: b94017e8     	ldr	w8, [sp, #0x14]
40087d54: 11000508     	add	w8, w8, #0x1
40087d58: b90017e8     	str	w8, [sp, #0x14]
40087d5c: 17ffffe4     	b	0x40087cec <pipe_write+0x11c>
40087d60: 14000001     	b	0x40087d64 <pipe_write+0x194>
40087d64: f85f03a8     	ldur	x8, [x29, #-0x10]
40087d68: 91083100     	add	x0, x8, #0x20c
40087d6c: f9400fe1     	ldr	x1, [sp, #0x18]
40087d70: 97fff234     	bl	0x40084640 <spinlock_release_irqrestore>
40087d74: 14000028     	b	0x40087e14 <pipe_write+0x244>
40087d78: f94013e8     	ldr	x8, [sp, #0x20]
40087d7c: b40003e8     	cbz	x8, 0x40087df8 <pipe_write+0x228>
40087d80: 14000001     	b	0x40087d84 <pipe_write+0x1b4>
40087d84: f94013e8     	ldr	x8, [sp, #0x20]
40087d88: b9400108     	ldr	w8, [x8]
40087d8c: 2a0803e0     	mov	w0, w8
40087d90: 2a0003e9     	mov	w9, w0
40087d94: 52800028     	mov	w8, #0x1                // =1
40087d98: 1ac9210a     	lsl	w10, w8, w9
40087d9c: f85f03a9     	ldur	x9, [x29, #-0x10]
40087da0: b9421d28     	ldr	w8, [x9, #0x21c]
40087da4: 2a0a0108     	orr	w8, w8, w10
40087da8: b9021d28     	str	w8, [x9, #0x21c]
40087dac: d0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40087db0: 912f8000     	add	x0, x0, #0xbe0
40087db4: f90003e0     	str	x0, [sp]
40087db8: 97fff212     	bl	0x40084600 <spinlock_acquire_irqsave>
40087dbc: aa0003e8     	mov	x8, x0
40087dc0: f94003e0     	ldr	x0, [sp]
40087dc4: f90007e8     	str	x8, [sp, #0x8]
40087dc8: f94013e9     	ldr	x9, [sp, #0x20]
40087dcc: 528000a8     	mov	w8, #0x5                // =5
40087dd0: b9000528     	str	w8, [x9, #0x4]
40087dd4: f94007e1     	ldr	x1, [sp, #0x8]
40087dd8: 97fff21a     	bl	0x40084640 <spinlock_release_irqrestore>
40087ddc: f85f03a8     	ldur	x8, [x29, #-0x10]
40087de0: 91083100     	add	x0, x8, #0x20c
40087de4: f9400fe1     	ldr	x1, [sp, #0x18]
40087de8: 97fff216     	bl	0x40084640 <spinlock_release_irqrestore>
40087dec: 12800028     	mov	w8, #-0x2               // =-2
40087df0: b81fc3a8     	stur	w8, [x29, #-0x4]
40087df4: 1400000c     	b	0x40087e24 <pipe_write+0x254>
40087df8: f85f03a8     	ldur	x8, [x29, #-0x10]
40087dfc: 91083100     	add	x0, x8, #0x20c
40087e00: f9400fe1     	ldr	x1, [sp, #0x18]
40087e04: 97fff20f     	bl	0x40084640 <spinlock_release_irqrestore>
40087e08: b9402fe8     	ldr	w8, [sp, #0x2c]
40087e0c: b81fc3a8     	stur	w8, [x29, #-0x4]
40087e10: 14000005     	b	0x40087e24 <pipe_write+0x254>
40087e14: 17ffff83     	b	0x40087c20 <pipe_write+0x50>
40087e18: b9402fe8     	ldr	w8, [sp, #0x2c]
40087e1c: b81fc3a8     	stur	w8, [x29, #-0x4]
40087e20: 14000001     	b	0x40087e24 <pipe_write+0x254>
40087e24: b85fc3a0     	ldur	w0, [x29, #-0x4]
40087e28: a9467bfd     	ldp	x29, x30, [sp, #0x60]
40087e2c: 9101c3ff     	add	sp, sp, #0x70
40087e30: d65f03c0     	ret
40087e34: d503201f     	nop
40087e38: d503201f     	nop
40087e3c: d503201f     	nop

0000000040087e40 <pipe_available>:
40087e40: d100c3ff     	sub	sp, sp, #0x30
40087e44: a9027bfd     	stp	x29, x30, [sp, #0x20]
40087e48: 910083fd     	add	x29, sp, #0x20
40087e4c: f9000be0     	str	x0, [sp, #0x10]
40087e50: f9400be8     	ldr	x8, [sp, #0x10]
40087e54: b50000a8     	cbnz	x8, 0x40087e68 <pipe_available+0x28>
40087e58: 14000001     	b	0x40087e5c <pipe_available+0x1c>
40087e5c: 12800008     	mov	w8, #-0x1               // =-1
40087e60: b81fc3a8     	stur	w8, [x29, #-0x4]
40087e64: 14000019     	b	0x40087ec8 <pipe_available+0x88>
40087e68: f9400be8     	ldr	x8, [sp, #0x10]
40087e6c: 91083100     	add	x0, x8, #0x20c
40087e70: 97fff1e4     	bl	0x40084600 <spinlock_acquire_irqsave>
40087e74: f90007e0     	str	x0, [sp, #0x8]
40087e78: f9400be8     	ldr	x8, [sp, #0x10]
40087e7c: b9420908     	ldr	w8, [x8, #0x208]
40087e80: b90007e8     	str	w8, [sp, #0x4]
40087e84: b94007e8     	ldr	w8, [sp, #0x4]
40087e88: 35000128     	cbnz	w8, 0x40087eac <pipe_available+0x6c>
40087e8c: 14000001     	b	0x40087e90 <pipe_available+0x50>
40087e90: f9400be8     	ldr	x8, [sp, #0x10]
40087e94: b9421508     	ldr	w8, [x8, #0x214]
40087e98: 350000a8     	cbnz	w8, 0x40087eac <pipe_available+0x6c>
40087e9c: 14000001     	b	0x40087ea0 <pipe_available+0x60>
40087ea0: 12800008     	mov	w8, #-0x1               // =-1
40087ea4: b90007e8     	str	w8, [sp, #0x4]
40087ea8: 14000001     	b	0x40087eac <pipe_available+0x6c>
40087eac: f9400be8     	ldr	x8, [sp, #0x10]
40087eb0: 91083100     	add	x0, x8, #0x20c
40087eb4: f94007e1     	ldr	x1, [sp, #0x8]
40087eb8: 97fff1e2     	bl	0x40084640 <spinlock_release_irqrestore>
40087ebc: b94007e8     	ldr	w8, [sp, #0x4]
40087ec0: b81fc3a8     	stur	w8, [x29, #-0x4]
40087ec4: 14000001     	b	0x40087ec8 <pipe_available+0x88>
40087ec8: b85fc3a0     	ldur	w0, [x29, #-0x4]
40087ecc: a9427bfd     	ldp	x29, x30, [sp, #0x20]
40087ed0: 9100c3ff     	add	sp, sp, #0x30
40087ed4: d65f03c0     	ret
		...

0000000040087ee0 <process_init>:
40087ee0: d10083ff     	sub	sp, sp, #0x20
40087ee4: a9017bfd     	stp	x29, x30, [sp, #0x10]
40087ee8: 910043fd     	add	x29, sp, #0x10
40087eec: d0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40087ef0: 912f8000     	add	x0, x0, #0xbe0
40087ef4: 97fff1a7     	bl	0x40084590 <spinlock_init>
40087ef8: d0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40087efc: 912f9000     	add	x0, x0, #0xbe4
40087f00: 97fff1a4     	bl	0x40084590 <spinlock_init>
40087f04: 2a1f03e8     	mov	w8, wzr
40087f08: b81fc3a8     	stur	w8, [x29, #-0x4]
40087f0c: 14000001     	b	0x40087f10 <process_init+0x30>
40087f10: b85fc3a8     	ldur	w8, [x29, #-0x4]
40087f14: 7100fd08     	subs	w8, w8, #0x3f
40087f18: 5400078c     	b.gt	0x40088008 <process_init+0x128>
40087f1c: 14000001     	b	0x40087f20 <process_init+0x40>
40087f20: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40087f24: 52803b09     	mov	w9, #0x1d8              // =472
40087f28: 2a0903e0     	mov	w0, w9
40087f2c: 2a0003ea     	mov	w10, w0
40087f30: 9b2a7d09     	smull	x9, w8, w10
40087f34: d0000cab     	adrp	x11, 0x4021d000 <pipes+0x7c24>
40087f38: 912fa16b     	add	x11, x11, #0xbe8
40087f3c: b8296968     	str	w8, [x11, x9]
40087f40: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40087f44: 9b2a2d09     	smaddl	x9, w8, w10, x11
40087f48: 2a1f03e8     	mov	w8, wzr
40087f4c: b9000528     	str	w8, [x9, #0x4]
40087f50: b89fc3a9     	ldursw	x9, [x29, #-0x4]
40087f54: 9b2a2d2c     	smaddl	x12, w9, w10, x11
40087f58: 12800009     	mov	w9, #-0x1               // =-1
40087f5c: b9000989     	str	w9, [x12, #0x8]
40087f60: b89fc3a9     	ldursw	x9, [x29, #-0x4]
40087f64: 9b2a2d2c     	smaddl	x12, w9, w10, x11
40087f68: aa1f03e9     	mov	x9, xzr
40087f6c: f900a189     	str	x9, [x12, #0x140]
40087f70: b89fc3a9     	ldursw	x9, [x29, #-0x4]
40087f74: 2a0903ec     	mov	w12, w9
40087f78: 52aa0009     	mov	w9, #0x50000000         // =1342177280
40087f7c: 0b0c552d     	add	w13, w9, w12, lsl #21
40087f80: 2a0d03e9     	mov	w9, w13
40087f84: 93407d29     	sxtw	x9, w9
40087f88: 9b2a2d8c     	smaddl	x12, w12, w10, x11
40087f8c: f900a589     	str	x9, [x12, #0x148]
40087f90: b89fc3a9     	ldursw	x9, [x29, #-0x4]
40087f94: 9b2a2d29     	smaddl	x9, w9, w10, x11
40087f98: b901d128     	str	w8, [x9, #0x1d0]
40087f9c: b9000be8     	str	w8, [sp, #0x8]
40087fa0: 14000001     	b	0x40087fa4 <process_init+0xc4>
40087fa4: b9400be8     	ldr	w8, [sp, #0x8]
40087fa8: 71007d08     	subs	w8, w8, #0x1f
40087fac: 5400024c     	b.gt	0x40087ff4 <process_init+0x114>
40087fb0: 14000001     	b	0x40087fb4 <process_init+0xd4>
40087fb4: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40087fb8: 52803b09     	mov	w9, #0x1d8              // =472
40087fbc: 2a0903e0     	mov	w0, w9
40087fc0: 2a0003e9     	mov	w9, w0
40087fc4: d0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40087fc8: 912fa14a     	add	x10, x10, #0xbe8
40087fcc: 9b292908     	smaddl	x8, w8, w9, x10
40087fd0: b9800be9     	ldrsw	x9, [sp, #0x8]
40087fd4: 8b090909     	add	x9, x8, x9, lsl #2
40087fd8: 12800008     	mov	w8, #-0x1               // =-1
40087fdc: b9015128     	str	w8, [x9, #0x150]
40087fe0: 14000001     	b	0x40087fe4 <process_init+0x104>
40087fe4: b9400be8     	ldr	w8, [sp, #0x8]
40087fe8: 11000508     	add	w8, w8, #0x1
40087fec: b9000be8     	str	w8, [sp, #0x8]
40087ff0: 17ffffed     	b	0x40087fa4 <process_init+0xc4>
40087ff4: 14000001     	b	0x40087ff8 <process_init+0x118>
40087ff8: b85fc3a8     	ldur	w8, [x29, #-0x4]
40087ffc: 11000508     	add	w8, w8, #0x1
40088000: b81fc3a8     	stur	w8, [x29, #-0x4]
40088004: 17ffffc3     	b	0x40087f10 <process_init+0x30>
40088008: 2a1f03e8     	mov	w8, wzr
4008800c: b90007e8     	str	w8, [sp, #0x4]
40088010: 14000001     	b	0x40088014 <process_init+0x134>
40088014: b94007e8     	ldr	w8, [sp, #0x4]
40088018: 71000d08     	subs	w8, w8, #0x3
4008801c: 5400018c     	b.gt	0x4008804c <process_init+0x16c>
40088020: 14000001     	b	0x40088024 <process_init+0x144>
40088024: b98007ea     	ldrsw	x10, [sp, #0x4]
40088028: b0000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
4008802c: 9107a129     	add	x9, x9, #0x1e8
40088030: 12800008     	mov	w8, #-0x1               // =-1
40088034: b82a7928     	str	w8, [x9, x10, lsl #2]
40088038: 14000001     	b	0x4008803c <process_init+0x15c>
4008803c: b94007e8     	ldr	w8, [sp, #0x4]
40088040: 11000508     	add	w8, w8, #0x1
40088044: b90007e8     	str	w8, [sp, #0x4]
40088048: 17fffff3     	b	0x40088014 <process_init+0x134>
4008804c: a9417bfd     	ldp	x29, x30, [sp, #0x10]
40088050: 910083ff     	add	sp, sp, #0x20
40088054: d65f03c0     	ret
40088058: d503201f     	nop
4008805c: d503201f     	nop

0000000040088060 <current_process>:
40088060: d10083ff     	sub	sp, sp, #0x20
40088064: a9017bfd     	stp	x29, x30, [sp, #0x10]
40088068: 910043fd     	add	x29, sp, #0x10
4008806c: 940007c1     	bl	0x40089f70 <get_cpuid>
40088070: b90007e0     	str	w0, [sp, #0x4]
40088074: b94007e8     	ldr	w8, [sp, #0x4]
40088078: 71001108     	subs	w8, w8, #0x4
4008807c: 540000a3     	b.lo	0x40088090 <current_process+0x30>
40088080: 14000001     	b	0x40088084 <current_process+0x24>
40088084: aa1f03e8     	mov	x8, xzr
40088088: f90007e8     	str	x8, [sp, #0x8]
4008808c: 1400001a     	b	0x400880f4 <current_process+0x94>
40088090: b94007e8     	ldr	w8, [sp, #0x4]
40088094: 2a0803e9     	mov	w9, w8
40088098: b0000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
4008809c: 9107a108     	add	x8, x8, #0x1e8
400880a0: b8697908     	ldr	w8, [x8, x9, lsl #2]
400880a4: b90003e8     	str	w8, [sp]
400880a8: b94003e8     	ldr	w8, [sp]
400880ac: 37f801e8     	tbnz	w8, #0x1f, 0x400880e8 <current_process+0x88>
400880b0: 14000001     	b	0x400880b4 <current_process+0x54>
400880b4: b94003e8     	ldr	w8, [sp]
400880b8: 7100fd08     	subs	w8, w8, #0x3f
400880bc: 5400016c     	b.gt	0x400880e8 <current_process+0x88>
400880c0: 14000001     	b	0x400880c4 <current_process+0x64>
400880c4: b98003e8     	ldrsw	x8, [sp]
400880c8: 52803b09     	mov	w9, #0x1d8              // =472
400880cc: 2a0903e0     	mov	w0, w9
400880d0: 2a0003e9     	mov	w9, w0
400880d4: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400880d8: 912fa14a     	add	x10, x10, #0xbe8
400880dc: 9b292908     	smaddl	x8, w8, w9, x10
400880e0: f90007e8     	str	x8, [sp, #0x8]
400880e4: 14000004     	b	0x400880f4 <current_process+0x94>
400880e8: aa1f03e8     	mov	x8, xzr
400880ec: f90007e8     	str	x8, [sp, #0x8]
400880f0: 14000001     	b	0x400880f4 <current_process+0x94>
400880f4: f94007e0     	ldr	x0, [sp, #0x8]
400880f8: a9417bfd     	ldp	x29, x30, [sp, #0x10]
400880fc: 910083ff     	add	sp, sp, #0x20
40088100: d65f03c0     	ret
40088104: d503201f     	nop
40088108: d503201f     	nop
4008810c: d503201f     	nop

0000000040088110 <process_get_phys_base>:
40088110: d10103ff     	sub	sp, sp, #0x40
40088114: a9037bfd     	stp	x29, x30, [sp, #0x30]
40088118: 9100c3fd     	add	x29, sp, #0x30
4008811c: b81f43a0     	stur	w0, [x29, #-0xc]
40088120: b85f43a8     	ldur	w8, [x29, #-0xc]
40088124: 37f800c8     	tbnz	w8, #0x1f, 0x4008813c <process_get_phys_base+0x2c>
40088128: 14000001     	b	0x4008812c <process_get_phys_base+0x1c>
4008812c: b85f43a8     	ldur	w8, [x29, #-0xc]
40088130: 71010108     	subs	w8, w8, #0x40
40088134: 540000ab     	b.lt	0x40088148 <process_get_phys_base+0x38>
40088138: 14000001     	b	0x4008813c <process_get_phys_base+0x2c>
4008813c: aa1f03e8     	mov	x8, xzr
40088140: f81f83a8     	stur	x8, [x29, #-0x8]
40088144: 14000016     	b	0x4008819c <process_get_phys_base+0x8c>
40088148: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008814c: 912f8000     	add	x0, x0, #0xbe0
40088150: f90007e0     	str	x0, [sp, #0x8]
40088154: 97fff12b     	bl	0x40084600 <spinlock_acquire_irqsave>
40088158: aa0003e8     	mov	x8, x0
4008815c: f94007e0     	ldr	x0, [sp, #0x8]
40088160: f9000fe8     	str	x8, [sp, #0x18]
40088164: b89f43a8     	ldursw	x8, [x29, #-0xc]
40088168: 52803b09     	mov	w9, #0x1d8              // =472
4008816c: 2a0903e1     	mov	w1, w9
40088170: 2a0103e9     	mov	w9, w1
40088174: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088178: 912fa14a     	add	x10, x10, #0xbe8
4008817c: 9b292908     	smaddl	x8, w8, w9, x10
40088180: f940a508     	ldr	x8, [x8, #0x148]
40088184: f9000be8     	str	x8, [sp, #0x10]
40088188: f9400fe1     	ldr	x1, [sp, #0x18]
4008818c: 97fff12d     	bl	0x40084640 <spinlock_release_irqrestore>
40088190: f9400be8     	ldr	x8, [sp, #0x10]
40088194: f81f83a8     	stur	x8, [x29, #-0x8]
40088198: 14000001     	b	0x4008819c <process_get_phys_base+0x8c>
4008819c: f85f83a0     	ldur	x0, [x29, #-0x8]
400881a0: a9437bfd     	ldp	x29, x30, [sp, #0x30]
400881a4: 910103ff     	add	sp, sp, #0x40
400881a8: d65f03c0     	ret
400881ac: d503201f     	nop

00000000400881b0 <process_set_entry>:
400881b0: d10103ff     	sub	sp, sp, #0x40
400881b4: a9037bfd     	stp	x29, x30, [sp, #0x30]
400881b8: 9100c3fd     	add	x29, sp, #0x30
400881bc: b81fc3a0     	stur	w0, [x29, #-0x4]
400881c0: f81f03a1     	stur	x1, [x29, #-0x10]
400881c4: f9000fe2     	str	x2, [sp, #0x18]
400881c8: b85fc3a8     	ldur	w8, [x29, #-0x4]
400881cc: 37f800c8     	tbnz	w8, #0x1f, 0x400881e4 <process_set_entry+0x34>
400881d0: 14000001     	b	0x400881d4 <process_set_entry+0x24>
400881d4: b85fc3a8     	ldur	w8, [x29, #-0x4]
400881d8: 71010108     	subs	w8, w8, #0x40
400881dc: 5400006b     	b.lt	0x400881e8 <process_set_entry+0x38>
400881e0: 14000001     	b	0x400881e4 <process_set_entry+0x34>
400881e4: 14000021     	b	0x40088268 <process_set_entry+0xb8>
400881e8: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400881ec: 912f8000     	add	x0, x0, #0xbe0
400881f0: f90007e0     	str	x0, [sp, #0x8]
400881f4: 97fff103     	bl	0x40084600 <spinlock_acquire_irqsave>
400881f8: aa0003e8     	mov	x8, x0
400881fc: f94007e0     	ldr	x0, [sp, #0x8]
40088200: f9000be8     	str	x8, [sp, #0x10]
40088204: f85f03a8     	ldur	x8, [x29, #-0x10]
40088208: b89fc3aa     	ldursw	x10, [x29, #-0x4]
4008820c: 52803b09     	mov	w9, #0x1d8              // =472
40088210: 2a0903e1     	mov	w1, w9
40088214: 2a0103e9     	mov	w9, w1
40088218: 2a0a03eb     	mov	w11, w10
4008821c: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088220: 912fa14a     	add	x10, x10, #0xbe8
40088224: 9b29296b     	smaddl	x11, w11, w9, x10
40088228: f9009568     	str	x8, [x11, #0x128]
4008822c: f9400fe8     	ldr	x8, [sp, #0x18]
40088230: b89fc3ab     	ldursw	x11, [x29, #-0x4]
40088234: 9b29296b     	smaddl	x11, w11, w9, x10
40088238: f9009d68     	str	x8, [x11, #0x138]
4008823c: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40088240: 9b29290b     	smaddl	x11, w8, w9, x10
40088244: aa1f03e8     	mov	x8, xzr
40088248: f9009968     	str	x8, [x11, #0x130]
4008824c: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40088250: 9b292909     	smaddl	x9, w8, w9, x10
40088254: 52800048     	mov	w8, #0x2                // =2
40088258: b9000528     	str	w8, [x9, #0x4]
4008825c: f9400be1     	ldr	x1, [sp, #0x10]
40088260: 97fff0f8     	bl	0x40084640 <spinlock_release_irqrestore>
40088264: 14000001     	b	0x40088268 <process_set_entry+0xb8>
40088268: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008826c: 910103ff     	add	sp, sp, #0x40
40088270: d65f03c0     	ret
40088274: d503201f     	nop
40088278: d503201f     	nop
4008827c: d503201f     	nop

0000000040088280 <process_create>:
40088280: d10103ff     	sub	sp, sp, #0x40
40088284: a9037bfd     	stp	x29, x30, [sp, #0x30]
40088288: 9100c3fd     	add	x29, sp, #0x30
4008828c: d503201f     	nop
40088290: 1002b260     	adr	x0, 0x4008d8dc <UART0_FR+0x59c>
40088294: 97fff153     	bl	0x400847e0 <uart_puts>
40088298: 12800008     	mov	w8, #-0x1               // =-1
4008829c: b81f83a8     	stur	w8, [x29, #-0x8]
400882a0: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400882a4: 912f8000     	add	x0, x0, #0xbe0
400882a8: 97fff0d6     	bl	0x40084600 <spinlock_acquire_irqsave>
400882ac: f81f03a0     	stur	x0, [x29, #-0x10]
400882b0: 2a1f03e8     	mov	w8, wzr
400882b4: b81ec3a8     	stur	w8, [x29, #-0x14]
400882b8: 14000001     	b	0x400882bc <process_create+0x3c>
400882bc: b85ec3a8     	ldur	w8, [x29, #-0x14]
400882c0: 7100fd08     	subs	w8, w8, #0x3f
400882c4: 540003ac     	b.gt	0x40088338 <process_create+0xb8>
400882c8: 14000001     	b	0x400882cc <process_create+0x4c>
400882cc: b89ec3a8     	ldursw	x8, [x29, #-0x14]
400882d0: 52803b09     	mov	w9, #0x1d8              // =472
400882d4: 2a0903e0     	mov	w0, w9
400882d8: 2a0003e9     	mov	w9, w0
400882dc: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400882e0: 912fa14a     	add	x10, x10, #0xbe8
400882e4: 9b292908     	smaddl	x8, w8, w9, x10
400882e8: b9400508     	ldr	w8, [x8, #0x4]
400882ec: 350001c8     	cbnz	w8, 0x40088324 <process_create+0xa4>
400882f0: 14000001     	b	0x400882f4 <process_create+0x74>
400882f4: b85ec3a8     	ldur	w8, [x29, #-0x14]
400882f8: b81f83a8     	stur	w8, [x29, #-0x8]
400882fc: b89ec3a8     	ldursw	x8, [x29, #-0x14]
40088300: 52803b09     	mov	w9, #0x1d8              // =472
40088304: 2a0903e0     	mov	w0, w9
40088308: 2a0003e9     	mov	w9, w0
4008830c: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088310: 912fa14a     	add	x10, x10, #0xbe8
40088314: 9b292909     	smaddl	x9, w8, w9, x10
40088318: 52800028     	mov	w8, #0x1                // =1
4008831c: b9000528     	str	w8, [x9, #0x4]
40088320: 14000006     	b	0x40088338 <process_create+0xb8>
40088324: 14000001     	b	0x40088328 <process_create+0xa8>
40088328: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008832c: 11000508     	add	w8, w8, #0x1
40088330: b81ec3a8     	stur	w8, [x29, #-0x14]
40088334: 17ffffe2     	b	0x400882bc <process_create+0x3c>
40088338: f85f03a1     	ldur	x1, [x29, #-0x10]
4008833c: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40088340: 912f8000     	add	x0, x0, #0xbe0
40088344: 97fff0bf     	bl	0x40084640 <spinlock_release_irqrestore>
40088348: b0000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008834c: 910e6000     	add	x0, x0, #0x398
40088350: 97fff124     	bl	0x400847e0 <uart_puts>
40088354: b85f83a0     	ldur	w0, [x29, #-0x8]
40088358: 97fff13e     	bl	0x40084850 <print_int>
4008835c: b85f83a8     	ldur	w8, [x29, #-0x8]
40088360: 36f80108     	tbz	w8, #0x1f, 0x40088380 <process_create+0x100>
40088364: 14000001     	b	0x40088368 <process_create+0xe8>
40088368: b0000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008836c: 91111000     	add	x0, x0, #0x444
40088370: 97fff11c     	bl	0x400847e0 <uart_puts>
40088374: 12800008     	mov	w8, #-0x1               // =-1
40088378: b81fc3a8     	stur	w8, [x29, #-0x4]
4008837c: 14000059     	b	0x400884e0 <process_create+0x260>
40088380: b89f83a8     	ldursw	x8, [x29, #-0x8]
40088384: 52803b09     	mov	w9, #0x1d8              // =472
40088388: 2a0903e0     	mov	w0, w9
4008838c: 2a0003e9     	mov	w9, w0
40088390: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088394: 912fa14a     	add	x10, x10, #0xbe8
40088398: 9b292908     	smaddl	x8, w8, w9, x10
4008839c: f9000be8     	str	x8, [sp, #0x10]
400883a0: f9400be9     	ldr	x9, [sp, #0x10]
400883a4: 12800008     	mov	w8, #-0x1               // =-1
400883a8: b9000928     	str	w8, [x9, #0x8]
400883ac: f9400be9     	ldr	x9, [sp, #0x10]
400883b0: 2a1f03e8     	mov	w8, wzr
400883b4: b9000d28     	str	w8, [x9, #0xc]
400883b8: b9000fe8     	str	w8, [sp, #0xc]
400883bc: 14000001     	b	0x400883c0 <process_create+0x140>
400883c0: b9400fe8     	ldr	w8, [sp, #0xc]
400883c4: 71007d08     	subs	w8, w8, #0x1f
400883c8: 5400018c     	b.gt	0x400883f8 <process_create+0x178>
400883cc: 14000001     	b	0x400883d0 <process_create+0x150>
400883d0: f9400be8     	ldr	x8, [sp, #0x10]
400883d4: b9800fe9     	ldrsw	x9, [sp, #0xc]
400883d8: 8b090109     	add	x9, x8, x9
400883dc: 2a1f03e8     	mov	w8, wzr
400883e0: 39004128     	strb	w8, [x9, #0x10]
400883e4: 14000001     	b	0x400883e8 <process_create+0x168>
400883e8: b9400fe8     	ldr	w8, [sp, #0xc]
400883ec: 11000508     	add	w8, w8, #0x1
400883f0: b9000fe8     	str	w8, [sp, #0xc]
400883f4: 17fffff3     	b	0x400883c0 <process_create+0x140>
400883f8: f9400be9     	ldr	x9, [sp, #0x10]
400883fc: 2a1f03e8     	mov	w8, wzr
40088400: b901d128     	str	w8, [x9, #0x1d0]
40088404: b9000be8     	str	w8, [sp, #0x8]
40088408: 14000001     	b	0x4008840c <process_create+0x18c>
4008840c: b9400be8     	ldr	w8, [sp, #0x8]
40088410: 71007d08     	subs	w8, w8, #0x1f
40088414: 5400018c     	b.gt	0x40088444 <process_create+0x1c4>
40088418: 14000001     	b	0x4008841c <process_create+0x19c>
4008841c: f9400be8     	ldr	x8, [sp, #0x10]
40088420: b9800be9     	ldrsw	x9, [sp, #0x8]
40088424: 8b090909     	add	x9, x8, x9, lsl #2
40088428: 12800008     	mov	w8, #-0x1               // =-1
4008842c: b9015128     	str	w8, [x9, #0x150]
40088430: 14000001     	b	0x40088434 <process_create+0x1b4>
40088434: b9400be8     	ldr	w8, [sp, #0x8]
40088438: 11000508     	add	w8, w8, #0x1
4008843c: b9000be8     	str	w8, [sp, #0x8]
40088440: 17fffff3     	b	0x4008840c <process_create+0x18c>
40088444: b0000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40088448: 9111d400     	add	x0, x0, #0x475
4008844c: 97fff0e5     	bl	0x400847e0 <uart_puts>
40088450: f9400be8     	ldr	x8, [sp, #0x10]
40088454: b9414900     	ldr	w0, [x8, #0x148]
40088458: 97fff0fe     	bl	0x40084850 <print_int>
4008845c: b0000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40088460: 9122c000     	add	x0, x0, #0x8b0
40088464: 97fff0df     	bl	0x400847e0 <uart_puts>
40088468: f9400be8     	ldr	x8, [sp, #0x10]
4008846c: f940a500     	ldr	x0, [x8, #0x148]
40088470: 52a00408     	mov	w8, #0x200000           // =2097152
40088474: 2a0803e2     	mov	w2, w8
40088478: 2a1f03e1     	mov	w1, wzr
4008847c: b90003e1     	str	w1, [sp]
40088480: 9400001c     	bl	0x400884f0 <kmemset>
40088484: b0000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40088488: 911ab000     	add	x0, x0, #0x6ac
4008848c: 97fff0d5     	bl	0x400847e0 <uart_puts>
40088490: b94003e8     	ldr	w8, [sp]
40088494: b90007e8     	str	w8, [sp, #0x4]
40088498: 14000001     	b	0x4008849c <process_create+0x21c>
4008849c: b94007e8     	ldr	w8, [sp, #0x4]
400884a0: 71008508     	subs	w8, w8, #0x21
400884a4: 5400018c     	b.gt	0x400884d4 <process_create+0x254>
400884a8: 14000001     	b	0x400884ac <process_create+0x22c>
400884ac: f9400be8     	ldr	x8, [sp, #0x10]
400884b0: b98007e9     	ldrsw	x9, [sp, #0x4]
400884b4: 8b090d09     	add	x9, x8, x9, lsl #3
400884b8: aa1f03e8     	mov	x8, xzr
400884bc: f9001928     	str	x8, [x9, #0x30]
400884c0: 14000001     	b	0x400884c4 <process_create+0x244>
400884c4: b94007e8     	ldr	w8, [sp, #0x4]
400884c8: 11000508     	add	w8, w8, #0x1
400884cc: b90007e8     	str	w8, [sp, #0x4]
400884d0: 17fffff3     	b	0x4008849c <process_create+0x21c>
400884d4: b85f83a8     	ldur	w8, [x29, #-0x8]
400884d8: b81fc3a8     	stur	w8, [x29, #-0x4]
400884dc: 14000001     	b	0x400884e0 <process_create+0x260>
400884e0: b85fc3a0     	ldur	w0, [x29, #-0x4]
400884e4: a9437bfd     	ldp	x29, x30, [sp, #0x30]
400884e8: 910103ff     	add	sp, sp, #0x40
400884ec: d65f03c0     	ret

00000000400884f0 <kmemset>:
400884f0: d10143ff     	sub	sp, sp, #0x50
400884f4: f90027e0     	str	x0, [sp, #0x48]
400884f8: 39011fe1     	strb	w1, [sp, #0x47]
400884fc: f9001fe2     	str	x2, [sp, #0x38]
40088500: aa1f03e8     	mov	x8, xzr
40088504: f9001be8     	str	x8, [sp, #0x30]
40088508: 2a1f03e8     	mov	w8, wzr
4008850c: b9002fe8     	str	w8, [sp, #0x2c]
40088510: 14000001     	b	0x40088514 <kmemset+0x24>
40088514: b9402fe8     	ldr	w8, [sp, #0x2c]
40088518: 71001d08     	subs	w8, w8, #0x7
4008851c: 540001cc     	b.gt	0x40088554 <kmemset+0x64>
40088520: 14000001     	b	0x40088524 <kmemset+0x34>
40088524: 39411fe8     	ldrb	w8, [sp, #0x47]
40088528: b9402fe9     	ldr	w9, [sp, #0x2c]
4008852c: 531d7129     	lsl	w9, w9, #3
40088530: 9ac92109     	lsl	x9, x8, x9
40088534: f9401be8     	ldr	x8, [sp, #0x30]
40088538: aa090108     	orr	x8, x8, x9
4008853c: f9001be8     	str	x8, [sp, #0x30]
40088540: 14000001     	b	0x40088544 <kmemset+0x54>
40088544: b9402fe8     	ldr	w8, [sp, #0x2c]
40088548: 11000508     	add	w8, w8, #0x1
4008854c: b9002fe8     	str	w8, [sp, #0x2c]
40088550: 17fffff1     	b	0x40088514 <kmemset+0x24>
40088554: f94027e8     	ldr	x8, [sp, #0x48]
40088558: f90013e8     	str	x8, [sp, #0x20]
4008855c: aa1f03e8     	mov	x8, xzr
40088560: f9000fe8     	str	x8, [sp, #0x18]
40088564: 14000001     	b	0x40088568 <kmemset+0x78>
40088568: f9400fe8     	ldr	x8, [sp, #0x18]
4008856c: f9401fe9     	ldr	x9, [sp, #0x38]
40088570: eb490d08     	subs	x8, x8, x9, lsr #3
40088574: 54000162     	b.hs	0x400885a0 <kmemset+0xb0>
40088578: 14000001     	b	0x4008857c <kmemset+0x8c>
4008857c: f9401be8     	ldr	x8, [sp, #0x30]
40088580: f94013e9     	ldr	x9, [sp, #0x20]
40088584: f9400fea     	ldr	x10, [sp, #0x18]
40088588: f82a7928     	str	x8, [x9, x10, lsl #3]
4008858c: 14000001     	b	0x40088590 <kmemset+0xa0>
40088590: f9400fe8     	ldr	x8, [sp, #0x18]
40088594: 91000508     	add	x8, x8, #0x1
40088598: f9000fe8     	str	x8, [sp, #0x18]
4008859c: 17fffff3     	b	0x40088568 <kmemset+0x78>
400885a0: f94027e8     	ldr	x8, [sp, #0x48]
400885a4: f9000be8     	str	x8, [sp, #0x10]
400885a8: f9400fe8     	ldr	x8, [sp, #0x18]
400885ac: d37df108     	lsl	x8, x8, #3
400885b0: f90007e8     	str	x8, [sp, #0x8]
400885b4: 14000001     	b	0x400885b8 <kmemset+0xc8>
400885b8: f94007e8     	ldr	x8, [sp, #0x8]
400885bc: f9401fe9     	ldr	x9, [sp, #0x38]
400885c0: eb090108     	subs	x8, x8, x9
400885c4: 54000162     	b.hs	0x400885f0 <kmemset+0x100>
400885c8: 14000001     	b	0x400885cc <kmemset+0xdc>
400885cc: 39411fe8     	ldrb	w8, [sp, #0x47]
400885d0: f9400be9     	ldr	x9, [sp, #0x10]
400885d4: f94007ea     	ldr	x10, [sp, #0x8]
400885d8: 382a6928     	strb	w8, [x9, x10]
400885dc: 14000001     	b	0x400885e0 <kmemset+0xf0>
400885e0: f94007e8     	ldr	x8, [sp, #0x8]
400885e4: 91000508     	add	x8, x8, #0x1
400885e8: f90007e8     	str	x8, [sp, #0x8]
400885ec: 17fffff3     	b	0x400885b8 <kmemset+0xc8>
400885f0: 910143ff     	add	sp, sp, #0x50
400885f4: d65f03c0     	ret
400885f8: d503201f     	nop
400885fc: d503201f     	nop

0000000040088600 <process_create_kernel>:
40088600: d10103ff     	sub	sp, sp, #0x40
40088604: a9037bfd     	stp	x29, x30, [sp, #0x30]
40088608: 9100c3fd     	add	x29, sp, #0x30
4008860c: f81f03a0     	stur	x0, [x29, #-0x10]
40088610: f9000fe1     	str	x1, [sp, #0x18]
40088614: 97ffff1b     	bl	0x40088280 <process_create>
40088618: b90017e0     	str	w0, [sp, #0x14]
4008861c: b94017e8     	ldr	w8, [sp, #0x14]
40088620: 36f800a8     	tbz	w8, #0x1f, 0x40088634 <process_create_kernel+0x34>
40088624: 14000001     	b	0x40088628 <process_create_kernel+0x28>
40088628: 12800008     	mov	w8, #-0x1               // =-1
4008862c: b81fc3a8     	stur	w8, [x29, #-0x4]
40088630: 1400001f     	b	0x400886ac <process_create_kernel+0xac>
40088634: b98017e8     	ldrsw	x8, [sp, #0x14]
40088638: 52803b09     	mov	w9, #0x1d8              // =472
4008863c: 2a0903e0     	mov	w0, w9
40088640: 2a0003e9     	mov	w9, w0
40088644: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088648: 912fa14a     	add	x10, x10, #0xbe8
4008864c: 9b292908     	smaddl	x8, w8, w9, x10
40088650: f90007e8     	str	x8, [sp, #0x8]
40088654: f94007e9     	ldr	x9, [sp, #0x8]
40088658: 52800028     	mov	w8, #0x1                // =1
4008865c: b9000d28     	str	w8, [x9, #0xc]
40088660: f85f03a8     	ldur	x8, [x29, #-0x10]
40088664: f94007e9     	ldr	x9, [sp, #0x8]
40088668: f9009528     	str	x8, [x9, #0x128]
4008866c: f94007e9     	ldr	x9, [sp, #0x8]
40088670: f940a528     	ldr	x8, [x9, #0x148]
40088674: 91480108     	add	x8, x8, #0x200, lsl #12 // =0x200000
40088678: f9009d28     	str	x8, [x9, #0x138]
4008867c: f94007e9     	ldr	x9, [sp, #0x8]
40088680: 52800088     	mov	w8, #0x4                // =4
40088684: f9009928     	str	x8, [x9, #0x130]
40088688: f9400fe8     	ldr	x8, [sp, #0x18]
4008868c: f94007e9     	ldr	x9, [sp, #0x8]
40088690: f9001928     	str	x8, [x9, #0x30]
40088694: f94007e9     	ldr	x9, [sp, #0x8]
40088698: 52800048     	mov	w8, #0x2                // =2
4008869c: b9000528     	str	w8, [x9, #0x4]
400886a0: b94017e8     	ldr	w8, [sp, #0x14]
400886a4: b81fc3a8     	stur	w8, [x29, #-0x4]
400886a8: 14000001     	b	0x400886ac <process_create_kernel+0xac>
400886ac: b85fc3a0     	ldur	w0, [x29, #-0x4]
400886b0: a9437bfd     	ldp	x29, x30, [sp, #0x30]
400886b4: 910103ff     	add	sp, sp, #0x40
400886b8: d65f03c0     	ret
400886bc: d503201f     	nop

00000000400886c0 <schedule>:
400886c0: d10603ff     	sub	sp, sp, #0x180
400886c4: a9167bfd     	stp	x29, x30, [sp, #0x160]
400886c8: f900bbfc     	str	x28, [sp, #0x170]
400886cc: 910583fd     	add	x29, sp, #0x160
400886d0: f81f83a0     	stur	x0, [x29, #-0x8]
400886d4: b81f43a1     	stur	w1, [x29, #-0xc]
400886d8: b0000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
400886dc: b941f908     	ldr	w8, [x8, #0x1f8]
400886e0: 35000068     	cbnz	w8, 0x400886ec <schedule+0x2c>
400886e4: 14000001     	b	0x400886e8 <schedule+0x28>
400886e8: 140000f9     	b	0x40088acc <schedule+0x40c>
400886ec: 94000621     	bl	0x40089f70 <get_cpuid>
400886f0: b81f03a0     	stur	w0, [x29, #-0x10]
400886f4: b85f03a8     	ldur	w8, [x29, #-0x10]
400886f8: 71001108     	subs	w8, w8, #0x4
400886fc: 54000063     	b.lo	0x40088708 <schedule+0x48>
40088700: 14000001     	b	0x40088704 <schedule+0x44>
40088704: 140000f2     	b	0x40088acc <schedule+0x40c>
40088708: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008870c: 912f8000     	add	x0, x0, #0xbe0
40088710: 97ffefbc     	bl	0x40084600 <spinlock_acquire_irqsave>
40088714: f81e83a0     	stur	x0, [x29, #-0x18]
40088718: b85f03a8     	ldur	w8, [x29, #-0x10]
4008871c: 2a0803e9     	mov	w9, w8
40088720: b0000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
40088724: 9107a108     	add	x8, x8, #0x1e8
40088728: b8697908     	ldr	w8, [x8, x9, lsl #2]
4008872c: b81e43a8     	stur	w8, [x29, #-0x1c]
40088730: b85e43a8     	ldur	w8, [x29, #-0x1c]
40088734: 37f804c8     	tbnz	w8, #0x1f, 0x400887cc <schedule+0x10c>
40088738: 14000001     	b	0x4008873c <schedule+0x7c>
4008873c: b89e43a8     	ldursw	x8, [x29, #-0x1c]
40088740: 52803b09     	mov	w9, #0x1d8              // =472
40088744: 2a0903e0     	mov	w0, w9
40088748: 2a0003e9     	mov	w9, w0
4008874c: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088750: 912fa14a     	add	x10, x10, #0xbe8
40088754: 9b292908     	smaddl	x8, w8, w9, x10
40088758: f81d83a8     	stur	x8, [x29, #-0x28]
4008875c: f85d83a8     	ldur	x8, [x29, #-0x28]
40088760: b9400508     	ldr	w8, [x8, #0x4]
40088764: 71000d08     	subs	w8, w8, #0x3
40088768: 54000121     	b.ne	0x4008878c <schedule+0xcc>
4008876c: 14000001     	b	0x40088770 <schedule+0xb0>
40088770: f85d83a0     	ldur	x0, [x29, #-0x28]
40088774: f85f83a1     	ldur	x1, [x29, #-0x8]
40088778: 940000da     	bl	0x40088ae0 <save_context>
4008877c: f85d83a9     	ldur	x9, [x29, #-0x28]
40088780: 52800048     	mov	w8, #0x2                // =2
40088784: b9000528     	str	w8, [x9, #0x4]
40088788: 14000010     	b	0x400887c8 <schedule+0x108>
4008878c: f85d83a8     	ldur	x8, [x29, #-0x28]
40088790: b9400508     	ldr	w8, [x8, #0x4]
40088794: 71001508     	subs	w8, w8, #0x5
40088798: 540000e0     	b.eq	0x400887b4 <schedule+0xf4>
4008879c: 14000001     	b	0x400887a0 <schedule+0xe0>
400887a0: f85d83a8     	ldur	x8, [x29, #-0x28]
400887a4: b9400508     	ldr	w8, [x8, #0x4]
400887a8: 71001908     	subs	w8, w8, #0x6
400887ac: 540000c1     	b.ne	0x400887c4 <schedule+0x104>
400887b0: 14000001     	b	0x400887b4 <schedule+0xf4>
400887b4: f85d83a0     	ldur	x0, [x29, #-0x28]
400887b8: f85f83a1     	ldur	x1, [x29, #-0x8]
400887bc: 940000c9     	bl	0x40088ae0 <save_context>
400887c0: 14000001     	b	0x400887c4 <schedule+0x104>
400887c4: 14000001     	b	0x400887c8 <schedule+0x108>
400887c8: 14000001     	b	0x400887cc <schedule+0x10c>
400887cc: 14000001     	b	0x400887d0 <schedule+0x110>
400887d0: 12800008     	mov	w8, #-0x1               // =-1
400887d4: b81d43a8     	stur	w8, [x29, #-0x2c]
400887d8: b85e43a8     	ldur	w8, [x29, #-0x1c]
400887dc: 37f800a8     	tbnz	w8, #0x1f, 0x400887f0 <schedule+0x130>
400887e0: 14000001     	b	0x400887e4 <schedule+0x124>
400887e4: b85e43a8     	ldur	w8, [x29, #-0x1c]
400887e8: b9000fe8     	str	w8, [sp, #0xc]
400887ec: 14000004     	b	0x400887fc <schedule+0x13c>
400887f0: 2a1f03e8     	mov	w8, wzr
400887f4: b9000fe8     	str	w8, [sp, #0xc]
400887f8: 14000001     	b	0x400887fc <schedule+0x13c>
400887fc: b9400fe8     	ldr	w8, [sp, #0xc]
40088800: b81d03a8     	stur	w8, [x29, #-0x30]
40088804: 52800028     	mov	w8, #0x1                // =1
40088808: b81cc3a8     	stur	w8, [x29, #-0x34]
4008880c: 14000001     	b	0x40088810 <schedule+0x150>
40088810: b85cc3a8     	ldur	w8, [x29, #-0x34]
40088814: 71010108     	subs	w8, w8, #0x40
40088818: 540003cc     	b.gt	0x40088890 <schedule+0x1d0>
4008881c: 14000001     	b	0x40088820 <schedule+0x160>
40088820: b85d03a8     	ldur	w8, [x29, #-0x30]
40088824: b85cc3a9     	ldur	w9, [x29, #-0x34]
40088828: 0b09010a     	add	w10, w8, w9
4008882c: 12001548     	and	w8, w10, #0x3f
40088830: 2a1f03e9     	mov	w9, wzr
40088834: 6b0a0129     	subs	w9, w9, w10
40088838: 12001529     	and	w9, w9, #0x3f
4008883c: 5a894508     	csneg	w8, w8, w9, mi
40088840: b81c83a8     	stur	w8, [x29, #-0x38]
40088844: b89c83a8     	ldursw	x8, [x29, #-0x38]
40088848: 52803b09     	mov	w9, #0x1d8              // =472
4008884c: 2a0903e0     	mov	w0, w9
40088850: 2a0003e9     	mov	w9, w0
40088854: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088858: 912fa14a     	add	x10, x10, #0xbe8
4008885c: 9b292908     	smaddl	x8, w8, w9, x10
40088860: b9400508     	ldr	w8, [x8, #0x4]
40088864: 71000908     	subs	w8, w8, #0x2
40088868: 540000a1     	b.ne	0x4008887c <schedule+0x1bc>
4008886c: 14000001     	b	0x40088870 <schedule+0x1b0>
40088870: b85c83a8     	ldur	w8, [x29, #-0x38]
40088874: b81d43a8     	stur	w8, [x29, #-0x2c]
40088878: 14000006     	b	0x40088890 <schedule+0x1d0>
4008887c: 14000001     	b	0x40088880 <schedule+0x1c0>
40088880: b85cc3a8     	ldur	w8, [x29, #-0x34]
40088884: 11000508     	add	w8, w8, #0x1
40088888: b81cc3a8     	stur	w8, [x29, #-0x34]
4008888c: 17ffffe1     	b	0x40088810 <schedule+0x150>
40088890: b85d43a8     	ldur	w8, [x29, #-0x2c]
40088894: 37f80748     	tbnz	w8, #0x1f, 0x4008897c <schedule+0x2bc>
40088898: 14000001     	b	0x4008889c <schedule+0x1dc>
4008889c: b85d43a8     	ldur	w8, [x29, #-0x2c]
400888a0: b85f03a9     	ldur	w9, [x29, #-0x10]
400888a4: 2a0903ea     	mov	w10, w9
400888a8: b0000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
400888ac: 9107a129     	add	x9, x9, #0x1e8
400888b0: b82a7928     	str	w8, [x9, x10, lsl #2]
400888b4: b89d43a8     	ldursw	x8, [x29, #-0x2c]
400888b8: 52803b09     	mov	w9, #0x1d8              // =472
400888bc: 2a0903e0     	mov	w0, w9
400888c0: 2a0003e9     	mov	w9, w0
400888c4: b9000be9     	str	w9, [sp, #0x8]
400888c8: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400888cc: 912fa14a     	add	x10, x10, #0xbe8
400888d0: f90003ea     	str	x10, [sp]
400888d4: 9b29290b     	smaddl	x11, w8, w9, x10
400888d8: 52800068     	mov	w8, #0x3                // =3
400888dc: b9000568     	str	w8, [x11, #0x4]
400888e0: b89d43a8     	ldursw	x8, [x29, #-0x2c]
400888e4: 9b292900     	smaddl	x0, w8, w9, x10
400888e8: 910083e1     	add	x1, sp, #0x20
400888ec: 940000a9     	bl	0x40088b90 <restore_context>
400888f0: f94003ea     	ldr	x10, [sp]
400888f4: b9400be9     	ldr	w9, [sp, #0x8]
400888f8: b85f03a8     	ldur	w8, [x29, #-0x10]
400888fc: 53103d08     	lsl	w8, w8, #16
40088900: 2a0803e8     	mov	w8, w8
40088904: 2a0803eb     	mov	w11, w8
40088908: b00046a8     	adrp	x8, 0x4095d000 <__bss_end+0x3ffb0>
4008890c: 91014108     	add	x8, x8, #0x50
40088910: eb0b0108     	subs	x8, x8, x11
40088914: f9000fe8     	str	x8, [sp, #0x18]
40088918: b89d43a8     	ldursw	x8, [x29, #-0x2c]
4008891c: 9b292908     	smaddl	x8, w8, w9, x10
40088920: f940a500     	ldr	x0, [x8, #0x148]
40088924: 97fff1bb     	bl	0x40085010 <mmu_switch_user_mapping>
40088928: f85e83a1     	ldur	x1, [x29, #-0x18]
4008892c: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40088930: 912f8000     	add	x0, x0, #0xbe0
40088934: 97ffef43     	bl	0x40084640 <spinlock_release_irqrestore>
40088938: b85f43a8     	ldur	w8, [x29, #-0xc]
4008893c: 34000168     	cbz	w8, 0x40088968 <schedule+0x2a8>
40088940: 14000001     	b	0x40088944 <schedule+0x284>
40088944: b85d43a8     	ldur	w8, [x29, #-0x2c]
40088948: b85e43a9     	ldur	w9, [x29, #-0x1c]
4008894c: 6b090108     	subs	w8, w8, w9
40088950: 540000c1     	b.ne	0x40088968 <schedule+0x2a8>
40088954: 14000001     	b	0x40088958 <schedule+0x298>
40088958: d50342ff     	msr	DAIFClr, #0x2
4008895c: d503207f     	wfi
40088960: d50342df     	msr	DAIFSet, #0x2
40088964: 14000001     	b	0x40088968 <schedule+0x2a8>
40088968: f9400fe1     	ldr	x1, [sp, #0x18]
4008896c: 910083e0     	add	x0, sp, #0x20
40088970: 97ffe2e6     	bl	0x40081508 <enter_user_space>
40088974: 14000001     	b	0x40088978 <schedule+0x2b8>
40088978: 14000000     	b	0x40088978 <schedule+0x2b8>
4008897c: 2a1f03e8     	mov	w8, wzr
40088980: b90017e8     	str	w8, [sp, #0x14]
40088984: b90013e8     	str	w8, [sp, #0x10]
40088988: 14000001     	b	0x4008898c <schedule+0x2cc>
4008898c: b94013e8     	ldr	w8, [sp, #0x10]
40088990: 7100fd08     	subs	w8, w8, #0x3f
40088994: 5400054c     	b.gt	0x40088a3c <schedule+0x37c>
40088998: 14000001     	b	0x4008899c <schedule+0x2dc>
4008899c: b98013e8     	ldrsw	x8, [sp, #0x10]
400889a0: 52803b09     	mov	w9, #0x1d8              // =472
400889a4: 2a0903e0     	mov	w0, w9
400889a8: 2a0003e9     	mov	w9, w0
400889ac: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400889b0: 912fa14a     	add	x10, x10, #0xbe8
400889b4: 9b292908     	smaddl	x8, w8, w9, x10
400889b8: b9400508     	ldr	w8, [x8, #0x4]
400889bc: 34000368     	cbz	w8, 0x40088a28 <schedule+0x368>
400889c0: 14000001     	b	0x400889c4 <schedule+0x304>
400889c4: b98013e8     	ldrsw	x8, [sp, #0x10]
400889c8: 52803b09     	mov	w9, #0x1d8              // =472
400889cc: 2a0903e0     	mov	w0, w9
400889d0: 2a0003e9     	mov	w9, w0
400889d4: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400889d8: 912fa14a     	add	x10, x10, #0xbe8
400889dc: 9b292908     	smaddl	x8, w8, w9, x10
400889e0: b9400508     	ldr	w8, [x8, #0x4]
400889e4: 71001108     	subs	w8, w8, #0x4
400889e8: 54000200     	b.eq	0x40088a28 <schedule+0x368>
400889ec: 14000001     	b	0x400889f0 <schedule+0x330>
400889f0: b98013e8     	ldrsw	x8, [sp, #0x10]
400889f4: 52803b09     	mov	w9, #0x1d8              // =472
400889f8: 2a0903e0     	mov	w0, w9
400889fc: 2a0003e9     	mov	w9, w0
40088a00: b0000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40088a04: 912fa14a     	add	x10, x10, #0xbe8
40088a08: 9b292908     	smaddl	x8, w8, w9, x10
40088a0c: b9400508     	ldr	w8, [x8, #0x4]
40088a10: 71000508     	subs	w8, w8, #0x1
40088a14: 540000a0     	b.eq	0x40088a28 <schedule+0x368>
40088a18: 14000001     	b	0x40088a1c <schedule+0x35c>
40088a1c: 52800028     	mov	w8, #0x1                // =1
40088a20: b90017e8     	str	w8, [sp, #0x14]
40088a24: 14000006     	b	0x40088a3c <schedule+0x37c>
40088a28: 14000001     	b	0x40088a2c <schedule+0x36c>
40088a2c: b94013e8     	ldr	w8, [sp, #0x10]
40088a30: 11000508     	add	w8, w8, #0x1
40088a34: b90013e8     	str	w8, [sp, #0x10]
40088a38: 17ffffd5     	b	0x4008898c <schedule+0x2cc>
40088a3c: b94017e8     	ldr	w8, [sp, #0x14]
40088a40: 350002e8     	cbnz	w8, 0x40088a9c <schedule+0x3dc>
40088a44: 14000001     	b	0x40088a48 <schedule+0x388>
40088a48: b85f03a8     	ldur	w8, [x29, #-0x10]
40088a4c: 2a0803ea     	mov	w10, w8
40088a50: b0000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
40088a54: 9107a129     	add	x9, x9, #0x1e8
40088a58: 12800008     	mov	w8, #-0x1               // =-1
40088a5c: b82a7928     	str	w8, [x9, x10, lsl #2]
40088a60: f85e83a1     	ldur	x1, [x29, #-0x18]
40088a64: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40088a68: 912f8000     	add	x0, x0, #0xbe0
40088a6c: 97ffeef5     	bl	0x40084640 <spinlock_release_irqrestore>
40088a70: b85f03a8     	ldur	w8, [x29, #-0x10]
40088a74: 35000088     	cbnz	w8, 0x40088a84 <schedule+0x3c4>
40088a78: 14000001     	b	0x40088a7c <schedule+0x3bc>
40088a7c: 94000071     	bl	0x40088c40 <scheduler_finished>
40088a80: 14000006     	b	0x40088a98 <schedule+0x3d8>
40088a84: 14000001     	b	0x40088a88 <schedule+0x3c8>
40088a88: d50342ff     	msr	DAIFClr, #0x2
40088a8c: d503207f     	wfi
40088a90: d50342df     	msr	DAIFSet, #0x2
40088a94: 17fffffd     	b	0x40088a88 <schedule+0x3c8>
40088a98: 1400000d     	b	0x40088acc <schedule+0x40c>
40088a9c: b85f03a8     	ldur	w8, [x29, #-0x10]
40088aa0: 2a0803ea     	mov	w10, w8
40088aa4: b0000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
40088aa8: 9107a129     	add	x9, x9, #0x1e8
40088aac: 12800008     	mov	w8, #-0x1               // =-1
40088ab0: b82a7928     	str	w8, [x9, x10, lsl #2]
40088ab4: f85e83a1     	ldur	x1, [x29, #-0x18]
40088ab8: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40088abc: 912f8000     	add	x0, x0, #0xbe0
40088ac0: 97ffeee0     	bl	0x40084640 <spinlock_release_irqrestore>
40088ac4: 9400006f     	bl	0x40088c80 <kernel_thread_exit_jump>
40088ac8: 17ffff42     	b	0x400887d0 <schedule+0x110>
40088acc: f940bbfc     	ldr	x28, [sp, #0x170]
40088ad0: a9567bfd     	ldp	x29, x30, [sp, #0x160]
40088ad4: 910603ff     	add	sp, sp, #0x180
40088ad8: d65f03c0     	ret
40088adc: d503201f     	nop

0000000040088ae0 <save_context>:
40088ae0: d10083ff     	sub	sp, sp, #0x20
40088ae4: f9000fe0     	str	x0, [sp, #0x18]
40088ae8: f9000be1     	str	x1, [sp, #0x10]
40088aec: 2a1f03e8     	mov	w8, wzr
40088af0: b9000fe8     	str	w8, [sp, #0xc]
40088af4: 14000001     	b	0x40088af8 <save_context+0x18>
40088af8: b9400fe8     	ldr	w8, [sp, #0xc]
40088afc: 71007508     	subs	w8, w8, #0x1d
40088b00: 540001cc     	b.gt	0x40088b38 <save_context+0x58>
40088b04: 14000001     	b	0x40088b08 <save_context+0x28>
40088b08: f9400be8     	ldr	x8, [sp, #0x10]
40088b0c: b9800fe9     	ldrsw	x9, [sp, #0xc]
40088b10: d37df12a     	lsl	x10, x9, #3
40088b14: f86a6908     	ldr	x8, [x8, x10]
40088b18: f9400fe9     	ldr	x9, [sp, #0x18]
40088b1c: 8b0a0129     	add	x9, x9, x10
40088b20: f9001928     	str	x8, [x9, #0x30]
40088b24: 14000001     	b	0x40088b28 <save_context+0x48>
40088b28: b9400fe8     	ldr	w8, [sp, #0xc]
40088b2c: 11000508     	add	w8, w8, #0x1
40088b30: b9000fe8     	str	w8, [sp, #0xc]
40088b34: 17fffff1     	b	0x40088af8 <save_context+0x18>
40088b38: f9400be8     	ldr	x8, [sp, #0x10]
40088b3c: f9407908     	ldr	x8, [x8, #0xf0]
40088b40: f9400fe9     	ldr	x9, [sp, #0x18]
40088b44: f9009128     	str	x8, [x9, #0x120]
40088b48: f9400be8     	ldr	x8, [sp, #0x10]
40088b4c: f9407d08     	ldr	x8, [x8, #0xf8]
40088b50: f9400fe9     	ldr	x9, [sp, #0x18]
40088b54: f9009528     	str	x8, [x9, #0x128]
40088b58: f9400be8     	ldr	x8, [sp, #0x10]
40088b5c: f9408108     	ldr	x8, [x8, #0x100]
40088b60: f9400fe9     	ldr	x9, [sp, #0x18]
40088b64: f9009928     	str	x8, [x9, #0x130]
40088b68: d5384108     	mrs	x8, SP_EL0
40088b6c: f90003e8     	str	x8, [sp]
40088b70: f94003e8     	ldr	x8, [sp]
40088b74: f9400fe9     	ldr	x9, [sp, #0x18]
40088b78: f9009d28     	str	x8, [x9, #0x138]
40088b7c: 910083ff     	add	sp, sp, #0x20
40088b80: d65f03c0     	ret
40088b84: d503201f     	nop
40088b88: d503201f     	nop
40088b8c: d503201f     	nop

0000000040088b90 <restore_context>:
40088b90: d10083ff     	sub	sp, sp, #0x20
40088b94: f9000fe0     	str	x0, [sp, #0x18]
40088b98: f9000be1     	str	x1, [sp, #0x10]
40088b9c: 2a1f03e8     	mov	w8, wzr
40088ba0: b9000fe8     	str	w8, [sp, #0xc]
40088ba4: 14000001     	b	0x40088ba8 <restore_context+0x18>
40088ba8: b9400fe8     	ldr	w8, [sp, #0xc]
40088bac: 71007508     	subs	w8, w8, #0x1d
40088bb0: 540001cc     	b.gt	0x40088be8 <restore_context+0x58>
40088bb4: 14000001     	b	0x40088bb8 <restore_context+0x28>
40088bb8: f9400fe8     	ldr	x8, [sp, #0x18]
40088bbc: b9800fe9     	ldrsw	x9, [sp, #0xc]
40088bc0: d37df12a     	lsl	x10, x9, #3
40088bc4: 8b0a0108     	add	x8, x8, x10
40088bc8: f9401908     	ldr	x8, [x8, #0x30]
40088bcc: f9400be9     	ldr	x9, [sp, #0x10]
40088bd0: f82a6928     	str	x8, [x9, x10]
40088bd4: 14000001     	b	0x40088bd8 <restore_context+0x48>
40088bd8: b9400fe8     	ldr	w8, [sp, #0xc]
40088bdc: 11000508     	add	w8, w8, #0x1
40088be0: b9000fe8     	str	w8, [sp, #0xc]
40088be4: 17fffff1     	b	0x40088ba8 <restore_context+0x18>
40088be8: f9400fe8     	ldr	x8, [sp, #0x18]
40088bec: f9409108     	ldr	x8, [x8, #0x120]
40088bf0: f9400be9     	ldr	x9, [sp, #0x10]
40088bf4: f9007928     	str	x8, [x9, #0xf0]
40088bf8: f9400fe8     	ldr	x8, [sp, #0x18]
40088bfc: f9409508     	ldr	x8, [x8, #0x128]
40088c00: f9400be9     	ldr	x9, [sp, #0x10]
40088c04: f9007d28     	str	x8, [x9, #0xf8]
40088c08: f9400fe8     	ldr	x8, [sp, #0x18]
40088c0c: f9409908     	ldr	x8, [x8, #0x130]
40088c10: f9400be9     	ldr	x9, [sp, #0x10]
40088c14: f9008128     	str	x8, [x9, #0x100]
40088c18: f9400fe8     	ldr	x8, [sp, #0x18]
40088c1c: f9409d08     	ldr	x8, [x8, #0x138]
40088c20: f90003e8     	str	x8, [sp]
40088c24: f94003e8     	ldr	x8, [sp]
40088c28: d5184108     	msr	SP_EL0, x8
40088c2c: 910083ff     	add	sp, sp, #0x20
40088c30: d65f03c0     	ret
40088c34: d503201f     	nop
40088c38: d503201f     	nop
40088c3c: d503201f     	nop

0000000040088c40 <scheduler_finished>:
40088c40: d10083ff     	sub	sp, sp, #0x20
40088c44: a9017bfd     	stp	x29, x30, [sp, #0x10]
40088c48: 910043fd     	add	x29, sp, #0x10
40088c4c: 940004c9     	bl	0x40089f70 <get_cpuid>
40088c50: b81fc3a0     	stur	w0, [x29, #-0x4]
40088c54: b85fc3a8     	ldur	w8, [x29, #-0x4]
40088c58: 2a0803e9     	mov	w9, w8
40088c5c: b0000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
40088c60: 91080108     	add	x8, x8, #0x200
40088c64: 8b091d00     	add	x0, x8, x9, lsl #7
40088c68: 52800021     	mov	w1, #0x1                // =1
40088c6c: 97ffe259     	bl	0x400815d0 <longjmp>
40088c70: a9417bfd     	ldp	x29, x30, [sp, #0x10]
40088c74: 910083ff     	add	sp, sp, #0x20
40088c78: d65f03c0     	ret
40088c7c: d503201f     	nop

0000000040088c80 <kernel_thread_exit_jump>:
40088c80: d10083ff     	sub	sp, sp, #0x20
40088c84: a9017bfd     	stp	x29, x30, [sp, #0x10]
40088c88: 910043fd     	add	x29, sp, #0x10
40088c8c: 940004b9     	bl	0x40089f70 <get_cpuid>
40088c90: b81fc3a0     	stur	w0, [x29, #-0x4]
40088c94: b85fc3a8     	ldur	w8, [x29, #-0x4]
40088c98: 2a0803e9     	mov	w9, w8
40088c9c: b0000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
40088ca0: 91080108     	add	x8, x8, #0x200
40088ca4: 8b091d00     	add	x0, x8, x9, lsl #7
40088ca8: 52800041     	mov	w1, #0x2                // =2
40088cac: 97ffe249     	bl	0x400815d0 <longjmp>
40088cb0: a9417bfd     	ldp	x29, x30, [sp, #0x10]
40088cb4: 910083ff     	add	sp, sp, #0x20
40088cb8: d65f03c0     	ret
40088cbc: d503201f     	nop

0000000040088cc0 <process_exit>:
40088cc0: d103c3ff     	sub	sp, sp, #0xf0
40088cc4: a90e7bfd     	stp	x29, x30, [sp, #0xe0]
40088cc8: 910383fd     	add	x29, sp, #0xe0
40088ccc: f81f83a0     	stur	x0, [x29, #-0x8]
40088cd0: 97fffce4     	bl	0x40088060 <current_process>
40088cd4: f81f03a0     	stur	x0, [x29, #-0x10]
40088cd8: f85f03a8     	ldur	x8, [x29, #-0x10]
40088cdc: b5000068     	cbnz	x8, 0x40088ce8 <process_exit+0x28>
40088ce0: 14000001     	b	0x40088ce4 <process_exit+0x24>
40088ce4: 140000db     	b	0x40089050 <process_exit+0x390>
40088ce8: 2a1f03e8     	mov	w8, wzr
40088cec: b9004fe8     	str	w8, [sp, #0x4c]
40088cf0: b0000029     	adrp	x9, 0x4008d000 <virtio_net_send+0x90>
40088cf4: 912d9929     	add	x9, x9, #0xb66
40088cf8: f90023e9     	str	x9, [sp, #0x40]
40088cfc: b9003fe8     	str	w8, [sp, #0x3c]
40088d00: 14000001     	b	0x40088d04 <process_exit+0x44>
40088d04: f94023e8     	ldr	x8, [sp, #0x40]
40088d08: b9803fe9     	ldrsw	x9, [sp, #0x3c]
40088d0c: 38696908     	ldrb	w8, [x8, x9]
40088d10: 34000208     	cbz	w8, 0x40088d50 <process_exit+0x90>
40088d14: 14000001     	b	0x40088d18 <process_exit+0x58>
40088d18: f94023e8     	ldr	x8, [sp, #0x40]
40088d1c: b9803fe9     	ldrsw	x9, [sp, #0x3c]
40088d20: 38696908     	ldrb	w8, [x8, x9]
40088d24: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088d28: 2a0a03e9     	mov	w9, w10
40088d2c: 11000529     	add	w9, w9, #0x1
40088d30: b9004fe9     	str	w9, [sp, #0x4c]
40088d34: 910143e9     	add	x9, sp, #0x50
40088d38: 382a6928     	strb	w8, [x9, x10]
40088d3c: 14000001     	b	0x40088d40 <process_exit+0x80>
40088d40: b9403fe8     	ldr	w8, [sp, #0x3c]
40088d44: 11000508     	add	w8, w8, #0x1
40088d48: b9003fe8     	str	w8, [sp, #0x3c]
40088d4c: 17ffffee     	b	0x40088d04 <process_exit+0x44>
40088d50: f85f03a8     	ldur	x8, [x29, #-0x10]
40088d54: b9400108     	ldr	w8, [x8]
40088d58: b9003be8     	str	w8, [sp, #0x38]
40088d5c: b9403be8     	ldr	w8, [sp, #0x38]
40088d60: 35000148     	cbnz	w8, 0x40088d88 <process_exit+0xc8>
40088d64: 14000001     	b	0x40088d68 <process_exit+0xa8>
40088d68: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088d6c: 2a0a03e8     	mov	w8, w10
40088d70: 11000508     	add	w8, w8, #0x1
40088d74: b9004fe8     	str	w8, [sp, #0x4c]
40088d78: 910143e9     	add	x9, sp, #0x50
40088d7c: 52800608     	mov	w8, #0x30               // =48
40088d80: 382a6928     	strb	w8, [x9, x10]
40088d84: 14000036     	b	0x40088e5c <process_exit+0x19c>
40088d88: 2a1f03e8     	mov	w8, wzr
40088d8c: b9002be8     	str	w8, [sp, #0x28]
40088d90: 14000001     	b	0x40088d94 <process_exit+0xd4>
40088d94: b9403be8     	ldr	w8, [sp, #0x38]
40088d98: 71000508     	subs	w8, w8, #0x1
40088d9c: 540003cb     	b.lt	0x40088e14 <process_exit+0x154>
40088da0: 14000001     	b	0x40088da4 <process_exit+0xe4>
40088da4: b9803be8     	ldrsw	x8, [sp, #0x38]
40088da8: 528ccce9     	mov	w9, #0x6667             // =26215
40088dac: 72acccc9     	movk	w9, #0x6666, lsl #16
40088db0: 2a0903e0     	mov	w0, w9
40088db4: 2a0003e9     	mov	w9, w0
40088db8: 9b297d0b     	smull	x11, w8, w9
40088dbc: d362fd6a     	lsr	x10, x11, #34
40088dc0: 2a0a03e0     	mov	w0, w10
40088dc4: 2a0003ea     	mov	w10, w0
40088dc8: 8b4bfd4a     	add	x10, x10, x11, lsr #63
40088dcc: 5280014b     	mov	w11, #0xa               // =10
40088dd0: 1b0b7d4a     	mul	w10, w10, w11
40088dd4: 6b0a0108     	subs	w8, w8, w10
40088dd8: 1100c108     	add	w8, w8, #0x30
40088ddc: b9802beb     	ldrsw	x11, [sp, #0x28]
40088de0: 2a0b03ea     	mov	w10, w11
40088de4: 1100054a     	add	w10, w10, #0x1
40088de8: b9002bea     	str	w10, [sp, #0x28]
40088dec: 9100bbea     	add	x10, sp, #0x2e
40088df0: 382b6948     	strb	w8, [x10, x11]
40088df4: b9803be8     	ldrsw	x8, [sp, #0x38]
40088df8: 9b297d09     	smull	x9, w8, w9
40088dfc: 9362fd28     	asr	x8, x9, #34
40088e00: 2a0803e0     	mov	w0, w8
40088e04: 2a0003e8     	mov	w8, w0
40088e08: 8b49fd08     	add	x8, x8, x9, lsr #63
40088e0c: b9003be8     	str	w8, [sp, #0x38]
40088e10: 17ffffe1     	b	0x40088d94 <process_exit+0xd4>
40088e14: 14000001     	b	0x40088e18 <process_exit+0x158>
40088e18: b9402be8     	ldr	w8, [sp, #0x28]
40088e1c: 71000508     	subs	w8, w8, #0x1
40088e20: 540001cb     	b.lt	0x40088e58 <process_exit+0x198>
40088e24: 14000001     	b	0x40088e28 <process_exit+0x168>
40088e28: b9402be8     	ldr	w8, [sp, #0x28]
40088e2c: 71000509     	subs	w9, w8, #0x1
40088e30: b9002be9     	str	w9, [sp, #0x28]
40088e34: 9100bbe8     	add	x8, sp, #0x2e
40088e38: 3869c908     	ldrb	w8, [x8, w9, sxtw]
40088e3c: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088e40: 2a0a03e9     	mov	w9, w10
40088e44: 11000529     	add	w9, w9, #0x1
40088e48: b9004fe9     	str	w9, [sp, #0x4c]
40088e4c: 910143e9     	add	x9, sp, #0x50
40088e50: 382a6928     	strb	w8, [x9, x10]
40088e54: 17fffff1     	b	0x40088e18 <process_exit+0x158>
40088e58: 14000001     	b	0x40088e5c <process_exit+0x19c>
40088e5c: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088e60: 2a0a03e8     	mov	w8, w10
40088e64: 11000508     	add	w8, w8, #0x1
40088e68: b9004fe8     	str	w8, [sp, #0x4c]
40088e6c: 910143e9     	add	x9, sp, #0x50
40088e70: 52800748     	mov	w8, #0x3a               // =58
40088e74: 382a6928     	strb	w8, [x9, x10]
40088e78: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088e7c: 2a0a03e8     	mov	w8, w10
40088e80: 11000508     	add	w8, w8, #0x1
40088e84: b9004fe8     	str	w8, [sp, #0x4c]
40088e88: 52800408     	mov	w8, #0x20               // =32
40088e8c: 382a6928     	strb	w8, [x9, x10]
40088e90: 2a1f03e8     	mov	w8, wzr
40088e94: b90027e8     	str	w8, [sp, #0x24]
40088e98: 14000001     	b	0x40088e9c <process_exit+0x1dc>
40088e9c: f85f03a8     	ldur	x8, [x29, #-0x10]
40088ea0: b98027e9     	ldrsw	x9, [sp, #0x24]
40088ea4: 8b090108     	add	x8, x8, x9
40088ea8: 39404108     	ldrb	w8, [x8, #0x10]
40088eac: 2a1f03e9     	mov	w9, wzr
40088eb0: b90007e9     	str	w9, [sp, #0x4]
40088eb4: 340000e8     	cbz	w8, 0x40088ed0 <process_exit+0x210>
40088eb8: 14000001     	b	0x40088ebc <process_exit+0x1fc>
40088ebc: b94027e8     	ldr	w8, [sp, #0x24]
40088ec0: 71008108     	subs	w8, w8, #0x20
40088ec4: 1a9fa7e8     	cset	w8, lt
40088ec8: b90007e8     	str	w8, [sp, #0x4]
40088ecc: 14000001     	b	0x40088ed0 <process_exit+0x210>
40088ed0: b94007e8     	ldr	w8, [sp, #0x4]
40088ed4: 36000228     	tbz	w8, #0x0, 0x40088f18 <process_exit+0x258>
40088ed8: 14000001     	b	0x40088edc <process_exit+0x21c>
40088edc: f85f03a8     	ldur	x8, [x29, #-0x10]
40088ee0: b98027e9     	ldrsw	x9, [sp, #0x24]
40088ee4: 8b090108     	add	x8, x8, x9
40088ee8: 39404108     	ldrb	w8, [x8, #0x10]
40088eec: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088ef0: 2a0a03e9     	mov	w9, w10
40088ef4: 11000529     	add	w9, w9, #0x1
40088ef8: b9004fe9     	str	w9, [sp, #0x4c]
40088efc: 910143e9     	add	x9, sp, #0x50
40088f00: 382a6928     	strb	w8, [x9, x10]
40088f04: 14000001     	b	0x40088f08 <process_exit+0x248>
40088f08: b94027e8     	ldr	w8, [sp, #0x24]
40088f0c: 11000508     	add	w8, w8, #0x1
40088f10: b90027e8     	str	w8, [sp, #0x24]
40088f14: 17ffffe2     	b	0x40088e9c <process_exit+0x1dc>
40088f18: b0000028     	adrp	x8, 0x4008d000 <virtio_net_send+0x90>
40088f1c: 911fbd08     	add	x8, x8, #0x7ef
40088f20: f9000fe8     	str	x8, [sp, #0x18]
40088f24: 2a1f03e8     	mov	w8, wzr
40088f28: b90017e8     	str	w8, [sp, #0x14]
40088f2c: 14000001     	b	0x40088f30 <process_exit+0x270>
40088f30: f9400fe8     	ldr	x8, [sp, #0x18]
40088f34: b98017e9     	ldrsw	x9, [sp, #0x14]
40088f38: 38696908     	ldrb	w8, [x8, x9]
40088f3c: 34000208     	cbz	w8, 0x40088f7c <process_exit+0x2bc>
40088f40: 14000001     	b	0x40088f44 <process_exit+0x284>
40088f44: f9400fe8     	ldr	x8, [sp, #0x18]
40088f48: b98017e9     	ldrsw	x9, [sp, #0x14]
40088f4c: 38696908     	ldrb	w8, [x8, x9]
40088f50: b9804fea     	ldrsw	x10, [sp, #0x4c]
40088f54: 2a0a03e9     	mov	w9, w10
40088f58: 11000529     	add	w9, w9, #0x1
40088f5c: b9004fe9     	str	w9, [sp, #0x4c]
40088f60: 910143e9     	add	x9, sp, #0x50
40088f64: 382a6928     	strb	w8, [x9, x10]
40088f68: 14000001     	b	0x40088f6c <process_exit+0x2ac>
40088f6c: b94017e8     	ldr	w8, [sp, #0x14]
40088f70: 11000508     	add	w8, w8, #0x1
40088f74: b90017e8     	str	w8, [sp, #0x14]
40088f78: 17ffffee     	b	0x40088f30 <process_exit+0x270>
40088f7c: b9804fe9     	ldrsw	x9, [sp, #0x4c]
40088f80: 910143e0     	add	x0, sp, #0x50
40088f84: 2a1f03e8     	mov	w8, wzr
40088f88: b90003e8     	str	w8, [sp]
40088f8c: 38296808     	strb	w8, [x0, x9]
40088f90: 97ffee14     	bl	0x400847e0 <uart_puts>
40088f94: b94003e8     	ldr	w8, [sp]
40088f98: b90013e8     	str	w8, [sp, #0x10]
40088f9c: 14000001     	b	0x40088fa0 <process_exit+0x2e0>
40088fa0: b94013e8     	ldr	w8, [sp, #0x10]
40088fa4: 71007d08     	subs	w8, w8, #0x1f
40088fa8: 5400024c     	b.gt	0x40088ff0 <process_exit+0x330>
40088fac: 14000001     	b	0x40088fb0 <process_exit+0x2f0>
40088fb0: f85f03a8     	ldur	x8, [x29, #-0x10]
40088fb4: b98013e9     	ldrsw	x9, [sp, #0x10]
40088fb8: 8b090908     	add	x8, x8, x9, lsl #2
40088fbc: b9415108     	ldr	w8, [x8, #0x150]
40088fc0: 31000508     	adds	w8, w8, #0x1
40088fc4: 540000c0     	b.eq	0x40088fdc <process_exit+0x31c>
40088fc8: 14000001     	b	0x40088fcc <process_exit+0x30c>
40088fcc: f85f03a0     	ldur	x0, [x29, #-0x10]
40088fd0: b94013e1     	ldr	w1, [sp, #0x10]
40088fd4: 97ffea47     	bl	0x400838f0 <file_close>
40088fd8: 14000001     	b	0x40088fdc <process_exit+0x31c>
40088fdc: 14000001     	b	0x40088fe0 <process_exit+0x320>
40088fe0: b94013e8     	ldr	w8, [sp, #0x10]
40088fe4: 11000508     	add	w8, w8, #0x1
40088fe8: b90013e8     	str	w8, [sp, #0x10]
40088fec: 17ffffed     	b	0x40088fa0 <process_exit+0x2e0>
40088ff0: b0000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40088ff4: 912f8000     	add	x0, x0, #0xbe0
40088ff8: 97ffed82     	bl	0x40084600 <spinlock_acquire_irqsave>
40088ffc: f90007e0     	str	x0, [sp, #0x8]
40089000: f85f03a8     	ldur	x8, [x29, #-0x10]
40089004: b9400d08     	ldr	w8, [x8, #0xc]
40089008: 340000c8     	cbz	w8, 0x40089020 <process_exit+0x360>
4008900c: 14000001     	b	0x40089010 <process_exit+0x350>
40089010: f85f03a9     	ldur	x9, [x29, #-0x10]
40089014: 2a1f03e8     	mov	w8, wzr
40089018: b9000528     	str	w8, [x9, #0x4]
4008901c: 14000005     	b	0x40089030 <process_exit+0x370>
40089020: f85f03a9     	ldur	x9, [x29, #-0x10]
40089024: 52800088     	mov	w8, #0x4                // =4
40089028: b9000528     	str	w8, [x9, #0x4]
4008902c: 14000001     	b	0x40089030 <process_exit+0x370>
40089030: f94007e1     	ldr	x1, [sp, #0x8]
40089034: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089038: 912f8000     	add	x0, x0, #0xbe0
4008903c: 97ffed81     	bl	0x40084640 <spinlock_release_irqrestore>
40089040: f85f83a0     	ldur	x0, [x29, #-0x8]
40089044: 2a1f03e1     	mov	w1, wzr
40089048: 97fffd9e     	bl	0x400886c0 <schedule>
4008904c: 14000001     	b	0x40089050 <process_exit+0x390>
40089050: a94e7bfd     	ldp	x29, x30, [sp, #0xe0]
40089054: 9103c3ff     	add	sp, sp, #0xf0
40089058: d65f03c0     	ret
4008905c: d503201f     	nop

0000000040089060 <kernel_exit>:
40089060: d100c3ff     	sub	sp, sp, #0x30
40089064: a9027bfd     	stp	x29, x30, [sp, #0x20]
40089068: 910083fd     	add	x29, sp, #0x20
4008906c: 97fffbfd     	bl	0x40088060 <current_process>
40089070: f81f83a0     	stur	x0, [x29, #-0x8]
40089074: f85f83a8     	ldur	x8, [x29, #-0x8]
40089078: b4000288     	cbz	x8, 0x400890c8 <kernel_exit+0x68>
4008907c: 14000001     	b	0x40089080 <kernel_exit+0x20>
40089080: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089084: 912f8000     	add	x0, x0, #0xbe0
40089088: f90007e0     	str	x0, [sp, #0x8]
4008908c: 97ffed5d     	bl	0x40084600 <spinlock_acquire_irqsave>
40089090: f9000be0     	str	x0, [sp, #0x10]
40089094: f85f83a9     	ldur	x9, [x29, #-0x8]
40089098: 2a1f03e8     	mov	w8, wzr
4008909c: b9000528     	str	w8, [x9, #0x4]
400890a0: 940003b4     	bl	0x40089f70 <get_cpuid>
400890a4: 2a0003ea     	mov	w10, w0
400890a8: f94007e0     	ldr	x0, [sp, #0x8]
400890ac: 90000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
400890b0: 9107a129     	add	x9, x9, #0x1e8
400890b4: 12800008     	mov	w8, #-0x1               // =-1
400890b8: b82a5928     	str	w8, [x9, w10, uxtw #2]
400890bc: f9400be1     	ldr	x1, [sp, #0x10]
400890c0: 97ffed60     	bl	0x40084640 <spinlock_release_irqrestore>
400890c4: 14000001     	b	0x400890c8 <kernel_exit+0x68>
400890c8: 97fffeee     	bl	0x40088c80 <kernel_thread_exit_jump>
400890cc: a9427bfd     	ldp	x29, x30, [sp, #0x20]
400890d0: 9100c3ff     	add	sp, sp, #0x30
400890d4: d65f03c0     	ret
400890d8: d503201f     	nop
400890dc: d503201f     	nop

00000000400890e0 <process_free>:
400890e0: d100c3ff     	sub	sp, sp, #0x30
400890e4: a9027bfd     	stp	x29, x30, [sp, #0x20]
400890e8: 910083fd     	add	x29, sp, #0x20
400890ec: b81fc3a0     	stur	w0, [x29, #-0x4]
400890f0: b85fc3a8     	ldur	w8, [x29, #-0x4]
400890f4: 37f800c8     	tbnz	w8, #0x1f, 0x4008910c <process_free+0x2c>
400890f8: 14000001     	b	0x400890fc <process_free+0x1c>
400890fc: b85fc3a8     	ldur	w8, [x29, #-0x4]
40089100: 71010108     	subs	w8, w8, #0x40
40089104: 5400006b     	b.lt	0x40089110 <process_free+0x30>
40089108: 14000001     	b	0x4008910c <process_free+0x2c>
4008910c: 14000016     	b	0x40089164 <process_free+0x84>
40089110: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089114: 912f8000     	add	x0, x0, #0xbe0
40089118: f90003e0     	str	x0, [sp]
4008911c: 97ffed39     	bl	0x40084600 <spinlock_acquire_irqsave>
40089120: aa0003e8     	mov	x8, x0
40089124: f94003e0     	ldr	x0, [sp]
40089128: f9000be8     	str	x8, [sp, #0x10]
4008912c: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40089130: 52803b09     	mov	w9, #0x1d8              // =472
40089134: 2a0903e1     	mov	w1, w9
40089138: 2a0103e9     	mov	w9, w1
4008913c: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089140: 912fa14a     	add	x10, x10, #0xbe8
40089144: 9b292908     	smaddl	x8, w8, w9, x10
40089148: f90007e8     	str	x8, [sp, #0x8]
4008914c: f94007e9     	ldr	x9, [sp, #0x8]
40089150: 2a1f03e8     	mov	w8, wzr
40089154: b9000528     	str	w8, [x9, #0x4]
40089158: f9400be1     	ldr	x1, [sp, #0x10]
4008915c: 97ffed39     	bl	0x40084640 <spinlock_release_irqrestore>
40089160: 14000001     	b	0x40089164 <process_free+0x84>
40089164: a9427bfd     	ldp	x29, x30, [sp, #0x20]
40089168: 9100c3ff     	add	sp, sp, #0x30
4008916c: d65f03c0     	ret

0000000040089170 <process_kill>:
40089170: d100c3ff     	sub	sp, sp, #0x30
40089174: a9027bfd     	stp	x29, x30, [sp, #0x20]
40089178: 910083fd     	add	x29, sp, #0x20
4008917c: b81f83a0     	stur	w0, [x29, #-0x8]
40089180: b85f83a8     	ldur	w8, [x29, #-0x8]
40089184: 37f800c8     	tbnz	w8, #0x1f, 0x4008919c <process_kill+0x2c>
40089188: 14000001     	b	0x4008918c <process_kill+0x1c>
4008918c: b85f83a8     	ldur	w8, [x29, #-0x8]
40089190: 71010108     	subs	w8, w8, #0x40
40089194: 540000ab     	b.lt	0x400891a8 <process_kill+0x38>
40089198: 14000001     	b	0x4008919c <process_kill+0x2c>
4008919c: 12800008     	mov	w8, #-0x1               // =-1
400891a0: b81fc3a8     	stur	w8, [x29, #-0x4]
400891a4: 14000045     	b	0x400892b8 <process_kill+0x148>
400891a8: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400891ac: 912f8000     	add	x0, x0, #0xbe0
400891b0: 97ffed14     	bl	0x40084600 <spinlock_acquire_irqsave>
400891b4: f9000be0     	str	x0, [sp, #0x10]
400891b8: b89f83a8     	ldursw	x8, [x29, #-0x8]
400891bc: 52803b09     	mov	w9, #0x1d8              // =472
400891c0: 2a0903e0     	mov	w0, w9
400891c4: 2a0003e9     	mov	w9, w0
400891c8: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400891cc: 912fa14a     	add	x10, x10, #0xbe8
400891d0: 9b292908     	smaddl	x8, w8, w9, x10
400891d4: f90007e8     	str	x8, [sp, #0x8]
400891d8: f94007e8     	ldr	x8, [sp, #0x8]
400891dc: b9400508     	ldr	w8, [x8, #0x4]
400891e0: 340000e8     	cbz	w8, 0x400891fc <process_kill+0x8c>
400891e4: 14000001     	b	0x400891e8 <process_kill+0x78>
400891e8: f94007e8     	ldr	x8, [sp, #0x8]
400891ec: b9400508     	ldr	w8, [x8, #0x4]
400891f0: 71001108     	subs	w8, w8, #0x4
400891f4: 54000121     	b.ne	0x40089218 <process_kill+0xa8>
400891f8: 14000001     	b	0x400891fc <process_kill+0x8c>
400891fc: f9400be1     	ldr	x1, [sp, #0x10]
40089200: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089204: 912f8000     	add	x0, x0, #0xbe0
40089208: 97ffed0e     	bl	0x40084640 <spinlock_release_irqrestore>
4008920c: 12800008     	mov	w8, #-0x1               // =-1
40089210: b81fc3a8     	stur	w8, [x29, #-0x4]
40089214: 14000029     	b	0x400892b8 <process_kill+0x148>
40089218: f94007e9     	ldr	x9, [sp, #0x8]
4008921c: 52800088     	mov	w8, #0x4                // =4
40089220: b9000528     	str	w8, [x9, #0x4]
40089224: f9400be1     	ldr	x1, [sp, #0x10]
40089228: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008922c: 912f8000     	add	x0, x0, #0xbe0
40089230: 97ffed04     	bl	0x40084640 <spinlock_release_irqrestore>
40089234: 2a1f03e8     	mov	w8, wzr
40089238: b90007e8     	str	w8, [sp, #0x4]
4008923c: 14000001     	b	0x40089240 <process_kill+0xd0>
40089240: b94007e8     	ldr	w8, [sp, #0x4]
40089244: 71007d08     	subs	w8, w8, #0x1f
40089248: 5400032c     	b.gt	0x400892ac <process_kill+0x13c>
4008924c: 14000001     	b	0x40089250 <process_kill+0xe0>
40089250: f94007e8     	ldr	x8, [sp, #0x8]
40089254: b98007e9     	ldrsw	x9, [sp, #0x4]
40089258: 8b090908     	add	x8, x8, x9, lsl #2
4008925c: b9415108     	ldr	w8, [x8, #0x150]
40089260: 31000508     	adds	w8, w8, #0x1
40089264: 540001a0     	b.eq	0x40089298 <process_kill+0x128>
40089268: 14000001     	b	0x4008926c <process_kill+0xfc>
4008926c: f94007e8     	ldr	x8, [sp, #0x8]
40089270: b98007e9     	ldrsw	x9, [sp, #0x4]
40089274: 8b090908     	add	x8, x8, x9, lsl #2
40089278: b9415100     	ldr	w0, [x8, #0x150]
4008927c: 97ffea05     	bl	0x40083a90 <fs_close_global>
40089280: f94007e8     	ldr	x8, [sp, #0x8]
40089284: b98007e9     	ldrsw	x9, [sp, #0x4]
40089288: 8b090909     	add	x9, x8, x9, lsl #2
4008928c: 12800008     	mov	w8, #-0x1               // =-1
40089290: b9015128     	str	w8, [x9, #0x150]
40089294: 14000001     	b	0x40089298 <process_kill+0x128>
40089298: 14000001     	b	0x4008929c <process_kill+0x12c>
4008929c: b94007e8     	ldr	w8, [sp, #0x4]
400892a0: 11000508     	add	w8, w8, #0x1
400892a4: b90007e8     	str	w8, [sp, #0x4]
400892a8: 17ffffe6     	b	0x40089240 <process_kill+0xd0>
400892ac: 2a1f03e8     	mov	w8, wzr
400892b0: b81fc3a8     	stur	w8, [x29, #-0x4]
400892b4: 14000001     	b	0x400892b8 <process_kill+0x148>
400892b8: b85fc3a0     	ldur	w0, [x29, #-0x4]
400892bc: a9427bfd     	ldp	x29, x30, [sp, #0x20]
400892c0: 9100c3ff     	add	sp, sp, #0x30
400892c4: d65f03c0     	ret
400892c8: d503201f     	nop
400892cc: d503201f     	nop

00000000400892d0 <process_fork>:
400892d0: d10183ff     	sub	sp, sp, #0x60
400892d4: a9057bfd     	stp	x29, x30, [sp, #0x50]
400892d8: 910143fd     	add	x29, sp, #0x50
400892dc: f81f03a0     	stur	x0, [x29, #-0x10]
400892e0: 97fffb60     	bl	0x40088060 <current_process>
400892e4: f81e83a0     	stur	x0, [x29, #-0x18]
400892e8: f85e83a8     	ldur	x8, [x29, #-0x18]
400892ec: b50000a8     	cbnz	x8, 0x40089300 <process_fork+0x30>
400892f0: 14000001     	b	0x400892f4 <process_fork+0x24>
400892f4: 12800008     	mov	w8, #-0x1               // =-1
400892f8: b81fc3a8     	stur	w8, [x29, #-0x4]
400892fc: 1400006d     	b	0x400894b0 <process_fork+0x1e0>
40089300: 97fffbe0     	bl	0x40088280 <process_create>
40089304: b81e43a0     	stur	w0, [x29, #-0x1c]
40089308: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008930c: 36f800a8     	tbz	w8, #0x1f, 0x40089320 <process_fork+0x50>
40089310: 14000001     	b	0x40089314 <process_fork+0x44>
40089314: 12800008     	mov	w8, #-0x1               // =-1
40089318: b81fc3a8     	stur	w8, [x29, #-0x4]
4008931c: 14000065     	b	0x400894b0 <process_fork+0x1e0>
40089320: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089324: 912f8000     	add	x0, x0, #0xbe0
40089328: 97ffecb6     	bl	0x40084600 <spinlock_acquire_irqsave>
4008932c: f90017e0     	str	x0, [sp, #0x28]
40089330: b89e43a8     	ldursw	x8, [x29, #-0x1c]
40089334: 52803b09     	mov	w9, #0x1d8              // =472
40089338: 2a0903e0     	mov	w0, w9
4008933c: 2a0003e9     	mov	w9, w0
40089340: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089344: 912fa14a     	add	x10, x10, #0xbe8
40089348: 9b292908     	smaddl	x8, w8, w9, x10
4008934c: f90013e8     	str	x8, [sp, #0x20]
40089350: f85e83a8     	ldur	x8, [x29, #-0x18]
40089354: b9400108     	ldr	w8, [x8]
40089358: f94013e9     	ldr	x9, [sp, #0x20]
4008935c: b9000928     	str	w8, [x9, #0x8]
40089360: 2a1f03e8     	mov	w8, wzr
40089364: b9001fe8     	str	w8, [sp, #0x1c]
40089368: 14000001     	b	0x4008936c <process_fork+0x9c>
4008936c: b9401fe8     	ldr	w8, [sp, #0x1c]
40089370: 71007d08     	subs	w8, w8, #0x1f
40089374: 540001cc     	b.gt	0x400893ac <process_fork+0xdc>
40089378: 14000001     	b	0x4008937c <process_fork+0xac>
4008937c: f85e83a8     	ldur	x8, [x29, #-0x18]
40089380: b9801fea     	ldrsw	x10, [sp, #0x1c]
40089384: 8b0a0108     	add	x8, x8, x10
40089388: 39404108     	ldrb	w8, [x8, #0x10]
4008938c: f94013e9     	ldr	x9, [sp, #0x20]
40089390: 8b0a0129     	add	x9, x9, x10
40089394: 39004128     	strb	w8, [x9, #0x10]
40089398: 14000001     	b	0x4008939c <process_fork+0xcc>
4008939c: b9401fe8     	ldr	w8, [sp, #0x1c]
400893a0: 11000508     	add	w8, w8, #0x1
400893a4: b9001fe8     	str	w8, [sp, #0x1c]
400893a8: 17fffff1     	b	0x4008936c <process_fork+0x9c>
400893ac: f94013e8     	ldr	x8, [sp, #0x20]
400893b0: f940a500     	ldr	x0, [x8, #0x148]
400893b4: f85e83a8     	ldur	x8, [x29, #-0x18]
400893b8: f940a501     	ldr	x1, [x8, #0x148]
400893bc: 52a00408     	mov	w8, #0x200000           // =2097152
400893c0: 2a0803e2     	mov	w2, w8
400893c4: 9400003f     	bl	0x400894c0 <kmemcpy>
400893c8: f94013e0     	ldr	x0, [sp, #0x20]
400893cc: f85f03a1     	ldur	x1, [x29, #-0x10]
400893d0: 97fffdc4     	bl	0x40088ae0 <save_context>
400893d4: f94013e9     	ldr	x9, [sp, #0x20]
400893d8: aa1f03e8     	mov	x8, xzr
400893dc: f9001928     	str	x8, [x9, #0x30]
400893e0: d5384108     	mrs	x8, SP_EL0
400893e4: f9000be8     	str	x8, [sp, #0x10]
400893e8: f9400be8     	ldr	x8, [sp, #0x10]
400893ec: f94013e9     	ldr	x9, [sp, #0x20]
400893f0: f9009d28     	str	x8, [x9, #0x138]
400893f4: f85e83a8     	ldur	x8, [x29, #-0x18]
400893f8: b941d108     	ldr	w8, [x8, #0x1d0]
400893fc: f94013e9     	ldr	x9, [sp, #0x20]
40089400: b901d128     	str	w8, [x9, #0x1d0]
40089404: 2a1f03e8     	mov	w8, wzr
40089408: b9000fe8     	str	w8, [sp, #0xc]
4008940c: 14000001     	b	0x40089410 <process_fork+0x140>
40089410: b9400fe8     	ldr	w8, [sp, #0xc]
40089414: 71007d08     	subs	w8, w8, #0x1f
40089418: 5400038c     	b.gt	0x40089488 <process_fork+0x1b8>
4008941c: 14000001     	b	0x40089420 <process_fork+0x150>
40089420: f85e83a8     	ldur	x8, [x29, #-0x18]
40089424: b9800fe9     	ldrsw	x9, [sp, #0xc]
40089428: d37ef52a     	lsl	x10, x9, #2
4008942c: 8b0a0108     	add	x8, x8, x10
40089430: b9415108     	ldr	w8, [x8, #0x150]
40089434: f94013e9     	ldr	x9, [sp, #0x20]
40089438: 8b0a0129     	add	x9, x9, x10
4008943c: b9015128     	str	w8, [x9, #0x150]
40089440: f94013e8     	ldr	x8, [sp, #0x20]
40089444: b9800fe9     	ldrsw	x9, [sp, #0xc]
40089448: 8b090908     	add	x8, x8, x9, lsl #2
4008944c: b9415108     	ldr	w8, [x8, #0x150]
40089450: 31000508     	adds	w8, w8, #0x1
40089454: 54000100     	b.eq	0x40089474 <process_fork+0x1a4>
40089458: 14000001     	b	0x4008945c <process_fork+0x18c>
4008945c: f94013e8     	ldr	x8, [sp, #0x20]
40089460: b9800fe9     	ldrsw	x9, [sp, #0xc]
40089464: 8b090908     	add	x8, x8, x9, lsl #2
40089468: b9415100     	ldr	w0, [x8, #0x150]
4008946c: 97ffeb85     	bl	0x40084280 <fs_reopen>
40089470: 14000001     	b	0x40089474 <process_fork+0x1a4>
40089474: 14000001     	b	0x40089478 <process_fork+0x1a8>
40089478: b9400fe8     	ldr	w8, [sp, #0xc]
4008947c: 11000508     	add	w8, w8, #0x1
40089480: b9000fe8     	str	w8, [sp, #0xc]
40089484: 17ffffe3     	b	0x40089410 <process_fork+0x140>
40089488: f94013e9     	ldr	x9, [sp, #0x20]
4008948c: 52800048     	mov	w8, #0x2                // =2
40089490: b9000528     	str	w8, [x9, #0x4]
40089494: f94017e1     	ldr	x1, [sp, #0x28]
40089498: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008949c: 912f8000     	add	x0, x0, #0xbe0
400894a0: 97ffec68     	bl	0x40084640 <spinlock_release_irqrestore>
400894a4: b85e43a8     	ldur	w8, [x29, #-0x1c]
400894a8: b81fc3a8     	stur	w8, [x29, #-0x4]
400894ac: 14000001     	b	0x400894b0 <process_fork+0x1e0>
400894b0: b85fc3a0     	ldur	w0, [x29, #-0x4]
400894b4: a9457bfd     	ldp	x29, x30, [sp, #0x50]
400894b8: 910183ff     	add	sp, sp, #0x60
400894bc: d65f03c0     	ret

00000000400894c0 <kmemcpy>:
400894c0: d10143ff     	sub	sp, sp, #0x50
400894c4: f90027e0     	str	x0, [sp, #0x48]
400894c8: f90023e1     	str	x1, [sp, #0x40]
400894cc: f9001fe2     	str	x2, [sp, #0x38]
400894d0: f94027e8     	ldr	x8, [sp, #0x48]
400894d4: f9001be8     	str	x8, [sp, #0x30]
400894d8: f94023e8     	ldr	x8, [sp, #0x40]
400894dc: f90017e8     	str	x8, [sp, #0x28]
400894e0: aa1f03e8     	mov	x8, xzr
400894e4: f90013e8     	str	x8, [sp, #0x20]
400894e8: 14000001     	b	0x400894ec <kmemcpy+0x2c>
400894ec: f94013e8     	ldr	x8, [sp, #0x20]
400894f0: f9401fe9     	ldr	x9, [sp, #0x38]
400894f4: eb490d08     	subs	x8, x8, x9, lsr #3
400894f8: 54000182     	b.hs	0x40089528 <kmemcpy+0x68>
400894fc: 14000001     	b	0x40089500 <kmemcpy+0x40>
40089500: f94017e8     	ldr	x8, [sp, #0x28]
40089504: f94013ea     	ldr	x10, [sp, #0x20]
40089508: f86a7908     	ldr	x8, [x8, x10, lsl #3]
4008950c: f9401be9     	ldr	x9, [sp, #0x30]
40089510: f82a7928     	str	x8, [x9, x10, lsl #3]
40089514: 14000001     	b	0x40089518 <kmemcpy+0x58>
40089518: f94013e8     	ldr	x8, [sp, #0x20]
4008951c: 91000508     	add	x8, x8, #0x1
40089520: f90013e8     	str	x8, [sp, #0x20]
40089524: 17fffff2     	b	0x400894ec <kmemcpy+0x2c>
40089528: f94027e8     	ldr	x8, [sp, #0x48]
4008952c: f9000fe8     	str	x8, [sp, #0x18]
40089530: f94023e8     	ldr	x8, [sp, #0x40]
40089534: f9000be8     	str	x8, [sp, #0x10]
40089538: f94013e8     	ldr	x8, [sp, #0x20]
4008953c: d37df108     	lsl	x8, x8, #3
40089540: f90007e8     	str	x8, [sp, #0x8]
40089544: 14000001     	b	0x40089548 <kmemcpy+0x88>
40089548: f94007e8     	ldr	x8, [sp, #0x8]
4008954c: f9401fe9     	ldr	x9, [sp, #0x38]
40089550: eb090108     	subs	x8, x8, x9
40089554: 54000182     	b.hs	0x40089584 <kmemcpy+0xc4>
40089558: 14000001     	b	0x4008955c <kmemcpy+0x9c>
4008955c: f9400be8     	ldr	x8, [sp, #0x10]
40089560: f94007ea     	ldr	x10, [sp, #0x8]
40089564: 386a6908     	ldrb	w8, [x8, x10]
40089568: f9400fe9     	ldr	x9, [sp, #0x18]
4008956c: 382a6928     	strb	w8, [x9, x10]
40089570: 14000001     	b	0x40089574 <kmemcpy+0xb4>
40089574: f94007e8     	ldr	x8, [sp, #0x8]
40089578: 91000508     	add	x8, x8, #0x1
4008957c: f90007e8     	str	x8, [sp, #0x8]
40089580: 17fffff2     	b	0x40089548 <kmemcpy+0x88>
40089584: 910143ff     	add	sp, sp, #0x50
40089588: d65f03c0     	ret
4008958c: d503201f     	nop

0000000040089590 <process_sleep>:
40089590: d100c3ff     	sub	sp, sp, #0x30
40089594: a9027bfd     	stp	x29, x30, [sp, #0x20]
40089598: 910083fd     	add	x29, sp, #0x20
4008959c: 94000275     	bl	0x40089f70 <get_cpuid>
400895a0: b81fc3a0     	stur	w0, [x29, #-0x4]
400895a4: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400895a8: 912f8000     	add	x0, x0, #0xbe0
400895ac: 97ffec15     	bl	0x40084600 <spinlock_acquire_irqsave>
400895b0: f9000be0     	str	x0, [sp, #0x10]
400895b4: b85fc3a8     	ldur	w8, [x29, #-0x4]
400895b8: 2a0803e9     	mov	w9, w8
400895bc: 90000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
400895c0: 9107a108     	add	x8, x8, #0x1e8
400895c4: b8697908     	ldr	w8, [x8, x9, lsl #2]
400895c8: b9000fe8     	str	w8, [sp, #0xc]
400895cc: b9400fe8     	ldr	w8, [sp, #0xc]
400895d0: 37f80228     	tbnz	w8, #0x1f, 0x40089614 <process_sleep+0x84>
400895d4: 14000001     	b	0x400895d8 <process_sleep+0x48>
400895d8: b9800fe8     	ldrsw	x8, [sp, #0xc]
400895dc: 52803b09     	mov	w9, #0x1d8              // =472
400895e0: 2a0903e0     	mov	w0, w9
400895e4: 2a0003e9     	mov	w9, w0
400895e8: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400895ec: 912fa14a     	add	x10, x10, #0xbe8
400895f0: 9b292909     	smaddl	x9, w8, w9, x10
400895f4: 528000a8     	mov	w8, #0x5                // =5
400895f8: b9000528     	str	w8, [x9, #0x4]
400895fc: f9400be1     	ldr	x1, [sp, #0x10]
40089600: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089604: 912f8000     	add	x0, x0, #0xbe0
40089608: 97ffec0e     	bl	0x40084640 <spinlock_release_irqrestore>
4008960c: d4001fe1     	svc	#0xff
40089610: 14000007     	b	0x4008962c <process_sleep+0x9c>
40089614: f9400be1     	ldr	x1, [sp, #0x10]
40089618: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008961c: 912f8000     	add	x0, x0, #0xbe0
40089620: 97ffec08     	bl	0x40084640 <spinlock_release_irqrestore>
40089624: d503207f     	wfi
40089628: 14000001     	b	0x4008962c <process_sleep+0x9c>
4008962c: a9427bfd     	ldp	x29, x30, [sp, #0x20]
40089630: 9100c3ff     	add	sp, sp, #0x30
40089634: d65f03c0     	ret
40089638: d503201f     	nop
4008963c: d503201f     	nop

0000000040089640 <process_wakeup>:
40089640: d10083ff     	sub	sp, sp, #0x20
40089644: a9017bfd     	stp	x29, x30, [sp, #0x10]
40089648: 910043fd     	add	x29, sp, #0x10
4008964c: b81fc3a0     	stur	w0, [x29, #-0x4]
40089650: b85fc3a8     	ldur	w8, [x29, #-0x4]
40089654: 37f800c8     	tbnz	w8, #0x1f, 0x4008966c <process_wakeup+0x2c>
40089658: 14000001     	b	0x4008965c <process_wakeup+0x1c>
4008965c: b85fc3a8     	ldur	w8, [x29, #-0x4]
40089660: 71010108     	subs	w8, w8, #0x40
40089664: 5400006b     	b.lt	0x40089670 <process_wakeup+0x30>
40089668: 14000001     	b	0x4008966c <process_wakeup+0x2c>
4008966c: 1400001f     	b	0x400896e8 <process_wakeup+0xa8>
40089670: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089674: 912f8000     	add	x0, x0, #0xbe0
40089678: 97ffebe2     	bl	0x40084600 <spinlock_acquire_irqsave>
4008967c: f90003e0     	str	x0, [sp]
40089680: b89fc3a8     	ldursw	x8, [x29, #-0x4]
40089684: 52803b09     	mov	w9, #0x1d8              // =472
40089688: 2a0903e0     	mov	w0, w9
4008968c: 2a0003e9     	mov	w9, w0
40089690: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089694: 912fa14a     	add	x10, x10, #0xbe8
40089698: 9b292908     	smaddl	x8, w8, w9, x10
4008969c: b9400508     	ldr	w8, [x8, #0x4]
400896a0: 71001508     	subs	w8, w8, #0x5
400896a4: 54000181     	b.ne	0x400896d4 <process_wakeup+0x94>
400896a8: 14000001     	b	0x400896ac <process_wakeup+0x6c>
400896ac: b89fc3a8     	ldursw	x8, [x29, #-0x4]
400896b0: 52803b09     	mov	w9, #0x1d8              // =472
400896b4: 2a0903e0     	mov	w0, w9
400896b8: 2a0003e9     	mov	w9, w0
400896bc: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400896c0: 912fa14a     	add	x10, x10, #0xbe8
400896c4: 9b292909     	smaddl	x9, w8, w9, x10
400896c8: 52800048     	mov	w8, #0x2                // =2
400896cc: b9000528     	str	w8, [x9, #0x4]
400896d0: 14000001     	b	0x400896d4 <process_wakeup+0x94>
400896d4: f94003e1     	ldr	x1, [sp]
400896d8: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400896dc: 912f8000     	add	x0, x0, #0xbe0
400896e0: 97ffebd8     	bl	0x40084640 <spinlock_release_irqrestore>
400896e4: 14000001     	b	0x400896e8 <process_wakeup+0xa8>
400896e8: a9417bfd     	ldp	x29, x30, [sp, #0x10]
400896ec: 910083ff     	add	sp, sp, #0x20
400896f0: d65f03c0     	ret
400896f4: d503201f     	nop
400896f8: d503201f     	nop
400896fc: d503201f     	nop

0000000040089700 <process_wake_all>:
40089700: d10083ff     	sub	sp, sp, #0x20
40089704: a9017bfd     	stp	x29, x30, [sp, #0x10]
40089708: 910043fd     	add	x29, sp, #0x10
4008970c: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089710: 912f8000     	add	x0, x0, #0xbe0
40089714: 97ffebbb     	bl	0x40084600 <spinlock_acquire_irqsave>
40089718: f90007e0     	str	x0, [sp, #0x8]
4008971c: 2a1f03e8     	mov	w8, wzr
40089720: b90007e8     	str	w8, [sp, #0x4]
40089724: 14000001     	b	0x40089728 <process_wake_all+0x28>
40089728: b94007e8     	ldr	w8, [sp, #0x4]
4008972c: 7100fd08     	subs	w8, w8, #0x3f
40089730: 5400038c     	b.gt	0x400897a0 <process_wake_all+0xa0>
40089734: 14000001     	b	0x40089738 <process_wake_all+0x38>
40089738: b98007e8     	ldrsw	x8, [sp, #0x4]
4008973c: 52803b09     	mov	w9, #0x1d8              // =472
40089740: 2a0903e0     	mov	w0, w9
40089744: 2a0003e9     	mov	w9, w0
40089748: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
4008974c: 912fa14a     	add	x10, x10, #0xbe8
40089750: 9b292908     	smaddl	x8, w8, w9, x10
40089754: b9400508     	ldr	w8, [x8, #0x4]
40089758: 71001508     	subs	w8, w8, #0x5
4008975c: 54000181     	b.ne	0x4008978c <process_wake_all+0x8c>
40089760: 14000001     	b	0x40089764 <process_wake_all+0x64>
40089764: b98007e8     	ldrsw	x8, [sp, #0x4]
40089768: 52803b09     	mov	w9, #0x1d8              // =472
4008976c: 2a0903e0     	mov	w0, w9
40089770: 2a0003e9     	mov	w9, w0
40089774: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089778: 912fa14a     	add	x10, x10, #0xbe8
4008977c: 9b292909     	smaddl	x9, w8, w9, x10
40089780: 52800048     	mov	w8, #0x2                // =2
40089784: b9000528     	str	w8, [x9, #0x4]
40089788: 14000001     	b	0x4008978c <process_wake_all+0x8c>
4008978c: 14000001     	b	0x40089790 <process_wake_all+0x90>
40089790: b94007e8     	ldr	w8, [sp, #0x4]
40089794: 11000508     	add	w8, w8, #0x1
40089798: b90007e8     	str	w8, [sp, #0x4]
4008979c: 17ffffe3     	b	0x40089728 <process_wake_all+0x28>
400897a0: f94007e1     	ldr	x1, [sp, #0x8]
400897a4: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
400897a8: 912f8000     	add	x0, x0, #0xbe0
400897ac: 97ffeba5     	bl	0x40084640 <spinlock_release_irqrestore>
400897b0: a9417bfd     	ldp	x29, x30, [sp, #0x10]
400897b4: 910083ff     	add	sp, sp, #0x20
400897b8: d65f03c0     	ret
400897bc: d503201f     	nop

00000000400897c0 <process_get_pcb>:
400897c0: d10043ff     	sub	sp, sp, #0x10
400897c4: b90007e0     	str	w0, [sp, #0x4]
400897c8: b94007e8     	ldr	w8, [sp, #0x4]
400897cc: 37f800c8     	tbnz	w8, #0x1f, 0x400897e4 <process_get_pcb+0x24>
400897d0: 14000001     	b	0x400897d4 <process_get_pcb+0x14>
400897d4: b94007e8     	ldr	w8, [sp, #0x4]
400897d8: 71010108     	subs	w8, w8, #0x40
400897dc: 540000ab     	b.lt	0x400897f0 <process_get_pcb+0x30>
400897e0: 14000001     	b	0x400897e4 <process_get_pcb+0x24>
400897e4: aa1f03e8     	mov	x8, xzr
400897e8: f90007e8     	str	x8, [sp, #0x8]
400897ec: 1400000a     	b	0x40089814 <process_get_pcb+0x54>
400897f0: b98007e8     	ldrsw	x8, [sp, #0x4]
400897f4: 52803b09     	mov	w9, #0x1d8              // =472
400897f8: 2a0903e0     	mov	w0, w9
400897fc: 2a0003e9     	mov	w9, w0
40089800: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089804: 912fa14a     	add	x10, x10, #0xbe8
40089808: 9b292908     	smaddl	x8, w8, w9, x10
4008980c: f90007e8     	str	x8, [sp, #0x8]
40089810: 14000001     	b	0x40089814 <process_get_pcb+0x54>
40089814: f94007e0     	ldr	x0, [sp, #0x8]
40089818: 910043ff     	add	sp, sp, #0x10
4008981c: d65f03c0     	ret

0000000040089820 <start_scheduler>:
40089820: d105c3ff     	sub	sp, sp, #0x170
40089824: a9157bfd     	stp	x29, x30, [sp, #0x150]
40089828: f900b3fc     	str	x28, [sp, #0x160]
4008982c: 910543fd     	add	x29, sp, #0x150
40089830: d50342df     	msr	DAIFSet, #0x2
40089834: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089838: 912b9400     	add	x0, x0, #0xae5
4008983c: 97ffebe9     	bl	0x400847e0 <uart_puts>
40089840: 940001cc     	bl	0x40089f70 <get_cpuid>
40089844: 97ffec03     	bl	0x40084850 <print_int>
40089848: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008984c: 9122c000     	add	x0, x0, #0x8b0
40089850: 97ffebe4     	bl	0x400847e0 <uart_puts>
40089854: 14000001     	b	0x40089858 <start_scheduler+0x38>
40089858: 90000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
4008985c: b941f908     	ldr	w8, [x8, #0x1f8]
40089860: 35000088     	cbnz	w8, 0x40089870 <start_scheduler+0x50>
40089864: 14000001     	b	0x40089868 <start_scheduler+0x48>
40089868: d503205f     	wfe
4008986c: 17fffffb     	b	0x40089858 <start_scheduler+0x38>
40089870: 940001c0     	bl	0x40089f70 <get_cpuid>
40089874: b81fc3a0     	stur	w0, [x29, #-0x4]
40089878: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008987c: 2a0803e9     	mov	w9, w8
40089880: 90000ce8     	adrp	x8, 0x40225000 <proc_table+0x7418>
40089884: 91080108     	add	x8, x8, #0x200
40089888: 8b091d00     	add	x0, x8, x9, lsl #7
4008988c: 97ffdf47     	bl	0x400815a8 <setjmp>
40089890: b81f83a0     	stur	w0, [x29, #-0x8]
40089894: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089898: 911cec00     	add	x0, x0, #0x73b
4008989c: 97ffebd1     	bl	0x400847e0 <uart_puts>
400898a0: b85f83a0     	ldur	w0, [x29, #-0x8]
400898a4: 97ffebeb     	bl	0x40084850 <print_int>
400898a8: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
400898ac: 9122c000     	add	x0, x0, #0x8b0
400898b0: 97ffebcc     	bl	0x400847e0 <uart_puts>
400898b4: b85f83a8     	ldur	w8, [x29, #-0x8]
400898b8: 71000508     	subs	w8, w8, #0x1
400898bc: 540001a1     	b.ne	0x400898f0 <start_scheduler+0xd0>
400898c0: 14000001     	b	0x400898c4 <start_scheduler+0xa4>
400898c4: d50342ff     	msr	DAIFClr, #0x2
400898c8: b85fc3a8     	ldur	w8, [x29, #-0x4]
400898cc: 350000c8     	cbnz	w8, 0x400898e4 <start_scheduler+0xc4>
400898d0: 14000001     	b	0x400898d4 <start_scheduler+0xb4>
400898d4: f940b3fc     	ldr	x28, [sp, #0x160]
400898d8: a9557bfd     	ldp	x29, x30, [sp, #0x150]
400898dc: 9105c3ff     	add	sp, sp, #0x170
400898e0: d65f03c0     	ret
400898e4: 14000001     	b	0x400898e8 <start_scheduler+0xc8>
400898e8: d503207f     	wfi
400898ec: 17ffffff     	b	0x400898e8 <start_scheduler+0xc8>
400898f0: b85f83a8     	ldur	w8, [x29, #-0x8]
400898f4: 71000908     	subs	w8, w8, #0x2
400898f8: 54000121     	b.ne	0x4008991c <start_scheduler+0xfc>
400898fc: 14000001     	b	0x40089900 <start_scheduler+0xe0>
40089900: 910003e8     	mov	x8, sp
40089904: f81f03a8     	stur	x8, [x29, #-0x10]
40089908: d50041bf     	msr	SPSel, #0x1
4008990c: d5033fdf     	isb
40089910: f85f03a8     	ldur	x8, [x29, #-0x10]
40089914: 9100011f     	mov	sp, x8
40089918: 14000001     	b	0x4008991c <start_scheduler+0xfc>
4008991c: 14000001     	b	0x40089920 <start_scheduler+0x100>
40089920: 14000001     	b	0x40089924 <start_scheduler+0x104>
40089924: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089928: 912f8000     	add	x0, x0, #0xbe0
4008992c: 97ffeb35     	bl	0x40084600 <spinlock_acquire_irqsave>
40089930: f81e83a0     	stur	x0, [x29, #-0x18]
40089934: 2a1f03e8     	mov	w8, wzr
40089938: b81e43a8     	stur	w8, [x29, #-0x1c]
4008993c: 14000001     	b	0x40089940 <start_scheduler+0x120>
40089940: b85e43a8     	ldur	w8, [x29, #-0x1c]
40089944: 7100fd08     	subs	w8, w8, #0x3f
40089948: 540007ec     	b.gt	0x40089a44 <start_scheduler+0x224>
4008994c: 14000001     	b	0x40089950 <start_scheduler+0x130>
40089950: b89e43a8     	ldursw	x8, [x29, #-0x1c]
40089954: 52803b09     	mov	w9, #0x1d8              // =472
40089958: 2a0903e0     	mov	w0, w9
4008995c: 2a0003e9     	mov	w9, w0
40089960: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
40089964: 912fa14a     	add	x10, x10, #0xbe8
40089968: 9b292908     	smaddl	x8, w8, w9, x10
4008996c: b9400508     	ldr	w8, [x8, #0x4]
40089970: 71000908     	subs	w8, w8, #0x2
40089974: 540005e1     	b.ne	0x40089a30 <start_scheduler+0x210>
40089978: 14000001     	b	0x4008997c <start_scheduler+0x15c>
4008997c: b85e43a8     	ldur	w8, [x29, #-0x1c]
40089980: b85fc3a9     	ldur	w9, [x29, #-0x4]
40089984: 2a0903ea     	mov	w10, w9
40089988: 90000ce9     	adrp	x9, 0x40225000 <proc_table+0x7418>
4008998c: 9107a129     	add	x9, x9, #0x1e8
40089990: b82a7928     	str	w8, [x9, x10, lsl #2]
40089994: b89e43a8     	ldursw	x8, [x29, #-0x1c]
40089998: 52803b09     	mov	w9, #0x1d8              // =472
4008999c: 2a0903e0     	mov	w0, w9
400899a0: 2a0003e9     	mov	w9, w0
400899a4: b90017e9     	str	w9, [sp, #0x14]
400899a8: 90000caa     	adrp	x10, 0x4021d000 <pipes+0x7c24>
400899ac: 912fa14a     	add	x10, x10, #0xbe8
400899b0: f90007ea     	str	x10, [sp, #0x8]
400899b4: 9b29290b     	smaddl	x11, w8, w9, x10
400899b8: 52800068     	mov	w8, #0x3                // =3
400899bc: b9000568     	str	w8, [x11, #0x4]
400899c0: b89e43a8     	ldursw	x8, [x29, #-0x1c]
400899c4: 9b292908     	smaddl	x8, w8, w9, x10
400899c8: f940a500     	ldr	x0, [x8, #0x148]
400899cc: 97ffed91     	bl	0x40085010 <mmu_switch_user_mapping>
400899d0: f94007ea     	ldr	x10, [sp, #0x8]
400899d4: b94017e9     	ldr	w9, [sp, #0x14]
400899d8: b85fc3a8     	ldur	w8, [x29, #-0x4]
400899dc: 53103d08     	lsl	w8, w8, #16
400899e0: 2a0803e8     	mov	w8, w8
400899e4: 2a0803eb     	mov	w11, w8
400899e8: 900046a8     	adrp	x8, 0x4095d000 <__bss_end+0x3ffb0>
400899ec: 91014108     	add	x8, x8, #0x50
400899f0: eb0b0108     	subs	x8, x8, x11
400899f4: f81d83a8     	stur	x8, [x29, #-0x28]
400899f8: b89e43a8     	ldursw	x8, [x29, #-0x1c]
400899fc: 9b292900     	smaddl	x0, w8, w9, x10
40089a00: 910083e1     	add	x1, sp, #0x20
40089a04: f9000fe1     	str	x1, [sp, #0x18]
40089a08: 97fffc62     	bl	0x40088b90 <restore_context>
40089a0c: f85e83a1     	ldur	x1, [x29, #-0x18]
40089a10: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089a14: 912f8000     	add	x0, x0, #0xbe0
40089a18: 97ffeb0a     	bl	0x40084640 <spinlock_release_irqrestore>
40089a1c: f9400fe0     	ldr	x0, [sp, #0x18]
40089a20: f85d83a1     	ldur	x1, [x29, #-0x28]
40089a24: 97ffdeb9     	bl	0x40081508 <enter_user_space>
40089a28: 14000001     	b	0x40089a2c <start_scheduler+0x20c>
40089a2c: 14000000     	b	0x40089a2c <start_scheduler+0x20c>
40089a30: 14000001     	b	0x40089a34 <start_scheduler+0x214>
40089a34: b85e43a8     	ldur	w8, [x29, #-0x1c]
40089a38: 11000508     	add	w8, w8, #0x1
40089a3c: b81e43a8     	stur	w8, [x29, #-0x1c]
40089a40: 17ffffc0     	b	0x40089940 <start_scheduler+0x120>
40089a44: f85e83a1     	ldur	x1, [x29, #-0x18]
40089a48: 90000ca0     	adrp	x0, 0x4021d000 <pipes+0x7c24>
40089a4c: 912f8000     	add	x0, x0, #0xbe0
40089a50: 97ffeafc     	bl	0x40084640 <spinlock_release_irqrestore>
40089a54: d50342ff     	msr	DAIFClr, #0x2
40089a58: d503207f     	wfi
40089a5c: d50342df     	msr	DAIFSet, #0x2
40089a60: 17ffffb1     	b	0x40089924 <start_scheduler+0x104>
		...

0000000040089a70 <load_and_run_program>:
40089a70: d101c3ff     	sub	sp, sp, #0x70
40089a74: a9067bfd     	stp	x29, x30, [sp, #0x60]
40089a78: 910183fd     	add	x29, sp, #0x60
40089a7c: f81f03a0     	stur	x0, [x29, #-0x10]
40089a80: d503201f     	nop
40089a84: 100207a0     	adr	x0, 0x4008db78 <UART0_FR+0x838>
40089a88: 97ffeb56     	bl	0x400847e0 <uart_puts>
40089a8c: f85f03a0     	ldur	x0, [x29, #-0x10]
40089a90: 97ffeb54     	bl	0x400847e0 <uart_puts>
40089a94: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089a98: 9122c000     	add	x0, x0, #0x8b0
40089a9c: 97ffeb51     	bl	0x400847e0 <uart_puts>
40089aa0: f85f03a0     	ldur	x0, [x29, #-0x10]
40089aa4: 910043e1     	add	x1, sp, #0x10
40089aa8: 97ffe17e     	bl	0x400820a0 <fat16_open>
40089aac: 340001a0     	cbz	w0, 0x40089ae0 <load_and_run_program+0x70>
40089ab0: 14000001     	b	0x40089ab4 <load_and_run_program+0x44>
40089ab4: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089ab8: 91151400     	add	x0, x0, #0x545
40089abc: 97ffeb49     	bl	0x400847e0 <uart_puts>
40089ac0: f85f03a0     	ldur	x0, [x29, #-0x10]
40089ac4: 97ffeb47     	bl	0x400847e0 <uart_puts>
40089ac8: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089acc: 91197400     	add	x0, x0, #0x65d
40089ad0: 97ffeb44     	bl	0x400847e0 <uart_puts>
40089ad4: 12800008     	mov	w8, #-0x1               // =-1
40089ad8: b81fc3a8     	stur	w8, [x29, #-0x4]
40089adc: 14000035     	b	0x40089bb0 <load_and_run_program+0x140>
40089ae0: 52a88008     	mov	w8, #0x44000000         // =1140850688
40089ae4: 2a0803e1     	mov	w1, w8
40089ae8: 910043e0     	add	x0, sp, #0x10
40089aec: 52a00022     	mov	w2, #0x10000            // =65536
40089af0: 97ffe3d4     	bl	0x40082a40 <fat16_read>
40089af4: b9000fe0     	str	w0, [sp, #0xc]
40089af8: b9400fe8     	ldr	w8, [sp, #0xc]
40089afc: 71000108     	subs	w8, w8, #0x0
40089b00: 540001ec     	b.gt	0x40089b3c <load_and_run_program+0xcc>
40089b04: 14000001     	b	0x40089b08 <load_and_run_program+0x98>
40089b08: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089b0c: 912c1000     	add	x0, x0, #0xb04
40089b10: 97ffeb34     	bl	0x400847e0 <uart_puts>
40089b14: f85f03a0     	ldur	x0, [x29, #-0x10]
40089b18: 97ffeb32     	bl	0x400847e0 <uart_puts>
40089b1c: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089b20: 9116d400     	add	x0, x0, #0x5b5
40089b24: 97ffeb2f     	bl	0x400847e0 <uart_puts>
40089b28: 910043e0     	add	x0, sp, #0x10
40089b2c: 97ffe371     	bl	0x400828f0 <fat16_close>
40089b30: 12800008     	mov	w8, #-0x1               // =-1
40089b34: b81fc3a8     	stur	w8, [x29, #-0x4]
40089b38: 1400001e     	b	0x40089bb0 <load_and_run_program+0x140>
40089b3c: b9800fe8     	ldrsw	x8, [sp, #0xc]
40089b40: 52a88009     	mov	w9, #0x44000000         // =1140850688
40089b44: 2a0903e0     	mov	w0, w9
40089b48: 8b000101     	add	x1, x8, x0
40089b4c: 97ffed91     	bl	0x40085190 <__clear_cache>
40089b50: 910043e0     	add	x0, sp, #0x10
40089b54: 97ffe367     	bl	0x400828f0 <fat16_close>
40089b58: 90000ce0     	adrp	x0, 0x40225000 <proc_table+0x7418>
40089b5c: 91100000     	add	x0, x0, #0x400
40089b60: 97ffde92     	bl	0x400815a8 <setjmp>
40089b64: 340000c0     	cbz	w0, 0x40089b7c <load_and_run_program+0x10c>
40089b68: 14000001     	b	0x40089b6c <load_and_run_program+0xfc>
40089b6c: d50342ff     	msr	DAIFClr, #0x2
40089b70: 2a1f03e8     	mov	w8, wzr
40089b74: b81fc3a8     	stur	w8, [x29, #-0x4]
40089b78: 1400000e     	b	0x40089bb0 <load_and_run_program+0x140>
40089b7c: 52a88008     	mov	w8, #0x44000000         // =1140850688
40089b80: 52a88409     	mov	w9, #0x44200000         // =1142947840
40089b84: d50342df     	msr	DAIFSet, #0x2
40089b88: d5184028     	msr	ELR_EL1, x8
40089b8c: d2800002     	mov	x2, #0x0                // =0
40089b90: d5184002     	msr	SPSR_EL1, x2
40089b94: d5184109     	msr	SP_EL0, x9
40089b98: d2800000     	mov	x0, #0x0                // =0
40089b9c: d2800001     	mov	x1, #0x0                // =0
40089ba0: d69f03e0     	eret
40089ba4: 12800008     	mov	w8, #-0x1               // =-1
40089ba8: b81fc3a8     	stur	w8, [x29, #-0x4]
40089bac: 14000001     	b	0x40089bb0 <load_and_run_program+0x140>
40089bb0: b85fc3a0     	ldur	w0, [x29, #-0x4]
40089bb4: a9467bfd     	ldp	x29, x30, [sp, #0x60]
40089bb8: 9101c3ff     	add	sp, sp, #0x70
40089bbc: d65f03c0     	ret

0000000040089bc0 <load_and_run_program_in_scheduler>:
40089bc0: d10283ff     	sub	sp, sp, #0xa0
40089bc4: a9097bfd     	stp	x29, x30, [sp, #0x90]
40089bc8: 910243fd     	add	x29, sp, #0x90
40089bcc: f81f03a0     	stur	x0, [x29, #-0x10]
40089bd0: b81ec3a1     	stur	w1, [x29, #-0x14]
40089bd4: b81e83a2     	stur	w2, [x29, #-0x18]
40089bd8: f85f03a8     	ldur	x8, [x29, #-0x10]
40089bdc: b50000a8     	cbnz	x8, 0x40089bf0 <load_and_run_program_in_scheduler+0x30>
40089be0: 14000001     	b	0x40089be4 <load_and_run_program_in_scheduler+0x24>
40089be4: 12800008     	mov	w8, #-0x1               // =-1
40089be8: b81fc3a8     	stur	w8, [x29, #-0x4]
40089bec: 140000dd     	b	0x40089f60 <load_and_run_program_in_scheduler+0x3a0>
40089bf0: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089bf4: 91220000     	add	x0, x0, #0x880
40089bf8: 97ffeafa     	bl	0x400847e0 <uart_puts>
40089bfc: f85f03a0     	ldur	x0, [x29, #-0x10]
40089c00: 97ffeaf8     	bl	0x400847e0 <uart_puts>
40089c04: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089c08: 9122c000     	add	x0, x0, #0x8b0
40089c0c: 97ffeaf5     	bl	0x400847e0 <uart_puts>
40089c10: 97fff99c     	bl	0x40088280 <process_create>
40089c14: b81e43a0     	stur	w0, [x29, #-0x1c]
40089c18: b85e43a8     	ldur	w8, [x29, #-0x1c]
40089c1c: 36f801a8     	tbz	w8, #0x1f, 0x40089c50 <load_and_run_program_in_scheduler+0x90>
40089c20: 14000001     	b	0x40089c24 <load_and_run_program_in_scheduler+0x64>
40089c24: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089c28: 91128000     	add	x0, x0, #0x4a0
40089c2c: 97ffeaed     	bl	0x400847e0 <uart_puts>
40089c30: f85f03a0     	ldur	x0, [x29, #-0x10]
40089c34: 97ffeaeb     	bl	0x400847e0 <uart_puts>
40089c38: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089c3c: 910d2000     	add	x0, x0, #0x348
40089c40: 97ffeae8     	bl	0x400847e0 <uart_puts>
40089c44: 12800008     	mov	w8, #-0x1               // =-1
40089c48: b81fc3a8     	stur	w8, [x29, #-0x4]
40089c4c: 140000c5     	b	0x40089f60 <load_and_run_program_in_scheduler+0x3a0>
40089c50: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089c54: 97fffedb     	bl	0x400897c0 <process_get_pcb>
40089c58: f81d83a0     	stur	x0, [x29, #-0x28]
40089c5c: f85d83a8     	ldur	x8, [x29, #-0x28]
40089c60: b40004e8     	cbz	x8, 0x40089cfc <load_and_run_program_in_scheduler+0x13c>
40089c64: 14000001     	b	0x40089c68 <load_and_run_program_in_scheduler+0xa8>
40089c68: 2a1f03e8     	mov	w8, wzr
40089c6c: b81d43a8     	stur	w8, [x29, #-0x2c]
40089c70: 14000001     	b	0x40089c74 <load_and_run_program_in_scheduler+0xb4>
40089c74: b85d43a9     	ldur	w9, [x29, #-0x2c]
40089c78: 2a1f03e8     	mov	w8, wzr
40089c7c: 71007929     	subs	w9, w9, #0x1e
40089c80: b90007e8     	str	w8, [sp, #0x4]
40089c84: 5400012c     	b.gt	0x40089ca8 <load_and_run_program_in_scheduler+0xe8>
40089c88: 14000001     	b	0x40089c8c <load_and_run_program_in_scheduler+0xcc>
40089c8c: f85f03a8     	ldur	x8, [x29, #-0x10]
40089c90: b89d43a9     	ldursw	x9, [x29, #-0x2c]
40089c94: 38696908     	ldrb	w8, [x8, x9]
40089c98: 71000108     	subs	w8, w8, #0x0
40089c9c: 1a9f07e8     	cset	w8, ne
40089ca0: b90007e8     	str	w8, [sp, #0x4]
40089ca4: 14000001     	b	0x40089ca8 <load_and_run_program_in_scheduler+0xe8>
40089ca8: b94007e8     	ldr	w8, [sp, #0x4]
40089cac: 36000268     	tbz	w8, #0x0, 0x40089cf8 <load_and_run_program_in_scheduler+0x138>
40089cb0: 14000001     	b	0x40089cb4 <load_and_run_program_in_scheduler+0xf4>
40089cb4: f85f03a8     	ldur	x8, [x29, #-0x10]
40089cb8: b89d43aa     	ldursw	x10, [x29, #-0x2c]
40089cbc: 386a6908     	ldrb	w8, [x8, x10]
40089cc0: f85d83a9     	ldur	x9, [x29, #-0x28]
40089cc4: 8b0a0129     	add	x9, x9, x10
40089cc8: 39004128     	strb	w8, [x9, #0x10]
40089ccc: f85d83a8     	ldur	x8, [x29, #-0x28]
40089cd0: b85d43a9     	ldur	w9, [x29, #-0x2c]
40089cd4: 11000529     	add	w9, w9, #0x1
40089cd8: 8b29c109     	add	x9, x8, w9, sxtw
40089cdc: 2a1f03e8     	mov	w8, wzr
40089ce0: 39004128     	strb	w8, [x9, #0x10]
40089ce4: 14000001     	b	0x40089ce8 <load_and_run_program_in_scheduler+0x128>
40089ce8: b85d43a8     	ldur	w8, [x29, #-0x2c]
40089cec: 11000508     	add	w8, w8, #0x1
40089cf0: b81d43a8     	stur	w8, [x29, #-0x2c]
40089cf4: 17ffffe0     	b	0x40089c74 <load_and_run_program_in_scheduler+0xb4>
40089cf8: 14000001     	b	0x40089cfc <load_and_run_program_in_scheduler+0x13c>
40089cfc: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d00: 9124cc00     	add	x0, x0, #0x933
40089d04: 97ffeab7     	bl	0x400847e0 <uart_puts>
40089d08: f85f03a0     	ldur	x0, [x29, #-0x10]
40089d0c: 910083e1     	add	x1, sp, #0x20
40089d10: 97ffe0e4     	bl	0x400820a0 <fat16_open>
40089d14: 340001e0     	cbz	w0, 0x40089d50 <load_and_run_program_in_scheduler+0x190>
40089d18: 14000001     	b	0x40089d1c <load_and_run_program_in_scheduler+0x15c>
40089d1c: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d20: 91170800     	add	x0, x0, #0x5c2
40089d24: 97ffeaaf     	bl	0x400847e0 <uart_puts>
40089d28: f85f03a0     	ldur	x0, [x29, #-0x10]
40089d2c: 97ffeaad     	bl	0x400847e0 <uart_puts>
40089d30: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d34: 9122c000     	add	x0, x0, #0x8b0
40089d38: 97ffeaaa     	bl	0x400847e0 <uart_puts>
40089d3c: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089d40: 97fffce8     	bl	0x400890e0 <process_free>
40089d44: 12800008     	mov	w8, #-0x1               // =-1
40089d48: b81fc3a8     	stur	w8, [x29, #-0x4]
40089d4c: 14000085     	b	0x40089f60 <load_and_run_program_in_scheduler+0x3a0>
40089d50: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089d54: 97fff8ef     	bl	0x40088110 <process_get_phys_base>
40089d58: f9000fe0     	str	x0, [sp, #0x18]
40089d5c: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d60: 912e2800     	add	x0, x0, #0xb8a
40089d64: 97ffea9f     	bl	0x400847e0 <uart_puts>
40089d68: f9400fe1     	ldr	x1, [sp, #0x18]
40089d6c: 910083e0     	add	x0, sp, #0x20
40089d70: 52a00022     	mov	w2, #0x10000            // =65536
40089d74: 97ffe333     	bl	0x40082a40 <fat16_read>
40089d78: b90017e0     	str	w0, [sp, #0x14]
40089d7c: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d80: 9127b000     	add	x0, x0, #0x9ec
40089d84: 97ffea97     	bl	0x400847e0 <uart_puts>
40089d88: b94017e8     	ldr	w8, [sp, #0x14]
40089d8c: 71000108     	subs	w8, w8, #0x0
40089d90: 5400022c     	b.gt	0x40089dd4 <load_and_run_program_in_scheduler+0x214>
40089d94: 14000001     	b	0x40089d98 <load_and_run_program_in_scheduler+0x1d8>
40089d98: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089d9c: 912c1000     	add	x0, x0, #0xb04
40089da0: 97ffea90     	bl	0x400847e0 <uart_puts>
40089da4: f85f03a0     	ldur	x0, [x29, #-0x10]
40089da8: 97ffea8e     	bl	0x400847e0 <uart_puts>
40089dac: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089db0: 9116d400     	add	x0, x0, #0x5b5
40089db4: 97ffea8b     	bl	0x400847e0 <uart_puts>
40089db8: 910083e0     	add	x0, sp, #0x20
40089dbc: 97ffe2cd     	bl	0x400828f0 <fat16_close>
40089dc0: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089dc4: 97fffcc7     	bl	0x400890e0 <process_free>
40089dc8: 12800008     	mov	w8, #-0x1               // =-1
40089dcc: b81fc3a8     	stur	w8, [x29, #-0x4]
40089dd0: 14000064     	b	0x40089f60 <load_and_run_program_in_scheduler+0x3a0>
40089dd4: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089dd8: 911e5400     	add	x0, x0, #0x795
40089ddc: 97ffea81     	bl	0x400847e0 <uart_puts>
40089de0: b94017e0     	ldr	w0, [sp, #0x14]
40089de4: 97ffea9b     	bl	0x40084850 <print_int>
40089de8: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089dec: 91228000     	add	x0, x0, #0x8a0
40089df0: 97ffea7c     	bl	0x400847e0 <uart_puts>
40089df4: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089df8: 97ffea96     	bl	0x40084850 <print_int>
40089dfc: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089e00: 9122c000     	add	x0, x0, #0x8b0
40089e04: 97ffea77     	bl	0x400847e0 <uart_puts>
40089e08: f9400fe0     	ldr	x0, [sp, #0x18]
40089e0c: b98017e8     	ldrsw	x8, [sp, #0x14]
40089e10: 8b080001     	add	x1, x0, x8
40089e14: 97ffecdf     	bl	0x40085190 <__clear_cache>
40089e18: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089e1c: 912c5000     	add	x0, x0, #0xb14
40089e20: 97ffea70     	bl	0x400847e0 <uart_puts>
40089e24: 910083e0     	add	x0, sp, #0x20
40089e28: 97ffe2b2     	bl	0x400828f0 <fat16_close>
40089e2c: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089e30: 910f0c00     	add	x0, x0, #0x3c3
40089e34: 97ffea6b     	bl	0x400847e0 <uart_puts>
40089e38: 97fff88a     	bl	0x40088060 <current_process>
40089e3c: f90007e0     	str	x0, [sp, #0x8]
40089e40: f94007e8     	ldr	x8, [sp, #0x8]
40089e44: b40007c8     	cbz	x8, 0x40089f3c <load_and_run_program_in_scheduler+0x37c>
40089e48: 14000001     	b	0x40089e4c <load_and_run_program_in_scheduler+0x28c>
40089e4c: f85d83a8     	ldur	x8, [x29, #-0x28]
40089e50: b4000768     	cbz	x8, 0x40089f3c <load_and_run_program_in_scheduler+0x37c>
40089e54: 14000001     	b	0x40089e58 <load_and_run_program_in_scheduler+0x298>
40089e58: b85ec3a8     	ldur	w8, [x29, #-0x14]
40089e5c: 37f80368     	tbnz	w8, #0x1f, 0x40089ec8 <load_and_run_program_in_scheduler+0x308>
40089e60: 14000001     	b	0x40089e64 <load_and_run_program_in_scheduler+0x2a4>
40089e64: b85ec3a8     	ldur	w8, [x29, #-0x14]
40089e68: 71007d08     	subs	w8, w8, #0x1f
40089e6c: 540002ec     	b.gt	0x40089ec8 <load_and_run_program_in_scheduler+0x308>
40089e70: 14000001     	b	0x40089e74 <load_and_run_program_in_scheduler+0x2b4>
40089e74: f94007e8     	ldr	x8, [sp, #0x8]
40089e78: b89ec3a9     	ldursw	x9, [x29, #-0x14]
40089e7c: 8b090908     	add	x8, x8, x9, lsl #2
40089e80: b9415108     	ldr	w8, [x8, #0x150]
40089e84: 31000508     	adds	w8, w8, #0x1
40089e88: 54000200     	b.eq	0x40089ec8 <load_and_run_program_in_scheduler+0x308>
40089e8c: 14000001     	b	0x40089e90 <load_and_run_program_in_scheduler+0x2d0>
40089e90: f94007e8     	ldr	x8, [sp, #0x8]
40089e94: b89ec3a9     	ldursw	x9, [x29, #-0x14]
40089e98: 8b090908     	add	x8, x8, x9, lsl #2
40089e9c: b9415108     	ldr	w8, [x8, #0x150]
40089ea0: f85d83a9     	ldur	x9, [x29, #-0x28]
40089ea4: b9015128     	str	w8, [x9, #0x150]
40089ea8: f85d83a8     	ldur	x8, [x29, #-0x28]
40089eac: b9415100     	ldr	w0, [x8, #0x150]
40089eb0: 97ffe8f4     	bl	0x40084280 <fs_reopen>
40089eb4: f85d83a9     	ldur	x9, [x29, #-0x28]
40089eb8: b941d128     	ldr	w8, [x9, #0x1d0]
40089ebc: 11000508     	add	w8, w8, #0x1
40089ec0: b901d128     	str	w8, [x9, #0x1d0]
40089ec4: 14000001     	b	0x40089ec8 <load_and_run_program_in_scheduler+0x308>
40089ec8: b85e83a8     	ldur	w8, [x29, #-0x18]
40089ecc: 37f80368     	tbnz	w8, #0x1f, 0x40089f38 <load_and_run_program_in_scheduler+0x378>
40089ed0: 14000001     	b	0x40089ed4 <load_and_run_program_in_scheduler+0x314>
40089ed4: b85e83a8     	ldur	w8, [x29, #-0x18]
40089ed8: 71007d08     	subs	w8, w8, #0x1f
40089edc: 540002ec     	b.gt	0x40089f38 <load_and_run_program_in_scheduler+0x378>
40089ee0: 14000001     	b	0x40089ee4 <load_and_run_program_in_scheduler+0x324>
40089ee4: f94007e8     	ldr	x8, [sp, #0x8]
40089ee8: b89e83a9     	ldursw	x9, [x29, #-0x18]
40089eec: 8b090908     	add	x8, x8, x9, lsl #2
40089ef0: b9415108     	ldr	w8, [x8, #0x150]
40089ef4: 31000508     	adds	w8, w8, #0x1
40089ef8: 54000200     	b.eq	0x40089f38 <load_and_run_program_in_scheduler+0x378>
40089efc: 14000001     	b	0x40089f00 <load_and_run_program_in_scheduler+0x340>
40089f00: f94007e8     	ldr	x8, [sp, #0x8]
40089f04: b89e83a9     	ldursw	x9, [x29, #-0x18]
40089f08: 8b090908     	add	x8, x8, x9, lsl #2
40089f0c: b9415108     	ldr	w8, [x8, #0x150]
40089f10: f85d83a9     	ldur	x9, [x29, #-0x28]
40089f14: b9015528     	str	w8, [x9, #0x154]
40089f18: f85d83a8     	ldur	x8, [x29, #-0x28]
40089f1c: b9415500     	ldr	w0, [x8, #0x154]
40089f20: 97ffe8d8     	bl	0x40084280 <fs_reopen>
40089f24: f85d83a9     	ldur	x9, [x29, #-0x28]
40089f28: b941d128     	ldr	w8, [x9, #0x1d0]
40089f2c: 11000508     	add	w8, w8, #0x1
40089f30: b901d128     	str	w8, [x9, #0x1d0]
40089f34: 14000001     	b	0x40089f38 <load_and_run_program_in_scheduler+0x378>
40089f38: 14000001     	b	0x40089f3c <load_and_run_program_in_scheduler+0x37c>
40089f3c: b85e43a0     	ldur	w0, [x29, #-0x1c]
40089f40: 52a88008     	mov	w8, #0x44000000         // =1140850688
40089f44: 2a0803e1     	mov	w1, w8
40089f48: 52a88408     	mov	w8, #0x44200000         // =1142947840
40089f4c: 2a0803e2     	mov	w2, w8
40089f50: 97fff898     	bl	0x400881b0 <process_set_entry>
40089f54: b85e43a8     	ldur	w8, [x29, #-0x1c]
40089f58: b81fc3a8     	stur	w8, [x29, #-0x4]
40089f5c: 14000001     	b	0x40089f60 <load_and_run_program_in_scheduler+0x3a0>
40089f60: b85fc3a0     	ldur	w0, [x29, #-0x4]
40089f64: a9497bfd     	ldp	x29, x30, [sp, #0x90]
40089f68: 910283ff     	add	sp, sp, #0xa0
40089f6c: d65f03c0     	ret

0000000040089f70 <get_cpuid>:
40089f70: d10043ff     	sub	sp, sp, #0x10
40089f74: d53800a8     	mrs	x8, MPIDR_EL1
40089f78: f90007e8     	str	x8, [sp, #0x8]
40089f7c: 394023e0     	ldrb	w0, [sp, #0x8]
40089f80: 910043ff     	add	sp, sp, #0x10
40089f84: d65f03c0     	ret
40089f88: d503201f     	nop
40089f8c: d503201f     	nop

0000000040089f90 <smp_init>:
40089f90: d10083ff     	sub	sp, sp, #0x20
40089f94: a9017bfd     	stp	x29, x30, [sp, #0x10]
40089f98: 910043fd     	add	x29, sp, #0x10
40089f9c: d503201f     	nop
40089fa0: 1001c3a0     	adr	x0, 0x4008d814 <UART0_FR+0x4d4>
40089fa4: 97ffea0f     	bl	0x400847e0 <uart_puts>
40089fa8: 52800028     	mov	w8, #0x1                // =1
40089fac: b81fc3a8     	stur	w8, [x29, #-0x4]
40089fb0: 14000001     	b	0x40089fb4 <smp_init+0x24>
40089fb4: b85fc3a8     	ldur	w8, [x29, #-0x4]
40089fb8: 71000d08     	subs	w8, w8, #0x3
40089fbc: 540004ec     	b.gt	0x4008a058 <smp_init+0xc8>
40089fc0: 14000001     	b	0x40089fc4 <smp_init+0x34>
40089fc4: b89fc3a0     	ldursw	x0, [x29, #-0x4]
40089fc8: d503201f     	nop
40089fcc: 10fb05a1     	adr	x1, 0x40080080 <secondary_entry>
40089fd0: aa1f03e2     	mov	x2, xzr
40089fd4: 94000027     	bl	0x4008a070 <psci_cpu_on>
40089fd8: f90003e0     	str	x0, [sp]
40089fdc: f94003e8     	ldr	x8, [sp]
40089fe0: b5000168     	cbnz	x8, 0x4008a00c <smp_init+0x7c>
40089fe4: 14000001     	b	0x40089fe8 <smp_init+0x58>
40089fe8: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
40089fec: 911e6c00     	add	x0, x0, #0x79b
40089ff0: 97ffe9fc     	bl	0x400847e0 <uart_puts>
40089ff4: b85fc3a0     	ldur	w0, [x29, #-0x4]
40089ff8: 97ffea16     	bl	0x40084850 <print_int>
40089ffc: 90000020     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a000: 912a6400     	add	x0, x0, #0xa99
4008a004: 97ffe9f7     	bl	0x400847e0 <uart_puts>
4008a008: 1400000f     	b	0x4008a044 <smp_init+0xb4>
4008a00c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a010: 910f6800     	add	x0, x0, #0x3da
4008a014: 97ffe9f3     	bl	0x400847e0 <uart_puts>
4008a018: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008a01c: 97ffea0d     	bl	0x40084850 <print_int>
4008a020: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a024: 910ff000     	add	x0, x0, #0x3fc
4008a028: 97ffe9ee     	bl	0x400847e0 <uart_puts>
4008a02c: b94003e0     	ldr	w0, [sp]
4008a030: 97ffea08     	bl	0x40084850 <print_int>
4008a034: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a038: 9112f800     	add	x0, x0, #0x4be
4008a03c: 97ffe9e9     	bl	0x400847e0 <uart_puts>
4008a040: 14000001     	b	0x4008a044 <smp_init+0xb4>
4008a044: 14000001     	b	0x4008a048 <smp_init+0xb8>
4008a048: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008a04c: 11000508     	add	w8, w8, #0x1
4008a050: b81fc3a8     	stur	w8, [x29, #-0x4]
4008a054: 17ffffd8     	b	0x40089fb4 <smp_init+0x24>
4008a058: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008a05c: 910083ff     	add	sp, sp, #0x20
4008a060: d65f03c0     	ret
4008a064: d503201f     	nop
4008a068: d503201f     	nop
4008a06c: d503201f     	nop

000000004008a070 <psci_cpu_on>:
4008a070: d10103ff     	sub	sp, sp, #0x40
4008a074: f9001fe0     	str	x0, [sp, #0x38]
4008a078: f9001be1     	str	x1, [sp, #0x30]
4008a07c: f90017e2     	str	x2, [sp, #0x28]
4008a080: 52800068     	mov	w8, #0x3                // =3
4008a084: 72b88008     	movk	w8, #0xc400, lsl #16
4008a088: f90013e8     	str	x8, [sp, #0x20]
4008a08c: f9401fe8     	ldr	x8, [sp, #0x38]
4008a090: f9000fe8     	str	x8, [sp, #0x18]
4008a094: f9401be8     	ldr	x8, [sp, #0x30]
4008a098: f9000be8     	str	x8, [sp, #0x10]
4008a09c: f94017e8     	ldr	x8, [sp, #0x28]
4008a0a0: f90007e8     	str	x8, [sp, #0x8]
4008a0a4: f94013e0     	ldr	x0, [sp, #0x20]
4008a0a8: f9400fe1     	ldr	x1, [sp, #0x18]
4008a0ac: f9400be2     	ldr	x2, [sp, #0x10]
4008a0b0: f94007e3     	ldr	x3, [sp, #0x8]
4008a0b4: d4000002     	hvc	#0
4008a0b8: f90013e0     	str	x0, [sp, #0x20]
4008a0bc: f94013e0     	ldr	x0, [sp, #0x20]
4008a0c0: 910103ff     	add	sp, sp, #0x40
4008a0c4: d65f03c0     	ret
		...

000000004008a0d0 <test_fw_cfg>:
4008a0d0: d100c3ff     	sub	sp, sp, #0x30
4008a0d4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008a0d8: 910083fd     	add	x29, sp, #0x20
4008a0dc: 52800108     	mov	w8, #0x8                // =8
4008a0e0: 72a12048     	movk	w8, #0x902, lsl #16
4008a0e4: f81f83a8     	stur	x8, [x29, #-0x8]
4008a0e8: 52a12048     	mov	w8, #0x9020000          // =151126016
4008a0ec: f9000be8     	str	x8, [sp, #0x10]
4008a0f0: f85f83a9     	ldur	x9, [x29, #-0x8]
4008a0f4: 2a1f03e8     	mov	w8, wzr
4008a0f8: 79000128     	strh	w8, [x9]
4008a0fc: 390033e8     	strb	w8, [sp, #0xc]
4008a100: b9000be8     	str	w8, [sp, #0x8]
4008a104: f9400be8     	ldr	x8, [sp, #0x10]
4008a108: 39400108     	ldrb	w8, [x8]
4008a10c: 390023e8     	strb	w8, [sp, #0x8]
4008a110: f9400be8     	ldr	x8, [sp, #0x10]
4008a114: 39400108     	ldrb	w8, [x8]
4008a118: 390027e8     	strb	w8, [sp, #0x9]
4008a11c: f9400be8     	ldr	x8, [sp, #0x10]
4008a120: 39400108     	ldrb	w8, [x8]
4008a124: 39002be8     	strb	w8, [sp, #0xa]
4008a128: f9400be8     	ldr	x8, [sp, #0x10]
4008a12c: 39400108     	ldrb	w8, [x8]
4008a130: 39002fe8     	strb	w8, [sp, #0xb]
4008a134: d503201f     	nop
4008a138: 7001cba0     	adr	x0, 0x4008daaf <UART0_FR+0x76f>
4008a13c: 97ffe9a9     	bl	0x400847e0 <uart_puts>
4008a140: 910023e0     	add	x0, sp, #0x8
4008a144: 97ffe9a7     	bl	0x400847e0 <uart_puts>
4008a148: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a14c: 9122c000     	add	x0, x0, #0x8b0
4008a150: 97ffe9a4     	bl	0x400847e0 <uart_puts>
4008a154: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008a158: 9100c3ff     	add	sp, sp, #0x30
4008a15c: d65f03c0     	ret

000000004008a160 <timer_init>:
4008a160: d10043ff     	sub	sp, sp, #0x10
4008a164: d53be008     	mrs	x8, CNTFRQ_EL0
4008a168: f90007e8     	str	x8, [sp, #0x8]
4008a16c: f94007e8     	ldr	x8, [sp, #0x8]
4008a170: d342fd08     	lsr	x8, x8, #2
4008a174: d29eb869     	mov	x9, #0xf5c3             // =62915
4008a178: f2ab8509     	movk	x9, #0x5c28, lsl #16
4008a17c: f2d851e9     	movk	x9, #0xc28f, lsl #32
4008a180: f2e51ea9     	movk	x9, #0x28f5, lsl #48
4008a184: 9bc97d08     	umulh	x8, x8, x9
4008a188: d342fd09     	lsr	x9, x8, #2
4008a18c: f0000cc8     	adrp	x8, 0x40225000 <proc_table+0x7418>
4008a190: f9024109     	str	x9, [x8, #0x480]
4008a194: f9424108     	ldr	x8, [x8, #0x480]
4008a198: d51be208     	msr	CNTP_TVAL_EL0, x8
4008a19c: 52800028     	mov	w8, #0x1                // =1
4008a1a0: f90003e8     	str	x8, [sp]
4008a1a4: f94003e8     	ldr	x8, [sp]
4008a1a8: d51be228     	msr	CNTP_CTL_EL0, x8
4008a1ac: 910043ff     	add	sp, sp, #0x10
4008a1b0: d65f03c0     	ret
4008a1b4: d503201f     	nop
4008a1b8: d503201f     	nop
4008a1bc: d503201f     	nop

000000004008a1c0 <timer_reload>:
4008a1c0: f0000cc8     	adrp	x8, 0x40225000 <proc_table+0x7418>
4008a1c4: f9424108     	ldr	x8, [x8, #0x480]
4008a1c8: d51be208     	msr	CNTP_TVAL_EL0, x8
4008a1cc: d65f03c0     	ret

000000004008a1d0 <timer_get_ms>:
4008a1d0: d10083ff     	sub	sp, sp, #0x20
4008a1d4: d53be028     	mrs	x8, CNTPCT_EL0
4008a1d8: f9000be8     	str	x8, [sp, #0x10]
4008a1dc: d53be008     	mrs	x8, CNTFRQ_EL0
4008a1e0: f90007e8     	str	x8, [sp, #0x8]
4008a1e4: f94007e8     	ldr	x8, [sp, #0x8]
4008a1e8: b50000a8     	cbnz	x8, 0x4008a1fc <timer_get_ms+0x2c>
4008a1ec: 14000001     	b	0x4008a1f0 <timer_get_ms+0x20>
4008a1f0: aa1f03e8     	mov	x8, xzr
4008a1f4: f9000fe8     	str	x8, [sp, #0x18]
4008a1f8: 14000008     	b	0x4008a218 <timer_get_ms+0x48>
4008a1fc: f9400be8     	ldr	x8, [sp, #0x10]
4008a200: 52807d09     	mov	w9, #0x3e8              // =1000
4008a204: 9b097d08     	mul	x8, x8, x9
4008a208: f94007e9     	ldr	x9, [sp, #0x8]
4008a20c: 9ac90908     	udiv	x8, x8, x9
4008a210: f9000fe8     	str	x8, [sp, #0x18]
4008a214: 14000001     	b	0x4008a218 <timer_get_ms+0x48>
4008a218: f9400fe0     	ldr	x0, [sp, #0x18]
4008a21c: 910083ff     	add	sp, sp, #0x20
4008a220: d65f03c0     	ret
		...

000000004008a230 <debug_print_tf>:
4008a230: d10083ff     	sub	sp, sp, #0x20
4008a234: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008a238: 910043fd     	add	x29, sp, #0x10
4008a23c: f90007e0     	str	x0, [sp, #0x8]
4008a240: f94007e8     	ldr	x8, [sp, #0x8]
4008a244: f9402108     	ldr	x8, [x8, #0x40]
4008a248: f1000d08     	subs	x8, x8, #0x3
4008a24c: 54000241     	b.ne	0x4008a294 <debug_print_tf+0x64>
4008a250: 14000001     	b	0x4008a254 <debug_print_tf+0x24>
4008a254: d503201f     	nop
4008a258: 30019340     	adr	x0, 0x4008d4c1 <UART0_FR+0x181>
4008a25c: 97ffe961     	bl	0x400847e0 <uart_puts>
4008a260: f94007e8     	ldr	x8, [sp, #0x8]
4008a264: b9400100     	ldr	w0, [x8]
4008a268: 97ffe97a     	bl	0x40084850 <print_int>
4008a26c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a270: 91102c00     	add	x0, x0, #0x40b
4008a274: 97ffe95b     	bl	0x400847e0 <uart_puts>
4008a278: f94007e8     	ldr	x8, [sp, #0x8]
4008a27c: f9407d00     	ldr	x0, [x8, #0xf8]
4008a280: 97ffe9c8     	bl	0x400849a0 <uart_print_hex>
4008a284: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a288: 9122c000     	add	x0, x0, #0x8b0
4008a28c: 97ffe955     	bl	0x400847e0 <uart_puts>
4008a290: 14000001     	b	0x4008a294 <debug_print_tf+0x64>
4008a294: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008a298: 910083ff     	add	sp, sp, #0x20
4008a29c: d65f03c0     	ret

000000004008a2a0 <sync_handler_c>:
4008a2a0: d100c3ff     	sub	sp, sp, #0x30
4008a2a4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008a2a8: 910083fd     	add	x29, sp, #0x20
4008a2ac: f81f83a0     	stur	x0, [x29, #-0x8]
4008a2b0: d5385208     	mrs	x8, ESR_EL1
4008a2b4: f9000be8     	str	x8, [sp, #0x10]
4008a2b8: b94013e8     	ldr	w8, [sp, #0x10]
4008a2bc: 531a7d08     	lsr	w8, w8, #26
4008a2c0: b9000fe8     	str	w8, [sp, #0xc]
4008a2c4: b94013e8     	ldr	w8, [sp, #0x10]
4008a2c8: 12005d08     	and	w8, w8, #0xffffff
4008a2cc: b9000be8     	str	w8, [sp, #0x8]
4008a2d0: b9400fe8     	ldr	w8, [sp, #0xc]
4008a2d4: 71005508     	subs	w8, w8, #0x15
4008a2d8: 54000181     	b.ne	0x4008a308 <sync_handler_c+0x68>
4008a2dc: 14000001     	b	0x4008a2e0 <sync_handler_c+0x40>
4008a2e0: b9400be8     	ldr	w8, [sp, #0x8]
4008a2e4: 7103fd08     	subs	w8, w8, #0xff
4008a2e8: 54000101     	b.ne	0x4008a308 <sync_handler_c+0x68>
4008a2ec: 14000001     	b	0x4008a2f0 <sync_handler_c+0x50>
4008a2f0: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a2f4: 2a1f03e1     	mov	w1, wzr
4008a2f8: 97fff8f2     	bl	0x400886c0 <schedule>
4008a2fc: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008a300: 9100c3ff     	add	sp, sp, #0x30
4008a304: d65f03c0     	ret
4008a308: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a30c: 91176000     	add	x0, x0, #0x5d8
4008a310: 97ffe934     	bl	0x400847e0 <uart_puts>
4008a314: 97ffff17     	bl	0x40089f70 <get_cpuid>
4008a318: 2a0003e8     	mov	w8, w0
4008a31c: 2a0803e0     	mov	w0, w8
4008a320: 97ffe9a0     	bl	0x400849a0 <uart_print_hex>
4008a324: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a328: 912c9000     	add	x0, x0, #0xb24
4008a32c: 97ffe92d     	bl	0x400847e0 <uart_puts>
4008a330: f9400be0     	ldr	x0, [sp, #0x10]
4008a334: 97ffe99b     	bl	0x400849a0 <uart_print_hex>
4008a338: d5386008     	mrs	x8, FAR_EL1
4008a33c: f90003e8     	str	x8, [sp]
4008a340: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a344: 910d4800     	add	x0, x0, #0x352
4008a348: 97ffe926     	bl	0x400847e0 <uart_puts>
4008a34c: f94003e0     	ldr	x0, [sp]
4008a350: 97ffe994     	bl	0x400849a0 <uart_print_hex>
4008a354: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a358: 910d6800     	add	x0, x0, #0x35a
4008a35c: 97ffe921     	bl	0x400847e0 <uart_puts>
4008a360: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a364: f9407d00     	ldr	x0, [x8, #0xf8]
4008a368: 97ffe98e     	bl	0x400849a0 <uart_print_hex>
4008a36c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a370: 9122c000     	add	x0, x0, #0x8b0
4008a374: 97ffe91b     	bl	0x400847e0 <uart_puts>
4008a378: 14000001     	b	0x4008a37c <sync_handler_c+0xdc>
4008a37c: 14000000     	b	0x4008a37c <sync_handler_c+0xdc>

000000004008a380 <sync_lower_handler_c>:
4008a380: d10183ff     	sub	sp, sp, #0x60
4008a384: a9057bfd     	stp	x29, x30, [sp, #0x50]
4008a388: 910143fd     	add	x29, sp, #0x50
4008a38c: f81f83a0     	stur	x0, [x29, #-0x8]
4008a390: d5385208     	mrs	x8, ESR_EL1
4008a394: f81f03a8     	stur	x8, [x29, #-0x10]
4008a398: f85f03a8     	ldur	x8, [x29, #-0x10]
4008a39c: d35a7d08     	ubfx	x8, x8, #26, #6
4008a3a0: f81e83a8     	stur	x8, [x29, #-0x18]
4008a3a4: b85f03a8     	ldur	w8, [x29, #-0x10]
4008a3a8: 12005d08     	and	w8, w8, #0xffffff
4008a3ac: b81e43a8     	stur	w8, [x29, #-0x1c]
4008a3b0: f85e83a8     	ldur	x8, [x29, #-0x18]
4008a3b4: f1005508     	subs	x8, x8, #0x15
4008a3b8: 54001a21     	b.ne	0x4008a6fc <sync_lower_handler_c+0x37c>
4008a3bc: 14000001     	b	0x4008a3c0 <sync_lower_handler_c+0x40>
4008a3c0: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008a3c4: 7103fd08     	subs	w8, w8, #0xff
4008a3c8: 54000521     	b.ne	0x4008a46c <sync_lower_handler_c+0xec>
4008a3cc: 14000001     	b	0x4008a3d0 <sync_lower_handler_c+0x50>
4008a3d0: 97fff724     	bl	0x40088060 <current_process>
4008a3d4: b40000c0     	cbz	x0, 0x4008a3ec <sync_lower_handler_c+0x6c>
4008a3d8: 14000001     	b	0x4008a3dc <sync_lower_handler_c+0x5c>
4008a3dc: 97fff721     	bl	0x40088060 <current_process>
4008a3e0: b9400008     	ldr	w8, [x0]
4008a3e4: b9000fe8     	str	w8, [sp, #0xc]
4008a3e8: 14000004     	b	0x4008a3f8 <sync_lower_handler_c+0x78>
4008a3ec: 12800008     	mov	w8, #-0x1               // =-1
4008a3f0: b9000fe8     	str	w8, [sp, #0xc]
4008a3f4: 14000001     	b	0x4008a3f8 <sync_lower_handler_c+0x78>
4008a3f8: b9400fe8     	ldr	w8, [sp, #0xc]
4008a3fc: b81e03a8     	stur	w8, [x29, #-0x20]
4008a400: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a404: 2a1f03e1     	mov	w1, wzr
4008a408: 97fff8ae     	bl	0x400886c0 <schedule>
4008a40c: 97fff715     	bl	0x40088060 <current_process>
4008a410: b40000c0     	cbz	x0, 0x4008a428 <sync_lower_handler_c+0xa8>
4008a414: 14000001     	b	0x4008a418 <sync_lower_handler_c+0x98>
4008a418: 97fff712     	bl	0x40088060 <current_process>
4008a41c: b9400008     	ldr	w8, [x0]
4008a420: b9000be8     	str	w8, [sp, #0x8]
4008a424: 14000004     	b	0x4008a434 <sync_lower_handler_c+0xb4>
4008a428: 12800008     	mov	w8, #-0x1               // =-1
4008a42c: b9000be8     	str	w8, [sp, #0x8]
4008a430: 14000001     	b	0x4008a434 <sync_lower_handler_c+0xb4>
4008a434: b9400be8     	ldr	w8, [sp, #0x8]
4008a438: b81dc3a8     	stur	w8, [x29, #-0x24]
4008a43c: b85e03a8     	ldur	w8, [x29, #-0x20]
4008a440: b85dc3a9     	ldur	w9, [x29, #-0x24]
4008a444: 6b090108     	subs	w8, w8, w9
4008a448: 54000101     	b.ne	0x4008a468 <sync_lower_handler_c+0xe8>
4008a44c: 14000001     	b	0x4008a450 <sync_lower_handler_c+0xd0>
4008a450: b85e03a8     	ldur	w8, [x29, #-0x20]
4008a454: 31000508     	adds	w8, w8, #0x1
4008a458: 54000080     	b.eq	0x4008a468 <sync_lower_handler_c+0xe8>
4008a45c: 14000001     	b	0x4008a460 <sync_lower_handler_c+0xe0>
4008a460: d503207f     	wfi
4008a464: 14000001     	b	0x4008a468 <sync_lower_handler_c+0xe8>
4008a468: 1400010d     	b	0x4008a89c <sync_lower_handler_c+0x51c>
4008a46c: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a470: f9402108     	ldr	x8, [x8, #0x40]
4008a474: f90013e8     	str	x8, [sp, #0x20]
4008a478: f94013e8     	ldr	x8, [sp, #0x20]
4008a47c: f1000508     	subs	x8, x8, #0x1
4008a480: 540000a1     	b.ne	0x4008a494 <sync_lower_handler_c+0x114>
4008a484: 14000001     	b	0x4008a488 <sync_lower_handler_c+0x108>
4008a488: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a48c: 94000109     	bl	0x4008a8b0 <sys_write_console>
4008a490: 1400009a     	b	0x4008a6f8 <sync_lower_handler_c+0x378>
4008a494: f94013e8     	ldr	x8, [sp, #0x20]
4008a498: f1000908     	subs	x8, x8, #0x2
4008a49c: 540000a1     	b.ne	0x4008a4b0 <sync_lower_handler_c+0x130>
4008a4a0: 14000001     	b	0x4008a4a4 <sync_lower_handler_c+0x124>
4008a4a4: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a4a8: 94000122     	bl	0x4008a930 <sys_exit>
4008a4ac: 14000092     	b	0x4008a6f4 <sync_lower_handler_c+0x374>
4008a4b0: f94013e8     	ldr	x8, [sp, #0x20]
4008a4b4: f1000d08     	subs	x8, x8, #0x3
4008a4b8: 540000a1     	b.ne	0x4008a4cc <sync_lower_handler_c+0x14c>
4008a4bc: 14000001     	b	0x4008a4c0 <sync_lower_handler_c+0x140>
4008a4c0: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a4c4: 94000127     	bl	0x4008a960 <sys_fork>
4008a4c8: 1400008a     	b	0x4008a6f0 <sync_lower_handler_c+0x370>
4008a4cc: f94013e8     	ldr	x8, [sp, #0x20]
4008a4d0: f1001108     	subs	x8, x8, #0x4
4008a4d4: 540000a1     	b.ne	0x4008a4e8 <sync_lower_handler_c+0x168>
4008a4d8: 14000001     	b	0x4008a4dc <sync_lower_handler_c+0x15c>
4008a4dc: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a4e0: 94000138     	bl	0x4008a9c0 <sys_open>
4008a4e4: 14000082     	b	0x4008a6ec <sync_lower_handler_c+0x36c>
4008a4e8: f94013e8     	ldr	x8, [sp, #0x20]
4008a4ec: f1001508     	subs	x8, x8, #0x5
4008a4f0: 540000a1     	b.ne	0x4008a504 <sync_lower_handler_c+0x184>
4008a4f4: 14000001     	b	0x4008a4f8 <sync_lower_handler_c+0x178>
4008a4f8: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a4fc: 94000155     	bl	0x4008aa50 <sys_close>
4008a500: 1400007a     	b	0x4008a6e8 <sync_lower_handler_c+0x368>
4008a504: f94013e8     	ldr	x8, [sp, #0x20]
4008a508: f1001908     	subs	x8, x8, #0x6
4008a50c: 540000a1     	b.ne	0x4008a520 <sync_lower_handler_c+0x1a0>
4008a510: 14000001     	b	0x4008a514 <sync_lower_handler_c+0x194>
4008a514: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a518: 94000162     	bl	0x4008aaa0 <sys_read>
4008a51c: 14000072     	b	0x4008a6e4 <sync_lower_handler_c+0x364>
4008a520: f94013e8     	ldr	x8, [sp, #0x20]
4008a524: f1001d08     	subs	x8, x8, #0x7
4008a528: 540000a1     	b.ne	0x4008a53c <sync_lower_handler_c+0x1bc>
4008a52c: 14000001     	b	0x4008a530 <sync_lower_handler_c+0x1b0>
4008a530: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a534: 94000197     	bl	0x4008ab90 <sys_write>
4008a538: 1400006a     	b	0x4008a6e0 <sync_lower_handler_c+0x360>
4008a53c: f94013e8     	ldr	x8, [sp, #0x20]
4008a540: f1002108     	subs	x8, x8, #0x8
4008a544: 540000a1     	b.ne	0x4008a558 <sync_lower_handler_c+0x1d8>
4008a548: 14000001     	b	0x4008a54c <sync_lower_handler_c+0x1cc>
4008a54c: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a550: 940001cc     	bl	0x4008ac80 <sys_spawn>
4008a554: 14000062     	b	0x4008a6dc <sync_lower_handler_c+0x35c>
4008a558: f94013e8     	ldr	x8, [sp, #0x20]
4008a55c: f1002508     	subs	x8, x8, #0x9
4008a560: 540000a1     	b.ne	0x4008a574 <sync_lower_handler_c+0x1f4>
4008a564: 14000001     	b	0x4008a568 <sync_lower_handler_c+0x1e8>
4008a568: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a56c: 94000235     	bl	0x4008ae40 <sys_map_fb>
4008a570: 1400005a     	b	0x4008a6d8 <sync_lower_handler_c+0x358>
4008a574: f94013e8     	ldr	x8, [sp, #0x20]
4008a578: f1002908     	subs	x8, x8, #0xa
4008a57c: 540000a1     	b.ne	0x4008a590 <sync_lower_handler_c+0x210>
4008a580: 14000001     	b	0x4008a584 <sync_lower_handler_c+0x204>
4008a584: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a588: 9400023e     	bl	0x4008ae80 <sys_flush_fb>
4008a58c: 14000052     	b	0x4008a6d4 <sync_lower_handler_c+0x354>
4008a590: f94013e8     	ldr	x8, [sp, #0x20]
4008a594: f1002d08     	subs	x8, x8, #0xb
4008a598: 540000a1     	b.ne	0x4008a5ac <sync_lower_handler_c+0x22c>
4008a59c: 14000001     	b	0x4008a5a0 <sync_lower_handler_c+0x220>
4008a5a0: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a5a4: 94000243     	bl	0x4008aeb0 <sys_get_cpuid>
4008a5a8: 1400004a     	b	0x4008a6d0 <sync_lower_handler_c+0x350>
4008a5ac: f94013e8     	ldr	x8, [sp, #0x20]
4008a5b0: f1003108     	subs	x8, x8, #0xc
4008a5b4: 540000a1     	b.ne	0x4008a5c8 <sync_lower_handler_c+0x248>
4008a5b8: 14000001     	b	0x4008a5bc <sync_lower_handler_c+0x23c>
4008a5bc: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a5c0: 94000248     	bl	0x4008aee0 <sys_pipe>
4008a5c4: 14000042     	b	0x4008a6cc <sync_lower_handler_c+0x34c>
4008a5c8: f94013e8     	ldr	x8, [sp, #0x20]
4008a5cc: f1003508     	subs	x8, x8, #0xd
4008a5d0: 540000a1     	b.ne	0x4008a5e4 <sync_lower_handler_c+0x264>
4008a5d4: 14000001     	b	0x4008a5d8 <sync_lower_handler_c+0x258>
4008a5d8: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a5dc: 94000275     	bl	0x4008afb0 <sys_get_events>
4008a5e0: 1400003a     	b	0x4008a6c8 <sync_lower_handler_c+0x348>
4008a5e4: f94013e8     	ldr	x8, [sp, #0x20]
4008a5e8: f1003908     	subs	x8, x8, #0xe
4008a5ec: 540000a1     	b.ne	0x4008a600 <sync_lower_handler_c+0x280>
4008a5f0: 14000001     	b	0x4008a5f4 <sync_lower_handler_c+0x274>
4008a5f4: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a5f8: 94000296     	bl	0x4008b050 <sys_available>
4008a5fc: 14000032     	b	0x4008a6c4 <sync_lower_handler_c+0x344>
4008a600: f94013e8     	ldr	x8, [sp, #0x20]
4008a604: f1003d08     	subs	x8, x8, #0xf
4008a608: 540000a1     	b.ne	0x4008a61c <sync_lower_handler_c+0x29c>
4008a60c: 14000001     	b	0x4008a610 <sync_lower_handler_c+0x290>
4008a610: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a614: 940002a3     	bl	0x4008b0a0 <sys_read_dir>
4008a618: 1400002a     	b	0x4008a6c0 <sync_lower_handler_c+0x340>
4008a61c: f94013e8     	ldr	x8, [sp, #0x20]
4008a620: f1004108     	subs	x8, x8, #0x10
4008a624: 540000a1     	b.ne	0x4008a638 <sync_lower_handler_c+0x2b8>
4008a628: 14000001     	b	0x4008a62c <sync_lower_handler_c+0x2ac>
4008a62c: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a630: 940002c0     	bl	0x4008b130 <sys_kill>
4008a634: 14000022     	b	0x4008a6bc <sync_lower_handler_c+0x33c>
4008a638: f94013e8     	ldr	x8, [sp, #0x20]
4008a63c: f1004508     	subs	x8, x8, #0x11
4008a640: 540000c1     	b.ne	0x4008a658 <sync_lower_handler_c+0x2d8>
4008a644: 14000001     	b	0x4008a648 <sync_lower_handler_c+0x2c8>
4008a648: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a64c: 52800021     	mov	w1, #0x1                // =1
4008a650: 97fff81c     	bl	0x400886c0 <schedule>
4008a654: 14000019     	b	0x4008a6b8 <sync_lower_handler_c+0x338>
4008a658: f94013e8     	ldr	x8, [sp, #0x20]
4008a65c: f1004908     	subs	x8, x8, #0x12
4008a660: 540000a1     	b.ne	0x4008a674 <sync_lower_handler_c+0x2f4>
4008a664: 14000001     	b	0x4008a668 <sync_lower_handler_c+0x2e8>
4008a668: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a66c: 940002c1     	bl	0x4008b170 <sys_connect>
4008a670: 14000011     	b	0x4008a6b4 <sync_lower_handler_c+0x334>
4008a674: f94013e8     	ldr	x8, [sp, #0x20]
4008a678: f103fd08     	subs	x8, x8, #0xff
4008a67c: 540000c1     	b.ne	0x4008a694 <sync_lower_handler_c+0x314>
4008a680: 14000001     	b	0x4008a684 <sync_lower_handler_c+0x304>
4008a684: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a688: 52800021     	mov	w1, #0x1                // =1
4008a68c: 97fff80d     	bl	0x400886c0 <schedule>
4008a690: 14000008     	b	0x4008a6b0 <sync_lower_handler_c+0x330>
4008a694: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a698: 9117b400     	add	x0, x0, #0x5ed
4008a69c: 97ffe851     	bl	0x400847e0 <uart_puts>
4008a6a0: f85f83a9     	ldur	x9, [x29, #-0x8]
4008a6a4: 92800008     	mov	x8, #-0x1               // =-1
4008a6a8: f9000128     	str	x8, [x9]
4008a6ac: 14000001     	b	0x4008a6b0 <sync_lower_handler_c+0x330>
4008a6b0: 14000001     	b	0x4008a6b4 <sync_lower_handler_c+0x334>
4008a6b4: 14000001     	b	0x4008a6b8 <sync_lower_handler_c+0x338>
4008a6b8: 14000001     	b	0x4008a6bc <sync_lower_handler_c+0x33c>
4008a6bc: 14000001     	b	0x4008a6c0 <sync_lower_handler_c+0x340>
4008a6c0: 14000001     	b	0x4008a6c4 <sync_lower_handler_c+0x344>
4008a6c4: 14000001     	b	0x4008a6c8 <sync_lower_handler_c+0x348>
4008a6c8: 14000001     	b	0x4008a6cc <sync_lower_handler_c+0x34c>
4008a6cc: 14000001     	b	0x4008a6d0 <sync_lower_handler_c+0x350>
4008a6d0: 14000001     	b	0x4008a6d4 <sync_lower_handler_c+0x354>
4008a6d4: 14000001     	b	0x4008a6d8 <sync_lower_handler_c+0x358>
4008a6d8: 14000001     	b	0x4008a6dc <sync_lower_handler_c+0x35c>
4008a6dc: 14000001     	b	0x4008a6e0 <sync_lower_handler_c+0x360>
4008a6e0: 14000001     	b	0x4008a6e4 <sync_lower_handler_c+0x364>
4008a6e4: 14000001     	b	0x4008a6e8 <sync_lower_handler_c+0x368>
4008a6e8: 14000001     	b	0x4008a6ec <sync_lower_handler_c+0x36c>
4008a6ec: 14000001     	b	0x4008a6f0 <sync_lower_handler_c+0x370>
4008a6f0: 14000001     	b	0x4008a6f4 <sync_lower_handler_c+0x374>
4008a6f4: 14000001     	b	0x4008a6f8 <sync_lower_handler_c+0x378>
4008a6f8: 14000069     	b	0x4008a89c <sync_lower_handler_c+0x51c>
4008a6fc: f85e83a8     	ldur	x8, [x29, #-0x18]
4008a700: f1008108     	subs	x8, x8, #0x20
4008a704: 54000120     	b.eq	0x4008a728 <sync_lower_handler_c+0x3a8>
4008a708: 14000001     	b	0x4008a70c <sync_lower_handler_c+0x38c>
4008a70c: f85e83a8     	ldur	x8, [x29, #-0x18]
4008a710: f1009108     	subs	x8, x8, #0x24
4008a714: 540000a0     	b.eq	0x4008a728 <sync_lower_handler_c+0x3a8>
4008a718: 14000001     	b	0x4008a71c <sync_lower_handler_c+0x39c>
4008a71c: f85e83a8     	ldur	x8, [x29, #-0x18]
4008a720: b5000828     	cbnz	x8, 0x4008a824 <sync_lower_handler_c+0x4a4>
4008a724: 14000001     	b	0x4008a728 <sync_lower_handler_c+0x3a8>
4008a728: 97fff64e     	bl	0x40088060 <current_process>
4008a72c: f9000fe0     	str	x0, [sp, #0x18]
4008a730: f9400fe8     	ldr	x8, [sp, #0x18]
4008a734: b40004e8     	cbz	x8, 0x4008a7d0 <sync_lower_handler_c+0x450>
4008a738: 14000001     	b	0x4008a73c <sync_lower_handler_c+0x3bc>
4008a73c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a740: 9119b800     	add	x0, x0, #0x66e
4008a744: 97ffe827     	bl	0x400847e0 <uart_puts>
4008a748: f9400fe8     	ldr	x8, [sp, #0x18]
4008a74c: b9400100     	ldr	w0, [x8]
4008a750: 97ffe840     	bl	0x40084850 <print_int>
4008a754: f9400fe8     	ldr	x8, [sp, #0x18]
4008a758: 39404108     	ldrb	w8, [x8, #0x10]
4008a75c: 34000188     	cbz	w8, 0x4008a78c <sync_lower_handler_c+0x40c>
4008a760: 14000001     	b	0x4008a764 <sync_lower_handler_c+0x3e4>
4008a764: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a768: 911d3000     	add	x0, x0, #0x74c
4008a76c: 97ffe81d     	bl	0x400847e0 <uart_puts>
4008a770: f9400fe8     	ldr	x8, [sp, #0x18]
4008a774: 91004100     	add	x0, x8, #0x10
4008a778: 97ffe81a     	bl	0x400847e0 <uart_puts>
4008a77c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a780: 911bc400     	add	x0, x0, #0x6f1
4008a784: 97ffe817     	bl	0x400847e0 <uart_puts>
4008a788: 14000001     	b	0x4008a78c <sync_lower_handler_c+0x40c>
4008a78c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a790: 91168400     	add	x0, x0, #0x5a1
4008a794: 97ffe813     	bl	0x400847e0 <uart_puts>
4008a798: f85e83a0     	ldur	x0, [x29, #-0x18]
4008a79c: 97ffe881     	bl	0x400849a0 <uart_print_hex>
4008a7a0: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a7a4: 91182c00     	add	x0, x0, #0x60b
4008a7a8: 97ffe80e     	bl	0x400847e0 <uart_puts>
4008a7ac: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a7b0: f9407d00     	ldr	x0, [x8, #0xf8]
4008a7b4: 97ffe87b     	bl	0x400849a0 <uart_print_hex>
4008a7b8: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a7bc: 9122c000     	add	x0, x0, #0x8b0
4008a7c0: 97ffe808     	bl	0x400847e0 <uart_puts>
4008a7c4: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a7c8: 97fff93e     	bl	0x40088cc0 <process_exit>
4008a7cc: 14000015     	b	0x4008a820 <sync_lower_handler_c+0x4a0>
4008a7d0: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a7d4: 911ea800     	add	x0, x0, #0x7aa
4008a7d8: 97ffe802     	bl	0x400847e0 <uart_puts>
4008a7dc: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a7e0: 9116c000     	add	x0, x0, #0x5b0
4008a7e4: 97ffe7ff     	bl	0x400847e0 <uart_puts>
4008a7e8: f85e83a0     	ldur	x0, [x29, #-0x18]
4008a7ec: 97ffe86d     	bl	0x400849a0 <uart_print_hex>
4008a7f0: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a7f4: 910d2c00     	add	x0, x0, #0x34b
4008a7f8: 97ffe7fa     	bl	0x400847e0 <uart_puts>
4008a7fc: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a800: f9407d00     	ldr	x0, [x8, #0xf8]
4008a804: 97ffe867     	bl	0x400849a0 <uart_print_hex>
4008a808: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a80c: 9122c000     	add	x0, x0, #0x8b0
4008a810: 97ffe7f4     	bl	0x400847e0 <uart_puts>
4008a814: 14000001     	b	0x4008a818 <sync_lower_handler_c+0x498>
4008a818: d503207f     	wfi
4008a81c: 17ffffff     	b	0x4008a818 <sync_lower_handler_c+0x498>
4008a820: 1400001e     	b	0x4008a898 <sync_lower_handler_c+0x518>
4008a824: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a828: 91184800     	add	x0, x0, #0x612
4008a82c: 97ffe7ed     	bl	0x400847e0 <uart_puts>
4008a830: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a834: 9116c000     	add	x0, x0, #0x5b0
4008a838: 97ffe7ea     	bl	0x400847e0 <uart_puts>
4008a83c: f85e83a0     	ldur	x0, [x29, #-0x18]
4008a840: 97ffe858     	bl	0x400849a0 <uart_print_hex>
4008a844: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a848: 910d2c00     	add	x0, x0, #0x34b
4008a84c: 97ffe7e5     	bl	0x400847e0 <uart_puts>
4008a850: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a854: f9407d00     	ldr	x0, [x8, #0xf8]
4008a858: 97ffe852     	bl	0x400849a0 <uart_print_hex>
4008a85c: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a860: 9122c000     	add	x0, x0, #0x8b0
4008a864: 97ffe7df     	bl	0x400847e0 <uart_puts>
4008a868: 97fff5fe     	bl	0x40088060 <current_process>
4008a86c: f9000be0     	str	x0, [sp, #0x10]
4008a870: f9400be8     	ldr	x8, [sp, #0x10]
4008a874: b40000a8     	cbz	x8, 0x4008a888 <sync_lower_handler_c+0x508>
4008a878: 14000001     	b	0x4008a87c <sync_lower_handler_c+0x4fc>
4008a87c: f85f83a0     	ldur	x0, [x29, #-0x8]
4008a880: 97fff910     	bl	0x40088cc0 <process_exit>
4008a884: 14000004     	b	0x4008a894 <sync_lower_handler_c+0x514>
4008a888: 14000001     	b	0x4008a88c <sync_lower_handler_c+0x50c>
4008a88c: d503207f     	wfi
4008a890: 17ffffff     	b	0x4008a88c <sync_lower_handler_c+0x50c>
4008a894: 14000001     	b	0x4008a898 <sync_lower_handler_c+0x518>
4008a898: 14000001     	b	0x4008a89c <sync_lower_handler_c+0x51c>
4008a89c: a9457bfd     	ldp	x29, x30, [sp, #0x50]
4008a8a0: 910183ff     	add	sp, sp, #0x60
4008a8a4: d65f03c0     	ret
4008a8a8: d503201f     	nop
4008a8ac: d503201f     	nop

000000004008a8b0 <sys_write_console>:
4008a8b0: d10083ff     	sub	sp, sp, #0x20
4008a8b4: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008a8b8: 910043fd     	add	x29, sp, #0x10
4008a8bc: f90007e0     	str	x0, [sp, #0x8]
4008a8c0: f94007e8     	ldr	x8, [sp, #0x8]
4008a8c4: f9400108     	ldr	x8, [x8]
4008a8c8: f90003e8     	str	x8, [sp]
4008a8cc: f94003e8     	ldr	x8, [sp]
4008a8d0: d35afd08     	lsr	x8, x8, #26
4008a8d4: f1004508     	subs	x8, x8, #0x11
4008a8d8: 540001a3     	b.lo	0x4008a90c <sys_write_console+0x5c>
4008a8dc: 14000001     	b	0x4008a8e0 <sys_write_console+0x30>
4008a8e0: f94003e8     	ldr	x8, [sp]
4008a8e4: d355fd08     	lsr	x8, x8, #21
4008a8e8: f1088108     	subs	x8, x8, #0x220
4008a8ec: 54000108     	b.hi	0x4008a90c <sys_write_console+0x5c>
4008a8f0: 14000001     	b	0x4008a8f4 <sys_write_console+0x44>
4008a8f4: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a8f8: 910d8800     	add	x0, x0, #0x362
4008a8fc: 97ffe7b9     	bl	0x400847e0 <uart_puts>
4008a900: f94003e0     	ldr	x0, [sp]
4008a904: 97ffe7b7     	bl	0x400847e0 <uart_puts>
4008a908: 14000001     	b	0x4008a90c <sys_write_console+0x5c>
4008a90c: f94007e9     	ldr	x9, [sp, #0x8]
4008a910: aa1f03e8     	mov	x8, xzr
4008a914: f9000128     	str	x8, [x9]
4008a918: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008a91c: 910083ff     	add	sp, sp, #0x20
4008a920: d65f03c0     	ret
4008a924: d503201f     	nop
4008a928: d503201f     	nop
4008a92c: d503201f     	nop

000000004008a930 <sys_exit>:
4008a930: d10083ff     	sub	sp, sp, #0x20
4008a934: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008a938: 910043fd     	add	x29, sp, #0x10
4008a93c: f90007e0     	str	x0, [sp, #0x8]
4008a940: f94007e0     	ldr	x0, [sp, #0x8]
4008a944: 97fff8df     	bl	0x40088cc0 <process_exit>
4008a948: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008a94c: 910083ff     	add	sp, sp, #0x20
4008a950: d65f03c0     	ret
4008a954: d503201f     	nop
4008a958: d503201f     	nop
4008a95c: d503201f     	nop

000000004008a960 <sys_fork>:
4008a960: d10083ff     	sub	sp, sp, #0x20
4008a964: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008a968: 910043fd     	add	x29, sp, #0x10
4008a96c: f90007e0     	str	x0, [sp, #0x8]
4008a970: f94007e0     	ldr	x0, [sp, #0x8]
4008a974: 97fffa57     	bl	0x400892d0 <process_fork>
4008a978: b90007e0     	str	w0, [sp, #0x4]
4008a97c: b98007e8     	ldrsw	x8, [sp, #0x4]
4008a980: f94007e9     	ldr	x9, [sp, #0x8]
4008a984: f9000128     	str	x8, [x9]
4008a988: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a98c: 9128c000     	add	x0, x0, #0xa30
4008a990: 97ffe794     	bl	0x400847e0 <uart_puts>
4008a994: f94007e8     	ldr	x8, [sp, #0x8]
4008a998: b9400100     	ldr	w0, [x8]
4008a99c: 97ffe7ad     	bl	0x40084850 <print_int>
4008a9a0: f0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008a9a4: 9122c000     	add	x0, x0, #0x8b0
4008a9a8: 97ffe78e     	bl	0x400847e0 <uart_puts>
4008a9ac: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008a9b0: 910083ff     	add	sp, sp, #0x20
4008a9b4: d65f03c0     	ret
4008a9b8: d503201f     	nop
4008a9bc: d503201f     	nop

000000004008a9c0 <sys_open>:
4008a9c0: d100c3ff     	sub	sp, sp, #0x30
4008a9c4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008a9c8: 910083fd     	add	x29, sp, #0x20
4008a9cc: f81f83a0     	stur	x0, [x29, #-0x8]
4008a9d0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008a9d4: f9400108     	ldr	x8, [x8]
4008a9d8: f9000be8     	str	x8, [sp, #0x10]
4008a9dc: 97fff5a1     	bl	0x40088060 <current_process>
4008a9e0: f90007e0     	str	x0, [sp, #0x8]
4008a9e4: f9400be8     	ldr	x8, [sp, #0x10]
4008a9e8: d35afd08     	lsr	x8, x8, #26
4008a9ec: f1004508     	subs	x8, x8, #0x11
4008a9f0: 540001e3     	b.lo	0x4008aa2c <sys_open+0x6c>
4008a9f4: 14000001     	b	0x4008a9f8 <sys_open+0x38>
4008a9f8: f9400be8     	ldr	x8, [sp, #0x10]
4008a9fc: d355fd08     	lsr	x8, x8, #21
4008aa00: f1088108     	subs	x8, x8, #0x220
4008aa04: 54000148     	b.hi	0x4008aa2c <sys_open+0x6c>
4008aa08: 14000001     	b	0x4008aa0c <sys_open+0x4c>
4008aa0c: f94007e0     	ldr	x0, [sp, #0x8]
4008aa10: f9400be1     	ldr	x1, [sp, #0x10]
4008aa14: 97ffe29b     	bl	0x40083480 <file_open>
4008aa18: 2a0003e8     	mov	w8, w0
4008aa1c: 93407d08     	sxtw	x8, w8
4008aa20: f85f83a9     	ldur	x9, [x29, #-0x8]
4008aa24: f9000128     	str	x8, [x9]
4008aa28: 14000005     	b	0x4008aa3c <sys_open+0x7c>
4008aa2c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008aa30: 92800008     	mov	x8, #-0x1               // =-1
4008aa34: f9000128     	str	x8, [x9]
4008aa38: 14000001     	b	0x4008aa3c <sys_open+0x7c>
4008aa3c: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008aa40: 9100c3ff     	add	sp, sp, #0x30
4008aa44: d65f03c0     	ret
4008aa48: d503201f     	nop
4008aa4c: d503201f     	nop

000000004008aa50 <sys_close>:
4008aa50: d100c3ff     	sub	sp, sp, #0x30
4008aa54: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008aa58: 910083fd     	add	x29, sp, #0x20
4008aa5c: f81f83a0     	stur	x0, [x29, #-0x8]
4008aa60: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aa64: f9400108     	ldr	x8, [x8]
4008aa68: b81f43a8     	stur	w8, [x29, #-0xc]
4008aa6c: 97fff57d     	bl	0x40088060 <current_process>
4008aa70: f90007e0     	str	x0, [sp, #0x8]
4008aa74: f94007e0     	ldr	x0, [sp, #0x8]
4008aa78: b85f43a1     	ldur	w1, [x29, #-0xc]
4008aa7c: 97ffe39d     	bl	0x400838f0 <file_close>
4008aa80: 2a0003e8     	mov	w8, w0
4008aa84: 93407d08     	sxtw	x8, w8
4008aa88: f85f83a9     	ldur	x9, [x29, #-0x8]
4008aa8c: f9000128     	str	x8, [x9]
4008aa90: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008aa94: 9100c3ff     	add	sp, sp, #0x30
4008aa98: d65f03c0     	ret
4008aa9c: d503201f     	nop

000000004008aaa0 <sys_read>:
4008aaa0: d10103ff     	sub	sp, sp, #0x40
4008aaa4: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008aaa8: 9100c3fd     	add	x29, sp, #0x30
4008aaac: f81f83a0     	stur	x0, [x29, #-0x8]
4008aab0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aab4: f9400108     	ldr	x8, [x8]
4008aab8: b81f43a8     	stur	w8, [x29, #-0xc]
4008aabc: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aac0: f9400508     	ldr	x8, [x8, #0x8]
4008aac4: f9000fe8     	str	x8, [sp, #0x18]
4008aac8: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aacc: f9400908     	ldr	x8, [x8, #0x10]
4008aad0: b90017e8     	str	w8, [sp, #0x14]
4008aad4: 97fff563     	bl	0x40088060 <current_process>
4008aad8: f90007e0     	str	x0, [sp, #0x8]
4008aadc: f9400fe8     	ldr	x8, [sp, #0x18]
4008aae0: d35afd08     	lsr	x8, x8, #26
4008aae4: f1004508     	subs	x8, x8, #0x11
4008aae8: 54000423     	b.lo	0x4008ab6c <sys_read+0xcc>
4008aaec: 14000001     	b	0x4008aaf0 <sys_read+0x50>
4008aaf0: f9400fe8     	ldr	x8, [sp, #0x18]
4008aaf4: b98017e9     	ldrsw	x9, [sp, #0x14]
4008aaf8: 8b090108     	add	x8, x8, x9
4008aafc: 52a88409     	mov	w9, #0x44200000         // =1142947840
4008ab00: eb090108     	subs	x8, x8, x9
4008ab04: 54000348     	b.hi	0x4008ab6c <sys_read+0xcc>
4008ab08: 14000001     	b	0x4008ab0c <sys_read+0x6c>
4008ab0c: f94007e0     	ldr	x0, [sp, #0x8]
4008ab10: b85f43a1     	ldur	w1, [x29, #-0xc]
4008ab14: f9400fe2     	ldr	x2, [sp, #0x18]
4008ab18: b94017e3     	ldr	w3, [sp, #0x14]
4008ab1c: f85f83a4     	ldur	x4, [x29, #-0x8]
4008ab20: 97ffe42c     	bl	0x40083bd0 <file_read>
4008ab24: b90007e0     	str	w0, [sp, #0x4]
4008ab28: b94007e8     	ldr	w8, [sp, #0x4]
4008ab2c: 31000908     	adds	w8, w8, #0x2
4008ab30: 54000141     	b.ne	0x4008ab58 <sys_read+0xb8>
4008ab34: 14000001     	b	0x4008ab38 <sys_read+0x98>
4008ab38: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ab3c: f9407d28     	ldr	x8, [x9, #0xf8]
4008ab40: f1001108     	subs	x8, x8, #0x4
4008ab44: f9007d28     	str	x8, [x9, #0xf8]
4008ab48: f85f83a0     	ldur	x0, [x29, #-0x8]
4008ab4c: 2a1f03e1     	mov	w1, wzr
4008ab50: 97fff6dc     	bl	0x400886c0 <schedule>
4008ab54: 14000005     	b	0x4008ab68 <sys_read+0xc8>
4008ab58: b98007e8     	ldrsw	x8, [sp, #0x4]
4008ab5c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ab60: f9000128     	str	x8, [x9]
4008ab64: 14000001     	b	0x4008ab68 <sys_read+0xc8>
4008ab68: 14000005     	b	0x4008ab7c <sys_read+0xdc>
4008ab6c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ab70: 92800008     	mov	x8, #-0x1               // =-1
4008ab74: f9000128     	str	x8, [x9]
4008ab78: 14000001     	b	0x4008ab7c <sys_read+0xdc>
4008ab7c: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008ab80: 910103ff     	add	sp, sp, #0x40
4008ab84: d65f03c0     	ret
4008ab88: d503201f     	nop
4008ab8c: d503201f     	nop

000000004008ab90 <sys_write>:
4008ab90: d10103ff     	sub	sp, sp, #0x40
4008ab94: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008ab98: 9100c3fd     	add	x29, sp, #0x30
4008ab9c: f81f83a0     	stur	x0, [x29, #-0x8]
4008aba0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aba4: f9400108     	ldr	x8, [x8]
4008aba8: b81f43a8     	stur	w8, [x29, #-0xc]
4008abac: f85f83a8     	ldur	x8, [x29, #-0x8]
4008abb0: f9400508     	ldr	x8, [x8, #0x8]
4008abb4: f9000fe8     	str	x8, [sp, #0x18]
4008abb8: f85f83a8     	ldur	x8, [x29, #-0x8]
4008abbc: f9400908     	ldr	x8, [x8, #0x10]
4008abc0: b90017e8     	str	w8, [sp, #0x14]
4008abc4: 97fff527     	bl	0x40088060 <current_process>
4008abc8: f90007e0     	str	x0, [sp, #0x8]
4008abcc: f9400fe8     	ldr	x8, [sp, #0x18]
4008abd0: d35afd08     	lsr	x8, x8, #26
4008abd4: f1004508     	subs	x8, x8, #0x11
4008abd8: 54000423     	b.lo	0x4008ac5c <sys_write+0xcc>
4008abdc: 14000001     	b	0x4008abe0 <sys_write+0x50>
4008abe0: f9400fe8     	ldr	x8, [sp, #0x18]
4008abe4: b98017e9     	ldrsw	x9, [sp, #0x14]
4008abe8: 8b090108     	add	x8, x8, x9
4008abec: 52a88409     	mov	w9, #0x44200000         // =1142947840
4008abf0: eb090108     	subs	x8, x8, x9
4008abf4: 54000348     	b.hi	0x4008ac5c <sys_write+0xcc>
4008abf8: 14000001     	b	0x4008abfc <sys_write+0x6c>
4008abfc: f94007e0     	ldr	x0, [sp, #0x8]
4008ac00: b85f43a1     	ldur	w1, [x29, #-0xc]
4008ac04: f9400fe2     	ldr	x2, [sp, #0x18]
4008ac08: b94017e3     	ldr	w3, [sp, #0x14]
4008ac0c: f85f83a4     	ldur	x4, [x29, #-0x8]
4008ac10: 97ffe4ac     	bl	0x40083ec0 <file_write>
4008ac14: b90007e0     	str	w0, [sp, #0x4]
4008ac18: b94007e8     	ldr	w8, [sp, #0x4]
4008ac1c: 31000908     	adds	w8, w8, #0x2
4008ac20: 54000141     	b.ne	0x4008ac48 <sys_write+0xb8>
4008ac24: 14000001     	b	0x4008ac28 <sys_write+0x98>
4008ac28: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ac2c: f9407d28     	ldr	x8, [x9, #0xf8]
4008ac30: f1001108     	subs	x8, x8, #0x4
4008ac34: f9007d28     	str	x8, [x9, #0xf8]
4008ac38: f85f83a0     	ldur	x0, [x29, #-0x8]
4008ac3c: 2a1f03e1     	mov	w1, wzr
4008ac40: 97fff6a0     	bl	0x400886c0 <schedule>
4008ac44: 14000005     	b	0x4008ac58 <sys_write+0xc8>
4008ac48: b98007e8     	ldrsw	x8, [sp, #0x4]
4008ac4c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ac50: f9000128     	str	x8, [x9]
4008ac54: 14000001     	b	0x4008ac58 <sys_write+0xc8>
4008ac58: 14000005     	b	0x4008ac6c <sys_write+0xdc>
4008ac5c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ac60: 92800008     	mov	x8, #-0x1               // =-1
4008ac64: f9000128     	str	x8, [x9]
4008ac68: 14000001     	b	0x4008ac6c <sys_write+0xdc>
4008ac6c: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008ac70: 910103ff     	add	sp, sp, #0x40
4008ac74: d65f03c0     	ret
4008ac78: d503201f     	nop
4008ac7c: d503201f     	nop

000000004008ac80 <sys_spawn>:
4008ac80: d10183ff     	sub	sp, sp, #0x60
4008ac84: a9057bfd     	stp	x29, x30, [sp, #0x50]
4008ac88: 910143fd     	add	x29, sp, #0x50
4008ac8c: f81f83a0     	stur	x0, [x29, #-0x8]
4008ac90: f85f83a8     	ldur	x8, [x29, #-0x8]
4008ac94: f9400108     	ldr	x8, [x8]
4008ac98: f81f03a8     	stur	x8, [x29, #-0x10]
4008ac9c: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aca0: f9400508     	ldr	x8, [x8, #0x8]
4008aca4: b81ec3a8     	stur	w8, [x29, #-0x14]
4008aca8: f85f83a8     	ldur	x8, [x29, #-0x8]
4008acac: f9400908     	ldr	x8, [x8, #0x10]
4008acb0: b81e83a8     	stur	w8, [x29, #-0x18]
4008acb4: 97fff4eb     	bl	0x40088060 <current_process>
4008acb8: f81e03a0     	stur	x0, [x29, #-0x20]
4008acbc: f85e03a8     	ldur	x8, [x29, #-0x20]
4008acc0: b50000c8     	cbnz	x8, 0x4008acd8 <sys_spawn+0x58>
4008acc4: 14000001     	b	0x4008acc8 <sys_spawn+0x48>
4008acc8: f85f83a9     	ldur	x9, [x29, #-0x8]
4008accc: 92800008     	mov	x8, #-0x1               // =-1
4008acd0: f9000128     	str	x8, [x9]
4008acd4: 14000056     	b	0x4008ae2c <sys_spawn+0x1ac>
4008acd8: f85f03a8     	ldur	x8, [x29, #-0x10]
4008acdc: d35afd08     	lsr	x8, x8, #26
4008ace0: f1004508     	subs	x8, x8, #0x11
4008ace4: 540009c3     	b.lo	0x4008ae1c <sys_spawn+0x19c>
4008ace8: 14000001     	b	0x4008acec <sys_spawn+0x6c>
4008acec: f85f03a8     	ldur	x8, [x29, #-0x10]
4008acf0: d355fd08     	lsr	x8, x8, #21
4008acf4: f1088108     	subs	x8, x8, #0x220
4008acf8: 54000928     	b.hi	0x4008ae1c <sys_spawn+0x19c>
4008acfc: 14000001     	b	0x4008ad00 <sys_spawn+0x80>
4008ad00: f85e03a8     	ldur	x8, [x29, #-0x20]
4008ad04: b9800108     	ldrsw	x8, [x8]
4008ad08: 52800589     	mov	w9, #0x2c               // =44
4008ad0c: 2a0903e0     	mov	w0, w9
4008ad10: 2a0003e9     	mov	w9, w0
4008ad14: f0000cca     	adrp	x10, 0x40225000 <proc_table+0x7418>
4008ad18: 9112214a     	add	x10, x10, #0x488
4008ad1c: 9b292908     	smaddl	x8, w8, w9, x10
4008ad20: f90017e8     	str	x8, [sp, #0x28]
4008ad24: 2a1f03e8     	mov	w8, wzr
4008ad28: b90027e8     	str	w8, [sp, #0x24]
4008ad2c: 14000001     	b	0x4008ad30 <sys_spawn+0xb0>
4008ad30: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ad34: b98027e9     	ldrsw	x9, [sp, #0x24]
4008ad38: 38696908     	ldrb	w8, [x8, x9]
4008ad3c: 2a1f03e9     	mov	w9, wzr
4008ad40: b90017e9     	str	w9, [sp, #0x14]
4008ad44: 340000e8     	cbz	w8, 0x4008ad60 <sys_spawn+0xe0>
4008ad48: 14000001     	b	0x4008ad4c <sys_spawn+0xcc>
4008ad4c: b94027e8     	ldr	w8, [sp, #0x24]
4008ad50: 71007d08     	subs	w8, w8, #0x1f
4008ad54: 1a9fa7e8     	cset	w8, lt
4008ad58: b90017e8     	str	w8, [sp, #0x14]
4008ad5c: 14000001     	b	0x4008ad60 <sys_spawn+0xe0>
4008ad60: b94017e8     	ldr	w8, [sp, #0x14]
4008ad64: 36000168     	tbz	w8, #0x0, 0x4008ad90 <sys_spawn+0x110>
4008ad68: 14000001     	b	0x4008ad6c <sys_spawn+0xec>
4008ad6c: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ad70: b98027ea     	ldrsw	x10, [sp, #0x24]
4008ad74: 386a6908     	ldrb	w8, [x8, x10]
4008ad78: f94017e9     	ldr	x9, [sp, #0x28]
4008ad7c: 382a6928     	strb	w8, [x9, x10]
4008ad80: b94027e8     	ldr	w8, [sp, #0x24]
4008ad84: 11000508     	add	w8, w8, #0x1
4008ad88: b90027e8     	str	w8, [sp, #0x24]
4008ad8c: 17ffffe9     	b	0x4008ad30 <sys_spawn+0xb0>
4008ad90: f94017e9     	ldr	x9, [sp, #0x28]
4008ad94: b98027ea     	ldrsw	x10, [sp, #0x24]
4008ad98: 2a1f03e8     	mov	w8, wzr
4008ad9c: b90013e8     	str	w8, [sp, #0x10]
4008ada0: 382a6928     	strb	w8, [x9, x10]
4008ada4: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008ada8: f94017e9     	ldr	x9, [sp, #0x28]
4008adac: b9002128     	str	w8, [x9, #0x20]
4008adb0: b85e83a8     	ldur	w8, [x29, #-0x18]
4008adb4: f94017e9     	ldr	x9, [sp, #0x28]
4008adb8: b9002528     	str	w8, [x9, #0x24]
4008adbc: f85e03a8     	ldur	x8, [x29, #-0x20]
4008adc0: b9400108     	ldr	w8, [x8]
4008adc4: f94017e9     	ldr	x9, [sp, #0x28]
4008adc8: b9002928     	str	w8, [x9, #0x28]
4008adcc: f94017e1     	ldr	x1, [sp, #0x28]
4008add0: b0000000     	adrp	x0, 0x4008b000 <sys_get_events+0x50>
4008add4: 910b4000     	add	x0, x0, #0x2d0
4008add8: 97fff60a     	bl	0x40088600 <process_create_kernel>
4008addc: f0000c80     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008ade0: 912f8000     	add	x0, x0, #0xbe0
4008ade4: f90007e0     	str	x0, [sp, #0x8]
4008ade8: 97ffe606     	bl	0x40084600 <spinlock_acquire_irqsave>
4008adec: aa0003e8     	mov	x8, x0
4008adf0: f94007e0     	ldr	x0, [sp, #0x8]
4008adf4: f9000fe8     	str	x8, [sp, #0x18]
4008adf8: f85e03a9     	ldur	x9, [x29, #-0x20]
4008adfc: 528000c8     	mov	w8, #0x6                // =6
4008ae00: b9000528     	str	w8, [x9, #0x4]
4008ae04: f9400fe1     	ldr	x1, [sp, #0x18]
4008ae08: 97ffe60e     	bl	0x40084640 <spinlock_release_irqrestore>
4008ae0c: b94013e1     	ldr	w1, [sp, #0x10]
4008ae10: f85f83a0     	ldur	x0, [x29, #-0x8]
4008ae14: 97fff62b     	bl	0x400886c0 <schedule>
4008ae18: 14000005     	b	0x4008ae2c <sys_spawn+0x1ac>
4008ae1c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008ae20: 92800008     	mov	x8, #-0x1               // =-1
4008ae24: f9000128     	str	x8, [x9]
4008ae28: 14000001     	b	0x4008ae2c <sys_spawn+0x1ac>
4008ae2c: a9457bfd     	ldp	x29, x30, [sp, #0x50]
4008ae30: 910183ff     	add	sp, sp, #0x60
4008ae34: d65f03c0     	ret
4008ae38: d503201f     	nop
4008ae3c: d503201f     	nop

000000004008ae40 <sys_map_fb>:
4008ae40: d10083ff     	sub	sp, sp, #0x20
4008ae44: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008ae48: 910043fd     	add	x29, sp, #0x10
4008ae4c: f90007e0     	str	x0, [sp, #0x8]
4008ae50: 94000508     	bl	0x4008c270 <virtio_gpu_get_framebuffer>
4008ae54: f90003e0     	str	x0, [sp]
4008ae58: f94003e0     	ldr	x0, [sp]
4008ae5c: 97ffe89d     	bl	0x400850d0 <mmu_map_user_framebuffer>
4008ae60: f94007e9     	ldr	x9, [sp, #0x8]
4008ae64: 52ac0008     	mov	w8, #0x60000000         // =1610612736
4008ae68: f9000128     	str	x8, [x9]
4008ae6c: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008ae70: 910083ff     	add	sp, sp, #0x20
4008ae74: d65f03c0     	ret
4008ae78: d503201f     	nop
4008ae7c: d503201f     	nop

000000004008ae80 <sys_flush_fb>:
4008ae80: d10083ff     	sub	sp, sp, #0x20
4008ae84: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008ae88: 910043fd     	add	x29, sp, #0x10
4008ae8c: f90007e0     	str	x0, [sp, #0x8]
4008ae90: 940004bc     	bl	0x4008c180 <virtio_gpu_flush>
4008ae94: f94007e9     	ldr	x9, [sp, #0x8]
4008ae98: aa1f03e8     	mov	x8, xzr
4008ae9c: f9000128     	str	x8, [x9]
4008aea0: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008aea4: 910083ff     	add	sp, sp, #0x20
4008aea8: d65f03c0     	ret
4008aeac: d503201f     	nop

000000004008aeb0 <sys_get_cpuid>:
4008aeb0: d10083ff     	sub	sp, sp, #0x20
4008aeb4: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008aeb8: 910043fd     	add	x29, sp, #0x10
4008aebc: f90007e0     	str	x0, [sp, #0x8]
4008aec0: 97fffc2c     	bl	0x40089f70 <get_cpuid>
4008aec4: 2a0003e8     	mov	w8, w0
4008aec8: f94007e9     	ldr	x9, [sp, #0x8]
4008aecc: f9000128     	str	x8, [x9]
4008aed0: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008aed4: 910083ff     	add	sp, sp, #0x20
4008aed8: d65f03c0     	ret
4008aedc: d503201f     	nop

000000004008aee0 <sys_pipe>:
4008aee0: d10103ff     	sub	sp, sp, #0x40
4008aee4: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008aee8: 9100c3fd     	add	x29, sp, #0x30
4008aeec: f81f83a0     	stur	x0, [x29, #-0x8]
4008aef0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008aef4: f9400108     	ldr	x8, [x8]
4008aef8: f81f03a8     	stur	x8, [x29, #-0x10]
4008aefc: 97fff459     	bl	0x40088060 <current_process>
4008af00: f9000fe0     	str	x0, [sp, #0x18]
4008af04: f85f03a8     	ldur	x8, [x29, #-0x10]
4008af08: f9000be8     	str	x8, [sp, #0x10]
4008af0c: f9400be8     	ldr	x8, [sp, #0x10]
4008af10: d35afd08     	lsr	x8, x8, #26
4008af14: f1004508     	subs	x8, x8, #0x11
4008af18: 540003c3     	b.lo	0x4008af90 <sys_pipe+0xb0>
4008af1c: 14000001     	b	0x4008af20 <sys_pipe+0x40>
4008af20: f9400be8     	ldr	x8, [sp, #0x10]
4008af24: 91002108     	add	x8, x8, #0x8
4008af28: 52a88409     	mov	w9, #0x44200000         // =1142947840
4008af2c: eb090108     	subs	x8, x8, x9
4008af30: 54000308     	b.hi	0x4008af90 <sys_pipe+0xb0>
4008af34: 14000001     	b	0x4008af38 <sys_pipe+0x58>
4008af38: f9400fe0     	ldr	x0, [sp, #0x18]
4008af3c: 910023e1     	add	x1, sp, #0x8
4008af40: 97ffe448     	bl	0x40084060 <file_pipe>
4008af44: b90007e0     	str	w0, [sp, #0x4]
4008af48: b94007e8     	ldr	w8, [sp, #0x4]
4008af4c: 35000188     	cbnz	w8, 0x4008af7c <sys_pipe+0x9c>
4008af50: 14000001     	b	0x4008af54 <sys_pipe+0x74>
4008af54: b9400be8     	ldr	w8, [sp, #0x8]
4008af58: f85f03a9     	ldur	x9, [x29, #-0x10]
4008af5c: b9000128     	str	w8, [x9]
4008af60: b9400fe8     	ldr	w8, [sp, #0xc]
4008af64: f85f03a9     	ldur	x9, [x29, #-0x10]
4008af68: b9000528     	str	w8, [x9, #0x4]
4008af6c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008af70: aa1f03e8     	mov	x8, xzr
4008af74: f9000128     	str	x8, [x9]
4008af78: 14000005     	b	0x4008af8c <sys_pipe+0xac>
4008af7c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008af80: 92800008     	mov	x8, #-0x1               // =-1
4008af84: f9000128     	str	x8, [x9]
4008af88: 14000001     	b	0x4008af8c <sys_pipe+0xac>
4008af8c: 14000005     	b	0x4008afa0 <sys_pipe+0xc0>
4008af90: f85f83a9     	ldur	x9, [x29, #-0x8]
4008af94: 92800008     	mov	x8, #-0x1               // =-1
4008af98: f9000128     	str	x8, [x9]
4008af9c: 14000001     	b	0x4008afa0 <sys_pipe+0xc0>
4008afa0: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008afa4: 910103ff     	add	sp, sp, #0x40
4008afa8: d65f03c0     	ret
4008afac: d503201f     	nop

000000004008afb0 <sys_get_events>:
4008afb0: d100c3ff     	sub	sp, sp, #0x30
4008afb4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008afb8: 910083fd     	add	x29, sp, #0x20
4008afbc: f81f83a0     	stur	x0, [x29, #-0x8]
4008afc0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008afc4: f9400108     	ldr	x8, [x8]
4008afc8: f9000be8     	str	x8, [sp, #0x10]
4008afcc: f85f83a8     	ldur	x8, [x29, #-0x8]
4008afd0: f9400508     	ldr	x8, [x8, #0x8]
4008afd4: b9000fe8     	str	w8, [sp, #0xc]
4008afd8: f9400be8     	ldr	x8, [sp, #0x10]
4008afdc: d35afd08     	lsr	x8, x8, #26
4008afe0: f1004508     	subs	x8, x8, #0x11
4008afe4: 54000243     	b.lo	0x4008b02c <sys_get_events+0x7c>
4008afe8: 14000001     	b	0x4008afec <sys_get_events+0x3c>
4008afec: f9400be8     	ldr	x8, [sp, #0x10]
4008aff0: b9400fe9     	ldr	w9, [sp, #0xc]
4008aff4: 531d7129     	lsl	w9, w9, #3
4008aff8: 8b29c108     	add	x8, x8, w9, sxtw
4008affc: 52a88409     	mov	w9, #0x44200000         // =1142947840
4008b000: eb090108     	subs	x8, x8, x9
4008b004: 54000148     	b.hi	0x4008b02c <sys_get_events+0x7c>
4008b008: 14000001     	b	0x4008b00c <sys_get_events+0x5c>
4008b00c: f9400be0     	ldr	x0, [sp, #0x10]
4008b010: b9400fe1     	ldr	w1, [sp, #0xc]
4008b014: 94000547     	bl	0x4008c530 <virtio_input_get_events>
4008b018: 2a0003e8     	mov	w8, w0
4008b01c: 93407d08     	sxtw	x8, w8
4008b020: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b024: f9000128     	str	x8, [x9]
4008b028: 14000005     	b	0x4008b03c <sys_get_events+0x8c>
4008b02c: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b030: 92800008     	mov	x8, #-0x1               // =-1
4008b034: f9000128     	str	x8, [x9]
4008b038: 14000001     	b	0x4008b03c <sys_get_events+0x8c>
4008b03c: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008b040: 9100c3ff     	add	sp, sp, #0x30
4008b044: d65f03c0     	ret
4008b048: d503201f     	nop
4008b04c: d503201f     	nop

000000004008b050 <sys_available>:
4008b050: d100c3ff     	sub	sp, sp, #0x30
4008b054: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008b058: 910083fd     	add	x29, sp, #0x20
4008b05c: f81f83a0     	stur	x0, [x29, #-0x8]
4008b060: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b064: f9400108     	ldr	x8, [x8]
4008b068: b81f43a8     	stur	w8, [x29, #-0xc]
4008b06c: 97fff3fd     	bl	0x40088060 <current_process>
4008b070: f90007e0     	str	x0, [sp, #0x8]
4008b074: f94007e0     	ldr	x0, [sp, #0x8]
4008b078: b85f43a1     	ldur	w1, [x29, #-0xc]
4008b07c: 97ffe335     	bl	0x40083d50 <file_available>
4008b080: 2a0003e8     	mov	w8, w0
4008b084: 93407d08     	sxtw	x8, w8
4008b088: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b08c: f9000128     	str	x8, [x9]
4008b090: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008b094: 9100c3ff     	add	sp, sp, #0x30
4008b098: d65f03c0     	ret
4008b09c: d503201f     	nop

000000004008b0a0 <sys_read_dir>:
4008b0a0: d100c3ff     	sub	sp, sp, #0x30
4008b0a4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008b0a8: 910083fd     	add	x29, sp, #0x20
4008b0ac: f81f83a0     	stur	x0, [x29, #-0x8]
4008b0b0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b0b4: f9400108     	ldr	x8, [x8]
4008b0b8: b81f43a8     	stur	w8, [x29, #-0xc]
4008b0bc: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b0c0: f9400508     	ldr	x8, [x8, #0x8]
4008b0c4: f90007e8     	str	x8, [sp, #0x8]
4008b0c8: f94007e8     	ldr	x8, [sp, #0x8]
4008b0cc: d35afd08     	lsr	x8, x8, #26
4008b0d0: f1004508     	subs	x8, x8, #0x11
4008b0d4: 54000203     	b.lo	0x4008b114 <sys_read_dir+0x74>
4008b0d8: 14000001     	b	0x4008b0dc <sys_read_dir+0x3c>
4008b0dc: f94007e8     	ldr	x8, [sp, #0x8]
4008b0e0: 91003108     	add	x8, x8, #0xc
4008b0e4: 52a88409     	mov	w9, #0x44200000         // =1142947840
4008b0e8: eb090108     	subs	x8, x8, x9
4008b0ec: 54000148     	b.hi	0x4008b114 <sys_read_dir+0x74>
4008b0f0: 14000001     	b	0x4008b0f4 <sys_read_dir+0x54>
4008b0f4: b85f43a0     	ldur	w0, [x29, #-0xc]
4008b0f8: f94007e1     	ldr	x1, [sp, #0x8]
4008b0fc: 97ffdd29     	bl	0x400825a0 <fat16_read_dir>
4008b100: 2a0003e8     	mov	w8, w0
4008b104: 93407d08     	sxtw	x8, w8
4008b108: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b10c: f9000128     	str	x8, [x9]
4008b110: 14000005     	b	0x4008b124 <sys_read_dir+0x84>
4008b114: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b118: 92800008     	mov	x8, #-0x1               // =-1
4008b11c: f9000128     	str	x8, [x9]
4008b120: 14000001     	b	0x4008b124 <sys_read_dir+0x84>
4008b124: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008b128: 9100c3ff     	add	sp, sp, #0x30
4008b12c: d65f03c0     	ret

000000004008b130 <sys_kill>:
4008b130: d10083ff     	sub	sp, sp, #0x20
4008b134: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008b138: 910043fd     	add	x29, sp, #0x10
4008b13c: f90007e0     	str	x0, [sp, #0x8]
4008b140: f94007e8     	ldr	x8, [sp, #0x8]
4008b144: f9400108     	ldr	x8, [x8]
4008b148: b90007e8     	str	w8, [sp, #0x4]
4008b14c: b94007e0     	ldr	w0, [sp, #0x4]
4008b150: 97fff808     	bl	0x40089170 <process_kill>
4008b154: 2a0003e8     	mov	w8, w0
4008b158: 93407d08     	sxtw	x8, w8
4008b15c: f94007e9     	ldr	x9, [sp, #0x8]
4008b160: f9000128     	str	x8, [x9]
4008b164: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008b168: 910083ff     	add	sp, sp, #0x20
4008b16c: d65f03c0     	ret

000000004008b170 <sys_connect>:
4008b170: d100c3ff     	sub	sp, sp, #0x30
4008b174: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008b178: 910083fd     	add	x29, sp, #0x20
4008b17c: f81f83a0     	stur	x0, [x29, #-0x8]
4008b180: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b184: f9400108     	ldr	x8, [x8]
4008b188: b81f43a8     	stur	w8, [x29, #-0xc]
4008b18c: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b190: f9400508     	ldr	x8, [x8, #0x8]
4008b194: 781f23a8     	sturh	w8, [x29, #-0xe]
4008b198: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b19c: f9400908     	ldr	x8, [x8, #0x10]
4008b1a0: b9000fe8     	str	w8, [sp, #0xc]
4008b1a4: 97fff3af     	bl	0x40088060 <current_process>
4008b1a8: f90003e0     	str	x0, [sp]
4008b1ac: f94003e0     	ldr	x0, [sp]
4008b1b0: b85f43a1     	ldur	w1, [x29, #-0xc]
4008b1b4: b9400fe3     	ldr	w3, [sp, #0xc]
4008b1b8: 785f23a2     	ldurh	w2, [x29, #-0xe]
4008b1bc: 97ffe159     	bl	0x40083720 <file_connect>
4008b1c0: 2a0003e8     	mov	w8, w0
4008b1c4: 93407d08     	sxtw	x8, w8
4008b1c8: f85f83a9     	ldur	x9, [x29, #-0x8]
4008b1cc: f9000128     	str	x8, [x9]
4008b1d0: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008b1d4: 9100c3ff     	add	sp, sp, #0x30
4008b1d8: d65f03c0     	ret
4008b1dc: d503201f     	nop

000000004008b1e0 <irq_lower_handler_c>:
4008b1e0: d100c3ff     	sub	sp, sp, #0x30
4008b1e4: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008b1e8: 910083fd     	add	x29, sp, #0x20
4008b1ec: f81f83a0     	stur	x0, [x29, #-0x8]
4008b1f0: 97ffe4cc     	bl	0x40084520 <gic_acknowledge_interrupt>
4008b1f4: b81f43a0     	stur	w0, [x29, #-0xc]
4008b1f8: b85f43a8     	ldur	w8, [x29, #-0xc]
4008b1fc: 71007908     	subs	w8, w8, #0x1e
4008b200: 54000201     	b.ne	0x4008b240 <irq_lower_handler_c+0x60>
4008b204: 14000001     	b	0x4008b208 <irq_lower_handler_c+0x28>
4008b208: 97fff396     	bl	0x40088060 <current_process>
4008b20c: f90007e0     	str	x0, [sp, #0x8]
4008b210: f94007e8     	ldr	x8, [sp, #0x8]
4008b214: b4000128     	cbz	x8, 0x4008b238 <irq_lower_handler_c+0x58>
4008b218: 14000001     	b	0x4008b21c <irq_lower_handler_c+0x3c>
4008b21c: 97fffbe9     	bl	0x4008a1c0 <timer_reload>
4008b220: b85f43a0     	ldur	w0, [x29, #-0xc]
4008b224: 97ffe4cf     	bl	0x40084560 <gic_end_interrupt>
4008b228: f85f83a0     	ldur	x0, [x29, #-0x8]
4008b22c: 2a1f03e1     	mov	w1, wzr
4008b230: 97fff524     	bl	0x400886c0 <schedule>
4008b234: 14000024     	b	0x4008b2c4 <irq_lower_handler_c+0xe4>
4008b238: 97fffbe2     	bl	0x4008a1c0 <timer_reload>
4008b23c: 1400001f     	b	0x4008b2b8 <irq_lower_handler_c+0xd8>
4008b240: b85f43a8     	ldur	w8, [x29, #-0xc]
4008b244: d0000009     	adrp	x9, 0x4008d000 <virtio_net_send+0x90>
4008b248: b94bad29     	ldr	w9, [x9, #0xbac]
4008b24c: 6b090108     	subs	w8, w8, w9
4008b250: 54000081     	b.ne	0x4008b260 <irq_lower_handler_c+0x80>
4008b254: 14000001     	b	0x4008b258 <irq_lower_handler_c+0x78>
4008b258: 94000046     	bl	0x4008b370 <virtio_blk_handle_irq>
4008b25c: 14000016     	b	0x4008b2b4 <irq_lower_handler_c+0xd4>
4008b260: b85f43a8     	ldur	w8, [x29, #-0xc]
4008b264: d0000009     	adrp	x9, 0x4008d000 <virtio_net_send+0x90>
4008b268: b94bb129     	ldr	w9, [x9, #0xbb0]
4008b26c: 6b090108     	subs	w8, w8, w9
4008b270: 54000081     	b.ne	0x4008b280 <irq_lower_handler_c+0xa0>
4008b274: 14000001     	b	0x4008b278 <irq_lower_handler_c+0x98>
4008b278: 940007c2     	bl	0x4008d180 <virtio_net_handle_irq>
4008b27c: 1400000d     	b	0x4008b2b0 <irq_lower_handler_c+0xd0>
4008b280: b85f43a8     	ldur	w8, [x29, #-0xc]
4008b284: 7100c108     	subs	w8, w8, #0x30
4008b288: 54000123     	b.lo	0x4008b2ac <irq_lower_handler_c+0xcc>
4008b28c: 14000001     	b	0x4008b290 <irq_lower_handler_c+0xb0>
4008b290: b85f43a8     	ldur	w8, [x29, #-0xc]
4008b294: 71013d08     	subs	w8, w8, #0x4f
4008b298: 540000a8     	b.hi	0x4008b2ac <irq_lower_handler_c+0xcc>
4008b29c: 14000001     	b	0x4008b2a0 <irq_lower_handler_c+0xc0>
4008b2a0: b85f43a0     	ldur	w0, [x29, #-0xc]
4008b2a4: 940003f7     	bl	0x4008c280 <virtio_input_handle_irq>
4008b2a8: 14000001     	b	0x4008b2ac <irq_lower_handler_c+0xcc>
4008b2ac: 14000001     	b	0x4008b2b0 <irq_lower_handler_c+0xd0>
4008b2b0: 14000001     	b	0x4008b2b4 <irq_lower_handler_c+0xd4>
4008b2b4: 14000001     	b	0x4008b2b8 <irq_lower_handler_c+0xd8>
4008b2b8: b85f43a0     	ldur	w0, [x29, #-0xc]
4008b2bc: 97ffe4a9     	bl	0x40084560 <gic_end_interrupt>
4008b2c0: 14000001     	b	0x4008b2c4 <irq_lower_handler_c+0xe4>
4008b2c4: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008b2c8: 9100c3ff     	add	sp, sp, #0x30
4008b2cc: d65f03c0     	ret

000000004008b2d0 <sys_spawn_worker>:
4008b2d0: d10103ff     	sub	sp, sp, #0x40
4008b2d4: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008b2d8: 9100c3fd     	add	x29, sp, #0x30
4008b2dc: f81f83a0     	stur	x0, [x29, #-0x8]
4008b2e0: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b2e4: f81f03a8     	stur	x8, [x29, #-0x10]
4008b2e8: f85f03a0     	ldur	x0, [x29, #-0x10]
4008b2ec: b9402001     	ldr	w1, [x0, #0x20]
4008b2f0: b9402402     	ldr	w2, [x0, #0x24]
4008b2f4: 97fffa33     	bl	0x40089bc0 <load_and_run_program_in_scheduler>
4008b2f8: b81ec3a0     	stur	w0, [x29, #-0x14]
4008b2fc: f85f03a8     	ldur	x8, [x29, #-0x10]
4008b300: b9402900     	ldr	w0, [x8, #0x28]
4008b304: 97fff92f     	bl	0x400897c0 <process_get_pcb>
4008b308: f9000be0     	str	x0, [sp, #0x10]
4008b30c: f9400be8     	ldr	x8, [sp, #0x10]
4008b310: b4000248     	cbz	x8, 0x4008b358 <sys_spawn_worker+0x88>
4008b314: 14000001     	b	0x4008b318 <sys_spawn_worker+0x48>
4008b318: b89ec3a8     	ldursw	x8, [x29, #-0x14]
4008b31c: f9400be9     	ldr	x9, [sp, #0x10]
4008b320: f9001928     	str	x8, [x9, #0x30]
4008b324: d0000c80     	adrp	x0, 0x4021d000 <pipes+0x7c24>
4008b328: 912f8000     	add	x0, x0, #0xbe0
4008b32c: f90003e0     	str	x0, [sp]
4008b330: 97ffe4b4     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b334: aa0003e8     	mov	x8, x0
4008b338: f94003e0     	ldr	x0, [sp]
4008b33c: f90007e8     	str	x8, [sp, #0x8]
4008b340: f9400be9     	ldr	x9, [sp, #0x10]
4008b344: 52800048     	mov	w8, #0x2                // =2
4008b348: b9000528     	str	w8, [x9, #0x4]
4008b34c: f94007e1     	ldr	x1, [sp, #0x8]
4008b350: 97ffe4bc     	bl	0x40084640 <spinlock_release_irqrestore>
4008b354: 14000001     	b	0x4008b358 <sys_spawn_worker+0x88>
4008b358: 97fff742     	bl	0x40089060 <kernel_exit>
4008b35c: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008b360: 910103ff     	add	sp, sp, #0x40
4008b364: d65f03c0     	ret
		...

000000004008b370 <virtio_blk_handle_irq>:
4008b370: d10083ff     	sub	sp, sp, #0x20
4008b374: a9017bfd     	stp	x29, x30, [sp, #0x10]
4008b378: 910043fd     	add	x29, sp, #0x10
4008b37c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b380: 91000000     	add	x0, x0, #0x0
4008b384: 97ffe49f     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b388: f90007e0     	str	x0, [sp, #0x8]
4008b38c: f0000cc8     	adrp	x8, 0x40226000 <blk_lock>
4008b390: f9400508     	ldr	x8, [x8, #0x8]
4008b394: b50000e8     	cbnz	x8, 0x4008b3b0 <virtio_blk_handle_irq+0x40>
4008b398: 14000001     	b	0x4008b39c <virtio_blk_handle_irq+0x2c>
4008b39c: f94007e1     	ldr	x1, [sp, #0x8]
4008b3a0: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b3a4: 91000000     	add	x0, x0, #0x0
4008b3a8: 97ffe4a6     	bl	0x40084640 <spinlock_release_irqrestore>
4008b3ac: 14000010     	b	0x4008b3ec <virtio_blk_handle_irq+0x7c>
4008b3b0: 52800c00     	mov	w0, #0x60               // =96
4008b3b4: 94000013     	bl	0x4008b400 <reg_read32>
4008b3b8: b90007e0     	str	w0, [sp, #0x4]
4008b3bc: b94007e8     	ldr	w8, [sp, #0x4]
4008b3c0: 340000c8     	cbz	w8, 0x4008b3d8 <virtio_blk_handle_irq+0x68>
4008b3c4: 14000001     	b	0x4008b3c8 <virtio_blk_handle_irq+0x58>
4008b3c8: b94007e1     	ldr	w1, [sp, #0x4]
4008b3cc: 52800c80     	mov	w0, #0x64               // =100
4008b3d0: 94000014     	bl	0x4008b420 <reg_write32>
4008b3d4: 14000001     	b	0x4008b3d8 <virtio_blk_handle_irq+0x68>
4008b3d8: f94007e1     	ldr	x1, [sp, #0x8]
4008b3dc: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b3e0: 91000000     	add	x0, x0, #0x0
4008b3e4: 97ffe497     	bl	0x40084640 <spinlock_release_irqrestore>
4008b3e8: 14000001     	b	0x4008b3ec <virtio_blk_handle_irq+0x7c>
4008b3ec: a9417bfd     	ldp	x29, x30, [sp, #0x10]
4008b3f0: 910083ff     	add	sp, sp, #0x20
4008b3f4: d65f03c0     	ret
4008b3f8: d503201f     	nop
4008b3fc: d503201f     	nop

000000004008b400 <reg_read32>:
4008b400: d10043ff     	sub	sp, sp, #0x10
4008b404: b9000fe0     	str	w0, [sp, #0xc]
4008b408: f0000cc8     	adrp	x8, 0x40226000 <blk_lock>
4008b40c: f9400508     	ldr	x8, [x8, #0x8]
4008b410: b9400fe9     	ldr	w9, [sp, #0xc]
4008b414: b8696900     	ldr	w0, [x8, x9]
4008b418: 910043ff     	add	sp, sp, #0x10
4008b41c: d65f03c0     	ret

000000004008b420 <reg_write32>:
4008b420: d10043ff     	sub	sp, sp, #0x10
4008b424: b9000fe0     	str	w0, [sp, #0xc]
4008b428: b9000be1     	str	w1, [sp, #0x8]
4008b42c: b9400be8     	ldr	w8, [sp, #0x8]
4008b430: f0000cc9     	adrp	x9, 0x40226000 <blk_lock>
4008b434: f9400529     	ldr	x9, [x9, #0x8]
4008b438: b9400fea     	ldr	w10, [sp, #0xc]
4008b43c: b82a6928     	str	w8, [x9, x10]
4008b440: 910043ff     	add	sp, sp, #0x10
4008b444: d65f03c0     	ret
4008b448: d503201f     	nop
4008b44c: d503201f     	nop

000000004008b450 <virtio_blk_init>:
4008b450: d10143ff     	sub	sp, sp, #0x50
4008b454: a9047bfd     	stp	x29, x30, [sp, #0x40]
4008b458: 910103fd     	add	x29, sp, #0x40
4008b45c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b460: 91000000     	add	x0, x0, #0x0
4008b464: 97ffe44b     	bl	0x40084590 <spinlock_init>
4008b468: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b46c: 91004000     	add	x0, x0, #0x10
4008b470: 97ffe448     	bl	0x40084590 <spinlock_init>
4008b474: 2a1f03e8     	mov	w8, wzr
4008b478: b81f83a8     	stur	w8, [x29, #-0x8]
4008b47c: 14000001     	b	0x4008b480 <virtio_blk_init+0x30>
4008b480: b85f83a8     	ldur	w8, [x29, #-0x8]
4008b484: 71007d08     	subs	w8, w8, #0x1f
4008b488: 5400048c     	b.gt	0x4008b518 <virtio_blk_init+0xc8>
4008b48c: 14000001     	b	0x4008b490 <virtio_blk_init+0x40>
4008b490: b85f83a8     	ldur	w8, [x29, #-0x8]
4008b494: 53175909     	lsl	w9, w8, #9
4008b498: 52a14008     	mov	w8, #0xa000000          // =167772160
4008b49c: 8b29c108     	add	x8, x8, w9, sxtw
4008b4a0: f81f03a8     	stur	x8, [x29, #-0x10]
4008b4a4: f85f03a8     	ldur	x8, [x29, #-0x10]
4008b4a8: b9400108     	ldr	w8, [x8]
4008b4ac: b81ec3a8     	stur	w8, [x29, #-0x14]
4008b4b0: f85f03a8     	ldur	x8, [x29, #-0x10]
4008b4b4: b9400908     	ldr	w8, [x8, #0x8]
4008b4b8: b81e83a8     	stur	w8, [x29, #-0x18]
4008b4bc: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008b4c0: 528d2ec9     	mov	w9, #0x6976             // =26998
4008b4c4: 72ae8e49     	movk	w9, #0x7472, lsl #16
4008b4c8: 6b090108     	subs	w8, w8, w9
4008b4cc: 540001c1     	b.ne	0x4008b504 <virtio_blk_init+0xb4>
4008b4d0: 14000001     	b	0x4008b4d4 <virtio_blk_init+0x84>
4008b4d4: b85e83a8     	ldur	w8, [x29, #-0x18]
4008b4d8: 71000908     	subs	w8, w8, #0x2
4008b4dc: 54000141     	b.ne	0x4008b504 <virtio_blk_init+0xb4>
4008b4e0: 14000001     	b	0x4008b4e4 <virtio_blk_init+0x94>
4008b4e4: f85f03a8     	ldur	x8, [x29, #-0x10]
4008b4e8: f0000cc9     	adrp	x9, 0x40226000 <blk_lock>
4008b4ec: f9000528     	str	x8, [x9, #0x8]
4008b4f0: b85f83a8     	ldur	w8, [x29, #-0x8]
4008b4f4: 1100c108     	add	w8, w8, #0x30
4008b4f8: d0000009     	adrp	x9, 0x4008d000 <virtio_net_send+0x90>
4008b4fc: b90bad28     	str	w8, [x9, #0xbac]
4008b500: 14000006     	b	0x4008b518 <virtio_blk_init+0xc8>
4008b504: 14000001     	b	0x4008b508 <virtio_blk_init+0xb8>
4008b508: b85f83a8     	ldur	w8, [x29, #-0x8]
4008b50c: 11000508     	add	w8, w8, #0x1
4008b510: b81f83a8     	stur	w8, [x29, #-0x8]
4008b514: 17ffffdb     	b	0x4008b480 <virtio_blk_init+0x30>
4008b518: f0000cc8     	adrp	x8, 0x40226000 <blk_lock>
4008b51c: f9400508     	ldr	x8, [x8, #0x8]
4008b520: b50000a8     	cbnz	x8, 0x4008b534 <virtio_blk_init+0xe4>
4008b524: 14000001     	b	0x4008b528 <virtio_blk_init+0xd8>
4008b528: 12800008     	mov	w8, #-0x1               // =-1
4008b52c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b530: 14000057     	b	0x4008b68c <virtio_blk_init+0x23c>
4008b534: 2a1f03e8     	mov	w8, wzr
4008b538: b90017e8     	str	w8, [sp, #0x14]
4008b53c: b81e43a8     	stur	w8, [x29, #-0x1c]
4008b540: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008b544: 52800e00     	mov	w0, #0x70               // =112
4008b548: b9001be0     	str	w0, [sp, #0x18]
4008b54c: 97ffffb5     	bl	0x4008b420 <reg_write32>
4008b550: b9401be0     	ldr	w0, [sp, #0x18]
4008b554: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008b558: 32000108     	orr	w8, w8, #0x1
4008b55c: b81e43a8     	stur	w8, [x29, #-0x1c]
4008b560: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008b564: 97ffffaf     	bl	0x4008b420 <reg_write32>
4008b568: b9401be0     	ldr	w0, [sp, #0x18]
4008b56c: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008b570: 321f0108     	orr	w8, w8, #0x2
4008b574: b81e43a8     	stur	w8, [x29, #-0x1c]
4008b578: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008b57c: 97ffffa9     	bl	0x4008b420 <reg_write32>
4008b580: 52800480     	mov	w0, #0x24               // =36
4008b584: b9000fe0     	str	w0, [sp, #0xc]
4008b588: 52800021     	mov	w1, #0x1                // =1
4008b58c: b9000be1     	str	w1, [sp, #0x8]
4008b590: 97ffffa4     	bl	0x4008b420 <reg_write32>
4008b594: b9400be1     	ldr	w1, [sp, #0x8]
4008b598: 52800400     	mov	w0, #0x20               // =32
4008b59c: b90013e0     	str	w0, [sp, #0x10]
4008b5a0: 97ffffa0     	bl	0x4008b420 <reg_write32>
4008b5a4: b9400fe0     	ldr	w0, [sp, #0xc]
4008b5a8: b94017e1     	ldr	w1, [sp, #0x14]
4008b5ac: 97ffff9d     	bl	0x4008b420 <reg_write32>
4008b5b0: b94013e0     	ldr	w0, [sp, #0x10]
4008b5b4: b94017e1     	ldr	w1, [sp, #0x14]
4008b5b8: 97ffff9a     	bl	0x4008b420 <reg_write32>
4008b5bc: b9401be0     	ldr	w0, [sp, #0x18]
4008b5c0: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008b5c4: 321d0108     	orr	w8, w8, #0x8
4008b5c8: b81e43a8     	stur	w8, [x29, #-0x1c]
4008b5cc: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008b5d0: 97ffff94     	bl	0x4008b420 <reg_write32>
4008b5d4: b9401be0     	ldr	w0, [sp, #0x18]
4008b5d8: 97ffff8a     	bl	0x4008b400 <reg_read32>
4008b5dc: 371800a0     	tbnz	w0, #0x3, 0x4008b5f0 <virtio_blk_init+0x1a0>
4008b5e0: 14000001     	b	0x4008b5e4 <virtio_blk_init+0x194>
4008b5e4: 12800008     	mov	w8, #-0x1               // =-1
4008b5e8: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b5ec: 14000028     	b	0x4008b68c <virtio_blk_init+0x23c>
4008b5f0: 52800500     	mov	w0, #0x28               // =40
4008b5f4: 52820001     	mov	w1, #0x1000             // =4096
4008b5f8: 97ffff8a     	bl	0x4008b420 <reg_write32>
4008b5fc: 52800600     	mov	w0, #0x30               // =48
4008b600: 2a1f03e1     	mov	w1, wzr
4008b604: 97ffff87     	bl	0x4008b420 <reg_write32>
4008b608: 52800680     	mov	w0, #0x34               // =52
4008b60c: 97ffff7d     	bl	0x4008b400 <reg_read32>
4008b610: b90023e0     	str	w0, [sp, #0x20]
4008b614: b94023e8     	ldr	w8, [sp, #0x20]
4008b618: 350000a8     	cbnz	w8, 0x4008b62c <virtio_blk_init+0x1dc>
4008b61c: 14000001     	b	0x4008b620 <virtio_blk_init+0x1d0>
4008b620: 12800008     	mov	w8, #-0x1               // =-1
4008b624: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b628: 14000019     	b	0x4008b68c <virtio_blk_init+0x23c>
4008b62c: 52800700     	mov	w0, #0x38               // =56
4008b630: 52800101     	mov	w1, #0x8                // =8
4008b634: 97ffff7b     	bl	0x4008b420 <reg_write32>
4008b638: 52800780     	mov	w0, #0x3c               // =60
4008b63c: 52820001     	mov	w1, #0x1000             // =4096
4008b640: 97ffff78     	bl	0x4008b420 <reg_write32>
4008b644: 90000ce8     	adrp	x8, 0x40227000 <vq>
4008b648: 91000108     	add	x8, x8, #0x0
4008b64c: d34cfd08     	lsr	x8, x8, #12
4008b650: b9001fe8     	str	w8, [sp, #0x1c]
4008b654: b9401fe1     	ldr	w1, [sp, #0x1c]
4008b658: 52800800     	mov	w0, #0x40               // =64
4008b65c: 97ffff71     	bl	0x4008b420 <reg_write32>
4008b660: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008b664: 321e0108     	orr	w8, w8, #0x4
4008b668: b81e43a8     	stur	w8, [x29, #-0x1c]
4008b66c: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008b670: 52800e00     	mov	w0, #0x70               // =112
4008b674: 97ffff6b     	bl	0x4008b420 <reg_write32>
4008b678: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b67c: 2a1f03e8     	mov	w8, wzr
4008b680: 79000128     	strh	w8, [x9]
4008b684: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b688: 14000001     	b	0x4008b68c <virtio_blk_init+0x23c>
4008b68c: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008b690: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008b694: 910143ff     	add	sp, sp, #0x50
4008b698: d65f03c0     	ret
4008b69c: d503201f     	nop

000000004008b6a0 <virtio_blk_read_sector>:
4008b6a0: d10103ff     	sub	sp, sp, #0x40
4008b6a4: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008b6a8: 9100c3fd     	add	x29, sp, #0x30
4008b6ac: f81f83a0     	stur	x0, [x29, #-0x8]
4008b6b0: f81f03a1     	stur	x1, [x29, #-0x10]
4008b6b4: b81ec3a2     	stur	w2, [x29, #-0x14]
4008b6b8: 14000001     	b	0x4008b6bc <virtio_blk_read_sector+0x1c>
4008b6bc: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b6c0: 91004000     	add	x0, x0, #0x10
4008b6c4: 97ffe3cf     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b6c8: f9000be0     	str	x0, [sp, #0x10]
4008b6cc: d0000ce8     	adrp	x8, 0x40229000 <ack_used_idx>
4008b6d0: b9400508     	ldr	w8, [x8, #0x4]
4008b6d4: 35000148     	cbnz	w8, 0x4008b6fc <virtio_blk_read_sector+0x5c>
4008b6d8: 14000001     	b	0x4008b6dc <virtio_blk_read_sector+0x3c>
4008b6dc: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b6e0: 52800028     	mov	w8, #0x1                // =1
4008b6e4: b9000528     	str	w8, [x9, #0x4]
4008b6e8: f9400be1     	ldr	x1, [sp, #0x10]
4008b6ec: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b6f0: 91004000     	add	x0, x0, #0x10
4008b6f4: 97ffe3d3     	bl	0x40084640 <spinlock_release_irqrestore>
4008b6f8: 14000007     	b	0x4008b714 <virtio_blk_read_sector+0x74>
4008b6fc: f9400be1     	ldr	x1, [sp, #0x10]
4008b700: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b704: 91004000     	add	x0, x0, #0x10
4008b708: 97ffe3ce     	bl	0x40084640 <spinlock_release_irqrestore>
4008b70c: d503207f     	wfi
4008b710: 17ffffeb     	b	0x4008b6bc <virtio_blk_read_sector+0x1c>
4008b714: 2a1f03e8     	mov	w8, wzr
4008b718: b9000fe8     	str	w8, [sp, #0xc]
4008b71c: b9000be8     	str	w8, [sp, #0x8]
4008b720: 14000001     	b	0x4008b724 <virtio_blk_read_sector+0x84>
4008b724: b9400be8     	ldr	w8, [sp, #0x8]
4008b728: b85ec3a9     	ldur	w9, [x29, #-0x14]
4008b72c: 6b090108     	subs	w8, w8, w9
4008b730: 540002e2     	b.hs	0x4008b78c <virtio_blk_read_sector+0xec>
4008b734: 14000001     	b	0x4008b738 <virtio_blk_read_sector+0x98>
4008b738: f85f83a8     	ldur	x8, [x29, #-0x8]
4008b73c: b9400be9     	ldr	w9, [sp, #0x8]
4008b740: 2a0903ea     	mov	w10, w9
4008b744: 2a0a03e9     	mov	w9, w10
4008b748: 8b0a0100     	add	x0, x8, x10
4008b74c: f85f03a8     	ldur	x8, [x29, #-0x10]
4008b750: 53175929     	lsl	w9, w9, #9
4008b754: 2a0903e9     	mov	w9, w9
4008b758: 8b090101     	add	x1, x8, x9
4008b75c: 2a1f03e2     	mov	w2, wzr
4008b760: 9400001c     	bl	0x4008b7d0 <virtio_blk_do_op>
4008b764: 340000a0     	cbz	w0, 0x4008b778 <virtio_blk_read_sector+0xd8>
4008b768: 14000001     	b	0x4008b76c <virtio_blk_read_sector+0xcc>
4008b76c: 12800008     	mov	w8, #-0x1               // =-1
4008b770: b9000fe8     	str	w8, [sp, #0xc]
4008b774: 14000006     	b	0x4008b78c <virtio_blk_read_sector+0xec>
4008b778: 14000001     	b	0x4008b77c <virtio_blk_read_sector+0xdc>
4008b77c: b9400be8     	ldr	w8, [sp, #0x8]
4008b780: 11000508     	add	w8, w8, #0x1
4008b784: b9000be8     	str	w8, [sp, #0x8]
4008b788: 17ffffe7     	b	0x4008b724 <virtio_blk_read_sector+0x84>
4008b78c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b790: 91004000     	add	x0, x0, #0x10
4008b794: f90003e0     	str	x0, [sp]
4008b798: 97ffe39a     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b79c: aa0003e8     	mov	x8, x0
4008b7a0: f94003e0     	ldr	x0, [sp]
4008b7a4: f9000be8     	str	x8, [sp, #0x10]
4008b7a8: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b7ac: 2a1f03e8     	mov	w8, wzr
4008b7b0: b9000528     	str	w8, [x9, #0x4]
4008b7b4: f9400be1     	ldr	x1, [sp, #0x10]
4008b7b8: 97ffe3a2     	bl	0x40084640 <spinlock_release_irqrestore>
4008b7bc: 97fff7d1     	bl	0x40089700 <process_wake_all>
4008b7c0: b9400fe0     	ldr	w0, [sp, #0xc]
4008b7c4: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008b7c8: 910103ff     	add	sp, sp, #0x40
4008b7cc: d65f03c0     	ret

000000004008b7d0 <virtio_blk_do_op>:
4008b7d0: d10143ff     	sub	sp, sp, #0x50
4008b7d4: a9047bfd     	stp	x29, x30, [sp, #0x40]
4008b7d8: 910103fd     	add	x29, sp, #0x40
4008b7dc: f81f03a0     	stur	x0, [x29, #-0x10]
4008b7e0: f81e83a1     	stur	x1, [x29, #-0x18]
4008b7e4: b81e43a2     	stur	w2, [x29, #-0x1c]
4008b7e8: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b7ec: 91000000     	add	x0, x0, #0x0
4008b7f0: 97ffe384     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b7f4: f9000fe0     	str	x0, [sp, #0x18]
4008b7f8: f0000cc8     	adrp	x8, 0x40226000 <blk_lock>
4008b7fc: f9400508     	ldr	x8, [x8, #0x8]
4008b800: b5000128     	cbnz	x8, 0x4008b824 <virtio_blk_do_op+0x54>
4008b804: 14000001     	b	0x4008b808 <virtio_blk_do_op+0x38>
4008b808: f9400fe1     	ldr	x1, [sp, #0x18]
4008b80c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b810: 91000000     	add	x0, x0, #0x0
4008b814: 97ffe38b     	bl	0x40084640 <spinlock_release_irqrestore>
4008b818: 12800008     	mov	w8, #-0x1               // =-1
4008b81c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b820: 14000055     	b	0x4008b974 <virtio_blk_do_op+0x1a4>
4008b824: 2a1f03e1     	mov	w1, wzr
4008b828: 79002fe1     	strh	w1, [sp, #0x16]
4008b82c: b85e43a9     	ldur	w9, [x29, #-0x1c]
4008b830: d0000ce8     	adrp	x8, 0x40229000 <ack_used_idx>
4008b834: 91002108     	add	x8, x8, #0x8
4008b838: b9000109     	str	w9, [x8]
4008b83c: b9000501     	str	w1, [x8, #0x4]
4008b840: f85f03a9     	ldur	x9, [x29, #-0x10]
4008b844: f9000509     	str	x9, [x8, #0x8]
4008b848: 90000ce9     	adrp	x9, 0x40227000 <vq>
4008b84c: 91000129     	add	x9, x9, #0x0
4008b850: f90003e9     	str	x9, [sp]
4008b854: f9000128     	str	x8, [x9]
4008b858: 52800208     	mov	w8, #0x10               // =16
4008b85c: b9000928     	str	w8, [x9, #0x8]
4008b860: 5280002a     	mov	w10, #0x1               // =1
4008b864: 7900192a     	strh	w10, [x9, #0xc]
4008b868: 79001d2a     	strh	w10, [x9, #0xe]
4008b86c: f85e83a8     	ldur	x8, [x29, #-0x18]
4008b870: f9000928     	str	x8, [x9, #0x10]
4008b874: 52804008     	mov	w8, #0x200              // =512
4008b878: b9001928     	str	w8, [x9, #0x18]
4008b87c: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008b880: 71000108     	subs	w8, w8, #0x0
4008b884: 52800068     	mov	w8, #0x3                // =3
4008b888: 1a9f0508     	csinc	w8, w8, wzr, eq
4008b88c: 79003928     	strh	w8, [x9, #0x1c]
4008b890: 52800048     	mov	w8, #0x2                // =2
4008b894: 79003d28     	strh	w8, [x9, #0x1e]
4008b898: d0000ceb     	adrp	x11, 0x40229000 <ack_used_idx>
4008b89c: 9100616b     	add	x11, x11, #0x18
4008b8a0: f900112b     	str	x11, [x9, #0x20]
4008b8a4: b900292a     	str	w10, [x9, #0x28]
4008b8a8: 79005928     	strh	w8, [x9, #0x2c]
4008b8ac: 79005d21     	strh	w1, [x9, #0x2e]
4008b8b0: 79410528     	ldrh	w8, [x9, #0x82]
4008b8b4: 79002be8     	strh	w8, [sp, #0x14]
4008b8b8: 79402be8     	ldrh	w8, [sp, #0x14]
4008b8bc: 12000908     	and	w8, w8, #0x7
4008b8c0: 8b284528     	add	x8, x9, w8, uxtw #1
4008b8c4: 79010901     	strh	w1, [x8, #0x84]
4008b8c8: d5033fbf     	dmb	sy
4008b8cc: 79410528     	ldrh	w8, [x9, #0x82]
4008b8d0: 11000508     	add	w8, w8, #0x1
4008b8d4: 79010528     	strh	w8, [x9, #0x82]
4008b8d8: d5033fbf     	dmb	sy
4008b8dc: 52800a00     	mov	w0, #0x50               // =80
4008b8e0: 97fffed0     	bl	0x4008b420 <reg_write32>
4008b8e4: 14000001     	b	0x4008b8e8 <virtio_blk_do_op+0x118>
4008b8e8: b0000ce8     	adrp	x8, 0x40228000 <vq+0x1000>
4008b8ec: 79400508     	ldrh	w8, [x8, #0x2]
4008b8f0: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b8f4: 79400129     	ldrh	w9, [x9]
4008b8f8: 6b090108     	subs	w8, w8, w9
4008b8fc: 54000061     	b.ne	0x4008b908 <virtio_blk_do_op+0x138>
4008b900: 14000001     	b	0x4008b904 <virtio_blk_do_op+0x134>
4008b904: 17fffff9     	b	0x4008b8e8 <virtio_blk_do_op+0x118>
4008b908: b0000ce8     	adrp	x8, 0x40228000 <vq+0x1000>
4008b90c: 79400508     	ldrh	w8, [x8, #0x2]
4008b910: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b914: 79000128     	strh	w8, [x9]
4008b918: d5033fbf     	dmb	sy
4008b91c: 52800c00     	mov	w0, #0x60               // =96
4008b920: 97fffeb8     	bl	0x4008b400 <reg_read32>
4008b924: b90013e0     	str	w0, [sp, #0x10]
4008b928: b94013e8     	ldr	w8, [sp, #0x10]
4008b92c: 340000c8     	cbz	w8, 0x4008b944 <virtio_blk_do_op+0x174>
4008b930: 14000001     	b	0x4008b934 <virtio_blk_do_op+0x164>
4008b934: b94013e1     	ldr	w1, [sp, #0x10]
4008b938: 52800c80     	mov	w0, #0x64               // =100
4008b93c: 97fffeb9     	bl	0x4008b420 <reg_write32>
4008b940: 14000001     	b	0x4008b944 <virtio_blk_do_op+0x174>
4008b944: d0000ce8     	adrp	x8, 0x40229000 <ack_used_idx>
4008b948: 39406108     	ldrb	w8, [x8, #0x18]
4008b94c: 71000108     	subs	w8, w8, #0x0
4008b950: 5a9f03e8     	csetm	w8, ne
4008b954: b9000fe8     	str	w8, [sp, #0xc]
4008b958: f9400fe1     	ldr	x1, [sp, #0x18]
4008b95c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b960: 91000000     	add	x0, x0, #0x0
4008b964: 97ffe337     	bl	0x40084640 <spinlock_release_irqrestore>
4008b968: b9400fe8     	ldr	w8, [sp, #0xc]
4008b96c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008b970: 14000001     	b	0x4008b974 <virtio_blk_do_op+0x1a4>
4008b974: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008b978: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008b97c: 910143ff     	add	sp, sp, #0x50
4008b980: d65f03c0     	ret
4008b984: d503201f     	nop
4008b988: d503201f     	nop
4008b98c: d503201f     	nop

000000004008b990 <virtio_blk_write_sector>:
4008b990: d10103ff     	sub	sp, sp, #0x40
4008b994: a9037bfd     	stp	x29, x30, [sp, #0x30]
4008b998: 9100c3fd     	add	x29, sp, #0x30
4008b99c: f81f83a0     	stur	x0, [x29, #-0x8]
4008b9a0: f81f03a1     	stur	x1, [x29, #-0x10]
4008b9a4: b81ec3a2     	stur	w2, [x29, #-0x14]
4008b9a8: 14000001     	b	0x4008b9ac <virtio_blk_write_sector+0x1c>
4008b9ac: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b9b0: 91004000     	add	x0, x0, #0x10
4008b9b4: 97ffe313     	bl	0x40084600 <spinlock_acquire_irqsave>
4008b9b8: f9000be0     	str	x0, [sp, #0x10]
4008b9bc: d0000ce8     	adrp	x8, 0x40229000 <ack_used_idx>
4008b9c0: b9400508     	ldr	w8, [x8, #0x4]
4008b9c4: 35000148     	cbnz	w8, 0x4008b9ec <virtio_blk_write_sector+0x5c>
4008b9c8: 14000001     	b	0x4008b9cc <virtio_blk_write_sector+0x3c>
4008b9cc: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008b9d0: 52800028     	mov	w8, #0x1                // =1
4008b9d4: b9000528     	str	w8, [x9, #0x4]
4008b9d8: f9400be1     	ldr	x1, [sp, #0x10]
4008b9dc: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b9e0: 91004000     	add	x0, x0, #0x10
4008b9e4: 97ffe317     	bl	0x40084640 <spinlock_release_irqrestore>
4008b9e8: 14000007     	b	0x4008ba04 <virtio_blk_write_sector+0x74>
4008b9ec: f9400be1     	ldr	x1, [sp, #0x10]
4008b9f0: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008b9f4: 91004000     	add	x0, x0, #0x10
4008b9f8: 97ffe312     	bl	0x40084640 <spinlock_release_irqrestore>
4008b9fc: d503207f     	wfi
4008ba00: 17ffffeb     	b	0x4008b9ac <virtio_blk_write_sector+0x1c>
4008ba04: 2a1f03e8     	mov	w8, wzr
4008ba08: b9000fe8     	str	w8, [sp, #0xc]
4008ba0c: b9000be8     	str	w8, [sp, #0x8]
4008ba10: 14000001     	b	0x4008ba14 <virtio_blk_write_sector+0x84>
4008ba14: b9400be8     	ldr	w8, [sp, #0x8]
4008ba18: b85ec3a9     	ldur	w9, [x29, #-0x14]
4008ba1c: 6b090108     	subs	w8, w8, w9
4008ba20: 540002e2     	b.hs	0x4008ba7c <virtio_blk_write_sector+0xec>
4008ba24: 14000001     	b	0x4008ba28 <virtio_blk_write_sector+0x98>
4008ba28: f85f83a8     	ldur	x8, [x29, #-0x8]
4008ba2c: b9400be9     	ldr	w9, [sp, #0x8]
4008ba30: 2a0903ea     	mov	w10, w9
4008ba34: 2a0a03e9     	mov	w9, w10
4008ba38: 8b0a0100     	add	x0, x8, x10
4008ba3c: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ba40: 53175929     	lsl	w9, w9, #9
4008ba44: 2a0903e9     	mov	w9, w9
4008ba48: 8b090101     	add	x1, x8, x9
4008ba4c: 52800022     	mov	w2, #0x1                // =1
4008ba50: 97ffff60     	bl	0x4008b7d0 <virtio_blk_do_op>
4008ba54: 340000a0     	cbz	w0, 0x4008ba68 <virtio_blk_write_sector+0xd8>
4008ba58: 14000001     	b	0x4008ba5c <virtio_blk_write_sector+0xcc>
4008ba5c: 12800008     	mov	w8, #-0x1               // =-1
4008ba60: b9000fe8     	str	w8, [sp, #0xc]
4008ba64: 14000006     	b	0x4008ba7c <virtio_blk_write_sector+0xec>
4008ba68: 14000001     	b	0x4008ba6c <virtio_blk_write_sector+0xdc>
4008ba6c: b9400be8     	ldr	w8, [sp, #0x8]
4008ba70: 11000508     	add	w8, w8, #0x1
4008ba74: b9000be8     	str	w8, [sp, #0x8]
4008ba78: 17ffffe7     	b	0x4008ba14 <virtio_blk_write_sector+0x84>
4008ba7c: f0000cc0     	adrp	x0, 0x40226000 <blk_lock>
4008ba80: 91004000     	add	x0, x0, #0x10
4008ba84: f90003e0     	str	x0, [sp]
4008ba88: 97ffe2de     	bl	0x40084600 <spinlock_acquire_irqsave>
4008ba8c: aa0003e8     	mov	x8, x0
4008ba90: f94003e0     	ldr	x0, [sp]
4008ba94: f9000be8     	str	x8, [sp, #0x10]
4008ba98: d0000ce9     	adrp	x9, 0x40229000 <ack_used_idx>
4008ba9c: 2a1f03e8     	mov	w8, wzr
4008baa0: b9000528     	str	w8, [x9, #0x4]
4008baa4: f9400be1     	ldr	x1, [sp, #0x10]
4008baa8: 97ffe2e6     	bl	0x40084640 <spinlock_release_irqrestore>
4008baac: 97fff715     	bl	0x40089700 <process_wake_all>
4008bab0: b9400fe0     	ldr	w0, [sp, #0xc]
4008bab4: a9437bfd     	ldp	x29, x30, [sp, #0x30]
4008bab8: 910103ff     	add	sp, sp, #0x40
4008babc: d65f03c0     	ret

000000004008bac0 <virtio_gpu_init>:
4008bac0: d10503ff     	sub	sp, sp, #0x140
4008bac4: a9127bfd     	stp	x29, x30, [sp, #0x120]
4008bac8: f9009bfc     	str	x28, [sp, #0x130]
4008bacc: 910483fd     	add	x29, sp, #0x120
4008bad0: d10123a8     	sub	x8, x29, #0x48
4008bad4: f9002be8     	str	x8, [sp, #0x50]
4008bad8: b0001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008badc: 91000000     	add	x0, x0, #0x0
4008bae0: 97ffe2ac     	bl	0x40084590 <spinlock_init>
4008bae4: 2a1f03e8     	mov	w8, wzr
4008bae8: b81f83a8     	stur	w8, [x29, #-0x8]
4008baec: 14000001     	b	0x4008baf0 <virtio_gpu_init+0x30>
4008baf0: b85f83a8     	ldur	w8, [x29, #-0x8]
4008baf4: 71007d08     	subs	w8, w8, #0x1f
4008baf8: 5400044c     	b.gt	0x4008bb80 <virtio_gpu_init+0xc0>
4008bafc: 14000001     	b	0x4008bb00 <virtio_gpu_init+0x40>
4008bb00: f9402be8     	ldr	x8, [sp, #0x50]
4008bb04: b85f83a9     	ldur	w9, [x29, #-0x8]
4008bb08: 5317592a     	lsl	w10, w9, #9
4008bb0c: 52a14009     	mov	w9, #0xa000000          // =167772160
4008bb10: 8b2ac129     	add	x9, x9, w10, sxtw
4008bb14: f9001d09     	str	x9, [x8, #0x38]
4008bb18: f9401d09     	ldr	x9, [x8, #0x38]
4008bb1c: b9400129     	ldr	w9, [x9]
4008bb20: b81ec3a9     	stur	w9, [x29, #-0x14]
4008bb24: f9401d08     	ldr	x8, [x8, #0x38]
4008bb28: b9400908     	ldr	w8, [x8, #0x8]
4008bb2c: b81e83a8     	stur	w8, [x29, #-0x18]
4008bb30: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008bb34: 528d2ec9     	mov	w9, #0x6976             // =26998
4008bb38: 72ae8e49     	movk	w9, #0x7472, lsl #16
4008bb3c: 6b090108     	subs	w8, w8, w9
4008bb40: 54000161     	b.ne	0x4008bb6c <virtio_gpu_init+0xac>
4008bb44: 14000001     	b	0x4008bb48 <virtio_gpu_init+0x88>
4008bb48: b85e83a8     	ldur	w8, [x29, #-0x18]
4008bb4c: 71004108     	subs	w8, w8, #0x10
4008bb50: 540000e1     	b.ne	0x4008bb6c <virtio_gpu_init+0xac>
4008bb54: 14000001     	b	0x4008bb58 <virtio_gpu_init+0x98>
4008bb58: f9402be8     	ldr	x8, [sp, #0x50]
4008bb5c: f9401d08     	ldr	x8, [x8, #0x38]
4008bb60: b0001ba9     	adrp	x9, 0x40400000 <gpu_lock>
4008bb64: f9000528     	str	x8, [x9, #0x8]
4008bb68: 14000006     	b	0x4008bb80 <virtio_gpu_init+0xc0>
4008bb6c: 14000001     	b	0x4008bb70 <virtio_gpu_init+0xb0>
4008bb70: b85f83a8     	ldur	w8, [x29, #-0x8]
4008bb74: 11000508     	add	w8, w8, #0x1
4008bb78: b81f83a8     	stur	w8, [x29, #-0x8]
4008bb7c: 17ffffdd     	b	0x4008baf0 <virtio_gpu_init+0x30>
4008bb80: b0001ba8     	adrp	x8, 0x40400000 <gpu_lock>
4008bb84: f9400508     	ldr	x8, [x8, #0x8]
4008bb88: b50000a8     	cbnz	x8, 0x4008bb9c <virtio_gpu_init+0xdc>
4008bb8c: 14000001     	b	0x4008bb90 <virtio_gpu_init+0xd0>
4008bb90: 12800008     	mov	w8, #-0x1               // =-1
4008bb94: b81fc3a8     	stur	w8, [x29, #-0x4]
4008bb98: 140000b1     	b	0x4008be5c <virtio_gpu_init+0x39c>
4008bb9c: 2a1f03e8     	mov	w8, wzr
4008bba0: b9004be8     	str	w8, [sp, #0x48]
4008bba4: b81e43a8     	stur	w8, [x29, #-0x1c]
4008bba8: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008bbac: 52800e00     	mov	w0, #0x70               // =112
4008bbb0: b9004fe0     	str	w0, [sp, #0x4c]
4008bbb4: 940000af     	bl	0x4008be70 <reg_write32>
4008bbb8: b9404fe0     	ldr	w0, [sp, #0x4c]
4008bbbc: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008bbc0: 32000108     	orr	w8, w8, #0x1
4008bbc4: b81e43a8     	stur	w8, [x29, #-0x1c]
4008bbc8: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008bbcc: 940000a9     	bl	0x4008be70 <reg_write32>
4008bbd0: b9404fe0     	ldr	w0, [sp, #0x4c]
4008bbd4: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008bbd8: 321f0108     	orr	w8, w8, #0x2
4008bbdc: b81e43a8     	stur	w8, [x29, #-0x1c]
4008bbe0: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008bbe4: 940000a3     	bl	0x4008be70 <reg_write32>
4008bbe8: 52800480     	mov	w0, #0x24               // =36
4008bbec: b90043e0     	str	w0, [sp, #0x40]
4008bbf0: 52800021     	mov	w1, #0x1                // =1
4008bbf4: b9003fe1     	str	w1, [sp, #0x3c]
4008bbf8: 9400009e     	bl	0x4008be70 <reg_write32>
4008bbfc: b9403fe1     	ldr	w1, [sp, #0x3c]
4008bc00: 52800400     	mov	w0, #0x20               // =32
4008bc04: b90047e0     	str	w0, [sp, #0x44]
4008bc08: 9400009a     	bl	0x4008be70 <reg_write32>
4008bc0c: b94043e0     	ldr	w0, [sp, #0x40]
4008bc10: b9404be1     	ldr	w1, [sp, #0x48]
4008bc14: 94000097     	bl	0x4008be70 <reg_write32>
4008bc18: b94047e0     	ldr	w0, [sp, #0x44]
4008bc1c: b9404be1     	ldr	w1, [sp, #0x48]
4008bc20: 94000094     	bl	0x4008be70 <reg_write32>
4008bc24: b9404fe0     	ldr	w0, [sp, #0x4c]
4008bc28: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008bc2c: 321d0108     	orr	w8, w8, #0x8
4008bc30: b81e43a8     	stur	w8, [x29, #-0x1c]
4008bc34: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008bc38: 9400008e     	bl	0x4008be70 <reg_write32>
4008bc3c: b9404fe0     	ldr	w0, [sp, #0x4c]
4008bc40: 94000098     	bl	0x4008bea0 <reg_read32>
4008bc44: 371800a0     	tbnz	w0, #0x3, 0x4008bc58 <virtio_gpu_init+0x198>
4008bc48: 14000001     	b	0x4008bc4c <virtio_gpu_init+0x18c>
4008bc4c: 12800008     	mov	w8, #-0x1               // =-1
4008bc50: b81fc3a8     	stur	w8, [x29, #-0x4]
4008bc54: 14000082     	b	0x4008be5c <virtio_gpu_init+0x39c>
4008bc58: 52800500     	mov	w0, #0x28               // =40
4008bc5c: 52820001     	mov	w1, #0x1000             // =4096
4008bc60: 94000084     	bl	0x4008be70 <reg_write32>
4008bc64: 52800600     	mov	w0, #0x30               // =48
4008bc68: 2a1f03e1     	mov	w1, wzr
4008bc6c: 94000081     	bl	0x4008be70 <reg_write32>
4008bc70: 52800680     	mov	w0, #0x34               // =52
4008bc74: 9400008b     	bl	0x4008bea0 <reg_read32>
4008bc78: b81e03a0     	stur	w0, [x29, #-0x20]
4008bc7c: b85e03a8     	ldur	w8, [x29, #-0x20]
4008bc80: 350000a8     	cbnz	w8, 0x4008bc94 <virtio_gpu_init+0x1d4>
4008bc84: 14000001     	b	0x4008bc88 <virtio_gpu_init+0x1c8>
4008bc88: 12800008     	mov	w8, #-0x1               // =-1
4008bc8c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008bc90: 14000073     	b	0x4008be5c <virtio_gpu_init+0x39c>
4008bc94: 52800700     	mov	w0, #0x38               // =56
4008bc98: 52800201     	mov	w1, #0x10               // =16
4008bc9c: b9000fe1     	str	w1, [sp, #0xc]
4008bca0: 94000074     	bl	0x4008be70 <reg_write32>
4008bca4: 52800780     	mov	w0, #0x3c               // =60
4008bca8: 52820001     	mov	w1, #0x1000             // =4096
4008bcac: 94000071     	bl	0x4008be70 <reg_write32>
4008bcb0: d0001ba8     	adrp	x8, 0x40401000 <gpu_vq>
4008bcb4: 91000108     	add	x8, x8, #0x0
4008bcb8: d34cfd08     	lsr	x8, x8, #12
4008bcbc: 2a0803e1     	mov	w1, w8
4008bcc0: 52800800     	mov	w0, #0x40               // =64
4008bcc4: 9400006b     	bl	0x4008be70 <reg_write32>
4008bcc8: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008bccc: 321e0108     	orr	w8, w8, #0x4
4008bcd0: b81e43a8     	stur	w8, [x29, #-0x1c]
4008bcd4: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008bcd8: 52800e00     	mov	w0, #0x70               // =112
4008bcdc: 94000065     	bl	0x4008be70 <reg_write32>
4008bce0: f9402be9     	ldr	x9, [sp, #0x50]
4008bce4: aa1f03e8     	mov	x8, xzr
4008bce8: f9000be8     	str	x8, [sp, #0x10]
4008bcec: f9001128     	str	x8, [x9, #0x20]
4008bcf0: f9000d28     	str	x8, [x9, #0x18]
4008bcf4: f9000928     	str	x8, [x9, #0x10]
4008bcf8: f9000528     	str	x8, [x9, #0x8]
4008bcfc: f81b83a8     	stur	x8, [x29, #-0x48]
4008bd00: 52802028     	mov	w8, #0x101              // =257
4008bd04: b81b83a8     	stur	w8, [x29, #-0x48]
4008bd08: 52800028     	mov	w8, #0x1                // =1
4008bd0c: b90027e8     	str	w8, [sp, #0x24]
4008bd10: b81d03a8     	stur	w8, [x29, #-0x30]
4008bd14: b81d43a8     	stur	w8, [x29, #-0x2c]
4008bd18: 52808008     	mov	w8, #0x400              // =1024
4008bd1c: b9001fe8     	str	w8, [sp, #0x1c]
4008bd20: b81d83a8     	stur	w8, [x29, #-0x28]
4008bd24: 52806008     	mov	w8, #0x300              // =768
4008bd28: b90023e8     	str	w8, [sp, #0x20]
4008bd2c: b81dc3a8     	stur	w8, [x29, #-0x24]
4008bd30: d10123a0     	sub	x0, x29, #0x48
4008bd34: 52800501     	mov	w1, #0x28               // =40
4008bd38: d10183a2     	sub	x2, x29, #0x60
4008bd3c: f90017e2     	str	x2, [sp, #0x28]
4008bd40: 52800303     	mov	w3, #0x18               // =24
4008bd44: b90037e3     	str	w3, [sp, #0x34]
4008bd48: 9400005e     	bl	0x4008bec0 <virtio_gpu_do_cmd>
4008bd4c: b9400fe3     	ldr	w3, [sp, #0xc]
4008bd50: f9400be8     	ldr	x8, [sp, #0x10]
4008bd54: b94027e9     	ldr	w9, [sp, #0x24]
4008bd58: f94017e4     	ldr	x4, [sp, #0x28]
4008bd5c: b94037e5     	ldr	w5, [sp, #0x34]
4008bd60: f81983a8     	stur	x8, [x29, #-0x68]
4008bd64: f81903a8     	stur	x8, [x29, #-0x70]
4008bd68: f81883a8     	stur	x8, [x29, #-0x78]
4008bd6c: f81803a8     	stur	x8, [x29, #-0x80]
4008bd70: 528020ca     	mov	w10, #0x106             // =262
4008bd74: b81803aa     	stur	w10, [x29, #-0x80]
4008bd78: b81983a9     	stur	w9, [x29, #-0x68]
4008bd7c: b819c3a9     	stur	w9, [x29, #-0x64]
4008bd80: f9004fe8     	str	x8, [sp, #0x98]
4008bd84: f9004be8     	str	x8, [sp, #0x90]
4008bd88: b0002ba8     	adrp	x8, 0x40600000 <framebuffer>
4008bd8c: 91000108     	add	x8, x8, #0x0
4008bd90: f9004be8     	str	x8, [sp, #0x90]
4008bd94: 52a00608     	mov	w8, #0x300000           // =3145728
4008bd98: b9009be8     	str	w8, [sp, #0x98]
4008bd9c: d10203a0     	sub	x0, x29, #0x80
4008bda0: 52800401     	mov	w1, #0x20               // =32
4008bda4: 910243e2     	add	x2, sp, #0x90
4008bda8: 9400009e     	bl	0x4008c020 <virtio_gpu_do_cmd_with_data>
4008bdac: f9400beb     	ldr	x11, [sp, #0x10]
4008bdb0: b9401fea     	ldr	w10, [sp, #0x1c]
4008bdb4: b94023e9     	ldr	w9, [sp, #0x20]
4008bdb8: b94027e8     	ldr	w8, [sp, #0x24]
4008bdbc: f94017e2     	ldr	x2, [sp, #0x28]
4008bdc0: b94037e3     	ldr	w3, [sp, #0x34]
4008bdc4: f90047eb     	str	x11, [sp, #0x88]
4008bdc8: f90043eb     	str	x11, [sp, #0x80]
4008bdcc: f9003feb     	str	x11, [sp, #0x78]
4008bdd0: f9003beb     	str	x11, [sp, #0x70]
4008bdd4: f90037eb     	str	x11, [sp, #0x68]
4008bdd8: f90033eb     	str	x11, [sp, #0x60]
4008bddc: 5280206b     	mov	w11, #0x103             // =259
4008bde0: b90063eb     	str	w11, [sp, #0x60]
4008bde4: b90083ea     	str	w10, [sp, #0x80]
4008bde8: b90087e9     	str	w9, [sp, #0x84]
4008bdec: 2a1f03e9     	mov	w9, wzr
4008bdf0: b9003be9     	str	w9, [sp, #0x38]
4008bdf4: b9008be9     	str	w9, [sp, #0x88]
4008bdf8: b9008fe8     	str	w8, [sp, #0x8c]
4008bdfc: 910183e0     	add	x0, sp, #0x60
4008be00: 52800601     	mov	w1, #0x30               // =48
4008be04: 9400002f     	bl	0x4008bec0 <virtio_gpu_do_cmd>
4008be08: b9403be8     	ldr	w8, [sp, #0x38]
4008be0c: b9005fe8     	str	w8, [sp, #0x5c]
4008be10: 14000001     	b	0x4008be14 <virtio_gpu_init+0x354>
4008be14: b9405fe8     	ldr	w8, [sp, #0x5c]
4008be18: 71430108     	subs	w8, w8, #0xc0, lsl #12  // =0xc0000
4008be1c: 5400018a     	b.ge	0x4008be4c <virtio_gpu_init+0x38c>
4008be20: 14000001     	b	0x4008be24 <virtio_gpu_init+0x364>
4008be24: b9805fea     	ldrsw	x10, [sp, #0x5c]
4008be28: b0002ba9     	adrp	x9, 0x40600000 <framebuffer>
4008be2c: 91000129     	add	x9, x9, #0x0
4008be30: 52bfe008     	mov	w8, #-0x1000000         // =-16777216
4008be34: b82a7928     	str	w8, [x9, x10, lsl #2]
4008be38: 14000001     	b	0x4008be3c <virtio_gpu_init+0x37c>
4008be3c: b9405fe8     	ldr	w8, [sp, #0x5c]
4008be40: 11000508     	add	w8, w8, #0x1
4008be44: b9005fe8     	str	w8, [sp, #0x5c]
4008be48: 17fffff3     	b	0x4008be14 <virtio_gpu_init+0x354>
4008be4c: 940000cd     	bl	0x4008c180 <virtio_gpu_flush>
4008be50: 2a1f03e8     	mov	w8, wzr
4008be54: b81fc3a8     	stur	w8, [x29, #-0x4]
4008be58: 14000001     	b	0x4008be5c <virtio_gpu_init+0x39c>
4008be5c: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008be60: f9409bfc     	ldr	x28, [sp, #0x130]
4008be64: a9527bfd     	ldp	x29, x30, [sp, #0x120]
4008be68: 910503ff     	add	sp, sp, #0x140
4008be6c: d65f03c0     	ret

000000004008be70 <reg_write32>:
4008be70: d10043ff     	sub	sp, sp, #0x10
4008be74: b9000fe0     	str	w0, [sp, #0xc]
4008be78: b9000be1     	str	w1, [sp, #0x8]
4008be7c: b9400be8     	ldr	w8, [sp, #0x8]
4008be80: b0001ba9     	adrp	x9, 0x40400000 <gpu_lock>
4008be84: f9400529     	ldr	x9, [x9, #0x8]
4008be88: b9400fea     	ldr	w10, [sp, #0xc]
4008be8c: b82a6928     	str	w8, [x9, x10]
4008be90: 910043ff     	add	sp, sp, #0x10
4008be94: d65f03c0     	ret
4008be98: d503201f     	nop
4008be9c: d503201f     	nop

000000004008bea0 <reg_read32>:
4008bea0: d10043ff     	sub	sp, sp, #0x10
4008bea4: b9000fe0     	str	w0, [sp, #0xc]
4008bea8: b0001ba8     	adrp	x8, 0x40400000 <gpu_lock>
4008beac: f9400508     	ldr	x8, [x8, #0x8]
4008beb0: b9400fe9     	ldr	w9, [sp, #0xc]
4008beb4: b8696900     	ldr	w0, [x8, x9]
4008beb8: 910043ff     	add	sp, sp, #0x10
4008bebc: d65f03c0     	ret

000000004008bec0 <virtio_gpu_do_cmd>:
4008bec0: d10183ff     	sub	sp, sp, #0x60
4008bec4: a9057bfd     	stp	x29, x30, [sp, #0x50]
4008bec8: 910143fd     	add	x29, sp, #0x50
4008becc: f81f03a0     	stur	x0, [x29, #-0x10]
4008bed0: b81ec3a1     	stur	w1, [x29, #-0x14]
4008bed4: f81e03a2     	stur	x2, [x29, #-0x20]
4008bed8: b81dc3a3     	stur	w3, [x29, #-0x24]
4008bedc: b0001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008bee0: 91000000     	add	x0, x0, #0x0
4008bee4: 97ffe1c7     	bl	0x40084600 <spinlock_acquire_irqsave>
4008bee8: f90013e0     	str	x0, [sp, #0x20]
4008beec: b0001ba8     	adrp	x8, 0x40400000 <gpu_lock>
4008bef0: f9400508     	ldr	x8, [x8, #0x8]
4008bef4: b5000128     	cbnz	x8, 0x4008bf18 <virtio_gpu_do_cmd+0x58>
4008bef8: 14000001     	b	0x4008befc <virtio_gpu_do_cmd+0x3c>
4008befc: f94013e1     	ldr	x1, [sp, #0x20]
4008bf00: b0001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008bf04: 91000000     	add	x0, x0, #0x0
4008bf08: 97ffe1ce     	bl	0x40084640 <spinlock_release_irqrestore>
4008bf0c: 12800008     	mov	w8, #-0x1               // =-1
4008bf10: b81fc3a8     	stur	w8, [x29, #-0x4]
4008bf14: 1400003d     	b	0x4008c008 <virtio_gpu_do_cmd+0x148>
4008bf18: f85f03a8     	ldur	x8, [x29, #-0x10]
4008bf1c: d0001ba9     	adrp	x9, 0x40401000 <gpu_vq>
4008bf20: 91000129     	add	x9, x9, #0x0
4008bf24: f90003e9     	str	x9, [sp]
4008bf28: f9000128     	str	x8, [x9]
4008bf2c: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008bf30: b9000928     	str	w8, [x9, #0x8]
4008bf34: 52800028     	mov	w8, #0x1                // =1
4008bf38: 79001928     	strh	w8, [x9, #0xc]
4008bf3c: 79001d28     	strh	w8, [x9, #0xe]
4008bf40: f85e03a8     	ldur	x8, [x29, #-0x20]
4008bf44: f9000928     	str	x8, [x9, #0x10]
4008bf48: b85dc3a8     	ldur	w8, [x29, #-0x24]
4008bf4c: b9001928     	str	w8, [x9, #0x18]
4008bf50: 52800048     	mov	w8, #0x2                // =2
4008bf54: 79003928     	strh	w8, [x9, #0x1c]
4008bf58: 2a1f03e1     	mov	w1, wzr
4008bf5c: 79003d21     	strh	w1, [x9, #0x1e]
4008bf60: 79420528     	ldrh	w8, [x9, #0x102]
4008bf64: 79003fe8     	strh	w8, [sp, #0x1e]
4008bf68: 79403fe8     	ldrh	w8, [sp, #0x1e]
4008bf6c: 12000d08     	and	w8, w8, #0xf
4008bf70: 8b284528     	add	x8, x9, w8, uxtw #1
4008bf74: 79020901     	strh	w1, [x8, #0x104]
4008bf78: d5033fbf     	dmb	sy
4008bf7c: 79420528     	ldrh	w8, [x9, #0x102]
4008bf80: 11000508     	add	w8, w8, #0x1
4008bf84: 79020528     	strh	w8, [x9, #0x102]
4008bf88: d5033fbf     	dmb	sy
4008bf8c: 52800a00     	mov	w0, #0x50               // =80
4008bf90: 97ffffb8     	bl	0x4008be70 <reg_write32>
4008bf94: 14000001     	b	0x4008bf98 <virtio_gpu_do_cmd+0xd8>
4008bf98: f0001ba8     	adrp	x8, 0x40402000 <gpu_vq+0x1000>
4008bf9c: 79400508     	ldrh	w8, [x8, #0x2]
4008bfa0: b00043a9     	adrp	x9, 0x40900000 <gpu_ack_used_idx>
4008bfa4: 79400129     	ldrh	w9, [x9]
4008bfa8: 6b090108     	subs	w8, w8, w9
4008bfac: 54000061     	b.ne	0x4008bfb8 <virtio_gpu_do_cmd+0xf8>
4008bfb0: 14000001     	b	0x4008bfb4 <virtio_gpu_do_cmd+0xf4>
4008bfb4: 17fffff9     	b	0x4008bf98 <virtio_gpu_do_cmd+0xd8>
4008bfb8: f0001ba8     	adrp	x8, 0x40402000 <gpu_vq+0x1000>
4008bfbc: 79400508     	ldrh	w8, [x8, #0x2]
4008bfc0: b00043a9     	adrp	x9, 0x40900000 <gpu_ack_used_idx>
4008bfc4: 79000128     	strh	w8, [x9]
4008bfc8: d5033fbf     	dmb	sy
4008bfcc: f85e03a8     	ldur	x8, [x29, #-0x20]
4008bfd0: f9000be8     	str	x8, [sp, #0x10]
4008bfd4: f9400be8     	ldr	x8, [sp, #0x10]
4008bfd8: b9400108     	ldr	w8, [x8]
4008bfdc: 53097d08     	lsr	w8, w8, #9
4008bfe0: 71002108     	subs	w8, w8, #0x8
4008bfe4: 5a9f93e8     	csetm	w8, hi
4008bfe8: b9000fe8     	str	w8, [sp, #0xc]
4008bfec: f94013e1     	ldr	x1, [sp, #0x20]
4008bff0: b0001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008bff4: 91000000     	add	x0, x0, #0x0
4008bff8: 97ffe192     	bl	0x40084640 <spinlock_release_irqrestore>
4008bffc: b9400fe8     	ldr	w8, [sp, #0xc]
4008c000: b81fc3a8     	stur	w8, [x29, #-0x4]
4008c004: 14000001     	b	0x4008c008 <virtio_gpu_do_cmd+0x148>
4008c008: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008c00c: a9457bfd     	ldp	x29, x30, [sp, #0x50]
4008c010: 910183ff     	add	sp, sp, #0x60
4008c014: d65f03c0     	ret
4008c018: d503201f     	nop
4008c01c: d503201f     	nop

000000004008c020 <virtio_gpu_do_cmd_with_data>:
4008c020: d10183ff     	sub	sp, sp, #0x60
4008c024: a9057bfd     	stp	x29, x30, [sp, #0x50]
4008c028: 910143fd     	add	x29, sp, #0x50
4008c02c: f81f03a0     	stur	x0, [x29, #-0x10]
4008c030: b81ec3a1     	stur	w1, [x29, #-0x14]
4008c034: f81e03a2     	stur	x2, [x29, #-0x20]
4008c038: b81dc3a3     	stur	w3, [x29, #-0x24]
4008c03c: f90013e4     	str	x4, [sp, #0x20]
4008c040: b9001fe5     	str	w5, [sp, #0x1c]
4008c044: 90001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008c048: 91000000     	add	x0, x0, #0x0
4008c04c: 97ffe16d     	bl	0x40084600 <spinlock_acquire_irqsave>
4008c050: f9000be0     	str	x0, [sp, #0x10]
4008c054: 90001ba8     	adrp	x8, 0x40400000 <gpu_lock>
4008c058: f9400508     	ldr	x8, [x8, #0x8]
4008c05c: b5000128     	cbnz	x8, 0x4008c080 <virtio_gpu_do_cmd_with_data+0x60>
4008c060: 14000001     	b	0x4008c064 <virtio_gpu_do_cmd_with_data+0x44>
4008c064: f9400be1     	ldr	x1, [sp, #0x10]
4008c068: 90001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008c06c: 91000000     	add	x0, x0, #0x0
4008c070: 97ffe174     	bl	0x40084640 <spinlock_release_irqrestore>
4008c074: 12800008     	mov	w8, #-0x1               // =-1
4008c078: b81fc3a8     	stur	w8, [x29, #-0x4]
4008c07c: 1400003b     	b	0x4008c168 <virtio_gpu_do_cmd_with_data+0x148>
4008c080: f85f03a8     	ldur	x8, [x29, #-0x10]
4008c084: b0001ba9     	adrp	x9, 0x40401000 <gpu_vq>
4008c088: 91000129     	add	x9, x9, #0x0
4008c08c: f90003e9     	str	x9, [sp]
4008c090: f9000128     	str	x8, [x9]
4008c094: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008c098: b9000928     	str	w8, [x9, #0x8]
4008c09c: 52800028     	mov	w8, #0x1                // =1
4008c0a0: 79001928     	strh	w8, [x9, #0xc]
4008c0a4: 79001d28     	strh	w8, [x9, #0xe]
4008c0a8: f85e03aa     	ldur	x10, [x29, #-0x20]
4008c0ac: f900092a     	str	x10, [x9, #0x10]
4008c0b0: b85dc3aa     	ldur	w10, [x29, #-0x24]
4008c0b4: b900192a     	str	w10, [x9, #0x18]
4008c0b8: 79003928     	strh	w8, [x9, #0x1c]
4008c0bc: 52800048     	mov	w8, #0x2                // =2
4008c0c0: 79003d28     	strh	w8, [x9, #0x1e]
4008c0c4: f94013ea     	ldr	x10, [sp, #0x20]
4008c0c8: f900112a     	str	x10, [x9, #0x20]
4008c0cc: b9401fea     	ldr	w10, [sp, #0x1c]
4008c0d0: b900292a     	str	w10, [x9, #0x28]
4008c0d4: 79005928     	strh	w8, [x9, #0x2c]
4008c0d8: 2a1f03e1     	mov	w1, wzr
4008c0dc: 79005d21     	strh	w1, [x9, #0x2e]
4008c0e0: 79420528     	ldrh	w8, [x9, #0x102]
4008c0e4: 79001fe8     	strh	w8, [sp, #0xe]
4008c0e8: 79401fe8     	ldrh	w8, [sp, #0xe]
4008c0ec: 12000d08     	and	w8, w8, #0xf
4008c0f0: 8b284528     	add	x8, x9, w8, uxtw #1
4008c0f4: 79020901     	strh	w1, [x8, #0x104]
4008c0f8: d5033fbf     	dmb	sy
4008c0fc: 79420528     	ldrh	w8, [x9, #0x102]
4008c100: 11000508     	add	w8, w8, #0x1
4008c104: 79020528     	strh	w8, [x9, #0x102]
4008c108: d5033fbf     	dmb	sy
4008c10c: 52800a00     	mov	w0, #0x50               // =80
4008c110: 97ffff58     	bl	0x4008be70 <reg_write32>
4008c114: 14000001     	b	0x4008c118 <virtio_gpu_do_cmd_with_data+0xf8>
4008c118: d0001ba8     	adrp	x8, 0x40402000 <gpu_vq+0x1000>
4008c11c: 79400508     	ldrh	w8, [x8, #0x2]
4008c120: 900043a9     	adrp	x9, 0x40900000 <gpu_ack_used_idx>
4008c124: 79400129     	ldrh	w9, [x9]
4008c128: 6b090108     	subs	w8, w8, w9
4008c12c: 54000061     	b.ne	0x4008c138 <virtio_gpu_do_cmd_with_data+0x118>
4008c130: 14000001     	b	0x4008c134 <virtio_gpu_do_cmd_with_data+0x114>
4008c134: 17fffff9     	b	0x4008c118 <virtio_gpu_do_cmd_with_data+0xf8>
4008c138: d0001ba8     	adrp	x8, 0x40402000 <gpu_vq+0x1000>
4008c13c: 79400508     	ldrh	w8, [x8, #0x2]
4008c140: 900043a9     	adrp	x9, 0x40900000 <gpu_ack_used_idx>
4008c144: 79000128     	strh	w8, [x9]
4008c148: d5033fbf     	dmb	sy
4008c14c: f9400be1     	ldr	x1, [sp, #0x10]
4008c150: 90001ba0     	adrp	x0, 0x40400000 <gpu_lock>
4008c154: 91000000     	add	x0, x0, #0x0
4008c158: 97ffe13a     	bl	0x40084640 <spinlock_release_irqrestore>
4008c15c: 2a1f03e8     	mov	w8, wzr
4008c160: b81fc3a8     	stur	w8, [x29, #-0x4]
4008c164: 14000001     	b	0x4008c168 <virtio_gpu_do_cmd_with_data+0x148>
4008c168: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008c16c: a9457bfd     	ldp	x29, x30, [sp, #0x50]
4008c170: 910183ff     	add	sp, sp, #0x60
4008c174: d65f03c0     	ret
4008c178: d503201f     	nop
4008c17c: d503201f     	nop

000000004008c180 <virtio_gpu_flush>:
4008c180: d10303ff     	sub	sp, sp, #0xc0
4008c184: a90b7bfd     	stp	x29, x30, [sp, #0xb0]
4008c188: 9102c3fd     	add	x29, sp, #0xb0
4008c18c: 90001ba8     	adrp	x8, 0x40400000 <gpu_lock>
4008c190: f9400508     	ldr	x8, [x8, #0x8]
4008c194: b5000068     	cbnz	x8, 0x4008c1a0 <virtio_gpu_flush+0x20>
4008c198: 14000001     	b	0x4008c19c <virtio_gpu_flush+0x1c>
4008c19c: 14000031     	b	0x4008c260 <virtio_gpu_flush+0xe0>
4008c1a0: aa1f03e8     	mov	x8, xzr
4008c1a4: f90007e8     	str	x8, [sp, #0x8]
4008c1a8: f81f83a8     	stur	x8, [x29, #-0x8]
4008c1ac: f81f03a8     	stur	x8, [x29, #-0x10]
4008c1b0: f81e83a8     	stur	x8, [x29, #-0x18]
4008c1b4: f81e03a8     	stur	x8, [x29, #-0x20]
4008c1b8: f81d83a8     	stur	x8, [x29, #-0x28]
4008c1bc: f81d03a8     	stur	x8, [x29, #-0x30]
4008c1c0: f81c83a8     	stur	x8, [x29, #-0x38]
4008c1c4: 528020a8     	mov	w8, #0x105              // =261
4008c1c8: b81c83a8     	stur	w8, [x29, #-0x38]
4008c1cc: 52808008     	mov	w8, #0x400              // =1024
4008c1d0: b90017e8     	str	w8, [sp, #0x14]
4008c1d4: b81e83a8     	stur	w8, [x29, #-0x18]
4008c1d8: 52806008     	mov	w8, #0x300              // =768
4008c1dc: b9001be8     	str	w8, [sp, #0x18]
4008c1e0: b81ec3a8     	stur	w8, [x29, #-0x14]
4008c1e4: 52800028     	mov	w8, #0x1                // =1
4008c1e8: b9001fe8     	str	w8, [sp, #0x1c]
4008c1ec: b81f83a8     	stur	w8, [x29, #-0x8]
4008c1f0: d100e3a0     	sub	x0, x29, #0x38
4008c1f4: 52800701     	mov	w1, #0x38               // =56
4008c1f8: d10143a2     	sub	x2, x29, #0x50
4008c1fc: f90013e2     	str	x2, [sp, #0x20]
4008c200: 52800303     	mov	w3, #0x18               // =24
4008c204: b9002fe3     	str	w3, [sp, #0x2c]
4008c208: 97ffff2e     	bl	0x4008bec0 <virtio_gpu_do_cmd>
4008c20c: f94007eb     	ldr	x11, [sp, #0x8]
4008c210: b94017ea     	ldr	w10, [sp, #0x14]
4008c214: b9401be9     	ldr	w9, [sp, #0x18]
4008c218: b9401fe8     	ldr	w8, [sp, #0x1c]
4008c21c: f94013e2     	ldr	x2, [sp, #0x20]
4008c220: b9402fe3     	ldr	w3, [sp, #0x2c]
4008c224: f9002feb     	str	x11, [sp, #0x58]
4008c228: f9002beb     	str	x11, [sp, #0x50]
4008c22c: f90027eb     	str	x11, [sp, #0x48]
4008c230: f90023eb     	str	x11, [sp, #0x40]
4008c234: f9001feb     	str	x11, [sp, #0x38]
4008c238: f9001beb     	str	x11, [sp, #0x30]
4008c23c: 5280208b     	mov	w11, #0x104             // =260
4008c240: b90033eb     	str	w11, [sp, #0x30]
4008c244: b90053ea     	str	w10, [sp, #0x50]
4008c248: b90057e9     	str	w9, [sp, #0x54]
4008c24c: b9005be8     	str	w8, [sp, #0x58]
4008c250: 9100c3e0     	add	x0, sp, #0x30
4008c254: 52800601     	mov	w1, #0x30               // =48
4008c258: 97ffff1a     	bl	0x4008bec0 <virtio_gpu_do_cmd>
4008c25c: 14000001     	b	0x4008c260 <virtio_gpu_flush+0xe0>
4008c260: a94b7bfd     	ldp	x29, x30, [sp, #0xb0]
4008c264: 910303ff     	add	sp, sp, #0xc0
4008c268: d65f03c0     	ret
4008c26c: d503201f     	nop

000000004008c270 <virtio_gpu_get_framebuffer>:
4008c270: 90002ba0     	adrp	x0, 0x40600000 <framebuffer>
4008c274: 91000000     	add	x0, x0, #0x0
4008c278: d65f03c0     	ret
4008c27c: 00000000     	udf	#0x0

000000004008c280 <virtio_input_handle_irq>:
4008c280: d10143ff     	sub	sp, sp, #0x50
4008c284: a9047bfd     	stp	x29, x30, [sp, #0x40]
4008c288: 910103fd     	add	x29, sp, #0x40
4008c28c: b81fc3a0     	stur	w0, [x29, #-0x4]
4008c290: b00043a0     	adrp	x0, 0x40901000 <input_lock>
4008c294: 91000000     	add	x0, x0, #0x0
4008c298: 97ffe0da     	bl	0x40084600 <spinlock_acquire_irqsave>
4008c29c: f81f03a0     	stur	x0, [x29, #-0x10]
4008c2a0: 2a1f03e8     	mov	w8, wzr
4008c2a4: b81ec3a8     	stur	w8, [x29, #-0x14]
4008c2a8: 14000001     	b	0x4008c2ac <virtio_input_handle_irq+0x2c>
4008c2ac: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008c2b0: b00043a9     	adrp	x9, 0x40901000 <input_lock>
4008c2b4: b9400529     	ldr	w9, [x9, #0x4]
4008c2b8: 6b090108     	subs	w8, w8, w9
4008c2bc: 5400100a     	b.ge	0x4008c4bc <virtio_input_handle_irq+0x23c>
4008c2c0: 14000001     	b	0x4008c2c4 <virtio_input_handle_irq+0x44>
4008c2c4: b89ec3a9     	ldursw	x9, [x29, #-0x14]
4008c2c8: d00043a8     	adrp	x8, 0x40902000 <input_devs>
4008c2cc: 91000108     	add	x8, x8, #0x0
4008c2d0: 8b093908     	add	x8, x8, x9, lsl #14
4008c2d4: f90013e8     	str	x8, [sp, #0x20]
4008c2d8: f94013e8     	ldr	x8, [sp, #0x20]
4008c2dc: b9400908     	ldr	w8, [x8, #0x8]
4008c2e0: b85fc3a9     	ldur	w9, [x29, #-0x4]
4008c2e4: 6b090108     	subs	w8, w8, w9
4008c2e8: 54000e01     	b.ne	0x4008c4a8 <virtio_input_handle_irq+0x228>
4008c2ec: 14000001     	b	0x4008c2f0 <virtio_input_handle_irq+0x70>
4008c2f0: f94013e8     	ldr	x8, [sp, #0x20]
4008c2f4: f9400108     	ldr	x8, [x8]
4008c2f8: b4000d88     	cbz	x8, 0x4008c4a8 <virtio_input_handle_irq+0x228>
4008c2fc: 14000001     	b	0x4008c300 <virtio_input_handle_irq+0x80>
4008c300: f94013e8     	ldr	x8, [sp, #0x20]
4008c304: f9400100     	ldr	x0, [x8]
4008c308: 52800c01     	mov	w1, #0x60               // =96
4008c30c: 94000075     	bl	0x4008c4e0 <reg_read32>
4008c310: b9001fe0     	str	w0, [sp, #0x1c]
4008c314: b9401fe8     	ldr	w8, [sp, #0x1c]
4008c318: 34000c68     	cbz	w8, 0x4008c4a4 <virtio_input_handle_irq+0x224>
4008c31c: 14000001     	b	0x4008c320 <virtio_input_handle_irq+0xa0>
4008c320: f94013e8     	ldr	x8, [sp, #0x20]
4008c324: f9400100     	ldr	x0, [x8]
4008c328: b9401fe2     	ldr	w2, [sp, #0x1c]
4008c32c: 52800c81     	mov	w1, #0x64               // =100
4008c330: 94000074     	bl	0x4008c500 <reg_write32>
4008c334: 14000001     	b	0x4008c338 <virtio_input_handle_irq+0xb8>
4008c338: f94013e9     	ldr	x9, [sp, #0x20]
4008c33c: 52864008     	mov	w8, #0x3200             // =12800
4008c340: 78686928     	ldrh	w8, [x9, x8]
4008c344: 5284004a     	mov	w10, #0x2002            // =8194
4008c348: 786a6929     	ldrh	w9, [x9, x10]
4008c34c: 6b090108     	subs	w8, w8, w9
4008c350: 540009e0     	b.eq	0x4008c48c <virtio_input_handle_irq+0x20c>
4008c354: 14000001     	b	0x4008c358 <virtio_input_handle_irq+0xd8>
4008c358: d5033fbf     	dmb	sy
4008c35c: f94013e8     	ldr	x8, [sp, #0x20]
4008c360: 52864009     	mov	w9, #0x3200             // =12800
4008c364: 78696908     	ldrh	w8, [x8, x9]
4008c368: 12001508     	and	w8, w8, #0x3f
4008c36c: 790037e8     	strh	w8, [sp, #0x1a]
4008c370: f94013e8     	ldr	x8, [sp, #0x20]
4008c374: 794037e9     	ldrh	w9, [sp, #0x1a]
4008c378: 8b090d08     	add	x8, x8, x9, lsl #3
4008c37c: b9600508     	ldr	w8, [x8, #0x2004]
4008c380: b90017e8     	str	w8, [sp, #0x14]
4008c384: d0004428     	adrp	x8, 0x40912000 <ring_head>
4008c388: b9400108     	ldr	w8, [x8]
4008c38c: 11000508     	add	w8, w8, #0x1
4008c390: 2a1f03e9     	mov	w9, wzr
4008c394: 6b080129     	subs	w9, w9, w8
4008c398: 12001d29     	and	w9, w9, #0xff
4008c39c: 12001d08     	and	w8, w8, #0xff
4008c3a0: 5a894508     	csneg	w8, w8, w9, mi
4008c3a4: b90013e8     	str	w8, [sp, #0x10]
4008c3a8: b94013e8     	ldr	w8, [sp, #0x10]
4008c3ac: d0004429     	adrp	x9, 0x40912000 <ring_head>
4008c3b0: b9400529     	ldr	w9, [x9, #0x4]
4008c3b4: 6b090108     	subs	w8, w8, w9
4008c3b8: 540001c0     	b.eq	0x4008c3f0 <virtio_input_handle_irq+0x170>
4008c3bc: 14000001     	b	0x4008c3c0 <virtio_input_handle_irq+0x140>
4008c3c0: d0004429     	adrp	x9, 0x40912000 <ring_head>
4008c3c4: b980012b     	ldrsw	x11, [x9]
4008c3c8: d000442a     	adrp	x10, 0x40912000 <ring_head>
4008c3cc: 9100214a     	add	x10, x10, #0x8
4008c3d0: f94013e8     	ldr	x8, [sp, #0x20]
4008c3d4: b94017ec     	ldr	w12, [sp, #0x14]
4008c3d8: 8b0c0d08     	add	x8, x8, x12, lsl #3
4008c3dc: f9580108     	ldr	x8, [x8, #0x3000]
4008c3e0: f82b7948     	str	x8, [x10, x11, lsl #3]
4008c3e4: b94013e8     	ldr	w8, [sp, #0x10]
4008c3e8: b9000128     	str	w8, [x9]
4008c3ec: 14000001     	b	0x4008c3f0 <virtio_input_handle_irq+0x170>
4008c3f0: f94013e9     	ldr	x9, [sp, #0x20]
4008c3f4: b94017e8     	ldr	w8, [sp, #0x14]
4008c3f8: 2a0803ea     	mov	w10, w8
4008c3fc: 8b0a0d28     	add	x8, x9, x10, lsl #3
4008c400: 91400d08     	add	x8, x8, #0x3, lsl #12   // =0x3000
4008c404: 8b0a1129     	add	x9, x9, x10, lsl #4
4008c408: f9080128     	str	x8, [x9, #0x1000]
4008c40c: f94013e8     	ldr	x8, [sp, #0x20]
4008c410: b94017e9     	ldr	w9, [sp, #0x14]
4008c414: 8b091109     	add	x9, x8, x9, lsl #4
4008c418: 52800108     	mov	w8, #0x8                // =8
4008c41c: b9100928     	str	w8, [x9, #0x1008]
4008c420: f94013e8     	ldr	x8, [sp, #0x20]
4008c424: b94017e9     	ldr	w9, [sp, #0x14]
4008c428: 8b091109     	add	x9, x8, x9, lsl #4
4008c42c: 52800048     	mov	w8, #0x2                // =2
4008c430: 79201928     	strh	w8, [x9, #0x100c]
4008c434: f94013e8     	ldr	x8, [sp, #0x20]
4008c438: 79680508     	ldrh	w8, [x8, #0x1402]
4008c43c: 79001fe8     	strh	w8, [sp, #0xe]
4008c440: b94017e8     	ldr	w8, [sp, #0x14]
4008c444: f94013e9     	ldr	x9, [sp, #0x20]
4008c448: 79401fea     	ldrh	w10, [sp, #0xe]
4008c44c: 1200154a     	and	w10, w10, #0x3f
4008c450: 8b2a4529     	add	x9, x9, w10, uxtw #1
4008c454: 79280928     	strh	w8, [x9, #0x1404]
4008c458: d5033fbf     	dmb	sy
4008c45c: f94013e9     	ldr	x9, [sp, #0x20]
4008c460: 79680528     	ldrh	w8, [x9, #0x1402]
4008c464: 11000508     	add	w8, w8, #0x1
4008c468: 79280528     	strh	w8, [x9, #0x1402]
4008c46c: d5033fbf     	dmb	sy
4008c470: f94013e9     	ldr	x9, [sp, #0x20]
4008c474: 52864008     	mov	w8, #0x3200             // =12800
4008c478: 2a0803ea     	mov	w10, w8
4008c47c: 786a6928     	ldrh	w8, [x9, x10]
4008c480: 11000508     	add	w8, w8, #0x1
4008c484: 782a6928     	strh	w8, [x9, x10]
4008c488: 17ffffac     	b	0x4008c338 <virtio_input_handle_irq+0xb8>
4008c48c: f94013e8     	ldr	x8, [sp, #0x20]
4008c490: f9400100     	ldr	x0, [x8]
4008c494: 52800a01     	mov	w1, #0x50               // =80
4008c498: 2a1f03e2     	mov	w2, wzr
4008c49c: 94000019     	bl	0x4008c500 <reg_write32>
4008c4a0: 14000001     	b	0x4008c4a4 <virtio_input_handle_irq+0x224>
4008c4a4: 14000001     	b	0x4008c4a8 <virtio_input_handle_irq+0x228>
4008c4a8: 14000001     	b	0x4008c4ac <virtio_input_handle_irq+0x22c>
4008c4ac: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008c4b0: 11000508     	add	w8, w8, #0x1
4008c4b4: b81ec3a8     	stur	w8, [x29, #-0x14]
4008c4b8: 17ffff7d     	b	0x4008c2ac <virtio_input_handle_irq+0x2c>
4008c4bc: f85f03a1     	ldur	x1, [x29, #-0x10]
4008c4c0: b00043a0     	adrp	x0, 0x40901000 <input_lock>
4008c4c4: 91000000     	add	x0, x0, #0x0
4008c4c8: 97ffe05e     	bl	0x40084640 <spinlock_release_irqrestore>
4008c4cc: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008c4d0: 910143ff     	add	sp, sp, #0x50
4008c4d4: d65f03c0     	ret
4008c4d8: d503201f     	nop
4008c4dc: d503201f     	nop

000000004008c4e0 <reg_read32>:
4008c4e0: d10043ff     	sub	sp, sp, #0x10
4008c4e4: f90007e0     	str	x0, [sp, #0x8]
4008c4e8: b90007e1     	str	w1, [sp, #0x4]
4008c4ec: f94007e8     	ldr	x8, [sp, #0x8]
4008c4f0: b94007e9     	ldr	w9, [sp, #0x4]
4008c4f4: b8696900     	ldr	w0, [x8, x9]
4008c4f8: 910043ff     	add	sp, sp, #0x10
4008c4fc: d65f03c0     	ret

000000004008c500 <reg_write32>:
4008c500: d10043ff     	sub	sp, sp, #0x10
4008c504: f90007e0     	str	x0, [sp, #0x8]
4008c508: b90007e1     	str	w1, [sp, #0x4]
4008c50c: b90003e2     	str	w2, [sp]
4008c510: b94003e8     	ldr	w8, [sp]
4008c514: f94007e9     	ldr	x9, [sp, #0x8]
4008c518: b94007ea     	ldr	w10, [sp, #0x4]
4008c51c: b82a6928     	str	w8, [x9, x10]
4008c520: 910043ff     	add	sp, sp, #0x10
4008c524: d65f03c0     	ret
4008c528: d503201f     	nop
4008c52c: d503201f     	nop

000000004008c530 <virtio_input_get_events>:
4008c530: d100c3ff     	sub	sp, sp, #0x30
4008c534: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008c538: 910083fd     	add	x29, sp, #0x20
4008c53c: f81f83a0     	stur	x0, [x29, #-0x8]
4008c540: b81f43a1     	stur	w1, [x29, #-0xc]
4008c544: b00043a0     	adrp	x0, 0x40901000 <input_lock>
4008c548: 91000000     	add	x0, x0, #0x0
4008c54c: 97ffe02d     	bl	0x40084600 <spinlock_acquire_irqsave>
4008c550: f90007e0     	str	x0, [sp, #0x8]
4008c554: 2a1f03e8     	mov	w8, wzr
4008c558: b90007e8     	str	w8, [sp, #0x4]
4008c55c: 14000001     	b	0x4008c560 <virtio_input_get_events+0x30>
4008c560: d0004428     	adrp	x8, 0x40912000 <ring_head>
4008c564: b9400509     	ldr	w9, [x8, #0x4]
4008c568: d0004428     	adrp	x8, 0x40912000 <ring_head>
4008c56c: b940010a     	ldr	w10, [x8]
4008c570: 2a1f03e8     	mov	w8, wzr
4008c574: 6b0a0129     	subs	w9, w9, w10
4008c578: b90003e8     	str	w8, [sp]
4008c57c: 54000100     	b.eq	0x4008c59c <virtio_input_get_events+0x6c>
4008c580: 14000001     	b	0x4008c584 <virtio_input_get_events+0x54>
4008c584: b94007e8     	ldr	w8, [sp, #0x4]
4008c588: b85f43a9     	ldur	w9, [x29, #-0xc]
4008c58c: 6b090108     	subs	w8, w8, w9
4008c590: 1a9fa7e8     	cset	w8, lt
4008c594: b90003e8     	str	w8, [sp]
4008c598: 14000001     	b	0x4008c59c <virtio_input_get_events+0x6c>
4008c59c: b94003e8     	ldr	w8, [sp]
4008c5a0: 360002c8     	tbz	w8, #0x0, 0x4008c5f8 <virtio_input_get_events+0xc8>
4008c5a4: 14000001     	b	0x4008c5a8 <virtio_input_get_events+0x78>
4008c5a8: f85f83aa     	ldur	x10, [x29, #-0x8]
4008c5ac: b98007eb     	ldrsw	x11, [sp, #0x4]
4008c5b0: d0004429     	adrp	x9, 0x40912000 <ring_head>
4008c5b4: b980052c     	ldrsw	x12, [x9, #0x4]
4008c5b8: d0004428     	adrp	x8, 0x40912000 <ring_head>
4008c5bc: 91002108     	add	x8, x8, #0x8
4008c5c0: f86c7908     	ldr	x8, [x8, x12, lsl #3]
4008c5c4: f82b7948     	str	x8, [x10, x11, lsl #3]
4008c5c8: b9400528     	ldr	w8, [x9, #0x4]
4008c5cc: 11000508     	add	w8, w8, #0x1
4008c5d0: 2a1f03ea     	mov	w10, wzr
4008c5d4: 6b08014a     	subs	w10, w10, w8
4008c5d8: 12001d4a     	and	w10, w10, #0xff
4008c5dc: 12001d08     	and	w8, w8, #0xff
4008c5e0: 5a8a4508     	csneg	w8, w8, w10, mi
4008c5e4: b9000528     	str	w8, [x9, #0x4]
4008c5e8: b94007e8     	ldr	w8, [sp, #0x4]
4008c5ec: 11000508     	add	w8, w8, #0x1
4008c5f0: b90007e8     	str	w8, [sp, #0x4]
4008c5f4: 17ffffdb     	b	0x4008c560 <virtio_input_get_events+0x30>
4008c5f8: f94007e1     	ldr	x1, [sp, #0x8]
4008c5fc: b00043a0     	adrp	x0, 0x40901000 <input_lock>
4008c600: 91000000     	add	x0, x0, #0x0
4008c604: 97ffe00f     	bl	0x40084640 <spinlock_release_irqrestore>
4008c608: b94007e0     	ldr	w0, [sp, #0x4]
4008c60c: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008c610: 9100c3ff     	add	sp, sp, #0x30
4008c614: d65f03c0     	ret
4008c618: d503201f     	nop
4008c61c: d503201f     	nop

000000004008c620 <virtio_input_init>:
4008c620: d10183ff     	sub	sp, sp, #0x60
4008c624: a9057bfd     	stp	x29, x30, [sp, #0x50]
4008c628: 910143fd     	add	x29, sp, #0x50
4008c62c: b00043a0     	adrp	x0, 0x40901000 <input_lock>
4008c630: 91000000     	add	x0, x0, #0x0
4008c634: 97ffdfd7     	bl	0x40084590 <spinlock_init>
4008c638: b00043a9     	adrp	x9, 0x40901000 <input_lock>
4008c63c: 2a1f03e8     	mov	w8, wzr
4008c640: b9000528     	str	w8, [x9, #0x4]
4008c644: b81fc3a8     	stur	w8, [x29, #-0x4]
4008c648: 14000001     	b	0x4008c64c <virtio_input_init+0x2c>
4008c64c: b85fc3a9     	ldur	w9, [x29, #-0x4]
4008c650: 2a1f03e8     	mov	w8, wzr
4008c654: 71007d29     	subs	w9, w9, #0x1f
4008c658: b90023e8     	str	w8, [sp, #0x20]
4008c65c: 5400010c     	b.gt	0x4008c67c <virtio_input_init+0x5c>
4008c660: 14000001     	b	0x4008c664 <virtio_input_init+0x44>
4008c664: b00043a8     	adrp	x8, 0x40901000 <input_lock>
4008c668: b9400508     	ldr	w8, [x8, #0x4]
4008c66c: 71001108     	subs	w8, w8, #0x4
4008c670: 1a9fa7e8     	cset	w8, lt
4008c674: b90023e8     	str	w8, [sp, #0x20]
4008c678: 14000001     	b	0x4008c67c <virtio_input_init+0x5c>
4008c67c: b94023e8     	ldr	w8, [sp, #0x20]
4008c680: 36001808     	tbz	w8, #0x0, 0x4008c980 <virtio_input_init+0x360>
4008c684: 14000001     	b	0x4008c688 <virtio_input_init+0x68>
4008c688: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008c68c: 53175909     	lsl	w9, w8, #9
4008c690: 52a14008     	mov	w8, #0xa000000          // =167772160
4008c694: 8b29c108     	add	x8, x8, w9, sxtw
4008c698: f81f03a8     	stur	x8, [x29, #-0x10]
4008c69c: f85f03a8     	ldur	x8, [x29, #-0x10]
4008c6a0: b9400108     	ldr	w8, [x8]
4008c6a4: b81ec3a8     	stur	w8, [x29, #-0x14]
4008c6a8: f85f03a8     	ldur	x8, [x29, #-0x10]
4008c6ac: b9400908     	ldr	w8, [x8, #0x8]
4008c6b0: b81e83a8     	stur	w8, [x29, #-0x18]
4008c6b4: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008c6b8: 528d2ec9     	mov	w9, #0x6976             // =26998
4008c6bc: 72ae8e49     	movk	w9, #0x7472, lsl #16
4008c6c0: 6b090108     	subs	w8, w8, w9
4008c6c4: 54001541     	b.ne	0x4008c96c <virtio_input_init+0x34c>
4008c6c8: 14000001     	b	0x4008c6cc <virtio_input_init+0xac>
4008c6cc: b85e83a8     	ldur	w8, [x29, #-0x18]
4008c6d0: 71004908     	subs	w8, w8, #0x12
4008c6d4: 540014c1     	b.ne	0x4008c96c <virtio_input_init+0x34c>
4008c6d8: 14000001     	b	0x4008c6dc <virtio_input_init+0xbc>
4008c6dc: b00043a8     	adrp	x8, 0x40901000 <input_lock>
4008c6e0: b9800509     	ldrsw	x9, [x8, #0x4]
4008c6e4: d00043a8     	adrp	x8, 0x40902000 <input_devs>
4008c6e8: 91000108     	add	x8, x8, #0x0
4008c6ec: 8b093908     	add	x8, x8, x9, lsl #14
4008c6f0: f81e03a8     	stur	x8, [x29, #-0x20]
4008c6f4: f85f03a8     	ldur	x8, [x29, #-0x10]
4008c6f8: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c6fc: f9000128     	str	x8, [x9]
4008c700: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008c704: 1100c108     	add	w8, w8, #0x30
4008c708: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c70c: b9000928     	str	w8, [x9, #0x8]
4008c710: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c714: 52864008     	mov	w8, #0x3200             // =12800
4008c718: 2a0803ea     	mov	w10, w8
4008c71c: 2a1f03e8     	mov	w8, wzr
4008c720: b9001be8     	str	w8, [sp, #0x18]
4008c724: 782a6928     	strh	w8, [x9, x10]
4008c728: b81dc3a8     	stur	w8, [x29, #-0x24]
4008c72c: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c730: b85dc3a2     	ldur	w2, [x29, #-0x24]
4008c734: 52800e01     	mov	w1, #0x70               // =112
4008c738: b9001fe1     	str	w1, [sp, #0x1c]
4008c73c: 97ffff71     	bl	0x4008c500 <reg_write32>
4008c740: b9401fe1     	ldr	w1, [sp, #0x1c]
4008c744: b85dc3a8     	ldur	w8, [x29, #-0x24]
4008c748: 32000108     	orr	w8, w8, #0x1
4008c74c: b81dc3a8     	stur	w8, [x29, #-0x24]
4008c750: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c754: b85dc3a2     	ldur	w2, [x29, #-0x24]
4008c758: 97ffff6a     	bl	0x4008c500 <reg_write32>
4008c75c: b9401fe1     	ldr	w1, [sp, #0x1c]
4008c760: b85dc3a8     	ldur	w8, [x29, #-0x24]
4008c764: 321f0108     	orr	w8, w8, #0x2
4008c768: b81dc3a8     	stur	w8, [x29, #-0x24]
4008c76c: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c770: b85dc3a2     	ldur	w2, [x29, #-0x24]
4008c774: 97ffff63     	bl	0x4008c500 <reg_write32>
4008c778: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c77c: 52800481     	mov	w1, #0x24               // =36
4008c780: b90013e1     	str	w1, [sp, #0x10]
4008c784: 52800022     	mov	w2, #0x1                // =1
4008c788: b9000fe2     	str	w2, [sp, #0xc]
4008c78c: 97ffff5d     	bl	0x4008c500 <reg_write32>
4008c790: b9400fe2     	ldr	w2, [sp, #0xc]
4008c794: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c798: 52800401     	mov	w1, #0x20               // =32
4008c79c: b90017e1     	str	w1, [sp, #0x14]
4008c7a0: 97ffff58     	bl	0x4008c500 <reg_write32>
4008c7a4: b94013e1     	ldr	w1, [sp, #0x10]
4008c7a8: b9401be2     	ldr	w2, [sp, #0x18]
4008c7ac: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c7b0: 97ffff54     	bl	0x4008c500 <reg_write32>
4008c7b4: b94017e1     	ldr	w1, [sp, #0x14]
4008c7b8: b9401be2     	ldr	w2, [sp, #0x18]
4008c7bc: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c7c0: 97ffff50     	bl	0x4008c500 <reg_write32>
4008c7c4: b9401fe1     	ldr	w1, [sp, #0x1c]
4008c7c8: b85dc3a8     	ldur	w8, [x29, #-0x24]
4008c7cc: 321d0108     	orr	w8, w8, #0x8
4008c7d0: b81dc3a8     	stur	w8, [x29, #-0x24]
4008c7d4: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c7d8: b85dc3a2     	ldur	w2, [x29, #-0x24]
4008c7dc: 97ffff49     	bl	0x4008c500 <reg_write32>
4008c7e0: b9401fe1     	ldr	w1, [sp, #0x1c]
4008c7e4: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c7e8: 97ffff3e     	bl	0x4008c4e0 <reg_read32>
4008c7ec: 37180060     	tbnz	w0, #0x3, 0x4008c7f8 <virtio_input_init+0x1d8>
4008c7f0: 14000001     	b	0x4008c7f4 <virtio_input_init+0x1d4>
4008c7f4: 1400005f     	b	0x4008c970 <virtio_input_init+0x350>
4008c7f8: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c7fc: 52800501     	mov	w1, #0x28               // =40
4008c800: 52820002     	mov	w2, #0x1000             // =4096
4008c804: 97ffff3f     	bl	0x4008c500 <reg_write32>
4008c808: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c80c: 52800601     	mov	w1, #0x30               // =48
4008c810: 2a1f03e2     	mov	w2, wzr
4008c814: 97ffff3b     	bl	0x4008c500 <reg_write32>
4008c818: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c81c: 52800681     	mov	w1, #0x34               // =52
4008c820: 97ffff30     	bl	0x4008c4e0 <reg_read32>
4008c824: b9002be0     	str	w0, [sp, #0x28]
4008c828: b9402be8     	ldr	w8, [sp, #0x28]
4008c82c: 35000068     	cbnz	w8, 0x4008c838 <virtio_input_init+0x218>
4008c830: 14000001     	b	0x4008c834 <virtio_input_init+0x214>
4008c834: 1400004f     	b	0x4008c970 <virtio_input_init+0x350>
4008c838: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c83c: 52800701     	mov	w1, #0x38               // =56
4008c840: 52800802     	mov	w2, #0x40               // =64
4008c844: b9000be2     	str	w2, [sp, #0x8]
4008c848: 97ffff2e     	bl	0x4008c500 <reg_write32>
4008c84c: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c850: 52800781     	mov	w1, #0x3c               // =60
4008c854: 52820002     	mov	w2, #0x1000             // =4096
4008c858: 97ffff2a     	bl	0x4008c500 <reg_write32>
4008c85c: b9400be1     	ldr	w1, [sp, #0x8]
4008c860: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c864: f85e03a8     	ldur	x8, [x29, #-0x20]
4008c868: 91400508     	add	x8, x8, #0x1, lsl #12   // =0x1000
4008c86c: d34cfd08     	lsr	x8, x8, #12
4008c870: 2a0803e2     	mov	w2, w8
4008c874: 97ffff23     	bl	0x4008c500 <reg_write32>
4008c878: 2a1f03e8     	mov	w8, wzr
4008c87c: b90027e8     	str	w8, [sp, #0x24]
4008c880: 14000001     	b	0x4008c884 <virtio_input_init+0x264>
4008c884: b94027e8     	ldr	w8, [sp, #0x24]
4008c888: 7100fd08     	subs	w8, w8, #0x3f
4008c88c: 5400040c     	b.gt	0x4008c90c <virtio_input_init+0x2ec>
4008c890: 14000001     	b	0x4008c894 <virtio_input_init+0x274>
4008c894: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c898: b98027ea     	ldrsw	x10, [sp, #0x24]
4008c89c: 8b0a0d28     	add	x8, x9, x10, lsl #3
4008c8a0: 91400d08     	add	x8, x8, #0x3, lsl #12   // =0x3000
4008c8a4: 8b0a1129     	add	x9, x9, x10, lsl #4
4008c8a8: f9080128     	str	x8, [x9, #0x1000]
4008c8ac: f85e03a8     	ldur	x8, [x29, #-0x20]
4008c8b0: b98027e9     	ldrsw	x9, [sp, #0x24]
4008c8b4: 8b091109     	add	x9, x8, x9, lsl #4
4008c8b8: 52800108     	mov	w8, #0x8                // =8
4008c8bc: b9100928     	str	w8, [x9, #0x1008]
4008c8c0: f85e03a8     	ldur	x8, [x29, #-0x20]
4008c8c4: b98027e9     	ldrsw	x9, [sp, #0x24]
4008c8c8: 8b091109     	add	x9, x8, x9, lsl #4
4008c8cc: 52800048     	mov	w8, #0x2                // =2
4008c8d0: 79201928     	strh	w8, [x9, #0x100c]
4008c8d4: f85e03a8     	ldur	x8, [x29, #-0x20]
4008c8d8: b98027e9     	ldrsw	x9, [sp, #0x24]
4008c8dc: 8b091109     	add	x9, x8, x9, lsl #4
4008c8e0: 2a1f03e8     	mov	w8, wzr
4008c8e4: 79201d28     	strh	w8, [x9, #0x100e]
4008c8e8: b98027e8     	ldrsw	x8, [sp, #0x24]
4008c8ec: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c8f0: 8b080529     	add	x9, x9, x8, lsl #1
4008c8f4: 79280928     	strh	w8, [x9, #0x1404]
4008c8f8: 14000001     	b	0x4008c8fc <virtio_input_init+0x2dc>
4008c8fc: b94027e8     	ldr	w8, [sp, #0x24]
4008c900: 11000508     	add	w8, w8, #0x1
4008c904: b90027e8     	str	w8, [sp, #0x24]
4008c908: 17ffffdf     	b	0x4008c884 <virtio_input_init+0x264>
4008c90c: d5033fbf     	dmb	sy
4008c910: f85e03a9     	ldur	x9, [x29, #-0x20]
4008c914: 52800808     	mov	w8, #0x40               // =64
4008c918: 79280528     	strh	w8, [x9, #0x1402]
4008c91c: d5033fbf     	dmb	sy
4008c920: b85dc3a8     	ldur	w8, [x29, #-0x24]
4008c924: 321e0108     	orr	w8, w8, #0x4
4008c928: b81dc3a8     	stur	w8, [x29, #-0x24]
4008c92c: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c930: b85dc3a2     	ldur	w2, [x29, #-0x24]
4008c934: 52800e01     	mov	w1, #0x70               // =112
4008c938: 97fffef2     	bl	0x4008c500 <reg_write32>
4008c93c: f85f03a0     	ldur	x0, [x29, #-0x10]
4008c940: 52800a01     	mov	w1, #0x50               // =80
4008c944: 2a1f03e2     	mov	w2, wzr
4008c948: 97fffeee     	bl	0x4008c500 <reg_write32>
4008c94c: f85e03a8     	ldur	x8, [x29, #-0x20]
4008c950: b9400900     	ldr	w0, [x8, #0x8]
4008c954: 97ffdee3     	bl	0x400844e0 <gic_enable_interrupt>
4008c958: b00043a9     	adrp	x9, 0x40901000 <input_lock>
4008c95c: b9400528     	ldr	w8, [x9, #0x4]
4008c960: 11000508     	add	w8, w8, #0x1
4008c964: b9000528     	str	w8, [x9, #0x4]
4008c968: 14000001     	b	0x4008c96c <virtio_input_init+0x34c>
4008c96c: 14000001     	b	0x4008c970 <virtio_input_init+0x350>
4008c970: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008c974: 11000508     	add	w8, w8, #0x1
4008c978: b81fc3a8     	stur	w8, [x29, #-0x4]
4008c97c: 17ffff34     	b	0x4008c64c <virtio_input_init+0x2c>
4008c980: b00043a8     	adrp	x8, 0x40901000 <input_lock>
4008c984: b9400508     	ldr	w8, [x8, #0x4]
4008c988: 71000508     	subs	w8, w8, #0x1
4008c98c: 5a9fa3e0     	csetm	w0, lt
4008c990: a9457bfd     	ldp	x29, x30, [sp, #0x50]
4008c994: 910183ff     	add	sp, sp, #0x60
4008c998: d65f03c0     	ret
4008c99c: 00000000     	udf	#0x0

000000004008c9a0 <virtio_net_get_mac>:
4008c9a0: d10043ff     	sub	sp, sp, #0x10
4008c9a4: f90007e0     	str	x0, [sp, #0x8]
4008c9a8: 2a1f03e8     	mov	w8, wzr
4008c9ac: b90007e8     	str	w8, [sp, #0x4]
4008c9b0: 14000001     	b	0x4008c9b4 <virtio_net_get_mac+0x14>
4008c9b4: b94007e8     	ldr	w8, [sp, #0x4]
4008c9b8: 71001508     	subs	w8, w8, #0x5
4008c9bc: 540001ac     	b.gt	0x4008c9f0 <virtio_net_get_mac+0x50>
4008c9c0: 14000001     	b	0x4008c9c4 <virtio_net_get_mac+0x24>
4008c9c4: b98007ea     	ldrsw	x10, [sp, #0x4]
4008c9c8: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008c9cc: 91000108     	add	x8, x8, #0x0
4008c9d0: 386a6908     	ldrb	w8, [x8, x10]
4008c9d4: f94007e9     	ldr	x9, [sp, #0x8]
4008c9d8: 382a6928     	strb	w8, [x9, x10]
4008c9dc: 14000001     	b	0x4008c9e0 <virtio_net_get_mac+0x40>
4008c9e0: b94007e8     	ldr	w8, [sp, #0x4]
4008c9e4: 11000508     	add	w8, w8, #0x1
4008c9e8: b90007e8     	str	w8, [sp, #0x4]
4008c9ec: 17fffff2     	b	0x4008c9b4 <virtio_net_get_mac+0x14>
4008c9f0: 910043ff     	add	sp, sp, #0x10
4008c9f4: d65f03c0     	ret
4008c9f8: d503201f     	nop
4008c9fc: d503201f     	nop

000000004008ca00 <virtio_net_init>:
4008ca00: d10143ff     	sub	sp, sp, #0x50
4008ca04: a9047bfd     	stp	x29, x30, [sp, #0x40]
4008ca08: 910103fd     	add	x29, sp, #0x40
4008ca0c: f0004420     	adrp	x0, 0x40913000 <local_mac>
4008ca10: 91002000     	add	x0, x0, #0x8
4008ca14: 97ffdedf     	bl	0x40084590 <spinlock_init>
4008ca18: f0004420     	adrp	x0, 0x40913000 <local_mac>
4008ca1c: 91003000     	add	x0, x0, #0xc
4008ca20: 97ffdedc     	bl	0x40084590 <spinlock_init>
4008ca24: 2a1f03e8     	mov	w8, wzr
4008ca28: b81f83a8     	stur	w8, [x29, #-0x8]
4008ca2c: 14000001     	b	0x4008ca30 <virtio_net_init+0x30>
4008ca30: b85f83a8     	ldur	w8, [x29, #-0x8]
4008ca34: 71007d08     	subs	w8, w8, #0x1f
4008ca38: 5400048c     	b.gt	0x4008cac8 <virtio_net_init+0xc8>
4008ca3c: 14000001     	b	0x4008ca40 <virtio_net_init+0x40>
4008ca40: b85f83a8     	ldur	w8, [x29, #-0x8]
4008ca44: 53175909     	lsl	w9, w8, #9
4008ca48: 52a14008     	mov	w8, #0xa000000          // =167772160
4008ca4c: 8b29c108     	add	x8, x8, w9, sxtw
4008ca50: f81f03a8     	stur	x8, [x29, #-0x10]
4008ca54: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ca58: b9400108     	ldr	w8, [x8]
4008ca5c: b81ec3a8     	stur	w8, [x29, #-0x14]
4008ca60: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ca64: b9400908     	ldr	w8, [x8, #0x8]
4008ca68: b81e83a8     	stur	w8, [x29, #-0x18]
4008ca6c: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008ca70: 528d2ec9     	mov	w9, #0x6976             // =26998
4008ca74: 72ae8e49     	movk	w9, #0x7472, lsl #16
4008ca78: 6b090108     	subs	w8, w8, w9
4008ca7c: 540001c1     	b.ne	0x4008cab4 <virtio_net_init+0xb4>
4008ca80: 14000001     	b	0x4008ca84 <virtio_net_init+0x84>
4008ca84: b85e83a8     	ldur	w8, [x29, #-0x18]
4008ca88: 71000508     	subs	w8, w8, #0x1
4008ca8c: 54000141     	b.ne	0x4008cab4 <virtio_net_init+0xb4>
4008ca90: 14000001     	b	0x4008ca94 <virtio_net_init+0x94>
4008ca94: f85f03a8     	ldur	x8, [x29, #-0x10]
4008ca98: f0004429     	adrp	x9, 0x40913000 <local_mac>
4008ca9c: f9000928     	str	x8, [x9, #0x10]
4008caa0: b85f83a8     	ldur	w8, [x29, #-0x8]
4008caa4: 1100c108     	add	w8, w8, #0x30
4008caa8: b0000009     	adrp	x9, 0x4008d000 <virtio_net_send+0x90>
4008caac: b90bb128     	str	w8, [x9, #0xbb0]
4008cab0: 14000006     	b	0x4008cac8 <virtio_net_init+0xc8>
4008cab4: 14000001     	b	0x4008cab8 <virtio_net_init+0xb8>
4008cab8: b85f83a8     	ldur	w8, [x29, #-0x8]
4008cabc: 11000508     	add	w8, w8, #0x1
4008cac0: b81f83a8     	stur	w8, [x29, #-0x8]
4008cac4: 17ffffdb     	b	0x4008ca30 <virtio_net_init+0x30>
4008cac8: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cacc: f9400908     	ldr	x8, [x8, #0x10]
4008cad0: b5000108     	cbnz	x8, 0x4008caf0 <virtio_net_init+0xf0>
4008cad4: 14000001     	b	0x4008cad8 <virtio_net_init+0xd8>
4008cad8: d503201f     	nop
4008cadc: 30004480     	adr	x0, 0x4008d36d <UART0_FR+0x2d>
4008cae0: 97ffdf40     	bl	0x400847e0 <uart_puts>
4008cae4: 12800008     	mov	w8, #-0x1               // =-1
4008cae8: b81fc3a8     	stur	w8, [x29, #-0x4]
4008caec: 140000a8     	b	0x4008cd8c <virtio_net_init+0x38c>
4008caf0: 2a1f03e8     	mov	w8, wzr
4008caf4: b9000fe8     	str	w8, [sp, #0xc]
4008caf8: b81e43a8     	stur	w8, [x29, #-0x1c]
4008cafc: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008cb00: 52800e00     	mov	w0, #0x70               // =112
4008cb04: b90017e0     	str	w0, [sp, #0x14]
4008cb08: 940000a6     	bl	0x4008cda0 <reg_write32>
4008cb0c: b94017e0     	ldr	w0, [sp, #0x14]
4008cb10: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008cb14: 32000108     	orr	w8, w8, #0x1
4008cb18: b81e43a8     	stur	w8, [x29, #-0x1c]
4008cb1c: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008cb20: 940000a0     	bl	0x4008cda0 <reg_write32>
4008cb24: b94017e0     	ldr	w0, [sp, #0x14]
4008cb28: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008cb2c: 321f0108     	orr	w8, w8, #0x2
4008cb30: b81e43a8     	stur	w8, [x29, #-0x1c]
4008cb34: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008cb38: 9400009a     	bl	0x4008cda0 <reg_write32>
4008cb3c: 52800480     	mov	w0, #0x24               // =36
4008cb40: b9000be0     	str	w0, [sp, #0x8]
4008cb44: 52800021     	mov	w1, #0x1                // =1
4008cb48: b90007e1     	str	w1, [sp, #0x4]
4008cb4c: 94000095     	bl	0x4008cda0 <reg_write32>
4008cb50: b94007e1     	ldr	w1, [sp, #0x4]
4008cb54: 52800400     	mov	w0, #0x20               // =32
4008cb58: b90013e0     	str	w0, [sp, #0x10]
4008cb5c: 94000091     	bl	0x4008cda0 <reg_write32>
4008cb60: b9400be0     	ldr	w0, [sp, #0x8]
4008cb64: b9400fe1     	ldr	w1, [sp, #0xc]
4008cb68: 9400008e     	bl	0x4008cda0 <reg_write32>
4008cb6c: b94013e1     	ldr	w1, [sp, #0x10]
4008cb70: 2a0103e0     	mov	w0, w1
4008cb74: 9400008b     	bl	0x4008cda0 <reg_write32>
4008cb78: b94017e0     	ldr	w0, [sp, #0x14]
4008cb7c: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008cb80: 321d0108     	orr	w8, w8, #0x8
4008cb84: b81e43a8     	stur	w8, [x29, #-0x1c]
4008cb88: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008cb8c: 94000085     	bl	0x4008cda0 <reg_write32>
4008cb90: b94017e0     	ldr	w0, [sp, #0x14]
4008cb94: 9400008f     	bl	0x4008cdd0 <reg_read32>
4008cb98: 371800a0     	tbnz	w0, #0x3, 0x4008cbac <virtio_net_init+0x1ac>
4008cb9c: 14000001     	b	0x4008cba0 <virtio_net_init+0x1a0>
4008cba0: 12800008     	mov	w8, #-0x1               // =-1
4008cba4: b81fc3a8     	stur	w8, [x29, #-0x4]
4008cba8: 14000079     	b	0x4008cd8c <virtio_net_init+0x38c>
4008cbac: 2a1f03e8     	mov	w8, wzr
4008cbb0: b90023e8     	str	w8, [sp, #0x20]
4008cbb4: 14000001     	b	0x4008cbb8 <virtio_net_init+0x1b8>
4008cbb8: b94023e8     	ldr	w8, [sp, #0x20]
4008cbbc: 71001508     	subs	w8, w8, #0x5
4008cbc0: 540001cc     	b.gt	0x4008cbf8 <virtio_net_init+0x1f8>
4008cbc4: 14000001     	b	0x4008cbc8 <virtio_net_init+0x1c8>
4008cbc8: b94023e8     	ldr	w8, [sp, #0x20]
4008cbcc: 11040100     	add	w0, w8, #0x100
4008cbd0: 94000088     	bl	0x4008cdf0 <reg_read8>
4008cbd4: b98023e9     	ldrsw	x9, [sp, #0x20]
4008cbd8: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cbdc: 91000108     	add	x8, x8, #0x0
4008cbe0: 38296900     	strb	w0, [x8, x9]
4008cbe4: 14000001     	b	0x4008cbe8 <virtio_net_init+0x1e8>
4008cbe8: b94023e8     	ldr	w8, [sp, #0x20]
4008cbec: 11000508     	add	w8, w8, #0x1
4008cbf0: b90023e8     	str	w8, [sp, #0x20]
4008cbf4: 17fffff1     	b	0x4008cbb8 <virtio_net_init+0x1b8>
4008cbf8: b0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008cbfc: 91252800     	add	x0, x0, #0x94a
4008cc00: 97ffdef8     	bl	0x400847e0 <uart_puts>
4008cc04: 2a1f03e8     	mov	w8, wzr
4008cc08: b9001fe8     	str	w8, [sp, #0x1c]
4008cc0c: 14000001     	b	0x4008cc10 <virtio_net_init+0x210>
4008cc10: b9401fe8     	ldr	w8, [sp, #0x1c]
4008cc14: 71001508     	subs	w8, w8, #0x5
4008cc18: 540002ac     	b.gt	0x4008cc6c <virtio_net_init+0x26c>
4008cc1c: 14000001     	b	0x4008cc20 <virtio_net_init+0x220>
4008cc20: b9801fe9     	ldrsw	x9, [sp, #0x1c]
4008cc24: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cc28: 91000108     	add	x8, x8, #0x0
4008cc2c: 38696908     	ldrb	w8, [x8, x9]
4008cc30: 2a0803e0     	mov	w0, w8
4008cc34: 97ffdf5b     	bl	0x400849a0 <uart_print_hex>
4008cc38: b9401fe8     	ldr	w8, [sp, #0x1c]
4008cc3c: 71001108     	subs	w8, w8, #0x4
4008cc40: 540000cc     	b.gt	0x4008cc58 <virtio_net_init+0x258>
4008cc44: 14000001     	b	0x4008cc48 <virtio_net_init+0x248>
4008cc48: b0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008cc4c: 91211400     	add	x0, x0, #0x845
4008cc50: 97ffdee4     	bl	0x400847e0 <uart_puts>
4008cc54: 14000001     	b	0x4008cc58 <virtio_net_init+0x258>
4008cc58: 14000001     	b	0x4008cc5c <virtio_net_init+0x25c>
4008cc5c: b9401fe8     	ldr	w8, [sp, #0x1c]
4008cc60: 11000508     	add	w8, w8, #0x1
4008cc64: b9001fe8     	str	w8, [sp, #0x1c]
4008cc68: 17ffffea     	b	0x4008cc10 <virtio_net_init+0x210>
4008cc6c: b0000000     	adrp	x0, 0x4008d000 <virtio_net_send+0x90>
4008cc70: 9122c000     	add	x0, x0, #0x8b0
4008cc74: 97ffdedb     	bl	0x400847e0 <uart_puts>
4008cc78: 52800500     	mov	w0, #0x28               // =40
4008cc7c: 52820001     	mov	w1, #0x1000             // =4096
4008cc80: 94000048     	bl	0x4008cda0 <reg_write32>
4008cc84: 52800600     	mov	w0, #0x30               // =48
4008cc88: 2a1f03e1     	mov	w1, wzr
4008cc8c: 94000045     	bl	0x4008cda0 <reg_write32>
4008cc90: 52800680     	mov	w0, #0x34               // =52
4008cc94: 9400004f     	bl	0x4008cdd0 <reg_read32>
4008cc98: b9001be0     	str	w0, [sp, #0x18]
4008cc9c: b9401be8     	ldr	w8, [sp, #0x18]
4008cca0: 340000c8     	cbz	w8, 0x4008ccb8 <virtio_net_init+0x2b8>
4008cca4: 14000001     	b	0x4008cca8 <virtio_net_init+0x2a8>
4008cca8: b9401be8     	ldr	w8, [sp, #0x18]
4008ccac: 71003d08     	subs	w8, w8, #0xf
4008ccb0: 540000a8     	b.hi	0x4008ccc4 <virtio_net_init+0x2c4>
4008ccb4: 14000001     	b	0x4008ccb8 <virtio_net_init+0x2b8>
4008ccb8: 12800008     	mov	w8, #-0x1               // =-1
4008ccbc: b81fc3a8     	stur	w8, [x29, #-0x4]
4008ccc0: 14000033     	b	0x4008cd8c <virtio_net_init+0x38c>
4008ccc4: 52800700     	mov	w0, #0x38               // =56
4008ccc8: 52800201     	mov	w1, #0x10               // =16
4008cccc: 94000035     	bl	0x4008cda0 <reg_write32>
4008ccd0: 52800780     	mov	w0, #0x3c               // =60
4008ccd4: 52820001     	mov	w1, #0x1000             // =4096
4008ccd8: 94000032     	bl	0x4008cda0 <reg_write32>
4008ccdc: 90004448     	adrp	x8, 0x40914000 <rx_vq>
4008cce0: 91000108     	add	x8, x8, #0x0
4008cce4: d34cfd08     	lsr	x8, x8, #12
4008cce8: 2a0803e1     	mov	w1, w8
4008ccec: 52800800     	mov	w0, #0x40               // =64
4008ccf0: 9400002c     	bl	0x4008cda0 <reg_write32>
4008ccf4: 52800600     	mov	w0, #0x30               // =48
4008ccf8: 52800021     	mov	w1, #0x1                // =1
4008ccfc: 94000029     	bl	0x4008cda0 <reg_write32>
4008cd00: 52800680     	mov	w0, #0x34               // =52
4008cd04: 94000033     	bl	0x4008cdd0 <reg_read32>
4008cd08: b9001be0     	str	w0, [sp, #0x18]
4008cd0c: b9401be8     	ldr	w8, [sp, #0x18]
4008cd10: 340000c8     	cbz	w8, 0x4008cd28 <virtio_net_init+0x328>
4008cd14: 14000001     	b	0x4008cd18 <virtio_net_init+0x318>
4008cd18: b9401be8     	ldr	w8, [sp, #0x18]
4008cd1c: 71003d08     	subs	w8, w8, #0xf
4008cd20: 540000a8     	b.hi	0x4008cd34 <virtio_net_init+0x334>
4008cd24: 14000001     	b	0x4008cd28 <virtio_net_init+0x328>
4008cd28: 12800008     	mov	w8, #-0x1               // =-1
4008cd2c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008cd30: 14000017     	b	0x4008cd8c <virtio_net_init+0x38c>
4008cd34: 52800700     	mov	w0, #0x38               // =56
4008cd38: 52800201     	mov	w1, #0x10               // =16
4008cd3c: 94000019     	bl	0x4008cda0 <reg_write32>
4008cd40: 52800780     	mov	w0, #0x3c               // =60
4008cd44: 52820001     	mov	w1, #0x1000             // =4096
4008cd48: 94000016     	bl	0x4008cda0 <reg_write32>
4008cd4c: d0004448     	adrp	x8, 0x40916000 <tx_vq>
4008cd50: 91000108     	add	x8, x8, #0x0
4008cd54: d34cfd08     	lsr	x8, x8, #12
4008cd58: 2a0803e1     	mov	w1, w8
4008cd5c: 52800800     	mov	w0, #0x40               // =64
4008cd60: 94000010     	bl	0x4008cda0 <reg_write32>
4008cd64: b85e43a8     	ldur	w8, [x29, #-0x1c]
4008cd68: 321e0108     	orr	w8, w8, #0x4
4008cd6c: b81e43a8     	stur	w8, [x29, #-0x1c]
4008cd70: b85e43a1     	ldur	w1, [x29, #-0x1c]
4008cd74: 52800e00     	mov	w0, #0x70               // =112
4008cd78: 9400000a     	bl	0x4008cda0 <reg_write32>
4008cd7c: 94000025     	bl	0x4008ce10 <fill_rx_descriptors>
4008cd80: 2a1f03e8     	mov	w8, wzr
4008cd84: b81fc3a8     	stur	w8, [x29, #-0x4]
4008cd88: 14000001     	b	0x4008cd8c <virtio_net_init+0x38c>
4008cd8c: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008cd90: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008cd94: 910143ff     	add	sp, sp, #0x50
4008cd98: d65f03c0     	ret
4008cd9c: d503201f     	nop

000000004008cda0 <reg_write32>:
4008cda0: d10043ff     	sub	sp, sp, #0x10
4008cda4: b9000fe0     	str	w0, [sp, #0xc]
4008cda8: b9000be1     	str	w1, [sp, #0x8]
4008cdac: b9400be8     	ldr	w8, [sp, #0x8]
4008cdb0: f0004429     	adrp	x9, 0x40913000 <local_mac>
4008cdb4: f9400929     	ldr	x9, [x9, #0x10]
4008cdb8: b9400fea     	ldr	w10, [sp, #0xc]
4008cdbc: b82a6928     	str	w8, [x9, x10]
4008cdc0: 910043ff     	add	sp, sp, #0x10
4008cdc4: d65f03c0     	ret
4008cdc8: d503201f     	nop
4008cdcc: d503201f     	nop

000000004008cdd0 <reg_read32>:
4008cdd0: d10043ff     	sub	sp, sp, #0x10
4008cdd4: b9000fe0     	str	w0, [sp, #0xc]
4008cdd8: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cddc: f9400908     	ldr	x8, [x8, #0x10]
4008cde0: b9400fe9     	ldr	w9, [sp, #0xc]
4008cde4: b8696900     	ldr	w0, [x8, x9]
4008cde8: 910043ff     	add	sp, sp, #0x10
4008cdec: d65f03c0     	ret

000000004008cdf0 <reg_read8>:
4008cdf0: d10043ff     	sub	sp, sp, #0x10
4008cdf4: b9000fe0     	str	w0, [sp, #0xc]
4008cdf8: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cdfc: f9400908     	ldr	x8, [x8, #0x10]
4008ce00: b9400fe9     	ldr	w9, [sp, #0xc]
4008ce04: 38696900     	ldrb	w0, [x8, x9]
4008ce08: 910043ff     	add	sp, sp, #0x10
4008ce0c: d65f03c0     	ret

000000004008ce10 <fill_rx_descriptors>:
4008ce10: d100c3ff     	sub	sp, sp, #0x30
4008ce14: a9027bfd     	stp	x29, x30, [sp, #0x20]
4008ce18: 910083fd     	add	x29, sp, #0x20
4008ce1c: 2a1f03e8     	mov	w8, wzr
4008ce20: b81fc3a8     	stur	w8, [x29, #-0x4]
4008ce24: 14000001     	b	0x4008ce28 <fill_rx_descriptors+0x18>
4008ce28: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008ce2c: 71001d08     	subs	w8, w8, #0x7
4008ce30: 5400082c     	b.gt	0x4008cf34 <fill_rx_descriptors+0x124>
4008ce34: 14000001     	b	0x4008ce38 <fill_rx_descriptors+0x28>
4008ce38: b89fc3a8     	ldursw	x8, [x29, #-0x4]
4008ce3c: 2a0803e9     	mov	w9, w8
4008ce40: 52800148     	mov	w8, #0xa                // =10
4008ce44: 2a0803e0     	mov	w0, w8
4008ce48: 2a0003ea     	mov	w10, w0
4008ce4c: b000448b     	adrp	x11, 0x4091d000 <rx_hdrs>
4008ce50: 9100016b     	add	x11, x11, #0x0
4008ce54: 9b2a2d2a     	smaddl	x10, w9, w10, x11
4008ce58: 531f792b     	lsl	w11, w9, #1
4008ce5c: 2a0b03e9     	mov	w9, w11
4008ce60: 937c7d2b     	sbfiz	x11, x9, #4, #32
4008ce64: 90004449     	adrp	x9, 0x40914000 <rx_vq>
4008ce68: 91000129     	add	x9, x9, #0x0
4008ce6c: f9000be9     	str	x9, [sp, #0x10]
4008ce70: f82b692a     	str	x10, [x9, x11]
4008ce74: b85fc3aa     	ldur	w10, [x29, #-0x4]
4008ce78: 531f794a     	lsl	w10, w10, #1
4008ce7c: 8b2ad12a     	add	x10, x9, w10, sxtw #4
4008ce80: b9000948     	str	w8, [x10, #0x8]
4008ce84: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008ce88: 531f7908     	lsl	w8, w8, #1
4008ce8c: 8b28d12a     	add	x10, x9, w8, sxtw #4
4008ce90: 52800068     	mov	w8, #0x3                // =3
4008ce94: 79001948     	strh	w8, [x10, #0xc]
4008ce98: b85fc3ab     	ldur	w11, [x29, #-0x4]
4008ce9c: 531f796a     	lsl	w10, w11, #1
4008cea0: 52800028     	mov	w8, #0x1                // =1
4008cea4: 331f7968     	bfi	w8, w11, #1, #31
4008cea8: 8b2ad12a     	add	x10, x9, w10, sxtw #4
4008ceac: 79001d48     	strh	w8, [x10, #0xe]
4008ceb0: b89fc3ab     	ldursw	x11, [x29, #-0x4]
4008ceb4: 2a0b03ea     	mov	w10, w11
4008ceb8: b0004468     	adrp	x8, 0x40919000 <rx_buffers>
4008cebc: 91000108     	add	x8, x8, #0x0
4008cec0: 8b0b2d08     	add	x8, x8, x11, lsl #11
4008cec4: 531f794a     	lsl	w10, w10, #1
4008cec8: 8b2ad12a     	add	x10, x9, w10, sxtw #4
4008cecc: f9000948     	str	x8, [x10, #0x10]
4008ced0: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008ced4: 531f7908     	lsl	w8, w8, #1
4008ced8: 8b28d12a     	add	x10, x9, w8, sxtw #4
4008cedc: 52810008     	mov	w8, #0x800              // =2048
4008cee0: b9001948     	str	w8, [x10, #0x18]
4008cee4: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008cee8: 531f7908     	lsl	w8, w8, #1
4008ceec: 8b28d12a     	add	x10, x9, w8, sxtw #4
4008cef0: 52800048     	mov	w8, #0x2                // =2
4008cef4: 79003948     	strh	w8, [x10, #0x1c]
4008cef8: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008cefc: 531f7908     	lsl	w8, w8, #1
4008cf00: 8b28d12a     	add	x10, x9, w8, sxtw #4
4008cf04: 2a1f03e8     	mov	w8, wzr
4008cf08: 79003d48     	strh	w8, [x10, #0x1e]
4008cf0c: b89fc3aa     	ldursw	x10, [x29, #-0x4]
4008cf10: 2a0a03e8     	mov	w8, w10
4008cf14: 531f7908     	lsl	w8, w8, #1
4008cf18: 8b0a0529     	add	x9, x9, x10, lsl #1
4008cf1c: 79020928     	strh	w8, [x9, #0x104]
4008cf20: 14000001     	b	0x4008cf24 <fill_rx_descriptors+0x114>
4008cf24: b85fc3a8     	ldur	w8, [x29, #-0x4]
4008cf28: 11000508     	add	w8, w8, #0x1
4008cf2c: b81fc3a8     	stur	w8, [x29, #-0x4]
4008cf30: 17ffffbe     	b	0x4008ce28 <fill_rx_descriptors+0x18>
4008cf34: d5033fbf     	dmb	sy
4008cf38: 90004449     	adrp	x9, 0x40914000 <rx_vq>
4008cf3c: 52800108     	mov	w8, #0x8                // =8
4008cf40: 79020528     	strh	w8, [x9, #0x102]
4008cf44: d5033fbf     	dmb	sy
4008cf48: 52800600     	mov	w0, #0x30               // =48
4008cf4c: 2a1f03e1     	mov	w1, wzr
4008cf50: b9000fe1     	str	w1, [sp, #0xc]
4008cf54: 97ffff93     	bl	0x4008cda0 <reg_write32>
4008cf58: b9400fe1     	ldr	w1, [sp, #0xc]
4008cf5c: 52800a00     	mov	w0, #0x50               // =80
4008cf60: 97ffff90     	bl	0x4008cda0 <reg_write32>
4008cf64: a9427bfd     	ldp	x29, x30, [sp, #0x20]
4008cf68: 9100c3ff     	add	sp, sp, #0x30
4008cf6c: d65f03c0     	ret

000000004008cf70 <virtio_net_send>:
4008cf70: d10143ff     	sub	sp, sp, #0x50
4008cf74: a9047bfd     	stp	x29, x30, [sp, #0x40]
4008cf78: 910103fd     	add	x29, sp, #0x40
4008cf7c: f81f03a0     	stur	x0, [x29, #-0x10]
4008cf80: b81ec3a1     	stur	w1, [x29, #-0x14]
4008cf84: f0004420     	adrp	x0, 0x40913000 <local_mac>
4008cf88: 91003000     	add	x0, x0, #0xc
4008cf8c: 97ffdd9d     	bl	0x40084600 <spinlock_acquire_irqsave>
4008cf90: f90013e0     	str	x0, [sp, #0x20]
4008cf94: f0004428     	adrp	x8, 0x40913000 <local_mac>
4008cf98: f9400908     	ldr	x8, [x8, #0x10]
4008cf9c: b5000128     	cbnz	x8, 0x4008cfc0 <virtio_net_send+0x50>
4008cfa0: 14000001     	b	0x4008cfa4 <virtio_net_send+0x34>
4008cfa4: f94013e1     	ldr	x1, [sp, #0x20]
4008cfa8: f0004420     	adrp	x0, 0x40913000 <local_mac>
4008cfac: 91003000     	add	x0, x0, #0xc
4008cfb0: 97ffdda4     	bl	0x40084640 <spinlock_release_irqrestore>
4008cfb4: 12800008     	mov	w8, #-0x1               // =-1
4008cfb8: b81fc3a8     	stur	w8, [x29, #-0x4]
4008cfbc: 1400006d     	b	0x4008d170 <virtio_net_send+0x200>
4008cfc0: 2a1f03e8     	mov	w8, wzr
4008cfc4: b9001fe8     	str	w8, [sp, #0x1c]
4008cfc8: 14000001     	b	0x4008cfcc <virtio_net_send+0x5c>
4008cfcc: b9801fe8     	ldrsw	x8, [sp, #0x1c]
4008cfd0: f1002508     	subs	x8, x8, #0x9
4008cfd4: 54000188     	b.hi	0x4008d004 <virtio_net_send+0x94>
4008cfd8: 14000001     	b	0x4008cfdc <virtio_net_send+0x6c>
4008cfdc: b9801fea     	ldrsw	x10, [sp, #0x1c]
4008cfe0: 90004469     	adrp	x9, 0x40918000 <tx_hdr>
4008cfe4: 91000129     	add	x9, x9, #0x0
4008cfe8: 2a1f03e8     	mov	w8, wzr
4008cfec: 382a6928     	strb	w8, [x9, x10]
4008cff0: 14000001     	b	0x4008cff4 <virtio_net_send+0x84>
4008cff4: b9401fe8     	ldr	w8, [sp, #0x1c]
4008cff8: 11000508     	add	w8, w8, #0x1
4008cffc: b9001fe8     	str	w8, [sp, #0x1c]
4008d000: 17fffff3     	b	0x4008cfcc <virtio_net_send+0x5c>
4008d004: b0004449     	adrp	x9, 0x40916000 <tx_vq>
4008d008: 91000129     	add	x9, x9, #0x0
4008d00c: f90007e9     	str	x9, [sp, #0x8]
4008d010: 79420528     	ldrh	w8, [x9, #0x102]
4008d014: 531f0908     	ubfiz	w8, w8, #1, #3
4008d018: 790037e8     	strh	w8, [sp, #0x1a]
4008d01c: 794037e8     	ldrh	w8, [sp, #0x1a]
4008d020: d37ced0a     	lsl	x10, x8, #4
4008d024: f0004448     	adrp	x8, 0x40918000 <tx_hdr>
4008d028: 91000108     	add	x8, x8, #0x0
4008d02c: f82a6928     	str	x8, [x9, x10]
4008d030: 794037e8     	ldrh	w8, [sp, #0x1a]
4008d034: 8b08112a     	add	x10, x9, x8, lsl #4
4008d038: 52800148     	mov	w8, #0xa                // =10
4008d03c: b9000948     	str	w8, [x10, #0x8]
4008d040: 794037e8     	ldrh	w8, [sp, #0x1a]
4008d044: 8b081128     	add	x8, x9, x8, lsl #4
4008d048: 52800021     	mov	w1, #0x1                // =1
4008d04c: b90017e1     	str	w1, [sp, #0x14]
4008d050: 79001901     	strh	w1, [x8, #0xc]
4008d054: 794037ea     	ldrh	w10, [sp, #0x1a]
4008d058: 11000548     	add	w8, w10, #0x1
4008d05c: 2a0a03e0     	mov	w0, w10
4008d060: 2a0003ea     	mov	w10, w0
4008d064: 8b2a312a     	add	x10, x9, w10, uxth #4
4008d068: 79001d48     	strh	w8, [x10, #0xe]
4008d06c: f85f03a8     	ldur	x8, [x29, #-0x10]
4008d070: 794037ea     	ldrh	w10, [sp, #0x1a]
4008d074: 1100054b     	add	w11, w10, #0x1
4008d078: 2a0b03ea     	mov	w10, w11
4008d07c: d37c7d4a     	ubfiz	x10, x10, #4, #32
4008d080: f82a6928     	str	x8, [x9, x10]
4008d084: b85ec3a8     	ldur	w8, [x29, #-0x14]
4008d088: 794037ea     	ldrh	w10, [sp, #0x1a]
4008d08c: 1100054a     	add	w10, w10, #0x1
4008d090: 8b2a512a     	add	x10, x9, w10, uxtw #4
4008d094: b9000948     	str	w8, [x10, #0x8]
4008d098: 794037e8     	ldrh	w8, [sp, #0x1a]
4008d09c: 11000508     	add	w8, w8, #0x1
4008d0a0: 8b28512a     	add	x10, x9, w8, uxtw #4
4008d0a4: 2a1f03e8     	mov	w8, wzr
4008d0a8: 79001948     	strh	w8, [x10, #0xc]
4008d0ac: 794037ea     	ldrh	w10, [sp, #0x1a]
4008d0b0: 1100054a     	add	w10, w10, #0x1
4008d0b4: 8b2a512a     	add	x10, x9, w10, uxtw #4
4008d0b8: 79001d48     	strh	w8, [x10, #0xe]
4008d0bc: 79420528     	ldrh	w8, [x9, #0x102]
4008d0c0: 12000d08     	and	w8, w8, #0xf
4008d0c4: 8b28452a     	add	x10, x9, w8, uxtw #1
4008d0c8: 794037e8     	ldrh	w8, [sp, #0x1a]
4008d0cc: 79020948     	strh	w8, [x10, #0x104]
4008d0d0: d5033fbf     	dmb	sy
4008d0d4: 79420528     	ldrh	w8, [x9, #0x102]
4008d0d8: 11000508     	add	w8, w8, #0x1
4008d0dc: 79020528     	strh	w8, [x9, #0x102]
4008d0e0: d5033fbf     	dmb	sy
4008d0e4: 52800600     	mov	w0, #0x30               // =48
4008d0e8: 97ffff2e     	bl	0x4008cda0 <reg_write32>
4008d0ec: b94017e1     	ldr	w1, [sp, #0x14]
4008d0f0: 52800a00     	mov	w0, #0x50               // =80
4008d0f4: 97ffff2b     	bl	0x4008cda0 <reg_write32>
4008d0f8: 14000001     	b	0x4008d0fc <virtio_net_send+0x18c>
4008d0fc: d0004448     	adrp	x8, 0x40917000 <tx_vq+0x1000>
4008d100: 79400508     	ldrh	w8, [x8, #0x2]
4008d104: f0004449     	adrp	x9, 0x40918000 <tx_hdr>
4008d108: 79401529     	ldrh	w9, [x9, #0xa]
4008d10c: 6b090108     	subs	w8, w8, w9
4008d110: 54000181     	b.ne	0x4008d140 <virtio_net_send+0x1d0>
4008d114: 14000001     	b	0x4008d118 <virtio_net_send+0x1a8>
4008d118: f94013e1     	ldr	x1, [sp, #0x20]
4008d11c: d0004420     	adrp	x0, 0x40913000 <local_mac>
4008d120: 91003000     	add	x0, x0, #0xc
4008d124: f90003e0     	str	x0, [sp]
4008d128: 97ffdd46     	bl	0x40084640 <spinlock_release_irqrestore>
4008d12c: f94003e0     	ldr	x0, [sp]
4008d130: d503207f     	wfi
4008d134: 97ffdd33     	bl	0x40084600 <spinlock_acquire_irqsave>
4008d138: f90013e0     	str	x0, [sp, #0x20]
4008d13c: 17fffff0     	b	0x4008d0fc <virtio_net_send+0x18c>
4008d140: d0004448     	adrp	x8, 0x40917000 <tx_vq+0x1000>
4008d144: 79400508     	ldrh	w8, [x8, #0x2]
4008d148: f0004449     	adrp	x9, 0x40918000 <tx_hdr>
4008d14c: 79001528     	strh	w8, [x9, #0xa]
4008d150: d5033fbf     	dmb	sy
4008d154: f94013e1     	ldr	x1, [sp, #0x20]
4008d158: d0004420     	adrp	x0, 0x40913000 <local_mac>
4008d15c: 91003000     	add	x0, x0, #0xc
4008d160: 97ffdd38     	bl	0x40084640 <spinlock_release_irqrestore>
4008d164: 2a1f03e8     	mov	w8, wzr
4008d168: b81fc3a8     	stur	w8, [x29, #-0x4]
4008d16c: 14000001     	b	0x4008d170 <virtio_net_send+0x200>
4008d170: b85fc3a0     	ldur	w0, [x29, #-0x4]
4008d174: a9447bfd     	ldp	x29, x30, [sp, #0x40]
4008d178: 910143ff     	add	sp, sp, #0x50
4008d17c: d65f03c0     	ret

000000004008d180 <virtio_net_handle_irq>:
