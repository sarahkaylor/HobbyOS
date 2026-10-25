/*
 * src/host/ho_scratch.h - GB-085 per-process scratch directory.
 *
 * Two concurrent `make host_tests` runs used to collide on fixed /tmp paths
 * (/tmp/TODO.TXT, /tmp/NOTES.TXT, /tmp/ANTFARM.TST, ...): whichever run lost
 * the race to create/open the shared file failed.  The fix is a per-process
 * scratch directory, honoured by every host test instead of a fixed /tmp
 * path, so two concurrent suite runs can never touch the same file.
 *
 * Design (per gaming.md GB-085 DoD):
 *   - The directory is unique per *process* (pid is enough), so two suite
 *     runs on one host get different dirs with zero coordination.
 *   - It honours $TMPDIR (falls back to /tmp), because the farm uses a
 *     quota'd tmpfs that must be steerable.
 *   - $HO_TEST_SHARED_DIR overrides it to ONE fixed path, which is how the
 *     concurrency gate proves it can FAIL: with the override set, two runs
 *     collide again and at least one must report failures.
 *   - The directory is created with a real `mkdir` (via system()), NOT the
 *     inert mkdir mock that compat.c links, so it actually exists on disk.
 *
 * This is a board-of-record helper for the infra lane; keep it self-contained.
 */
#ifndef HO_SCRATCH_H
#define HO_SCRATCH_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char ho_scratch_dir_buf[512];
static int  ho_scratch_ready = 0;

/* Per-process scratch root (created on disk). Never a fixed /tmp path. */
static const char *ho_scratch_dir(void) {
    if (ho_scratch_ready) return ho_scratch_dir_buf;
    const char *shared = getenv("HO_TEST_SHARED_DIR");
    const char *tmp    = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    if (shared && *shared) {
        snprintf(ho_scratch_dir_buf, sizeof ho_scratch_dir_buf, "%s", shared);
    } else {
        /* Keep the per-process component SHORT: the apps store and re-display
         * the full filename in a 64-byte buffer (HEX_NAME_MAX/DIFF_NAME_MAX),
         * so a long TMPDIR must still leave room.  PID gives uniqueness. */
        snprintf(ho_scratch_dir_buf, sizeof ho_scratch_dir_buf,
                 "%s/h%d", tmp, (int)getpid());
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", ho_scratch_dir_buf);
    (void)system(cmd);   /* real shell mkdir; bypasses the compat mkdir mock */
    ho_scratch_ready = 1;
    return ho_scratch_dir_buf;
}

/* scratch_dir/<name> into a rotating buffer so two scratch paths can be
 * live in one statement (e.g. diff_load_slot(0, DIFF_A) && (1, DIFF_B)).
 * Real files live here instead of a shared /tmp path (GB-085). */
static const char *ho_scratch_path(const char *name) {
    static char p[8][600];
    static int  idx = 0;
    int i = idx;
    idx = (idx + 1) % 8;
    snprintf(p[i], sizeof p[i], "%s/%s", ho_scratch_dir(), name);
    return p[i];
}

#endif /* HO_SCRATCH_H */
