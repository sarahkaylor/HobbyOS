/*
 * wc — print the number of bytes, words, and lines in files.
 *
 * Port of GNU textutils 2.1 src/wc.c (GPLv2), by Paul Rubin and David
 * MacKenzie.  Counting loops, option handling, and the output layout
 * (write_counts) are kept verbatim from the original; only the non-
 * portable parts were trimmed:
 *   - config.h / gettext (_() becomes identity), setlocale, bindtextdomain
 *   - the multibyte/wide-char path (MB_CUR_MAX > 1) — HobbyOS is ASCII,
 *     so chars==bytes and the byte path runs
 *   - SET_BINARY (no-op here), close_stdout/atexit (stdout errors surface
 *     through our stdio's ferror at exit via exit_status)
 *   - human_readable() collapses to a decimal number printer (block
 *     size 1 == no scaling, which is what wc requests)
 *   - error() comes from the Phase-2 sysroot (error.h/error.c)
 *   - fstat/lseek (the "-c only" size fast path) are backed by real
 *     syscalls since Phase 3, so regular files take the size fast path;
 *     the reported byte counts are identical either way.
 *
 * The host build of this file (see Makefile wc_host) links against
 * glibc except for our getopt_long (hb_* renames via getopt.h), and is
 * raced byte-for-byte against /usr/bin/wc in src/host/wc_parity.sh.
 */
#include <stdio.h>
#include <getopt.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <error.h>

#define _(s) (s)

#define PROGRAM_NAME "wc"
#define AUTHORS "Paul Rubin and David MacKenzie"

#define BUFFER_SIZE (16 * 1024)

/* Owned by error.c; set from argv[0] in main(). */

/* Cumulative counts for all files processed so far. */
static uint64_t total_lines;
static uint64_t total_words;
static uint64_t total_chars;
static uint64_t total_bytes;
static uint64_t max_line_length;

/* Which counts to print. */
static int print_lines, print_words, print_chars, print_bytes;
static int print_linelength;

/* Nonzero if we have ever read the standard input. */
static int have_read_stdin;

/* The error code to return to the system. */
static int exit_status;

/* If nonzero, do not line up columns but instead separate numbers by
   a single space (POSIX mode). */
static int posixly_correct;

static struct option const longopts[] =
{
  {"bytes", no_argument, NULL, 'c'},
  {"chars", no_argument, NULL, 'm'},
  {"lines", no_argument, NULL, 'l'},
  {"words", no_argument, NULL, 'w'},
  {"max-line-length", no_argument, NULL, 'L'},
  {"help", no_argument, NULL, 'h'},
  {"version", no_argument, NULL, 'V'},
  {NULL, 0, NULL, 0}
};

