#ifndef MALLOC_H
#define MALLOC_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void*)0)
#endif

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);

#endif
