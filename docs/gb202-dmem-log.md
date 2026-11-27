# GB202 FSP DMEM log — the download-log command is not answered by this FSP (1 boot)

Card: `t_2e98aaee`. Part: GB202 `41:00.0`, VM 313. Boot tag: **rm66**
(`5090-gb202fsp-rm66.log`, sha256 `9203bee3…`, 613 lines). Mode 66.

## 0. The answer, in six lines

**Acceptance option 2.** The FSP's own DMEM log cannot be read in-band on this
part *because the FSP never answers the download-log command*, and that is
measured with the channel proven alive either side of it in the same boot:

1. the COT is authenticated and answered (`cot answer errorCode=0 (FSP_OK)`),
   and the 615.78.08 FMC writes its terminal code inside that round trip:
   `MAILBOX0 = 0x824`, `0x14DC = 0x12000824`;
2. **1 s after** that code, a clock-boost NVDM query on the same EMEM channel
   is answered at the first poll (`prelog-boost latency_us=0`, `errorCode=0`);
3. the download-log request is written exactly as the vendor builds it
   (4 dwords `C1000000 0016477F 06018000 0000FF01`, `tail=0xC`) and **nothing
   arrives in 200 ms**: `MSGQ_HEAD = MSGQ_TAIL = 0` at every poll;
4. a second clock-boost query **after** it is again answered at `latency_us=0`
   (`postlog boost_errorCode=0`) — so the mailbox is not dead and the answer
   is not sitting unread in a stuck queue;
5. NVIDIA's own code says this is what an older FSP does: “Older FSP versions
   do not support DMEM logging and will ignore the request causing a timeout
   here” (`620.73.04 kern_fsp_gb100.c:403-407`);
6. this part's FSP is **v4.76** (`ucode=0x44C`, `[NV-PFSP]` row) and is started
   by BootFSM out of chip reset — no tag ships an FSP image, so there is no
   newer FSP to ask (survey §9, `kern_fsp_gb202.c`).

There is **no DMEM record to decode**, so option 1 is *not* met and no name for
`0x824` is claimed. The dump captured is 0 bytes precisely because the FSP sent
0 bytes; the refusal — not a decode failure — is the measurement.

## 1. What was built

The command is `_kfspGatherDmemLog_GB100`, and it is **not an NVDM message**.
The vendor builds `MctpDownloadLogV1` by hand and calls `kfspSendPacket_HAL`
directly (`kern_fsp_gb100.c:357-433`), so the frame is an MCTP transport dword
followed by a 9-byte MCTP VDM IANA request and a session id, with no NVDM dword.
Compiled from the vendor's own `#pragma pack(1)` structs
(`dm-log-encode-check.c`, same source lines 76-113):

```
sizeof(MctpVdmIanaRequest)=9
sizeof(MctpVdmIanaResponse)=10
sizeof(MctpDownloadLogV1)=14
reply.sessionId_off=14 reply.length_off=15 reply.data_off=16
request.bytes 00 00 00 c1 7f 47 16 00 00 80 01 06 01 ff
request.dwords 0xc1000000 0x0016477f 0x06018000 0x0000ff01
```

| dword | bytes | meaning |
|---|---|---|
| `0xC1000000` | 0..3 | MCTP `SOM|EOM|SEID=0|SEQ=0|TAG=1` |
| `0x0016477F` | 4..7 | `messageType 0x7F` (vendor IANA) + `iana 0x1647` |
| `0x06018000` | 8..11 | instance 0, `rq=1`, `vendorMessageType=1`, `commandCode=6` |
| `0x0000FF01` | 12..13 | `messageVersion=1`, `sessionId=0xFF` (new session) |

14 bytes, padded to 16 by `kfspSendPacket_GH100`, so `TAIL = 12`
(`0xC`). The reply's layout, from the same function's pointer arithmetic, is
`mctp | 9-byte request echo | completionCode | sessionId | length | data[52]`;
`length==0` ends the log and the first reply carries the session id the rest of
the reads must repeat. The decoder treats the echo dword `0x0016477F` at
`resp[1]` as the shape test: an answer of any other wrapper (e.g. the NVDM
`0xFSP_RESPONSE` `0x1510DE7E` that COT/caps answers carry) is printed raw and
**not** read as log bytes.

Kernel side: `src/kernel/nv_fsp_cot.c` mode 66 = mode 63's host state (615 FMC,
terminal code `0x824`) plus `rm_dmem_log()` and two clock-boost controls. The
full diff is `nv_fsp_cot.c.mode66.diff` (`pre66` → booted source, 300 lines).

