// madvise(MADV_DONTNEED) on a private file mapping must leave the file's bytes
// (Linux reads them back from the file); iSH zeroed them, and Bun, which
// discards its embedded sources this way, then failed with "Invalid character
// '\0'". MADV_FREE on a file mapping is EINVAL as on Linux. Anonymous memory
// still reads as zero after either.
// Guest-only:  gcc -O1 -o madvise_file madvise_file.c && ./madvise_file    expected: "madvise_file ok"
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#define PG 4096
#define FILE_PAGES 20
#define MAP_PAGES 16

static int same_as_file(int fd, const char *p, off_t off, size_t len) {
    static char buf[MAP_PAGES * PG];
    return pread(fd, buf, len, off) == (ssize_t) len && memcmp(buf, p, len) == 0;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    int fail = 0;

    char path[] = "/tmp/madvise_fileXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    unlink(path);
    static char data[FILE_PAGES * PG];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (char) (i % 251 + 1); // never zero
    if (write(fd, data, sizeof(data)) != (ssize_t) sizeof(data)) { perror("write"); return 1; }

    // offset 4096: 4K- but not 16K-aligned, like claude.exe's RW segment
    char *p = mmap(NULL, MAP_PAGES * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, PG);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    volatile unsigned sum = 0;
    for (int i = 0; i < MAP_PAGES; i++)
        sum += p[i * PG];
    if (madvise(p + PG, 8 * PG, MADV_DONTNEED) != 0) { printf("MADV_DONTNEED failed: %s\n", strerror(errno)); fail = 1; }
    if (!same_as_file(fd, p, PG, MAP_PAGES * PG)) { printf("MADV_DONTNEED changed a private file page\n"); fail = 1; }
    // a range that isn't host-page aligned at either end
    if (madvise(p + 3 * PG, 5 * PG, MADV_DONTNEED) != 0 || !same_as_file(fd, p, PG, MAP_PAGES * PG)) {
        printf("unaligned MADV_DONTNEED changed a private file page\n"); fail = 1;
    }

    errno = 0;
    if (madvise(p, PG, MADV_FREE) != -1 || errno != EINVAL) { printf("MADV_FREE on a file mapping: not EINVAL\n"); fail = 1; }
    if (!same_as_file(fd, p, PG, MAP_PAGES * PG)) { printf("MADV_FREE changed a private file page\n"); fail = 1; }

    // anonymous memory still reads as zero afterwards
    char *a = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) return 1;
    memset(a, 0xaa, 4 * PG);
    if (madvise(a, 2 * PG, MADV_DONTNEED) != 0 || a[0] != 0 || a[2 * PG - 1] != 0 || (unsigned char) a[2 * PG] != 0xaa) {
        printf("anonymous MADV_DONTNEED wrong\n"); fail = 1;
    }
    memset(a, 0xaa, 4 * PG);
    if (madvise(a, 4 * PG, MADV_FREE) != 0) { printf("anonymous MADV_FREE failed: %s\n", strerror(errno)); fail = 1; }
    a[0] = 1;
    if (a[0] != 1) { printf("anonymous page unusable after MADV_FREE\n"); fail = 1; }

    // a discard over a huge sparse reservation (V8/JSC do this) must be quick
    size_t huge = (size_t) 64 << 30;
    char *r = mmap(NULL, huge, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (r != MAP_FAILED) {
        double t = now();
        if (madvise(r, huge, MADV_DONTNEED) != 0) { printf("MADV_DONTNEED over 64 GB failed: %s\n", strerror(errno)); fail = 1; }
        t = now() - t;
        if (t > 1.0) { printf("MADV_DONTNEED over 64 GB took %.2f s\n", t); fail = 1; }
        munmap(r, huge);
    }

    if (fail) { printf("madvise_file FAIL\n"); return 1; }
    printf("madvise_file ok\n");
    return 0;
}
