#include <errno.h>
#include <sys/mman.h>
#include <stdlib.h>

#if defined(__linux__)
#include <sys/user.h>
#elif defined(_WIN32)
#error windows not supported
#else /* darwin and bsd, hope for the best */
#include <sys/param.h>
#endif

void *__internal_malloc(size_t size) {
  size += sizeof(size_t);
  /* overflow check */
  if (size < sizeof(size_t)) {
    errno = ENOMEM;
    return NULL;
  }
  /* round up to multiple of page size */
  if ((size & (PAGE_SIZE - 1)) != 0)
    size = (size | (PAGE_SIZE - 1)) + 1;
  size_t *ptr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (ptr == MAP_FAILED)
    return NULL;

  ptr[0] = size;

  return ptr + 1;
}
