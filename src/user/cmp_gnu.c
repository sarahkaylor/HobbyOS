/* cmp -- compare two files
   Copyright (C) 1987-2002 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

/* Written by Torbjorn Granlund and David MacKenzie. */

/* Compare two files byte by byte.  Options: -b (print differing bytes),
   -c (obsolescent synonym), -i SKIP[:SKIP2] (ignore initial bytes),
   -l (print all differing bytes), -n LIMIT (compare at most LIMIT
   bytes), -s (status only).  Exit 0 if identical, 1 if different,
   2 on trouble.  */

/* HobbyOS port: faithful copy of the GNU diffutils-2.8.1 src/cmp.c with
   the gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc -> the sysroot
   malloc, and the inlined helpers from lib/ (block_read from cmpbuf.c,
   xstrtoumax from xstrtol.c, offtostr from inttostr.c, same_file from
   system.h) transcribed below.  Dropped: NLS/locale machinery (single C
   locale: the "char" message variant is always used, matching the
   byte-exact GNU behaviour under LC_ALL=C), the c-stack action, and
   set_binary_mode (no-op outside Windows).  The block compare reads
   bytes rather than machine words; output is identical, the constant
   factor is not.  buf_size comes from a fixed 64 KB block instead of
   fstat's blksize, which no GNU output depends on.  The host build
   (Makefile cmp_host) races this against the reference build in
   src/host/cmp_parity.sh; byte-exact output is the bar.  */

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PROGRAM_NAME "cmp"
#define PACKAGE_BUGREPORT "bug-gnu-utils@gnu.org"

typedef uintmax_t word;

#define STREQ(a, b) (strcmp (a, b) == 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ISPRINT(c) ((unsigned char) (c) >= 32 && (unsigned char) (c) < 127)
#define ISSPACE(c) ((c) == ' ' || (c) == '\t' || (c) == '\n' || \
                    (c) == '\v' || (c) == '\f' || (c) == '\r')

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define EXIT_TROUBLE 2

char const version_string[] = "(GNU diffutils) 2.8.1";

static char const copyright_string[] =
  "Copyright (C) 2002 Free Software Foundation, Inc.";

static char const free_software_msgid[] =
  "This program comes with NO WARRANTY, to the extent permitted by law.\n\
You may redistribute copies of this program\n\
under the terms of the GNU General Public License.\n\
For more information about these matters, see the file named COPYING.";

static char const authorship_msgid[] =
  "Written by Torbjorn Granlund and David MacKenzie.";

/* ---- lib/transcriptions ---------------------------------------------- */

/* From lib/cmpbuf.c: read what we can, retrying after EINTR.  Returns
   the byte count, or SIZE_MAX on error.  */

static size_t block_read (int fd, char *buf, size_t n) {
  char *bp = buf;
  size_t bytes_left = n;
  ssize_t nr;

  while ((nr = read (fd, bp, bytes_left)) != 0) {
    if (nr < 0) {
      if (errno == EINTR)
        continue;
      return SIZE_MAX;
    }
    bp += nr;
    bytes_left -= nr;
    if (bytes_left == 0)
      break;
  }

  return bp - buf;
}

/* From lib/xstrtol.c: strict uintmax parse with the diffutils suffix
   grammar (kKMGTPEZY plus the '0' flag for the optional B/iB second
   suffix that selects base 1000 vs 1024).  */

enum strtol_error {
  LONGINT_OK = 0,
  LONGINT_OVERFLOW = 1,
  LONGINT_INVALID_SUFFIX_CHAR = 2,
  LONGINT_INVALID = 3
};

static int bkm_scale (uintmax_t *x, int scale_factor) {
  uintmax_t product = *x * scale_factor;
  if (*x != product / scale_factor)
    return 1;
  *x = product;
  return 0;
}

static int bkm_scale_by_power (uintmax_t *x, int base, int power) {
  while (power--)
    if (bkm_scale (x, base))
      return 1;

  return 0;
}

