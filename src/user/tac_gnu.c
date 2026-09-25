/* tac -- concatenate and print files in reverse
   Copyright (C) 1988-1991, 1995-2002 Free Software Foundation, Inc.

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

/* Written by Jay Lepreau (lepreau@cs.utah.edu).
   GNU enhancements by David MacKenzie (djm@gnu.ai.mit.edu). */

/* Copy each FILE, or the standard input if none are given or when a
   FILE name of "-" is encountered, to the standard output with the
   order of the records reversed.  The records are separated by
   instances of a string, or a newline if none is given.  By default, the
   separator string is attached to the end of the record that it
   follows in the file.

   Options:
   -b, --before                 The separator is attached to the beginning
                                of the record that it precedes in the file.
   -r, --regex                  The separator is a regular expression.
   -s, --separator=separator    Use SEPARATOR as the record separator.

   To reverse a file byte by byte, use (in bash, ksh, or sh):
tac -r -s '.\|
' file */

/* HobbyOS port: faithful copy of the GNU textutils-2.1 tac.c with the
   gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc/xrealloc ->
   local allocating helpers over the sysroot malloc, safe_read -> a
   local EINTR-retrying read wrapper, SET_BINARY/SET_BINARY2 -> no-op
   stubs, close_stdout covered by exit()'s weak fflush(0).  setlocale/
   bindtextdomain/textdomain are dropped (single C locale); nothing in
   tac consults the locale.  The GNU re_* interface (-r) resolves
   against the sysroot's transcribed regex engine, the same API GNU sed
   uses.  The dead #if 0 tac_mem/tac_stdin_to_mem experiment is omitted.

   Note on the pipe path: like upstream, a non-regular standard input is
   first copied to a mkstemp temp file under $TMPDIR (default /tmp) and
   then reversed by seeking.  HobbyOS images therefore carry a writable
   /tmp directory (Makefile disk.img, mmd).  The host build (Makefile
   tac_host) races this against the reference build in src/host/tac_parity.sh;
   byte-exact output is the bar.  */

#define _GNU_SOURCE 1
#define PROGRAM_NAME "tac"
#define AUTHORS N_ ("Jay Lepreau and David MacKenzie")

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <regex.h>
#include "error.h"

#define _(s) (s)

/* textutils-2.1's sys2.h no-op gettext marker.  */
#define N_(s) (s)

/* glibc getopt.h's long-only option chars.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
/* HobbyOS port: unbraced, exactly as textutils-2.1's sys2.h defines them;
   the original call sites brace these macros (see cut_gnu.c's note).  */
#define GETOPT_HELP_OPTION_DECL "help", no_argument, NULL, GETOPT_HELP_CHAR
#define GETOPT_VERSION_OPTION_DECL "version", no_argument, NULL, GETOPT_VERSION_CHAR
#define HELP_OPTION_DESCRIPTION _("      --help     display this help and exit\n")
#define VERSION_OPTION_DESCRIPTION _("      --version  output version information and exit\n")
#define case_GETOPT_HELP_CHAR case GETOPT_HELP_CHAR: usage (0); break
#define case_GETOPT_VERSION_CHAR(Program_name, Authors) \
  case GETOPT_VERSION_CHAR: \
    printf ("%s (%s) %s\n", (Program_name), PACKAGE, PACKAGE_VERSION); \
    exit (EXIT_SUCCESS)

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

#define STREQ(a, b) (strcmp ((a), (b)) == 0)

/* The name this program was run with (defined by the sysroot error.c).  */
extern char *program_name;

/* Allocating helpers in the spirit of the gnulib x* wrappers.  */
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

/* gnulib safe-read.h: read once, retrying EINTR.  */
static ssize_t safe_read (int fd, void *buf, size_t count) {
  ssize_t n;

  for (;;) {
    n = read (fd, buf, count);
    if (n >= 0 || errno != EINTR)
      return n;
  }
}

/* No text/binary distinction on HobbyOS; keep the upstream call sites.  */
#define SET_BINARY(fd) ((void) 0)
#define SET_BINARY2(fd1, fd2) ((void) 0)

#ifndef DEFAULT_TMPDIR
# define DEFAULT_TMPDIR "/tmp"
#endif

/* The number of bytes per atomic read. */
#define INITIAL_READSIZE 8192

/* The number of bytes per atomic write. */
#define WRITESIZE 8192

/* The string that separates the records of the file. */
static char *separator;

/* If nonzero, print `separator' along with the record preceding it
   in the file; otherwise with the record following it. */
static int separator_ends_record;

/* 0 if `separator' is to be matched as a regular expression;
   otherwise, the length of `separator', used as a sentinel to
   stop the search. */
