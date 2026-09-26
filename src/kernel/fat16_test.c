#ifdef KERNEL_MODE_UNIT_TEST

#include "unit_test.h"
#include "fat16.h"
#include "fs.h"

static void test_fat16_open_existing(void) {
  uart_puts("  Running test_fat16_open_existing...\n");
  tests_run++;

  struct file f;
  int res = fat16_open("TEST.TXT", &f);
  EXPECT_EQ(res, 0);
  EXPECT_EQ(f.fat16.cursor, 0);

  fat16_close(&f);
}

static void test_fat16_open_nonexistent(void) {
  uart_puts("  Running test_fat16_open_nonexistent...\n");
  tests_run++;

  /* fat16_open() has create-on-open semantics: opening a file that does not
   * exist creates it and returns 0 (the editor's Save flow relies on this).
   * So a missing file in an existing directory is NOT an error. It fails
   * only when the file cannot be created — e.g. its parent directory does
   * not exist. */
  struct file f;
  int res = fat16_open("/NOSUCHDIR/MISSING.TXT", &f);
  EXPECT_EQ(res, -1);
}

static void test_fat16_read_file(void) {
  uart_puts("  Running test_fat16_read_file...\n");
  tests_run++;

  struct file f;
  int res = fat16_open("TEST.TXT", &f);
  EXPECT_EQ(res, 0);

  char buf[128] = {0};
  int bytes_read = fat16_read(&f, buf, sizeof(buf));
  // Since TEST.TXT is empty or just created by touch, it might be 0 bytes.
  // Wait, the Makefile just does `touch TEST.TXT` so it is 0 bytes.
  EXPECT_EQ(bytes_read, 0);

  fat16_close(&f);
}

