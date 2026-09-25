/* md5sum -- compute MD5 or SHA1 checksum of files or strings
   Copyright (C) 1995-2002 Free Software Foundation, Inc.

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

/* Written by Ulrich Drepper <drepper@gnu.ai.mit.edu>.  */

/* HobbyOS port: faithful copy of GNU textutils-2.1's src/md5sum.c with
   the gnulib scaffolding replaced by the Phase-2/3 sysroot: error() from
   error.h/error.c, getopt_long from our getopt.h, xmalloc -> a local
   allocating helper over the sysroot malloc, close_stdout covered by
   exit()'s weak fflush(0), setlocale/bindtextdomain/textdomain dropped
   (single C locale), _()/N_() identity, system.h/sys2.h reduced to the
   hand-picked macros this file uses (STREQ/MAX/ISXDIGIT/TOLOWER and the
   Unix no-op text/binary I/O definitions).

   md5sum only: 2.1 builds both md5sum and shasum from this source, the
   engine being chosen at run time through `algorithm' (set by the tiny
   wrapper src/md5.c).  Only the MD5 engine is ported, so the digest
   dispatch is constant and shasum's arm of the `--string' branch is
   dropped; the embedded src/md5.c pins `algorithm = ALG_MD5'.

   The in-OS build is a single translation unit (md5sum_gnu.o + libc.a),
   so the parts gnulib/the libraries would contribute are embedded at the
   marked points below: md5.h and md5.c are transcribed from
   third_party/textutils-2.1/lib/ (same FSF texts; only the definitions
   are modernized to ANSI prototypes), and getline() is transcribed from
   lib/getline.c + lib/getstr.c (named md5_getline here, to avoid the
   sysroot <stdio.h> declaration).

   The host build (Makefile md5sum_host) races this against the 2.1
   reference build in src/host/md5sum_parity.sh; byte-exact output, exit
   codes and diagnostics are the bar.  */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include "error.h"

#define _(s) (s)
#define N_(s) (s)

/* From textutils-2.1's sys2.h, the macros this file relies on.  */
#define STREQ(a, b) (strcmp ((a), (b)) == 0)
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ISXDIGIT(c) isxdigit (c)
#define TOLOWER(Ch) tolower (Ch)

/* Text/binary I/O: system.h's Unix branch.  There is one read mode, so
   SET_MODE/SET_BINARY are no-ops and OPENOPTS is "r".  */
#define O_BINARY 0
#define O_TEXT 0
#define SET_MODE(f, m) ((void) 0)
#define SET_BINARY(f) ((void) 0)
#define OPENOPTS(BINARY) "r"

/* glibc getopt.h's long-only option chars (GETOPT_HELP_OPTION_DECL /
   GETOPT_VERSION_OPTION_DECL are brace-less, exactly as in sys2.h).  */
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

#define PROGRAM_NAME (algorithm == ALG_MD5 ? "md5sum" : "shasum")

#define AUTHORS N_ ("Ulrich Drepper and Scott Miller")

#define PACKAGE "textutils"
#define PACKAGE_VERSION "2.1"
#define PACKAGE_BUGREPORT "bug-textutils@gnu.org"

/* Most systems do not distinguish between external and internal
   text representations.  */
/* HobbyOS port: the port targets system.h's Unix branch only -- O_BINARY
   is 0, both modes read with "r", and stdin needs no setmode -- so the
   DOS/VMS OPENOPTS dances are not carried over.  */

/* The embedded checksum.h: 2.1's algorithm selector, whose `algorithm'
   variable the MD5 build's src/md5.c sets to ALG_MD5.  */
enum {
  ALG_UNSPECIFIED = 0,
  ALG_MD5 = CHAR_MAX + 1,
  ALG_SHA1
};

extern int algorithm;

/* The name this program was run with (defined by the sysroot error.c).  */
extern char *program_name;

/* Allocating helpers in the spirit of the gnulib x* wrappers: the
   original code calls xmalloc/xalloc_die for the --string vector.  */
static void xalloc_die (void) {
  error (EXIT_FAILURE, 0, _("memory exhausted"));
}

static void *xmalloc (size_t n) {
  void *p = malloc (n);
  if (p == NULL)
    xalloc_die ();
  return p;
}

/* ===================================================================
   BEGIN EMBEDDED md5.h (third_party/textutils-2.1/lib/md5.h)
   The original md5sum.c says `#include "md5.h"'; the in-OS build is a
   single translation unit, so the header's text is embedded here.
   HobbyOS port: the `#if defined HAVE_LIMITS_H || _LIBC' guard around
   <limits.h> is dropped (the sysroot always has it), and the i386
   inline-asm `rol' is kept as-is -- x86_64/aarch64 builds take the
   macro branch right below it.
   =================================================================== */

/* md5.h - Declaration of functions and data types used for MD5 sum
   computing library functions.
   Copyright (C) 1995, 1996, 1999 Free Software Foundation, Inc.
   NOTE: The canonical source of this file is maintained with the GNU C
   Library.  Bugs can be reported to bug-glibc@prep.ai.mit.edu.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#ifndef _MD5_H
#define _MD5_H 1

#include <stdio.h>

#include <limits.h>

/* The following contortions are an attempt to use the C preprocessor
   to determine an unsigned integral type that is 32 bits wide.  An
   alternative approach is to use autoconf's AC_CHECK_SIZEOF macro, but
   doing that would require that the configure script compile and *run*
   the resulting executable.  Locally running cross-compiled executables
   is usually not possible.  */

#ifdef _LIBC
# include <sys/types.h>
typedef u_int32_t md5_uint32;
#else
# if defined __STDC__ && __STDC__
#  define UINT_MAX_32_BITS 4294967295U
# else
#  define UINT_MAX_32_BITS 0xFFFFFFFF
# endif

