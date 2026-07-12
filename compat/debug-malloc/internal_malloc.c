#include <errno.h>
#include <stdlib.h>

/* use the real system malloc here */
#undef malloc

void *__internal_malloc(size_t size) {
  size += sizeof(size_t);
  /* overflow check */
  if (size < sizeof(size_t)) {
    errno = ENOMEM;
    return NULL;
  }
  /* round up to multiple of page size */
  size_t *ptr = malloc(size);
  if (!ptr)
    return NULL;

  ptr[0] = size;

  return ptr + 1;
}
