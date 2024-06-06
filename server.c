// TODO: only touch my own file?
// TODO: function too long
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <stdbool.h>
#include "json.h"
#include "arg_parser.h"
#include "utils.h"

typedef struct {
    int port;
    char *work_dir;
} config_t;

config_t config;

void load_config_default() {
    int const PORT = 52124;
    char const *WORK_DIR = ".";

    config.port = PORT;
    config.work_dir = (char *)malloc(sizeof(char) * (strlen(WORK_DIR) + 1));
    strcpy(config.work_dir, WORK_DIR);
}

void load_config_file(char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ERR_LOG("open config file failed");
        return;
    }

    // get file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        ERR_LOG("get config file status failed");
        close(fd);
        return;
    }

    // read whole file
    char *buf = malloc(sizeof(char) * (st.st_size + 1));
    int len = bulk_read(fd, buf, st.st_size);
    close(fd);
    if (len != st.st_size) {
        if (len == -1) {
            ERR_LOG("read config file failed");
        }
        free(buf);
        return;
    }
    buf[len] = 0;

    // convert to json
    json_data *json = json_parse(buf);
    free(buf);

    // get config
    json_data *sub_json;
    sub_json = json_obj_get(json, "port");
    if (sub_json) {
        config.port = (int)json_num_get(sub_json);
    }
    sub_json = json_obj_get(json, "workDir");
    if (sub_json) {
        free(config.work_dir);
        config.work_dir = json_str_get(sub_json);
    }

    json_kill(json);
}

void load_config_arg(int argc, char **argv) {
    arg_parser *arg = arg_init();
    arg_register(arg, "-p", "port", ARG_INT);
    arg_register(arg, "-d", "working directory", ARG_STRING);
    arg_parse(arg, argc, argv);

    arg_get(arg, "-p", &config.port);
    if (arg_is_parsed(arg, "-d")) {
        free(config.work_dir);
        arg_get(arg, "-d", &config.work_dir);
    }

    arg_kill(arg);
}

void load_config(int argc, char **argv) {
    char const *FILE_PATH = "./server_config.json";

    // config priority:
    // arg > file > default
    load_config_default();
    load_config_file((char *)FILE_PATH);
    load_config_arg(argc, argv);
}

void kill_config() {
    free(config.work_dir);
}

int init_socket(int port) {
    int const LISTEN_BACKLOG = 5;

    // socket
    int sock_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        ERR_EXIT("socket");
    }
    int option_value = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(int)) == -1) {
        ERR_EXIT("setsockopt");
    }

    // bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        ERR_EXIT("bind");
    }

    // listen
    if (listen(sock_fd, LISTEN_BACKLOG) == -1) {
        ERR_EXIT("listen");
    }

    return sock_fd;
}

// traverse current working directory and store result in `*info`
// return 0 when success, -1 when error
int traverse(char *name, json_data **info) {
    struct stat st;
    if (stat(".", &st) == -1) {
        char *cwd = getcwd(NULL, 0);
        ERR_PID_LOG("get %s status failed", cwd);
        free(cwd);
        return -1;
    }

    *info = json_obj_init();
    json_obj_set(*info, "name", json_str_init(name));
    json_obj_set(*info, "type", json_str_init("directory"));
    json_obj_set(*info, "entries", json_arr_init());
    json_obj_set(*info, "updateTime", json_num_init((double)st.st_mtime));

    // read directory
    DIR *dirp = opendir(".");
    if (!dirp) {
        char *cwd = getcwd(NULL, 0);
        ERR_PID_LOG("open directory %s failed", cwd);
        free(cwd);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        json_data *sub_info = NULL;

        if (entry->d_type == DT_REG) {
            if (stat(entry->d_name, &st) == -1) {
                ERR_PID_LOG("get %s status failed", entry->d_name);
                closedir(dirp);
                return -1;
            }

            sub_info = json_obj_init();
            json_obj_set(sub_info, "name", json_str_init(entry->d_name));
            json_obj_set(sub_info, "type", json_str_init("file"));
            json_obj_set(sub_info, "updateTime", json_num_init((double)st.st_mtime));
        }

        else if (entry->d_type == DT_DIR) {
            char *cwd = getcwd(NULL, 0);
            if (chdir(entry->d_name) == -1) {
                ERR_PID_LOG("change working directory to %s failed", entry->d_name);
                free(cwd);
                closedir(dirp);
                return -1;
            }

            if (traverse(entry->d_name, &sub_info) == -1) {
                if (sub_info) {
                    json_kill(sub_info);
                }
                free(cwd);
                closedir(dirp);
                return -1;
            }

            if (chdir(cwd) == -1) {
                ERR_PID_LOG("change working directory to %s failed", cwd);
                json_kill(sub_info);
                free(cwd);
                closedir(dirp);
                return -1;
            }
            free(cwd);
        }

        if (sub_info) {
            json_arr_append(json_obj_get(*info, "entries"), sub_info);
        }
    }

    closedir(dirp);

    return 0;
}

