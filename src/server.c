#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include "json.h"
#include "utils.h"
#include "server_config.h"

// return socket fd when success, -1 when error
int init_socket(int port) {
    int const LISTEN_BACKLOG = 5;

    // socket
    int sock_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        ERROR("socket");
        return -1;
    }
    int option_value = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &option_value, sizeof(int)) == -1) {
        ERROR("setsockopt");
        return -1;
    }

    // bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        ERROR("bind to port %d failed", port);
        return -1;
    }

    // listen
    if (listen(sock_fd, LISTEN_BACKLOG) == -1) {
        ERROR("listen");
        return -1;
    }

    return sock_fd;
}

// traverse current working directory and store result in `*info`
// return 0 when success, -1 when working directory error
int traverse(char *name, json_data **info) {
    // init info of current directory
    struct stat st;
    if (stat(".", &st) == -1) {
        // skip current working directory
        char *cwd = getcwd(NULL, 0);
        ERROR("get %s status failed (pid %d)", cwd, getpid());
        free(cwd);
        return 0;
    }

    *info = json_obj_init();
    json_obj_set(*info, "name", json_str_init(name));
    json_obj_set(*info, "type", json_str_init("directory"));
    json_obj_set(*info, "entries", json_arr_init());
    json_obj_set(*info, "updateTime", json_num_init((double)st.st_mtime));
    json_obj_set(*info, "permission", json_num_init((double)(st.st_mode & 0777)));

    // read directory
    DIR *dirp = opendir(".");
    if (!dirp) {
        // skip entries of current working directory
        char *cwd = getcwd(NULL, 0);
        ERROR("open directory %s failed (pid %d)", cwd, getpid());
        free(cwd);
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dirp))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        json_data *sub_info = NULL;

        if (entry->d_type == DT_REG) {
            if (stat(entry->d_name, &st) == -1) {
                ERROR("get %s status failed (pid %d)", entry->d_name, getpid());
                continue;
            }

            sub_info = json_obj_init();
            json_obj_set(sub_info, "name", json_str_init(entry->d_name));
            json_obj_set(sub_info, "type", json_str_init("file"));
            json_obj_set(sub_info, "updateTime", json_num_init((double)st.st_mtime));
            json_obj_set(sub_info, "permission", json_num_init((double)(st.st_mode & 0777)));
        }

        else if (entry->d_type == DT_DIR) {
            char *cwd = getcwd(NULL, 0);
            if (chdir(entry->d_name) == -1) {
                ERROR("change working directory to %s failed (pid %d)", entry->d_name, getpid());
                free(cwd);
                continue;
            }

            // the result doesn't matter, only care whether we can change back to correct working directory later
            traverse(entry->d_name, &sub_info);

            if (chdir(cwd) == -1) {
                // can't change back to correct working directory
                ERROR("change working directory to %s failed (pid %d)", cwd, getpid());
                if (sub_info) {
                    json_arr_append(json_obj_get(*info, "entries"), sub_info);
                }
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

// transform `path` to relative path in-place
// strlen(path) must > 0
// if `path` is "/", it will become "."
void to_relative(char *path) {
    int i = 0;
    while (path[i] && path[i] == '/') {
        i++;
    }
    if (!path[i]) {
        path[0] = '.';
        path[1] = 0;
        return;
    }
    memmove(path, path + i, sizeof(char) * (strlen(path) + 1 - i));
}

// return 0 when success, -1 when error
int respond_info(int conn_fd, char **buf, uint64_t *buf_size) {
    // get requested path
    uint64_t message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("receive info request failed (pid %d)", getpid());
        return -1;
    }
    message_len = my_ntohll(message_len);

    if (message_len == 0) {
        INFO("receive info request with empty path (pid %d)", getpid());
        goto respond_empty;
    }

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERROR("receive info request failed (pid %d)", getpid());
        return -1;
    }
    (*buf)[message_len] = 0;
    INFO("received %s info request (pid %d)", *buf, getpid());

    if (!is_valid_request_path(*buf)) {
        INFO("invalid info request path %s (pid %d)", *buf, getpid());
        goto respond_empty;
    }

    // transform to relative path
    to_relative(*buf);

    // traverse
    // change directory outside of `traverse` because in this way,
    // we don't need to release `cwd` when there's error in `traverse`
    char *cwd = getcwd(NULL, 0);
    if (chdir(*buf) == -1) {
        ERROR("change working directory to %s failed (pid %d)", *buf, getpid());
        free(cwd);
        goto respond_empty;
    }

    json_data *info = NULL;
    // the result doesn't matter, only care whether we can change back to correct working directory later
    traverse(".", &info);

    if (chdir(cwd) == -1) {
        ERROR("change working directory to %s failed (pid %d)", cwd, getpid());
        if (info) {
            json_kill(info);
        }
        free(cwd);
        return -1;
    }
    free(cwd);

    if (!info) {
        goto respond_empty;
    }

    // send to client
    char *info_str = json_to_str(info, false);
    json_kill(info);

    char *path = (char *)malloc(sizeof(char) * (strlen(*buf) + 1));
    strcpy(path, *buf);

    *buf_size = extend_buf(buf, *buf_size, sizeof(uint64_t));
    append_buf_uint64(*buf, 0, my_htonll(strlen(info_str)));
    // not appending `info_str` to `*buf` because it's too large
    if (bulk_write(conn_fd, *buf, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("respond %s info failed (pid %d)", path, getpid());
        free(info_str);
        free(path);
        return -1;
    }
    if (bulk_write(conn_fd, info_str, strlen(info_str)) != strlen(info_str)) {
        ERROR("respond %s info failed (pid %d)", path, getpid());
        free(info_str);
        free(path);
        return -1;
    }
    INFO("responded %s info (%zu bytes) (pid %d)", path, strlen(info_str), getpid());

    free(info_str);
    free(path);

    return 0;

respond_empty:
    *buf_size = extend_buf(buf, *buf_size, sizeof(uint64_t));
    append_buf_uint64(*buf, 0, my_htonll(0));
    if (bulk_write(conn_fd, *buf, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("respond empty info failed (pid %d)", getpid());
        return -1;
    }
    INFO("responded empty info (pid %d)", getpid());

    return 0;
}

// return 0 when success, -1 when error
int respond_content(int conn_fd, char **buf, uint64_t *buf_size) {
    int const BLOCK_SIZE = 4096;

    // get requested path
    uint64_t message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("receive content request failed (pid %d)", getpid());
        return -1;
    }
    message_len = my_ntohll(message_len);

    if (message_len == 0) {
        INFO("receive content request with empty path (pid %d)", getpid());
        goto respond_empty;
    }

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        ERROR("receive content request failed (pid %d)", getpid());
        return -1;
    }
    (*buf)[message_len] = 0;
    INFO("received %s content request (pid %d)", *buf, getpid());

    if (!is_valid_request_path(*buf)) {
        INFO("invalid content request path %s (pid %d)", *buf, getpid());
        goto respond_empty;
    }

    // transform to relative path
    to_relative(*buf);

    // open file and get file size
    int file_fd = open(*buf, O_RDONLY);
    if (file_fd == -1) {
        ERROR("open %s failed (pid %d)", *buf, getpid());
        goto respond_empty;
    }

    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        ERROR("get %s status failed (pid %d)", *buf, getpid());
        close(file_fd);
        goto respond_empty;
    }

    // send file length
    char *path = (char *)malloc(sizeof(char) * (strlen(*buf) + 1));
    strcpy(path, *buf);

    *buf_size = extend_buf(buf, *buf_size, sizeof(uint64_t));
    append_buf_uint64(*buf, 0, my_htonll(st.st_size));
    if (bulk_write(conn_fd, *buf, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("respond %s content failed (pid %d)", path, getpid());
        free(path);
        close(file_fd);
        return -1;
    }

    // read file and send to client
    *buf_size = extend_buf(buf, *buf_size, BLOCK_SIZE);
    uint64_t send_len = 0;
    while (send_len < st.st_size) {
        // read file
        int len = bulk_read(file_fd, *buf, BLOCK_SIZE);
        if (len == 0) {
            WARN("unexpected EOF when reading %s (pid %d)", path, getpid());
        }
        if (len == 0 || len == -1) {
            ERROR("read %s failed (pid %d)", path, getpid());
            free(path);
            close(file_fd);
            return -1;
        }

        // send to client
        if (bulk_write(conn_fd, *buf, len) != len) {
            ERROR("respond %s content failed (pid %d)", path, getpid());
            free(path);
            close(file_fd);
            return -1;
        }
        send_len += len;
    }
    INFO("responded %s content (%" PRIu64 " bytes) (pid %d)", path, send_len, getpid());

    free(path);
    close(file_fd);

    return 0;

respond_empty:
    *buf_size = extend_buf(buf, *buf_size, sizeof(uint64_t));
    append_buf_uint64(*buf, 0, my_htonll(0));
    if (bulk_write(conn_fd, *buf, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ERROR("respond empty content failed (pid %d)", getpid());
        return -1;
    }
    INFO("responded empty content (pid %d)", getpid());

    return 0;
}

// return 0 when success, -1 when error
int respond_working_dir(int conn_fd, char **buf, uint64_t *buf_size) {
    char *cwd = getcwd(NULL, 0);

    uint64_t message_len = sizeof(uint64_t) + strlen(cwd);
    *buf_size = extend_buf(buf, *buf_size, message_len);
    message_len = append_buf_uint64(*buf, 0, my_htonll(strlen(cwd)));
    message_len = append_buf_charp(*buf, message_len, cwd);
    free(cwd);

    if (bulk_write(conn_fd, *buf, message_len) != message_len) {
        ERROR("respond working directory failed (pid %d)", getpid());
        return -1;
    }
    INFO("responded working directory (pid %d)", getpid());

    return 0;
}

void communicate(int conn_fd) {
    uint64_t const INIT_BUF_SIZE = 128;

    uint64_t buf_size = INIT_BUF_SIZE;
    char *buf = (char *)malloc(sizeof(char) * (buf_size + 1));

    uint32_t command;
    while (1) {
        if (bulk_read(conn_fd, &command, sizeof(uint32_t)) != sizeof(uint32_t)) {
            goto finish;
        }
        command = ntohl(command);

        switch (command) {
        case 0:
        {
            INFO("received command: request info (pid %d)", getpid());
            if (respond_info(conn_fd, &buf, &buf_size) == -1) {
                goto finish;
            }
            break;
        }
        case 1:
        {
            INFO("received command: request content (pid %d)", getpid());
            if (respond_content(conn_fd, &buf, &buf_size) == -1) {
                goto finish;
            }
            break;
        }
        case 2:
        {
            INFO("received exit message (pid %d)", getpid());
            goto finish;
            break;
        }
        case 3:
        {
            INFO("received command: request working directory (pid %d)", getpid());
            if (respond_working_dir(conn_fd, &buf, &buf_size) == -1) {
                goto finish;
            }
            break;
        }
        default:
        {
            WARN("received unknown command (pid %d)", getpid());
            break;
        }
        }
    }

finish:
    free(buf);
}

int main(int argc, char **argv) {
    load_config(argc, argv);
    if (!is_valid_config()) {
        kill_config();
        return 1;
    }
    printf("config:\n  port = %d\n  working directory = %s\n\n", config.port, config.work_dir);

    if (chdir(config.work_dir) == -1) {
        ERROR("change working directory to %s failed", config.work_dir);
        kill_config();
        return 1;
    }
    char *cwd = getcwd(NULL, 0);
    INFO("working at %s", cwd);
    free(cwd);

    int sock_fd = init_socket(config.port);
    if (sock_fd == -1) {
        kill_config();
        return 1;
    }
    INFO("listening on port %d", config.port);

    while (1) {
        // accept connection
        struct sockaddr_in addr;
        socklen_t addr_size = sizeof(addr);
        int conn_fd = accept(sock_fd, (struct sockaddr *)&addr, &addr_size);
        if (conn_fd == -1) {
            ERROR("accept");
            continue;
        }

        // use multiprocess because we need to change working directory
        int pid = fork();
        if (pid == -1) {
            ERROR("fork");
            close(conn_fd);
            continue;
        }

        if (pid == 0) {
            // child
            close(sock_fd);

            INFO("connected from %s:%d (pid %d)", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), getpid());
            communicate(conn_fd);

            close(conn_fd);
            INFO("disconnected %s:%d (pid %d)", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), getpid());
            kill_config();
            return 0;
        }

        close(conn_fd);
    }

    close(sock_fd);
    kill_config();

    return 0;
}
