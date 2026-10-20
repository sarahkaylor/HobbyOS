#ifndef LINUX_UNAME_H
#define LINUX_UNAME_H

#include "linux_layout.h"

/* GB-016: Linux-ish uname identity for the compat surface.
 *
 * SDL and the games sniff uname (sysname==Linux, machine per arch) to pick
 * paths + codepaths, so a HobbyOS-compat process's uname() must not present a
 * foreign identity.  This fills the Linux `struct utsname` layout with a
 * Linux-ABI identity: sysname "Linux", machine "x86_64" on x86_64 and
 * "aarch64" on arm, plus a HobbyOS-marked release/version so the identity is
 * honest (not a forged Linux kernel string).
 *
 * Pure function, no syscall wiring here (that rides GB-014's dispatch):
 * host-buildable and T2-testable.  Returns 0. */
int linux_fill_utsname(linux_utsname_t *u);

#endif /* LINUX_UNAME_H */
