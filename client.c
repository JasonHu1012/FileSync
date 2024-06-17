// TODO: tolerate error
// TODO: program terminates when updating file
// TODO: function too long
// TODO: specify config path with arg
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include "json.h"
#include "arg_parser.h"
#include "utils.h"

typedef struct {
    int port;
    char *host;
    char *remote_dir;
    char *local_dir;
} config_t;

config_t config;

void load_config_arg(int argc, char **argv) {
    arg_parser *arg = arg_init();
    arg_register(arg, "-p", "port", ARG_INT);
    arg_register(arg, "--host", "host ip", ARG_STRING);
    arg_register(arg, "--rdir", "remote directory", ARG_STRING);
    arg_register(arg, "--ldir", "local directory", ARG_STRING);
    arg_parse(arg, argc, argv);

    if (config.port == -1) {
        arg_get(arg, "-p", &config.port);
    }
    if (config.host == NULL) {
        arg_get(arg, "--host", &config.host);
    }
    if (config.remote_dir == NULL) {
        arg_get(arg, "--rdir", &config.remote_dir);
    }
    if (config.local_dir == NULL) {
        arg_get(arg, "--ldir", &config.local_dir);
    }

    arg_kill(arg);
}

void load_config_file(char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ERR_LOG("open config file %s failed", path);
        return;
    }

    // get file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        ERR_LOG("get config file %s status failed", path);
        close(fd);
        return;
    }

    // read whole file
    char *buf = malloc(sizeof(char) * (st.st_size + 1));
    int len = bulk_read(fd, buf, st.st_size);
    close(fd);
    if (len != st.st_size) {
        if (len == -1) {
            ERR_LOG("read config file %s failed", path);
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
    if (config.port == -1 && sub_json) {
        config.port = (int)json_num_get(sub_json);
    }
    sub_json = json_obj_get(json, "host");
    if (config.host == NULL && sub_json) {
        config.host = json_str_get(sub_json);
    }
    sub_json = json_obj_get(json, "remoteDir");
    if (config.remote_dir == NULL && sub_json) {
        config.remote_dir = json_str_get(sub_json);
    }
    sub_json = json_obj_get(json, "localDir");
    if (config.local_dir == NULL && sub_json) {
        config.local_dir = json_str_get(sub_json);
    }

    json_kill(json);
}

void load_config_default() {
    int const PORT = 52124;
    char const *HOST = "127.0.0.1";
    char const *REMOTE_DIR = ".";
    char const *LOCAL_DIR = ".";

    if (config.port == -1) {
        config.port = PORT;
    }
    if (config.host == NULL) {
        config.host = (char *)malloc(sizeof(char) * (strlen(HOST) + 1));
        strcpy(config.host, HOST);
    }
    if (config.remote_dir == NULL) {
        config.remote_dir = (char *)malloc(sizeof(char) * (strlen(REMOTE_DIR) + 1));
        strcpy(config.remote_dir, REMOTE_DIR);
    }
    if (config.local_dir == NULL) {
        config.local_dir = (char *)malloc(sizeof(char) * (strlen(LOCAL_DIR) + 1));
        strcpy(config.local_dir, LOCAL_DIR);
    }
}

void load_config(int argc, char **argv) {
    char const *FILE_PATH = "./client_config.json";

    // initialize config
    config.port = -1;
    config.host = NULL;
    config.remote_dir = NULL;
    config.local_dir = NULL;

    // config priority:
    // arg > file > default
    load_config_arg(argc, argv);
    load_config_file((char *)FILE_PATH);
    load_config_default();
}

void validate_config() {
    if (config.port < 0 || config.port > 65535) {
        fprintf(stderr, "fatal: invalid port %d\n", config.port);
        exit(1);
    }

    // prohibit ".." in `remote_dir`
    for (int i = 0; config.remote_dir[i]; i++) {
        if (config.remote_dir[i] == '.' && config.remote_dir[i + 1] == '.') {
            fprintf(stderr, "fatal: .. is prohibited in remote directory path\n");
            exit(1);
        }
    }
}

void kill_config() {
    free(config.host);
    free(config.remote_dir);
    free(config.local_dir);
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
        ERR_EXIT("connect");
    }

    return conn_fd;
}

// info is stored in `*info`
// return 0 when success, -1 when error
int request_info(int conn_fd, char *path, json_data **info, char **buf, long long *buf_size) {
    // send [0][path length][path]
    *buf_size = extend_buf(buf, *buf_size, sizeof(int));
    ((int *)*buf)[0] = htonl(0);
    if (bulk_write(conn_fd, *buf, sizeof(int)) != sizeof(int)) {
        ERR_LOG("request %s info failed", path);
        return -1;
    }
    ((long long *)*buf)[0] = my_htonll(strlen(path));
    if (bulk_write(conn_fd, *buf, sizeof(long long)) != sizeof(long long)) {
        ERR_LOG("request %s info failed", path);
        return -1;
    }
    if (bulk_write(conn_fd, path, strlen(path)) != strlen(path)) {
        ERR_LOG("request %s info failed", path);
        return -1;
    }
    printf("requested %s info\n", path);

    // get requested result
    long long message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(long long)) != sizeof(long long)) {
        ERR_LOG("receive %s info failed", path);
        return -1;
    }
    message_len = my_ntohll(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERR_LOG("receive %s info failed", path);
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("received %s info (%lld bytes)\n", path, message_len);

    // convert result to json
    *info = json_parse(*buf);

    return 0;
}

