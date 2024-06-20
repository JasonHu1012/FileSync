#include "server_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "arg_parser.h"
#include "json.h"
#include "utils.h"

config_t config;

static void load_config_arg(int argc, char **argv) {
    arg_parser *arg = arg_init();
    arg_register(arg, "-p", "port", ARG_INT);
    arg_register(arg, "-d", "working directory", ARG_STRING);
    arg_register(arg, "--config", "config file path", ARG_STRING);
    arg_parse(arg, argc, argv);

    if (config.port == -1) {
        arg_get(arg, "-p", &config.port);
    }
    if (config.work_dir == NULL) {
        arg_get(arg, "-d", &config.work_dir);
    }
    if (config.config_path == NULL) {
        arg_get(arg, "--config", &config.config_path);
    }

    arg_kill(arg);
}

static void load_config_file(char *path) {
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
    sub_json = json_obj_get(json, "workDir");
    if (config.work_dir == NULL && sub_json) {
        config.work_dir = json_str_get(sub_json);
    }

    json_kill(json);
}

static void load_config_default() {
    int const PORT = 52124;
    char const *WORK_DIR = ".";

    if (config.port == -1) {
        config.port = PORT;
    }
    if (config.work_dir == NULL) {
        config.work_dir = (char *)malloc(sizeof(char) * (strlen(WORK_DIR) + 1));
        strcpy(config.work_dir, WORK_DIR);
    }
}

void load_config(int argc, char **argv) {
    char const *FILE_PATH = "config/server_config.json";

    // initialize config
    config.port = -1;
    config.work_dir = NULL;
    config.config_path = NULL;

    // config priority:
    // arg > file > default
    load_config_arg(argc, argv);
    load_config_file(config.config_path == NULL ? (char *)FILE_PATH : config.config_path);
    load_config_default();
}

void validate_config() {
    if (config.port < 0 || config.port > 65535) {
        fprintf(stderr, "fatal: invalid port %d\n", config.port);
        exit(1);
    }
}

void kill_config() {
    free(config.work_dir);
    if (config.config_path) {
        free(config.config_path);
    }
}