static void
usage (int status)
{
  if (status != 0)
    fprintf (stderr, _("Try `%s --help' for more information.\n"),
	     program_name);
  else
    {
      printf (_("\
Usage: %s [OPTION]... [FILE]...\n\
"), program_name);
      fputs (_("\
Print byte, word, and newline counts for each FILE, and a total line if\n\
more than one FILE is specified.  With no FILE, or when FILE is -,\n\
read standard input.\n\
  -c, --bytes            print the byte counts\n\
  -m, --chars            print the character counts\n\
  -l, --lines            print the newline counts\n\
"), stdout);
      fputs (_("\
  -L, --max-line-length  print the length of the longest line\n\
  -w, --words            print the word counts\n\
"), stdout);
      fputs (_("\
  -h, --help             display this help and exit\n\
  -V, --version          output version information and exit\n\
"), stdout);
    }
  exit (status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* human_readable with block sizes 1,1: just the decimal representation. */
static char *
human_readable (uint64_t n, char *buf, int from, int to)
{
  (void)from;
  (void)to;
  snprintf (buf, 24, "%llu", (unsigned long long)n);
  return buf;
}

#define LONGEST_HUMAN_READABLE 24

static void
write_counts (uint64_t lines,
	      uint64_t words,
	      uint64_t chars,
	      uint64_t bytes,
	      uint64_t linelength,
	      const char *file)
{
  char buf[LONGEST_HUMAN_READABLE + 1];
  char const *space = "";
  char const *format_int = (posixly_correct ? "%s" : "%7s");
  char const *format_sp_int = (posixly_correct ? "%s%s" : "%s%7s");

  if (print_lines)
    {
      printf (format_int, human_readable (lines, buf, 1, 1));
      space = " ";
    }
  if (print_words)
    {
      printf (format_sp_int, space, human_readable (words, buf, 1, 1));
      space = " ";
    }
  if (print_chars)
    {
      printf (format_sp_int, space, human_readable (chars, buf, 1, 1));
      space = " ";
    }
  if (print_bytes)
    {
      printf (format_sp_int, space, human_readable (bytes, buf, 1, 1));
      space = " ";
    }
  if (print_linelength)
    {
      printf (format_sp_int, space,
	      human_readable (linelength, buf, 1, 1));
    }
  if (*file)
    printf (" %s", file);
  putchar ('\n');
}

/* Equivalent of gnulib safe_read: loop until the whole request is
   satisfied or an error/EOF, retrying on EINTR. */
static ssize_t
safe_read (int fd, void *buf, size_t count)
{
  ssize_t total = 0;

  while (total < (ssize_t)count)
    {
      ssize_t n = read (fd, (char *)buf + total, count - (size_t)total);
      if (n < 0)
	{
	  if (errno == EINTR)
	    continue;
	  return -1;
	}
      if (n == 0)
	break;
      total += n;
    }
  return total;
}

static void
wc (int fd, const char *file)
{
  char buf[BUFFER_SIZE + 1];
  ssize_t bytes_read;
  uint64_t lines, words, chars, bytes, linelength;
  int count_bytes, count_chars, count_complicated;

  lines = words = chars = bytes = linelength = 0;

  /* In an ASCII system chars are equivalent to bytes. */
  count_bytes = print_bytes + print_chars;
  count_chars = 0;
  count_complicated = print_words + print_linelength;

  /* When counting only bytes, use the size fast path when available
     (fstat/lseek).  Phase 3 implemented these: regular files resolve
     their size directly, pipes degrade to the read loop below. */
  if (count_bytes && !count_chars && !print_lines && !count_complicated)
    {
      off_t current_pos, end_pos;
      struct stat stats;

      if (fstat (fd, &stats) == 0 && S_ISREG (stats.st_mode)
	  && (current_pos = lseek (fd, (off_t) 0, SEEK_CUR)) != -1
	  && (end_pos = lseek (fd, (off_t) 0, SEEK_END)) != -1)
	{
	  off_t diff;
	  bytes = (diff = end_pos - current_pos) < 0 ? 0 : (off_t)diff;
	}
      else
	{
	  while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
	    {
	      bytes += (uint64_t)bytes_read;
	    }
	  if (bytes_read < 0)
	    {
	      error (0, errno, "%s", file);
	      exit_status = 1;
	    }
	}
    }
  else if (!count_chars && !count_complicated)
    {
      /* Separate loop when counting only lines or lines and bytes. */
      while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
	{
	  register char *p = buf;

	  while ((p = memchr (p, '\n', (buf + bytes_read) - p)))
	    {
	      ++p;
	      ++lines;
	    }
	  bytes += (uint64_t)bytes_read;
	}
      if (bytes_read < 0)
	{
	  error (0, errno, "%s", file);
	  exit_status = 1;
	}
    }
  else
    {
      int in_word = 0;
      uint64_t linepos = 0;

      while ((bytes_read = safe_read (fd, buf, BUFFER_SIZE)) > 0)
	{
	  const char *p = buf;

	  bytes += (uint64_t)bytes_read;
	  do
	    {
	      switch (*p++)
		{
		case '\n':
		  lines++;
		  /* Fall through. */
		case '\r':
		case '\f':
		  if (linepos > linelength)
		    linelength = linepos;
		  linepos = 0;
		  goto word_separator;
		case '\t':
		  linepos += 8 - (linepos % 8);
		  goto word_separator;
		case ' ':
		  linepos++;
		  /* Fall through. */
		case '\v':
		word_separator:
		  if (in_word)
		    {
		      in_word = 0;
		      words++;
		    }
		  break;
		default:
		  if (isprint ((unsigned char) p[-1]))
		    {
		      linepos++;
		      if (isspace ((unsigned char) p[-1]))
			goto word_separator;
		      in_word = 1;
		    }
		  break;
		}
	    }
	  while (--bytes_read);
	}
      if (bytes_read < 0)
	{
	  error (0, errno, "%s", file);
	  exit_status = 1;
	}
      if (linepos > linelength)
	linelength = linepos;
      if (in_word)
	words++;
    }

  if (count_chars < print_chars)
    chars = bytes;

  write_counts (lines, words, chars, bytes, linelength, file);
  total_lines += lines;
  total_words += words;
  total_chars += chars;
  total_bytes += bytes;
  if (linelength > max_line_length)
    max_line_length = linelength;
}

static void
wc_file (const char *file)
{
  if (strcmp (file, "-") == 0)
    {
      have_read_stdin = 1;
      wc (0, file);
    }
  else
    {
      int fd = open (file, O_RDONLY);
      if (fd == -1)
	{
	  error (0, errno, "%s", file);
	  exit_status = 1;
	  return;
	}
      wc (fd, file);
      if (close (fd))
	{
	  error (0, errno, "%s", file);
	  exit_status = 1;
	}
    }
}

int
main (int argc, char **argv)
{
  int optc;
  int nfiles;

  program_name = argv[0];
  if (program_name == NULL)
    program_name = "wc";

  exit_status = 0;
  posixly_correct = (getenv ("POSIXLY_CORRECT") != NULL);
  print_lines = print_words = print_chars = print_bytes = print_linelength = 0;
  total_lines = total_words = total_chars = total_bytes = max_line_length = 0;

  while ((optc = getopt_long (argc, argv, "clLmw", longopts, NULL)) != -1)
    switch (optc)
      {
      case 0:
	break;

      case 'c':
	print_bytes = 1;
	break;

      case 'm':
	print_chars = 1;
	break;

      case 'l':
	print_lines = 1;
	break;

      case 'w':
	print_words = 1;
	break;

      case 'L':
	print_linelength = 1;
	break;

      case 'h':
	usage (0);

      case 'V':
	printf ("%s (HobbyOS textutils) 2.1\n", PROGRAM_NAME);
	exit (EXIT_SUCCESS);

      default:
	usage (1);
      }

  if (print_lines + print_words + print_chars + print_bytes + print_linelength
      == 0)
    print_lines = print_words = print_bytes = 1;

  nfiles = argc - optind;

  if (nfiles == 0)
    {
      have_read_stdin = 1;
      wc (0, "");
    }
  else
    {
      for (; optind < argc; ++optind)
	wc_file (argv[optind]);

      if (nfiles > 1)
	write_counts (total_lines, total_words, total_chars, total_bytes,
		      max_line_length, _("total"));
    }

  if (have_read_stdin && close (0))
    error (EXIT_FAILURE, errno, "-");

  exit (exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
