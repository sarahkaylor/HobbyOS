# l8-netfix — on-device evidence

All lines below are RAW serial/serial-log markers from QEMU guest boots of the
lane kernel (MODE=webproc single-program, WEBPROC.BIN = `obj/arm/netfix.bin`
from `src/user/netfix_test.c`), against the host echo server
(`netfix_server.py`, port 8765 over QEMU user networking at 10.0.2.2), plus a
QEMU `filter-dump` packet capture of the guest NIC.

## Bug #2 (RX leading NULs) — before vs after

Before (net_rx_packet used the padded frame length), every trial FAILed.
`NETFIX RX total=175` — the trailing 12 = 6 NULs prepended by the padded ACK
+ 6 NULs from the padded FIN (Ethernet 60-byte minimum padding parsed as TCP
payload):

```
.NETFIX RX total=175
.NETFIX hex[48]: 00 00 00 00 00 00 4E 45 54 46 49 58 2D 53 52 56 2D 4F 4B 0D 0A ...
.NETFIX RX-FAIL: recv prepended 6 NUL byte(s) before the server response (trial 1)
...
.NETFIX RX-FAIL: recv prepended 6 NUL byte(s) before the server response (trial 8)
.NETFIX RESULTS: 8 FAILURES
```

After (L4 length from IPv4 total_len), every trial PASSed — the stream starts
clean at byte 0 and is 163 B (the real response; the two 6-byte pads are gone):

```
.NETFIX RX total=163
.NETFIX hex[48]: 4E 45 54 46 49 58 2D 53 52 56 2D 4F 4B 0D 0A 72 78 6C 65 6E 3D 34 39 0D 0A ...
.NETFIX RX-OK: response starts clean at byte 0 (trial 1)
...
24 x NETFIX TX-OK
24 x NETFIX RX-OK
.NETFIX RESULTS: ALL PASS
```

Wire capture (`run-wire-capture.pcap`, decoded with tcpdump) — the producer
pads every header-only segment to 60 bytes; the guest must not parse the pad:

```
17:23:28.999321 52:55:0a:00:02:02 > 52:54:00:12:34:56, ethertype IPv4 (0x0800), length 217: 10.0.2.2.8765 > 10.0.2.15.49154: Flags [P.], seq 1:164, ..., length 163
17:23:28.999472 52:55:0a:00:02:02 > 52:54:00:12:34:56, ethertype IPv4 (0x0800), length 60:  10.0.2.2.8765 > 10.0.2.15.49154: Flags [F.], seq 164, ack 50, ..., length 0
```

Per-segment kernel trace (temporary NETFIX_DIAG build) showing the SAME seq
(0xFA02) on both the 6-NUL segment and the real response, and used-ring
lengths `len 70` (10 + 60-padded) vs `len 227` (10 + 217 real):

```
[NETDBG] arm-rx used 4 id 8  len 70  b 4 ...
[NETDBG] tcp state=2 seq=0x000000000000FA02 ack=0x000000000000041A do=20 len=26 pay=0000000000 00 00 00 00 00
[NETDBG] arm-rx used 5 id 10 len 227 b 5 ...
[NETDBG] tcp state=2 seq=0x000000000000FA02 ack=0x000000000000041A do=20 len=183 pay=4E 45 54 46 49 58 2D 53 52 56 2D 4F 4B
[NETDBG] arm-rx used 6 id 12 len 70  b 6 ...
[NETDBG] tcp state=2 seq=0x000000000000FAA5 ack=0x000000000000041A do=20 len=26 pay=0000000000 00 00 00 00 00
```

## Bug #1 (TX/console contamination) — structural fix + byte-exact proof

Every request the guest sent reached the host echo server byte-exact under a
console flood (the server hex-records the FIRST bytes of each connection in
`run-after-fix-server/rx-NNN.txt`, all `NETFIX-REQ ...`):

```
-- run-after-fix-server/rx-000.txt: NETFIX-REQ 001 BCDEFGHIJKLMNOPQRSTUVWXYZABCDEFG..
-- run-after-fix-server/rx-001.txt: NETFIX-REQ 002 CDEFGHIJKLMNOPQRSTUVWXYZABCDEFGH..
...
24 x NETFIX TX-OK
0 x NETFIX TX-FAIL
```

Plus the guest-side self-verification (`rxhex=` echoed by the server decodes
to exactly what the guest sent): `NETFIX TX-OK` 24/24 above.

## Bug #3 (serial drops) — measured, not reproduced

The OS console path is lossless under heavy load on this stack: 24 trials x
(20 pre + 10 post) flood lines = 720 expected; the serial log contains all
of them plus every verdict:

```
$ grep -c 'NETFIX console flood line' run-after-fix-serial.log   # 480
$ grep -c 'NETFIX console drain line'  run-after-fix-serial.log  # 240
$ grep -c 'NETFIX TX connected'        run-after-fix-serial.log  # 24
$ grep -c 'NETFIX RX total'            run-after-fix-serial.log  # 24
```
