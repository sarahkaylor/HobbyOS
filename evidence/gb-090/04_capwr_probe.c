/* GB-090 probe: write more than console_write_user's STAGE_CAP (8192) in ONE
 * SYS_WRITE_CONSOLE call (print_console is that path), with a checkable pattern.
 * Total payload 8320 bytes, bracketed by markers so the serial region can be
 * verified byte-exact offline. Temporary — not for commit. */
extern void print_console(const char *s);
extern void exit(int code);

static char buf[8321];
static const char *BEGIN = "CAPWR-BEGIN\n";
static const char *END = "CAPWR-END\n";

__attribute__((section(".text._start"))) void _start(void) {
  const char *digits = "0123456789";
  for (int i = 0; i < 8320; i++)
    buf[i] = digits[i % 10];
  buf[8320] = '\0';
  print_console(BEGIN);
  print_console(buf);   /* ONE call, 8320 bytes > STAGE_CAP */
  print_console(END);
  exit(0);
}
