// libnx has no mmap; plutovg only uses it to read font files, so emulate a
// read-only private mapping with malloc + read.
#pragma once
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#define PROT_READ 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)prot; (void)flags;
    char *p = malloc(len ? len : 1);
    if (!p || lseek(fd, off, SEEK_SET) < 0) {
        free(p);
        return MAP_FAILED;
    }
    for (size_t got = 0; got < len;) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            free(p);
            return MAP_FAILED;
        }
        got += (size_t)n;
    }
    return p;
}

static inline int munmap(void *addr, size_t len) {
    (void)len;
    free(addr);
    return 0;
}
