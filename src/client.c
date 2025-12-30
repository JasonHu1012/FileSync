// TODO: record error and summarize at last
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
#include <libgen.h>
#include "json.h"
#include "list.h"
#include "utils.h"
#include "client_config.h"

volatile bool raised_sigint = false;

void handler_sigint(int signum) {
    raised_sigint = true;

    printf("received SIGINT, will terminate after updating current file\n");
    printf("use ^\\ to terminate forcibly, but current file may be incomplete\n");
    printf("and can't be updated with re-execution\n");
}

// return socket fd when success, -1 when error
int init_socket(char *host, int port) {
    // socket
    int conn_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (conn_fd == -1) {
        ERROR("socket");
        return -1;
    }

    // connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
        ERROR("invalid host %s", host);
        return -1;
    };

    if (connect(conn_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        ERROR("connect to %s:%d failed", host, port);
        WARN("is server alive?");
        return -1;
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
        ERROR("request %s info failed", path);
        return -1;
    }
    INFO("requested %s info", path);

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
    INFO("received %s info (%" PRIu64 " bytes)", path, message_len);

    // convert result to json
    *info = json_parse(*buf);

    return 0;

receive_fail:
    ERROR("receive %s info failed", path);
    return -1;
}

// add permission to directory of given file
// original permission will be saved in `opermission`
// return 0 when success, -1 when error
int add_dir_permission(char *path, mode_t permission, mode_t *opermission) {
    // dirname may modify content, so copy `path`
    char *path_copy = (char *)malloc(sizeof(char) * (strlen(path) + 1));
    strcpy(path_copy, path);
    char *dir = dirname(path_copy);
    free(path_copy);

    struct stat st;
    if (stat(dir, &st) == -1) {
        ERROR("get %s status failed", dir);
        return -1;
    }
    if (chmod(dir, st.st_mode | permission) == -1) {
        ERROR("change %s mode failed", dir);
        return -1;
    }

    *opermission = st.st_mode;
    return 0;
}

// set permission to directory of given file
// return 0 when success, -1 when error
int set_dir_permission(char *path, mode_t permission) {
    // dirname may modify content, so copy `path`
    char *path_copy = (char *)malloc(sizeof(char) * (strlen(path) + 1));
    strcpy(path_copy, path);
    char *dir = dirname(path_copy);
    free(path_copy);

    if (chmod(dir, permission) == -1) {
        ERROR("change %s mode failed", dir);
        return -1;
    }

    return 0;
}

