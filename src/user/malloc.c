#include "malloc.h"
#include <stdint.h>

#ifndef HOST_TEST
#include "process.h"
#else
/* Host stand-in: the allocator is pure free-list logic on a fake heap, so
 * compile it natively (hb_* names) and test against glibc behavior. */
#define USER_VIRT_BASE 0x44000000UL
#define USER_REGION_SIZE 0x2000000UL
#endif

#ifdef HOST_TEST
#define malloc hb_malloc
#define free hb_free
#define calloc hb_calloc
#define realloc hb_realloc
#endif

// --- Memory Allocator ---

#ifndef HOST_TEST
extern char _end[];
#else
/* Host heap: a real static buffer so writes land in mapped memory. */
#define HOST_HEAP_SIZE (48u * 1024u * 1024u)
static unsigned char host_heap_buf[HOST_HEAP_SIZE] __attribute__((aligned(16)));
#endif
static void *heap_ptr = NULL;

struct block {
  size_t size;
  int free;
  struct block *next;
};

#define BLOCK_SIZE sizeof(struct block)

static struct block *free_list = NULL;

void *malloc(size_t size) {
  if (size == 0) return NULL;

  // Alignment: 16 bytes
  size = (size + 15) & ~15;

  if (!free_list) {
    // Initialize heap
    if (!heap_ptr) {
#ifdef HOST_TEST
      heap_ptr = (void *)host_heap_buf;
#else
      heap_ptr = (void *)(((uintptr_t)_end + 15) & ~15);
#endif
    }
    free_list = (struct block *)heap_ptr;

    // Calculate heap limit: USER_VIRT_BASE + USER_REGION_SIZE - 256KB for stack
#ifdef HOST_TEST
    uintptr_t heap_limit =
        (uintptr_t)heap_ptr + sizeof(host_heap_buf) - 256 * 1024;
#else
    uintptr_t stack_reserve = 256 * 1024;
    uintptr_t heap_limit = USER_VIRT_BASE + USER_REGION_SIZE - stack_reserve;
#endif

    if ((uintptr_t)heap_ptr >= heap_limit) return NULL;

    free_list->size = heap_limit - (uintptr_t)heap_ptr - BLOCK_SIZE;
    free_list->free = 1;
    free_list->next = NULL;
  }

  struct block *curr = free_list;
  while (curr) {
    if (curr->free && curr->size >= size) {
      // Split block if there's enough room
      if (curr->size >= size + BLOCK_SIZE + 16) {
        struct block *new_block = (struct block *)((char *)curr + BLOCK_SIZE + size);
        new_block->size = curr->size - size - BLOCK_SIZE;
        new_block->free = 1;
        new_block->next = curr->next;

        curr->size = size;
        curr->next = new_block;
      }
      curr->free = 0;
      return (void *)((char *)curr + BLOCK_SIZE);
    }
    curr = curr->next;
  }

  return NULL; // Out of memory
}

void free(void *ptr) {
  if (!ptr) return;

  struct block *curr = (struct block *)((char *)ptr - BLOCK_SIZE);
  curr->free = 1;

  // Coalesce adjacent free blocks
  curr = free_list;
  while (curr && curr->next) {
    if (curr->free && curr->next->free) {
      curr->size += BLOCK_SIZE + curr->next->size;
      curr->next = curr->next->next;
    } else {
      curr = curr->next;
    }
  }
}

void *calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  void *ptr = malloc(total);
  if (ptr) {
    char *cptr = (char *)ptr;
    for (size_t i = 0; i < total; i++) {
      cptr[i] = 0;
    }
  }
  return ptr;
}

void *realloc(void *ptr, size_t size) {
  struct block *curr, *next;
  size_t aligned;

  if (!ptr) return malloc(size);
  if (size == 0) {
    free(ptr);
    return NULL;
  }

  aligned = (size + 15) & ~15;
  curr = (struct block *)((char *)ptr - BLOCK_SIZE);
  next = curr->next;

  /* Shrink in place: split the current block. */
  if (aligned <= curr->size) {
    if (curr->size >= aligned + BLOCK_SIZE + 16) {
      struct block *rest =
          (struct block *)((char *)curr + BLOCK_SIZE + aligned);
      rest->size = curr->size - aligned - BLOCK_SIZE;
      rest->free = 1;
      rest->next = curr->next;
      curr->size = aligned;
      curr->next = rest;
    }
    return ptr;
  }

  /* Grow in place if the next block is free and big enough. The old next
   * header becomes part of curr's payload, so curr->size must grow by the
   * full consumed span and any remainder is re-chained as curr->next. */
  if (next && next->free &&
      curr->size + BLOCK_SIZE + next->size >= aligned) {
    size_t total = aligned;
    size_t leftover =
        curr->size + BLOCK_SIZE + next->size - total;
    if (leftover >= BLOCK_SIZE + 16) {
      struct block *rest =
          (struct block *)((char *)curr + BLOCK_SIZE + total);
      /* rest may overlap the old next header (when the growth need is
       * smaller than BLOCK_SIZE), so capture next->next BEFORE the rest
       * writes overwrite it. */
      struct block *nnext = next->next;
      rest->size = leftover - BLOCK_SIZE;
      rest->free = 1;
      rest->next = nnext;
      curr->size = total;
      curr->next = rest;
    } else {
      curr->size += BLOCK_SIZE + next->size;
      curr->next = next->next;
    }
    return ptr;
  }

  /* General case: new block, copy, free old. */
  {
    void *newptr = malloc(size);
    size_t copy;
    size_t i;
    if (!newptr) return NULL;
    copy = curr->size < aligned ? curr->size : aligned;
    {
      char *d = (char *)newptr, *s = (char *)ptr;
      for (i = 0; i < copy; i++) d[i] = s[i];
    }
    free(ptr);
    return newptr;
  }
}

#ifdef HOST_TEST
/* Consistency probe for the host realloc test: walks the free list and
 * returns the first node index that violates a basic structural invariant
 * (out-of-heap, non-forward next, or a cycle). 0 == healthy. */
int hb_heap_integrity(void) {
  struct block *c = free_list;
  int depth = 0;
  const unsigned char *lo = host_heap_buf;
  const unsigned char *hi = host_heap_buf + sizeof(host_heap_buf);
  while (c) {
    if (depth > 20000) return 20001;
    if ((unsigned char *)c < lo || (unsigned char *)c >= hi)
      return 0x10000 + depth;
    depth++;
    if (!c->next) return 0;
    if ((unsigned char *)c->next <= (unsigned char *)c ||
        (unsigned char *)c->next > hi)
      return depth;
    c = c->next;
  }
  return 0;
}
#endif
