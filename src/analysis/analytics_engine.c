#include "logana/logana.h"
#include "logana/simd.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ttak/async/task.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

typedef struct {
    size_t idx;
    double dist;
} logana_neighbor_t;

static void logana_log_sink(ttak_log_level_t level, const char *msg) {
    const char *tag = "INFO";
    if (level == TTAK_LOG_DEBUG) tag = "DEBUG";
    else if (level == TTAK_LOG_WARN) tag = "WARN";
    else if (level == TTAK_LOG_ERROR) tag = "ERROR";
    fprintf(stderr, "[%s] %s\n", tag, msg);
}

uint64_t logana_now_ms(void) {
    return ttak_get_tick_count();
}

uint64_t logana_hash64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void logana_set_job_status(logana_job_t *job, logana_job_status_t status, const char *error) {
    pthread_mutex_lock(&job->lock);
    job->status = status;
    job->updated_ms = logana_now_ms();
    if (error) snprintf(job->error, sizeof(job->error), "%s", error);
    pthread_mutex_unlock(&job->lock);
}

static const char *logana_find_key_ci(const char *line, const char *key) {
    size_t key_len = strlen(key);
    for (const char *p = line; *p; ++p) {
        if (*p != '"') continue;
        ++p;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i < key_len &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)key[i])) {
            ++i;
        }
        if (i == key_len && p[i] == '"') return p - 1;
    }
    return NULL;
}

static int logana_extract_number(const char *line, const char *key, bool case_sensitive, double *out) {
    const char *found = case_sensitive ? strstr(line, key) : logana_find_key_ci(line, key);
    if (!found) return 0;
    const char *colon = strchr(found, ':');
    if (!colon) return 0;
    char *end = NULL;
    double value = strtod(colon + 1, &end);
    if (end == colon + 1) return 0;
    *out = value;
    return 1;
}

static size_t logana_count_chars(const char *line, char needle) {
    size_t count = 0;
    for (const char *p = line; *p; ++p) {
        if (*p == needle) ++count;
    }
    return count;
}

static int logana_parse_numeric_token(const char *start, const char *end, double *out) {
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    while (start < end && (*start == '"' || *start == '\'' || *start == '{' || *start == '[' || *start == '(')) ++start;
    while (end > start && (end[-1] == '"' || end[-1] == '\'' || end[-1] == '}' || end[-1] == ']' || end[-1] == ')' || end[-1] == ',' || end[-1] == ';' || end[-1] == '|')) --end;
    if (end <= start) return 0;

    char token[128];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(token)) len = sizeof(token) - 1;
    memcpy(token, start, len);
    token[len] = '\0';

    if (!strncmp(token, "0x", 2) || !strncmp(token, "0X", 2)) {
        char *hex_end = NULL;
        unsigned long long value = strtoull(token, &hex_end, 16);
        if (hex_end && *hex_end == '\0' && hex_end != token) {
            *out = (double)value;
            return 1;
        }
    }

    char *num_end = NULL;
    double value = strtod(token, &num_end);
    if (!num_end || num_end == token || *num_end != '\0') return 0;
    *out = value;
    return 1;
}

static size_t logana_collect_freeform_numbers(const char *line, double *values, size_t cap) {
    size_t count = 0;
    const char *p = line;
    while (*p && count < cap) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == '|')) ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';' && *p != '|') ++p;
        const char *end = p;
        const char *value_start = start;
        for (const char *q = end; q > start; --q) {
            if (q[-1] == '=' || q[-1] == ':') {
                value_start = q;
                break;
            }
        }
        double value = 0.0;
        if (logana_parse_numeric_token(value_start, end, &value)) {
            values[count++] = value;
        }
    }
    return count;
}

static double logana_distance_sq(const float *a, const float *b, size_t dims) {
    double total = 0.0;
    for (size_t i = 0; i < dims; ++i) {
        double d = (double)a[i] - (double)b[i];
        total += d * d;
    }
    return total;
}

