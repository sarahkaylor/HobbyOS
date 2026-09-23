/*
 * assert.h — standard <assert.h> for the HobbyOS sysroot.
 *
 * NDEBUG disables the checks entirely (as required).  _assert_fail lives
 * in the sysroot stdlib.c and writes the diagnostic to fd 2 (console on
 * the device, stderr in HOST_TEST) before abort().
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
void _assert_fail(const char *file, int line, const char *func,
                  const char *expr);
#define assert(expr) \
    ((expr) ? (void)0 \
            : _assert_fail(__FILE__, __LINE__, __func__, #expr))
#endif

#endif /* _ASSERT_H */