static void test_fat16_move_across_dirs(void) {
  uart_puts("  Running test_fat16_move_across_dirs...\n");
  tests_run++;

  struct fat16_dir_entry e;
  struct file f;

  /* Set up: two directories and a file in the root. */
  (void)fat16_mkdir("/MOVEDIR");
  (void)fat16_mkdir("/MOVEDIR2");
  EXPECT_EQ(fat16_open("/MOVE.TXT", &f), 0);
  fat16_close(&f);

  /* Move a file into a subdirectory. */
  EXPECT_EQ(fat16_rename("/MOVE.TXT", "/MOVEDIR/MOVE.TXT"), 0);
  ASSERT(fat16_resolve_path("/MOVE.TXT", &e, 0, 0) != 0);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR/MOVE.TXT", &e, 0, 0), 0);

  /* Rename in place inside that directory. */
  EXPECT_EQ(fat16_rename("/MOVEDIR/MOVE.TXT", "/MOVEDIR/RENAMED.TXT"), 0);
  ASSERT(fat16_resolve_path("/MOVEDIR/MOVE.TXT", &e, 0, 0) != 0);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR/RENAMED.TXT", &e, 0, 0), 0);

  /* POSIX rename semantics: an existing destination is replaced, not a
   * rename failure (sed -i and mv both depend on this). */
  EXPECT_EQ(fat16_open("/CLOB1.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_open("/MOVEDIR/CLOB2.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_rename("/CLOB1.TXT", "/MOVEDIR/CLOB2.TXT"), 0);
  ASSERT(fat16_resolve_path("/CLOB1.TXT", &e, 0, 0) != 0);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR/CLOB2.TXT", &e, 0, 0), 0);

  /* Same-directory replace: the exact shape sed -i performs (temp file
   * renamed over the original in the same directory). */
  EXPECT_EQ(fat16_open("/SAME1.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_open("/SAME2.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_rename("/SAME1.TXT", "/SAME2.TXT"), 0);
  ASSERT(fat16_resolve_path("/SAME1.TXT", &e, 0, 0) != 0);
  EXPECT_EQ(fat16_resolve_path("/SAME2.TXT", &e, 0, 0), 0);

  /* fat16_truncate empties a file: old content gone, new writes land at 0. */
  {
    struct file wf;
    EXPECT_EQ(fat16_open("/TRUNC.TXT", &wf), 0);
    const char *seed = "PLENTY OF OLD CONTENT";
    EXPECT_EQ(fat16_write(&wf, seed, 21), 21);
    fat16_close(&wf);

    EXPECT_EQ(fat16_open("/TRUNC.TXT", &wf), 0);
    EXPECT_EQ(fat16_truncate(&wf), 0);
    const char *fresh = "new";
    EXPECT_EQ(fat16_write(&wf, fresh, 3), 3);
    fat16_close(&wf);

    EXPECT_EQ(fat16_open("/TRUNC.TXT", &wf), 0);
    ASSERT(fat16_seek(&wf, 0) == 0);
    char rbuf[8] = {0};
    int rn = fat16_read(&wf, rbuf, 8);
    fat16_close(&wf);
    EXPECT_EQ(rn, 3);
    if (rn == 3) {
      EXPECT_EQ(rbuf[0], 'n');
      EXPECT_EQ(rbuf[1], 'e');
      EXPECT_EQ(rbuf[2], 'w');
    }
  }

  /* Move a directory, then keep using it. */
  EXPECT_EQ(fat16_mkdir("/MOVESUB"), 0);
  EXPECT_EQ(fat16_rename("/MOVESUB", "/MOVEDIR2/MOVESUB"), 0);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR2/MOVESUB", &e, 0, 0), 0);
  EXPECT_EQ(fat16_open("/MOVEDIR2/MOVESUB/INNER.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR2/MOVESUB/INNER.TXT", &e, 0, 0), 0);

  /* The moved directory's ".." must now point at its new parent: moving
   * its parent into it has to be refused as a cycle. */
  EXPECT_EQ(fat16_rename("/MOVEDIR2", "/MOVEDIR2/MOVESUB/MOVEDIR2"), -1);

  /* Missing sources fail. */
  EXPECT_EQ(fat16_rename("/NO-SUCH-FILE.TXT", "/X.TXT"), -1);
  EXPECT_EQ(fat16_rename("/CLOB1.TXT", "/NO-SUCH-DIR/X.TXT"), -1);
}

static void test_fat16_lfn_names(void) {
  uart_puts("  Running test_fat16_lfn_names...\n");
  tests_run++;

  struct fat16_dir_entry e;

  /* 1. A mtools-created long name (0x0F records on disk) must open by its
     long name, not by truncation onto some other entry.  This is exactly
     the bug that made the UNEXPAND_T.BIN wave test load UNEXPAND.BIN. */
  {
    struct file f;
    EXPECT_EQ(fat16_open("/UNEXPAND_T.BIN", &f), 0);
    EXPECT_EQ(f.fat16.entry.file_size, 12592); /* the on-device test binary */
    fat16_close(&f);
  }

  /* 2. A created long name round-trips: write, reopen by long name, read
     the content back, and resolve it through the path walker. */
  {
    struct file f;
    const char *seed = "long file body\n";
    EXPECT_EQ(fat16_open("/LONG_FILENAME_TEST.TXT", &f), 0);
    EXPECT_EQ(fat16_write(&f, seed, 15), 15);
    fat16_close(&f);

    EXPECT_EQ(fat16_open("/LONG_FILENAME_TEST.TXT", &f), 0);
    ASSERT(fat16_seek(&f, 0) == 0);
    {
      char rbuf[32] = {0};
      int rn = fat16_read(&f, rbuf, sizeof(rbuf) - 1);
      fat16_close(&f);
      EXPECT_EQ(rn, 15);
      if (rn == 15) {
        EXPECT_EQ(rbuf[0], 'l');
        EXPECT_EQ(rbuf[13], 'y');
        EXPECT_EQ(rbuf[14], '\n');
      }
    }
    EXPECT_EQ(fat16_resolve_path("/LONG_FILENAME_TEST.TXT", &e, 0, 0), 0);
    EXPECT_EQ(e.file_size, 15);
  }

  /* 3. A query that looks like a truncated version of an EXISTING 8.3
     name must not resolve to that file: 'ABCDEFGH_LONG.TXT' must not open
     'ABCDEFGH.TXT'.  A fresh file is created instead (create-on-open), so
     the result carries the new, empty entry rather than the old content. */
  {
    struct file f;
    const char *seed = "0123456789";
    EXPECT_EQ(fat16_open("/ABCDEFGH.TXT", &f), 0);
    EXPECT_EQ(fat16_write(&f, seed, 10), 10);
    fat16_close(&f);

    EXPECT_EQ(fat16_open("/ABCDEFGH_LONG.TXT", &f), 0);
    EXPECT_EQ(f.fat16.entry.file_size, 0); /* new entry, not 'ABCDEFGH.TXT' */
    fat16_close(&f);
    EXPECT_EQ(fat16_resolve_path("/ABCDEFGH.TXT", &e, 0, 0), 0);
    EXPECT_EQ(e.file_size, 10);
  }
}

void fat16_test_suite(void) {
  uart_puts("fat16_test_suite:\n");
  test_fat16_open_existing();
  test_fat16_open_nonexistent();
  test_fat16_read_file();
  test_fat16_move_across_dirs();
  test_fat16_lfn_names();
}

#endif // KERNEL_MODE_UNIT_TEST