static enum strtol_error xstrtoumax (const char *s, char **ptr, int strtol_base, uintmax_t *val,
            const char *valid_suffixes) {
  char *t_ptr;
  char **p;
  uintmax_t tmp;
  const char *q;

  p = (ptr ? ptr : &t_ptr);

  q = s;
  while (ISSPACE ((unsigned char) *q))
    ++q;
  if (*q == '-')
    return LONGINT_INVALID;

  errno = 0;
  tmp = strtoull (s, p, strtol_base);
  if (errno != 0)
    return LONGINT_OVERFLOW;

  if (*p == s) {
    if (valid_suffixes && **p && strchr (valid_suffixes, **p))
      tmp = 1;
    else
      return LONGINT_INVALID;
  }

  if (**p != '\0') {
    int base = 1024;
    int suffixes = 1;
    int overflow;

    if (!strchr (valid_suffixes, **p)) {
      *val = tmp;
      return LONGINT_INVALID_SUFFIX_CHAR;
    }

    if (strchr (valid_suffixes, '0')) {
      switch (p[0][1]) {
      case 'i':
        if (p[0][2] == 'B')
          suffixes += 2;
        break;

      case 'B':
        ++suffixes;
        break;
      }

      if ((*p)[suffixes] == 'B')
        base = 1000;
    }

    switch ((*p)[0]) {
    case 'b':
      overflow = bkm_scale (&tmp, 512);
      break;

    case 'B':
      overflow = bkm_scale (&tmp, 1024);
      break;

    case 'c':
      overflow = 0;
      break;

    case 'E':
      overflow = bkm_scale_by_power (&tmp, base, 6);
      break;

    case 'G':
    case 'g':
      overflow = bkm_scale_by_power (&tmp, base, 3);
      break;

    case 'k':
    case 'K':
      overflow = bkm_scale_by_power (&tmp, base, 1);
      break;

    case 'M':
    case 'm':
      overflow = bkm_scale_by_power (&tmp, base, 2);
      break;

    case 'P':
      overflow = bkm_scale_by_power (&tmp, base, 5);
      break;

    case 'T':
    case 't':
      overflow = bkm_scale_by_power (&tmp, base, 4);
      break;

    case 'w':
      overflow = bkm_scale (&tmp, 2);
      break;

    case 'Y':
      overflow = bkm_scale_by_power (&tmp, base, 8);
      break;

    case 'Z':
      overflow = bkm_scale_by_power (&tmp, base, 7);
      break;

    default:
      *val = tmp;
      return LONGINT_INVALID_SUFFIX_CHAR;
    }

    if (overflow)
      return LONGINT_OVERFLOW;

    (*p) += suffixes;
  }

  *val = tmp;
  return LONGINT_OK;
}

/* From lib/inttostr.c: stringify an off_t into a caller buffer.  */

#define INT_BUFSIZE_BOUND(t) (sizeof "-9223372036854775808")

static char *
offtostr (off_t i, char *buf) {
  sprintf (buf, "%lld", (long long) i);
  return buf;
}

/* From src/system.h: true if two stat buffers describe the same file. */

static int same_file (struct stat const *a, struct stat const *b) {
  return a->st_ino == b->st_ino && a->st_dev == b->st_dev;
}

static int same_file_attributes (struct stat const *a, struct stat const *b) {
  return (a->st_mode == b->st_mode && a->st_size == b->st_size
          && a->st_mtime == b->st_mtime);
}

static size_t buffer_lcm (size_t a, size_t b) {
  /* cmp only uses the result to size buffers; any sensible block size
     keeps the byte stream identical, so pick a fixed 64 KB.  */
  (void) a;
  (void) b;
  return 64 * 1024;
}

/* ---- the program ------------------------------------------------------ */

/* Name under which this program was invoked.  */
char *program_name;

static int cmp (void);
static off_t file_position (int);
static size_t block_compare (char const *, char const *);
static size_t block_compare_and_count (char const *, char const *, off_t *);
static void sprintc (char *, unsigned char);

/* Filenames of the compared files.  */
static char const *file[2];

/* File descriptors of the files.  */
static int file_desc[2];

/* Status of the files.  */
static struct stat stat_buf[2];

/* Read buffers for the files.  */
static word *buffer[2];

/* Optimal block size for the files.  */
static size_t buf_size;

