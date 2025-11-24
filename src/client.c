#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <signal.h>
#include <inttypes.h>
#include "json.h"
#include "utils.h"
#include "client_config.h"

bool raised_sigint = false;

void handler_sigint(int signum) {
    raised_sigint = true;

    printf("received SIGINT, will terminate after updating current file\n");
    printf("use ^\\ to terminate forcibly, but current file may be incomplete\n");
    printf("and can't be updated with re-execution\n");
}

int init_socket(char *host, int port) {
    // socket
    int conn_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (conn_fd == -1) {
        ERR_EXIT("socket");
    }

    // connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
        fprintf(stderr, "fatal: invalid host %s\n", host);
        exit(1);
    };

    if (connect(conn_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "warning: is server alive?\n");
        ERR_EXIT("connect to %s:%d failed", host, port);
    }

    return conn_fd;
}

// info is stored in `*info`
// return 0 when success, -1 when error
int request_info(int conn_fd, char *path, json_data **info, char **buf, uint64_t *buf_size) {
    // send [0][path length][path]
    uint64_t message_len = sizeof(uint32_t) + sizeof(uint64_t) + strlen(path);
    *buf_size = extend_buf(buf, *buf_size, message_len);
    message_len = append_buf_uint32(*buf, 0, htonl(0));
    message_len = append_buf_uint64(*buf, message_len, my_htonll(strlen(path)));
    message_len = append_buf_charp(*buf, message_len, path);

    if (bulk_write(conn_fd, *buf, message_len) != message_len) {
        ERR_LOG("request %s info failed", path);
        return -1;
    }
    printf("requested %s info\n", path);

    // get requested result
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        goto receive_fail;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        goto receive_fail;
    }
    (*buf)[message_len] = 0;
    printf("received %s info (%" PRIu64 " bytes)\n", path, message_len);

    // convert result to json
    *info = json_parse(*buf);

    return 0;

receive_fail:
    ERR_LOG("receive %s info failed", path);
    return -1;
}

// request "{remote_dir}/{path}" content
// received content will be written to file
// return 0 when success, -1 when error
int request_content(int conn_fd, char *path, mode_t permission, time_t modify_time, char **buf, uint64_t *buf_size) {
    int const BLOCK_SIZE = 4096;

    // send [1][path length][path]
    uint64_t message_len = sizeof(uint32_t) + sizeof(uint64_t) + strlen(config.remote_dir) + 1 + strlen(path);
    *buf_size = extend_buf(buf, *buf_size, message_len);
    message_len = append_buf_uint32(*buf, 0, htonl(1));
    message_len = append_buf_uint64(*buf, message_len, my_htonll(strlen(config.remote_dir) + 1 + strlen(path)));
    message_len = append_buf_charp(*buf, message_len, config.remote_dir);
    message_len = append_buf_charp(*buf, message_len, "/");
    message_len = append_buf_charp(*buf, message_len, path);

    if (bulk_write(conn_fd, *buf, message_len) != message_len) {
        ERR_LOG("request %s/%s content failed", config.remote_dir, path);
        return -1;
    }
    printf("requested %s/%s content\n", config.remote_dir, path);

    // get requested content length
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERR_LOG("receive %s/%s content failed", config.remote_dir, path);
        return -1;
    }
    message_len = my_ntohll(message_len);

    // open file
    int file_fd;
    if (access(path, F_OK) == 0) {
        // file exists
        // add write permission
        struct stat st;
        if (stat(path, &st) == -1) {
            ERR_LOG("get %s status failed", path);
            return -1;
        }
        if (chmod(path, st.st_mode | 0200) == -1) {
            ERR_LOG("change %s mode failed", path);
            return -1;
        }

        file_fd = open(path, O_WRONLY | O_TRUNC);
        if (file_fd == -1) {
            ERR_LOG("open %s failed", path);
            return -1;
        }

        // reset permission
        if (chmod(path, st.st_mode) == -1) {
            ERR_LOG("reset %s mode failed", path);
            close(file_fd);
            return -1;
        }
    }
    else {
        // file doesn't exist, create it
        file_fd = open(path, O_WRONLY | O_CREAT | O_EXCL, permission);
        if (file_fd == -1) {
            ERR_LOG("open %s failed", path);
            return -1;
        }
    }

    // get content and write to file
    *buf_size = extend_buf(buf, *buf_size, BLOCK_SIZE);
    uint64_t receive_len = 0;
    while (receive_len < message_len) {
        // get content
        int len = bulk_read(conn_fd, *buf, MIN(BLOCK_SIZE, message_len - receive_len));
        if (len == 0 || len == -1) {
            ERR_LOG("receive %s/%s content failed", config.remote_dir, path);
            close(file_fd);
            return -1;
        }
        receive_len += len;

        // write to file
        if (bulk_write(file_fd, *buf, len) != len) {
            ERR_LOG("write %s/%s content to file failed", config.remote_dir, path);
            close(file_fd);
            return -1;
        }
    }

    // make mtime equal for bidirectional sync
    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        ERR_LOG("get %s status failed", path);
        return -1;
    }
    struct timeval tv[2] = { 0 };
    tv[0].tv_sec = st.st_atime;
    tv[1].tv_sec = modify_time;
    if (futimes(file_fd, tv) == -1) {
        ERR_LOG("set %s mtime failed", path);
        return -1;
    }
    printf("synced %s/%s (%" PRIu64 " bytes)\n", config.remote_dir, path, receive_len);

    close(file_fd);

    return 0;
}

