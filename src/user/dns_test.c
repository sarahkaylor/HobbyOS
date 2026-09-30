/*
 * DNSTST.BIN — real-network DNS resolution via the DHCP-learned server
 * (browser.md F2.2/F2.3).  NOT WIRED YET — committed as source only, per
 * the lane brief; the parent wires it at integration.
 *
 * Why unwired: resolv_lookup() needs the net lane's kernel side — syscalls
 * 65/66 (socket/connect_fd), the write/read paths and SYS_GETRANDOM's
 * sibling plumbing — plus sysinfo(4).dns from the F1.7 DHCP hand-off.
 * Until those land this program can only SKIP, and a boot-wave entry that
 * can never pass would mask real failures.
 *
 * Wiring steps for the parent (all in this repo, one change):
 *   Makefile: the object rules already exist (object-only, no .bin wiring):
 *     obj/$(ARCH)/user_resolv.o    (src/user/resolv.c)
 *     obj/$(ARCH)/user_dns_test.o  (src/user/dns_test.c)
 *   add the build/link rule:
 *     DNSTST_BIN = $(OBJ_DIR)/dns_test.bin
 *     $(DNSTST_BIN): $(OBJ_DIR)/user_dns_test.o $(OBJ_DIR)/user_resolv.o \
 *                    $(OBJ_DIR)/libc.a
 *      $(LD) -T src/user/linker.ld -e _start -o $(OBJ_DIR)/dns_test.elf $^
 *      $(OBJCOPY) -O binary $(OBJ_DIR)/dns_test.elf $(DNSTST_BIN)
 *   add $(DNSTST_BIN) to the disk.img dependency list, and
 *     $(MCOPY) -i disk.img $(DNSTST_BIN) ::/DNSTST.BIN
 *   src/kernel/main.c: at the END of test_wave_loader() (it spawns
 *   nothing, so no process-slot pressure):
 *     load_and_run_program_in_scheduler("DNSTST.BIN", -1, -1, -1, -1);
 *
 * What it does: resolves "example.com" through the real network stack.
 * Path 1 — resolv_lookup(): sysinfo(4).dns (the F1.7 field; 0 until the
 * kernel lane lands) drives resolv_lookup_server().  Path 2 — when that
 * field is still 0 (ENETUNREACH) it falls back to QEMU slirp's built-in
 * DNS at 10.0.2.3 explicitly, so the test is exercisable by the kernel
 * lane before F1.7 completes.
 *
 * Verdicts (print_console, boot-wave convention):
 *   PASS  — an A record came back with a nonzero address (address printed).
 *   SKIP  — no usable reply (unreachable / no kernel network): exit 0;
 *           network-dependent, must not fail the suite on a host-level gap.
 *   FAIL  — the server answered but the answer was unusable (ENOENT/EIO
 *           from the parser: no A record / server failure / malformed).
 * Self-terminating; no sleeps.
 */

#include "libc.h"
#include "resolv.h"

/* QEMU slirp's built-in DNS server, wire order in memory. */
#define DNSTST_FALLBACK_DNS RESOLV_IP4(10, 0, 2, 3)

int main(void) {
  struct sys_netinfo net;
  char text[INET_ADDRSTRLEN];
  uint32_t ip = 0;
  int rc;

  print_console("[DNSTST] DNS resolution test (example.com)\n");

  memset(&net, 0, sizeof net);
  if (sysinfo(4, &net, (int)sizeof net) == 0 && net.dns != 0) {
    print_console("[DNSTST] DHCP DNS server: ");
    if (inet_ntop(AF_INET, &net.dns, text, sizeof text))
      print_console(text);
    print_console("\n");
  } else {
    print_console("[DNSTST] sysinfo(4).dns unset (kernel lane pending)\n");
  }

  rc = resolv_lookup("example.com", &ip);
  if (rc < 0 && errno == ENETUNREACH) {
    print_console("[DNSTST] falling back to slirp DNS 10.0.2.3\n");
    rc = resolv_lookup_server("example.com", DNSTST_FALLBACK_DNS, &ip);
  }
  if (rc < 0) {
    if (errno == ENOENT || errno == EIO || errno == EINVAL) {
      print_console("[DNSTST] FAIL: reply arrived but unusable (errno=");
      print_dec(errno);
      print_console(")\n");
      exit(1);
    }
    print_console("[DNSTST] SKIP: DNS unreachable (errno=");
    print_dec(errno);
    print_console(")\n");
    exit(0);
  }
  if (ip == 0) {
    print_console("[DNSTST] FAIL: A record is 0.0.0.0\n");
    exit(1);
  }

  print_console("[DNSTST] example.com A = ");
  if (inet_ntop(AF_INET, &ip, text, sizeof text))
    print_console(text);
  print_console("\nALL TESTS PASSED SUCCESSFULLY!\n");
  exit(0);
}
