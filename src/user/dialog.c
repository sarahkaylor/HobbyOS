/*
 * Modal Dialog Library for HobbyOS user programs.
 *
 * Text-based dialogs rendered via print() to stdout (captured by
 * the desktop window manager). Input is read from stdin (fd 0).
 *
 * ESC sequences from the desktop window manager are handled:
 *   - Arrow keys (ESC [ A/B/C/D) for navigation
 *   - Menu selections (ESC [ M ...) are consumed and ignored
 *   - ESC alone cancels the dialog
 */

#include "dialog.h"
#include "libc.h"

/* ---- String helpers (HobbyOS has no standard string functions) ---- */

static int d_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- Key reading with ESC sequence handling ---- */

/* Read one key from stdin, handling ESC sequences.
 * Returns the key character, or a DKEY_* constant for special keys.
 * Menu selection sequences (ESC [ M ...) are consumed and return 0
 * so they don't interfere with dialog input.
 *
 * The desktop sends arrow keys as 3-byte ESC sequences: ESC [ A/B/C/D.
 * We read the ESC byte, then poll for the remaining bytes with a timeout
 * to distinguish ESC-alone (cancel) from ESC sequences (arrow keys).
 */
int dialog_read_key(void) {
    char c;
    if (read(0, &c, 1) <= 0) return 0;

    if (c != 27) {
        return (int)(unsigned char)c;
    }

    /* ESC: check for escape sequence.
     * The desktop writes all 3 bytes (ESC [ X) in a single write() call,
     * but due to scheduling the bytes may arrive in the pipe with a slight
     * delay. We poll available() with a retry loop to handle this.
     */
    int retries = 0;
    while (retries < 50) {
        int avail = available(0);
        if (avail >= 2) break;
        /* Not enough bytes yet — wait a bit and retry */
        sleep(1);  /* 1ms sleep */
        retries++;
    }

    /* If no more bytes arrived, this is ESC alone = cancel */
    if (available(0) < 2) {
        return DKEY_ESC;
    }

    /* Read the 2-byte sequence */
    char seq[2];
    int n = read(0, seq, 2);
    if (n < 2) {
        /* Couldn't read 2 bytes — treat as cancel */
        return DKEY_ESC;
    }

    if (seq[0] == '[') {
        if (seq[1] == 'M') {
            /* Menu selection: ESC [ M <menu> ; <item> ~
             * Consume the rest and return 0 (ignore) */
            if (available(0) >= 4) {
                char tmp[4];
                read(0, tmp, 4);
            }
            return 0;
        }
        if (seq[1] == 'A') return DKEY_UP;
        if (seq[1] == 'B') return DKEY_DOWN;
        if (seq[1] == 'C') return DKEY_RIGHT;
        if (seq[1] == 'D') return DKEY_LEFT;
        /* Unknown CSI sequence — return 0 to ignore */
        return 0;
    }
    /* ESC followed by something else — treat as cancel */
    return DKEY_ESC;
}

/* ---- Dialog box rendering ---- */

/* Draw a simple text dialog box with a title and message.
 * Uses a clear-screen + bordered layout.
 */
static void dialog_draw(const char *title, const char *msg, const char *input) {
    print("\f");
    /* Top border */
    print("+--------------------------------+\n");
    /* Title */
    print("| ");
    print(title);
    /* Pad title line to width 32 */
    int tlen = d_strlen(title);
    for (int i = 0; i < 30 - tlen; i++) print(" ");
    print("|\n");
    print("+--------------------------------+\n");
    /* Message */
    print("| ");
    print(msg);
    int mlen = d_strlen(msg);
    for (int i = 0; i < 30 - mlen; i++) print(" ");
    print("|\n");
    /* Input line (if any) */
    if (input) {
        print("|> ");
        print(input);
        print("_");
        int ilen = d_strlen(input);
        for (int i = 0; i < 27 - ilen; i++) print(" ");
        print("|\n");
    }
    print("+--------------------------------+\n");
    print("ESC=Cancel  Enter=OK\n");
}

/* ---- Prompt Dialog ---- */

int dialog_prompt(const char *title, const char *msg, char *buf, int max) {
    int len = 0;
    buf[0] = '\0';

    while (1) {
        dialog_draw(title, msg, buf);
        int key = dialog_read_key();
        if (key == DKEY_ESC) {
            return 0;
        }
        if (key == '\n') {
            return 1;
        }
        if (key == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
            }
            continue;
        }
        if (key == 0) {
            /* Menu selection or unknown — ignore */
            continue;
        }
        if (key >= 32 && key <= 126) {
            if (len < max - 1) {
                buf[len++] = (char)key;
                buf[len] = '\0';
            }
            continue;
        }
        /* Other special keys (arrows) — ignore in prompt */
    }
}

/* ---- Message Dialog ---- */

void dialog_message(const char *title, const char *msg) {
    dialog_draw(title, msg, 0);
    print("\nPress any key to continue...\n");
    /* Wait for any key (consume menu selections) */
    while (1) {
        int key = dialog_read_key();
        if (key != 0) break;
    }
}

/* ---- Confirm Dialog (Yes/No) ---- */

int dialog_confirm(const char *title, const char *msg) {
    print("\f");
    print("+--------------------------------+\n");
    print("| ");
    print(title);
    int tlen = d_strlen(title);
    for (int i = 0; i < 30 - tlen; i++) print(" ");
    print("|\n");
    print("+--------------------------------+\n");
    print("| ");
    print(msg);
    int mlen = d_strlen(msg);
    for (int i = 0; i < 30 - mlen; i++) print(" ");
    print("|\n");
    print("+--------------------------------+\n");
    print("(Y)es  (N)o  ESC=Cancel\n");

    while (1) {
        int key = dialog_read_key();
        if (key == 0) continue;  /* Ignore menu selections */
        if (key == DKEY_ESC) return 0;
        if (key == 'y' || key == 'Y') return 1;
        if (key == 'n' || key == 'N' || key == '\n') return 0;
    }
}