// request "{remote_dir}/{path}" content
// received content will be written to file "{path}", which must exist
// file mtime will be set to `modify_time`
// return 0 when success, -1 when error
int request_content(int conn_fd, char *path, time_t modify_time, char **buf, uint64_t *buf_size) {
    int const BLOCK_SIZE = 4096;

    // open file
    int file_fd;
    // add write permission
    struct stat st;
    bool stat_success = true;
    if (stat(path, &st) == -1) {
        ERROR("get %s status failed", path);
        stat_success = false;
    }
    if (stat_success && chmod(path, st.st_mode | 0200) == -1) {
        ERROR("change %s mode failed", path);
    }

    file_fd = open(path, O_WRONLY | O_TRUNC);
    if (file_fd == -1) {
        ERROR("open %s failed", path);
        return -1;
    }

    // reset permission
    if (stat_success && chmod(path, st.st_mode) == -1) {
        ERROR("reset %s mode failed", path);
    }

    // send [1][path length][path]
    uint64_t message_len = sizeof(uint32_t) + sizeof(uint64_t) + strlen(config.remote_dir) + 1 + strlen(path);
    *buf_size = extend_buf(buf, *buf_size, message_len);
    message_len = append_buf_uint32(*buf, 0, htonl(1));
    message_len = append_buf_uint64(*buf, message_len, my_htonll(strlen(config.remote_dir) + 1 + strlen(path)));
    message_len = append_buf_charp(*buf, message_len, config.remote_dir);
    message_len = append_buf_charp(*buf, message_len, "/");
    message_len = append_buf_charp(*buf, message_len, path);

    if (bulk_write(conn_fd, *buf, message_len) != message_len) {
        ERROR("request %s/%s content failed", config.remote_dir, path);
        close(file_fd);
        return -1;
    }
    INFO("requested %s/%s content", config.remote_dir, path);

    // get requested content length
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("receive %s/%s content failed", config.remote_dir, path);
        close(file_fd);
        return -1;
    }
    message_len = my_ntohll(message_len);
    INFO("receiving %s/%s content", config.remote_dir, path);

    // get content and write to file
    *buf_size = extend_buf(buf, *buf_size, BLOCK_SIZE);
    uint64_t receive_len = 0;
    while (receive_len < message_len) {
        // get content
        int len = bulk_read(conn_fd, *buf, MIN(BLOCK_SIZE, message_len - receive_len));
        if (len == 0 || len == -1) {
            ERROR("receive %s/%s content failed", config.remote_dir, path);
            close(file_fd);
            return -1;
        }
        receive_len += len;

        // write to file
        if (bulk_write(file_fd, *buf, len) != len) {
            ERROR("write %s/%s content to file failed", config.remote_dir, path);
            close(file_fd);
            return -1;
        }
    }

    // make mtime equal for bidirectional sync
    if (fstat(file_fd, &st) == -1) {
        ERROR("get %s status failed", path);
    }
    else {
        struct timeval tv[2] = { 0 };
        // remain atime
        tv[0].tv_sec = st.st_atime;
        // set mtime equal to server file
        tv[1].tv_sec = modify_time;
        if (futimes(file_fd, tv) == -1) {
            ERROR("set %s mtime failed", path);
        }
    }
    INFO("synced %s (%" PRIu64 " bytes)", path, receive_len);

    close(file_fd);

    return 0;
}

// `prefix` indicates "./" if it's NULL
// the directory "{prefix}" must exist
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
            if (access(path, F_OK) == -1) {
                // file doesn't exist, create it and request content
                // add write permission to directory
                mode_t opermission;
                bool add_permission_success = true;
                if (add_dir_permission(path, 0200, &opermission) == -1) {
                    add_permission_success = false;
                }

                int file_fd = open(path, O_CREAT | O_EXCL, (mode_t)json_num_get(json_obj_get(sub_info, "permission")));
                if (file_fd == -1) {
                    ERROR("create %s failed", path);
                    goto finish_current;
                }
                close(file_fd);
                INFO("created %s", path);

                // reset directory permission
                if (add_permission_success) {
                    set_dir_permission(path, opermission);
                }

                request_content(conn_fd, path, (time_t)json_num_get(json_obj_get(sub_info, "updateTime")), buf, buf_size);
                goto finish_current;
            }

            // compare update time
            struct stat st;
            if (stat(path, &st) == -1) {
                ERROR("get %s status failed", path);
                goto finish_current;
            }
            time_t update_time = (time_t)json_num_get(json_obj_get(sub_info, "updateTime"));
            if (st.st_mtime < update_time) {
                // local file is out of date, request content
                request_content(conn_fd, path, (time_t)json_num_get(json_obj_get(sub_info, "updateTime")), buf, buf_size);
            }
        }

        else if (!strcmp(type, "directory")) {
            if (access(path, F_OK) == -1) {
                // the directory doesn't exist, create it
                // add write permission to parent directory
                mode_t opermission;
                bool add_permission_success = true;
                if (add_dir_permission(path, 0200, &opermission) == -1) {
                    add_permission_success = false;
                }

                if (mkdir(path, (mode_t)json_num_get(json_obj_get(sub_info, "permission"))) == -1) {
                    ERROR("create directory %s failed", path);
                    goto finish_current;
                }
                INFO("created directory %s", path);

                // reset parent directory permission
                if (add_permission_success) {
                    set_dir_permission(path, opermission);
                }
            }

            // unlike server, client doesn't chdir because client must request content with full path
            traverse(conn_fd, sub_info, path, buf, buf_size);
        }

        else {
            WARN("unknown type %s of file %s", type, path);
        }

