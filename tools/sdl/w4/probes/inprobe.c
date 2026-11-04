#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* One Linux-ABI input_event: timeval (16) + type@16 + code@18 + value@20. */
struct iev { unsigned long sec, usec; unsigned short type, code; int value; };

int main(void) {
  int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
  if (fd < 0) return 41;
  int seen = 0;
  unsigned short last_code = 0;
  for (int i = 0; i < 3000; i++) {         /* ~15 s of polling */
    struct iev e;
    ssize_t n = read(fd, &e, sizeof e);
    if (n == (ssize_t)sizeof e && e.type == 1) { /* EV_KEY */
      seen++;
      last_code = e.code;
      break;
    }
    usleep(5000);
  }
  FILE *f = fopen("/INPUT.TXT", "w");
  if (f) {
    fprintf(f, "events=%d last_key_code=%u\n", seen, (unsigned)last_code);
    fclose(f);
  }
  return seen > 0 ? (int)last_code : 99;   /* exit status = the key code */
}
