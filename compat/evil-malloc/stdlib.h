#ifndef _BS_SWAP_H_
#define _BS_SWAP_H_

#include_next <stdlib.h>

void *dlmalloc(size_t);
void *dlcalloc(size_t, size_t);
void *dlrealloc(void *, size_t);
void dlfree(void *);
char *dlstrdup(const char *str);
#define malloc dlmalloc
#define calloc dlcalloc
#define realloc dlrealloc
#define free dlfree
#define strdup dlstrdup

#endif
