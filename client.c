// TODO: when read file content, read with fixed size iteratively
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

#define ERR_EXIT(s) fprintf(stderr, "fatal: %s: %s\n", s, strerror(errno)); exit(1)

typedef struct {
    int port;
    char *host;
    char *remote_dir;
    char *local_dir;
} config_t;

config_t config;

void load_config_default() {
    int const PORT = 52124;
    char const *HOST = "127.0.0.1";
    char const *REMOTE_DIR = ".";
    char const *LOCAL_DIR = ".";

    config.port = PORT;
    config.host = (char *)malloc(sizeof(char) * (strlen(HOST) + 1));
    strcpy(config.host, HOST);
    config.remote_dir = (char *)malloc(sizeof(char) * (strlen(REMOTE_DIR) + 1));
    strcpy(config.remote_dir, REMOTE_DIR);
    config.local_dir = (char *)malloc(sizeof(char) * (strlen(LOCAL_DIR) + 1));
    strcpy(config.local_dir, LOCAL_DIR);
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
    sub_json = json_obj_get(json, "host");
    if (sub_json) {
        free(config.host);
        config.host = json_str_get(sub_json);
    }
    sub_json = json_obj_get(json, "remoteDir");
    if (sub_json) {
        free(config.remote_dir);
        config.remote_dir = json_str_get(sub_json);
    }
    sub_json = json_obj_get(json, "localDir");
    if (sub_json) {
        free(config.local_dir);
        config.local_dir = json_str_get(sub_json);
    }

    json_kill(json);
}

void load_config_arg(int argc, char **argv) {
    arg_parser *arg = arg_init();
    arg_register(arg, "-p", "port", ARG_INT);
    arg_register(arg, "--host", "host ip", ARG_STRING);
    arg_register(arg, "--rdir", "remote directory", ARG_STRING);
    arg_register(arg, "--ldir", "local directory", ARG_STRING);
    arg_parse(arg, argc, argv);

    arg_get(arg, "-p", &config.port);
    if (arg_is_parsed(arg, "--host")) {
        free(config.host);
        arg_get(arg, "--host", &config.host);
    }
    if (arg_is_parsed(arg, "--rdir")) {
        free(config.remote_dir);
        arg_get(arg, "--rdir", &config.remote_dir);
    }
    if (arg_is_parsed(arg, "--ldir")) {
        free(config.local_dir);
        arg_get(arg, "--ldir", &config.local_dir);
    }

    arg_kill(arg);
}

void load_config(int argc, char **argv) {
    char const *FILE_PATH = "./client_config.json";

    // config priority:
    // arg > file > default
    load_config_default();
    load_config_file((char *)FILE_PATH);
    load_config_arg(argc, argv);
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
        fprintf(stderr, "fatal: host invalid\n");
        exit(1);
    };

    if (connect(conn_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        ERR_EXIT("connect");
    }

    return conn_fd;
}

// requested result is stored in `*json`
// return 0 when success, -1 when error
int request_info(int conn_fd, char *path, json_data **json, char **buf, int *buf_size) {
    // [0][path length][path]
    *buf_size = extend_buf(buf, *buf_size, sizeof(int) * 2);
    ((int *)*buf)[0] = htonl(0);
    ((int *)*buf)[1] = htonl(strlen(path));
    if (bulk_write(conn_fd, *buf, sizeof(int) * 2) != sizeof(int) * 2) {
        fprintf(stderr, "error: request info failed\n");
        return -1;
    }
    if (bulk_write(conn_fd, path, strlen(path)) != strlen(path)) {
        fprintf(stderr, "error: request info failed\n");
        return -1;
    }
    printf("requested info of %s\n", path);

    // get requested result
    int message_len;
    if (bulk_read(conn_fd, &message_len, sizeof(int)) != sizeof(int)) {
        fprintf(stderr, "error: receive info failed\n");
        return -1;
    }
    message_len = ntohl(message_len);

    *buf_size = extend_buf(buf, *buf_size, message_len);
    if (bulk_read(conn_fd, *buf, message_len) != message_len) {
        fprintf(stderr, "error: receive info failed\n");
        return -1;
    }
    (*buf)[message_len] = 0;
    printf("received info (%d bytes)\n", message_len);

    // convert result to json
    *json = json_parse(*buf);

    return 0;
}

void communicate(int conn_fd) {
    // TODO: request remote directory
    // maybe add arg option to interactively select remote directory
    int const INIT_BUF_SIZE = 128;

    int buf_size = INIT_BUF_SIZE;
    char *buf = (char *)malloc(sizeof(char) * (buf_size + 1));

    json_data *json = NULL;
    if (request_info(conn_fd, config.remote_dir, &json, &buf, &buf_size) == -1) {
        goto finish;
    }

finish:
    free(buf);
    if (json) {
        json_kill(json);
    }
}

int main(int argc, char **argv) {
    load_config(argc, argv);
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
