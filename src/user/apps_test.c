/*
 * apps_test.c - In-OS desktop application harness (APPS_T.BIN). [STUB]
 *
 * This program runs in place of the desktop (it links desktop.c with
 * DESKTOP_TEST_WRAPPER, like src/user/editor_test.c) and drives the real
 * window manager with injected events to launch and verify every desktop
 * application, end to end, inside QEMU.
 *
 * Replace this stub with the real harness. See src/user/editor_test.c for
 * the wrapping pattern (mock read_dir / flush_fb / get_events), and
 * run_apps_test.py for the host-side verdict watcher.
 */

#include "libc.h"
#include "graphics/graphics.h"
#include "graphics/window.h"

extern int desktop_main(void);

__attribute__((section(".text._start")))
void _start(void) {
    print_console("[APPS_T] STUB harness - replace with the real one\n");
    print_console("APPS TEST FAILED (stub)\n");
    exit(1);
}
