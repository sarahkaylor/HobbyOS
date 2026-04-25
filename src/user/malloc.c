#include "malloc.h"
#include <stdint.h>
#include "process.h"

// --- Memory Allocator ---

extern char _end[];
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
      heap_ptr = (void *)(((uintptr_t)_end + 15) & ~15);
    }
    free_list = (struct block *)heap_ptr;
    
    // Calculate heap limit: USER_VIRT_BASE + USER_REGION_SIZE - 4MB for stack
    uintptr_t stack_reserve = 4 * 1024 * 1024;
    uintptr_t heap_limit = USER_VIRT_BASE + USER_REGION_SIZE - stack_reserve;
    
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