static int sentinel_length;

/* The length of a match with `separator'.  If `sentinel_length' is 0,
   `match_length' is computed every time a match succeeds;
   otherwise, it is simply the length of `separator'. */
static int match_length;

/* The input buffer. */
static char *G_buffer;

/* The number of bytes to read at once into `buffer'. */
static size_t read_size;

/* The size of `buffer'.  This is read_size * 2 + sentinel_length + 2.
   The extra 2 bytes allow `past_end' to have a value beyond the
   end of `G_buffer' and `match_start' to run off the front of `G_buffer'. */
static unsigned G_buffer_size;

/* The compiled regular expression representing `separator'. */
static struct re_pattern_buffer compiled_separator;

static struct option const longopts[] = { {"before", no_argument, NULL, 'b'}, {"regex", no_argument, NULL, 'r'}, {"separator", required_argument, NULL, 's'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {NULL, 0, NULL, 0}
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
Write each FILE to standard output, last line first.\n\
With no FILE, or when FILE is -, read standard input.\n\
\n\
"), stdout);
    fputs (_("\
Mandatory arguments to long options are mandatory for short options too.\n\
"), stdout);
    fputs (_("\
  -b, --before             attach the separator before instead of after\n\
  -r, --regex              interpret the separator as a regular expression\n\
  -s, --separator=STRING   use STRING as the separator instead of newline\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* Print the characters from START to PAST_END - 1.
   If START is NULL, just flush the buffer. */

static void output (const char *start, const char *past_end) {
  static char buffer[WRITESIZE];
  static int bytes_in_buffer = 0;
  int bytes_to_add = past_end - start;
  int bytes_available = WRITESIZE - bytes_in_buffer;

  if (start == 0) {
    fwrite (buffer, 1, bytes_in_buffer, stdout);
    bytes_in_buffer = 0;
    return;
  }

  /* Write out as many full buffers as possible. */
  while (bytes_to_add >= bytes_available) {
    memcpy (buffer + bytes_in_buffer, start, bytes_available);
    bytes_to_add -= bytes_available;
    start += bytes_available;
    fwrite (buffer, 1, WRITESIZE, stdout);
    bytes_in_buffer = 0;
    bytes_available = WRITESIZE;
  }

  memcpy (buffer + bytes_in_buffer, start, bytes_to_add);
  bytes_in_buffer += bytes_to_add;
}

/* Print in reverse the file open on descriptor FD for reading FILE.
   Return 0 if ok, 1 if an error occurs. */

static int tac_seekable (int input_fd, const char *file) {
  /* Pointer to the location in `G_buffer' where the search for
     the next separator will begin. */
  char *match_start;

  /* Pointer to one past the rightmost character in `G_buffer' that
     has not been printed yet. */
  char *past_end;

  /* Length of the record growing in `G_buffer'. */
  size_t saved_record_size;

  /* Offset in the file of the next read. */
  off_t file_pos;

  /* Nonzero if `output' has not been called yet for any file.
     Only used when the separator is attached to the preceding record. */
  int first_time = 1;
  char first_char = *separator; /* Speed optimization, non-regexp. */
  char *separator1 = separator + 1; /* Speed optimization, non-regexp. */
  int match_length1 = match_length - 1; /* Speed optimization, non-regexp. */
  struct re_registers regs;

  /* Find the size of the input file. */
  file_pos = lseek (input_fd, (off_t) 0, SEEK_END);
  if (file_pos < 1)
    return 0;                   /* It's an empty file. */

  /* Arrange for the first read to lop off enough to leave the rest of the
     file a multiple of `read_size'.  Since `read_size' can change, this may
     not always hold during the program run, but since it usually will, leave
     it here for i/o efficiency (page/sector boundaries and all that).
     Note: the efficiency gain has not been verified. */
  saved_record_size = file_pos % read_size;
  if (saved_record_size == 0)
    saved_record_size = read_size;
  file_pos -= saved_record_size;
  /* `file_pos' now points to the start of the last (probably partial) block
     in the input file. */

  if (lseek (input_fd, file_pos, SEEK_SET) < 0)
    error (0, errno, "%s: seek failed", file);

  if (safe_read (input_fd, G_buffer, saved_record_size) != saved_record_size) {
    error (0, errno, "%s", file);
    return 1;
  }

  match_start = past_end = G_buffer + saved_record_size;
  /* For non-regexp search, move past impossible positions for a match. */
  if (sentinel_length)
    match_start -= match_length1;

  for (;;) {
    /* Search backward from `match_start' - 1 to `G_buffer' for a match
       with `separator'; for speed, use strncmp if `separator' contains no
       metacharacters.
       If the match succeeds, set `match_start' to point to the start of
       the match and `match_length' to the length of the match.
       Otherwise, make `match_start' < `G_buffer'. */
    if (sentinel_length == 0) {
      int i = match_start - G_buffer;
      int ret;

      ret = re_search (&compiled_separator, G_buffer, i, i - 1, -i, &regs);
      if (ret == -1)
        match_start = G_buffer - 1;
      else if (ret == -2) {
        error (EXIT_FAILURE, 0,
               _("error in regular expression search"));
      } else {
        match_start = G_buffer + regs.start[0];
        match_length = regs.end[0] - regs.start[0];
      }
    } else {
      /* `match_length' is constant for non-regexp boundaries. */
      while (*--match_start != first_char
             || (match_length1 && strncmp (match_start + 1, separator1,
                                           match_length1)))
        /* Do nothing. */ ;
    }

    /* Check whether we backed off the front of `G_buffer' without finding
       a match for `separator'. */
    if (match_start < G_buffer) {
      if (file_pos == 0) {
        /* Hit the beginning of the file; print the remaining record. */
        output (G_buffer, past_end);
        return 0;
      }

      saved_record_size = past_end - G_buffer;
      if (saved_record_size > read_size) {
        /* `G_buffer_size' is about twice `read_size', so since
           we want to read in another `read_size' bytes before
           the data already in `G_buffer', we need to increase
           `G_buffer_size'. */
        char *newbuffer;
        int offset = sentinel_length ? sentinel_length : 1;

        read_size *= 2;
        G_buffer_size = read_size * 2 + sentinel_length + 2;
        newbuffer = xrealloc (G_buffer - offset, G_buffer_size);
        newbuffer += offset;
        /* Adjust the pointers for the new buffer location.  */
        match_start += newbuffer - G_buffer;
        past_end += newbuffer - G_buffer;
        G_buffer = newbuffer;
      }

      /* Back up to the start of the next bufferfull of the file.  */
      if (file_pos >= read_size)
        file_pos -= read_size;
      else {
        read_size = file_pos;
        file_pos = 0;
      }
      lseek (input_fd, file_pos, SEEK_SET);

      /* Shift the pending record data right to make room for the new.
         The source and destination regions probably overlap.  */
      memmove (G_buffer + read_size, G_buffer, saved_record_size);
      past_end = G_buffer + read_size + saved_record_size;
      /* For non-regexp searches, avoid unneccessary scanning. */
      if (sentinel_length)
        match_start = G_buffer + read_size;
      else
        match_start = past_end;

      if (safe_read (input_fd, G_buffer, read_size) != read_size) {
        error (0, errno, "%s", file);
        return 1;
      }
    } else {
      /* Found a match of `separator'. */
      if (separator_ends_record) {
        char *match_end = match_start + match_length;

        /* If this match of `separator' isn't at the end of the
           file, print the record. */
        if (first_time == 0 || match_end != past_end)
          output (match_end, past_end);
        past_end = match_end;
        first_time = 0;
      } else {
        output (match_start, past_end);
        past_end = match_start;
      }

      /* For non-regex matching, we can back up.  */
      if (sentinel_length > 0)
        match_start -= match_length - 1;
    }
  }
}

/* Print FILE in reverse.
   Return 0 if ok, 1 if an error occurs. */

static int tac_file (const char *file) {
  int errors;
  FILE *in;

  in = fopen (file, "r");
  if (in == NULL) {
    error (0, errno, "%s", file);
    return 1;
  }
  SET_BINARY (fileno (in));
  errors = tac_seekable (fileno (in), file);
  if (ferror (in) || fclose (in) == EOF) {
    error (0, errno, "%s", file);
    return 1;
  }
  return errors;
}

/* Make a copy of the standard input in `FIXME'. */

static void save_stdin (FILE **g_tmp, char **g_tempfile) {
  static char *template = NULL;
  static char *tempdir;
  char *tempfile;
  FILE *tmp;
  ssize_t bytes_read;
  int fd;

  if (template == NULL) {
    tempdir = getenv ("TMPDIR");
    if (tempdir == NULL)
      tempdir = DEFAULT_TMPDIR;
    template = xmalloc (strlen (tempdir) + 11);
  }
  sprintf (template, "%s/tacXXXXXX", tempdir);
  tempfile = template;
  fd = mkstemp (template);
  if (fd == -1)
    error (EXIT_FAILURE, errno, "%s", tempfile);

  tmp = fdopen (fd, "w+");
  if (tmp == NULL)
    error (EXIT_FAILURE, errno, "%s", tempfile);

  unlink (tempfile);

  while (1) {
    bytes_read = safe_read (STDIN_FILENO, G_buffer, read_size);
    if (bytes_read == 0)
      break;
    if (bytes_read < 0)
      error (EXIT_FAILURE, errno, _("stdin: read error"));

    if (fwrite (G_buffer, 1, bytes_read, tmp) != bytes_read)
      break;
  }

  if (ferror (tmp) || fflush (tmp) == EOF)
    error (EXIT_FAILURE, errno, "%s", tempfile);

  SET_BINARY (fileno (tmp));
  *g_tmp = tmp;
  *g_tempfile = tempfile;
}

/* Print the standard input in reverse, saving it to temporary
   file first if it is a pipe.
   Return 0 if ok, 1 if an error occurs. */

static int tac_stdin (void) {
  int errors;
  struct stat stats;

  /* No tempfile is needed for "tac < file".
     Use fstat instead of checking for errno == ESPIPE because
     lseek doesn't work on some special files but doesn't return an
     error, either. */
  if (fstat (STDIN_FILENO, &stats)) {
    error (0, errno, _("standard input"));
    return 1;
  }

  if (S_ISREG (stats.st_mode)) {
    errors = tac_seekable (fileno (stdin), _("standard input"));
  } else {
    FILE *tmp_stream;
    char *tmp_file;
    save_stdin (&tmp_stream, &tmp_file);
    errors = tac_seekable (fileno (tmp_stream), tmp_file);
  }

  return errors;
}

int main (int argc, char **argv) {
  const char *error_message;    /* Return value from re_compile_pattern. */
  int optc, errors;
  int have_read_stdin = 0;

  program_name = argv[0];

  errors = 0;
  separator = "\n";
  sentinel_length = 1;
  separator_ends_record = 1;

  while ((optc = getopt_long (argc, argv, "brs:", longopts, NULL)) != -1) {
    switch (optc) {
    case 0:
      break;
    case 'b':
      separator_ends_record = 0;
      break;
    case 'r':
      sentinel_length = 0;
      break;
    case 's':
      separator = optarg;
      if (*separator == 0)
        error (EXIT_FAILURE, 0, _("separator cannot be empty"));
      break;
      case_GETOPT_HELP_CHAR;
      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
    default:
      usage (1);
    }
  }

  if (sentinel_length == 0) {
    compiled_separator.allocated = 100;
    /* HobbyOS: the tree's regex.h (glibc lineage) types this slot
       `struct re_dfa_t *' where 2.1's regex.h said `unsigned char *'.
       Both engines realloc this block into their own compiled-pattern
       storage, and xmalloc's void * converts implicitly, so the cast
       is dropped (same treatment as nl_gnu.c).  */
    compiled_separator.buffer = xmalloc (compiled_separator.allocated);
    compiled_separator.fastmap = xmalloc (256);
    compiled_separator.translate = 0;
    error_message = re_compile_pattern (separator, strlen (separator),
                                        &compiled_separator);
    if (error_message)
      error (EXIT_FAILURE, 0, "%s", error_message);
  } else
    match_length = sentinel_length = strlen (separator);

  read_size = INITIAL_READSIZE;
  /* A precaution that will probably never be needed. */
  while (sentinel_length * 2 >= read_size)
    read_size *= 2;
  G_buffer_size = read_size * 2 + sentinel_length + 2;
  G_buffer = xmalloc (G_buffer_size);
  if (sentinel_length) {
    strcpy (G_buffer, separator);
    G_buffer += sentinel_length;
  } else {
    ++G_buffer;
  }

  if (optind == argc) {
    have_read_stdin = 1;
    /* We need binary I/O, since `tac' relies
       on `lseek' and byte counts.  */
    SET_BINARY2 (STDIN_FILENO, STDOUT_FILENO);
    errors = tac_stdin ();
  } else {
    for (; optind < argc; ++optind) {
      if (STREQ (argv[optind], "-")) {
        have_read_stdin = 1;
        SET_BINARY2 (STDIN_FILENO, STDOUT_FILENO);
        errors |= tac_stdin ();
      } else {
        /* Binary output will leave the lines' ends (NL or
           CR/LF) intact when the output is a disk file.
           Writing a file with CR/LF pairs at end of lines in
           text mode has no visible effect on console output,
           since two CRs in a row are just like one CR.  */
        SET_BINARY (STDOUT_FILENO);
        errors |= tac_file (argv[optind]);
      }
    }
  }

  /* Flush the output buffer. */
  output ((char *) NULL, (char *) NULL);

  if (have_read_stdin && close (0) < 0)
    error (EXIT_FAILURE, errno, "-");
  exit (errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