// return 0 when success, -1 when error
int traverse(int conn_fd, json_data *info, char *prefix, char **buf, uint64_t *buf_size) {
    // handle sigint
    struct sigaction act_sigint;
    struct sigaction oact_sigint;
    act_sigint.sa_handler = handler_sigint;
    sigemptyset(&act_sigint.sa_mask);
    act_sigint.sa_flags = SA_RESTART;
    sigaction(SIGINT, &act_sigint, &oact_sigint);

    json_data *entries = json_obj_get(info, "entries");
    int entries_size = json_arr_size(entries);
    for (int i = 0; i < entries_size; i++) {
        json_data *sub_info = json_arr_get(entries, i);

        char *name = json_str_get(json_obj_get(sub_info, "name"));
        char *path;
        if (prefix) {
            path = (char *)malloc(sizeof(char) * (strlen(prefix) + strlen(name) + 2));
            sprintf(path, "%s/%s", prefix, name);
        }
        else {
            path = (char *)malloc(sizeof(char) * (strlen(name) + 1));
            strcpy(path, name);
        }
        free(name);

        char *type = json_str_get(json_obj_get(sub_info, "type"));
        if (!strcmp(type, "file")) {
            free(type);

            struct stat st;
            if (stat(path, &st) == -1) {
                if (errno == ENOENT) {
                    // the file doesn't exist, request content
                    if (request_content(conn_fd, path, (mode_t)json_num_get(json_obj_get(sub_info, "permission")), (time_t)json_num_get(json_obj_get(sub_info, "updateTime")), buf, buf_size) == -1) {
                        free(path);
                        return -1;
                    }
                }
                else {
                    ERR_LOG("get %s status failed", path);
                    free(path);
                    return -1;
                }
            }

            else {
                // compare update time
                time_t update_time = (time_t)json_num_get(json_obj_get(sub_info, "updateTime"));
                if (st.st_mtime < update_time) {
                    // local file is out of date, request content
                    if (request_content(conn_fd, path, (mode_t)json_num_get(json_obj_get(sub_info, "permission")), (time_t)json_num_get(json_obj_get(sub_info, "updateTime")), buf, buf_size) == -1) {
                        free(path);
                        return -1;
                    }
                }
            }
        }

        else if (!strcmp(type, "directory")) {
            free(type);

            if (access(path, F_OK) == -1) {
                // the directory doesn't exist
                // TODO: change permission after traversing it
                if (mkdir(path, (mode_t)json_num_get(json_obj_get(sub_info, "permission"))) == -1) {
                    ERR_LOG("create directory %s failed", path);
                }
                else {
                    printf("create directory %s\n", path);
                }
            }

            if (traverse(conn_fd, sub_info, path, buf, buf_size) == -1) {
                free(path);
                return -1;
            }
        }

        else {
            fprintf(stderr, "warning: unknown type %s\n", type);
            free(type);
        }

        free(path);

        // check whether sigint was raised when updating files
        if (raised_sigint) {
            break;
        }
    }

    // restore sigint handler
    sigaction(SIGINT, &oact_sigint, NULL);

    return 0;
}

