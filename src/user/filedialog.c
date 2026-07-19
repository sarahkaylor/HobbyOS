/*
 * File Open/Save Dialog Library for HobbyOS user programs.
 *
 * Uses the modal dialog library for display and input.
 * File listing uses read_dir() from the HobbyOS libc.
 */

#include "filedialog.h"
#include "dialog.h"
#include "libc.h"

/* ---- Constants ---- */

#define FD_MAX_FILES   16    /* Maximum files to list */
#define FD_NAME_LEN    32    /* Matches sys_dirent.name size */

/* ---- String helpers ---- */

static void fd_strcpy(char *dest, const char *src) {
    int i = 0;
    while (src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static int fd_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- File listing ---- */

/* Load file names from the root directory.
 * Returns the number of files loaded, or 0 on error.
 */
static int load_files(char files[][FD_NAME_LEN], int max) {
    int count = 0;
    for (int i = 0; i < max; i++) {
        struct sys_dirent ent;
        if (read_dir("/", i, &ent) < 0) break;
        if (ent.name[0] == '\0') break;
        fd_strcpy(files[count], ent.name);
        count++;
    }
    return count;
}

/* ---- File Open Dialog ---- */

int file_open_dialog(char *buf, int max) {
    char files[FD_MAX_FILES][FD_NAME_LEN];
    int num_files = load_files(files, FD_MAX_FILES);
    int selected = 0;
    int top = 0;

    if (num_files == 0) {
        return dialog_prompt("Open File",
                             "No files found. Type name:",
                             buf, max);
    }

    while (1) {
        /* Render the file open dialog */
        print("\f");
        print("+--------------------------------+\n");
        print("| Open File                      |\n");
        print("+--------------------------------+\n");
        print("  Files in root:\n");

        int visible = 10;
        if (num_files < visible) visible = num_files;

        /* Adjust top so selected is always visible */
        if (selected < top) top = selected;
        if (selected >= top + visible) top = selected - visible + 1;
        if (top < 0) top = 0;

        for (int i = 0; i < visible; i++) {
            int idx = top + i;
            if (idx >= num_files) break;
            if (idx == selected) {
                print(" > ");
            } else {
                print("   ");
            }
            print(files[idx]);
            /* Pad to column width */
            int len = fd_strlen(files[idx]);
            for (int j = 0; j < 28 - len; j++) print(" ");
            print("\n");
        }

        print("+--------------------------------+\n");
        print("Up/Down=Navigate Enter=Open ESC=Cancel\n");

        int key = dialog_read_key();
        if (key == DKEY_ESC) {
            return 0;
        }
        if (key == '\n') {
            if (num_files > 0 && selected >= 0 && selected < num_files) {
                fd_strcpy(buf, files[selected]);
                return 1;
            }
            return 0;
        }
        if (key == DKEY_UP) {
            if (selected > 0) selected--;
            continue;
        }
        if (key == DKEY_DOWN) {
            if (selected < num_files - 1) selected++;
            continue;
        }
        if (key == 0) {
            /* Menu selection — ignore */
            continue;
        }
        /* Other keys — ignore */
    }
}

/* ---- File Save Dialog ---- */

int file_save_dialog(char *buf, int max) {
    /* Show existing files as reference, then prompt for a name */
    char files[FD_MAX_FILES][FD_NAME_LEN];
    int num_files = load_files(files, FD_MAX_FILES);

    print("\f");
    print("+--------------------------------+\n");
    print("| Save File                      |\n");
    print("+--------------------------------+\n");
    if (num_files > 0) {
        print("  Existing files:\n");
        int show = num_files;
        if (show > 5) show = 5;
        for (int i = 0; i < show; i++) {
            print("  ");
            print(files[i]);
            print("\n");
        }
        print("\n");
    }
    print("Enter filename to save to:\n");

    /* Use dialog_prompt for the actual input */
    return dialog_prompt("Save File", "Filename", buf, max);
}