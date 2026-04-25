#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>

#include "malloc.h"

void print(const char* str);
void exit(void);
int fork(void);

#endif
