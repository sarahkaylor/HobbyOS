/*
 * nvsmi: read the native NVIDIA driver's state and print it.
 *
 * The kernel node /dev/nvctl answers read(2) with one struct nv_info record
 * (src/include/nv.h).  Native HobbyOS programs have no ioctl(2) row, so the
 * node is read like a file; a Linux-ABI consumer uses the NV_IOC_* requests
 * on the same node instead.
 *
 * Exit status: 0 when a card was probed (even if a later stage failed -- the
 * report says where), 1 when the driver saw no card, 2 when the node is
 * missing (a kernel without the driver).
 */

#include "libc.h"
#include "../include/nv.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

static const char *state_name(uint32_t s) {
  switch (s) {
  case 0: return "absent";
  case 1: return "pci";
  case 2: return "bar";
  case 3: return "id";
  case 4: return "pmc";
  case 5: return "timer";
  case 6: return "falcon";
  case 7: return "firmware";
  case 8: return "gsp";
  case 9: return "ready";
  default: return "?";
  }
}

/* Every row is "nv: " + a fixed-width label; callers pass the padding in the
 * label so the report lines up without a printf. */
static void label(const char *name) {
  print("nv: ");
  print(name);
}

static void row_str(const char *name, const char *val) {
  label(name);
  print(val);
  print("\n");
}

static void row_hex(const char *name, uint64_t val) {
  label(name);
  print_hex((long)val);
  print("\n");
}

static void row_dec(const char *name, long val) {
  label(name);
  print_dec(val);
  print("\n");
}

int main(void) {
  struct nv_info info;
  int fd = open("/dev/nvctl", 0);
  if (fd < 0) {
    print("nvsmi: /dev/nvctl is missing (kernel built without nv?)\n");
    return 2;
  }
  int n = (int)read(fd, &info, (int)sizeof(info));
  if (n < (int)sizeof(info)) {
    print("nvsmi: short read from /dev/nvctl\n");
    return 2;
  }

  print("nv: HobbyOS native NVIDIA driver report\n");
  row_dec("abi         ", (long)info.abi);
  row_str("state       ", state_name(info.state));
  if (info.state == 0) {
    row_str("card        ", "none: no NVIDIA display-class device on the bus");
    return 1;
  }
  if (info.failed_stage)
    row_str("failed at   ", state_name(info.failed_stage));

  char namebuf[9];
  for (int i = 0; i < 8; i++) namebuf[i] = info.name[i];
  namebuf[8] = 0;
  print("nv: gpu         ");
  print(namebuf[0] ? namebuf : "(unknown)");
  print(" chipset ");
  print_hex((long)info.chipset);
  print("\n");

  print("nv: pci         ");
  print_dec(info.bus);
  print(":");
  print_dec(info.slot);
  print(".");
  print_dec(info.func);
  print(" vendor 10de:");
  print_hex((long)info.device_id);
  print("\n");

  row_hex("bar0 phys   ", info.bar0_phys);
  row_hex("bar0 size   ", info.bar0_size);
  row_hex("bar1 phys   ", info.bar1_phys);
  row_hex("bar1 size   ", info.bar1_size);
  row_hex("bar3 phys   ", info.bar3_phys);
  row_hex("bar3 size   ", info.bar3_size);

  row_hex("pmc boot0   ", info.boot0);
  row_hex("pmc boot1   ", info.boot1);
  row_hex("pmc enable  ", info.pmc_enable);
  row_dec("ptimer hz   ", (long)info.timer_hz);
  row_hex("irq vector  ", info.irq_vector);

  static const char *fname[3] = {"pmu", "gsp", "sec2"};
  for (int i = 0; i < 3; i++) {
    print("nv: falcon      ");
    print(fname[i]);
    if (!(info.falcon_mask & (1u << i))) {
      print(" absent\n");
      continue;
    }
    print(" v");
    print_dec((long)(info.falcon_caps2[i] & 0xf));
    print(" caps ");
    print_hex((long)info.falcon_caps[i]);
    print(" caps2 ");
    print_hex((long)info.falcon_caps2[i]);
    print(" ctl ");
    print_hex((long)info.falcon_ctl[i]);
    print(" busy ");
    print_hex((long)info.falcon_busy[i]);
    print("\n");
  }

  if (info.gsp_required) {
    print("nv: gsp         required on this chip; 3D/memory/display work only "
          "after GSP-RM boots\n");
  } else {
    print("nv: gsp         optional on this chip\n");
  }
  if (info.state < 9 && info.gsp_required) {
    print("nv: verdict     driver reached '");
    print(state_name(info.state));
    print("'; hardware 3D is not available until GSP-RM is running\n");
  }
  return 0;
}
