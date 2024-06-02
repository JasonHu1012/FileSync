// TODO: only touch my own file?
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
#include "json.h"
#include "arg_parser.h"
#include "utils.h"

#define ERR_EXIT(s) fprintf(stderr, "fatal: %s: %s\n", s, strerror(errno)); exit(1)

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
        fprintf(stderr, "error: open config file failed\n");
        return;
    }

    // get file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "error: get config file status failed\n");
        close(fd);
        return;
    }

    // read whole file
    char *buf = malloc(sizeof(char) * (st.st_size + 1));
    int len = bulk_read(fd, buf, st.st_size);
    close(fd);
    if (len != st.st_size) {
        if (len == -1) {
            fprintf(stderr, "error: read config file failed\n");
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

// traverse current working directory and store result in `*json`
// return 0 when success, -1 when error
int traverse(char *name, json_data **json) {
    struct stat st;
    if (stat(".", &st) == -1) {
        char *cwd = getcwd(NULL, 0);
        fprintf(stderr, "error: get status of %s failed (pid %d)\n", cwd, getpid());
        free(cwd);
        return -1;
    }

    *json = json_obj_init();
    json_obj_set(*json, "name", json_str_init(name));
    json_obj_set(*json, "type", json_str_init("directory"));
    json_obj_set(*json, "entries", json_arr_init());
    json_obj_set(*json, "updateTime", json_num_init(st.st_mtime));

    // read directory
    DIR *dirp = opendir(".");
    if (!dirp) {
        char *cwd = getcwd(NULL, 0);
        fprintf(stderr, "error: open directory %s failed (pid %d)\n", cwd, getpid());
        free(cwd);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        json_data *sub_json = NULL;

        if (entry->d_type == DT_REG) {
            if (stat(entry->d_name, &st) == -1) {
                fprintf(stderr, "error: get status of %s failed (pid %d)\n", entry->d_name, getpid());
                closedir(dirp);
                return -1;
            }

            sub_json = json_obj_init();
            json_obj_set(sub_json, "name", json_str_init(entry->d_name));
            json_obj_set(sub_json, "type", json_str_init("file"));
            json_obj_set(sub_json, "updateTime", json_num_init(st.st_mtime));
        }

        else if (entry->d_type == DT_DIR) {
            char *cwd = getcwd(NULL, 0);
            if (chdir(entry->d_name) == -1) {
                fprintf(stderr, "error: change working directory to %s failed (pid %d)\n", entry->d_name, getpid());
                free(cwd);
                closedir(dirp);
                return -1;
            }

            if (traverse(entry->d_name, &sub_json) == -1) {
                if (sub_json) {
                    json_kill(sub_json);
                }
                free(cwd);
                closedir(dirp);
                return -1;
            }

            if (chdir(cwd) == -1) {
                fprintf(stderr, "error: change working directory to %s failed (pid %d)\n", cwd, getpid());
                json_kill(sub_json);
                free(cwd);
                closedir(dirp);
                return -1;
            }
            free(cwd);
        }

        if (sub_json) {
            json_arr_append(json_obj_get(*json, "entries"), sub_json);
        }
    }

    closedir(dirp);

    return 0;
}

// return 0 when success, -1 when error
int response_info(int conn_fd, char **buf, int *buf_size) {
    // get requested path
    int message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(int)) != sizeof(int)) {
        fprintf(stderr, "receive request failed (pid %d)\n", getpid());
        return -1;
    }
    message_len = ntohl(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        fprintf(stderr, "receive request failed (pid %d)\n", getpid());
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("received request of %s (pid %d)\n", *buf, getpid());

    // traverse
    char *cwd = getcwd(NULL, 0);
    if (chdir(*buf) == -1) {
        fprintf(stderr, "error: change working directory to %s failed (pid %d)\n", *buf, getpid());
        free(cwd);
        return -1;
    }

    json_data *json = NULL;
    if (traverse(*buf, &json) == -1) {
        if (json) {
            json_kill(json);
        }
        free(cwd);
        return -1;
    }

    if (chdir(cwd) == -1) {
        fprintf(stderr, "error: change working directory to %s failed (pid %d)\n", cwd, getpid());
        json_kill(json);
        free(cwd);
        return -1;
    }
    free(cwd);

    // send to client
    char *json_str = json_to_str(json, false);
    json_kill(json);

    *buf_size = extend_buf(buf, *buf_size, sizeof(int));
    ((int *)*buf)[0] = htonl(strlen(json_str));
    if (bulk_write(conn_fd, *buf, sizeof(int)) != sizeof(int)) {
        fprintf(stderr, "response info failed (pid %d)\n", getpid());
        free(json_str);
        return -1;
    }
    if (bulk_write(conn_fd, json_str, strlen(json_str)) != strlen(json_str)) {
        fprintf(stderr, "response info failed (pid %d)\n", getpid());
        free(json_str);
        return -1;
    }
    printf("responsed info (%ld bytes) (pid %d)\n", strlen(json_str), getpid());
    free(json_str);

    return 0;
}

void communicate(int conn_fd) {
    int const INIT_BUF_SIZE = 128;

    int buf_size = INIT_BUF_SIZE;
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
            if (response_info(conn_fd, &buf, &buf_size) == -1) {
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
            fprintf(stderr, "error: accept: %s\n", strerror(errno));
            continue;
        }

        // use multiprocess because we need to change working directory
        int pid = fork();
        if (pid == -1) {
            fprintf(stderr, "error: fork: %s\n", strerror(errno));
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
