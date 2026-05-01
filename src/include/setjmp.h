#ifndef SETJMP_H
#define SETJMP_H

#include <stdint.h>

/**
 * Buffer to save the CPU context for non-local jumps.
 * Stores callee-saved registers (x19-x29), SP, and LR.
 */
typedef uint64_t jmp_buf[16];

/**
 * Saves the current execution context.
 * 
 * Returns:
 *   0 when the context is initially saved.
 */
int setjmp(jmp_buf env);

/**
 * Restores a previously saved execution context.
 */
void longjmp(jmp_buf env, int val);

#endif // SETJMP_H
