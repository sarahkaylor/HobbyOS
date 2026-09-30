/*
 * assert.h — standard <assert.h> for the HobbyOS sysroot.
 *
 * NDEBUG disables the checks entirely (as required).  _assert_fail lives
 * in the sysroot stdlib.c and writes the diagnostic to fd 2 (console on
 * the device, stderr in HOST_TEST) before abort().
 *
 * F2.5: the declaration carries extern "C" guards — without them a C++
 * TU's assert() call mangles to _Z12_assert_fail..., which no object in
 * libc.a defines (the implementation is C: symbol _assert_fail).
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

  void _assert_fail(const char *file, int line, const char *func,
                    const char *expr);

#ifdef __cplusplus
}
#endif

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 \
            : _assert_fail(__FILE__, __LINE__, __func__, #expr))
#endif

#endif /* _ASSERT_H */