## 2. Offline validation — before the boot was spent

`validate-offline.py` (output: `offline-validation.txt`) checks the frame and
the decoder against ground truth, not against themselves:

```
PASS  vendor MctpDownloadLogV1 is 14 bytes  [sizeof(MctpDownloadLogV1)=14]
PASS  vendor request dwords  [0xc1000000 0x0016477f 0x06018000 0x0000ff01]
PASS  vendor reply offsets 14/15/16  [reply.sessionId_off=14 reply.length_off=15 reply.data_off=16]
PASS  kernel NV_FSPLOG_MCTP
PASS  kernel NV_FSPLOG_ECHO_W1
PASS  kernel NV_FSPLOG_ECHO_W2
PASS  kernel NV_FSPLOG_REQ_W3 byte13
PASS  decoder accepts the synthetic reply  [rc=0]
PASS  decoder recovers session 0x12
PASS  decoder recovers length 36
PASS  decoder recovers the payload text
PASS  decoder accepts mode-66 serial rows  [rc=0]
PASS  serial path keeps the wait row  [lat_us=1000]
PASS  real COT answer is refused (rc=2)  [rc=2]
PASS  and named as another wrapper
RESULT: all checks passed; the frame and decoder agree with the vendor
structs and refuse a real non-download-log answer.
```

The synthetic reply is built by `dm-log-make-frame.c` from the same vendor
structs and decodes back byte-for-byte; the negative control is the **real COT
answer this lane already holds** (`rm63`: `C0000000 1510DE7E 1 0x14 0`), which
the decoder refuses as another wrapper. So a timeout in the boot is not “the
host wrote a malformed frame” and not “the decoder cannot read a reply”.

## 3. The boot, and what was staged

One boot, VM 313, `tools/nv/gb202fsp-boot.sh` (sha256 `2ed863ae…`), tag `rm66`,
header rows:

```
# [NV-GB202FSP] boot 2026-11-27T19:23:42Z
# image=/home/nvcard/.hermes/kanban/boards/hobbyos-nv/workspaces/t_2e98aaee/rm66.img sha256=122f3204d22e5f95754e0bd662457c525161cda39efadb5d759a54e45367749d
# tag=rm66 vm=313 hostpci0=0000:41:00
```

Staged and hashed in the image after staging: `/boot/hobbyos.elf`
(`58db5e8e…`), `FMC615.BIN` (`ae4e7055…`, 287,884 B), `BOOT615.BIN`
(`37159b4b…`, 290,816 B), `/RM.CFG` = `66`, `/COT.CFG` = `a`. The other GSP
blobs (`GSPRM.BIN`, `GSPRM.SIG`, `FMC.BIN`, `FMCV.BIN`, `BOOTIMG.BIN`) are
byte-identical to `base.img` (the staging script refuses otherwise).

## 4. The rows (verbatim, citable by line)

The channel itself, before the COT — the page answers and the SP answers an
NVDM caps query twice:

```
203:[NV-GB202FSP] recv i=5 v=0x0000000000000020
217:[NV-GB202FSP] verdict=sp-answered consumed=1 answered=1 autoinc=1 resp_dwords=6
```

The COT is accepted and the FMC's terminal code is written inside the round trip:

```
269:[NV-FSPCOT] cot send dwords=217 tail=0x0000000000000360 qh=0x0000000000000000 hdr0=0x00000000C0000000 hdr1=0x000000001410DE7E ememc=0x0000000001000364
278:[NV-FSPCOT] cot answer errorCode=0 (FSP_OK)
279:[NV-FSPCOT] fmcerr post-cot reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
281:[NV-FSPCOT] rm t1s t_ms=1000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
```

Positive control **after the terminal code, before the log request**:

```
285:[NV-FSPCOT] prelog-boost latency_us=0
292:[NV-FSPCOT] prelog-boost class=1[NV-FSPCOT] prelog-boost cmd_nvdm=32[NV-FSPCOT] prelog-boost errorCode=0
293:dmemlog prelog boost_errorCode=0
```

The download-log request and its non-answer (the `=0x0x` double prefix is the
known `uart_print_hex` quirk of these `p("=0x")` rows; quoted verbatim):