/* Initial prefix to ignore for each file.  */
static off_t ignore_initial[2];

/* Number of bytes to compare.  */
static uintmax_t bytes = UINTMAX_MAX;

/* Output format.  */
static enum comparison_type {
  type_first_diff,    /* Print the first difference.  */
  type_all_diffs,     /* Print all differences.  */
  type_status         /* Exit status only.  */
} comparison_type;

/* If nonzero, print values of bytes quoted like cat -t does. */
static bool opt_print_bytes;

/* Values for long options that do not have single-letter equivalents.  */
enum {
  HELP_OPTION = CHAR_MAX + 1
};

static struct option const long_options[] = { {"print-bytes", 0, 0, 'b'}, {"print-chars", 0, 0, 'c'}, /* obsolescent as of diffutils 2.7.3 */ {"ignore-initial", 1, 0, 'i'}, {"verbose", 0, 0, 'l'}, {"bytes", 1, 0, 'n'}, {"silent", 0, 0, 's'}, {"quiet", 0, 0, 's'}, {"version", 0, 0, 'v'}, {"help", 0, 0, HELP_OPTION}, {0, 0, 0, 0}
};

static void try_help (char const *, char const *) __attribute__((noreturn));
static void try_help (char const *reason_msgid, char const *operand) {
  if (reason_msgid)
    error (0, 0, reason_msgid, operand);
  error (EXIT_TROUBLE, 0,
         "Try `%s --help' for more information.", program_name);
  abort ();
}

static char const valid_suffixes[] = "kKMGTPEZY0";

/* Parse an operand *ARGPTR of --ignore-initial, updating *ARGPTR to
   point after the operand.  If DELIMITER is nonzero, the operand may
   be followed by DELIMITER; otherwise it must be null-terminated.  */
static off_t parse_ignore_initial (char **argptr, char delimiter) {
  uintmax_t val;
  off_t o;
  char const *arg = *argptr;
  enum strtol_error e = xstrtoumax (arg, argptr, 0, &val, valid_suffixes);
  if (! (e == LONGINT_OK
         || (e == LONGINT_INVALID_SUFFIX_CHAR && **argptr == delimiter))
      || (o = val) < 0 || (uintmax_t) o != val || val == UINTMAX_MAX)
    try_help ("invalid --ignore-initial value `%s'", arg);
  return o;
}

/* Specify the output format.  */
static void specify_comparison_type (enum comparison_type t) {
  if (comparison_type)
    try_help ("options -l and -s are incompatible", 0);
  comparison_type = t;
}

static void check_stdout (void) {
  if (ferror (stdout))
    error (EXIT_TROUBLE, 0, "%s", "write failed");
  else if (fclose (stdout) != 0)
    error (EXIT_TROUBLE, errno, "%s", "standard output");
}

static char const * const option_help_msgid[] = {
  "-b  --print-bytes  Print differing bytes.",
  "-i SKIP  --ignore-initial=SKIP  Skip the first SKIP bytes of input.",
  "-i SKIP1:SKIP2  --ignore-initial=SKIP1:SKIP2",
  "  Skip the first SKIP1 bytes of FILE1 and the first SKIP2 bytes of FILE2.",
  "-l  --verbose  Output byte numbers and values of all differing bytes.",
  "-n LIMIT  --bytes=LIMIT  Compare at most LIMIT bytes.",
  "-s  --quiet  --silent  Output nothing; yield exit status only.",
  "-v  --version  Output version info.",
  "--help  Output this help.",
  0
};

static void usage (void) {
  char const * const *p;

  printf ("Usage: %s [OPTION]... FILE1 [FILE2 [SKIP1 [SKIP2]]]\n",
          program_name);
  printf ("%s\n\n", "Compare two files byte by byte.");
  for (p = option_help_msgid;  *p;  p++)
    printf ("  %s\n", *p);
  printf ("\n%s\n%s\n\n%s\n\n%s\n",
          "SKIP1 and SKIP2 are the number of bytes to skip in each file.",
          "SKIP values may be followed by the following multiplicative suffixes:\n\
kB 1000, K 1024, MB 1,000,000, M 1,048,576,\n\
GB 1,000,000,000, G 1,073,741,824, and so on for T, P, E, Z, Y.",
          "If a FILE is `-' or missing, read standard input.",
          "Report bugs to <" PACKAGE_BUGREPORT ">.");
}