bool is_valid_request_path(char *path) {
    // ".." is prohibited
    for (int i = 0; path[i]; i++) {
        if (path[i] == '.' && path[i + 1] == '.') {
            return false;
        }
    }

    return true;
}

// return 0 when success, -1 when error
int respond_info(int conn_fd, char **buf, long long *buf_size) {
    // get requested path
    long long message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(long long)) != sizeof(long long)) {
        ERR_PID_LOG("receive info request failed");
        return -1;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERR_PID_LOG("receive info request failed");
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("received %s info request (pid %d)\n", *buf, getpid());

    if (!is_valid_request_path(*buf)) {
        fprintf(stderr, "error: invalid request path %s (pid %d)\n", *buf, getpid());
        return -1;
    }

    // add "./" in front of the path
    *buf_size = extend_buf(buf, *buf_size, strlen(*buf) + 2);
    memmove(*buf + 2, *buf, sizeof(char) * (strlen(*buf) + 1));
    memcpy(*buf, "./", sizeof(char) * 2);

    // traverse
    // change directory outside of `traverse` because in this way,
    // we don't need to release `cwd` when there's error in `traverse`
    char *cwd = getcwd(NULL, 0);
    if (chdir(*buf) == -1) {
        ERR_PID_LOG("change working directory to %s failed", *buf);
        free(cwd);
        return -1;
    }

    json_data *info = NULL;
    if (traverse(".", &info) == -1) {
        if (info) {
            json_kill(info);
        }
        free(cwd);
        return -1;
    }

    if (chdir(cwd) == -1) {
        ERR_PID_LOG("change working directory to %s failed", cwd);
        json_kill(info);
        free(cwd);
        return -1;
    }
    free(cwd);

    // send to client
    char *info_str = json_to_str(info, false);
    json_kill(info);

    char *path = (char *)malloc(sizeof(char) * (strlen(*buf) + 1));
    strcpy(path, *buf);

    *buf_size = extend_buf(buf, *buf_size, sizeof(long long));
    ((long long *)*buf)[0] = my_htonll(strlen(info_str));
    if (bulk_write(conn_fd, *buf, sizeof(long long)) != sizeof(long long)) {
        ERR_PID_LOG("respond %s info failed", path);
        free(info_str);
        free(path);
        return -1;
    }
    if (bulk_write(conn_fd, info_str, strlen(info_str)) != strlen(info_str)) {
        ERR_PID_LOG("respond %s info failed", path);
        free(info_str);
        free(path);
        return -1;
    }
    printf("responded %s info (%lu bytes) (pid %d)\n", path, strlen(info_str), getpid());

    free(info_str);
    free(path);

    return 0;
}