// request "{remote_dir}/{path}" content
// received content will be written to file
// return 0 when success, -1 when error
int request_content(int conn_fd, char *path, mode_t permission, char **buf, long long *buf_size) {
    int const BLOCK_SIZE = 4096;

    // send [1][path length][path]
    *buf_size = extend_buf(buf, *buf_size, sizeof(int));
    ((int *)*buf)[0] = htonl(1);
    if (bulk_write(conn_fd, *buf, sizeof(int)) != sizeof(int)) {
        ERR_LOG("request %s content failed", path);
        return -1;
    }
    *buf_size = extend_buf(buf, *buf_size, sizeof(long long));
    ((long long *)*buf)[0] = my_htonll(strlen(config.remote_dir) + strlen(path) + 1);
    if (bulk_write(conn_fd, *buf, sizeof(long long)) != sizeof(long long)) {
        ERR_LOG("request %s content failed", path);
        return -1;
    }
    *buf_size = extend_buf(buf, *buf_size, strlen(config.remote_dir) + strlen(path) + 1);
    sprintf(*buf, "%s/%s", config.remote_dir, path);
    if (bulk_write(conn_fd, *buf, strlen(*buf)) != strlen(*buf)) {
        ERR_LOG("request %s content failed", path);
        return -1;
    }
    printf("requested %s content\n", *buf);

    // get requested content length
    long long message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(long long)) != sizeof(long long)) {
        ERR_LOG("receive %s content failed", path);
        return -1;
    }
    message_len = my_ntohll(message_len);

    // open file
    int file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, permission);
    if (file_fd == -1) {
        if (errno == EACCES) {
            // maybe we don't have the write permission, delete and create
            if (remove(path) == -1) {
                ERR_LOG("remove %s failed", path);
                return -1;
            }

            file_fd = open(path, O_WRONLY | O_CREAT | O_EXCL, permission);
            if (file_fd == -1) {
                ERR_LOG("open %s failed", path);
                return -1;
            }
        }
        else {
            ERR_LOG("open %s failed", path);
            return -1;
        }
    }

    // get content and write to file
    *buf_size = extend_buf(buf, *buf_size, BLOCK_SIZE);
    long long receive_len = 0;
    while (receive_len < message_len) {
        // get content
        int len = bulk_read(conn_fd, *buf, MIN(BLOCK_SIZE, message_len - receive_len));
        if (len == 0 || len == -1) {
            ERR_LOG("receive %s content failed", path);
            close(file_fd);
            return -1;
        }
        receive_len += len;

        // write to file
        if (bulk_write(file_fd, *buf, len) != len) {
            ERR_LOG("write %s content to file failed", path);
            close(file_fd);
            return -1;
        }
    }
    printf("synced %s (%lld bytes)\n", path, receive_len);

    close(file_fd);

    return 0;
}

// return 0 when success, -1 when error
int traverse(int conn_fd, json_data *info, char *prefix, char **buf, long long *buf_size) {
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
                    if (request_content(conn_fd, path, (mode_t)json_num_get(json_obj_get(sub_info, "permission")), buf, buf_size) == -1) {
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
                long long update_time = (long long)json_num_get(json_obj_get(sub_info, "updateTime"));
                if (st.st_mtime < update_time) {
                    // local file is out of date, request content
                    if (request_content(conn_fd, path, (mode_t)json_num_get(json_obj_get(sub_info, "permission")), buf, buf_size) == -1) {
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
                if (mkdir(path, 0755) == -1) {
                    if (errno == EEXIST) {
                        fprintf(stderr, "warning: try to create directory %s but it already exists\n", path);
                    }
                    else {
                        ERR_LOG("error: create directory %s failed", path);
                        free(path);
                        return -1;
                    }
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
    }

    return 0;
}

void communicate(int conn_fd) {
    // TODO: request remote directory
    // maybe add arg option to interactively select remote directory
    long long const INIT_BUF_SIZE = 128;

    long long buf_size = INIT_BUF_SIZE;
    char *buf = (char *)malloc(sizeof(char) * (buf_size + 1));

    json_data *info = NULL;
    if (request_info(conn_fd, config.remote_dir, &info, &buf, &buf_size) == -1) {
        goto finish;
    }

    if (traverse(conn_fd, info, NULL, &buf, &buf_size) == -1) {
        goto finish;
    }

finish:
    free(buf);
    if (info) {
        json_kill(info);
    }
}

int main(int argc, char **argv) {
    load_config(argc, argv);
    validate_config();
    printf("config:\n  host = %s\n  port = %d\n  remote directory = %s\n  local directory = %s\n",
        config.host, config.port, config.remote_dir, config.local_dir);

    if (chdir(config.local_dir) == -1) {
        ERR_EXIT("chdir");
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
