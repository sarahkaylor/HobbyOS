# FS lane N3 — per-socket RX ring: >4 MiB single-response fetch fix

Branch `browser/fs-n3` (worktree `/home/sarah/hobbyos-lanes/fs-r5`).
Commit: `9479d7b fs-n3: TCP RX flow control — fix >=4MiB single-response aborts (per-socket ring overflow)`

## Root cause (G11 A/B + this lane's code reading)

The guest kernel keeps a fixed per-socket circular RX ring
(`SOCKET_RX_BUF_SIZE = 4 MiB`, `src/include/net.h`) that the RX ISR fills
and the app's `read()` path drains (`net_socket_recv`).  The v1
`handle_tcp` copied **every** segment into the ring unconditionally and
ACKed all of it — even once unread bytes (`rx_tail - rx_head`) reached the
ring size.  For a single-response wire body >= ~4 MiB arriving ahead of
the app's drain, the write wrapped around and silently overwrote unread
data (`rx_buf[(rx_tail + j) % SOCKET_RX_BUF_SIZE] = ...`), corrupting the
byte stream.  libcurl then saw a corrupt HTTP/TLS stream and aborted the
transfer: `open-fail 'Unsupported protocol'`, `bytes=0`.  G11's A/B probe
(`continuation/fs-g11/evidence/probe-ab`) proved the threshold is exactly
the ring size, independent of server/port/content (4 KB head and 226 KB
control pass; 5.7 MB CNN and a 6 MB synthetic blob both fail with
`bytes=0`).  The gzip wire (936 KB) dodged the cliff; it did not fix it.

## Fix — producer flow control, ring size unchanged

`src/kernel/net.c` `handle_tcp` ESTABLISHED payload path:
1. **Contiguous-only store.**  Payload is stored only when its sequence
   number equals the next expected byte (`seg_seq == pcb->ack`), and only
   up to the free ring space.  `pcb->ack` advances only by stored bytes.
   Anything out-of-order or beyond the ring is refused (counted +
   printed once per socket via the new `rx_drops`/`rx_drop_segments`
   fields in `src/include/net.h`).  Because only the exact expected
   prefix is ever appended, the ring byte stream can never wrap and can
   never reorder — at any body size.
2. **Truthful window.**  `send_tcp_segment` now advertises the true free
   ring space (capped at the 16-bit window); the v1 `min 2048` floor
   (which lied once the ring approached full and let the peer push past
   the real free space) is removed.  A full ring advertises window 0;
   the peer parks with zero-window probes until the **existing**
   post-drain window-update ACK in `net_socket_recv()` reopens the flow.
3. **FIN integrity.**  A remote FIN is honored only when it sits exactly
   at the end of the contiguous stored stream (`seg_seq + data_len ==
   pcb->ack`), so an un-retransmitted tail can never be silently dropped
   along with the close.

Design choice (documented in the code comment):
- *Raise the ring?* No — the table is 16 static 4 MiB buffers (64 MiB
  total); raising scales memory linearly and only moves the cliff, not
  the wrap bug.  Ring size is now **latency slack**, not the correctness
  bound.
- *Bounded auto-grow?* Not needed — the peer must never outrun the
  consumer anyway; flow control is the correct TCP answer and needs no
  dynamic allocation in ISR context.

## Retest receipts (identity, non-gzip)

- `evidence/probe/`  — boot 1: small control (225,998 B) PASS,
  4 KB head PASS, **6 MiB synthetic blob identity PASS** (`[WIN] net
  fetch ok=1 status=200 bytes=6291456` + `net persist 6291456 B`,
  PAGE-NET copy-back sha `6ec571f1…` == served blob sha).
- `evidence/probe2/` — boot 2: CNN 5,704,241 B identity gate (browser
  `[WIN] net fetch`) + kernel-only shell `nc` byte-match of the same
  5,704,241 B body and the 6 MiB blob.
- Gate bytes: `evidence/CNN-5704241.HTM` = G11's cnn-direct persist
  (5,704,241 B, sha `c1dd9525…`), served identity (no Content-Encoding,
  honest Content-Length) by `tools/n3_identity_relay.py`.

## Net regression sweep (F-R3)

See `sweep-N3.md` for the exact commands + results (SOCK2TST, NETFIX,
DNSTST, unit-arm, unit-x64 KVM, ARM wave 0-FAIL).
