/* linux/futex.h - HobbyOS shim for the Mesa cross build (M4/L5).
 *
 * src/util/futex.c implements Mesa's futex helpers on top of the futex
 * syscall; it needs the op codes and the 32-bit futex word type.  HobbyOS's
 * compat class routes SYS_futex (202) to the kernel's futex-lite, and the
 * native track has no futex at all - futex.c degrades to a spin/poll when
 * the syscall fails, so only the constants are required here. */
#ifndef HOBBYOS_LINUX_FUTEX_H
#define HOBBYOS_LINUX_FUTEX_H 1

#include <linux/types.h>

#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_FD          2
#define FUTEX_REQUEUE     3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP     5
#define FUTEX_LOCK_PI     6
#define FUTEX_UNLOCK_PI   7
#define FUTEX_TRYLOCK_PI  8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_WAIT_REQUEUE_PI   11
#define FUTEX_CMP_REQUEUE_PI    12

#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_OP_SET 0
#define FUTEX_OP_ADD 1
#define FUTEX_OP_OR  2
#define FUTEX_OP_ANDN 3
#define FUTEX_OP_XOR 4

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

#endif
