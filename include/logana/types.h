#ifndef LOGANA_TYPES_H
#define LOGANA_TYPES_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ttak/container/ringbuf.h>
#include <ttak/log/logger.h>
#include <ttak/thread/pool.h>

#define LOGANA_MAX_KEYS 16
#define LOGANA_MAX_ROWS 2000000
#define LOGANA_MAX_DIMENSIONS 8
#define LOGANA_MAX_BATCH_JOBS 256
#define LOGANA_MAX_JOBS 4096

typedef enum {
    LOGANA_JOB_QUEUED = 0,
    LOGANA_JOB_BATCHING,
    LOGANA_JOB_ANALYZING,
    LOGANA_JOB_RENDERING,
    LOGANA_JOB_READY,
    LOGANA_JOB_FAILED
} logana_job_status_t;

typedef enum {
    LOGANA_ALGO_KMEANS_PP = 0,
    LOGANA_ALGO_DBSCAN,
    LOGANA_ALGO_BIRCH,
    LOGANA_ALGO_MEAN_SHIFT,
    LOGANA_ALGO_OPTICS,
    LOGANA_ALGO_GMM,
    LOGANA_ALGO_AGGLOMERATIVE
} logana_algorithm_t;

typedef struct {
    bool case_sensitive;
    double fuzzy_threshold;
    size_t max_rows_per_analysis;
    size_t memory_pool_size_mb;
    uint32_t aggregation_window_ms;
    size_t min_batch_size_bytes;
    size_t worker_threads;
    size_t async_render_threads;
    char timestamp_keys[LOGANA_MAX_KEYS][64];
    size_t timestamp_key_count;
    char numeric_keys[LOGANA_MAX_KEYS][64];
    size_t numeric_key_count;
    char category_keys[LOGANA_MAX_KEYS][64];
    size_t category_key_count;
    char nested_paths[LOGANA_MAX_KEYS][96];
    size_t nested_path_count;
    logana_algorithm_t default_algorithm;
    double dbscan_eps;
    size_t dbscan_min_samples;
    bool enable_time_series_trend;
    bool enable_outlier_detection;
    bool enable_correlation_matrix;
    bool enable_shannon_entropy;
    bool enable_linear_regression;
    char theme[32];
    int canvas_width;
    int canvas_height;
    char color_palette[8][16];
    size_t color_count;
} logana_config_t;

typedef struct {
    size_t row_count;
    size_t dimensions;
    double mean[LOGANA_MAX_DIMENSIONS];
    double min[LOGANA_MAX_DIMENSIONS];
    double max[LOGANA_MAX_DIMENSIONS];
    double stddev[LOGANA_MAX_DIMENSIONS];
    double entropy;
    double slope;
    double outlier_ratio;
    size_t cluster_count;
} logana_analysis_summary_t;

typedef struct {
    float *values;
    size_t row_count;
    size_t dimensions;
    int *labels;
} logana_feature_matrix_t;

typedef struct logana_job {
    uint64_t job_id;
    pthread_mutex_t lock;
    logana_job_status_t status;
    uint64_t created_ms;
    uint64_t updated_ms;
    size_t payload_size;
    char *payload;
    logana_algorithm_t algorithm;
    logana_feature_matrix_t matrix;
    logana_analysis_summary_t summary;
    char *svg;
    char *html;
    char error[256];
} logana_job_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    ttak_ringbuf_t *ring;
    bool closed;
} logana_queue_t;

typedef struct {
    struct logana_engine *engine;
    logana_job_t **jobs;
    size_t job_count;
    size_t total_bytes;
} logana_batch_t;

typedef struct logana_engine {
    logana_config_t config;
    ttak_logger_t logger;
    ttak_thread_pool_t *analysis_pool;
    ttak_thread_pool_t *render_pool;
    logana_queue_t ingress_queue;
    logana_queue_t render_queue;
    pthread_t aggregator_thread;
    pthread_t render_dispatcher_thread;
    bool shutting_down;
    pthread_mutex_t jobs_lock;
    logana_job_t *jobs[LOGANA_MAX_JOBS];
    size_t job_count;
    uint64_t next_job_id;
} logana_engine_t;

typedef struct {
    logana_engine_t *engine;
    uint16_t port;
} logana_server_opts_t;

#endif
