/* src/user/subprobe.c — in-OS ground truth for the subdirectory file flow.
 *
 * shell_test3 fails deterministically at "echo nested_content_val >
 * nested.txt" inside /SUB1/SUB2 followed by "cat nested.txt": the redirect
 * open reports no error, yet the spawned cat cannot find the file.  This
 * probe replays the exact sequence without the shell:
 *
 *   A: mkdir /SUB1, mkdir /SUB1/SUB2, chdir, print getcwd.
 *   B: relative create ("nested.txt"), write, close, reopen, read back.
 *   C: absolute open ("/SUB1/SUB2/nested.txt") read back.
 *   D: spawn CAT.BIN with our cwd and args "nested.txt" (the failing
 *      shell flow), capture stdout + exit status.
 *
 * Every stage prints fd/errno/bytes so the first divergence from the
 * expected path is unambiguous in the UART log.  Runs early in
 * KERNEL_MODE_TEST, before the shell tests, so the log is still lossless.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "libc.h"

extern int errno;

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
  print_console("\"");
}

int main(void) {
  char cwd[128];
  char buf[256];
  int fd, n, r;

  print_console("[SUBPRB] start\n");

  /* A: build the nested directory tree and enter it. */
  errno = 0;
  r = mkdir("/SUB1");
  pnum("A mkdir /SUB1 r=", r);
  pnum("errno=", errno);
  print_console("\n");
  errno = 0;
  r = mkdir("/SUB1/SUB2");
  pnum("A mkdir /SUB1/SUB2 r=", r);
  pnum("errno=", errno);
  print_console("\n");
  errno = 0;
  r = chdir("/SUB1/SUB2");
  pnum("A chdir r=", r);
  pnum("errno=", errno);
  print_console(" ");
  if (getcwd(cwd, sizeof cwd)) {
    ptxt("cwd=\"", cwd, strlen(cwd));
  }
  if (r != 0) {
    print_console("\n[SUBPRB] chdir failed; aborting\n");
    return 1;
  }
  print_console("\n");

  /* B: relative create + immediate reread (the shell's redirect flow). */
  errno = 0;
  fd = open("nested.txt", O_WRONLY | O_CREAT | O_TRUNC);
  pnum("B create fd=", fd);
  pnum("errno=", errno);
  if (fd >= 0) {
    n = (int)write(fd, "probe-content\n", 14);
    pnum("wrote=", n);
    close(fd);
  }
  print_console("\n");
  errno = 0;
  fd = open("nested.txt", O_RDONLY);
  pnum("B reread fd=", fd);
  pnum("errno=", errno);
  if (fd >= 0) {
    n = (int)read(fd, buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = '\0';
    }
    pnum("got=", n);
    print_console(" ");
    ptxt("data=\"", buf, n > 0 ? (size_t)n : 0);
    close(fd);
  }
  print_console("\n");

  /* C: the same file through its absolute path. */
  errno = 0;
  fd = open("/SUB1/SUB2/nested.txt", O_RDONLY);
  pnum("C abs fd=", fd);
  pnum("errno=", errno);
  if (fd >= 0) {
    n = (int)read(fd, buf, sizeof buf - 1);
    pnum("got=", n);
    close(fd);
  }
  print_console("\n");

  /* D: spawn CAT.BIN exactly like the shell does, from this cwd. */
  {
    int b[2];
    int pid;
    if (pipe(b) != 0) {
      print_console("D pipe failed\n");
    } else {
      pid = spawn2("CAT.BIN", 0, b[1], -1, "nested.txt");
      pnum("D spawn pid=", pid);
      print_console("\n");
      if (pid > 0) {
        /* Drain before dropping the read end; only the parent's copy of
           the write end is closed here so the child keeps its dup. */
        close(b[1]);
        size_t tn = 0;
        while (tn + 1 < sizeof buf) {
          ssize_t g = read(b[0], buf + tn, sizeof buf - 1 - tn);
          if (g <= 0) break;
          tn += (size_t)g;
        }
        buf[tn] = '\0';
        pnum("D cat got=", (int)tn);
        ptxt(" out=\"", buf, tn);
        close(b[0]);
        {
          int ws = 0;
          waitpid(pid, &ws, 0);
          pnum(" code=", WEXITSTATUS(ws));
          print_console("\n");
        }
      } else {
        close(b[0]);
        close(b[1]);
      }
    }
  }

  /* E: the shell_test2 file-op chain, replayed without the shell:
     create -> mv -> cp -> rm -> cat, with a direct read after each
     step so a broken rung is unambiguous. */
  {
    const char *content = "file_content\n";
    static const char *const spawns[][2] = {{"/MV.BIN", "TEMP.TXT TEMP2.TXT"},
      {"/CP.BIN", "TEMP2.TXT TEMP3.TXT"},
      {"/RM.BIN", "TEMP2.TXT"},
      {"/CAT.BIN", "TEMP3.TXT"}};
    const char *reads[3] = {"/TEMP.TXT", "/TEMP2.TXT", "/TEMP3.TXT"};

    print_console("E start\n");
    if (chdir("/") != 0) {
      print_console("E chdir / failed\n");
    }
    fd = open("/TEMP.TXT", O_WRONLY | O_CREAT | O_TRUNC);
    pnum("E create fd=", fd);
    if (fd >= 0) {
      n = (int)write(fd, content, strlen(content));
      pnum("wrote=", n);
      close(fd);
    }
    print_console("\n");

    for (size_t i = 0; i < 4; i++) {
      int pid2 = spawn2(spawns[i][0], 0, 1, -1, spawns[i][1]);
      pnum("E spawn ", (int)i);
      print_console(spawns[i][0]);
      pnum(" pid=", pid2);
      if (pid2 > 0) {
        int ws2 = 0;
        waitpid(pid2, &ws2, 0);
        pnum(" code=", WEXITSTATUS(ws2));
      }
      print_console("\n");
    }

    for (size_t i = 0; i < 3; i++) {
      errno = 0;
      fd = open(reads[i], O_RDONLY);
      pnum("E read ", (int)i);
      print_console(reads[i]);
      pnum(" fd=", fd);
      pnum("errno=", errno);
      if (fd >= 0) {
        n = (int)read(fd, buf, sizeof buf - 1);
        if (n > 0) buf[n] = '\0';
        pnum(" n=", n);
        print_console(" ");
        ptxt("data=\"", buf, n > 0 ? (size_t)n : 0);
        close(fd);
      }
      print_console("\n");
    }
  }

  print_console("[SUBPRB] done\n");
  return 0;
}
