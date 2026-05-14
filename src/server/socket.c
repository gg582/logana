#include "logana/server.h"
#include "logana/logana.h"

#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/sstring/sstring.h>
#include <cwist/net/http/http.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static logana_engine_t *g_engine = NULL;

static void logana_signal_handler(int signo) {
    if (g_engine) g_engine->shutting_down = true;
    (void)signo;
}

static const char *logana_http_status_text(cwist_http_status_t status) {
    switch (status) {
        case CWIST_HTTP_OK: return "OK";
        case CWIST_HTTP_CREATED: return "Created";
        case CWIST_HTTP_BAD_REQUEST: return "Bad Request";
        case CWIST_HTTP_NOT_FOUND: return "Not Found";
        case CWIST_HTTP_INTERNAL_ERROR: return "Internal Server Error";
        case CWIST_HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
        default: return "OK";
    }
}

static void logana_set_response(cwist_http_response *res, cwist_http_status_t status, const char *content_type, const char *body) {
    res->status_code = status;
    cwist_sstring_assign(res->status_text, (char *)logana_http_status_text(status));
    cwist_http_header_add(&res->headers, "Content-Type", content_type);
    cwist_sstring_assign(res->body, (char *)(body ? body : ""));
}

static void logana_send_json(cwist_http_response *res, cwist_http_status_t status, char *json) {
    if (!json) {
        logana_set_response(res, CWIST_HTTP_INTERNAL_ERROR, "application/json", "{\"error\":\"response encode failed\"}");
        return;
    }
    logana_set_response(res, status, "application/json", json);
    free(json);
}

static void logana_send_error(cwist_http_response *res, cwist_http_status_t status, const char *message) {
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        logana_set_response(res, CWIST_HTTP_INTERNAL_ERROR, "application/json", "{\"error\":\"oom\"}");
        return;
    }
    cJSON_AddStringToObject(json, "error", message);
    char *rendered = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    logana_send_json(res, status, rendered);
}

static bool logana_parse_job_id(const char *value, uint64_t *out) {
    if (!value || !*value || !out) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *out = (uint64_t)parsed;
    return true;
}

static logana_algorithm_t logana_parse_algorithm_json(const cJSON *body) {
    const cJSON *algorithm = cJSON_GetObjectItemCaseSensitive((cJSON *)body, "algorithm");
    if (cJSON_IsString(algorithm) && algorithm->valuestring && algorithm->valuestring[0] != '\0') {
        return logana_parse_algorithm(algorithm->valuestring);
    }
    return LOGANA_ALGO_DBSCAN;
}

static void logana_handle_ingest(cwist_http_request *req, cwist_http_response *res) {
    if (!req->body || !req->body->data) {
        logana_send_error(res, CWIST_HTTP_BAD_REQUEST, "payload is required");
        return;
    }

    cJSON *body = cJSON_Parse(req->body->data);
    if (!body || !cJSON_IsObject(body)) {
        if (body) cJSON_Delete(body);
        logana_send_error(res, CWIST_HTTP_BAD_REQUEST, "invalid json body");
        return;
    }

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(body, "payload");
    if (!cJSON_IsString(payload) || !payload->valuestring || payload->valuestring[0] == '\0') {
        cJSON_Delete(body);
        logana_send_error(res, CWIST_HTTP_BAD_REQUEST, "payload is required");
        return;
    }

    logana_job_t *job = logana_engine_submit(
        g_engine,
        payload->valuestring,
        strlen(payload->valuestring),
        logana_parse_algorithm_json(body));
    cJSON_Delete(body);

    if (!job) {
        logana_send_error(res, CWIST_HTTP_INTERNAL_ERROR, "submit failed");
        return;
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) {
        logana_send_error(res, CWIST_HTTP_INTERNAL_ERROR, "oom");
        return;
    }

    char job_id[32];
    snprintf(job_id, sizeof(job_id), "%llu", (unsigned long long)job->job_id);
    cJSON_AddStringToObject(json, "jobId", job_id);
    cJSON_AddStringToObject(json, "status", logana_status_name(job->status));
    char *rendered = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    logana_send_json(res, CWIST_HTTP_OK, rendered);
}

static const char *logana_extract_job_id_from_path(const char *path, const char *suffix, char *buffer, size_t buffer_size) {
    const char *prefix = "/jobs/";
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    size_t path_len = path ? strlen(path) : 0;
    if (!path || strncmp(path, prefix, prefix_len) != 0 || path_len <= prefix_len + suffix_len) return NULL;
    if (suffix_len > 0 && strcmp(path + path_len - suffix_len, suffix) != 0) return NULL;
    size_t id_len = path_len - prefix_len - suffix_len;
    if (id_len == 0 || id_len + 1 > buffer_size) return NULL;
    memcpy(buffer, path + prefix_len, id_len);
    buffer[id_len] = '\0';
    return buffer;
}

