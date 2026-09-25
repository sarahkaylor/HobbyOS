/* head -- output first part of file(s)
   Copyright (C) 89, 90, 91, 1995-2002 Free Software Foundation, Inc.

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

/* Options: (see usage)
   Reads from standard input if no files are given or when a filename of
   ``-'' is encountered.
   By default, filename headers are printed only if more than one file
   is given.
   By default, prints the first 10 lines (head -n 10).

   David MacKenzie <djm@gnu.ai.mit.edu> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <getopt.h>
#include "error.h"

#define _(s) (s)

/* HobbyOS port: faithful copy of the GNU head.c with the gnulib
   scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xstrtoumax ->
   local decimal+suffix parser (b/k/m), safe_read -> plain read(),
   close_stdout covered by exit()'s weak fflush(0); lseek/fstat are
   Phase-3 real syscalls.  The host build (Makefile head_host) races
   this against GNU head in src/host/head_parity.sh; byte-exact output
   is the bar.  */

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "head"

#define AUTHORS "David MacKenzie"

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"
#define LOCALEDIR "/usr/share/locale"

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#define STREQ(a, b) (strcmp ((a), (b)) == 0)
#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')

/* glibc getopt.h's long-only option chars.  These decl macros are
   BRACE-LESS (as textutils' sys2.h defines them): call sites wrap them
   as {GETOPT_HELP_OPTION_DECL}.  A braced definition would make the
   call site double-brace the struct, leaving has_arg/flag/val zero and
   silently disabling --help/--version.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
#define GETOPT_HELP_OPTION_DECL \
  "help", no_argument, NULL, GETOPT_HELP_CHAR
#define GETOPT_VERSION_OPTION_DECL \
  "version", no_argument, NULL, GETOPT_VERSION_CHAR
#define HELP_OPTION_DESCRIPTION _("      --help     display this help and exit\n")
#define VERSION_OPTION_DESCRIPTION _("      --version  output version information and exit\n")
#define case_GETOPT_HELP_CHAR \
  case GETOPT_HELP_CHAR: usage (0); break
#define case_GETOPT_VERSION_CHAR(Program_name, Authors) \
  case GETOPT_VERSION_CHAR: \
    printf ("%s (%s) %s\n", (Program_name), PACKAGE, PACKAGE_VERSION); \
    exit (EXIT_SUCCESS)

#define SET_BINARY2(a, b) ((void) 0)

/* Number of lines/chars/blocks to head. */
#define DEFAULT_NUMBER 10

/* Size of atomic reads. */
#define BUFSIZE (512 * 8)

/* If nonzero, print filename headers. */
static int print_headers;

/* When to print the filename banners. */
enum header_mode {
  multiple_files, always, never
};

/* The name this program was run with (defined by the sysroot error.c). */
extern char *program_name;

/* Have we ever read standard input?  */
static int have_read_stdin;

