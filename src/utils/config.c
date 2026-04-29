#include "logana/utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int logana_strieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

const char *logana_status_name(logana_job_status_t status) {
    switch (status) {
        case LOGANA_JOB_QUEUED: return "queued";
        case LOGANA_JOB_BATCHING: return "batching";
        case LOGANA_JOB_ANALYZING: return "analyzing";
        case LOGANA_JOB_RENDERING: return "rendering";
        case LOGANA_JOB_READY: return "ready";
        case LOGANA_JOB_FAILED: return "failed";
    }
    return "unknown";
}

const char *logana_algorithm_name(logana_algorithm_t algorithm) {
    switch (algorithm) {
        case LOGANA_ALGO_KMEANS_PP: return "kmeans++";
        case LOGANA_ALGO_DBSCAN: return "dbscan";
        case LOGANA_ALGO_BIRCH: return "birch";
        case LOGANA_ALGO_MEAN_SHIFT: return "mean_shift";
        case LOGANA_ALGO_OPTICS: return "optics";
        case LOGANA_ALGO_GMM: return "gmm";
        case LOGANA_ALGO_AGGLOMERATIVE: return "agglomerative";
    }
    return "dbscan";
}

logana_algorithm_t logana_parse_algorithm(const char *name) {
    if (!name) return LOGANA_ALGO_DBSCAN;
    if (logana_strieq(name, "kmeans++") || logana_strieq(name, "kmeans")) return LOGANA_ALGO_KMEANS_PP;
    if (logana_strieq(name, "dbscan")) return LOGANA_ALGO_DBSCAN;
    if (logana_strieq(name, "birch")) return LOGANA_ALGO_BIRCH;
    if (logana_strieq(name, "mean_shift")) return LOGANA_ALGO_MEAN_SHIFT;
    if (logana_strieq(name, "optics")) return LOGANA_ALGO_OPTICS;
    if (logana_strieq(name, "gmm")) return LOGANA_ALGO_GMM;
    if (logana_strieq(name, "agglomerative")) return LOGANA_ALGO_AGGLOMERATIVE;
    return LOGANA_ALGO_DBSCAN;
}

void logana_default_config(logana_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->case_sensitive = false;
    config->fuzzy_threshold = 0.85;
    config->max_rows_per_analysis = 2000000;
    config->memory_pool_size_mb = 1024;
    config->aggregation_window_ms = 20;
    config->min_batch_size_bytes = 64 * 1024;
    config->worker_threads = 8;
    config->async_render_threads = 4;
    config->default_algorithm = LOGANA_ALGO_DBSCAN;
    config->dbscan_eps = 0.45;
    config->dbscan_min_samples = 10;
    config->enable_time_series_trend = true;
    config->enable_outlier_detection = true;
    config->enable_correlation_matrix = true;
    config->enable_shannon_entropy = true;
    config->enable_linear_regression = true;
    snprintf(config->theme, sizeof(config->theme), "%s", "hyper-dark");
    config->canvas_width = 1440;
    config->canvas_height = 500;
    snprintf(config->color_palette[config->color_count++], 16, "%s", "#00FF41");
    snprintf(config->color_palette[config->color_count++], 16, "%s", "#00E5FF");
    snprintf(config->color_palette[config->color_count++], 16, "%s", "#FF0055");
    snprintf(config->color_palette[config->color_count++], 16, "%s", "#FFCC00");
    snprintf(config->color_palette[config->color_count++], 16, "%s", "#9D00FF");
}

static void logana_parse_array_line(char items[][96], size_t *count, size_t width, const char *line) {
    const char *lb = strchr(line, '[');
    const char *rb = strrchr(line, ']');
    if (!lb || !rb || rb <= lb) return;
    char buffer[1024];
    size_t len = (size_t)(rb - lb - 1);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, lb + 1, len);
    buffer[len] = '\0';
    char *cursor = buffer;
    while (*cursor && *count < LOGANA_MAX_KEYS) {
        while (*cursor && (isspace((unsigned char)*cursor) || *cursor == ',')) ++cursor;
        if (*cursor == '"') ++cursor;
        char *start = cursor;
        while (*cursor && *cursor != '"' && *cursor != ',') ++cursor;
        size_t item_len = (size_t)(cursor - start);
        if (item_len > 0) {
            size_t copy_len = item_len < (width - 1) ? item_len : (width - 1);
            memset(items[*count], 0, width);
            memcpy(items[*count], start, copy_len);
            (*count)++;
        }
        while (*cursor && *cursor != ',') ++cursor;
    }
}

