#include <stdlib.h>

/* use the real system free */
#undef free

void __internal_free(void *ptr) {
  if (!ptr)
    return;

  ptr = (size_t *)ptr - 1;

  free(ptr);
}
