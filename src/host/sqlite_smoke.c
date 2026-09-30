/*
 * sqlite_smoke.c - host smoke for the vendored SQLite 3.49.2 amalgamation
 * (L6, browser plan AD-8 / P6.2).
 *
 * Checks:
 *   1. sqlite3_threadsafe() reports the serialized build that P6.2 wants
 *      (SQLITE_THREADSAFE=1).
 *   2. in-memory CREATE / INSERT / SELECT round trip with exact row values.
 *   3. PRAGMA compile_options is dumped verbatim (the compile-time option
 *      record for the coming P1/P6 threads work).
 *
 * Built and run by src/host/build_sqlite_host.sh; exits 0 only when every
 * check passes.  Host-only tooling: no HobbyOS code here.
 */
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

static int failures = 0;

static void exec_or_fail(sqlite3 *db, const char *sql) {
  char *err = NULL;

  if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
    printf("FAIL: exec [%s]: %s\n", sql, err != NULL ? err : "?");
    sqlite3_free(err);
    failures++;
  }
}

static void select_rows(sqlite3 *db) {
  sqlite3_stmt *st = NULL;
  const char *want[3][2] = {{"1", "one"}, {"2", "two"}, {"3", "three"}};
  int row = 0;
  int rc;

  rc = sqlite3_prepare_v2(db, "SELECT a, b FROM t ORDER BY a;", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    printf("FAIL: prepare SELECT: %s\n", sqlite3_errmsg(db));
    failures++;
    return;
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const unsigned char *a = sqlite3_column_text(st, 0);
    const unsigned char *b = sqlite3_column_text(st, 1);

    printf("row %d: a=%s b=%s\n", row + 1, a != NULL ? (const char *)a : "?",
           b != NULL ? (const char *)b : "?");
    if (row < 3 && strcmp((const char *)a, want[row][0]) == 0 &&
        strcmp((const char *)b, want[row][1]) == 0) {
      printf("PASS: row %d matches\n", row + 1);
    } else {
      printf("FAIL: row %d unexpected\n", row + 1);
      failures++;
    }
    row++;
  }
  if (rc != SQLITE_DONE) {
    printf("FAIL: step: %s\n", sqlite3_errmsg(db));
    failures++;
  }
  sqlite3_finalize(st);
  if (row != 3) {
    printf("FAIL: SELECT returned %d rows (want 3)\n", row);
    failures++;
  } else {
    printf("PASS: SELECT returned 3 rows\n");
  }
}

static void dump_compile_options(sqlite3 *db) {
  sqlite3_stmt *st = NULL;
  int rc;
  int n = 0;
  int threadsafe_seen = 0;

  rc = sqlite3_prepare_v2(db, "PRAGMA compile_options;", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    printf("FAIL: prepare PRAGMA compile_options: %s\n", sqlite3_errmsg(db));
    failures++;
    return;
  }
  printf("--- PRAGMA compile_options ---\n");
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const unsigned char *opt = sqlite3_column_text(st, 0);

    printf("compile_option: %s\n", opt);
    if (strncmp((const char *)opt, "THREADSAFE=", 11) == 0) {
      threadsafe_seen = 1;
    }
    n++;
  }
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE || n == 0) {
    printf("FAIL: PRAGMA compile_options produced %d rows\n", n);
    failures++;
    return;
  }
  printf("--- %d compile option(s); THREADSAFE row %s ---\n", n,
         threadsafe_seen ? "present" : "not listed (serialized default)");
}

int main(void) {
  sqlite3 *db = NULL;
  int rc;

  printf("sqlite3_libversion()  = %s\n", sqlite3_libversion());
  printf("sqlite3_sourceid()    = %s\n", sqlite3_sourceid());
  printf("sqlite3_threadsafe()  = %d\n", sqlite3_threadsafe());
  if (sqlite3_threadsafe() != 1) {
    printf("FAIL: expected a serialized build (SQLITE_THREADSAFE=1)\n");
    failures++;
  } else {
    printf("PASS: serialized thread-safe build (P6.2 target)\n");
  }

  rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    printf("FAIL: sqlite3_open: %s\n", sqlite3_errmsg(db));
    return 1;
  }
  exec_or_fail(db, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT);");
  exec_or_fail(db,
               "INSERT INTO t(a, b) VALUES (1, 'one'), (2, 'two'), "
               "(3, 'three');");
  select_rows(db);
  dump_compile_options(db);

  sqlite3_close(db);

  if (failures != 0) {
    printf("SMOKE FAIL: %d check(s) failed\n", failures);
    return 1;
  }
  printf("SMOKE PASS: SQLite in-memory CREATE/INSERT/SELECT OK\n");
  return 0;
}
