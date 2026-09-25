/* nl -- number lines of files
   Copyright (C) 89, 92, 1995-2001 Free Software Foundation, Inc.

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

/* Written by Scott Bartram (nancy!scott@uunet.uu.net)
   Revised by David MacKenzie (djm@gnu.ai.mit.edu) */

/* The GNU re_* interface (re_set_syntax, re_compile_pattern, re_search,
   struct re_pattern_buffer) is gated behind _GNU_SOURCE, mirroring
   glibc; it must be defined before the first header is read.  */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <getopt.h>
#include <regex.h>
#include "error.h"

#define _(s) (s)

/* HobbyOS port: faithful copy of the GNU textutils-2.1 nl.c with the
   gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc ->
   local allocating helper over the sysroot malloc, xstrtol reduced to
   the base-10 no-suffix shape nl actually calls, the linebuffer
   routines (initbuffer/readline) transcribed from lib/linebuffer.c at
   the bottom of this file, regex from the sysroot's GNU regex
   (src/libc/src/regex.c, the engine REGTEST.BIN races against glibc),
   close_stdout covered by exit()'s weak fflush(0), setlocale/
   bindtextdomain/textdomain dropped (single C locale).  One 2.1-ism is
   kept on purpose: print_lineno() emits an unnumbered line's field
   with puts(), so such a line contributes an extra newline (modern nl
   uses fputs; that is the cmp_case_21 bucket in nl_parity.sh).  The
   GETOPT_HELP_OPTION_DECL/GETOPT_VERSION_OPTION_DECL macros are
   brace-less (sys2.h form), so --help/--version reach their switch
   cases and print, matching 2.1.  The host build (Makefile nl_host)
   races this against the reference build in src/host/nl_parity.sh;
   byte-exact output is the bar.  */

/* The official name of this program (e.g., no `g' prefix).  */
#define PROGRAM_NAME "nl"

#define AUTHORS "Scott Bartram and David MacKenzie"

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

#define STREQ(a, b) (strcmp ((a), (b)) == 0)
#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')

/* glibc getopt.h's long-only option chars.  These decl macros are
   BRACE-LESS (as textutils' sys2.h defines them): call sites wrap them
   as {GETOPT_HELP_OPTION_DECL}.  A braced definition would make the
   call site double-brace the struct, leaving has_arg/flag/val zero and
   silently disabling --help/--version.  */
#define GETOPT_HELP_CHAR -2
#define GETOPT_VERSION_CHAR -3
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

/* xstrtol.h's error codes, transcribed from textutils-2.1 lib/xstrtol.h.  */
enum strtol_error {
  LONGINT_OK,
  LONGINT_INVALID,
  LONGINT_INVALID_SUFFIX_CHAR,
  LONGINT_OVERFLOW
};
typedef enum strtol_error strtol_error;

/* xstrtol -- transcribed from textutils-2.1 lib/xstrtol.c (written by
   Jim Meyering), reduced to the one shape nl calls it in: base 10 with an
   empty valid_suffixes string, so the b/k/m scaling machinery
   (bkm_scale/bkm_scale_by_power) is unreachable and dropped.  The
   semantics kept: LONGINT_OK for a plain decimal that fits a long int,
   LONGINT_INVALID when no digits are present (empty string included),
   LONGINT_INVALID_SUFFIX_CHAR for trailing junk, LONGINT_OVERFLOW when
   strtol overflows.  */
static strtol_error xstrtol (const char *s, char **ptr, int strtol_base,
                             long int *val, const char *valid_suffixes) {
  char *t_ptr;
  char **p;
  long int tmp;

  assert (0 <= strtol_base && strtol_base <= 36);

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
    /* nl passes "" for every call, so any trailing character (a suffix
       letter included) lands here.  */
    if (!strchr (valid_suffixes, **p)) {
      *val = tmp;
      return LONGINT_INVALID_SUFFIX_CHAR;
    }
  }

  *val = tmp;
  return LONGINT_OK;
}

