#ifndef _BS_DEBUG_MALLOC_H_
#define _BS_DEBUG_MALLOC_H_

#include <stddef.h>
#include <string.h>

struct __malloc_block {
  size_t size;
  struct __malloc_block *prev;
  struct __malloc_block *next;
};

struct __heap {
  struct __malloc_block *last;
};

/* These must always be powers of 2 */
#define __HEAP_LIST_BLOCK_SIZE 4096
#define __HEAP_BLOCK_SIZE 8192

#define free __bs_debug_free
#define malloc __bs_debug_malloc
#define realloc __bs_debug_realloc
#define calloc __bs_debug_calloc
#define malloc_usable_size __bs_debug_malloc_usable_size
#define aligned_alloc __bs_debug_aligned_alloc
#define memalign __bs_debug_memalign
#define posix_memalign __bs_debug_posix_memalign
#define strdup __bs_debug_strdup

extern void *malloc(size_t);
extern void *realloc(void *, size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern size_t malloc_usable_size(void *);
extern void *aligned_alloc(size_t, size_t);
extern void *memalign(size_t, size_t);
extern int posix_memalign(void **, size_t, size_t);
extern char *strdup(const char *);

extern struct __heap **__heap_list;
extern size_t __heap_list_size;
extern void *__internal_malloc(size_t);
extern void __internal_free(void *);
extern size_t __internal_malloc_usable_size(void *);

extern size_t __total_heap_bytes_used(void);

#endif
