#include "malloc.h"
#include <stdint.h>

#ifndef HOST_TEST
#include "process.h"
#include "errno.h"
#else
/* Host stand-in: the allocator is pure free-list logic on a fake heap, so
 * compile it natively (hb_* names) and test against glibc behavior. */
#include <errno.h>
#define USER_VIRT_BASE 0x44000000UL
#define USER_REGION_SIZE 0x2000000UL
#endif

#ifdef HOST_TEST
#define malloc hb_malloc
#define free hb_free
#define calloc hb_calloc
#define realloc hb_realloc
#define aligned_alloc hb_aligned_alloc
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
  unsigned long magic; /* BLOCK_MAGIC: distinguishes real blocks from the
                        * word before an aligned_alloc() payload */
};

#define BLOCK_SIZE sizeof(struct block)
#define BLOCK_MAGIC 0x48424F424C4B4D47UL /* "HBOBLKMG" */

static struct block *free_list = NULL;

/* P1 (p1-threads-design.md section 6): the allocator is shared by every
 * thread of a process, so the free list must be serialized.  A plain
 * test-and-set spinlock is enough: the critical sections are short and
 * never block (no allocation inside an I/O wait), and threads preempt
 * freely at the tick, so a spinning waiter cannot starve the holder.
 * Host builds (single-threaded harness, hb_* names) compile it out. */
#ifndef HOST_TEST
static char malloc_lock_byte;
static void malloc_lock(void) {
  while (__atomic_test_and_set(&malloc_lock_byte, __ATOMIC_ACQUIRE)) {
    /* spin; preemption at the timer tick keeps this fair */
  }
}
static void malloc_unlock(void) {
  __atomic_clear(&malloc_lock_byte, __ATOMIC_RELEASE);
}
#else
#define malloc_lock() ((void)0)
#define malloc_unlock() ((void)0)
#endif

static void *malloc_locked(size_t size) {
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
    free_list->magic = BLOCK_MAGIC;
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
        new_block->magic = BLOCK_MAGIC;

        curr->size = size;
        curr->next = new_block;
      }
      curr->free = 0;
      curr->magic = BLOCK_MAGIC;
      return (void *)((char *)curr + BLOCK_SIZE);
    }
    curr = curr->next;
  }

  return NULL; // Out of memory
}

void *malloc(size_t size) {
  malloc_lock();
  void *p = malloc_locked(size);
  malloc_unlock();
  return p;
}

static void free_locked(void *ptr) {
  struct block *curr;

  if (!ptr) return;

  curr = (struct block *)((char *)ptr - BLOCK_SIZE);
  if (curr->magic != BLOCK_MAGIC) {
    /* Possibly an aligned_alloc() payload: its raw block payload pointer is
     * stashed in the word directly before it.  Recompute the real block. */
    char *raw = ((char **)ptr)[-1];
    curr = (struct block *)(raw - BLOCK_SIZE);
    if (curr->magic != BLOCK_MAGIC)
      return; /* not a HobbyOS heap pointer (UB input): ignore */
  }
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

void free(void *ptr) {
  malloc_lock();
  free_locked(ptr);
  malloc_unlock();
}

void *calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  malloc_lock();
  void *ptr = malloc_locked(total);
  if (ptr) {
    char *cptr = (char *)ptr;
    for (size_t i = 0; i < total; i++) {
      cptr[i] = 0;
    }
  }
  malloc_unlock();
  return ptr;
}

static void *realloc_locked(void *ptr, size_t size) {
  struct block *curr, *next;
  size_t aligned;

  if (!ptr) return malloc_locked(size);
  if (size == 0) {
    free_locked(ptr);
    return NULL;
  }

  aligned = (size + 15) & ~15;
  curr = (struct block *)((char *)ptr - BLOCK_SIZE);
  if (curr->magic != BLOCK_MAGIC) {
    /* Aligned allocation: relocate through an ordinary block. */
    char *raw = ((char **)ptr)[-1];
    struct block *rb = (struct block *)(raw - BLOCK_SIZE);
    void *newptr;
    size_t copy, i;
    char *d, *s2;

    if (rb->magic != BLOCK_MAGIC)
      return NULL;
    newptr = malloc_locked(size);
    if (!newptr)
      return NULL;
    copy = rb->size < size ? rb->size : size;
    d = (char *)newptr;
    s2 = (char *)ptr;
    for (i = 0; i < copy; i++)
      d[i] = s2[i];
    free_locked(ptr);
    return newptr;
  }
  next = curr->next;

  /* Shrink in place: split the current block. */
  if (aligned <= curr->size) {
    if (curr->size >= aligned + BLOCK_SIZE + 16) {
      struct block *rest =
          (struct block *)((char *)curr + BLOCK_SIZE + aligned);
      rest->size = curr->size - aligned - BLOCK_SIZE;
      rest->free = 1;
      rest->next = curr->next;
      rest->magic = BLOCK_MAGIC;
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
      rest->magic = BLOCK_MAGIC;
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
    void *newptr = malloc_locked(size);
    size_t copy;
    size_t i;
    if (!newptr) return NULL;
    copy = curr->size < aligned ? curr->size : aligned;
    {
      char *d = (char *)newptr, *s = (char *)ptr;
      for (i = 0; i < copy; i++) d[i] = s[i];
    }
    free_locked(ptr);
    return newptr;
  }
}

void *realloc(void *ptr, size_t size) {
  malloc_lock();
  void *p = realloc_locked(ptr, size);
  malloc_unlock();
  return p;
}

/* C11 7.22.3.1 aligned_alloc (P3.2: libc++'s C++17 aligned operator new
 * calls this).  The payload is aligned inside a raw block with the raw
 * payload pointer stashed in the word right before it, so free() and
 * realloc() can recover the block (see free_locked). */
void *aligned_alloc(size_t alignment, size_t size) {
  char *raw;
  uintptr_t u;

  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    errno = EINVAL;
    return NULL;
  }
  if (alignment < 16)
    alignment = 16; /* the allocator's natural granularity */

  malloc_lock();
  raw = (char *)malloc_locked(size + alignment);
  if (!raw) {
    malloc_unlock();
    errno = ENOMEM;
    return NULL;
  }
  u = ((uintptr_t)raw + alignment - 1) & ~(uintptr_t)(alignment - 1);
  if ((char *)u != raw)
    ((char **)u)[-1] = raw;
  malloc_unlock();
  return (void *)u;
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
