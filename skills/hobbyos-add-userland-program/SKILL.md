---
name: hobbyos-add-userland-program
description: "Use when adding a new user-space program (shell command or GUI app) to HobbyOS. The exact, order-sensitive set of Makefile edits (obj rule, bin rule, disk.img dependency, mcopy line) plus the FAT 8.3 naming and optional host test, with a completion checklist."
version: 1.0.0
author: Sarah Kaylor
license: GPL-2.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [hobbyos, userland, makefile, syscalls, fat16, program]
    related_skills: [hobbyos-run-tests, hobbyos-build-and-run, hobbyos-kernel-constraints]
---

# HobbyOS: Add a Userland Program

## Overview

Adding a user-space program to HobbyOS is a mechanical but **scattered** edit: one new `.c` file plus **four separate Makefile insertions** in different regions of the file. Miss any one and the program either won't build, won't be copied to the disk, or won't exist at runtime — often with no obvious error. This skill is the exact recipe; follow every step and finish with the checklist so nothing is skipped. The payoff of getting the Makefile edits right: the shell runs the program automatically — it resolves a typed command by uppercasing it and appending `.BIN`, so `/FOO.BIN` on disk is runnable as `foo` with **no shell source change**.

## When to Use

- Adding a CLI utility (like `ls`, `grep`, `ps`) or a GUI app (like `pong`, `editor`).
- Any time a new binary must land on `disk.img` and be launchable.

Don't use for: kernel-side changes (→ [[hobbyos-kernel-constraints]]) or adding a *test* only (→ [[hobbyos-run-tests]]).

## Step 1 — Write the program

Create `src/user/<prog>.c`. It compiles freestanding against the userland libc; include the syscall wrappers you need:

```c
#include "libc.h"            // print, read, write, open, close, exit, spawn2, malloc...
// GUI apps also:  #include "graphics/graphics.h"   (and window.h for windowed apps)

int main(int argc, char **argv) {
    print("hello from <prog>\n");
    return 0;
}
```

Links against `user_libc.o` + `user_malloc.o` always; GUI apps additionally link `user_graphics.o` (and `user_window.o` for windowed apps like the desktop). Keep to the userland API — no kernel headers. Mind [[hobbyos-kernel-constraints]] only if you touch alignment-sensitive data.

## Step 2 — Four Makefile edits (all required)

Pick a short name; the pattern below uses `foo`. **The FAT-16 disk is 8.3 uppercase**, so the on-disk name is `FOO.BIN`.

**2a. Declare the bin variable** (near the other `*_BIN =` lines, ~line 89–115):
```make
FOO_BIN = $(OBJ_DIR)/foo.bin
```

**2b. Object compile rule** (in the userland `.o` rules region):
```make
$(OBJ_DIR)/foo.o: src/user/foo.c $(USER_LIBC)
	@mkdir -p $(OBJ_DIR)
	$(CC) $(USER_CFLAGS) -c $< -o $@
```

**2c. Link + objcopy-to-binary rule** (with the other `$(*_BIN):` rules):
```make
# CLI program:
$(FOO_BIN): $(OBJ_DIR)/foo.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o
	$(LD) -T src/user/linker.ld -o $(OBJ_DIR)/foo.elf $^
	$(OBJCOPY) -O binary $(OBJ_DIR)/foo.elf $(FOO_BIN)

# GUI program — add $(OBJ_DIR)/user_graphics.o (and user_window.o if windowed):
# $(FOO_BIN): $(OBJ_DIR)/foo.o $(OBJ_DIR)/user_libc.o $(OBJ_DIR)/user_malloc.o $(OBJ_DIR)/user_graphics.o
```

**2d. Put it on the disk** — two edits in the `disk.img:` target:
- Add `$(FOO_BIN)` to the long prerequisite list on the `disk.img:` line.
- Add an `mcopy` line in the recipe body, next to the other program copies:
```make
	/opt/homebrew/bin/mcopy -i disk.img $(FOO_BIN) ::/FOO.BIN
```

That's the complete set. There is intentionally **no** central program registry beyond these; the four edits *are* the registration.

## Step 3 — Build, deploy, run

```bash
make run              # rebuilds disk.img (depends on every *_BIN) and boots the desktop
# then in the HobbyOS shell:
foo                   # shell uppercases -> spawns /FOO.BIN
```

For a headless/scripted check, add it to an integration path or write a host test (Step 4). See [[hobbyos-build-and-run]] for modes.

## Step 4 — Optional: a host golden test (recommended for logic)

Fast native validation without QEMU, following `src/host/pong_test.c`:
- `src/host/<prog>_test.c` does `#define main <prog>_main` then `#include "../user/<prog>.c"`, and `ASSERT`s the pure functions.
- Add a `*_host` link target in the Makefile's host section and list it under `host_tests`.
- Run `make host_tests`. See [[hobbyos-run-tests]] Tier 1.

## Common Pitfalls

1. **Forgot the `mcopy` line or the `disk.img` prerequisite.** Program builds but isn't on the disk (or isn't rebuilt) → shell says `command not found`. Both 2d edits are required.
2. **Lowercase / long on-disk name.** FAT-16 is 8.3 uppercase — use `::/FOO.BIN`. A name >8 chars or lowercase won't resolve from the shell's uppercase lookup.
3. **GUI app missing `user_graphics.o`** (or `user_window.o`) in the link rule → undefined graphics symbols at link time.
4. **Editing only one place.** The four edits live in four different Makefile regions; a single-spot edit silently half-works. Re-scan all four.
5. **Assuming a shell registration step exists.** There isn't one — but the binary *must* physically be on the disk under the right 8.3 name.

## Verification Checklist

- [ ] `src/user/<prog>.c` compiles (correct includes; no kernel headers).
- [ ] `<PROG>_BIN` variable declared.
- [ ] `.o` compile rule added.
- [ ] `.bin` link+objcopy rule added, with `user_graphics.o`/`user_window.o` if GUI.
- [ ] `$(<PROG>_BIN)` added to the `disk.img:` prerequisite list **and** an `mcopy ... ::/<PROG>.BIN` line added.
- [ ] `make run` rebuilds the disk; the shell launches `<prog>` successfully.
- [ ] (If logic-heavy) a `src/host/<prog>_test.c` passes under `make host_tests`.
