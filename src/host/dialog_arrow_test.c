/*
 * Host-side test for dialog arrow key handling.
 *
 * This test verifies that dialog_read_key() correctly parses the
 * 3-byte ESC sequences sent by the desktop for arrow keys:
 *   ESC [ A = UP, ESC [ B = DOWN, ESC [ C = RIGHT, ESC [ D = LEFT
 *
 * It also tests the file open dialog's arrow key navigation by
 * simulating the desktop's key injection and checking the dialog's
 * selection state.
 *
 * Build: linked with dialog.o, filedialog.o, compat.o (host versions)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* Include the dialog and filedialog headers */
#include "../user_include/dialog.h"
#include "../user_include/filedialog.h"
#include "../user_include/libc.h"

/* Test results */
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else { tests_failed++; printf("  FAIL: %s\n", msg); } \
} while(0)

/* ---- Test 1: DKEY constants ---- */
static void test_dkey_constants(void) {
    printf("[TEST] DKEY constants...\n");
    ASSERT(DKEY_UP == 0x100, "DKEY_UP == 0x100");
    ASSERT(DKEY_DOWN == 0x101, "DKEY_DOWN == 0x101");
    ASSERT(DKEY_LEFT == 0x102, "DKEY_LEFT == 0x102");
    ASSERT(DKEY_RIGHT == 0x103, "DKEY_RIGHT == 0x103");
    ASSERT(DKEY_ESC == 0x1B, "DKEY_ESC == 0x1B");
}

/* ---- Test 2: dialog_read_key with arrow key sequences ---- */
/* This test simulates the desktop sending arrow key ESC sequences
 * by writing them to a pipe connected to fd 0.
 *
 * Since dialog_read_key() reads from fd 0 (stdin), we redirect
 * stdin to a pipe and write the ESC sequences to it.
 */

static int test_pipe_fd[2];

static void setup_stdin_pipe(void) {
    if (pipe(test_pipe_fd) < 0) {
        perror("pipe");
        exit(1);
    }
    /* Redirect fd 0 (stdin) to read end of pipe */
    dup2(test_pipe_fd[0], 0);
    close(test_pipe_fd[0]);
}

static void write_to_stdin(const char *data, int len) {
    write(test_pipe_fd[1], data, len);
}

static void test_arrow_keys(void) {
    printf("[TEST] Arrow key ESC sequence parsing...\n");

    setup_stdin_pipe();

    /* Test DOWN arrow: ESC [ B */
    {
        char seq[] = { 27, '[', 'B' };
        write_to_stdin(seq, 3);
        int key = dialog_read_key();
        ASSERT(key == DKEY_DOWN, "DOWN arrow (ESC [ B) -> DKEY_DOWN");
    }

    /* Test UP arrow: ESC [ A */
    {
        char seq[] = { 27, '[', 'A' };
        write_to_stdin(seq, 3);
        int key = dialog_read_key();
        ASSERT(key == DKEY_UP, "UP arrow (ESC [ A) -> DKEY_UP");
    }

    /* Test RIGHT arrow: ESC [ C */
    {
        char seq[] = { 27, '[', 'C' };
        write_to_stdin(seq, 3);
        int key = dialog_read_key();
        ASSERT(key == DKEY_RIGHT, "RIGHT arrow (ESC [ C) -> DKEY_RIGHT");
    }

    /* Test LEFT arrow: ESC [ D */
    {
        char seq[] = { 27, '[', 'D' };
        write_to_stdin(seq, 3);
        int key = dialog_read_key();
        ASSERT(key == DKEY_LEFT, "LEFT arrow (ESC [ D) -> DKEY_LEFT");
    }

    /* Test ESC alone (cancel) */
    {
        char seq[] = { 27 };
        write_to_stdin(seq, 1);
        int key = dialog_read_key();
        ASSERT(key == DKEY_ESC, "ESC alone -> DKEY_ESC (cancel)");
    }

    /* Test regular character */
    {
        char c = 'x';
        write_to_stdin(&c, 1);
        int key = dialog_read_key();
        ASSERT(key == 'x', "Regular char 'x' -> 'x'");
    }

    /* Test multiple arrows in sequence */
    {
        char seq[] = { 27, '[', 'B', 27, '[', 'B', 27, '[', 'A' };
        write_to_stdin(seq, 9);
        int k1 = dialog_read_key();
        int k2 = dialog_read_key();
        int k3 = dialog_read_key();
        ASSERT(k1 == DKEY_DOWN && k2 == DKEY_DOWN && k3 == DKEY_UP,
               "Multiple arrows: DOWN, DOWN, UP");
    }

    close(test_pipe_fd[1]);
}