int main (int argc, char **argv) {
  int c, f, exit_status;
  size_t words_per_buffer;

  program_name = argv[0];

  /* Parse command line options.  */

  while ((c = getopt_long (argc, argv, "bci:ln:sv", long_options, 0))
         != -1)
    switch (c) {
    case 'b':
    case 'c': /* 'c' is obsolescent as of diffutils 2.7.3 */
      opt_print_bytes = 1;
      break;

    case 'i':
      ignore_initial[0] = parse_ignore_initial (&optarg, ':');
      ignore_initial[1] = (*optarg++ == ':'
                           ? parse_ignore_initial (&optarg, 0)
                           : ignore_initial[0]);
      break;

    case 'l':
      specify_comparison_type (type_all_diffs);
      break;

    case 'n': {
        uintmax_t n;
        if (xstrtoumax (optarg, 0, 0, &n, valid_suffixes) != LONGINT_OK)
          try_help ("invalid --bytes value `%s'", optarg);
        if (n < bytes)
          bytes = n;
      }
      break;

    case 's':
      specify_comparison_type (type_status);
      break;

    case 'v':
      printf ("cmp %s\n%s\n\n%s\n\n%s\n",
              version_string, copyright_string,
              free_software_msgid, authorship_msgid);
      check_stdout ();
      return EXIT_SUCCESS;

    case HELP_OPTION:
      usage ();
      check_stdout ();
      return EXIT_SUCCESS;

    default:
      try_help (0, 0);
    }

  if (optind == argc)
    try_help ("missing operand after `%s'", argv[argc - 1]);

  file[0] = argv[optind++];
  file[1] = optind < argc ? argv[optind++] : "-";

  for (f = 0; f < 2 && optind < argc; f++) {
    char *arg = argv[optind++];
    ignore_initial[f] = parse_ignore_initial (&arg, 0);
  }

  if (optind < argc)
    try_help ("extra operand `%s'", argv[optind]);

  for (f = 0; f < 2; f++) {
    /* If file[1] is "-", treat it first; this avoids a misdiagnostic if
       stdin is closed and opening file[0] yields file descriptor 0.  */
    int f1 = f ^ (strcmp (file[1], "-") == 0);

    /* Two files with the same name are identical.
       But wait until we open the file once, for proper diagnostics.  */
    if (f && STREQ (file[0], file[1]))
      return EXIT_SUCCESS;

    file_desc[f1] = (strcmp (file[f1], "-") == 0
                     ? STDIN_FILENO
                     : open (file[f1], O_RDONLY, 0));
    if (file_desc[f1] < 0 || fstat (file_desc[f1], stat_buf + f1) != 0) {
      if (file_desc[f1] < 0 && comparison_type == type_status)
        exit (EXIT_TROUBLE);
      else
        error (EXIT_TROUBLE, errno, "%s", file[f1]);
    }
  }

  /* If the files are links to the same inode and have the same file position,
     they are identical.  */
  if (0 < same_file (&stat_buf[0], &stat_buf[1])
      && same_file_attributes (&stat_buf[0], &stat_buf[1])
      && file_position (0) == file_position (1))
    return EXIT_SUCCESS;

  /* If only a return code is needed,
     and if both input descriptors are associated with plain files,
     conclude that the files differ if they have different sizes
     and if more bytes will be compared than are in the smaller file.  */
  if (comparison_type == type_status
      && S_ISREG (stat_buf[0].st_mode)
      && S_ISREG (stat_buf[1].st_mode)) {
    off_t s0 = stat_buf[0].st_size - file_position (0);
    off_t s1 = stat_buf[1].st_size - file_position (1);
    if (s0 < 0)
      s0 = 0;
    if (s1 < 0)
      s1 = 0;
    if (s0 != s1 && (uintmax_t) MIN (s0, s1) < bytes)
      exit (EXIT_FAILURE);
  }

  /* Get the optimal block size of the files.  */

  buf_size = buffer_lcm (stat_buf[0].st_blksize, stat_buf[1].st_blksize);

  /* Allocate word-aligned buffers, with space for sentinels at the end.  */

  words_per_buffer = (buf_size + 2 * sizeof (word) - 1) / sizeof (word);
  buffer[0] = malloc (2 * sizeof (word) * words_per_buffer);
  buffer[1] = buffer[0] + words_per_buffer;

  exit_status = cmp ();

  for (f = 0; f < 2; f++)
    if (close (file_desc[f]) != 0)
      error (EXIT_TROUBLE, errno, "%s", file[f]);
  if (exit_status != 0  &&  comparison_type != type_status)
    check_stdout ();
  exit (exit_status);
  return exit_status;
}