// return 0 when success, -1 when error
int send_exit(int conn_fd, char **buf, uint64_t *buf_size) {
    // send [2]
    *buf_size = extend_buf(buf, *buf_size, sizeof(uint32_t));
    append_buf_uint32(*buf, 0, htonl(2));
    if (bulk_write(conn_fd, *buf, sizeof(uint32_t)) != sizeof(uint32_t)) {
        ERR_LOG("send exit message failed");
        return -1;
    }
    printf("sent exit message\n");

    return 0;
}

// return 0 when success, -1 when error
int request_working_dir(int conn_fd, char **buf, uint64_t *buf_size) {
    // send [3]
    *buf_size = extend_buf(buf, *buf_size, sizeof(uint32_t));
    append_buf_uint32(*buf, 0, htonl(3));
    if (bulk_write(conn_fd, *buf, sizeof(uint32_t)) != sizeof(uint32_t)) {
        ERR_LOG("request working directory failed");
        return -1;
    }
    printf("requested working directory\n");

    // get requested result
    uint64_t message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERR_LOG("receive working directory failed");
        return -1;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERR_LOG("receive working directory failed");
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("server is working at %s\n", *buf);

    return 0;
}

void communicate(int conn_fd) {
    uint64_t const INIT_BUF_SIZE = 128;

    // not including the terminating '\0'
    uint64_t buf_size = INIT_BUF_SIZE;
    char *buf = (char *)malloc(sizeof(char) * (buf_size + 1));

    json_data *info = NULL;

    if (config.is_query_mode) {
        request_working_dir(conn_fd, &buf, &buf_size);
        goto finish;
    }

    if (request_info(conn_fd, config.remote_dir, &info, &buf, &buf_size) == -1) {
        goto finish;
    }

    if (traverse(conn_fd, info, NULL, &buf, &buf_size) == -1) {
        goto finish;
    }

finish:
    send_exit(conn_fd, &buf, &buf_size);

    free(buf);
    if (info) {
        json_kill(info);
    }
}

int main(int argc, char **argv) {
    load_config(argc, argv);
    validate_config();
    printf("config:\n  host = %s\n  port = %d\n  remote directory = %s\n  local directory = %s\n\n",
        config.host, config.port, config.remote_dir, config.local_dir);

    if (access(config.local_dir, F_OK) == -1) {
        // local directory doesn't exist, create it
        // TODO: mkdir a/b/c
        if (mkdir(config.local_dir, 0777) == -1) {
            ERR_LOG("create directory %s failed", config.local_dir);
        }
        else {
            printf("created directory %s\n", config.local_dir);
        }
    }

    if (chdir(config.local_dir) == -1) {
        ERR_EXIT("change working directory to %s failed", config.local_dir);
    }
    char *cwd = getcwd(NULL, 0);
    printf("sync to local directory %s\n", cwd);
    free(cwd);

    int conn_fd = init_socket(config.host, config.port);
    printf("connected to %s:%d\n", config.host, config.port);

    communicate(conn_fd);

    close(conn_fd);
    printf("disconnect\n");

    kill_config();

    return 0;
}
