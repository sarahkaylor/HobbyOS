/* src/user/pipeprobe.c — in-OS ground truth for pipe fd + spawn stdin flow.
 *
 * Boot23 result (first generation): the pipe fd contract is correct (write
 * to [1] works, read from [0] returns the data), the parent successfully
 * writes "hi\n" to SED.BIN's stdin after spawn, yet SED output is empty and
 * it exits 0 — i.e. the child either never sees the bytes or never emits its
 * result.  This second generation isolates the plausible causes with four
 * sub-runs, printing every stage to the console:
 *
 *   A: stdin written BEFORE the spawn (pipe pre-filled).
 *   B: stdin written AFTER the spawn, stderr redirected to the same pipe as
 *      stdout so SED's own error messages are captured.
 *   C: file argument (SEDT.IN) with no stdin — the file-based control.
 *   D: like B but with a 50 ms pause between spawn and write.
 *
 * Each sub-run prints: spawn pid, write results, captured bytes, exit code.
 * Runs early in KERNEL_MODE_TEST so the UART log is still lossless.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libc.h"

static void con_int(int v) {
  char b[16];
  int i = 15, neg = 0;
  if (v < 0) {
    neg = 1;
    v = (v == -2147483648) ? 2147483647 : -v;
  }
  b[i--] = '\0';
  do {
    b[i--] = (char)('0' + v % 10);
    v /= 10;
  } while (v > 0);
  if (neg) b[i--] = '-';
  print_console(&b[i + 1]);
}

static void pnum(const char *tag, int v) {
  print_console(tag);
  con_int(v);
  print_console(" ");
}

static void ptxt(const char *tag, const char *s, size_t n) {
  print_console(tag);
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\n') {
      print_console("\\n");
    } else if (c < 32 || c > 126) {
      print_console("?");
    } else {
      char b[2];
      b[0] = c;
      b[1] = 0;
      print_console(b);
    }
  }
  print_console("\"\n");
}

/* Drain a pipe read end into buf until EOF; returns bytes captured. */
static size_t drain(int fd, char *buf, size_t cap) {
  size_t n = 0;
  while (n + 1 < cap) {
    ssize_t r = read(fd, buf + n, cap - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  buf[n] = '\0';
  return n;
}

/* Write all of s to fd, retrying on 0/-1 a bounded number of times. */
static ssize_t write_all(int fd, const char *s, int *tries_out) {
  size_t off = 0, len = strlen(s);
  int tries = 0;
  while (off < len && tries < 100) {
    ssize_t w = write(fd, s + off, len - off);
    if (w > 0) {
      off += (size_t)w;
      tries = 0;
      continue;
    }
    usleep(2000);
    tries++;
  }
  *tries_out = tries;
  return (ssize_t)off;
}

/* Spawn with a bounded retry: during KERNEL_MODE_TEST the physical memory
   pool can be drained by the boot wave, making process_create fail
   transiently.  Retry until the pool frees a block (up to ~5 s). */
static int spawn_retry(const char *prog, int in_fd, int out_fd, int err_fd,
                       const char *args) {
  for (int i = 0; i < 200; i++) {
    int pid = spawn2(prog, in_fd, out_fd, err_fd, args);
    if (pid > 0) return pid;
    usleep(25000);
  }
  return -1;
}

/* Sub-run A: data pre-filled before spawn. */
static void run_prefill(void) {
  int a[2], b[2];
  char buf[256];
  int tries = 0;
  ssize_t w;
  int pid;

  print_console("[PP] A: prefill\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] A pipes failed\n");
    return;
  }
  w = write_all(a[1], "hi\n", &tries);
  pnum("A write_pre=", (int)w);
  print_console("\n");
  pid = spawn_retry("SED.BIN", a[0], b[1], -1, "s/hi/HI/");
  pnum("A pid=", pid);
  pnum("tries=", tries);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("A got=", (int)n);
    ptxt(" data=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      waitpid(pid, &ws, 0);
      pnum("A code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Sub-run B: write after spawn, stderr shared with the stdout pipe. */
static void run_after_stderr(void) {
  int a[2], b[2];
  char buf[256];
  int tries = 0;
  ssize_t w;
  int pid;

  print_console("[PP] B: post+stderr\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] B pipes failed\n");
    return;
  }
  pid = spawn_retry("SED.BIN", a[0], b[1], b[1], "s/hi/HI/");
  pnum("B pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    w = write_all(a[1], "hi\n", &tries);
    pnum("B write=", (int)w);
    pnum("tries=", tries);
    print_console("\n");
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("B got=", (int)n);
    ptxt(" data=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      waitpid(pid, &ws, 0);
      pnum("B code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Sub-run C: file argument control (no stdin). */
static void run_file(void) {
  int a[2], b[2];
  char buf[256];
  int pid;
  int fd = open("SEDT.IN", O_WRONLY | O_CREAT | O_TRUNC);
  if (fd >= 0) {
    write(fd, "alpha\nbeta\n", 12);
    close(fd);
  }
  print_console("[PP] C: file\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] C pipes failed\n");
    return;
  }
  pid = spawn_retry("SED.BIN", a[0], b[1], -1, "s/beta/BETA/ SEDT.IN");
  pnum("C pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("C got=", (int)n);
    ptxt(" data=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      waitpid(pid, &ws, 0);
      pnum("C code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Sub-run D: write after spawn, with a settling pause first. */
static void run_settled(void) {
  int a[2], b[2];
  char buf[256];
  int tries = 0;
  ssize_t w;
  int pid;

  print_console("[PP] D: settled\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] D pipes failed\n");
    return;
  }
  pid = spawn_retry("SED.BIN", a[0], b[1], -1, "s/hi/HI/");
  pnum("D pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    usleep(50000); /* 50 ms: the child reaches its first read first */
    w = write_all(a[1], "hi\n", &tries);
    pnum("D write=", (int)w);
    pnum("tries=", tries);
    print_console("\n");
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("D got=", (int)n);
    ptxt(" data=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      waitpid(pid, &ws, 0);
      pnum("D code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Echo-child mode: spawned by the parent probe with argv[1]="echo" through
 * the same pipe pair.  Reads fd 0 raw (no stdio), mirrors every byte to
 * fd 1, prints each intermediate result to the console, exits 42.  If this
 * works the plumbing is sound and the bug is inside SED's own stdin path;
 * if it fails the bug is in the kernel pipe/spawn plumbing. */
static int echo_child(void) {
  char b2[64];
  ssize_t r;
  ssize_t w;

  print_console("[ECHO] start\n");

  /* 1: the exact failing call shape — stdio fread on fd 0, which goes
     through hb_file_fill -> read(0, rbuf, 512) with the malloc'd stdio
     buffer.  "hi\n" is expected to be available. */
  errno = 0;
  size_t g = fread(b2, 1, 32, stdin);
  pnum("[ECHO] fread=", (int)g);
  pnum("errno=", errno);
  ptxt(" data=\"", b2, g < 32 ? g : 32);

  /* 2: raw stack-buffer read (known to work). */
  errno = 0;
  r = read(0, b2, sizeof b2);
  pnum("[ECHO] stack read=", (int)r);
  pnum("errno=", errno);
  print_console("\n");

  /* 3: raw malloc'd-buffer read (the stdio buffer shape). */
  char *p = malloc(512);
  errno = 0;
  if (p) {
    r = read(0, p, 512);
    pnum("[ECHO] malloc read=", (int)r);
  } else {
    print_console("[ECHO] malloc failed ");
    r = -99;
  }
  pnum("errno=", errno);
  print_console("\n");

  /* 4: write the bytes that fread returned through fd 1 so the parent
     can verify the data made it end-to-end. */
  errno = 0;
  w = write(1, b2, g < 32 ? g : 32);
  pnum("[ECHO] wrote=", (int)w);
  pnum("errno=", errno);
  print_console("\n");

  print_console("[ECHO] eof\n");
  return 42;
}

/* Sub-run E: self-spawn in echo mode through the full pipe plumbing. */
static void run_echo_child(void) {
  int a[2], b[2];
  char buf[512];
  int tries = 0;
  ssize_t w;
  int pid;

  print_console("[PP] E: echo child\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] E pipes failed\n");
    return;
  }
  pid = spawn_retry("PIPEPRB.BIN", a[0], b[1], -1, "echo");
  pnum("E pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    w = write_all(a[1], "hi\n", &tries);
    pnum("E write=", (int)w);
    pnum("tries=", tries);
    print_console("\n");
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("E got=", (int)n);
    ptxt(" data=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      int wr = waitpid(pid, &ws, 0);
      pnum("E wre=", wr);
      pnum("ws=", ws);
      pnum("code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Sub-run F: exit-status probes through the standard spawn path.  Each
 * prints the raw waitpid status + WEXITSTATUS so a mismatch pinpoints
 * whether sed itself or the kernel/harness status plumbing is at fault. */
static void run_one_status(const char *tag, const char *args) {
  int a[2], b[2];
  char buf[256];
  int pid;
  int fd = open("SEDT.IN", O_WRONLY | O_CREAT | O_TRUNC);
  if (fd >= 0) {
    write(fd, "alpha\nbeta\ngamma\ndelta\n", 22);
    close(fd);
  }
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] status pipes failed\n");
    return;
  }
  pid = spawn_retry("SED.BIN", a[0], b[1], -1, args);
  pnum(tag, pid);
  if (pid > 0) {
    close(a[0]);
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum(" got=", (int)n);
    ptxt(" out=\"", buf, n);
    close(b[0]);
    {
      int ws = 0;
      int wr = waitpid(pid, &ws, 0);
      pnum(" wre=", wr);
      pnum(" ws=", ws);
      pnum(" code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  } else {
    print_console("\n");
  }
}

/* Sub-run I: in-place edit (the inplace_i case) — then read SEDT.IN back. */
static void run_inplace(void) {
  int a[2], b[2];
  char buf[256];
  int pid;
  int fd = open("SEDT.IN", O_WRONLY | O_CREAT | O_TRUNC);
  if (fd >= 0) {
    write(fd, "alpha\nbeta\ngamma\ndelta\n", 22);
    close(fd);
  }
  print_console("[PP] I: inplace\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] I pipes failed\n");
    return;
  }
  pid = spawn_retry("SED.BIN", a[0], b[1], -1, "-i s/beta/BETA/ SEDT.IN");
  pnum("I pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    close(a[1]);
    close(b[1]);
    drain(b[0], buf, sizeof buf);
    close(b[0]);
    {
      int ws = 0;
      waitpid(pid, &ws, 0);
      pnum("I code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
  fd = open("SEDT.IN", O_RDONLY);
  if (fd >= 0) {
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r > 0) {
      ptxt("I SEDT.IN=\"", buf, (size_t)r);
    } else {
      pnum("I read back=", (int)r);
      print_console("\n");
    }
  } else {
    print_console("I SEDT.IN open failed\n");
  }
}

/* Sub-run J: minimal exit-code end-to-end — spawn ourselves in exit7 mode
   and print the raw waitpid result.  No sed, no pipes beyond the default. */
static void run_exit7(void) {
  int a[2], b[2];
  char buf[64];
  int pid;

  print_console("[PP] J: exit7\n");
  if (pipe(a) != 0 || pipe(b) != 0) {
    print_console("[PP] J pipes failed\n");
    return;
  }
  pid = spawn_retry("PIPEPRB.BIN", a[0], b[1], -1, "exit7");
  pnum("J pid=", pid);
  print_console("\n");
  if (pid > 0) {
    close(a[0]);
    close(a[1]);
    close(b[1]);
    size_t n = drain(b[0], buf, sizeof buf);
    pnum("J drain=", (int)n);
    close(b[0]);
    {
      int ws = 0;
      int wr = waitpid(pid, &ws, 0);
      pnum("J wre=", wr);
      pnum("ws=", ws);
      pnum("code=", WEXITSTATUS(ws));
      print_console("\n");
    }
  }
}

/* Sub-run K: write a known 23-byte file at the FAT root, read it straight
   back, and print both with escapes.  Isolates the one-byte corruption the
   sed golden suite sees in SEDT.IN ("gamma" -> "\0amma", offset 11, the
   exact position of the pair-data NUL in the generated case tables). */
static void run_fat_repro(void) {
  static const char payload[] = "alpha\nbeta\ngamma\ndelta\n";
  char buf[64];
  ssize_t n = 0;
  int fd, r, i, w;

  print_console("[PP] K: fat repro\n");
  pnum("K strlen=", (int)(sizeof payload - 1));
  print_console("\n");
  unlink("SEDT.RPR");
  fd = open("SEDT.RPR", O_WRONLY | O_CREAT | O_TRUNC);
  if (fd < 0) {
    print_console("[PP] K create failed\n");
    return;
  }
  w = (int)write(fd, payload, sizeof payload - 1);
  pnum("K wrote=", w);
  close(fd);
  fd = open("SEDT.RPR", O_RDONLY);
  if (fd < 0) {
    print_console("[PP] K open-ro failed\n");
    return;
  }
  while (n < (ssize_t)sizeof buf - 1) {
    r = (int)read(fd, buf + n, sizeof buf - 1 - (size_t)n);
    if (r <= 0) break;
    n += r;
  }
  close(fd);
  pnum("K readback=", (int)n);
  print_console(" [");
  for (i = 0; i < (int)n; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (c == '\n') {
      print_console("\\n");
    } else if (c < 32 || c > 126) {
      pnum("\\x", (int)c);
    } else {
      char one[2];
      one[0] = (char)c;
      one[1] = '\0';
      print_console(one);
    }
  }
  print_console("]\n");
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "echo") == 0) {
      return echo_child();
    }
    if (strcmp(argv[i], "exit7") == 0) {
      print_console("[EXIT7] returning 7\n");
      return 7;
    }
  }
  print_console("[PIPEPROBE] start\n");
  run_prefill();
  run_after_stderr();
  run_file();
  run_settled();
  run_echo_child();
  run_one_status("F quit_status", "-n 2q5 SEDT.IN");
  run_one_status("G bad_script", "s/broken SEDT.IN");
  run_one_status("H nonexistent", "s/a/b/ SEDT.NOPE");
  run_inplace();
  run_exit7();
  run_fat_repro();
  print_console("[PIPEPROBE] done\n");
  return 0;
}