finish_current:
        free(type);
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
        ERROR("send exit message failed");
        return -1;
    }
    INFO("sent exit message");

    return 0;
}

// return 0 when success, -1 when error
int request_working_dir(int conn_fd, char **buf, uint64_t *buf_size) {
    // send [3]
    *buf_size = extend_buf(buf, *buf_size, sizeof(uint32_t));
    append_buf_uint32(*buf, 0, htonl(3));
    if (bulk_write(conn_fd, *buf, sizeof(uint32_t)) != sizeof(uint32_t)) {
        ERROR("request working directory failed");
        return -1;
    }
    INFO("requested working directory");

    // get requested result
    uint64_t message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("receive working directory failed");
        return -1;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERROR("receive working directory failed");
        return -1;
    }
    (*buf)[message_len] = 0;
    INFO("server is working at %s", *buf);

    return 0;
}

void communicate(int conn_fd) {
    uint64_t const INIT_BUF_SIZE = 128;

    uint64_t buf_size = INIT_BUF_SIZE;
    // `buf_size` doesn't include the terminating '\0', so + 1
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

// create intermediate directory as required
// return 0 when success, -1 when error
int mkdir_full(char *path, mode_t mode) {
    list *names = lst_init(sizeof(char *));
    int start = 0;
    int len = strlen(path);
    for (int i = 0; i < len; i++) {
        if (path[i] != '/') {
            continue;
        }

        if (start != i) {
            char *name = (char *)malloc(sizeof(char) * (i - start + 1));
            strncpy(name, path + start, i - start + 1);
            lst_append(names, &name);
        }
        start = i + 1;
    }
    if (start != len) {
        char *name = (char *)malloc(sizeof(char) * (len - start + 1));
        strcpy(name, path + start);
        lst_append(names, &name);
    }

    // open "/" if path is absolute path
    int dir_fd = open(path[0] == '/' ? "/" : ".", O_SEARCH);
    if (dir_fd == -1) {
        ERROR("open directory %s failed", path[0] == '/' ? "/" : ".");
        lst_kill_f(names, free);
        return -1;
    }

    for (int i = 0; i < lst_size(names); i++) {
        char *name;
        lst_get(names, i, &name);

        if (faccessat(dir_fd, name, F_OK, AT_EACCESS) == -1) {
            // directory doesn't exist, create it
            if (mkdirat(dir_fd, name, mode) == -1) {
                ERROR("create directory %s failed", name);
                lst_kill_f(names, free);
                close(dir_fd);
                return -1;
            }
        }

        int last_dir_fd = dir_fd;
        dir_fd = openat(last_dir_fd, name, O_SEARCH);
        close(last_dir_fd);
        if (dir_fd == -1) {
            ERROR("open directory %s failed", name);
            lst_kill_f(names, free);
            return -1;
        }
    }
    close(dir_fd);
    lst_kill_f(names, free);

    return 0;
}

int main(int argc, char **argv) {
    load_config(argc, argv);
    if (!is_valid_config()) {
        kill_config();
        return 1;
    }
    printf("config:\n  host = %s\n  port = %d\n  remote directory = %s\n  local directory = %s\n\n",
        config.host, config.port, config.remote_dir, config.local_dir);

    if (access(config.local_dir, F_OK) == -1) {
        // local directory doesn't exist, create it
        if (mkdir_full(config.local_dir, 0777) == 0) {
            INFO("created directory %s", config.local_dir);
        }
    }

    if (chdir(config.local_dir) == -1) {
        ERROR("change working directory to %s failed", config.local_dir);
        kill_config();
        return 1;
    }
    char *cwd = getcwd(NULL, 0);
    INFO("syncing to local directory %s", cwd);
    free(cwd);

    int conn_fd = init_socket(config.host, config.port);
    if (conn_fd == -1) {
        kill_config();
        return 1;
    }
    INFO("connected to %s:%d", config.host, config.port);

    communicate(conn_fd);

    close(conn_fd);
    INFO("disconnected");

    kill_config();

    return 0;
}