static logana_job_t *logana_require_job(const char *raw_id, cwist_http_response *res) {
    uint64_t job_id = 0;
    if (!logana_parse_job_id(raw_id, &job_id)) {
        logana_send_error(res, CWIST_HTTP_BAD_REQUEST, "invalid job id");
        return NULL;
    }
    logana_job_t *job = logana_engine_find_job(g_engine, job_id);
    if (!job) {
        logana_send_error(res, CWIST_HTTP_NOT_FOUND, "job not found");
        return NULL;
    }
    return job;
}

static void logana_handle_status(cwist_http_request *req, cwist_http_response *res) {
    char raw_id[32];
    const char *path = req->path && req->path->data ? req->path->data : NULL;
    logana_job_t *job = logana_require_job(logana_extract_job_id_from_path(path, "/status", raw_id, sizeof(raw_id)), res);
    if (!job) return;
    logana_send_json(res, CWIST_HTTP_OK, logana_job_status_json(job));
}

static void logana_handle_result(cwist_http_request *req, cwist_http_response *res) {
    char raw_id[32];
    const char *path = req->path && req->path->data ? req->path->data : NULL;
    logana_job_t *job = logana_require_job(logana_extract_job_id_from_path(path, "", raw_id, sizeof(raw_id)), res);
    if (!job) return;
    logana_send_json(res, CWIST_HTTP_OK, logana_job_result_json(job));
}

static void logana_handle_report(cwist_http_request *req, cwist_http_response *res) {
    char raw_id[32];
    const char *path = req->path && req->path->data ? req->path->data : NULL;
    logana_job_t *job = logana_require_job(logana_extract_job_id_from_path(path, "/view", raw_id, sizeof(raw_id)), res);
    if (!job) return;
    char *html = logana_job_report_page(job);
    if (!html) {
        logana_send_error(res, CWIST_HTTP_INTERNAL_ERROR, "report render failed");
        return;
    }
    logana_set_response(res, CWIST_HTTP_OK, "text/html; charset=utf-8", html);
    free(html);
}

static void logana_handle_root(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    logana_set_response(
        res,
        CWIST_HTTP_OK,
        "text/html; charset=utf-8",
        "<!doctype html><html><head><meta charset='utf-8'><title>Logana Engine</title>"
        "<style>body{font-family:IBM Plex Sans,Noto Sans KR,sans-serif;background:#f3ede2;color:#171311;"
        "padding:32px;line-height:1.6}code{background:#fffaf1;padding:2px 6px;border-radius:8px}</style></head>"
        "<body><h1>Logana Engine</h1><p>CWIST HTTP/SSR server is active.</p>"
        "<p>Use <code>POST /ingest</code>, <code>GET /jobs/:id/status</code>, <code>GET /jobs/:id</code>, or "
        "<code>GET /jobs/:id/view</code>.</p></body></html>");
}

static void logana_http_handler(int client_fd, void *arg) {
    (void)arg;
    char *buffer = cwist_alloc(CWIST_HTTP_READ_BUFFER_SIZE);
    if (!buffer) {
        close(client_fd);
        return;
    }

    size_t buffer_len = 0;
    cwist_http_request *req = cwist_http_receive_request(client_fd, buffer, CWIST_HTTP_READ_BUFFER_SIZE, &buffer_len);
    if (!req) {
        cwist_free(buffer);
        close(client_fd);
        return;
    }

    cwist_http_response *res = cwist_http_response_create();
    if (!res) {
        cwist_http_request_destroy(req);
        cwist_free(buffer);
        close(client_fd);
        return;
    }

    cwist_http_header_add(&res->headers, "Connection", "close");
    res->keep_alive = false;

    const char *path = req->path && req->path->data ? req->path->data : "/";
    if (req->method == CWIST_HTTP_GET && strcmp(path, "/") == 0) logana_handle_root(req, res);
    else if (req->method == CWIST_HTTP_POST && strcmp(path, "/ingest") == 0) logana_handle_ingest(req, res);
    else if (req->method == CWIST_HTTP_GET && strstr(path, "/status") != NULL) logana_handle_status(req, res);
    else if (req->method == CWIST_HTTP_GET && strstr(path, "/view") != NULL) logana_handle_report(req, res);
    else if (req->method == CWIST_HTTP_GET && strncmp(path, "/jobs/", 6) == 0) logana_handle_result(req, res);
    else logana_send_error(res, CWIST_HTTP_NOT_FOUND, "not found");

    cwist_http_send_response(client_fd, res);
    cwist_http_response_destroy(res);
    cwist_http_request_destroy(req);
    cwist_free(buffer);
    close(client_fd);
}

int logana_server_run(logana_engine_t *engine, const logana_server_opts_t *opts) {
    struct sockaddr_in addr;
    int server_fd = cwist_make_socket_ipv4(&addr, "127.0.0.1", opts->port, 128);
    if (server_fd < 0) return -1;

    g_engine = engine;
    signal(SIGINT, logana_signal_handler);
    signal(SIGTERM, logana_signal_handler);

    cwist_server_config config = {
        .use_forking = false,
        .use_threading = true,
        .use_epoll = false,
    };
    cwist_http_server_loop(server_fd, &config, logana_http_handler, NULL);

    close(server_fd);
    return 0;
}