/* `struct linebuffer' from lib/linebuffer.h: holds a line of text.  */
struct linebuffer {
  size_t size;   /* Allocated. */
  size_t length; /* Used. */
  char *buffer;
};

/* Transcribed from lib/linebuffer.c (definitions at the bottom of this
   file); nl reads its input with readline, which keeps the newline and
   appends one to a last line that lacks it.  */
static void initbuffer (struct linebuffer *linebuffer);
static struct linebuffer *readline (struct linebuffer *linebuffer, FILE *stream);

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* Line-number formats. */
enum number_format {
  FORMAT_RIGHT_NOLZ, /* Right justified, no leading zeroes.  */
  FORMAT_RIGHT_LZ,   /* Right justified, leading zeroes.  */
  FORMAT_LEFT        /* Left justified, no leading zeroes.  */
};

/* Default section delimiter characters.  */
#define DEFAULT_SECTION_DELIMITERS "\\:"

/* Types of input lines: either one of the section delimiters,
   or text to output. */
enum section { Header, Body, Footer, Text };

/* Format of body lines (-b).  */
static char *body_type = "t";

/* Format of header lines (-h).  */
static char *header_type = "n";

/* Format of footer lines (-f).  */
static char *footer_type = "n";

/* Format currently being used (body, header, or footer).  */
static char *current_type;

/* Regex for body lines to number (-bp).  */
static struct re_pattern_buffer body_regex;

/* Regex for header lines to number (-hp).  */
static struct re_pattern_buffer header_regex;

/* Regex for footer lines to number (-fp).  */
static struct re_pattern_buffer footer_regex;

/* Pointer to current regex, if any.  */
static struct re_pattern_buffer *current_regex = NULL;

/* Separator string to print after line number (-s).  */
static char *separator_str = "\t";

/* Input section delimiter string (-d).  */
static char *section_del = DEFAULT_SECTION_DELIMITERS;

/* Header delimiter string.  */
static char *header_del = NULL;

/* Header section delimiter length.  */
static size_t header_del_len;

/* Body delimiter string.  */
static char *body_del = NULL;

/* Body section delimiter length.  */
static size_t body_del_len;

/* Footer delimiter string.  */
static char *footer_del = NULL;

/* Footer section delimiter length.  */
static size_t footer_del_len;

/* Input buffer.  */
static struct linebuffer line_buf;

/* printf format string for line number.  */
static char *print_fmt;

/* printf format string for unnumbered lines.  */
static char *print_no_line_fmt = NULL;

/* Starting line number on each page (-v).  */
static int starting_line_number = 1;

/* Line number increment (-i).  */
static int page_incr = 1;

/* If TRUE, reset line number at start of each page (-p).  */
static int reset_numbers = TRUE;

/* Number of blank lines to consider to be one line for numbering (-l).  */
static int blank_join = 1;

/* Width of line numbers (-w).  */
static int lineno_width = 6;

/* Line number format (-n).  */
static enum number_format lineno_format = FORMAT_RIGHT_NOLZ;

/* Current print line number.  */
static int line_no;

/* Nonzero if we have ever read standard input. */
static int have_read_stdin;