/* Compare the two files already open on `file_desc[0]' and `file_desc[1]',
   using `buffer[0]' and `buffer[1]'.
   Return EXIT_SUCCESS if identical, EXIT_FAILURE if different,
   >1 if error.  */

static int cmp (void) {
  off_t line_number = 1;        /* Line number (1...) of difference. */
  off_t byte_number = 1;        /* Byte number (1...) of difference. */
  uintmax_t remaining = bytes;  /* Remaining number of bytes to compare.  */
  size_t read0, read1;          /* Number of bytes read from each file. */
  size_t first_diff;            /* Offset (0...) in buffers of 1st diff. */
  size_t smaller;               /* The lesser of `read0' and `read1'. */
  word *buffer0 = buffer[0];
  word *buffer1 = buffer[1];
  char *buf0 = (char *) buffer0;
  char *buf1 = (char *) buffer1;
  int ret = EXIT_SUCCESS;
  int f;
  int offset_width;

  if (comparison_type == type_all_diffs) {
    off_t byte_number_max = MIN (bytes, (uintmax_t) INT32_MAX);

    for (f = 0; f < 2; f++)
      if (S_ISREG (stat_buf[f].st_mode)) {
        off_t file_bytes = stat_buf[f].st_size - file_position (f);
        if (file_bytes < byte_number_max)
          byte_number_max = file_bytes;
      }

    for (offset_width = 1; (byte_number_max /= 10) != 0; offset_width++)
      continue;
  }

  for (f = 0; f < 2; f++) {
    off_t ig = ignore_initial[f];
    if (ig && file_position (f) == -1) {
      /* lseek failed; read and discard the ignored initial prefix.  */
      do {
        size_t bytes_to_read = MIN ((size_t) ig, buf_size);
        size_t r = block_read (file_desc[f], buf0, bytes_to_read);
        if (r != bytes_to_read) {
          if (r == SIZE_MAX)
            error (EXIT_TROUBLE, errno, "%s", file[f]);
          break;
        }
        ig -= r;
      } while (ig);
    }
  }

  do {
    size_t bytes_to_read = buf_size;

    if (remaining != UINTMAX_MAX) {
      if (remaining < bytes_to_read)
        bytes_to_read = remaining;
      remaining -= bytes_to_read;
    }

    read0 = block_read (file_desc[0], buf0, bytes_to_read);
    if (read0 == SIZE_MAX)
      error (EXIT_TROUBLE, errno, "%s", file[0]);
    read1 = block_read (file_desc[1], buf1, bytes_to_read);
    if (read1 == SIZE_MAX)
      error (EXIT_TROUBLE, errno, "%s", file[1]);

    /* Insert sentinels for the block compare.  */

    buf0[read0] = ~buf1[read0];
    buf1[read1] = ~buf0[read1];

    /* If the line number should be written for differing files,
       compare the blocks and count the number of newlines
       simultaneously.  */
    first_diff = (comparison_type == type_first_diff
                  ? block_compare_and_count (buf0, buf1, &line_number)
                  : block_compare (buf0, buf1));

    byte_number += first_diff;
    smaller = MIN (read0, read1);

    if (first_diff < smaller) {
      switch (comparison_type) {
      case type_first_diff: {
          char byte_buf[INT_BUFSIZE_BOUND (off_t)];
          char line_buf[INT_BUFSIZE_BOUND (off_t)];
          char const *byte_num = offtostr (byte_number, byte_buf);
          char const *line_num = offtostr (line_number, line_buf);
          if (!opt_print_bytes) {
            /* See POSIX 1003.1-2001 for this format.  This
               message is used only in the POSIX locale, so it
               need not be translated.  */
            printf ("%s %s differ: char %s, line %s\n",
                    file[0], file[1], byte_num, line_num);
          } else {
            unsigned char c0 = buf0[first_diff];
            unsigned char c1 = buf1[first_diff];
            char s0[5];
            char s1[5];
            sprintc (s0, c0);
            sprintc (s1, c1);
            printf ("%s %s differ: byte %s, line %s is %3o %s %3o %s\n",
                    file[0], file[1], byte_num, line_num,
                    c0, s0, c1, s1);
          }
        }
      /* Fall through.  */
      case type_status:
        return EXIT_FAILURE;

      case type_all_diffs:
        do {
          unsigned char c0 = buf0[first_diff];
          unsigned char c1 = buf1[first_diff];
          if (c0 != c1) {
            char byte_buf[INT_BUFSIZE_BOUND (off_t)];
            char const *byte_num = offtostr (byte_number, byte_buf);
            if (!opt_print_bytes) {
              /* See POSIX 1003.1-2001 for this format.  */
              printf ("%*s %3o %3o\n",
                      offset_width, byte_num, c0, c1);
            } else {
              char s0[5];
              char s1[5];
              sprintc (s0, c0);
              sprintc (s1, c1);
              printf ("%*s %3o %-4s %3o %s\n",
                      offset_width, byte_num, c0, s0, c1, s1);
            }
          }
          byte_number++;
          first_diff++;
        } while (first_diff < smaller);
        ret = EXIT_FAILURE;
        break;
      }
    }

    if (read0 != read1) {
      if (comparison_type != type_status) {
        /* See POSIX 1003.1-2001 for this format.  */
        fprintf (stderr, "cmp: EOF on %s\n", file[read1 < read0]);
      }

      return EXIT_FAILURE;
    }
  } while (read0 == buf_size);

  return ret;
}

