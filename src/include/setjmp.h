#ifndef SETJMP_H
#define SETJMP_H

#include <stdint.h>

typedef uint64_t jmp_buf[16]; // 16 * 8 = 128 bytes, plenty of room

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
