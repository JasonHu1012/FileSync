#include "utils.h"
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>

long long extend_buf(char **buf, long long buf_size, long long new_buf_size) {
    if (buf_size >= new_buf_size) {
        return buf_size;
    }

    // avoid `buf_size` becomes zero and goes into infinite loop
    int count = 0;
    while (count < sizeof(long long) && buf_size < new_buf_size) {
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
        if (ret == -1 && read_len == 0) {
            return -1;
        }
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
        if (ret == -1 && write_len == 0) {
            return -1;
        }
        if (ret == 0 || ret == -1) {
            break;
        }
        len -= ret;
        buf = (char *)buf + ret;
        write_len += ret;
    }
    return write_len;
}

uint64_t my_ntohll(uint64_t n) {
    // don't need to consider (un)signed problem
    if (ntohl(2) == 2) {
        return n;
    }

    uint8_t na[8];
    memcpy(na, &n, sizeof(uint64_t));
    for (int i = 0; i < 4; i++) {
        na[i] ^= na[7 - i];
        na[7 - i] ^= na[i];
        na[i] ^= na[7 - i];
    }
    memcpy(&n, na, sizeof(uint64_t));

    return n;
}

uint64_t my_htonll(uint64_t n) {
    // hton and ntoh are actually the same
    return my_ntohll(n);
}