/* If UINT_MAX isn't defined, assume it's a 32-bit type.
   This should be valid for all systems GNU cares about because
   that doesn't include 16-bit systems, and only modern systems
   (that certainly have <limits.h>) have 64+-bit integral types.  */

# ifndef UINT_MAX
#  define UINT_MAX UINT_MAX_32_BITS
# endif

# if UINT_MAX == UINT_MAX_32_BITS
typedef unsigned int md5_uint32;
# else
#  if USHRT_MAX == UINT_MAX_32_BITS
typedef unsigned short md5_uint32;
#  else
#   if ULONG_MAX == UINT_MAX_32_BITS
typedef unsigned long md5_uint32;
#   else
/* The following line is intended to evoke an error.
   Using #error is not portable enough.  */
"Cannot determine unsigned 32-bit data type."
#   endif
#  endif
# endif
#endif

#undef __P
#if defined (__STDC__) && __STDC__
#define __P(x) x
#else
#define __P(x) ()
#endif

/* Structure to save state of computation between the single steps.  */
struct md5_ctx {
  md5_uint32 A;
  md5_uint32 B;
  md5_uint32 C;
  md5_uint32 D;

  md5_uint32 total[2];
  md5_uint32 buflen;
  char buffer[128];
};

/*
 * The following three functions are build up the low level used in
 * the functions `md5_stream' and `md5_buffer'.
 */

/* Initialize structure containing state of computation.
   (RFC 1321, 3.3: Step 3)  */
extern void md5_init_ctx __P ((struct md5_ctx *ctx));

/* Starting with the result of former calls of this function (or the
   initialization function update the context for the next LEN bytes
   starting at BUFFER.
   It is necessary that LEN is a multiple of 64!!! */
extern void md5_process_block __P ((const void *buffer, size_t len,
                                    struct md5_ctx *ctx));

/* Starting with the result of former calls of this function (or the
   initialization function update the context for the next LEN bytes
   starting at BUFFER.
   It is NOT required that LEN is a multiple of 64.  */
extern void md5_process_bytes __P ((const void *buffer, size_t len,
                                    struct md5_ctx *ctx));

/* Process the remaining bytes in the buffer and put result from CTX
   in first 16 bytes following RESBUF.  The result is always in little
   endian byte order, so that a byte-wise output yields to the wanted
   ASCII representation of the message digest.

   IMPORTANT: On some systems it is required that RESBUF be correctly
   aligned for a 32 bits value.  */
extern void *md5_finish_ctx __P ((struct md5_ctx *ctx, void *resbuf));

/* Put result from CTX in first 16 bytes following RESBUF.  The result is
   always in little endian byte order, so that a byte-wise output yields
   to the wanted ASCII representation of the message digest.

   IMPORTANT: On some systems it is required that RESBUF is correctly
   aligned for a 32 bits value.  */
extern void *md5_read_ctx __P ((const struct md5_ctx *ctx, void *resbuf));

/* Compute MD5 message digest for bytes read from STREAM.  The
   resulting message digest number will be written into the 16 bytes
   beginning at RESBLOCK.  */
extern int md5_stream __P ((FILE *stream, void *resblock));

/* Compute MD5 message digest for LEN bytes beginning at BUFFER.  The
   result is always in little endian byte order, so that a byte-wise
   output yields to the wanted ASCII representation of the message
   digest.  */
extern void *md5_buffer __P ((const char *buffer, size_t len, void *resblock));

/* The following is from gnupg-1.0.2's cipher/bithelp.h.  */
/* Rotate a 32 bit integer by n bytes */
#if defined __GNUC__ && defined __i386__
static inline md5_uint32 rol(md5_uint32 x, int n) {
  __asm__("roll %%cl,%0"
          :"=r" (x)
          :"0" (x),"c" (n));
  return x;
}
#else
# define rol(x,n) ( ((x) << (n)) | ((x) >> (32-(n))) )
#endif

#endif

/* ===================================================================
   END EMBEDDED md5.h
   =================================================================== */

#define DIGEST_TYPE_STRING(Alg) ((Alg) == ALG_MD5 ? "MD5" : "SHA1")

/* HobbyOS port: 2.1 dispatches on `algorithm' here
   (`(Alg) == ALG_MD5 ? md5_stream : sha_stream'); the SHA1 engine is not
   ported, so this resolves to the MD5 engine (and `algorithm' is pinned
   to ALG_MD5 by the embedded src/md5.c below).  */
#define DIGEST_STREAM(Alg) (md5_stream)

#define DIGEST_BITS(Alg) ((Alg) == ALG_MD5 ? 128 : 160)
#define DIGEST_HEX_BYTES(Alg) (DIGEST_BITS (Alg) / 4)
#define DIGEST_BIN_BYTES(Alg) (DIGEST_BITS (Alg) / 8)

#define MAX_DIGEST_BIN_BYTES MAX (DIGEST_BIN_BYTES (ALG_MD5), \
                                  DIGEST_BIN_BYTES (ALG_SHA1))

/* The minimum length of a valid digest line.  This length does
   not include any newline character at the end of a line.  */
#define MIN_DIGEST_LINE_LENGTH(Alg) \
  (DIGEST_HEX_BYTES (Alg) /* length of hexadecimal message digest */ \
   + 2 /* blank and binary indicator */ \
   + 1 /* minimum filename length */)

/* Nonzero if any of the files read were the standard input. */
static int have_read_stdin;

/* The minimum length of a valid checksum line for the selected algorithm.
   (HobbyOS port: declared size_t -- 2.1 has `int' -- so the unsigned
   s_len - i comparisons in split_3 stay -Wsign-compare clean; the value
   always fits in an int.)  */
static size_t min_digest_line_length;

/* Set to the length of a digest hex string for the selected algorithm.  */
static size_t digest_hex_bytes;

/* With --check, don't generate any output.
   The exit code indicates success or failure.  */
static int status_only = 0;

