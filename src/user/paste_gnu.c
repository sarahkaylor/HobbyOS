/* paste - merge lines of files
   Copyright (C) 1984, 1997, 1998, 1999, 2000, 2001 by David M. Ihnat

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

/* Written by David Ihnat.  */

/* The list of valid escape sequences has been expanded over the Unix
   version, to include \b, \f, \r, and \v.

   POSIX changes, bug fixes, long-named options, and cleanup
   by David MacKenzie <djm@gnu.ai.mit.edu>.

   Options:
   --serial
   -s                           Paste one file at a time rather than
                                one line from each file.
   --delimiters=delim-list
   -d delim-list                Consecutively use the characters in
                                DELIM-LIST instead of tab to separate
                                merged lines.  When DELIM-LIST is exhausted,
                                start again at its beginning.
   A FILE of `-' means standard input.
   If no FILEs are given, standard input is used. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <getopt.h>
#include "error.h"

#define _(s) (s)

/* HobbyOS port: faithful copy of the GNU textutils-2.1 paste.c with the
   gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc/xrealloc ->
   local allocating helpers over the sysroot malloc, _() -> identity,
   setlocale/bindtextdomain/textdomain dropped (single C locale),
   atexit (close_stdout) dropped (exit()'s weak fflush(0) flushes
   stdout itself).  No lib helper is needed here: paste is pure
   getc/putc/fwrite over FILE* streams (getstr/safe-read never enter).
   The one type-level adaptation is the two FILE sentinels below, since
   the sysroot stdio.h keeps FILE opaque.  The host build (Makefile
   paste_host) races this against the reference build in
   src/host/paste_parity.sh; byte-exact output is the bar.  */

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "paste"

#define AUTHORS N_ ("David M. Ihnat and David MacKenzie")

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

#define STREQ(a, b) (strcmp ((a), (b)) == 0)
#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')

/* sys2.h's no-op gettext marker and lint-only initializers (kept so the
   original source transcribes verbatim).  */
#define N_(s) (s)
#define IF_LINT(Code) /* empty */

/* glibc getopt.h's long-only option chars.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
/* HobbyOS port: unbraced, exactly as textutils-2.1's sys2.h defines them.
   cut_gnu.c wraps these in braces while its call sites also brace them
   ({GETOPT_HELP_OPTION_DECL}), which the compiler reads as "initialize the
   scalar `name' member from a braced list" -- val collapses to 0 and the
   --help/--version long options are swallowed by `case 0'.  Keeping the
   original brace-free form restores the upstream call sites and makes
   --help/--version work (and byte-comparable against the 2.1 reference).  */
#define GETOPT_HELP_OPTION_DECL "help", no_argument, NULL, GETOPT_HELP_CHAR
#define GETOPT_VERSION_OPTION_DECL "version", no_argument, NULL, GETOPT_VERSION_CHAR
#define HELP_OPTION_DESCRIPTION _("      --help     display this help and exit\n")
#define VERSION_OPTION_DESCRIPTION _("      --version  output version information and exit\n")
#define case_GETOPT_HELP_CHAR case GETOPT_HELP_CHAR: usage (0); break
#define case_GETOPT_VERSION_CHAR(Program_name, Authors) \
  case GETOPT_VERSION_CHAR: \
    printf ("%s (%s) %s\n", (Program_name), PACKAGE, PACKAGE_VERSION); \
    exit (EXIT_SUCCESS)

/* Name this program was run with.  */
/* HobbyOS port: defined by the sysroot error.c, declared here.  */
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

/* Indicates that no delimiter should be added in the current position. */
#define EMPTY_DELIM '\0'

/* HobbyOS port: the sysroot stdio.h keeps FILE opaque, so the original
   `static FILE dummy_closed' objects cannot be instantiated.  These
   sentinels stand in for them: only their addresses are ever used
   (pointer-identity comparisons against real FILE* streams), never
   dereferenced, which is all the originals were good for.  */
struct paste_sentinel {
  FILE *stream;
};

static struct paste_sentinel dummy_closed;
/* Element marking a file that has reached EOF and been closed. */
#define CLOSED ((FILE *) &dummy_closed)

static struct paste_sentinel dummy_endlist;
/* Element marking end of list of open files. */
#define ENDLIST ((FILE *) &dummy_endlist)

/* If nonzero, we have read standard input at some point. */
static int have_read_stdin;

/* If nonzero, merge subsequent lines of each file rather than
   corresponding lines from each file in parallel. */
static int serial_merge;

/* The delimeters between lines of input files (used cyclically). */
static char *delims;

/* A pointer to the character after the end of `delims'. */
static char *delim_end;

