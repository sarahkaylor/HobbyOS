/*
 * NETFIX.BIN — on-device repro + regression app for the l8-netfix kernel
 * lane (browser.md §11, WK-4 entry).  Booted in MODE=webproc as WEBPROC.BIN
 * (single-program mode; the WebKit driver is NOT involved).
 *
 * The two kernel bugs this exercises:
 *
 *  1. TX corruption (bug #1): while the UART console is emitting a marker
 *     line, a socket send must still transmit exactly the payload.  The
 *     host netfix server echoes back a hex dump of the FIRST bytes it
 *     received, so the guest can verify — from its own side — that the
 *     request arrived un-contaminated (the WK-4 bug put the console line
 *     into the stream; the host then saw "WK4 NET connect=ok ..." -> 400).
 *
 *  2. RX leading NULs (bug #2): recv() on an accepted connection must not
 *     prepend bytes the host never sent (WK-4 observed six 0x00 before the
 *     HTTP status line).  We hex-dump the first bytes of every recv and
 *     FAIL on any leading NUL before the expected response magic.
 *
 * Each trial = fresh TCP socket to the host netfix server.  The response is
 * a fixed ASCII envelope ("NETFIX-SRV-OK\n...\nEND\n") so integrity checks
 * are byte-exact and independent of HTTP parsing.
 *
 * Prints VERDICT markers (NETFIX-TX-*, NETFIX-RX-*) for the harness tally.
 * Exit 0/1.
 */

#include "libc.h"
#include <stdint.h>

/* Host (10.0.2.2) as seen from the guest under QEMU user networking.
   10.0.2.2 = 0x0A000202 big-endian on the wire; as a little-endian
   uint32_t value that is 0x0202000A. */
#define NETFIX_GW_BE 0x0202000Au
/* Port 8765 in network byte order (as a little-endian uint16 value). */
#define NETFIX_PORT_BE 0x3D22u

#define NETFIX_ATTEMPTS 24
#define NETFIX_RXBUF 4096

static int fails;

static void putstr(const char* s) { print_console(s); }

static void puthex_byte(uint8_t b) {
  static const char* hx = "0123456789ABCDEF";
  char c[2];
  c[0] = hx[(b >> 4) & 0xF];
  c[1] = hx[b & 0xF];
  print_console(c);  /* no NUL-terminated short-string helper; 2 chars */
}

/* Print a hex dump of up to n bytes of buf. */
static void dump_hex(const uint8_t* buf, int n) {
  int i;
  putstr("NETFIX hex[");
  print_dec(n);
  putstr("]: ");
  for (i = 0; i < n; i++) {
    puthex_byte(buf[i]);
    putstr(" ");
  }
  putstr("\n");
}

static void decode_and_verify_hex(const char* hex, int hexlen, int trial,
                                  const uint8_t* want, int wantlen) {
  /* hex is the ASCII hex (uppercase) of the first bytes the server
     received.  Decode and compare against what we sent. */
  int i;
  int bad = 0;
  if (hexlen < wantlen * 2) {
    putstr("NETFIX TX-FAIL: server hex shorter than payload\n");
    fails++;
    return;
  }
  for (i = 0; i < wantlen; i++) {
    int hi = 0, lo = 0;
    char c = hex[i * 2];
    char d = hex[i * 2 + 1];
    hi = (c >= '0' && c <= '9') ? c - '0' : (c - 'A' + 10);
    lo = (d >= '0' && d <= '9') ? d - '0' : (d - 'A' + 10);
    if ((hi < 0 || hi > 15) || (lo < 0 || lo > 15)) {
      bad = 1;
      break;
    }
    if ((hi << 4 | lo) != want[i]) bad = 1;
    if (bad) break;
  }
  if (bad) {
    putstr("NETFIX TX-FAIL: request was corrupted on the wire (trial ");
    print_dec(trial);
    putstr(")\n");
    fails++;
  } else {
    putstr("NETFIX TX-OK: server received exactly the request (trial ");
    print_dec(trial);
    putstr(")\n");
  }
}

