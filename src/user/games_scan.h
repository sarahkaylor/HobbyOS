/* games_scan.h — the S2 game-launcher menu source (GB-039).
 *
 * The desktop's Apps menu was a single pass over the disk root with a fixed
 * pinned_games[] list; a game not pinned sat at the (128-capped) tail and was
 * silently dropped.  This module is the launcher's /GAMES scan: it reads the
 * /GAMES directory, classifies each entry as a native .BIN or an ELF-compat
 * executable, and produces the menu ordering (pinned native games first, then
 * scanned) under the MAX_MENU_ITEMS cap.
 *
 * The classify + order core is pure and host-testable (games_scan_test.c);
 * the guest-only read_dir scan is the thin wrapper the desktop calls.
 */
#ifndef GAMES_SCAN_H
#define GAMES_SCAN_H

#define GAMES_MAX 128
#define GAMES_NAME 16

enum games_kind {
  GAMES_NONE = 0,   /* not a launchable entry */
  GAMES_BIN = 1,    /* native .BIN (HobbyOS ABI) */
  GAMES_ELF = 2,    /* ELF-compat executable (kernel loader dispatches on 0x7fELF) */
  GAMES_OTHER = 3,  /* present but not a game we launch directly */
};

struct games_entry {
  char name[GAMES_NAME];
  int kind;
};

/* Classify a file name: case-insensitive .BIN/.EXE -> GAMES_BIN; .ELF or an
 * extension-less executable -> GAMES_ELF; known data extensions -> OTHER;
 * otherwise NONE.  (The loader does the authoritative 0x7fELF magic check at
 * spawn; this is the menu's intent/display classification.) */
int games_classify(const char *name);

/* Produce the launcher menu: known pinned native games first, then scanned
 * entries, each stable, capped at `max`.  Returns the number written.  Pure +
 * host-testable. */
int games_order(struct games_entry *out,
                const struct games_entry *pinned, int npinned,
                const struct games_entry *scanned, int nscanned,
                int max);

/* Short human label for a kind ("BIN"/"ELF"/"OTHER"/"---"). */
int games_kind_str(int kind, char *buf, int n);

/* Guest-side: scan /GAMES via read_dir into `out` (capped at max), each
 * entry classified.  Returns the count. */
int games_scan_dir(struct games_entry *out, int max);

#endif
