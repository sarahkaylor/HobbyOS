/* comm -- compare two sorted files line by line.
   Copyright (C) 86, 90, 91, 1995-2002 Free Software Foundation, Inc.

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

/* Written by Richard Stallman and David MacKenzie. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include "error.h"

#define _(s) (s)

/* HobbyOS port: faithful copy of the GNU textutils-2.1 comm.c with the
   gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc/xrealloc ->
   local allocating helpers over the sysroot malloc, the linebuffer
   routines (initbuffer/readline) transcribed from lib/linebuffer.c at
   the bottom of this file, close_stdout covered by exit()'s weak
   fflush(0), setlocale/bindtextdomain/textdomain dropped (single C
   locale).  Dropping setlocale also drops hard_locale/xmemcoll: with
   LC_COLLATE pinned to C, hard_locale (LC_COLLATE) is 0 and the memcmp
   branch of compare_files is the only live path -- the same path the
   textutils-2.1 binary itself takes under LC_ALL=C, which
   src/host/comm_parity.sh pins for both sides.  comm reads through
   lib/linebuffer.c (initbuffer/readline), so getstr/safe-read never
   enter; readline appends the missing newline of a final partial line,
   exactly as upstream.  The host build (Makefile comm_host) races this
   against the reference build in src/host/comm_parity.sh; byte-exact
   output is the bar.  */

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "comm"

#define AUTHORS N_ ("Richard Stallman and David MacKenzie")

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

#define STREQ(a, b) (strcmp ((a), (b)) == 0)

/* sys2.h's no-op gettext marker (kept so the original AUTHORS line
   transcribes verbatim).  */
#define N_(s) (s)

/* glibc getopt.h's long-only option chars.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
/* HobbyOS port: unbraced, exactly as textutils-2.1's sys2.h defines them.
   comm.c's long_options braces these call sites ({GETOPT_HELP_OPTION_DECL}),
   which is the form upstream compiled: a braced macro definition here would
   nest braces and make the compiler read "initialize the scalar `name'
   member from a braced list", collapsing val to 0 so --help/--version
   would be swallowed by `case 0'.  Keeping the original brace-free form
   restores the upstream call sites and makes --help/--version work (and
   byte-comparable against the 2.1 reference).  */
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
   original code (via lib/linebuffer.c) calls xmalloc/xrealloc/xalloc_die.  */
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

/* Undefine, to avoid warning about redefinition on some systems.  */
#undef min
#define min(x, y) ((x) < (y) ? (x) : (y))

/* If nonzero, print lines that are found only in file 1. */
static int only_file_1;

/* If nonzero, print lines that are found only in file 2. */
static int only_file_2;

/* If nonzero, print lines that are found in both files. */
static int both;

/* `struct linebuffer' from lib/linebuffer.h: holds a line of text.  */
struct linebuffer {
  size_t size;   /* Allocated. */
  size_t length; /* Used. */
  char *buffer;
};

/* Transcribed from lib/linebuffer.c (definitions at the bottom of this
   file); comm reads its input with readline, which keeps the newline and
   appends one to a final line that lacks it.  */
static void initbuffer (struct linebuffer *linebuffer);
static struct linebuffer *readline (struct linebuffer *linebuffer, FILE *stream);

