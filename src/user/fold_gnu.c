/* fold -- wrap each input line to fit in specified width.
   Copyright (C) 91, 1995-2002 Free Software Foundation, Inc.

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

/* Written by David MacKenzie, djm@gnu.ai.mit.edu. */

#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <getopt.h>
#include "error.h"

#define _(s) (s)

/* HobbyOS port: faithful copy of the GNU textutils-2.1 fold.c with the
   gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc/xrealloc ->
   local allocating helpers over the sysroot malloc, xstrtol() and
   posix2_version() transcribed from lib/xstrtol.c and lib/posixver.c at
   the bottom of this file, close_stdout covered by exit()'s weak
   fflush(0), setlocale/bindtextdomain/textdomain dropped (single C
   locale).  The host build (Makefile fold_host, once wired) races this
   against the reference build in src/host/fold_parity.sh; byte-exact
   output is the bar.  */

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "fold"

#define AUTHORS "David MacKenzie"

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

#define STREQ(a, b) (strcmp ((a), (b)) == 0)
#define ISBLANK(c) ((c) == ' ' || (c) == '\t')
#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')

/* glibc getopt.h's long-only option chars.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
/* Braceless, exactly as textutils-2.1's src/sys2.h defines them: fold.c
   writes `{GETOPT_HELP_OPTION_DECL}' in longopts, so a braced definition
   would double-brace the initializer (clang warns and zeroes the
   option's `val', turning --help/--version into no-ops).  */
#define GETOPT_HELP_OPTION_DECL "help", no_argument, NULL, GETOPT_HELP_CHAR
#define GETOPT_VERSION_OPTION_DECL "version", no_argument, NULL, GETOPT_VERSION_CHAR
#define HELP_OPTION_DESCRIPTION _("      --help     display this help and exit\n")
#define VERSION_OPTION_DESCRIPTION _("      --version  output version information and exit\n")
#define case_GETOPT_HELP_CHAR case GETOPT_HELP_CHAR: usage (0); break
#define case_GETOPT_VERSION_CHAR(Program_name, Authors) \
  case GETOPT_VERSION_CHAR: \
    printf ("%s (%s) %s\n", (Program_name), PACKAGE, PACKAGE_VERSION); \
    exit (EXIT_SUCCESS)

/* The name this program was run with (defined by the sysroot error.c).  */
extern char *program_name;

/* Allocating helpers in the spirit of the gnulib x* wrappers: the
   original code calls xmalloc/xrealloc/xalloc_die.  */
static void xalloc_die (void) {
  error (EXIT_FAILURE, 0, _("memory exhausted"));
}

static void *xmalloc (size_t n) {
  void *p = malloc (n);
  if (p == NULL)
    xalloc_die ();
  return p;
}

static void *xrealloc (void *p, size_t n) {
  void *q = realloc (p, n);
  if (q == NULL)
    xalloc_die ();
  return q;
}

/* textutils-2.1's lib/xstrtol.h error taxonomy, kept so the -w parse
   below reads like the original.  */
enum strtol_error {
  LONGINT_OK,
  LONGINT_INVALID,
  LONGINT_INVALID_SUFFIX_CHAR,
  LONGINT_OVERFLOW
};

/* If nonzero, try to break on whitespace. */
static int break_spaces;

/* If nonzero, count bytes, not column positions. */
static int count_bytes;

/* If nonzero, at least one of the files we read was standard input. */
static int have_read_stdin;