static size_t logana_parse_matrix(logana_engine_t *engine, logana_job_t *job) {
    size_t dims = engine->config.numeric_key_count;
    if (dims == 0) dims = LOGANA_MAX_DIMENSIONS;
    if (dims > LOGANA_MAX_DIMENSIONS) dims = LOGANA_MAX_DIMENSIONS;
    size_t row_capacity = 1024;
    float *values = malloc(row_capacity * dims * sizeof(float));
    if (!values) return 0;
    size_t rows = 0;
    char *cursor = job->payload;
    while (cursor && *cursor && rows < engine->config.max_rows_per_analysis) {
        char *next = strchr(cursor, '\n');
        if (next) *next = '\0';
        if (*cursor) {
            if (rows == row_capacity) {
                row_capacity *= 2;
                float *grown = realloc(values, row_capacity * dims * sizeof(float));
                if (!grown) break;
                values = grown;
            }
            double extracted[LOGANA_MAX_DIMENSIONS] = {0};
            size_t captured = 0;
            for (size_t d = 0; d < engine->config.numeric_key_count && captured < dims; ++d) {
                double number = 0.0;
                if (logana_extract_number(cursor, engine->config.numeric_keys[d], engine->config.case_sensitive, &number)) {
                    extracted[captured++] = number;
                }
            }
            if (captured < dims) {
                double freeform[LOGANA_MAX_DIMENSIONS] = {0};
                size_t freeform_count = logana_collect_freeform_numbers(cursor, freeform, dims - captured);
                for (size_t i = 0; i < freeform_count && captured < dims; ++i) {
                    extracted[captured++] = freeform[i];
                }
            }
            if (captured == 0) {
                extracted[captured++] = (double)(logana_hash64(cursor, strlen(cursor)) % 1000000ULL) / 1000.0;
                if (captured < dims) extracted[captured++] = (double)strlen(cursor);
                if (captured < dims) extracted[captured++] = (double)(logana_count_chars(cursor, '=') + logana_count_chars(cursor, ':'));
            }
            while (captured < dims) {
                extracted[captured] = extracted[captured - 1];
                ++captured;
            }
            for (size_t d = 0; d < dims; ++d) values[rows * dims + d] = (float)extracted[d];
            ++rows;
        }
        if (!next) break;
        *next = '\n';
        cursor = next + 1;
    }
    job->matrix.values = values;
    job->matrix.row_count = rows;
    job->matrix.dimensions = dims;
    return rows;
}

static void logana_compute_summary(logana_job_t *job) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    job->summary.row_count = rows;
    job->summary.dimensions = dims;
    if (rows == 0 || dims == 0) return;
    for (size_t d = 0; d < dims; ++d) {
        job->summary.min[d] = job->matrix.values[d];
        job->summary.max[d] = job->matrix.values[d];
    }
    double entropy_hist[32] = {0};
    double slope_num = 0.0;
    double slope_den = 0.0;
    double x_mean = ((double)rows - 1.0) / 2.0;
    for (size_t r = 0; r < rows; ++r) {
        double primary = job->matrix.values[r * dims];
        double bucket = fmin(31.0, fmax(0.0, floor(fabs(primary))));
        entropy_hist[(size_t)bucket] += 1.0;
        slope_num += (((double)r - x_mean) * primary);
        slope_den += ((double)r - x_mean) * ((double)r - x_mean);
        for (size_t d = 0; d < dims; ++d) {
            double value = job->matrix.values[r * dims + d];
            job->summary.mean[d] += value;
            if (value < job->summary.min[d]) job->summary.min[d] = value;
            if (value > job->summary.max[d]) job->summary.max[d] = value;
        }
    }
    for (size_t d = 0; d < dims; ++d) {
        job->summary.mean[d] /= (double)rows;
    }
    size_t outliers = 0;
    for (size_t r = 0; r < rows; ++r) {
        for (size_t d = 0; d < dims; ++d) {
            double delta = job->matrix.values[r * dims + d] - job->summary.mean[d];
            job->summary.stddev[d] += delta * delta;
        }
    }
    for (size_t d = 0; d < dims; ++d) {
        job->summary.stddev[d] = sqrt(job->summary.stddev[d] / (double)rows);
    }
    for (size_t r = 0; r < rows; ++r) {
        double score = 0.0;
        for (size_t d = 0; d < dims; ++d) {
            double sigma = job->summary.stddev[d] > 0.0001 ? job->summary.stddev[d] : 1.0;
            double z = (job->matrix.values[r * dims + d] - job->summary.mean[d]) / sigma;
            score += z * z;
        }
        if (score > 9.0) ++outliers;
    }
    for (size_t i = 0; i < 32; ++i) {
        if (entropy_hist[i] <= 0.0) continue;
        double p = entropy_hist[i] / (double)rows;
        job->summary.entropy -= p * log2(p);
    }
    job->summary.slope = slope_den > 0.0 ? (slope_num / slope_den) : 0.0;
    job->summary.outlier_ratio = rows ? ((double)outliers / (double)rows) : 0.0;
}

