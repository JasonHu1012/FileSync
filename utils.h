#ifndef _UTILS_H
#define _UTILS_H

#include <sys/types.h>
#include <stdint.h>

#define ERR_EXIT(s) fprintf(stderr, "fatal: %s: %s\n", s, strerror(errno)); exit(1)
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