static struct option const longopts[] = { {"bytes", no_argument, NULL, 'b'}, {"spaces", no_argument, NULL, 's'}, {"width", required_argument, NULL, 'w'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {NULL, 0, NULL, 0}
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
Wrap input lines in each FILE (standard input by default), writing to\n\
standard output.\n\
\n\
"), stdout);
    fputs (_("\
Mandatory arguments to long options are mandatory for short options too.\n\
"), stdout);
    fputs (_("\
  -b, --bytes         count bytes rather than columns\n\
  -s, --spaces        break at spaces\n\
  -w, --width=WIDTH   use WIDTH columns instead of 80\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* Assuming the current column is COLUMN, return the column that
   printing C will move the cursor to.
   The first column is 0. */

static int adjust_column (int column, char c) {
  if (!count_bytes) {
    if (c == '\b') {
      if (column > 0)
        column--;
    } else if (c == '\r')
      column = 0;
    else if (c == '\t')
      column = column + 8 - column % 8;
    else /* if (isprint (c)) */
      column++;
  } else
    column++;
  return column;
}

/* Fold file FILENAME, or standard input if FILENAME is "-",
   to stdout, with maximum line length WIDTH.
   Return 0 if successful, 1 if an error occurs. */

static int fold_file (char *filename, int width) {
  FILE *istream;
  register int c;
  int column = 0;     /* Screen column where next char will go. */
  int offset_out = 0; /* Index in `line_out' for next char. */
  static char *line_out = NULL;
  static int allocated_out = 0;

  if (STREQ (filename, "-")) {
    istream = stdin;
    have_read_stdin = 1;
  } else
    istream = fopen (filename, "r");

  if (istream == NULL) {
    error (0, errno, "%s", filename);
    return 1;
  }

  while ((c = getc (istream)) != EOF) {
    if (offset_out + 1 >= allocated_out) {
      allocated_out += 1024;
      line_out = xrealloc (line_out, allocated_out);
    }

    if (c == '\n') {
      line_out[offset_out++] = c;
      fwrite (line_out, sizeof (char), (size_t) offset_out, stdout);
      column = offset_out = 0;
      continue;
    }

  rescan:
    column = adjust_column (column, c);

    if (column > width) {
      /* This character would make the line too long.
         Print the line plus a newline, and make this character
         start the next line. */
      if (break_spaces) {
        /* Look for the last blank. */
        int logical_end;

        for (logical_end = offset_out - 1; logical_end >= 0;
             logical_end--)
          if (ISBLANK (line_out[logical_end]))
            break;
        if (logical_end >= 0) {
          int i;

          /* Found a blank.  Don't output the part after it. */
          logical_end++;
          fwrite (line_out, sizeof (char), (size_t) logical_end,
                  stdout);
          putchar ('\n');
          /* Move the remainder to the beginning of the next line.
             The areas being copied here might overlap. */
          memmove (line_out, line_out + logical_end,
                   offset_out - logical_end);
          offset_out -= logical_end;
          for (column = i = 0; i < offset_out; i++)
            column = adjust_column (column, line_out[i]);
          goto rescan;
        }
      } else {
        if (offset_out == 0) {
          line_out[offset_out++] = c;
          continue;
        }
      }
      line_out[offset_out++] = '\n';
      fwrite (line_out, sizeof (char), (size_t) offset_out, stdout);
      column = offset_out = 0;
      goto rescan;
    }

    line_out[offset_out++] = c;
  }

  if (offset_out)
    fwrite (line_out, sizeof (char), (size_t) offset_out, stdout);

  if (ferror (istream)) {
    error (0, errno, "%s", filename);
    if (!STREQ (filename, "-"))
      fclose (istream);
    return 1;
  }
  if (!STREQ (filename, "-") && fclose (istream) == EOF) {
    error (0, errno, "%s", filename);
    return 1;
  }

  return 0;
}

/* Transcribed from lib/posixver.c (definition at the bottom of this file).  */
static int posix2_version (void);

/* Transcribed from lib/xstrtol.c (definition at the bottom of this file).  */
static enum strtol_error xstrtol (const char *s, char **ptr, int strtol_base,
                                  long int *val, const char *valid_suffixes);

int main (int argc, char **argv) {
  int width = 80;
  int i;
  int optc;
  int errs = 0;

  program_name = argv[0];

  /* HobbyOS port: setlocale/bindtextdomain/textdomain and
     atexit (close_stdout) dropped (single C locale; exit()'s weak
     fflush(0) flushes stdout).  */

  break_spaces = count_bytes = have_read_stdin = 0;

  /* Turn any numeric options into -w options.  */
  for (i = 1; i < argc; i++) {
    char const *a = argv[i];
    if (a[0] == '-') {
      if (a[1] == '-' && !a[2])
        break;
      if (ISDIGIT (a[1])) {
        char *s = xmalloc (strlen (a) + 2);
        s[0] = '-';
        s[1] = 'w';
        strcpy (s + 2, a + 1);
        argv[i] = s;
        if (200112 <= posix2_version ()) {
          error (0, 0, _("`%s' option is obsolete; use `%s'"), a, s);
          usage (EXIT_FAILURE);
        }
      }
    }
  }

  while ((optc = getopt_long (argc, argv, "bsw:", longopts, NULL)) != -1) {
    switch (optc) {
    case 0:
      break;

    case 'b': /* Count bytes rather than columns. */
      count_bytes = 1;
      break;

    case 's': /* Break at word boundaries. */
      break_spaces = 1;
      break;

    case 'w': /* Line width. */ {
        long int tmp_long;
        if (xstrtol (optarg, NULL, 10, &tmp_long, "") != LONGINT_OK
            || tmp_long <= 0 || tmp_long > INT_MAX)
          error (EXIT_FAILURE, 0,
                 _("invalid number of columns: `%s'"), optarg);
        width = (int) tmp_long;
      }
      break;

      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

    default:
      usage (1);
    }
  }

  if (argc == optind)
    errs |= fold_file ("-", width);
  else
    for (i = optind; i < argc; i++)
      errs |= fold_file (argv[i], width);

  if (have_read_stdin && fclose (stdin) == EOF)
    error (EXIT_FAILURE, errno, "-");

  exit (errs == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* posix2_version -- which POSIX version to conform to, for utilities.
   Transcribed from textutils-2.1 lib/posixver.c (written by Paul Eggert)
   as fold's gate on the obsolete `-N' width syntax.  The compile-time
   default comes from <unistd.h>'s _POSIX2_VERSION; the sysroot publishes
   none, so it is 0 (no POSIX.2 conformance) and the obsolete form is
   accepted silently -- exactly the behavior of this release on the
   systems it shipped for.  */

#ifndef _POSIX2_VERSION
#define _POSIX2_VERSION 0
#endif

static int posix2_version (void) {
  long int v = _POSIX2_VERSION;
  char const *s = getenv ("_POSIX2_VERSION");

  if (s && *s) {
    char *e;
    long int i = strtol (s, &e, 10);
    if (!*e)
      v = i;
  }

  return v < INT_MIN ? INT_MIN : v < INT_MAX ? v : INT_MAX;
}

/* xstrtol -- a more useful interface to strtol.
   Transcribed from textutils-2.1 lib/xstrtol.c (written by Jim Meyering)
   for fold's one call, `xstrtol (optarg, NULL, 10, &tmp_long, "")': base
   10, and "" as the valid-suffix set, i.e. no multiplier suffix is
   accepted, so any character trailing the digits is
   LONGINT_INVALID_SUFFIX_CHAR and a non-number is LONGINT_INVALID.  (The
   original's bkm_scale suffix table is unreachable from that call and is
   not reproduced.)  */

static enum strtol_error xstrtol (const char *s, char **ptr, int strtol_base,
                                  long int *val, const char *valid_suffixes) {
  char *t_ptr;
  char **p;
  long int tmp;

  p = (ptr ? ptr : &t_ptr);

  errno = 0;
  tmp = strtol (s, p, strtol_base);
  if (errno != 0)
    return LONGINT_OVERFLOW;

  if (*p == s) {
    /* If there is no number but there is a valid suffix, assume the
       number is 1.  The string is invalid otherwise.  */
    if (valid_suffixes && **p && strchr (valid_suffixes, **p))
      tmp = 1;
    else
      return LONGINT_INVALID;
  }

  /* Let valid_suffixes == NULL mean `allow any suffix'.  */
  if (!valid_suffixes) {
    *val = tmp;
    return LONGINT_OK;
  }

  if (**p != '\0') {
    *val = tmp;
    return LONGINT_INVALID_SUFFIX_CHAR;
  }

  *val = tmp;
  return LONGINT_OK;
}