static size_t logana_assign_cluster_modes(const float *modes, size_t mode_count, size_t dims, float *candidate) {
    for (size_t i = 0; i < mode_count; ++i) {
        if (logana_distance_sq(modes + i * dims, candidate, dims) < 0.04) return i;
    }
    memcpy((float *)(modes + mode_count * dims), candidate, dims * sizeof(float));
    return mode_count;
}

static size_t logana_run_kmeans(logana_job_t *job, size_t k) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (!rows) return 0;
    if (k > rows) k = rows;
    float centers[LOGANA_MAX_DIMENSIONS * 4] = {0};
    memcpy(centers, job->matrix.values, dims * sizeof(float));
    for (size_t c = 1; c < k; ++c) {
        double best_dist = -1.0;
        size_t best_idx = 0;
        for (size_t r = 0; r < rows; ++r) {
            double nearest = 1e18;
            for (size_t j = 0; j < c; ++j) {
                double dist = logana_distance_sq(job->matrix.values + r * dims, centers + j * dims, dims);
                if (dist < nearest) nearest = dist;
            }
            if (nearest > best_dist) {
                best_dist = nearest;
                best_idx = r;
            }
        }
        memcpy(centers + c * dims, job->matrix.values + best_idx * dims, dims * sizeof(float));
    }
    for (size_t iter = 0; iter < 12; ++iter) {
        double accum[LOGANA_MAX_DIMENSIONS * 4] = {0};
        size_t counts[4] = {0};
        for (size_t r = 0; r < rows; ++r) {
            double best_dist = 1e18;
            int best = 0;
            for (size_t c = 0; c < k; ++c) {
                double dist = logana_distance_sq(job->matrix.values + r * dims, centers + c * dims, dims);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = (int)c;
                }
            }
            job->matrix.labels[r] = best;
            counts[best]++;
            for (size_t d = 0; d < dims; ++d) accum[best * dims + d] += job->matrix.values[r * dims + d];
        }
        for (size_t c = 0; c < k; ++c) {
            if (!counts[c]) continue;
            for (size_t d = 0; d < dims; ++d) centers[c * dims + d] = (float)(accum[c * dims + d] / (double)counts[c]);
        }
    }
    return k;
}

static size_t logana_run_dbscan(logana_job_t *job, double eps, size_t min_samples) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    double eps_sq = eps * eps;
    int *labels = job->matrix.labels;
    for (size_t i = 0; i < rows; ++i) labels[i] = -2;
    size_t cluster_id = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] != -2) continue;
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            if (logana_distance_sq(job->matrix.values + i * dims, job->matrix.values + j * dims, dims) <= eps_sq) count++;
        }
        if (count < min_samples) {
            labels[i] = -1;
            continue;
        }
        labels[i] = (int)cluster_id;
        for (size_t j = 0; j < rows; ++j) {
            if (labels[j] == -2 &&
                logana_distance_sq(job->matrix.values + i * dims, job->matrix.values + j * dims, dims) <= eps_sq) {
                labels[j] = (int)cluster_id;
            }
        }
        cluster_id++;
    }
    return cluster_id;
}

