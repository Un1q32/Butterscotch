#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern void *dlmalloc(size_t size);

#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void *evil_mmap(size_t size) {
    static const char suffix[] = "butterscotch_swapXXXXXX";
    const char *tmpdir = getenv("TMPDIR");
    char path[PATH_MAX];
    size_t len;
    int needs_sep;
    int fd;
    off_t file_size;
    void *ptr;
    int saved_errno;

    if (tmpdir == NULL || tmpdir[0] == '\0')
        tmpdir = "/var/tmp";

    len = strlen(tmpdir);
    needs_sep = len == 0 || tmpdir[len - 1] != '/';
    if (len + needs_sep + sizeof(suffix) > sizeof(path)) {
        errno = ENAMETOOLONG;
        return MAP_FAILED;
    }

    memcpy(path, tmpdir, len);
    if (needs_sep)
        path[len++] = '/';
    memcpy(path + len, suffix, sizeof(suffix));

    fd = mkstemp(path);
    if (fd == -1)
        return MAP_FAILED;

    if (unlink(path) == -1) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return MAP_FAILED;
    }

    file_size = (off_t)size;
    if (file_size < 0 || (size_t)file_size != size) {
        close(fd);
        errno = EOVERFLOW;
        return MAP_FAILED;
    }

    if (ftruncate(fd, file_size) == -1) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return MAP_FAILED;
    }

    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return ptr;
}

#endif

char *dlstrdup(const char *str) {
    size_t len = strlen(str) + 1;
    char *copy = dlmalloc(len);

    if (copy != NULL)
        memcpy(copy, str, len);
    return copy;
}
