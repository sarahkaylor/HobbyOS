#ifndef LIBC_H
#define LIBC_H

#include <stddef.h>

#include "malloc.h"

void print(const char* str);
void exit(void);
int fork(void);

int open(const char* filename);
int close(int fd);
int read(int fd, void* buf, int size);
int write(int fd, const void* buf, int size);
int spawn(const char* filename);

#endif
void* map_fb(void);
void flush_fb(void);