static void logana_trim(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

int logana_config_load(const char *path, logana_config_t *config) {
    logana_default_config(config);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        logana_trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;
        if (strstr(line, "case_sensitive")) config->case_sensitive = strstr(line, "true") != NULL;
        else if (strstr(line, "fuzzy_threshold")) config->fuzzy_threshold = atof(strchr(line, '=') + 1);
        else if (strstr(line, "max_rows_per_analysis")) config->max_rows_per_analysis = strtoull(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "memory_pool_size_mb")) config->memory_pool_size_mb = strtoull(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "aggregation_window_ms")) config->aggregation_window_ms = (uint32_t)strtoul(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "min_batch_size_kb")) config->min_batch_size_bytes = (size_t)strtoull(strchr(line, '=') + 1, NULL, 10) * 1024ULL;
        else if (strstr(line, "worker_threads")) config->worker_threads = strtoull(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "async_render_threads")) config->async_render_threads = strtoull(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "timestamp_keys")) {
            config->timestamp_key_count = 0;
            logana_parse_array_line((char (*)[96])config->timestamp_keys, &config->timestamp_key_count, sizeof(config->timestamp_keys[0]), line);
        } else if (strstr(line, "numeric_keys")) {
            config->numeric_key_count = 0;
            logana_parse_array_line((char (*)[96])config->numeric_keys, &config->numeric_key_count, sizeof(config->numeric_keys[0]), line);
        } else if (strstr(line, "category_keys")) {
            config->category_key_count = 0;
            logana_parse_array_line((char (*)[96])config->category_keys, &config->category_key_count, sizeof(config->category_keys[0]), line);
        } else if (strstr(line, "default_algorithm")) config->default_algorithm = logana_parse_algorithm(strchr(line, '"') + 1);
        else if (strstr(line, "dbscan_eps")) config->dbscan_eps = atof(strchr(line, '=') + 1);
        else if (strstr(line, "dbscan_min_samples")) config->dbscan_min_samples = strtoull(strchr(line, '=') + 1, NULL, 10);
        else if (strstr(line, "enable_time_series_trend")) config->enable_time_series_trend = strstr(line, "true") != NULL;
        else if (strstr(line, "enable_outlier_detection")) config->enable_outlier_detection = strstr(line, "true") != NULL;
        else if (strstr(line, "enable_correlation_matrix")) config->enable_correlation_matrix = strstr(line, "true") != NULL;
        else if (strstr(line, "enable_shannon_entropy")) config->enable_shannon_entropy = strstr(line, "true") != NULL;
        else if (strstr(line, "enable_linear_regression")) config->enable_linear_regression = strstr(line, "true") != NULL;
        else if (strstr(line, "theme")) snprintf(config->theme, sizeof(config->theme), "%s", strchr(line, '"') + 1);
        else if (strstr(line, "canvas_width")) config->canvas_width = atoi(strchr(line, '=') + 1);
        else if (strstr(line, "canvas_height")) config->canvas_height = atoi(strchr(line, '=') + 1);
        else if (strstr(line, "color_palette")) {
            config->color_count = 0;
            logana_parse_array_line((char (*)[96])config->color_palette, &config->color_count, sizeof(config->color_palette[0]), line);
        } else if (strstr(line, "paths")) {
            config->nested_path_count = 0;
            logana_parse_array_line((char (*)[96])config->nested_paths, &config->nested_path_count, sizeof(config->nested_paths[0]), line);
        }
    }
    fclose(fp);
    return 0;
}
