#include "logana/server.h"
#include "logana/logana.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static logana_engine_t *g_engine = NULL;

static void logana_signal_handler(int signo) {
    if (g_engine) g_engine->shutting_down = true;
    (void)signo;
}

static ssize_t read_line(int fd, char *buffer, size_t cap) {
    size_t used = 0;
    while (used + 1 < cap) {
        char c = '\0';
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        buffer[used++] = c;
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static int read_exact(int fd, char *buffer, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = recv(fd, buffer + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void write_json_line(int fd, const char *json) {
    send(fd, json, strlen(json), 0);
    send(fd, "\n", 1, 0);
}

static logana_algorithm_t parse_algorithm_token(const char *token) {
    return logana_parse_algorithm(token && *token ? token : "dbscan");
}

static void handle_ingest(logana_engine_t *engine, int fd, const char *header) {
    char algorithm[64] = "dbscan";
    size_t payload_size = 0;
    if (sscanf(header, "INGEST %63s %zu", algorithm, &payload_size) != 2 || payload_size == 0) {
        write_json_line(fd, "{\"error\":\"invalid ingest header\"}");
        return;
    }
    char *payload = malloc(payload_size + 1);
    if (!payload) {
        write_json_line(fd, "{\"error\":\"oom\"}");
        return;
    }
    if (read_exact(fd, payload, payload_size) != 0) {
        free(payload);
        write_json_line(fd, "{\"error\":\"failed to read payload\"}");
        return;
    }
    payload[payload_size] = '\0';
    logana_job_t *job = logana_engine_submit(engine, payload, payload_size, parse_algorithm_token(algorithm));
    free(payload);
    if (!job) {
        write_json_line(fd, "{\"error\":\"submit failed\"}");
        return;
    }
    char response[256];
    snprintf(response, sizeof(response), "{\"jobId\":\"%llu\",\"status\":\"%s\"}",
             (unsigned long long)job->job_id, logana_status_name(job->status));
    write_json_line(fd, response);
}

static void handle_status(logana_engine_t *engine, int fd, const char *header) {
    unsigned long long job_id = 0;
    if (sscanf(header, "STATUS %llu", &job_id) != 1) {
        write_json_line(fd, "{\"error\":\"invalid status header\"}");
        return;
    }
    logana_job_t *job = logana_engine_find_job(engine, (uint64_t)job_id);
    if (!job) {
        write_json_line(fd, "{\"error\":\"job not found\"}");
        return;
    }
    char *json = logana_job_status_json(job);
    if (!json) {
        write_json_line(fd, "{\"error\":\"status unavailable\"}");
        return;
    }
    write_json_line(fd, json);
    free(json);
}

static void handle_result(logana_engine_t *engine, int fd, const char *header) {
    unsigned long long job_id = 0;
    if (sscanf(header, "RESULT %llu", &job_id) != 1) {
        write_json_line(fd, "{\"error\":\"invalid result header\"}");
        return;
    }
    logana_job_t *job = logana_engine_find_job(engine, (uint64_t)job_id);
    if (!job) {
        write_json_line(fd, "{\"error\":\"job not found\"}");
        return;
    }
    char *json = logana_job_result_json(job);
    if (!json) {
        write_json_line(fd, "{\"error\":\"result unavailable\"}");
        return;
    }
    write_json_line(fd, json);
    free(json);
}

static void handle_client(logana_engine_t *engine, int fd) {
    char header[256];
    if (read_line(fd, header, sizeof(header)) <= 0) return;
    if (strncmp(header, "INGEST ", 7) == 0) handle_ingest(engine, fd, header);
    else if (strncmp(header, "STATUS ", 7) == 0) handle_status(engine, fd, header);
    else if (strncmp(header, "RESULT ", 7) == 0) handle_result(engine, fd, header);
    else write_json_line(fd, "{\"error\":\"unknown command\"}");
}

int logana_server_run(logana_engine_t *engine, const logana_server_opts_t *opts) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;
    int one = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts->port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
    if (listen(server_fd, 128) != 0) return -1;
    g_engine = engine;
    signal(SIGINT, logana_signal_handler);
    signal(SIGTERM, logana_signal_handler);
    while (!engine->shutting_down) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(engine, client);
        close(client);
    }
    close(server_fd);
    return 0;
}
