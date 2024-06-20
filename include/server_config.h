#ifndef _CONFIG_H
#define _CONFIG_H

typedef struct {
    int port;
    char *work_dir;
    char *config_path;
} config_t;

extern config_t config;

void load_config(int argc, char **argv);

void validate_config();

void kill_config();

#endif