static struct option const longopts[] = { {"serial", no_argument, 0, 's'}, {"delimiters", required_argument, 0, 'd'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {0, 0, 0, 0}
};

/* Replace backslash representations of special characters in
   STRPTR with their actual values.
   The set of possible backslash characters has been expanded beyond
   that recognized by the Unix version.

   Return a pointer to the character after the new end of STRPTR. */

static char *collapse_escapes (char *strptr) {
  register char *strout;

  strout = strptr;              /* Start at the same place, anyway. */

  while (*strptr) {
    if (*strptr != '\\')        /* Is it an escape character? */
      *strout++ = *strptr++;    /* No, just transfer it. */
    else {
      switch (*++strptr) {
      case '0':
        *strout++ = EMPTY_DELIM;
        break;

      case 'b':
        *strout++ = '\b';
        break;

      case 'f':
        *strout++ = '\f';
        break;

      case 'n':
        *strout++ = '\n';
        break;

      case 'r':
        *strout++ = '\r';
        break;

      case 't':
        *strout++ = '\t';
        break;

      case 'v':
        *strout++ = '\v';
        break;

      default:
        *strout++ = *strptr;
        break;
      }
      strptr++;
    }
  }
  return strout;
}

/* Perform column paste on the NFILES files named in FNAMPTR.
   Return 0 if no errors, 1 if one or more files could not be
   opened or read. */

static int paste_parallel (int nfiles, char **fnamptr) {
  int errors = 0;               /* 1 if open or read errors occur. */
  /* Number of files for which space is allocated in `delbuf' and `fileptr'.
     Enlarged as necessary. */
  int file_list_size = 12;
  int chr IF_LINT (= 0);        /* Input character. */
  int line_length;              /* Number of chars in line. */
  int somedone;                 /* 0 if all files empty for this line. */
  /* If all files are just ready to be closed, or will be on this
     round, the string of delimiters must be preserved.
     delbuf[0] through delbuf[file_list_size]
     store the delimiters for closed files. */
  char *delbuf;
  int delims_saved;             /* Number of delims saved in `delbuf'. */
  register char *delimptr;      /* Cycling pointer into `delims'. */
  FILE **fileptr;               /* Streams open to the files to process. */
  int files_open;               /* Number of files still open to process. */
  int i;                        /* Loop index. */
  int opened_stdin = 0;         /* Nonzero if any fopen got fd 0. */

  delbuf = (char *) xmalloc (file_list_size + 2);
  fileptr = (FILE **) xmalloc ((file_list_size + 1) * sizeof (FILE *));

  /* Attempt to open all files.  This could be expanded to an infinite
     number of files, but at the (considerable) expense of remembering
     each file and its current offset, then opening/reading/closing.  */

  for (files_open = 0; files_open < nfiles; ++files_open) {
    if (files_open == file_list_size - 2) {
      file_list_size += 12;
      delbuf = (char *) xrealloc (delbuf, file_list_size + 2);
      fileptr = (FILE **) xrealloc ((char *) fileptr, (file_list_size + 1)
                                    * sizeof (FILE *));
    }
    if (STREQ (fnamptr[files_open], "-")) {
      have_read_stdin = 1;
      fileptr[files_open] = stdin;
    } else {
      fileptr[files_open] = fopen (fnamptr[files_open], "r");
      if (fileptr[files_open] == NULL)
        error (EXIT_FAILURE, errno, "%s", fnamptr[files_open]);
      else if (fileno (fileptr[files_open]) == 0)
        opened_stdin = 1;
    }
  }

  fileptr[files_open] = ENDLIST;

  if (opened_stdin && have_read_stdin)
    error (EXIT_FAILURE, 0, _("standard input is closed"));

  /* Read a line from each file and output it to stdout separated by a
     delimiter, until we go through the loop without successfully
     reading from any of the files. */

  while (files_open) {
    /* Set up for the next line. */
    somedone = 0;
    delimptr = delims;
    delims_saved = 0;

    for (i = 0; fileptr[i] != ENDLIST && files_open; i++) {
      line_length = 0;  /* Clear so we can easily detect EOF. */
      if (fileptr[i] != CLOSED) {
        chr = getc (fileptr[i]);
        if (chr != EOF && delims_saved) {
          fwrite (delbuf, sizeof (char), delims_saved, stdout);
          delims_saved = 0;
        }

        while (chr != EOF) {
          line_length++;
          if (chr == '\n')
            break;
          putc (chr, stdout);
          chr = getc (fileptr[i]);
        }
      }

      if (line_length == 0) {
        /* EOF, read error, or closed file.
           If an EOF or error, close the file and mark it in the list. */
        if (fileptr[i] != CLOSED) {
          if (ferror (fileptr[i])) {
            error (0, errno, "%s", fnamptr[i]);
            errors = 1;
          }
          if (fileptr[i] == stdin)
            clearerr (fileptr[i]); /* Also clear EOF. */
          else if (fclose (fileptr[i]) == EOF) {
            error (0, errno, "%s", fnamptr[i]);
            errors = 1;
          }

          fileptr[i] = CLOSED;
          files_open--;
        }

        if (fileptr[i + 1] == ENDLIST) {
          /* End of this output line.
             Is this the end of the whole thing? */
          if (somedone) {
            /* No.  Some files were not closed for this line. */
            if (delims_saved) {
              fwrite (delbuf, sizeof (char), delims_saved, stdout);
              delims_saved = 0;
            }
            putc ('\n', stdout);
          }
          continue;     /* Next read of files, or exit. */
        } else {
          /* Closed file; add delimiter to `delbuf'. */
          if (*delimptr != EMPTY_DELIM)
            delbuf[delims_saved++] = *delimptr;
          if (++delimptr == delim_end)
            delimptr = delims;
        }
      } else {
        /* Some data read. */
        somedone++;

        /* Except for last file, replace last newline with delim. */
        if (fileptr[i + 1] != ENDLIST) {
          if (chr != '\n')
            putc (chr, stdout);
          if (*delimptr != EMPTY_DELIM)
            putc (*delimptr, stdout);
          if (++delimptr == delim_end)
            delimptr = delims;
        } else
          putc (chr, stdout);
      }
    }
  }
  return errors;
}

/* Perform serial paste on the NFILES files named in FNAMPTR.
   Return 0 if no errors, 1 if one or more files could not be
   opened or read. */

static int paste_serial (int nfiles, char **fnamptr) {
  int errors = 0;               /* 1 if open or read errors occur. */
  register int charnew, charold; /* Current and previous char read. */
  register char *delimptr;      /* Current delimiter char. */
  register FILE *fileptr;       /* Open for reading current file. */

  for (; nfiles; nfiles--, fnamptr++) {
    if (STREQ (*fnamptr, "-")) {
      have_read_stdin = 1;
      fileptr = stdin;
    } else {
      fileptr = fopen (*fnamptr, "r");
      if (fileptr == NULL) {
        error (0, errno, "%s", *fnamptr);
        errors = 1;
        continue;
      }
    }

    delimptr = delims;  /* Set up for delimiter string. */

    charold = getc (fileptr);
    if (charold != EOF) {
      /* `charold' is set up.  Hit it!
         Keep reading characters, stashing them in `charnew';
         output `charold', converting to the appropriate delimiter
         character if needed.  After the EOF, output `charold'
         if it's a newline; otherwise, output it and then a newline. */

      while ((charnew = getc (fileptr)) != EOF) {
        /* Process the old character. */
        if (charold == '\n') {
          if (*delimptr != EMPTY_DELIM)
            putc (*delimptr, stdout);

          if (++delimptr == delim_end)
            delimptr = delims;
        } else
          putc (charold, stdout);

        charold = charnew;
      }

      /* Hit EOF.  Process that last character. */
      putc (charold, stdout);
    }

    if (charold != '\n')
      putc ('\n', stdout);

    if (ferror (fileptr)) {
      error (0, errno, "%s", *fnamptr);
      errors = 1;
    }
    if (fileptr == stdin)
      clearerr (fileptr);       /* Also clear EOF. */
    else if (fclose (fileptr) == EOF) {
      error (0, errno, "%s", *fnamptr);
      errors = 1;
    }
  }
  return errors;
}

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
Write lines consisting of the sequentially corresponding lines from\n\
each FILE, separated by TABs, to standard output.\n\
With no FILE, or when FILE is -, read standard input.\n\
\n\
"), stdout);
    fputs (_("\
Mandatory arguments to long options are mandatory for short options too.\n\
"), stdout);
    fputs (_("\
  -d, --delimiters=LIST   reuse characters from LIST instead of TABs\n\
  -s, --serial            paste one file at a time instead of in parallel\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    /* FIXME: add a couple of examples.  */
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

int main (int argc, char **argv) {
  int optc, exit_status;
  char default_delims[2], zero_delims[3];

  program_name = argv[0];

  have_read_stdin = 0;
  serial_merge = 0;
  delims = default_delims;
  strcpy (delims, "\t");
  strcpy (zero_delims, "\\0");

  while ((optc = getopt_long (argc, argv, "d:s", longopts, NULL)) != -1) {
    switch (optc) {
    case 0:
      break;

    case 'd':
      /* Delimiter character(s). */
      if (optarg[0] == '\0')
        optarg = zero_delims;
      delims = optarg;
      break;

    case 's':
      serial_merge++;
      break;

      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

    default:
      usage (1);
    }
  }

  if (optind == argc)
    argv[argc++] = "-";

  delim_end = collapse_escapes (delims);

  if (!serial_merge)
    exit_status = paste_parallel (argc - optind, &argv[optind]);
  else
    exit_status = paste_serial (argc - optind, &argv[optind]);
  if (have_read_stdin && fclose (stdin) == EOF)
    error (EXIT_FAILURE, errno, "-");
  exit (exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