/* ---- Test 3: File open dialog with arrow navigation ---- */
/* This test simulates the file open dialog by calling file_open_dialog()
 * with mock read_dir data, and injecting arrow keys to navigate.
 *
 * Since file_open_dialog() calls dialog_read_key() which reads from fd 0,
 * we write arrow key sequences to the pipe and verify the dialog selects
 * the correct file.
 */

static void test_file_open_dialog_arrows(void) {
    printf("[TEST] File open dialog arrow navigation...\n");

    /* Create a new pipe for this test */
    if (pipe(test_pipe_fd) < 0) {
        perror("pipe");
        return;
    }
    dup2(test_pipe_fd[0], 0);
    close(test_pipe_fd[0]);

    /* Write: DOWN, DOWN, ENTER (select 3rd file)
     * The mock read_dir in compat.c returns:
     *   0: EDITOR.BIN, 1: DESKTOP.BIN, 2: SH.BIN, ...
     * So after 2 DOWNs, selected = 2 (SH.BIN), then ENTER selects it.
     */
    {
        char seq[] = {
            27, '[', 'B',  /* DOWN */
            27, '[', 'B',  /* DOWN */
            '\n'            /* ENTER */
        };
        write_to_stdin(seq, sizeof(seq));
    }

    char buf[64] = {0};
    int result = file_open_dialog(buf, sizeof(buf));

    /* The dialog should return 1 (selected) and buf should contain a filename */
    ASSERT(result == 1, "file_open_dialog returns 1 after ENTER");
    ASSERT(buf[0] != '\0', "Selected filename is non-empty");

    /* The 3rd file (index 2) in the mock is SH.BIN */
    if (strstr(buf, "SH.BIN") || strstr(buf, "SH")) {
        tests_passed++;
        printf("  PASS: Selected 3rd file (SH.BIN) after 2 DOWNs\n");
    } else {
        tests_failed++;
        printf("  FAIL: Expected SH.BIN, got '%s'\n", buf);
    }

    close(test_pipe_fd[1]);
}

/* ---- Test 4: File open dialog UP navigation ---- */
/* Test that UP arrow moves selection up */

static void test_file_open_dialog_up(void) {
    printf("[TEST] File open dialog UP navigation...\n");

    if (pipe(test_pipe_fd) < 0) {
        perror("pipe");
        return;
    }
    dup2(test_pipe_fd[0], 0);
    close(test_pipe_fd[0]);

    /* Write: DOWN, DOWN, DOWN, UP, ENTER
     * After 3 DOWNs: selected = 3 (LS.BIN)
     * After 1 UP: selected = 2 (SH.BIN)
     * ENTER selects SH.BIN
     */
    {
        char seq[] = {
            27, '[', 'B',  /* DOWN */
            27, '[', 'B',  /* DOWN */
            27, '[', 'B',  /* DOWN */
            27, '[', 'A',  /* UP */
            '\n'            /* ENTER */
        };
        write_to_stdin(seq, sizeof(seq));
    }

    char buf[64] = {0};
    int result = file_open_dialog(buf, sizeof(buf));

    ASSERT(result == 1, "file_open_dialog returns 1 after UP+ENTER");

    /* After 3 DOWNs and 1 UP, selected = 2 (SH.BIN) */
    if (strstr(buf, "SH.BIN") || strstr(buf, "SH")) {
        tests_passed++;
        printf("  PASS: UP navigation moved selection to SH.BIN\n");
    } else {
        tests_failed++;
        printf("  FAIL: Expected SH.BIN after UP, got '%s'\n", buf);
    }

    close(test_pipe_fd[1]);
}

