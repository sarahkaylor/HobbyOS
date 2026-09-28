/* dirname -- print the directory part of a pathname.
   Fresh port with loose GNU coreutils parity. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  const char *path = NULL;
  int i;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] == '\0')
      continue;
    if (!path) path = argv[i];
  }
  if (path == NULL) {
    fprintf(stderr, "dirname: missing operand\n");
    return 1;
  }
  if (path[0] == '\0') {
    printf(".\n");
    return 0;
  }

  /* ignore trailing slashes */
  const char *end = path + strlen(path);
  while (end > path && end[-1] == '/') end--;
  if (end == path) {
    printf("/\n");
    return 0;
  }
  /* last slash in [path, end) */
  const char *last = NULL;
  for (const char *p = path; p < end; p++) {
    if (*p == '/') last = p;
  }
  if (last == NULL) {
    printf(".\n");
    return 0;
  }
  if (last == path) {
    printf("/\n");
    return 0;
  }
  /* drop trailing slashes inside the directory part (a//b -> a) */
  const char *e2 = last;
  while (e2 > path && e2[-1] == '/') e2--;
  if (e2 == path) {
    printf("/\n");
    return 0;
  }
  printf("%.*s\n", (int)(e2 - path), path);
  return 0;
}
