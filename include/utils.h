#ifndef _UTILS_H
#define _UTILS_H

#include <sys/types.h>
#include <stdint.h>
#include <sys/errno.h>
#include <unistd.h>

// use these when errno is set
#define ERR_EXIT(s) do {fprintf(stderr, "fatal: %s: %s\n", s, strerror(errno)); exit(1);} while (0)
#define ERR_LOG(...) do {fprintf(stderr, __VA_ARGS__); fprintf(stderr, ": %s\n", strerror(errno));} while (0)
#define ERR_PID_LOG(...) do {fprintf(stderr, __VA_ARGS__); fprintf(stderr, ": %s (pid %d)\n", strerror(errno), getpid());} while (0)

#define MIN(a, b) (a) < (b) ? (a) : (b)

// extend `*buf` to at least `new_buf_size` long
// return extended buffer size
long long extend_buf(char **buf, long long buf_size, long long new_buf_size);

// use loop to make sure all data is read / written
ssize_t bulk_read(int fd, void *buf, size_t len);
ssize_t bulk_write(int fd, void const *buf, size_t len);

// ntohll and htonll are only in macOS
uint64_t my_ntohll(uint64_t n);
uint64_t my_htonll(uint64_t n);

#endif