/* With --check, print a message to standard error warning about each
   improperly formatted checksum line.  */
static int warn = 0;

/* Declared and set via one of the wrapper .c files.  */
/* int algorithm = ALG_UNSPECIFIED; */

static const struct option long_options[] = { {"binary", no_argument, 0, 'b'}, {"check", no_argument, 0, 'c'}, {"status", no_argument, 0, 2}, {"string", required_argument, 0, 1}, {"text", no_argument, 0, 't'}, {"warn", no_argument, 0, 'w'}, {GETOPT_HELP_OPTION_DECL}, {GETOPT_VERSION_OPTION_DECL}, {NULL, 0, NULL, 0}
};

void usage (int status) {
  if (status != 0)
    fprintf (stderr, _("Try `%s --help' for more information.\n"),
             program_name);
  else {
    printf (_("\
Usage: %s [OPTION] [FILE]...\n\
  or:  %s [OPTION] --check [FILE]\n\
Print or check %s (%d-bit) checksums.\n\
With no FILE, or when FILE is -, read standard input.\n\
"),
            program_name, program_name,
            DIGEST_TYPE_STRING (algorithm),
            DIGEST_BITS (algorithm));
    printf (_("\
\n\
  -b, --binary            read files in binary mode (default on DOS/Windows)\n\
  -c, --check             check %s sums against given list\n\
  -t, --text              read files in text mode (default)\n\
\n\
"),
            DIGEST_TYPE_STRING (algorithm));
    fputs (_("\
The following two options are useful only when verifying checksums:\n\
      --status            don't output anything, status code shows success\n\
  -w, --warn              warn about improperly formated checksum lines\n\
\n\
"), stdout);
    fputs (HELP_OPTION_DESCRIPTION, stdout);
    fputs (VERSION_OPTION_DESCRIPTION, stdout);
    printf (_("\
\n\
The sums are computed as described in %s.  When checking, the input\n\
should be a former output of this program.  The default mode is to print\n\
a line with checksum, a character indicating type (`*' for binary, ` ' for\n\
text), and name for each FILE.\n"),
            (algorithm == ALG_MD5 ? "RFC 1321" : "FIPS-180-1"));
    printf (_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
  }

  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int split_3 (char *s, size_t s_len, unsigned char **u, int *binary, char **w) {
  size_t i;
  int escaped_filename = 0;

#define ISWHITE(c) ((c) == ' ' || (c) == '\t')

  i = 0;
  while (ISWHITE (s[i]))
    ++i;

  /* The line must have at least `min_digest_line_length - 1' (or one more, if
     the first is a backslash) more characters to contain correct message digest
     information.  Ignore this line if it is too short.  */
  if (!(s_len - i >= min_digest_line_length
        || (s[i] == '\\' && s_len - i >= 1 + min_digest_line_length)))
    return 1;

  if (s[i] == '\\') {
    ++i;
    escaped_filename = 1;
  }
  *u = (unsigned char *) &s[i];

  /* The first field has to be the n-character hexadecimal
     representation of the message digest.  If it is not followed
     immediately by a white space it's an error.  */
  i += digest_hex_bytes;
  if (!ISWHITE (s[i]))
    return 1;

  s[i++] = '\0';

  if (s[i] != ' ' && s[i] != '*')
    return 1;
  *binary = (s[i++] == '*');

  /* All characters between the type indicator and end of line are
     significant -- that includes leading and trailing white space.  */
  *w = &s[i];

  if (escaped_filename) {
    /* Translate each `\n' string in the file name to a NEWLINE,
       and each `\\' string to a backslash.  */

    char *dst = &s[i];

    while (i < s_len) {
      switch (s[i]) {
      case '\\':
        if (i == s_len - 1) {
          /* A valid line does not end with a backslash.  */
          return 1;
        }
        ++i;
        switch (s[i++]) {
        case 'n':
          *dst++ = '\n';
          break;
        case '\\':
          *dst++ = '\\';
          break;
        default:
          /* Only `\' or `n' may follow a backslash.  */
          return 1;
        }
        break;

      case '\0':
        /* The file name may not contain a NUL.  */
        return 1;
        break;

      default:
        *dst++ = s[i++];
        break;
      }
    }
    *dst = '\0';
  }
  return 0;
}

static int hex_digits (unsigned char const *s) {
  while (*s) {
    if (!ISXDIGIT (*s))
      return 0;
    ++s;
  }
  return 1;
}

/* An interface to the function, DIGEST_STREAM, (either md5_stream or sha_stream).
   Operate on FILENAME (it may be "-") and put the result in *BIN_RESULT.
   Return non-zero upon failure, zero to indicate success.  */

static int digest_file (const char *filename, int binary, unsigned char *bin_result,
                        int (*digest_stream)(FILE *, void *)) {
  FILE *fp;
  int err;

  if (STREQ (filename, "-")) {
    have_read_stdin = 1;
    fp = stdin;
#if O_BINARY
    /* If we need binary reads from a pipe or redirected stdin, we need
       to switch it to BINARY mode here, since stdin is already open.  */
    if (binary)
      SET_BINARY (fileno (stdin));
#else
    /* HobbyOS port: one read mode only, so `binary' acts as the output
       marker alone (2.1's Unix branch compiles no call here either).  */
    (void) binary;
#endif
  } else {
    /* OPENOPTS is a macro.  It varies with the system.
       Some systems distinguish between internal and
       external text representations.  */

    fp = fopen (filename, OPENOPTS (binary));
    if (fp == NULL) {
      error (0, errno, "%s", filename);
      return 1;
    }
  }

  err = (*digest_stream) (fp, bin_result);
  if (err) {
    error (0, errno, "%s", filename);
    if (fp != stdin)
      fclose (fp);
    return 1;
  }

  if (fp != stdin && fclose (fp) == EOF) {
    error (0, errno, "%s", filename);
    return 1;
  }

  return 0;
}

/* Transcribed from lib/getline.c + lib/getstr.c (definitions at the
   bottom of this file); same signatures, the line reader's name carries
   the md5_ prefix to stay clear of the sysroot <stdio.h> declaration.  */
static int getstr (char **lineptr, size_t *n, FILE *stream, int delim1, int delim2, size_t offset);
static int md5_getline (char **lineptr, size_t *n, FILE *stream);

static int digest_check (const char *checkfile_name, int (*digest_stream)(FILE *, void *)) {
  FILE *checkfile_stream;
  int n_properly_formated_lines = 0;
  int n_mismatched_checksums = 0;
  int n_open_or_read_failures = 0;
  unsigned char bin_buffer[MAX_DIGEST_BIN_BYTES];
  size_t line_number;
  char *line;
  size_t line_chars_allocated;

  if (STREQ (checkfile_name, "-")) {
    have_read_stdin = 1;
    checkfile_name = _("standard input");
    checkfile_stream = stdin;
  } else {
    checkfile_stream = fopen (checkfile_name, "r");
    if (checkfile_stream == NULL) {
      error (0, errno, "%s", checkfile_name);
      return 1;
    }
  }

  SET_MODE (fileno (checkfile_stream), O_TEXT);
  line_number = 0;
  line = NULL;
  line_chars_allocated = 0;
  do {
    char *filename;
    int binary;
    unsigned char *hex_digest;
    int err;
    int line_length;

    ++line_number;

    /* HobbyOS port: getline -> md5_getline (the embedded transcription
       of lib/getline.c + lib/getstr.c; identical semantics).  */
    line_length = md5_getline (&line, &line_chars_allocated, checkfile_stream);
    if (line_length <= 0)
      break;

    /* Ignore comment lines, which begin with a '#' character.  */
    if (line[0] == '#')
      continue;

    /* Remove any trailing newline.  */
    if (line[line_length - 1] == '\n')
      line[--line_length] = '\0';

    err = split_3 (line, line_length, &hex_digest, &binary, &filename);
    if (err || !hex_digits (hex_digest)) {
      if (warn) {
        error (0, 0,
               _("%s: %lu: improperly formatted %s checksum line"),
               checkfile_name, (unsigned long) line_number,
               DIGEST_TYPE_STRING (algorithm));
      }
    } else {
      static const char bin2hex[] = { '0', '1', '2', '3',
                                      '4', '5', '6', '7',
                                      '8', '9', 'a', 'b',
                                      'c', 'd', 'e', 'f' };
      int fail;

      ++n_properly_formated_lines;

      fail = digest_file (filename, binary, bin_buffer, digest_stream);

      if (fail) {
        ++n_open_or_read_failures;
        if (!status_only) {
          printf (_("%s: FAILED open or read\n"), filename);
          fflush (stdout);
        }
      } else {
        size_t digest_bin_bytes = digest_hex_bytes / 2;
        size_t cnt;
        /* Compare generated binary number with text representation
           in check file.  Ignore case of hex digits.  */
        for (cnt = 0; cnt < digest_bin_bytes; ++cnt) {
          if (TOLOWER (hex_digest[2 * cnt])
              != bin2hex[bin_buffer[cnt] >> 4]
              || (TOLOWER (hex_digest[2 * cnt + 1])
                  != (bin2hex[bin_buffer[cnt] & 0xf])))
            break;
        }
        if (cnt != digest_bin_bytes)
          ++n_mismatched_checksums;

        if (!status_only) {
          printf ("%s: %s\n", filename,
                  (cnt != digest_bin_bytes ? _("FAILED") : _("OK")));
          fflush (stdout);
        }
      }
    }
  } while (!feof (checkfile_stream) && !ferror (checkfile_stream));

  if (line)
    free (line);

  if (ferror (checkfile_stream)) {
    error (0, 0, _("%s: read error"), checkfile_name);
    return 1;
  }

  if (checkfile_stream != stdin && fclose (checkfile_stream) == EOF) {
    error (0, errno, "%s", checkfile_name);
    return 1;
  }

  if (n_properly_formated_lines == 0) {
    /* Warn if no tests are found.  */
    error (0, 0, _("%s: no properly formatted %s checksum lines found"),
           checkfile_name, DIGEST_TYPE_STRING (algorithm));
  } else {
    if (!status_only) {
      int n_computed_checkums = (n_properly_formated_lines
                                 - n_open_or_read_failures);

      if (n_open_or_read_failures > 0) {
        error (0, 0,
               _("WARNING: %d of %d listed %s could not be read"),
               n_open_or_read_failures, n_properly_formated_lines,
               (n_properly_formated_lines == 1
                ? _("file") : _("files")));
      }

      if (n_mismatched_checksums > 0) {
        error (0, 0,
               _("WARNING: %d of %d computed %s did NOT match"),
               n_mismatched_checksums, n_computed_checkums,
               (n_computed_checkums == 1
                ? _("checksum") : _("checksums")));
      }
    }
  }

  return ((n_properly_formated_lines > 0 && n_mismatched_checksums == 0
           && n_open_or_read_failures == 0) ? 0 : 1);
}

int main (int argc, char **argv) {
  unsigned char bin_buffer[MAX_DIGEST_BIN_BYTES];
  int do_check = 0;
  int opt;
  char **string = NULL;
  size_t n_strings = 0;
  size_t err = 0;
  int file_type_specified = 0;

#if O_BINARY
  /* Binary is default on MSDOS, so the actual file contents
     are used in computation.  */
  int binary = 1;
#else
  /* Text is default of the Plumb/Lankester format.  */
  int binary = 0;
#endif

  /* Setting values of global variables.  (HobbyOS port: setlocale,
     bindtextdomain and textdomain are dropped -- a single C locale --
     and close_stdout is covered by exit()'s weak fflush(0).) */
  program_name = argv[0];

  while ((opt = getopt_long (argc, argv, "bctw", long_options, NULL)) != -1)
    switch (opt) {
    case 0: /* long option */
      break;
    case 1: /* --string */ {
        if (string == NULL)
          string = (char **) xmalloc ((argc - 1) * sizeof (char *));

        if (optarg == NULL)
          optarg = "";
        string[n_strings++] = optarg;
      }
      break;
    case 'b':
      file_type_specified = 1;
      binary = 1;
      break;
    case 'c':
      do_check = 1;
      break;
    case 2:
      status_only = 1;
      warn = 0;
      break;
    case 't':
      file_type_specified = 1;
      binary = 0;
      break;
    case 'w':
      status_only = 0;
      warn = 1;
      break;
      case_GETOPT_HELP_CHAR;
      case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);
    default:
      usage (EXIT_FAILURE);
    }

  min_digest_line_length = MIN_DIGEST_LINE_LENGTH (algorithm);
  digest_hex_bytes = DIGEST_HEX_BYTES (algorithm);

  if (file_type_specified && do_check) {
    error (0, 0, _("the --binary and --text options are meaningless when \
verifying checksums"));
    usage (EXIT_FAILURE);
  }

  if (n_strings > 0 && do_check) {
    error (0, 0,
           _("the --string and --check options are mutually exclusive"));
    usage (EXIT_FAILURE);
  }

  if (status_only && !do_check) {
    error (0, 0,
           _("the --status option is meaningful only when verifying checksums"));
    usage (EXIT_FAILURE);
  }

  if (warn && !do_check) {
    error (0, 0,
           _("the --warn option is meaningful only when verifying checksums"));
    usage (EXIT_FAILURE);
  }

  if (n_strings > 0) {
    size_t i;

    if (optind < argc) {
      error (0, 0, _("no files may be specified when using --string"));
      usage (EXIT_FAILURE);
    }
    for (i = 0; i < n_strings; ++i) {
      size_t cnt;
      /* HobbyOS port: md5sum only -- 2.1's `else sha_buffer (...)' arm
         (compiled when built as shasum) is not carried over.  */
      md5_buffer (string[i], strlen (string[i]), bin_buffer);

      for (cnt = 0; cnt < (digest_hex_bytes / 2); ++cnt)
        printf ("%02x", bin_buffer[cnt]);

      printf ("  \"%s\"\n", string[i]);
    }
  } else if (do_check) {
    if (optind + 1 < argc) {
      error (0, 0,
             _("only one argument may be specified when using --check"));
      usage (EXIT_FAILURE);
    }

    err = digest_check ((optind == argc) ? "-" : argv[optind],
                        DIGEST_STREAM (algorithm));
  } else {
    if (optind == argc)
      argv[argc++] = "-";

    for (; optind < argc; ++optind) {
      int fail;
      char *file = argv[optind];

      fail = digest_file (file, binary, bin_buffer,
                          DIGEST_STREAM (algorithm));
      err |= fail;
      if (!fail) {
        size_t i;

        /* Output a leading backslash if the file name contains
           a newline or backslash.  */
        if (strchr (file, '\n') || strchr (file, '\\'))
          putchar ('\\');

        for (i = 0; i < (digest_hex_bytes / 2); ++i)
          printf ("%02x", bin_buffer[i]);

        putchar (' ');
        if (binary)
          putchar ('*');
        else
          putchar (' ');

        /* Translate each NEWLINE byte to the string, "\n",
           and each backslash to "\\".  */
        for (i = 0; i < strlen (file); ++i) {
          switch (file[i]) {
          case '\n':
            fputs ("\\n", stdout);
            break;

          case '\\':
            fputs ("\\\\", stdout);
            break;

          default:
            putchar (file[i]);
            break;
          }
        }
        putchar ('\n');
      }
    }
  }

  if (have_read_stdin && fclose (stdin) == EOF)
    error (EXIT_FAILURE, errno, _("standard input"));

  exit (err == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* ===================================================================
   BEGIN EMBEDDED getstr/getline (third_party/textutils-2.1 lib/getstr.c
   and lib/getline.c)
   md5sum.c reads its check file with getline(); gnulib supplies it from
   lib/getline.c, which on systems without getdelim delegates to
   lib/getstr.c.  Both are transcribed here (md5_getline below is
   getline's definition from lib/getline.c).  HobbyOS port: no
   <getline.h>/<getstr.h> headers in the sysroot -- these are file-local
   statics -- and getline carries the md5_ prefix to stay clear of the
   sysroot <stdio.h> declaration.
   =================================================================== */

/* getstr -- read a delimited string from a stream.
   Transcribed from textutils-2.1 lib/getstr.c (written by Jan
   Brittenson).  */

/* Always add at least this many bytes when extending the buffer.  */
#define MIN_CHUNK 64

/* Read up to (and including) a delimiter DELIM1 from STREAM into *LINEPTR
   + OFFSET (and NUL-terminate it).  If DELIM2 is non-zero, then read up
   and including the first occurrence of DELIM1 or DELIM2.  *LINEPTR is
   a pointer returned from malloc (or NULL), pointing to *N characters of
   space.  It is realloc'd as necessary.  Return the number of characters
   read (not including the NUL terminator), or -1 on error or EOF.  */

static int getstr (char **lineptr, size_t *n, FILE *stream, int delim1, int delim2, size_t offset) {
  size_t nchars_avail; /* Allocated but unused chars in *LINEPTR.  */
  char *read_pos;      /* Where we're reading into *LINEPTR. */
  int ret;

  if (!lineptr || !n || !stream)
    return -1;

  if (!*lineptr) {
    *n = MIN_CHUNK;
    *lineptr = malloc (*n);
    if (!*lineptr)
      return -1;
  }

  if (*n < offset)
    return -1;

  nchars_avail = *n - offset;
  read_pos = *lineptr + offset;

  for (;;) {
    register int c = getc (stream);

    /* We always want at least one char left in the buffer, since we
       always (unless we get an error while reading the first char)
       NUL-terminate the line buffer.  */

    if (nchars_avail < 2) {
      if (*n > MIN_CHUNK)
        *n *= 2;
      else
        *n += MIN_CHUNK;

      nchars_avail = *n + *lineptr - read_pos;
      *lineptr = realloc (*lineptr, *n);
      if (!*lineptr)
        return -1;
      read_pos = *n - nchars_avail + *lineptr;
    }

    if (c == EOF || ferror (stream)) {
      /* Return partial line, if any.  */
      if (read_pos == *lineptr)
        return -1;
      else
        break;
    }

    *read_pos++ = c;
    nchars_avail--;

    if (c == delim1 || (delim2 && c == delim2))
      /* Return the line.  */
      break;
  }

  /* Done - NUL terminate and return the number of chars read.  */
  *read_pos = '\0';

  ret = read_pos - (*lineptr + offset);
  return ret;
}

/* getline -- read a line (delimited by NEWLINE) from STREAM, with the
   result in *LINEPTR / *N.  Transcribed from textutils-2.1
   lib/getline.c (written by Jan Brittenson), which defines it in terms
   of getstr on systems without getdelim.  */

static int md5_getline (char **lineptr, size_t *n, FILE *stream) {
  return getstr (lineptr, n, stream, '\n', 0, 0);
}

/* ===================================================================
   END EMBEDDED getstr/getline
   =================================================================== */

/* ===================================================================
   BEGIN EMBEDDED md5.c (third_party/textutils-2.1/lib/md5.c)
   The original program links gnulib's lib/md5.c; the in-OS build is a
   single translation unit, so the engine's text is embedded here.
   HobbyOS port: the `#ifdef HAVE_CONFIG_H' include is dropped (no
   config.h), <stdlib.h>/<string.h> come from the file's own includes,
   `#include "md5.h"' is the embedded copy above, `#include
   "unlocked-io.h"' is dropped (the sysroot's plain stdio is already the
   unlocked variant, and md5_stream's fread/ferror calls are unchanged),
   and the K&R function definitions are modernized to ANSI prototypes.
   Every constant, comment and body statement is as in 2.1.
   =================================================================== */

/* md5.c - Functions to compute MD5 message digest of files or memory blocks
   according to the definition of MD5 in RFC 1321 from April 1992.
   Copyright (C) 1995, 1996, 2001 Free Software Foundation, Inc.
   NOTE: The canonical source of this file is maintained with the GNU C
   Library.  Bugs can be reported to bug-glibc@prep.ai.mit.edu.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 2, or (at your option) any
   later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

/* Written by Ulrich Drepper <drepper@gnu.ai.mit.edu>, 1995.  */

#ifdef WORDS_BIGENDIAN
# define SWAP(n) \
    (((n) << 24) | (((n) & 0xff00) << 8) | (((n) >> 8) & 0xff00) | ((n) >> 24))
#else
# define SWAP(n) (n)
#endif

/* This array contains the bytes used to pad the buffer to the next
   64-byte boundary.  (RFC 1321, 3.1: Step 1)  */
static const unsigned char fillbuf[64] = { 0x80, 0 /* , 0, 0, ...  */ };

/* Initialize structure containing state of computation.
   (RFC 1321, 3.3: Step 3)  */
void md5_init_ctx (struct md5_ctx *ctx) {
  ctx->A = 0x67452301;
  ctx->B = 0xefcdab89;
  ctx->C = 0x98badcfe;
  ctx->D = 0x10325476;

  ctx->total[0] = ctx->total[1] = 0;
  ctx->buflen = 0;
}

/* Put result from CTX in first 16 bytes following RESBUF.  The result
   must be in little endian byte order.

   IMPORTANT: On some systems it is required that RESBUF is correctly
   aligned for a 32 bits value.  */
void *md5_read_ctx (const struct md5_ctx *ctx, void *resbuf) {
  ((md5_uint32 *) resbuf)[0] = SWAP (ctx->A);
  ((md5_uint32 *) resbuf)[1] = SWAP (ctx->B);
  ((md5_uint32 *) resbuf)[2] = SWAP (ctx->C);
  ((md5_uint32 *) resbuf)[3] = SWAP (ctx->D);

  return resbuf;
}

/* Process the remaining bytes in the internal buffer and the usual
   prolog according to the standard and write the result to RESBUF.

   IMPORTANT: On some systems it is required that RESBUF is correctly
   aligned for a 32 bits value.  */
void *md5_finish_ctx (struct md5_ctx *ctx, void *resbuf) {
  /* Take yet unprocessed bytes into account.  */
  md5_uint32 bytes = ctx->buflen;
  size_t pad;

  /* Now count remaining bytes.  */
  ctx->total[0] += bytes;
  if (ctx->total[0] < bytes)
    ++ctx->total[1];

  pad = bytes >= 56 ? 64 + 56 - bytes : 56 - bytes;
  memcpy (&ctx->buffer[bytes], fillbuf, pad);

  /* Put the 64-bit file length in *bits* at the end of the buffer.  */
  *(md5_uint32 *) &ctx->buffer[bytes + pad] = SWAP (ctx->total[0] << 3);
  *(md5_uint32 *) &ctx->buffer[bytes + pad + 4] = SWAP ((ctx->total[1] << 3) |
                                                         (ctx->total[0] >> 29));

  /* Process last bytes.  */
  md5_process_block (ctx->buffer, bytes + pad + 8, ctx);

  return md5_read_ctx (ctx, resbuf);
}

/* Compute MD5 message digest for bytes read from STREAM.  The
   resulting message digest number will be written into the 16 bytes
   beginning at RESBLOCK.  */
int md5_stream (FILE *stream, void *resblock) {
  /* Important: BLOCKSIZE must be a multiple of 64.  */
#define BLOCKSIZE 4096
  struct md5_ctx ctx;
  char buffer[BLOCKSIZE + 72];
  size_t sum;

  /* Initialize the computation context.  */
  md5_init_ctx (&ctx);

  /* Iterate over full file contents.  */
  while (1) {
    /* We read the file in blocks of BLOCKSIZE bytes.  One call of the
       computation function processes the whole buffer so that with the
       next round of the loop another block can be read.  */
    size_t n;
    sum = 0;

    /* Read block.  Take care for partial reads.  */
    do {
      n = fread (buffer + sum, 1, BLOCKSIZE - sum, stream);

      sum += n;
    } while (sum < BLOCKSIZE && n != 0);
    if (n == 0 && ferror (stream))
      return 1;

    /* If end of file is reached, end the loop.  */
    if (n == 0)
      break;

    /* Process buffer with BLOCKSIZE bytes.  Note that
       BLOCKSIZE % 64 == 0
     */
    md5_process_block (buffer, BLOCKSIZE, &ctx);
  }

  /* Add the last bytes if necessary.  */
  if (sum > 0)
    md5_process_bytes (buffer, sum, &ctx);

  /* Construct result in desired memory.  */
  md5_finish_ctx (&ctx, resblock);
  return 0;
}

/* Compute MD5 message digest for LEN bytes beginning at BUFFER.  The
   result is always in little endian byte order, so that a byte-wise
   output yields to the wanted ASCII representation of the message
   digest.  */
void *md5_buffer (const char *buffer, size_t len, void *resblock) {
  struct md5_ctx ctx;

  /* Initialize the computation context.  */
  md5_init_ctx (&ctx);

  /* Process whole buffer but last len % 64 bytes.  */
  md5_process_bytes (buffer, len, &ctx);

  /* Put result in desired memory area.  */
  return md5_finish_ctx (&ctx, resblock);
}

void md5_process_bytes (const void *buffer, size_t len, struct md5_ctx *ctx) {
  /* When we already have some bits in our internal buffer concatenate
     both inputs first.  */
  if (ctx->buflen != 0) {
    size_t left_over = ctx->buflen;
    size_t add = 128 - left_over > len ? len : 128 - left_over;

    memcpy (&ctx->buffer[left_over], buffer, add);
    ctx->buflen += add;

    if (left_over + add > 64) {
      md5_process_block (ctx->buffer, (left_over + add) & ~63, ctx);
      /* The regions in the following copy operation cannot overlap.  */
      memcpy (ctx->buffer, &ctx->buffer[(left_over + add) & ~63],
              (left_over + add) & 63);
      ctx->buflen = (left_over + add) & 63;
    }

    buffer = (const char *) buffer + add;
    len -= add;
  }

  /* Process available complete blocks.  */
  if (len > 64) {
    md5_process_block (buffer, len & ~63, ctx);
    buffer = (const char *) buffer + (len & ~63);
    len &= 63;
  }

  /* Move remaining bytes in internal buffer.  */
  if (len > 0) {
    memcpy (ctx->buffer, buffer, len);
    ctx->buflen = len;
  }
}

/* These are the four functions used in the four steps of the MD5 algorithm
   and defined in the RFC 1321.  The first function is a little bit optimized
   (as found in Colin Plumbs public domain implementation).  */
/* #define FF(b, c, d) ((b & c) | (~b & d)) */
#define FF(b, c, d) (d ^ (b & (c ^ d)))
#define FG(b, c, d) FF (d, b, c)
#define FH(b, c, d) (b ^ c ^ d)
#define FI(b, c, d) (c ^ (b | ~d))

/* Process LEN bytes of BUFFER, accumulating context into CTX.
   It is assumed that LEN % 64 == 0.  */

void md5_process_block (const void *buffer, size_t len, struct md5_ctx *ctx) {
  md5_uint32 correct_words[16];
  const md5_uint32 *words = buffer;
  size_t nwords = len / sizeof (md5_uint32);
  const md5_uint32 *endp = words + nwords;
  md5_uint32 A = ctx->A;
  md5_uint32 B = ctx->B;
  md5_uint32 C = ctx->C;
  md5_uint32 D = ctx->D;

  /* First increment the byte count.  RFC 1321 specifies the possible
     length of the file up to 2^64 bits.  Here we only compute the
     number of bytes.  Do a double word increment.  */
  ctx->total[0] += len;
  if (ctx->total[0] < len)
    ++ctx->total[1];

  /* Process all bytes in the buffer with 64 bytes in each round of
     the loop.  */
  while (words < endp) {
    md5_uint32 *cwp = correct_words;
    md5_uint32 A_save = A;
    md5_uint32 B_save = B;
    md5_uint32 C_save = C;
    md5_uint32 D_save = D;

    /* First round: using the given function, the context and a constant
       the next context is computed.  Because the algorithms processing
       unit is a 32-bit word and it is determined to work on words in
       little endian byte order we perhaps have to change the byte order
       before the computation.  To reduce the work for the next steps
       we store the swapped words in the array CORRECT_WORDS.  */

#define OP(a, b, c, d, s, T) \
      do \
        { \
          a += FF (b, c, d) + (*cwp++ = SWAP (*words)) + T; \
          ++words; \
          a = rol (a, s); \
          a += b; \
        } \
      while (0)

    /* Before we start, one word to the strange constants.
       They are defined in RFC 1321 as

       T[i] = (int) (4294967296.0 * fabs (sin (i))), i=1..64, or
       perl -e 'foreach(1..64){printf "0x%08x\n", int (4294967296 * abs (sin $_))}'
     */

    /* Round 1.  */
    OP (A, B, C, D,  7, 0xd76aa478);
    OP (D, A, B, C, 12, 0xe8c7b756);
    OP (C, D, A, B, 17, 0x242070db);
    OP (B, C, D, A, 22, 0xc1bdceee);
    OP (A, B, C, D,  7, 0xf57c0faf);
    OP (D, A, B, C, 12, 0x4787c62a);
    OP (C, D, A, B, 17, 0xa8304613);
    OP (B, C, D, A, 22, 0xfd469501);
    OP (A, B, C, D,  7, 0x698098d8);
    OP (D, A, B, C, 12, 0x8b44f7af);
    OP (C, D, A, B, 17, 0xffff5bb1);
    OP (B, C, D, A, 22, 0x895cd7be);
    OP (A, B, C, D,  7, 0x6b901122);
    OP (D, A, B, C, 12, 0xfd987193);
    OP (C, D, A, B, 17, 0xa679438e);
    OP (B, C, D, A, 22, 0x49b40821);

    /* For the second to fourth round we have the possibly swapped words
       in CORRECT_WORDS.  Redefine the macro to take an additional first
       argument specifying the function to use.  */
#undef OP
#define OP(f, a, b, c, d, k, s, T) \
      do \
        { \
          a += f (b, c, d) + correct_words[k] + T; \
          a = rol (a, s); \
          a += b; \
        } \
      while (0)

    /* Round 2.  */
    OP (FG, A, B, C, D,  1,  5, 0xf61e2562);
    OP (FG, D, A, B, C,  6,  9, 0xc040b340);
    OP (FG, C, D, A, B, 11, 14, 0x265e5a51);
    OP (FG, B, C, D, A,  0, 20, 0xe9b6c7aa);
    OP (FG, A, B, C, D,  5,  5, 0xd62f105d);
    OP (FG, D, A, B, C, 10,  9, 0x02441453);
    OP (FG, C, D, A, B, 15, 14, 0xd8a1e681);
    OP (FG, B, C, D, A,  4, 20, 0xe7d3fbc8);
    OP (FG, A, B, C, D,  9,  5, 0x21e1cde6);
    OP (FG, D, A, B, C, 14,  9, 0xc33707d6);
    OP (FG, C, D, A, B,  3, 14, 0xf4d50d87);
    OP (FG, B, C, D, A,  8, 20, 0x455a14ed);
    OP (FG, A, B, C, D, 13,  5, 0xa9e3e905);
    OP (FG, D, A, B, C,  2,  9, 0xfcefa3f8);
    OP (FG, C, D, A, B,  7, 14, 0x676f02d9);
    OP (FG, B, C, D, A, 12, 20, 0x8d2a4c8a);

    /* Round 3.  */
    OP (FH, A, B, C, D,  5,  4, 0xfffa3942);
    OP (FH, D, A, B, C,  8, 11, 0x8771f681);
    OP (FH, C, D, A, B, 11, 16, 0x6d9d6122);
    OP (FH, B, C, D, A, 14, 23, 0xfde5380c);
    OP (FH, A, B, C, D,  1,  4, 0xa4beea44);
    OP (FH, D, A, B, C,  4, 11, 0x4bdecfa9);
    OP (FH, C, D, A, B,  7, 16, 0xf6bb4b60);
    OP (FH, B, C, D, A, 10, 23, 0xbebfbc70);
    OP (FH, A, B, C, D, 13,  4, 0x289b7ec6);
    OP (FH, D, A, B, C,  0, 11, 0xeaa127fa);
    OP (FH, C, D, A, B,  3, 16, 0xd4ef3085);
    OP (FH, B, C, D, A,  6, 23, 0x04881d05);
    OP (FH, A, B, C, D,  9,  4, 0xd9d4d039);
    OP (FH, D, A, B, C, 12, 11, 0xe6db99e5);
    OP (FH, C, D, A, B, 15, 16, 0x1fa27cf8);
    OP (FH, B, C, D, A,  2, 23, 0xc4ac5665);

    /* Round 4.  */
    OP (FI, A, B, C, D,  0,  6, 0xf4292244);
    OP (FI, D, A, B, C,  7, 10, 0x432aff97);
    OP (FI, C, D, A, B, 14, 15, 0xab9423a7);
    OP (FI, B, C, D, A,  5, 21, 0xfc93a039);
    OP (FI, A, B, C, D, 12,  6, 0x655b59c3);
    OP (FI, D, A, B, C,  3, 10, 0x8f0ccc92);
    OP (FI, C, D, A, B, 10, 15, 0xffeff47d);
    OP (FI, B, C, D, A,  1, 21, 0x85845dd1);
    OP (FI, A, B, C, D,  8,  6, 0x6fa87e4f);
    OP (FI, D, A, B, C, 15, 10, 0xfe2ce6e0);
    OP (FI, C, D, A, B,  6, 15, 0xa3014314);
    OP (FI, B, C, D, A, 13, 21, 0x4e0811a1);
    OP (FI, A, B, C, D,  4,  6, 0xf7537e82);
    OP (FI, D, A, B, C, 11, 10, 0xbd3af235);
    OP (FI, C, D, A, B,  2, 15, 0x2ad7d2bb);
    OP (FI, B, C, D, A,  9, 21, 0xeb86d391);

    /* Add the starting values of the context.  */
    A += A_save;
    B += B_save;
    C += C_save;
    D += D_save;
  }

  /* Put checksum in context given as argument.  */
  ctx->A = A;
  ctx->B = B;
  ctx->C = C;
  ctx->D = D;
}

/* ===================================================================
   END EMBEDDED md5.c
   =================================================================== */

/* ===================================================================
   BEGIN EMBEDDED src/md5.c (third_party/textutils-2.1/src/md5.c)
   The tiny algorithm-selector wrapper: 2.1 compiles md5sum.c twice,
   once with src/md5.c (ALG_MD5) and once with src/sha.c's wrapper
   (ALG_SHA1, the shasum program).  This port links only the md5sum
   flavor.
   =================================================================== */

int algorithm = ALG_MD5;

/* ===================================================================
   END EMBEDDED src/md5.c
   =================================================================== */
