/* memprobe.c - T2-18 guest memory high-water probe (FS lane V4).
 *
 * Prints the guest user-region memory usage (sysinfo cmd 2 = total/used/free
 * bytes of the process/user allocation region) to a guest-disk file so the
 * acceptance runner can copy it back byte-exact.  Used at the end of the
 * multi-page soak to capture guest memory high-water.  Probe only, no behavior
 * change ("probe, don't fix").
 *
 * Build (standalone, mirrors clockprobe.c):
 *   clang --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 \
 *     -O2 -Wall -Wextra -Isrc/user_include -Isrc/user_include/graphics \
 *     -Isrc/include -Isrc/libc/include -c memprobe.c -o memprobe.o
 *   clang --target=aarch64-none-elf -ffreestanding -mcpu=cortex-a53 -O2 \
 *     -T src/user/linker.ld -e _start -o memprobe.elf memprobe.o
 *     $(OBJ_DIR)/libc.a
 *   llvm-objcopy -O binary memprobe.elf MEMPX.BIN
 */
#include "libc.h"
#include <stdint.h>

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
  struct sys_meminfo mem;
  int rc = sysinfo(2, &mem, (int)sizeof mem);
  if (rc != 0) {
    print("[MEMPX] no-meminfo rc=");
    print_dec(rc);
    print("\n");
    return 1;
  }
  uint64_t used = mem.total_bytes - mem.free_bytes;
  print("[MEMPX] total=");
  print_dec((long)mem.total_bytes);
  print(" free=");
  print_dec((long)mem.free_bytes);
  print(" used=");
  print_dec((long)used);
  print("\n");
  return 0;
}
