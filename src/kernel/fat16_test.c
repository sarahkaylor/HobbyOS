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

  /* A move must not clobber an existing entry. */
  EXPECT_EQ(fat16_open("/CLOB1.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_open("/MOVEDIR/CLOB2.TXT", &f), 0);
  fat16_close(&f);
  EXPECT_EQ(fat16_rename("/CLOB1.TXT", "/MOVEDIR/CLOB2.TXT"), -1);
  EXPECT_EQ(fat16_resolve_path("/CLOB1.TXT", &e, 0, 0), 0);
  EXPECT_EQ(fat16_resolve_path("/MOVEDIR/CLOB2.TXT", &e, 0, 0), 0);

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

void fat16_test_suite(void) {
  uart_puts("fat16_test_suite:\n");
  test_fat16_open_existing();
  test_fat16_open_nonexistent();
  test_fat16_read_file();
  test_fat16_move_across_dirs();
}

#endif // KERNEL_MODE_UNIT_TEST
