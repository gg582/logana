#include "logana/logana.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *config_path = getenv("LOGANA_CONFIG");
    if (!config_path) config_path = "./collect.ini";
    const char *port_env = getenv("LOGANA_ENGINE_PORT");
    uint16_t port = (uint16_t)(port_env ? atoi(port_env) : 24445);

    logana_config_t config;
    if (logana_config_load(config_path, &config) != 0) {
        fprintf(stderr, "failed to load config: %s\n", config_path);
        return 1;
    }

    logana_engine_t engine;
    if (logana_engine_init(&engine, &config) != 0) {
        fprintf(stderr, "failed to initialize engine\n");
        return 1;
    }

    logana_server_opts_t opts = {.engine = &engine, .port = port};
    int rc = logana_server_run(&engine, &opts);
    logana_engine_shutdown(&engine);
    return rc == 0 ? 0 : 1;
}
