#ifndef SETJMP_H
#define SETJMP_H

#include <stdint.h>

// Buffer to save the CPU context for non-local jumps.
// Stores critical registers (x19-x29, sp, lr).
typedef uint64_t jmp_buf[16];

// Save the current execution context into the provided buffer.
// Returns 0 when the context is saved.
int setjmp(jmp_buf env);

// Restore a previously saved execution context from the buffer.
// The program resumes as if setjmp returned 'val'.
void longjmp(jmp_buf env, int val);

#endif // SETJMP_H