static struct option const long_options[] = { {"bytes", required_argument, NULL, 'c'}, {"lines", required_argument, NULL, 'n'}, {"quiet", no_argument, NULL, 'q'}, {"silent", no_argument, NULL, 'q'}, {"verbose", no_argument, NULL, 'v'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {NULL, 0, NULL, 0}
};

void usage (int status) {
  if (status != 0)
    fprintf (stderr, _("Try `%s --help' for more information.\n"),
             program_name);
  else {
    printf (_("\
Usage: %s [OPTION]... [FILE]...\n\
"),
            program_name);
    fputs (_("\
Print first 10 lines of each FILE to standard output.\n\
With more than one FILE, precede each with a header giving the file name.\n\
With no FILE, or when FILE is -, read standard input.\n\
\n\
"), stdout);
    fputs (_("\
Mandatory arguments to long options are mandatory for short options too.\n\
"), stdout);
    fputs (_("\
  -c, --bytes=SIZE         print first SIZE bytes\n\
  -n, --lines=NUMBER       print first NUMBER lines instead of first 10\n\
"), stdout);
    fputs (_("\
  -q, --quiet, --silent    never print headers giving file names\n\
  -v, --verbose            always print headers giving file names\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    fputs (_("\
\n\
SIZE may have a multiplier suffix: b for 512, k for 1K, m for 1 Meg.\n\
"), stdout);
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static void write_header (const char *filename) {
  static int first_file = 1;

  printf ("%s==> %s <==\n", (first_file ? "" : "\n"), filename);
  first_file = 0;
}

/* No partial-read surprises on HobbyOS, so this is a plain read().  */
static ssize_t safe_read (int fd, void *buf, size_t count) {
  return read (fd, buf, count);
}

static int head_bytes (const char *filename, int fd, uintmax_t bytes_to_write) {
  char buffer[BUFSIZE];
  int bytes_read;
  size_t bytes_to_read = BUFSIZE;

  /* Need BINARY I/O for the byte counts to be accurate.  */
  SET_BINARY2 (fd, fileno (stdout));

  while (bytes_to_write) {
    if (bytes_to_write < bytes_to_read)
      bytes_to_read = (size_t) bytes_to_write;
    bytes_read = (int) safe_read (fd, buffer, bytes_to_read);
    if (bytes_read < 0) {
      error (0, errno, "%s", filename);
      return 1;
    }
    if (bytes_read == 0)
      break;
    if (fwrite (buffer, 1, bytes_read, stdout) == 0)
      error (EXIT_FAILURE, errno, _("write error"));
    bytes_to_write -= (uintmax_t) bytes_read;
  }
  return 0;
}

static int head_lines (const char *filename, int fd, uintmax_t lines_to_write) {
  char buffer[BUFSIZE];

  /* Need BINARY I/O for the byte counts to be accurate.  */
  SET_BINARY2 (fd, fileno (stdout));

  while (lines_to_write) {
    int bytes_read = (int) safe_read (fd, buffer, BUFSIZE);
    int bytes_to_write = 0;

    if (bytes_read < 0) {
      error (0, errno, "%s", filename);
      return 1;
    }
    if (bytes_read == 0)
      break;
    while (bytes_to_write < bytes_read)
      if (buffer[bytes_to_write++] == '\n' && --lines_to_write == 0) {
        /* If we have read more data than that on the specified number
           of lines, try to seek back to the position we would have
           gotten to had we been reading one byte at a time.  */
        if (lseek (fd, bytes_to_write - bytes_read, SEEK_CUR) < 0) {
          int e = errno;
          struct stat st;
          if (fstat (fd, &st) != 0 || S_ISREG (st.st_mode))
            error (0, e, _("cannot reposition file pointer for %s"),
                   filename);
        }
        break;
      }
    if (fwrite (buffer, 1, (size_t) bytes_to_write, stdout) == 0)
      error (EXIT_FAILURE, errno, _("write error"));
  }
  return 0;
}

static int head (const char *filename, int fd, uintmax_t n_units, int count_lines) {
  if (print_headers)
    write_header (filename);

  if (count_lines)
    return head_lines (filename, fd, n_units);
  else
    return head_bytes (filename, fd, n_units);
}

static int head_file (const char *filename, uintmax_t n_units, int count_lines) {
  int fd;

  if (STREQ (filename, "-")) {
    have_read_stdin = 1;
    return head (_("standard input"), STDIN_FILENO, n_units, count_lines);
  } else {
    fd = open (filename, O_RDONLY);
    if (fd >= 0) {
      int errors;

      errors = head (filename, fd, n_units, count_lines);
      if (close (fd) == 0)
        return errors;
    }
    error (0, errno, "%s", filename);
    return 1;
  }
}

/* Convert a string of decimal digits, N_STRING, with a single, optional suffix
   character (b, k, or m) to an integral value.  Upon successful conversion,
   return that value.  If it cannot be converted, give a diagnostic and exit.
   COUNT_LINES indicates whether N_STRING is a number of bytes or a number
   of lines.  It is used solely to give a more specific diagnostic.  */

static uintmax_t string_to_integer (int count_lines, const char *n_string) {
  uintmax_t n;
  char *end;
  int overflow = 0;

  errno = 0;
  n = (uintmax_t) strtoull (n_string, &end, 10);
  if (errno == ERANGE && n == UINTMAX_MAX)
    overflow = 1;

  if (overflow) {
    error (EXIT_FAILURE, 0,
           _("%s: %s is so large that it is not representable"), n_string,
           count_lines ? _("number of lines") : _("number of bytes"));
  }

  /* Parse a single multiplier suffix: b=512, k=1024, m=1 Meg.  */
  if (*end) {
    unsigned long mult = 0;
    if (end[1] == '\0') {
      switch (*end) {
      case 'b': mult = 512;          break;
      case 'k': mult = 1024;         break;
      case 'm': mult = 1024u * 1024; break;
      default:  break;
      }
    }
    if (mult != 0) {
      if (n > UINTMAX_MAX / mult) {
        error (EXIT_FAILURE, 0,
               _("%s: %s is so large that it is not representable"),
               n_string,
               count_lines ? _("number of lines")
                           : _("number of bytes"));
      }
      n *= mult;
    } else {
      error (EXIT_FAILURE, 0, "%s: %s", n_string,
             (count_lines
              ? _("invalid number of lines")
              : _("invalid number of bytes")));
    }
  }

  return n;
}

int main (int argc, char **argv) {
  enum header_mode header_mode = multiple_files;
  int exit_status = 0;
  int c;

  /* Number of items to print. */
  uintmax_t n_units = DEFAULT_NUMBER;

  /* If nonzero, interpret the numeric argument as the number of lines.
     Otherwise, interpret it as the number of bytes.  */
  int count_lines = 1;

  program_name = argv[0];

  have_read_stdin = 0;

  print_headers = 0;

  if (1 < argc && argv[1][0] == '-' && ISDIGIT (argv[1][1])) {
    char *a = argv[1];
    char *n_string = ++a;
    char *end_n_string;
    char multiplier_char = 0;

    /* Old option syntax; a dash, one or more digits, and one or
       more option letters.  Move past the number. */
    do ++a;
    while (ISDIGIT (*a));

    /* Pointer to the byte after the last digit.  */
    end_n_string = a;

    /* Parse any appended option letters. */
    for (; *a; a++) {
      switch (*a) {
      case 'c':
        count_lines = 0;
        multiplier_char = 0;
        break;

      case 'b':
      case 'k':
      case 'm':
        count_lines = 0;
        multiplier_char = *a;
        break;

      case 'l':
        count_lines = 1;
        break;

      case 'q':
        header_mode = never;
        break;

      case 'v':
        header_mode = always;
        break;

      default:
        error (0, 0, _("unrecognized option `-%c'"), *a);
        usage (1);
      }
    }

    /* (The POSIX-200112 obsolete-syntax deprecation check is dropped:
       HobbyOS sets no POSIX version.)  */

    /* Append the multiplier character (if any) onto the end of
       the digit string.  Then add NUL byte if necessary.  */
    *end_n_string = multiplier_char;
    if (multiplier_char)
      *(++end_n_string) = 0;

    n_units = string_to_integer (count_lines, n_string);

    /* Make the options we just parsed invisible to getopt. */
    argv[1] = argv[0];
    argv++;
    argc--;

    /* FIXME: allow POSIX options if there were obsolescent ones?  */

  }

  while ((c = getopt_long (argc, argv, "c:n:qv", long_options, NULL)) != -1) {
    switch (c) {
    case 0:
      break;

    case 'c':
      count_lines = 0;
      n_units = string_to_integer (count_lines, optarg);
      break;

    case 'n':
      count_lines = 1;
      n_units = string_to_integer (count_lines, optarg);
      break;

    case 'q':
      header_mode = never;
      break;

    case 'v':
      header_mode = always;
      break;

      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

    default:
      usage (1);
    }
  }

  if (header_mode == always
      || (header_mode == multiple_files && optind < argc - 1))
    print_headers = 1;

  if (optind == argc)
    exit_status |= head_file ("-", n_units, count_lines);

  for (; optind < argc; ++optind)
    exit_status |= head_file (argv[optind], n_units, count_lines);

  if (have_read_stdin && close (STDIN_FILENO) < 0)
    error (EXIT_FAILURE, errno, "-");

  exit (exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