/* ---- Test 5: ESC cancels file open dialog ---- */

static void test_file_open_dialog_cancel(void) {
    printf("[TEST] File open dialog ESC cancel...\n");

    if (pipe(test_pipe_fd) < 0) {
        perror("pipe");
        return;
    }
    dup2(test_pipe_fd[0], 0);
    close(test_pipe_fd[0]);

    /* Write just ESC (cancel) */
    char esc = 27;
    write_to_stdin(&esc, 1);

    char buf[64] = {0};
    int result = file_open_dialog(buf, sizeof(buf));

    ASSERT(result == 0, "file_open_dialog returns 0 on ESC cancel");
    ASSERT(buf[0] == '\0', "Buffer empty on cancel");

    close(test_pipe_fd[1]);
}

/* ---- Test 6: Regression — the dialog must list ALL files, not just 2 ---- */
/*
 * Reproduces the reported bug where the editor's File > Open dialog listed
 * only the first two root entries (e.g. "EFI", "BOOT") instead of the whole
 * directory. The mock read_dir() in compat.c returns 7 files; a healthy
 * dialog must make every one of them reachable.
 *
 * We press DOWN far more times than there are files, so the selection clamps
 * on the LAST enumerated file, then ENTER. The last of the 7 mock files is
 * NOTES.TXT (index 6). Reaching it proves the dialog enumerated the full list.
 *
 * If enumeration ever regresses to stopping after two entries, load_files()
 * would return only 2 files, the selection would clamp at DESKTOP.BIN
 * (index 1), and this test FAILS — exactly reproducing the reported symptom.
 */
static void test_file_open_dialog_lists_all_files(void) {
    printf("[TEST] File open dialog lists ALL files, not just 2...\n");

    if (pipe(test_pipe_fd) < 0) {
        perror("pipe");
        return;
    }
    dup2(test_pipe_fd[0], 0);
    close(test_pipe_fd[0]);

    /* Press DOWN 12 times (more than the 7 mock files) so selection clamps on
     * the last file, then ENTER to select it. */
    {
        char seq[3 * 12 + 1];
        int p = 0;
        for (int i = 0; i < 12; i++) {
            seq[p++] = 27; seq[p++] = '['; seq[p++] = 'B';  /* DOWN */
        }
        seq[p++] = '\n';                                    /* ENTER */
        write_to_stdin(seq, p);
    }

    char buf[64] = {0};
    int result = file_open_dialog(buf, sizeof(buf));

    ASSERT(result == 1, "file_open_dialog returns 1 after navigating to last file");
    /* The last of the 7 mock files (index 6) is NOTES.TXT. Reaching it proves
     * every entry past the first two was enumerated and listed. */
    if (strstr(buf, "NOTES.TXT")) {
        tests_passed++;
        printf("  PASS: reached last file NOTES.TXT (all 7 entries listed, not just 2)\n");
    } else {
        tests_failed++;
        printf("  FAIL: expected NOTES.TXT (index 6); got '%s' -- list truncated\n", buf);
    }

    close(test_pipe_fd[1]);
}

int main(void) {
    printf("=== Dialog Arrow Key Test Suite ===\n\n");

    test_dkey_constants();
    test_arrow_keys();
    test_file_open_dialog_arrows();
    test_file_open_dialog_up();
    test_file_open_dialog_cancel();
    test_file_open_dialog_lists_all_files();

    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    if (tests_failed == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        printf("\nSOME TESTS FAILED\n");
        return 1;
    }
}