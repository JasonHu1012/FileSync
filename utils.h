#ifndef _UTILS_H
#define _UTILS_H

#include <sys/types.h>

// extend `*buf` to at least `new_buf_size` long
// return extended buffer size
int extend_buf(char **buf, int buf_size, int new_buf_size);

// use loop to make sure all data is read
ssize_t bulk_read(int fd, void *buf, size_t len);

// use loop to make sure all data is written
ssize_t bulk_write(int fd, void const *buf, size_t len);

#endif