static struct option const long_options[] = { {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {0, 0, 0, 0}
};

void usage (int status) {
  if (status != 0)
    fprintf (stderr, _("Try `%s --help' for more information.\n"),
             program_name);
  else {
    printf (_("\
Usage: %s [OPTION]... LEFT_FILE RIGHT_FILE\n\
"),
            program_name);
    fputs (_("\
Compare sorted files LEFT_FILE and RIGHT_FILE line by line.\n\
\n\
  -1              suppress lines unique to left file\n\
  -2              suppress lines unique to right file\n\
  -3              suppress lines that appear in both files\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* Output the line in linebuffer LINE to stream STREAM
   provided the switches say it should be output.
   CLASS is 1 for a line found only in file 1,
   2 for a line only in file 2, 3 for a line in both. */

static void writeline (const struct linebuffer *line, FILE *stream, int class) {
  switch (class) {
  case 1:
    if (!only_file_1)
      return;
    break;

  case 2:
    if (!only_file_2)
      return;
    /* Print a TAB if we are printing lines from file 1.  */
    if (only_file_1)
      putc ('\t', stream);
    break;

  case 3:
    if (!both)
      return;
    /* Print a TAB if we are printing lines from file 1.  */
    if (only_file_1)
      putc ('\t', stream);
    /* Print a TAB if we are printing lines from file 2.  */
    if (only_file_2)
      putc ('\t', stream);
    break;
  }

  fwrite (line->buffer, sizeof (char), line->length, stream);
}

/* Compare INFILES[0] and INFILES[1].
   If either is "-", use the standard input for that file.
   Assume that each input file is sorted;
   merge them and output the result.
   Return 0 if successful, 1 if any errors occur. */

static int compare_files (char **infiles) {
  /* For each file, we have one linebuffer in lb1.  */
  struct linebuffer lb1[2];

  /* thisline[i] points to the linebuffer holding the next available line
     in file i, or is NULL if there are no lines left in that file.  */
  struct linebuffer *thisline[2];

  /* streams[i] holds the input stream for file i.  */
  FILE *streams[2];

  int i, ret = 0;

  /* Initialize the storage. */
  for (i = 0; i < 2; i++) {
    initbuffer (&lb1[i]);
    thisline[i] = &lb1[i];
    streams[i] = (STREQ (infiles[i], "-") ? stdin : fopen (infiles[i], "r"));
    if (!streams[i]) {
      error (0, errno, "%s", infiles[i]);
      return 1;
    }

    thisline[i] = readline (thisline[i], streams[i]);
  }

  while (thisline[0] || thisline[1]) {
    int order;

    /* Compare the next available lines of the two files.  */

    if (!thisline[0])
      order = 1;
    else if (!thisline[1])
      order = -1;
    else {
      /* HobbyOS port: textutils-2.1 branches here to xmemcoll when
         (HAVE_SETLOCALE && hard_LC_COLLATE), i.e. only under a hard
         LC_COLLATE locale.  The sysroot has no setlocale (single C
         locale), so LC_COLLATE is never hard and that branch is
         unreachable; its gnulib graph (hard-locale/hard_locale,
         xmemcoll, memcoll, quotearg) is deliberately not ported.  What
         remains is the C-collation path below, byte-identical to what
         the textutils-2.1 binary itself computes under LC_ALL=C (the
         locale src/host/comm_parity.sh pins for both sides).  memcmp
         compares unsigned bytes, matching strcoll in the C locale.  */
      size_t len = min (thisline[0]->length, thisline[1]->length) - 1;
      order = memcmp (thisline[0]->buffer, thisline[1]->buffer, len);
      if (order == 0)
        order = (thisline[0]->length < thisline[1]->length
                 ? -1
                 : thisline[0]->length != thisline[1]->length);
    }

    /* Output the line that is lesser. */
    if (order == 0)
      writeline (thisline[1], stdout, 3);
    else if (order > 0)
      writeline (thisline[1], stdout, 2);
    else
      writeline (thisline[0], stdout, 1);

    /* Step the file the line came from.
       If the files match, step both files.  */
    if (order >= 0)
      thisline[1] = readline (thisline[1], streams[1]);
    if (order <= 0)
      thisline[0] = readline (thisline[0], streams[0]);
  }

  /* Free all storage and close all input streams. */
  for (i = 0; i < 2; i++) {
    free (lb1[i].buffer);
    if (ferror (streams[i]) || fclose (streams[i]) == EOF) {
      error (0, errno, "%s", infiles[i]);
      ret = 1;
    }
  }
  return ret;
}

int main (int argc, char **argv) {
  int c;

  program_name = argv[0];

  /* HobbyOS port: setlocale/bindtextdomain/textdomain and
     hard_locale (LC_COLLATE) are dropped (the sysroot has a single C
     locale, so hard_LC_COLLATE would always be 0), and so is
     atexit (close_stdout) -- exit() flushes stdout through its weak
     fflush(0).  */

  only_file_1 = 1;
  only_file_2 = 1;
  both = 1;

  while ((c = getopt_long (argc, argv, "123", long_options, NULL)) != -1)
    switch (c) {
    case 0:
      break;

    case '1':
      only_file_1 = 0;
      break;

    case '2':
      only_file_2 = 0;
      break;

    case '3':
      both = 0;
      break;

      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

    default:
      usage (1);
    }

  if (optind + 2 != argc)
    usage (1);

  exit (compare_files (argv + optind) == 0
        ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* linebuffer -- read arbitrarily long lines.
   Transcribed from textutils-2.1 lib/linebuffer.c (written by Richard
   Stallman) as comm's input path.  freebuffer () is not called by comm
   (compare_files frees the raw buffers itself), so it is not
   transcribed.  */

/* Initialize linebuffer LINEBUFFER for use. */

static void initbuffer (struct linebuffer *linebuffer) {
  linebuffer->length = 0;
  linebuffer->size = 200;
  linebuffer->buffer = xmalloc (linebuffer->size);
}

/* Read an arbitrarily long line of text from STREAM into LINEBUFFER.
   Keep the newline; append a newline if it's the last line of a file
   that ends in a non-newline character.  Do not null terminate.
   Therefore the stream can contain NUL bytes, and the length
   (including the newline) is returned in linebuffer->length.
   Return NULL upon error, or when STREAM is empty.
   Otherwise, return LINEBUFFER.  */

static struct linebuffer *readline (struct linebuffer *linebuffer, FILE *stream) {
  int c;
  char *buffer = linebuffer->buffer;
  char *p = linebuffer->buffer;
  char *end = buffer + linebuffer->size; /* Sentinel. */

  if (feof (stream) || ferror (stream))
    return NULL;

  do {
    c = getc (stream);
    if (c == EOF) {
      if (p == buffer)
        return NULL;
      if (p[-1] == '\n')
        break;
      c = '\n';
    }
    if (p == end) {
      linebuffer->size *= 2;
      buffer = xrealloc (buffer, linebuffer->size);
      p = p - linebuffer->buffer + buffer;
      linebuffer->buffer = buffer;
      end = buffer + linebuffer->size;
    }
    *p++ = c;
  } while (c != '\n');

  linebuffer->length = p - buffer;
  return linebuffer;
}
