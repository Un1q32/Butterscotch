#include <malloc.h>

size_t __total_heap_bytes_used(void) {
  size_t ret = 0;
  if (__heap_list) {
    ret += __internal_malloc_usable_size(__heap_list);
    for (size_t i = 0; i < __heap_list_size; ++i)
      ret += __internal_malloc_usable_size(__heap_list[i]);
  }
  return ret;
}