```
295:[NV-FSPCOT] dmemlog req send dwords=4 tail=0x000000000000000C[NV-FSPCOT]  w0[NV-FSPCOT] =0x0x00000000C1000000[NV-FSPCOT]  w1[NV-FSPCOT] =0x0x000000000016477F[NV-FSPCOT]  w2[NV-FSPCOT] =0x0x0000000006018000[NV-FSPCOT]  w3[NV-FSPCOT] =0x0x000000000000FF01 qh=0x0000000000000000
296:[NV-FSPCOT] dmemlog wait pkt=0 lat_us=-1 req_sid=0x00000000000000FF msgq_head=0x0000000000000000 msgq_tail=0x0000000000000000
297:[NV-FSPCOT] dmemlog status=no-response (nothing arrived in 200000 us)
298:[NV-FSPCOT] dmemlog done pkts=0 bytes=0[NV-FSPCOT]  session=0x0x00000000000000FF
```

Positive control **after** the log request, same channel, same boot:

```
301:[NV-FSPCOT] postlog-boost latency_us=0
309:dmemlog postlog boost_errorCode=0
```

And the terminal state is unchanged either side of the attempt:

```
299:[NV-FSPCOT] fmcerr postlog reg=0x14dc=0x0000000012000824 alias_0x4dc=0x00000000BADF5040 variant=0x0000000000000001 partition=0x0000000000000001 err=0x0000000000000824 info=0x0000000000000000
310:[NV-FSPCOT] rm postlog t_ms=2000 hwcfg2=0x000000008187A7F7 m0=0x0000000000000824 m1=0x0000000000000000 os=0x0000000000000000 rm=0x0000000000000000 rvctl=0x00000000BADF5620 gmb0=0x0000000000000000 s2_0=0x000000000000009F s2_1=0x0000000000000000 s2_2=0x0000000000000000 s2_3=0x000000000000000A mb1=0x0000000000000000 mb2=0x0000000000000000 mb3=0x0000000000000000 i2cs=0x00000000000000FF wpr2_lo=0x000000007FFFFE00 wpr2_hi=0x0000000000000000 fmcerr=0x0000000012000824
```

Verdict:

```
358:[NV-FSPRM] verdict=fmc-or-rm-window-open mode=66 cot_error=0 lock_released=1 statq_written=0 linked=0 initdone=0 hwcfg2_last=0x000000008187A7F7 m0_last=0x0000000000000824 m1_last=0x0000000000000000 os_last=0x0000000000000000 rm_last=0x0000000000000000 fmcerr_last=0x0000000012000824 radix3=1
```

## 5. How the rows are read

- **The channel works.** The same 6-byte EMEM transport answered an NVDM
  clock-boost query at `latency_us=0` (i.e. at the first poll) 1 s after
  `m0=0x824`, and again at `latency_us=0` after the download-log attempt. The
  COT on the same channel was answered `errorCode=0` minutes earlier. So
  `latency_us=-1` for the download-log request is a statement about **that
  command**, not about the mailbox, the doorbell, or the interrupt path.
- **The queue was not stuck.** `msgq_head=0` and `msgq_tail=0` in the wait row:
  the SP had not moved `MSGQ_TAIL` at all, and the two later controls ack and
  drain normally, so there is no unread answer left behind.
- **The request is the vendor's, not this file's invention.** Its four dwords
  are emitted by the vendor's own packed struct compiled on the host, and the
  kernel's constants are asserted equal to them (`validate-offline.py`). The
  one field no second source can confirm is the MCTP tag (`TAG=1`, the vendor's
  constant); a dispatcher that ignores an unimplemented VDM does not read the
  tag, but say so rather than claim more.
- **This is the documented behaviour.** `kern_fsp_gb100.c:399-407`:

  ```
      while(responseSize + sizeof(pResponse->data) < maxLogSize)
      {
          NV_ASSERT_OK_OR_RETURN(kfspSendPacket_HAL(pGpu, pKernelFsp, (NvU8*)&logRequest, sizeof(logRequest)));
          //
          // Older FSP versions do not support DMEM logging and will ignore
          // the request causing a timeout here.
          //
          if (kfspPollForResponse(pGpu, pKernelFsp, DMEM_RESPONSE_TIMEOUT_US) != NV_OK)
              return NV_ERR_NOT_SUPPORTED;
  ```

  with `#define DMEM_RESPONSE_TIMEOUT_US (5000)` at line 74. This port waits
  **200 000 µs** (40× the vendor) so a slow answer would not be lost; nothing
  arrived, and the boot's own COT answer took 100 ms on this channel, which is
  why the longer window was used at all.
- **The log was asked for after the abort, not before.** The request is one
  second after `rm t1s … m0=0x824`, and `0x14DC` still reads `0x12000824`
  immediately before and after it. If a log had come back it would have been
  the terminal-state log; it did not come back.