static size_t logana_run_birch(logana_job_t *job, double threshold) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    float centroids[LOGANA_MAX_DIMENSIONS * 8] = {0};
    size_t counts[8] = {0};
    size_t cluster_count = 0;
    for (size_t r = 0; r < rows; ++r) {
        size_t best = 0;
        double best_dist = 1e18;
        for (size_t c = 0; c < cluster_count; ++c) {
            double dist = logana_distance_sq(job->matrix.values + r * dims, centroids + c * dims, dims);
            if (dist < best_dist) {
                best_dist = dist;
                best = c;
            }
        }
        if (cluster_count == 0 || best_dist > threshold * threshold) {
            best = cluster_count < 8 ? cluster_count++ : 7;
            memcpy(centroids + best * dims, job->matrix.values + r * dims, dims * sizeof(float));
        }
        job->matrix.labels[r] = (int)best;
        counts[best]++;
        for (size_t d = 0; d < dims; ++d) {
            centroids[best * dims + d] =
                (float)(((double)centroids[best * dims + d] * (double)(counts[best] - 1) +
                         (double)job->matrix.values[r * dims + d]) / (double)counts[best]);
        }
    }
    return cluster_count;
}

static size_t logana_run_mean_shift(logana_job_t *job, double bandwidth) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    float modes[LOGANA_MAX_DIMENSIONS * 16] = {0};
    size_t mode_count = 0;
    for (size_t r = 0; r < rows; ++r) {
        float point[LOGANA_MAX_DIMENSIONS];
        memcpy(point, job->matrix.values + r * dims, dims * sizeof(float));
        for (size_t iter = 0; iter < 6; ++iter) {
            double accum[LOGANA_MAX_DIMENSIONS] = {0};
            size_t count = 0;
            for (size_t j = 0; j < rows; ++j) {
                if (logana_distance_sq(point, job->matrix.values + j * dims, dims) <= bandwidth * bandwidth) {
                    for (size_t d = 0; d < dims; ++d) accum[d] += job->matrix.values[j * dims + d];
                    count++;
                }
            }
            if (!count) break;
            for (size_t d = 0; d < dims; ++d) point[d] = (float)(accum[d] / (double)count);
        }
        size_t label = logana_assign_cluster_modes(modes, mode_count, dims, point);
        if (label == mode_count && mode_count < 16) mode_count++;
        job->matrix.labels[r] = (int)label;
    }
    return mode_count;
}

static size_t logana_run_optics(logana_job_t *job, double eps, size_t min_samples) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    logana_neighbor_t neighbors[LOGANA_MAX_ROWS < 4096 ? LOGANA_MAX_ROWS : 4096];
    if (rows > 4096) rows = 4096;
    for (size_t i = 0; i < rows; ++i) {
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            double dist = sqrt(logana_distance_sq(job->matrix.values + i * dims, job->matrix.values + j * dims, dims));
            if (dist <= eps) count++;
        }
        neighbors[i].idx = i;
        neighbors[i].dist = -(double)count;
    }
    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = i + 1; j < rows; ++j) {
            if (neighbors[j].dist < neighbors[i].dist) {
                logana_neighbor_t tmp = neighbors[i];
                neighbors[i] = neighbors[j];
                neighbors[j] = tmp;
            }
        }
    }
    size_t cluster = 0;
    for (size_t rank = 0; rank < rows; ++rank) {
        size_t idx = neighbors[rank].idx;
        job->matrix.labels[idx] = (rank < min_samples) ? 0 : (int)cluster;
        if ((rank + 1) % min_samples == 0) cluster++;
    }
    return cluster + 1;
}

