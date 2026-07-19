#ifndef FILEDIALOG_H
#define FILEDIALOG_H

/*
 * File Open/Save Dialog Library for HobbyOS user programs.
 *
 * Uses the modal dialog library for display and input.
 * File listing uses read_dir() from the HobbyOS libc.
 *
 * The open dialog lists files in the root directory and allows
 * navigation with Up/Down arrows, selection with Enter, and
 * cancellation with ESC.
 */

/* File open dialog.
 * Lists files in the root directory, allows navigation with
 * arrow keys, selection with Enter, cancellation with ESC.
 * buf:  buffer to receive the selected filename (null-terminated)
 * max:  buffer size
 * Returns: 1 on selection, 0 on cancel
 */
int file_open_dialog(char *buf, int max);

/* File save dialog.
 * Prompts for a filename to save to.
 * buf:  buffer to receive the filename (null-terminated)
 * max:  buffer size
 * Returns: 1 on confirm, 0 on cancel
 */
int file_save_dialog(char *buf, int max);

#endif /* FILEDIALOG_H */