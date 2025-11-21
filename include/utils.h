#ifndef _UTILS_H
#define _UTILS_H

#include <sys/types.h>
#include <stdint.h>
#include <sys/errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

// use these when errno is set
#define ERR_EXIT(...) do {fprintf(stderr, "fatal: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, ": %s\n", strerror(errno)); exit(1);} while (0)
#define ERR_LOG(...) do {fprintf(stderr, "error: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, ": %s\n", strerror(errno));} while (0)
#define ERR_PID_LOG(...) do {fprintf(stderr, "error: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, ": %s (pid %d)\n", strerror(errno), getpid());} while (0)

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// extend `*buf` to at least `new_buf_size` long, data may be cleared
// `new_buf_size` doesn't include the terminating '\0'
// return extended buffer size
uint64_t extend_buf(char **buf, uint64_t buf_size, uint64_t new_buf_size);

// append `data` to `buf[offset]`
// `buf` should be long enough
// return valid buffer length after appending
uint64_t append_buf_uint32(char *buf, uint64_t offset, uint32_t data);
uint64_t append_buf_uint64(char *buf, uint64_t offset, uint64_t data);
uint64_t append_buf_charp(char *buf, uint64_t offset, char *data);

// use loop to make sure all data is read / written
ssize_t bulk_read(int fd, void *buf, size_t len);
ssize_t bulk_write(int fd, void const *buf, size_t len);

// ntohll and htonll are only in macOS
uint64_t my_ntohll(uint64_t n);
uint64_t my_htonll(uint64_t n);

#endif