static size_t logana_run_gmm(logana_job_t *job, size_t k) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (k > 3) k = 3;
    float means[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double variances[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double weights[3] = {0.34, 0.33, 0.33};
    for (size_t c = 0; c < k; ++c) {
        memcpy(means + c * dims, job->matrix.values + (c % rows) * dims, dims * sizeof(float));
        for (size_t d = 0; d < dims; ++d) variances[c * dims + d] = 1.0;
    }
    for (size_t iter = 0; iter < 8; ++iter) {
        double resp_sum[3] = {0};
        double mean_accum[LOGANA_MAX_DIMENSIONS * 3] = {0};
        for (size_t r = 0; r < rows; ++r) {
            double probs[3] = {0};
            double norm = 0.0;
            for (size_t c = 0; c < k; ++c) {
                double dist = 0.0;
                for (size_t d = 0; d < dims; ++d) {
                    double delta = job->matrix.values[r * dims + d] - means[c * dims + d];
                    dist += (delta * delta) / variances[c * dims + d];
                }
                probs[c] = weights[c] * exp(-0.5 * dist);
                norm += probs[c];
            }
            if (norm == 0.0) norm = 1.0;
            int best = 0;
            double best_resp = -1.0;
            for (size_t c = 0; c < k; ++c) {
                double resp = probs[c] / norm;
                resp_sum[c] += resp;
                if (resp > best_resp) {
                    best_resp = resp;
                    best = (int)c;
                }
                for (size_t d = 0; d < dims; ++d) mean_accum[c * dims + d] += resp * job->matrix.values[r * dims + d];
            }
            job->matrix.labels[r] = best;
        }
        for (size_t c = 0; c < k; ++c) {
            double denom = resp_sum[c] > 0.0 ? resp_sum[c] : 1.0;
            weights[c] = resp_sum[c] / (double)rows;
            for (size_t d = 0; d < dims; ++d) means[c * dims + d] = (float)(mean_accum[c * dims + d] / denom);
        }
    }
    return k;
}

static size_t logana_run_agglomerative(logana_job_t *job, size_t target_clusters) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    int active[1024];
    if (rows > 1024) rows = 1024;
    for (size_t i = 0; i < rows; ++i) active[i] = (int)i;
    size_t clusters = rows;
    while (clusters > target_clusters && clusters > 1) {
        double best = 1e18;
        size_t best_i = 0, best_j = 1;
        for (size_t i = 0; i < rows; ++i) {
            if (active[i] < 0) continue;
            for (size_t j = i + 1; j < rows; ++j) {
                if (active[j] < 0) continue;
                double dist = logana_distance_sq(job->matrix.values + i * dims, job->matrix.values + j * dims, dims);
                if (dist < best) {
                    best = dist;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        active[best_j] = active[best_i];
        clusters--;
    }
    int label_map[1024];
    memset(label_map, -1, sizeof(label_map));
    int next_label = 0;
    for (size_t i = 0; i < rows; ++i) {
        int root = active[i];
        if (root < 0) root = (int)i;
        if (label_map[root] < 0) label_map[root] = next_label++;
        job->matrix.labels[i] = label_map[root];
    }
    return (size_t)next_label;
}

static size_t logana_cluster(logana_engine_t *engine, logana_job_t *job) {
    size_t rows = job->matrix.row_count;
    if (!rows) return 0;
    job->matrix.labels = calloc(rows, sizeof(int));
    if (!job->matrix.labels) return 0;
    switch (job->algorithm) {
        case LOGANA_ALGO_KMEANS_PP: return logana_run_kmeans(job, 3);
        case LOGANA_ALGO_DBSCAN: return logana_run_dbscan(job, engine->config.dbscan_eps, engine->config.dbscan_min_samples);
        case LOGANA_ALGO_BIRCH: return logana_run_birch(job, engine->config.dbscan_eps);
        case LOGANA_ALGO_MEAN_SHIFT: return logana_run_mean_shift(job, engine->config.dbscan_eps * 2.0);
        case LOGANA_ALGO_OPTICS: return logana_run_optics(job, engine->config.dbscan_eps * 1.2, engine->config.dbscan_min_samples);
        case LOGANA_ALGO_GMM: return logana_run_gmm(job, 3);
        case LOGANA_ALGO_AGGLOMERATIVE: return logana_run_agglomerative(job, 3);
    }
    return 0;
}

int logana_analyze_job(logana_engine_t *engine, logana_job_t *job) {
    logana_set_job_status(job, LOGANA_JOB_ANALYZING, NULL);
    size_t rows = logana_parse_matrix(engine, job);
    if (!rows) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "no analyzable rows found");
        return -1;
    }
    logana_compute_summary(job);
    job->summary.cluster_count = logana_cluster(engine, job);
    return 0;
}

static void logana_register_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&engine->jobs_lock);
    if (engine->job_count < LOGANA_MAX_JOBS) {
        engine->jobs[engine->job_count++] = job;
    }
    pthread_mutex_unlock(&engine->jobs_lock);
}

