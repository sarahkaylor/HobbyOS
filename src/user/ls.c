#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  exit(main());
}
#endif

int main(void) {
  char arg_buf[256];
  get_args(arg_buf, sizeof(arg_buf));
  char *argv[16];
  int argc = parse_args(arg_buf, argv, 16);

  int long_format = 0;
  const char *path = ".";

  for (int i = 0; i < argc; i++) {
    if (argv[i][0] == '-') {
      int j = 1;
      while (argv[i][j]) {
        if (argv[i][j] == 'l') {
          long_format = 1;
        }
        j++;
      }
    } else {
      path = argv[i];
    }
  }

  struct sys_dirent ent;
  int index = 0;
  while (read_dir(path, index, &ent) == 0) {
    if (long_format) {
      if (ent.attr & 0x10) {
        print("d ");
      } else {
        print("- ");
      }
      print_dec(ent.size);
      print(" ");
      print(ent.name);
      print("\n");
    } else {
      print(ent.name);
      print("\n");
    }
    index++;
  }
  /* GB-070: W0 output-differential marker (see cat.c / hello.c).  LS lists the
     root dir; in the wave it runs with no start-path so it reaches here with
     RC 0.  The t4_w0 gate compares this token against the Linux-leg .ref. */
#if !defined(HOST_TEST)
  print("[W0] LS linux=entered-main:main-returned\n");
#endif
  return 0;
}