/* One trial: fresh socket, print a console marker, send, verify. */
static void trial(int n) {
  char req[160];
  int reqlen = 0;
  int fd;
  uint8_t buf[NETFIX_RXBUF];
  int total = 0;
  int i;
  const uint8_t* magic = (const uint8_t*)"NETFIX-SRV-OK";
  int lead_nul = 0;

  /* Marker line built right here so it shares the same stack-generation
     window as the request construction (WK-4: console text contaminated the
     TX DMA source while it was printing).  A long multi-line flood keeps the
     console channel busy across the connect/send window, like the WK-4
     driver's boot-time marker output. */
  {
    int li;
    for (li = 0; li < 20; li++) {
      putstr("NETFIX console flood line #");
      print_dec(li);
      putstr(" trial ");
      print_dec(n);
      putstr(" flowing through the UART channel right now 0123456789\n");
    }
  }

  reqlen = 0;
  {
    static const char* head = "NETFIX-REQ ";
    for (i = 0; head[i]; i++) req[reqlen++] = head[i];
  }
  req[reqlen++] = (char)('0' + (n / 100) % 10);
  req[reqlen++] = (char)('0' + (n / 10) % 10);
  req[reqlen++] = (char)('0' + n % 10);
  req[reqlen++] = ' ';
  for (i = 0; i < 32; i++) {
    req[reqlen++] = (char)('A' + (i + n) % 26);
  }
  req[reqlen++] = '\r';
  req[reqlen++] = '\n';

  fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    putstr("NETFIX FAIL: socket\n");
    fails++;
    return;
  }
  if (connect_fd(fd, NETFIX_GW_BE, (uint16_t)NETFIX_PORT_BE) != 0) {
    putstr("NETFIX SKIP: connect failed (no host server), trial ");
    print_dec(n);
    putstr("\n");
    close(fd);
    return;
  }
  putstr("NETFIX TX connected trial ");
  print_dec(n);
  putstr(", sending ");
  print_dec(reqlen);
  putstr(" bytes\n");
  {
    int li;
    for (li = 0; li < 10; li++) {
      putstr("NETFIX console drain line ");
      print_dec(li);
      putstr(" right at the send window trial ");
      print_dec(n);
      putstr("\n");
    }
  }

  if (write(fd, req, reqlen) != reqlen) {
    putstr("NETFIX FAIL: short write\n");
    fails++;
    close(fd);
    return;
  }
  putstr("NETFIX TX sent trial ");
  print_dec(n);
  putstr("\n");

  /* Read the whole response. */
  for (;;) {
    int rd = read(fd, buf + total, NETFIX_RXBUF - total);
    if (rd <= 0) break;
    total += rd;
    if (total >= NETFIX_RXBUF) break;
  }

  if (total <= 0) {
    putstr("NETFIX FAIL: no response bytes (trial ");
    print_dec(n);
    putstr(")\n");
    fails++;
    close(fd);
    return;
  }
  putstr("NETFIX RX total=");
  print_dec(total);
  putstr("\n");
  dump_hex(buf, total < 48 ? total : 48);

  /* Bug #2: leading NULs before the response magic. */
  lead_nul = 0;
  if (buf[0] == 0x00) {
    /* count the leading NUL run */
    int run = 0;
    while (run < total && buf[run] == 0x00) run++;
    if (run > 0 && run < total - 8) {
      /* real data follows the NUL run */
      for (i = 0; i < 12; i++) {
        if (buf[run + i] != magic[i]) break;
      }
      if (i == 12) lead_nul = run;
    }
  }
  if (lead_nul) {
    putstr("NETFIX RX-FAIL: recv prepended ");
    print_dec(lead_nul);
    putstr(" NUL byte(s) before the server response (trial ");
    print_dec(n);
    putstr(")\n");
    fails++;
  } else if (total < 12) {
    putstr("NETFIX RX-FAIL: response too short\n");
    fails++;
  } else {
    int ok = 1;
    for (i = 0; i < 12; i++) {
      if (buf[total >= 12 ? i : i] != magic[i]) ok = 0;
    }
    if (ok) {
      putstr("NETFIX RX-OK: response starts clean at byte 0 (trial ");
      print_dec(n);
      putstr(")\n");
    } else {
      putstr("NETFIX RX-FAIL: response magic not at stream start (trial ");
      print_dec(n);
      putstr(")\n");
      fails++;
    }
  }

  /* Bug #1: decode the server's echoed hex of what it received. */
  {
    /* find "rxhex=" and the following hex run */
    int j;
    for (j = 0; j + 6 < total; j++) {
      if (buf[j] == 'r' && buf[j + 1] == 'x' && buf[j + 2] == 'h' &&
          buf[j + 3] == 'e' && buf[j + 4] == 'x' && buf[j + 5] == '=') {
        const char* hex = (const char*)buf + j + 6;
        int hexlen = 0;
        while (j + 6 + hexlen < total && hex[hexlen] != '\n' &&
               hex[hexlen] != '\r')
          hexlen++;
        decode_and_verify_hex(hex, hexlen, n, (const uint8_t*)req, reqlen);
        break;
      }
    }
  }

  close(fd);
}

int main(void) {
  int n;
  putstr("NETFIX START repro (TX/console + RX-NUL)\n");
  for (n = 1; n <= NETFIX_ATTEMPTS; n++) {
    trial(n);
  }
  if (fails) {
    putstr("NETFIX RESULTS: ");
    print_dec(fails);
    putstr(" FAILURES\n");
    exit(1);
  }
  putstr("NETFIX RESULTS: ALL PASS\n");
  exit(0);
}
