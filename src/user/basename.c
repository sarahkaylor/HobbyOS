/* basename -- strip directory and suffix from a pathname.
   Fresh port with loose GNU coreutils parity for the HobbyOS userland.
   Link against libc.a (crt0 provides main(argc, argv)). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *path = NULL;
  const char *suffix = NULL;
  int have_suffix = 0;
  int i;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == 's' && argv[i][2] == '\0') {
      if (i + 1 >= argc) {
        fprintf(stderr, "basename: option requires an argument -- 's'\n");
        return 1;
      }
      suffix = argv[++i];
      have_suffix = 1;
      continue;
    }
    if (argv[i][0] == '-' && argv[i][1] == 'a' && argv[i][2] == '\0')
      continue;
    if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == '\0')
      continue;
    if (path == NULL) {
      path = argv[i];
    } else if (!have_suffix) {
      suffix = argv[i];
      have_suffix = 1;
    }
  }
  if (path == NULL) {
    fprintf(stderr, "basename: missing operand\n");
    return 1;
  }
  const char *end_path;

  /* ignore trailing slashes, but keep the root "/" itself */
  end_path = path + strlen(path);
  while (end_path > path && end_path[-1] == '/') end_path--;
  if (end_path == path) {
    printf("/\n");
    return 0;
  }
  /* base = the component after the last slash in [path, end_path) */
  const char *base = path;
  for (const char *p = path; p < end_path; p++) {
    if (*p == '/') base = p + 1;
  }
  size_t len = (size_t)(end_path - base);

  if (suffix && suffix[0]) {
    size_t sl = strlen(suffix);
    if (len >= sl && strncmp(base + (len - sl), suffix, sl) == 0)
      len -= sl;
  }

  fwrite(base, 1, len, stdout);
  fputc('\n', stdout);
  return 0;
}