logana_job_t *logana_engine_find_job(logana_engine_t *engine, uint64_t job_id) {
    pthread_mutex_lock(&engine->jobs_lock);
    for (size_t i = 0; i < engine->job_count; ++i) {
        if (engine->jobs[i] && engine->jobs[i]->job_id == job_id) {
            pthread_mutex_unlock(&engine->jobs_lock);
            return engine->jobs[i];
        }
    }
    pthread_mutex_unlock(&engine->jobs_lock);
    return NULL;
}

int logana_engine_init(logana_engine_t *engine, const logana_config_t *config) {
    memset(engine, 0, sizeof(*engine));
    engine->config = *config;
    ttak_logger_init(&engine->logger, logana_log_sink, TTAK_LOG_INFO);
    pthread_mutex_init(&engine->jobs_lock, NULL);
    if (logana_queue_init(&engine->ingress_queue, 2048) != 0) return -1;
    if (logana_queue_init(&engine->render_queue, 2048) != 0) return -1;
    uint64_t now = logana_now_ms();
    engine->analysis_pool = ttak_thread_pool_create(engine->config.worker_threads, 0, now);
    engine->render_pool = ttak_thread_pool_create(engine->config.async_render_threads, 0, now);
    if (!engine->analysis_pool || !engine->render_pool) return -1;
    if (pthread_create(&engine->aggregator_thread, NULL, logana_aggregator_main, engine) != 0) return -1;
    if (pthread_create(&engine->render_dispatcher_thread, NULL, logana_render_dispatcher_main, engine) != 0) return -1;
    engine->next_job_id = 1;
    ttak_logger_log(&engine->logger, TTAK_LOG_INFO, "log analytics engine initialized with %zu analysis workers and %zu render workers",
                    engine->config.worker_threads, engine->config.async_render_threads);
    return 0;
}

logana_job_t *logana_engine_submit(logana_engine_t *engine, const char *payload, size_t payload_size, logana_algorithm_t algorithm) {
    logana_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    pthread_mutex_init(&job->lock, NULL);
    job->payload = malloc(payload_size + 1);
    if (!job->payload) {
        free(job);
        return NULL;
    }
    memcpy(job->payload, payload, payload_size);
    job->payload[payload_size] = '\0';
    job->job_id = __atomic_fetch_add(&engine->next_job_id, 1, __ATOMIC_RELAXED);
    job->payload_size = payload_size;
    job->algorithm = algorithm;
    job->created_ms = logana_now_ms();
    job->updated_ms = job->created_ms;
    job->status = LOGANA_JOB_QUEUED;
    logana_register_job(engine, job);
    if (!logana_queue_push(&engine->ingress_queue, job, 100)) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "ingress queue is saturated");
    }
    return job;
}

void logana_job_destroy(logana_job_t *job) {
    if (!job) return;
    pthread_mutex_destroy(&job->lock);
    free(job->payload);
    free(job->matrix.values);
    free(job->matrix.labels);
    free(job->svg);
    free(job->html);
    free(job);
}

void logana_engine_shutdown(logana_engine_t *engine) {
    engine->shutting_down = true;
    logana_queue_close(&engine->ingress_queue);
    logana_queue_close(&engine->render_queue);
    pthread_join(engine->aggregator_thread, NULL);
    pthread_join(engine->render_dispatcher_thread, NULL);
    if (engine->analysis_pool) ttak_thread_pool_destroy(engine->analysis_pool);
    if (engine->render_pool) ttak_thread_pool_destroy(engine->render_pool);
    for (size_t i = 0; i < engine->job_count; ++i) logana_job_destroy(engine->jobs[i]);
    logana_queue_destroy(&engine->ingress_queue);
    logana_queue_destroy(&engine->render_queue);
    pthread_mutex_destroy(&engine->jobs_lock);
}
