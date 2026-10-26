#include "libc.h"

int main(void);

#ifndef HOST_TEST
__attribute__((section(".text._start")))
void _start(void) {
  main();
  exit(0);
}
#endif

void cat_fd(int fd) {
  char buf[512];
  int n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    write(1, buf, n);
  }
}

int main(void) {
  char arg_buf[256];
  get_args(arg_buf, sizeof(arg_buf));

  char *argv[16];
  int argc = parse_args(arg_buf, argv, 16);

  if (argc == 0) {
    cat_fd(0);
  } else {
    for (int i = 0; i < argc; i++) {
      if (argv[i][0] == '-' && argv[i][1] == '\0') {
        cat_fd(0);
      } else {
        int fd = open(argv[i], 0);
        if (fd < 0) {
          print("cat: ");
          print(argv[i]);
          print(": No such file or directory\n");
        } else {
          cat_fd(fd);
          close(fd);
        }
      }
    }
  }
  /* GB-070: W0 output-differential marker (same shape as HELLO's).  A real
     wave runs CAT with empty stdin, so the observable output is that main was
     entered and returned; the t4_w0 gate compares this token against the
     Linux-leg .ref, so a CAT that exits early or crashes FAILs live.
     Emitted to stderr (fd 2), not stdout: cat's stdout is a harness pipe, so
     a stdout marker would be swallowed into that pipe's framing; the gate
     reads the serial console, which stderr reaches. */
#if !defined(HOST_TEST)
  {
    const char *m = "[W0] CAT linux=entered-main:main-returned\n";
    size_t ml = 0;
    while (m[ml]) ml++;
    (void)!write(2, m, ml);
  }
#endif
  return 0;
}