// return 0 when success, -1 when error
int respond_content(int conn_fd, char **buf, long long *buf_size) {
    int const BLOCK_SIZE = 4096;

    // get requested path
    long long message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(long long)) != sizeof(long long)) {
        ERR_PID_LOG("receive content request failed");
        return -1;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERR_PID_LOG("receive content request failed");
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("received %s content request (pid %d)\n", *buf, getpid());

    if (!is_valid_request_path(*buf)) {
        fprintf(stderr, "error: invalid request path %s (pid %d)\n", *buf, getpid());
        return -1;
    }

    // add "./" in front of the path
    *buf_size = extend_buf(buf, *buf_size, strlen(*buf) + 2);
    memmove(*buf + 2, *buf, sizeof(char) * (strlen(*buf) + 1));
    memcpy(*buf, "./", sizeof(char) * 2);

    // open file and get file size
    int file_fd = open(*buf, O_RDONLY);
    if (file_fd == -1) {
        ERR_PID_LOG("open %s failed", *buf);
        return -1;
    }

    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        ERR_PID_LOG("get %s status failed", *buf);
        close(file_fd);
        return -1;
    }

    // send file length
    char *path = (char *)malloc(sizeof(char) * (strlen(*buf) + 1));
    strcpy(path, *buf);

    *buf_size = extend_buf(buf, *buf_size, sizeof(long long));
    ((long long *)*buf)[0] = my_htonll(st.st_size);
    if (bulk_write(conn_fd, *buf, sizeof(long long)) != sizeof(long long)) {
        ERR_PID_LOG("respond %s content failed", path);
        free(path);
        close(file_fd);
        return -1;
    }

    // read file and send to client
    *buf_size = extend_buf(buf, *buf_size, BLOCK_SIZE);
    long long send_len = 0;
    while (send_len < st.st_size) {
        // read file
        int len = bulk_read(file_fd, *buf, BLOCK_SIZE);
        if (len == 0) {
            fprintf(stderr, "warning: unexpected EOF when reading %s (pid %d)\n", path, getpid());
        }
        if (len == 0 || len == -1) {
            ERR_PID_LOG("read %s failed", path);
            free(path);
            close(file_fd);
            return -1;
        }

        // send to client
        if (bulk_write(conn_fd, *buf, len) != len) {
            ERR_PID_LOG("respond %s content failed", path);
            free(path);
            close(file_fd);
            return -1;
        }
        send_len += len;
    }
    printf("responded %s content (%lld bytes) (pid %d)\n", path, send_len, getpid());

    free(path);
    close(file_fd);

    return 0;
}

void communicate(int conn_fd) {
    long long const INIT_BUF_SIZE = 128;

    long long buf_size = INIT_BUF_SIZE;
    char *buf = (char *)malloc(sizeof(char) * (buf_size + 1));

    int command;
    while (1) {
        if (bulk_read(conn_fd, &command, sizeof(int)) != sizeof(int)) {
            goto finish;
        }
        command = ntohl(command);

        switch (command) {
        case 0:
        {
            printf("received command: request info (pid %d)\n", getpid());
            if (respond_info(conn_fd, &buf, &buf_size) == -1) {
                goto finish;
            }
            break;
        }
        case 1:
        {
            printf("received command: request content (pid %d)\n", getpid());
            if (respond_content(conn_fd, &buf, &buf_size) == -1) {
                goto finish;
            }
            break;
        }
        default:
            goto finish;
        }
    }

finish:
    free(buf);
}

int main(int argc, char **argv) {
    load_config(argc, argv);
    printf("config:\n  port = %d\n  working directory = %s\n", config.port, config.work_dir);

    if (chdir(config.work_dir) == -1) {
        ERR_EXIT("chdir");
    }
    char *cwd = getcwd(NULL, 0);
    printf("working at %s\n", cwd);
    free(cwd);

    int sock_fd = init_socket(config.port);
    printf("listening on port %d\n", config.port);

    while (1) {
        // accept connection
        struct sockaddr_in addr;
        socklen_t addr_size = sizeof(addr);
        int conn_fd = accept(sock_fd, (struct sockaddr *)&addr, &addr_size);
        if (conn_fd == -1) {
            ERR_LOG("accept");
            continue;
        }

        // use multiprocess because we need to change working directory
        int pid = fork();
        if (pid == -1) {
            ERR_LOG("fork");
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            // child
            close(sock_fd);

            printf("connected from %s:%d (pid %d)\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), getpid());
            communicate(conn_fd);

            close(conn_fd);
            printf("disconnect (pid %d)\n", getpid());
            break;
        }

        close(conn_fd);
    }

    close(sock_fd);
    kill_config();

    return 0;
}