- **The FSP cannot be replaced to ask a newer one.** `[NV-PFSP]` line 138 gives
  `ucode=0x44C version=4.76`; `kern_fsp_gb202.c` says GB202's BootFSM starts
  the FSP out of chip reset, and survey §9 measured that no tag ships an FSP
  image. There is no host-side lever here, and none was attempted.

## 6. What this does **not** say

- It does **not** name `0x824` (or `0x0B`). No DMEM record exists to name it.
- It does **not** say a DMEM log does not exist in the FSP; it says **this FSP
  build does not answer the vendor's download-log command**, so the log is not
  reachable in-band by this path.
- It does **not** prove the same for the `0x0B` (570/580-FMC) state: the FSP is
  the same object and the channel answered repeated COTs there in earlier
  rounds, but this card measured the refusal in the `0x824` state only. That is
  an extrapolation and is labelled as one.
- The CMS2 window stays what `t_a769beaa` measured: static, textless, 48 dwords
  identical pre/post. It was not re-read here; the download-log path was the
  only untried namer and it is now measured.

## 7. Acceptance

| option | status |
|---|---|
| 1. a decoded DMEM-log record naming the abort | **not met** — the FSP returned no packet (`pkts=0 bytes=0`) |
| 2. a demonstrated reason the log cannot be read in-band | **met** — the vendor's download-log command is written correctly (`295`), gets no answer in 200 ms (`296`/`297`), while the same channel answers clock-boost before (`285`/`292`) and after (`301`/`309`) at `latency_us=0`, and the vendor documents exactly this timeout for older FSPs |

One boot spent, one hypothesis (`mode 66`), VM 313, GB202 `41:00.0`. Closing
state: `qm status 313` = stopped, `qm status 300` = stopped,
`pgrep -c qemu-system-x86` = 0.

## 8. Reproduce the decode from the dump

The dump is the serial log; the extractor is deterministic:

```
$ python3 gb202-dmem-log-decode.py --from-serial 5090-gb202fsp-rm66.log --write-bin rm66-dmem.bin
# source: 5090-gb202fsp-rm66.log
# packets: 0
# wait pkt=0 lat_us=-1 req_sid=0xff msgq_head=0x0 msgq_tail=0x0  <-- NO ANSWER
# status=no-response

No download-log packet rows in the input.
A log the FSP never sent is a measurement, not a decode failure:
quote the wait/status rows above as the refusal.
# wrote rm66-dmem.bin (0 bytes: no log bytes were returned)
$ echo $?
2
$ ls -l rm66-dmem.bin
-rw-rw-r-- 1 nvcard nvcard 0 Nov 27 19:24 rm66-dmem.bin
```

The same decoder **does** decode a reply built from the vendor structs
(`dm-log-make-frame 0x12 "GSP-FMC: abort 0x824 at wpr2 stage 7"` →
`session=0x12 length=36`, text recovered), and **refuses** the real `rm63` COT
answer — both in `offline-validation.txt` and `rm66-decode.txt`.

## 9. Files

| file | sha256 |
|---|---|
| `5090-gb202fsp-rm66.log` | `9203bee396a64571fa06c1831b60d604249eb85aeab40080c87e4b982aeb8a39` |
| `rm66.img` | `122f3204d22e5f95754e0bd662457c525161cda39efadb5d759a54e45367749d` |
| `tree/hobbyos.stripped.elf` | `58db5e8e8284284b8ec6f24b29fba961fc588d2c00285a2179cc04d43c067fa0` |
| `tree/src/kernel/nv_fsp_cot.c` (booted) | `752f088d56be1e9b950b085ca34be4a2ec9ff6e28b046f30ae1ddb5b34ad3f12` |
| `nv_fsp_cot.c.pre66` | `bc7c09eb93e82ea3796bbdcada5766a5dc10e8e6a85cb21d44140d8d9828531a` |
| `gb202-dmem-log-decode.py` | `346755b287d0ecb9cf7e237a0fea7018f8bebc25b27eb02957bf276eab46fb35` |
| `dm-log-encode-check.c` | `a54a018891c5a456c25429c512c3cc4e48818e678262d50381bd64527983fe31` |
| `dm-log-make-frame.c` | `59b43195e9e60bd4fbed22998902a0ad8bbc18b56a9625f46d4be6f8550b613d` |
| `boot66.sh` | `9b6429e5bdf88339e00f1463c8ce3078bff4a61e82384c227c1692756bb8f607` |
