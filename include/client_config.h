#ifndef _CLIENT_CONFIG_H
#define _CLIENT_CONFIG_H

typedef struct {
    int port;
    char *host;
    char *remote_dir;
    char *local_dir;
    char *config_path;
} config_t;

extern config_t config;

void load_config(int argc, char **argv);

void validate_config();

void kill_config();

#endif
