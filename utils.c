#include "utils.h"
#include <stdlib.h>
#include <unistd.h>

int extend_buf(char **buf, int buf_size, int new_buf_size) {
    if (buf_size >= new_buf_size) {
        return buf_size;
    }

    // avoid `buf_size` becomes zero and goes into infinite loop
    int count = 0;
    while (count < sizeof(int) && buf_size < new_buf_size) {
        buf_size <<= 1;
        count++;
    }
    buf_size = new_buf_size > buf_size ? new_buf_size : buf_size;

    free(*buf);
    *buf = (char *)malloc(sizeof(char) * (buf_size + 1));
    return buf_size;
}

ssize_t bulk_read(int fd, void *buf, size_t len) {
    ssize_t read_len = 0;
    while (len > 0) {
        ssize_t ret = read(fd, buf, len);
        if (ret == 0 || ret == -1) {
            break;
        }
        len -= ret;
        buf = (char *)buf + ret;
        read_len += ret;
    }
    return read_len;
}

ssize_t bulk_write(int fd, void const *buf, size_t len) {
    ssize_t write_len = 0;
    while (len > 0) {
        ssize_t ret = write(fd, buf, len);
        if (ret == 0 || ret == -1) {
            break;
        }
        len -= ret;
        buf = (char *)buf + ret;
        write_len += ret;
    }
    return write_len;
}
