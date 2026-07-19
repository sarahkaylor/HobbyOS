#ifndef DIALOG_H
#define DIALOG_H

/*
 * Modal Dialog Library for HobbyOS user programs.
 *
 * Dialogs are text-based, rendered via print() to stdout (captured by
 * the desktop window manager). Input is read from stdin (fd 0).
 * Each dialog blocks until the user confirms (Enter) or cancels (ESC).
 *
 * ESC sequences from the desktop window manager are handled:
 *   - Arrow keys (ESC [ A/B/C/D) for navigation
 *   - Menu selections (ESC [ M ...) are consumed and ignored
 *   - ESC alone cancels the dialog
 */

/* Special key codes returned by dialog_read_key() */
#define DKEY_UP    0x100
#define DKEY_DOWN  0x101
#define DKEY_LEFT  0x102
#define DKEY_RIGHT 0x103
#define DKEY_ESC   0x1B

/* Read one key from stdin, handling ESC sequences.
 * Returns the key character, or a DKEY_* constant for special keys.
 * Menu selection sequences (ESC [ M ...) are consumed and return 0
 * so they don't interfere with dialog input.
 * This is exposed so other dialog libraries (filedialog) can reuse it.
 */
int dialog_read_key(void);

/* Prompt for a string input.
 * title: dialog title shown at top
 * msg:  prompt message
 * buf:  buffer to receive the entered string (null-terminated)
 * max:  buffer size
 * Returns: 1 on OK (Enter), 0 on cancel (ESC)
 */
int dialog_prompt(const char *title, const char *msg, char *buf, int max);

/* Show a message and wait for any key.
 * title: dialog title
 * msg:  message text
 */
void dialog_message(const char *title, const char *msg);

/* Yes/No confirmation dialog.
 * title: dialog title
 * msg:  question text
 * Returns: 1 for Yes, 0 for No (or ESC)
 */
int dialog_confirm(const char *title, const char *msg);

#endif /* DIALOG_H */