static struct option const longopts[] = { {"header-numbering", required_argument, NULL, 'h'}, {"body-numbering", required_argument, NULL, 'b'}, {"footer-numbering", required_argument, NULL, 'f'}, {"starting-line-number", required_argument, NULL, 'v'}, {"page-increment", required_argument, NULL, 'i'}, {"no-renumber", no_argument, NULL, 'p'}, {"join-blank-lines", required_argument, NULL, 'l'}, {"number-separator", required_argument, NULL, 's'}, {"number-width", required_argument, NULL, 'w'}, {"number-format", required_argument, NULL, 'n'}, {"section-delimiter", required_argument, NULL, 'd'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {NULL, 0, NULL, 0}
};

/* Print a usage message and quit. */

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
Write each FILE to standard output, with line numbers added.\n\
With no FILE, or when FILE is -, read standard input.\n\
\n\
"), stdout);
    fputs (_("\
Mandatory arguments to long options are mandatory for short options too.\n\
"), stdout);
    fputs (_("\
  -b, --body-numbering=STYLE      use STYLE for numbering body lines\n\
  -d, --section-delimiter=CC      use CC for separating logical pages\n\
  -f, --footer-numbering=STYLE    use STYLE for numbering footer lines\n\
"), stdout);
    fputs (_("\
  -h, --header-numbering=STYLE    use STYLE for numbering header lines\n\
  -i, --page-increment=NUMBER     line number increment at each line\n\
  -l, --join-blank-lines=NUMBER   group of NUMBER empty lines counted as one\n\
  -n, --number-format=FORMAT      insert line numbers according to FORMAT\n\
  -p, --no-renumber               do not reset line numbers at logical pages\n\
  -s, --number-separator=STRING   add STRING after (possible) line number\n\
"), stdout);
    fputs (_("\
  -v, --first-page=NUMBER         first line number on each logical page\n\
  -w, --number-width=NUMBER       use NUMBER columns for line numbers\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    fputs (_("\
\n\
By default, selects -v1 -i1 -l1 -sTAB -w6 -nrn -hn -bt -fn.  CC are\n\
two delimiter characters for separating logical pages, a missing\n\
second character implies :.  Type \\\\ for \\.  STYLE is one of:\n\
"), stdout);
    fputs (_("\
\n\
  a         number all lines\n\
  t         number only nonempty lines\n\
  n         number no lines\n\
  pREGEXP   number only lines that contain a match for REGEXP\n\
\n\
FORMAT is one of:\n\
\n\
  ln   left justified, no leading zeros\n\
  rn   right justified, no leading zeros\n\
  rz   right justified, leading zeros\n\
\n\
"), stdout);
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* Build the printf format string, based on `lineno_format'. */

static void build_print_fmt (void) {
  /* 12 = 10 chars for lineno_width, 1 for %, 1 for \0.  */
  print_fmt = xmalloc (strlen (separator_str) + 12);
  switch (lineno_format) {
  case FORMAT_RIGHT_NOLZ:
    sprintf (print_fmt, "%%%dd%s", lineno_width, separator_str);
    break;
  case FORMAT_RIGHT_LZ:
    sprintf (print_fmt, "%%0%dd%s", lineno_width, separator_str);
    break;
  case FORMAT_LEFT:
    sprintf (print_fmt, "%%-%dd%s", lineno_width, separator_str);
    break;
  }
}

/* Set the command line flag TYPEP and possibly the regex pointer REGEXP,
   according to `optarg'.  */

static int build_type_arg (char **typep, struct re_pattern_buffer *regexp) {
  const char *errmsg;
  int rval = TRUE;
  int optlen;

  switch (*optarg) {
  case 'a':
  case 't':
  case 'n':
    *typep = optarg;
    break;
  case 'p':
    *typep = optarg++;
    optlen = strlen (optarg);
    regexp->allocated = optlen * 2;
    /* HobbyOS: the tree's regex.h (glibc lineage) types this slot
       `struct re_dfa_t *' where 2.1's regex.h said `unsigned char *'.
       Both engines realloc this block into their own compiled-pattern
       storage, and xmalloc's void * converts implicitly, so the cast
       is dropped.  */
    regexp->buffer = xmalloc (regexp->allocated);
    regexp->translate = NULL;
    regexp->fastmap = xmalloc (256);
    regexp->fastmap_accurate = 0;
    errmsg = re_compile_pattern (optarg, optlen, regexp);
    if (errmsg)
      error (EXIT_FAILURE, 0, "%s", errmsg);
    break;
  default:
    rval = FALSE;
    break;
  }
  return rval;
}

/* Print and increment the line number. */

static void print_lineno (void) {
  printf (print_fmt, line_no);
  line_no += page_incr;
}

/* Switch to a header section. */

static void proc_header (void) {
  current_type = header_type;
  current_regex = &header_regex;
  if (reset_numbers)
    line_no = starting_line_number;
  putchar ('\n');
}

/* Switch to a body section. */

static void proc_body (void) {
  current_type = body_type;
  current_regex = &body_regex;
  putchar ('\n');
}

/* Switch to a footer section. */

static void proc_footer (void) {
  current_type = footer_type;
  current_regex = &footer_regex;
  putchar ('\n');
}

/* Process a regular text line in `line_buf'. */

static void proc_text (void) {
  static int blank_lines = 0; /* Consecutive blank lines so far. */

  switch (*current_type) {
  case 'a':
    if (blank_join > 1) {
      if (1 < line_buf.length || ++blank_lines == blank_join) {
        print_lineno ();
        blank_lines = 0;
      } else
        puts (print_no_line_fmt);
    } else
      print_lineno ();
    break;
  case 't':
    if (1 < line_buf.length)
      print_lineno ();
    else
      puts (print_no_line_fmt);
    break;
  case 'n':
    puts (print_no_line_fmt);
    break;
  case 'p':
    if (re_search (current_regex, line_buf.buffer, line_buf.length - 1, 0,
                   line_buf.length - 1, (struct re_registers *) 0) < 0)
      puts (print_no_line_fmt);
    else
      print_lineno ();
    break;
  }
  fwrite (line_buf.buffer, sizeof (char), line_buf.length, stdout);
}

/* Return the type of line in `line_buf'. */

static enum section check_section (void) {
  size_t len = line_buf.length - 1;

  if (len < 2 || memcmp (line_buf.buffer, section_del, 2))
    return Text;
  if (len == header_del_len && !memcmp (line_buf.buffer, header_del, header_del_len))
    return Header;
  if (len == body_del_len && !memcmp (line_buf.buffer, body_del, body_del_len))
    return Body;
  if (len == footer_del_len && !memcmp (line_buf.buffer, footer_del, footer_del_len))
    return Footer;
  return Text;
}

/* Read and process the file pointed to by FP. */

static void process_file (FILE *fp) {
  while (readline (&line_buf, fp)) {
    switch ((int) check_section ()) {
    case Header:
      proc_header ();
      break;
    case Body:
      proc_body ();
      break;
    case Footer:
      proc_footer ();
      break;
    case Text:
      proc_text ();
      break;
    }
  }
}

/* Process file FILE to standard output.
   Return 0 if successful, 1 if not. */

static int nl_file (const char *file) {
  FILE *stream;

  if (STREQ (file, "-")) {
    have_read_stdin = 1;
    stream = stdin;
  } else {
    stream = fopen (file, "r");
    if (stream == NULL) {
      error (0, errno, "%s", file);
      return 1;
    }
  }

  process_file (stream);

  if (ferror (stream)) {
    error (0, errno, "%s", file);
    return 1;
  }
  if (STREQ (file, "-"))
    clearerr (stream); /* Also clear EOF. */
  else if (fclose (stream) == EOF) {
    error (0, errno, "%s", file);
    return 1;
  }
  return 0;
}

int main (int argc, char **argv) {
  int c, exit_status = 0;
  size_t len;

  program_name = argv[0];

  have_read_stdin = 0;

  while ((c = getopt_long (argc, argv, "h:b:f:v:i:pl:s:w:n:d:", longopts, NULL)) != -1) {
    switch (c) {
    case 0:
      break;

    case 'h':
      if (build_type_arg (&header_type, &header_regex) != TRUE)
        usage (2);
      break;
    case 'b':
      if (build_type_arg (&body_type, &body_regex) != TRUE)
        usage (2);
      break;
    case 'f':
      if (build_type_arg (&footer_type, &footer_regex) != TRUE)
        usage (2);
      break;
    case 'v': {
        long int tmp_long;
        if (xstrtol (optarg, NULL, 10, &tmp_long, "") != LONGINT_OK
            /* Allow it to be negative.  */
            || tmp_long > INT_MAX)
          error (EXIT_FAILURE, 0, _("invalid starting line number: `%s'"),
                 optarg);
        starting_line_number = (int) tmp_long;
      } break;
    case 'i': {
        long int tmp_long;
        if (xstrtol (optarg, NULL, 10, &tmp_long, "") != LONGINT_OK
            || tmp_long <= 0 || tmp_long > INT_MAX)
          error (EXIT_FAILURE, 0, _("invalid line number increment: `%s'"),
                 optarg);
        page_incr = (int) tmp_long;
      } break;
    case 'p':
      reset_numbers = FALSE;
      break;
    case 'l': {
        long int tmp_long;
        if (xstrtol (optarg, NULL, 10, &tmp_long, "") != LONGINT_OK
            || tmp_long <= 0 || tmp_long > INT_MAX)
          error (EXIT_FAILURE, 0, _("invalid number of blank lines: `%s'"),
                 optarg);
        blank_join = (int) tmp_long;
      } break;
    case 's':
      separator_str = optarg;
      break;
    case 'w': {
        long int tmp_long;
        if (xstrtol (optarg, NULL, 10, &tmp_long, "") != LONGINT_OK
            || tmp_long <= 0 || tmp_long > INT_MAX)
          error (EXIT_FAILURE, 0,
                 _("invalid line number field width: `%s'"),
                 optarg);
        lineno_width = (int) tmp_long;
      } break;
    case 'n':
      switch (*optarg) {
      case 'l':
        if (optarg[1] == 'n')
          lineno_format = FORMAT_LEFT;
        else
          usage (2);
        break;
      case 'r':
        switch (optarg[1]) {
        case 'n':
          lineno_format = FORMAT_RIGHT_NOLZ;
          break;
        case 'z':
          lineno_format = FORMAT_RIGHT_LZ;
          break;
        default:
          usage (2);
          break;
        }
        break;
      default:
        usage (2);
        break;
      }
      break;
    case 'd':
      section_del = optarg;
      break;
      case_GETOPT_HELP_CHAR;

      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

    default:
      usage (2);
      break;
    }
  }

  /* Initialize the section delimiters.  */
  len = strlen (section_del);

  header_del_len = len * 3;
  header_del = xmalloc (header_del_len + 1);
  strcat (strcat (strcpy (header_del, section_del), section_del), section_del);

  body_del_len = len * 2;
  body_del = xmalloc (body_del_len + 1);
  strcat (strcpy (body_del, section_del), section_del);

  footer_del_len = len;
  footer_del = xmalloc (footer_del_len + 1);
  strcpy (footer_del, section_del);

  /* Initialize the input buffer.  */
  initbuffer (&line_buf);

  /* Initialize the printf format for unnumbered lines. */
  len = strlen (separator_str);
  print_no_line_fmt = xmalloc (lineno_width + len + 1);
  memset (print_no_line_fmt, ' ', lineno_width + len);
  print_no_line_fmt[lineno_width + len] = '\0';

  line_no = starting_line_number;
  current_type = body_type;
  current_regex = &body_regex;
  build_print_fmt ();

  /* Main processing. */

  if (optind == argc)
    exit_status |= nl_file ("-");
  else
    for (; optind < argc; optind++)
      exit_status |= nl_file (argv[optind]);

  if (have_read_stdin && fclose (stdin) == EOF) {
    error (0, errno, "-");
    exit_status = 1;
  }

  exit (exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* linebuffer -- read arbitrarily long lines.
   Transcribed from textutils-2.1 lib/linebuffer.c (written by Richard
   Stallman) as nl's input path.  */

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