/* Compare two blocks of memory P0 and P1 until they differ,
   and count the number of '\n' occurrences in the common
   part of P0 and P1.
   If the blocks are not guaranteed to be different, put sentinels at the ends
   of the blocks before calling this function.

   Return the offset of the first byte that differs.
   Increment *COUNT by the count of '\n' occurrences.  */

static size_t block_compare_and_count (char const *p0, char const *p1, off_t *count) {
  char const *c0, *c1;          /* Pointers for finding exact address.  */
  size_t cnt = 0;               /* Number of '\n' occurrences.  */

  for (c0 = p0, c1 = p1;  *c0 == *c1;  c0++, c1++)
    cnt += *c0 == '\n';

  *count += cnt;
  return c0 - p0;
}

/* Compare two blocks of memory P0 and P1 until they differ.
   If the blocks are not guaranteed to be different, put sentinels at the ends
   of the blocks before calling this function.

   Return the offset of the first byte that differs.  */

static size_t block_compare (char const *p0, char const *p1) {
  char const *c0, *c1;

  for (c0 = p0, c1 = p1;  *c0 == *c1;  c0++, c1++)
    continue;

  return c0 - p0;
}

/* Put into BUF the unsigned char C, making unprintable bytes
   visible by quoting like cat -t does.  */

static void sprintc (char *buf, unsigned char c) {
  if (! ISPRINT (c)) {
    if (c >= 128) {
      *buf++ = 'M';
      *buf++ = '-';
      c -= 128;
    }
    if (c < 32) {
      *buf++ = '^';
      c += 64;
    } else if (c == 127) {
      *buf++ = '^';
      c = '?';
    }
  }

  *buf++ = c;
  *buf = 0;
}

/* Position file F to ignore_initial[F] bytes from its initial position,
   and yield its new position.  Don't try more than once.  */

static off_t file_position (int f) {
  static bool positioned[2];
  static off_t position[2];

  if (! positioned[f]) {
    positioned[f] = 1;
    position[f] = lseek (file_desc[f], ignore_initial[f], SEEK_CUR);
  }
  return position[f];
}
