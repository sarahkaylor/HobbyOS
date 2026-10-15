/*
 * dentest.c — w3-kernel Gap B in-OS end-to-end proof.
 *
 * A real libc consumer: uses opendir()/readdir()/closedir() from the
 * sysroot libc (the same functions that were ENOSYS stubs before this
 * lane) to enumerate /FIXTURE and /FIXTURE/SUB, then writes the listing
 * to /DENTEST.OUT via stdio.  The file is the in-OS hard assertion (the
 * console is framebuffer-only); the same build also prints markers on
 * stdout for the screendump evidence.
 *
 * Build (replicating the Makefile's user.link rule, no Makefile edit):
 *   clang -O2 -target aarch64-none-elf -mcpu=cortex-a53 -ffreestanding \
 *         -I src/libc/include -I src/include -I src/user_include \
 *         -c dentest.c -o obj/arm/dentest.o
 *   ld.lld -T src/user/linker.ld -e _start -o obj/arm/dentest.elf \
 *         obj/arm/dentest.o obj/arm/libc.a obj/arm/crt0.o
 *   llvm-objcopy -O binary obj/arm/dentest.elf obj/arm/dentest.bin
 */
#include <stdio.h>
#include <string.h>
#include <dirent.h>

int main(void) {
  static char out[512];
  size_t n = 0;
  int r = 0;

  out[n++] = 'O';
  out[n++] = 'K';
  out[n++] = 0;

  DIR *d = opendir("/FIXTURE");
  if (!d) {
    out[n++] = 'E';
    out[n++] = 'R';
    out[n++] = 'R';
    out[n++] = 0;
    printf("DENTEST FAIL opendir /FIXTURE\n");
    r = 1;
  } else {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      printf("ENT %s\n", de->d_name);
      size_t l = strlen(de->d_name);
      if (n + l + 2 < sizeof(out)) {
        out[n++] = (char)('A' + l); /* length-marker byte */
        memcpy(out + n, de->d_name, l);
        n += l;
      }
    }
    closedir(d);
  }

  /* Subdirectory recursion: proof readdir descends. */
  d = opendir("/FIXTURE/SUB");
  if (!d) {
    out[n++] = 'S';
    out[n++] = 'E';
    out[n++] = 'R';
    out[n++] = 'R';
    out[n++] = 0;
    printf("DENTEST FAIL opendir /FIXTURE/SUB\n");
    r = 1;
  } else {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      printf("SUB %s\n", de->d_name);
      size_t l = strlen(de->d_name);
      if (n + l + 2 < sizeof(out)) {
        out[n++] = (char)('a' + l);
        memcpy(out + n, de->d_name, l);
        n += l;
      }
    }
    closedir(d);
  }

  FILE *f = fopen("/DENTEST.OUT", "w");
  if (!f) {
    printf("DENTEST FAIL fopen /DENTEST.OUT\n");
    return 1;
  }
  fwrite(out, 1, n, f);
  fclose(f);
  printf("DENTEST written %d bytes rc=%d\n", (int)n, r);
  return r;
}
