#ifndef LOGANA_TYPES_H
#define LOGANA_TYPES_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ttak/container/ringbuf.h>
#include <ttak/log/logger.h>
#include <ttak/thread/pool.h>

#define LOGANA_MAX_KEYS 16
#define LOGANA_MAX_ROWS 2000000
#define LOGANA_MAX_DIMENSIONS 32
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
    LOGANA_ALGO_AGGLOMERATIVE,
    LOGANA_ALGO_AUTO,
    LOGANA_ALGO_FALLBACK_SCATTERPLOT
} logana_algorithm_t;

typedef enum {
    LOGANA_DIST_EUCLIDEAN = 0,
    LOGANA_DIST_MANHATTAN,
    LOGANA_DIST_ZSCORE
} logana_distance_metric_t;

typedef struct {
    /* DBSCAN / OPTICS (shared) */
    double dbscan_eps;
    size_t dbscan_min_samples;
    bool has_dbscan_eps;
    bool has_dbscan_min_samples;

    /* BIRCH */
    double birch_threshold;
    bool has_birch_threshold;

    /* GMM */
    size_t gmm_max_components;
    size_t gmm_em_iterations;
    bool has_gmm_max_components;
    bool has_gmm_em_iterations;

    /* K-means++ */
    size_t kmeans_max_k;
    size_t kmeans_n_init;
    size_t kmeans_iterations;
    bool has_kmeans_max_k;
    bool has_kmeans_n_init;
    bool has_kmeans_iterations;

    /* Mean Shift */
    double mean_shift_bandwidth;
    size_t mean_shift_iterations;
    bool has_mean_shift_bandwidth;
    bool has_mean_shift_iterations;

    /* Agglomerative */
    size_t agglomerative_target_clusters;
    bool has_agglomerative_target_clusters;

    /* ------------------------------------------------------------------ */
    /* Dynamic / scale-dependent thresholds (replaces hard-coded magic    */
    /* numbers scattered across strategies and math modules).             */
    /* ------------------------------------------------------------------ */

    /* Mean-shift: merge two modes if squared distance is below this
     * fraction of the bandwidth. Default: derived from data scale. */
    double mean_shift_merge_threshold;
    bool   has_mean_shift_merge_threshold;

    /* BIRCH: when no explicit threshold is given, use
     * median_pairwise_distance * birch_auto_threshold. Default 0.5. */
    double birch_auto_threshold;
    bool   has_birch_auto_threshold;

    /* Outlier detection: z-score squared threshold.
     * Default corresponds to ~3 sigma (9.0). */
    double outlier_zscore_threshold;
    bool   has_outlier_zscore_threshold;

    /* Winsorization / MAD boundary: multiplier for sigma estimation.
     * Default 5.0. */
    double winsorize_sigma_multiplier;
    bool   has_winsorize_sigma_multiplier;

    /* Floor for stddev used to prevent division-by-zero in z-score
     * distances and outlier scoring.  Expressed as a fraction of the
     * dynamic range (max - min) per dimension.  Default 1e-4. */
    double min_stddev_fraction;
    bool   has_min_stddev_fraction;

    /* DBSCAN/OPTICS eps multiplier when knee detection is used.
     * Default 1.05 for DBSCAN, 1.2 for OPTICS. */
    double dbscan_eps_multiplier;
    bool   has_dbscan_eps_multiplier;
} logana_cluster_options_t;

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
    logana_distance_metric_t distance_metric;
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
    double cluster_balance;      /* min/max cluster ratio; 1.0 = perfect balance */
    double schema_drift;         /* 0.0 = uniform format, 1.0 = max mixed */
    logana_cluster_options_t cluster_options;
} logana_analysis_summary_t;

typedef struct {
    int    *labels;              /* Per-row cluster assignment; -1 = noise */
    bool   *is_noise;            /* Explicit noise flag to prevent UI indexing bugs */
    size_t  row_count;
    size_t  cluster_count;       /* Logical clusters EXCLUDING noise */
    size_t  noise_count;
    logana_algorithm_t algorithm;
    double  silhouette_score;
    double  davies_bouldin_index;
} logana_cluster_result_t;

typedef struct {
    double *values;
    uint64_t *timestamps;        /* Normalized epoch milliseconds per row */
    uint8_t *valid_mask;         /* Per-dimension validity: 1 = valid, 0 = invalid/parsed trap */
    uint8_t *formats;            /* Per-row format tag: 0=JSON, 1=KV, 2=Text */
    size_t row_count;
    size_t dimensions;
    int *labels;
    uint64_t *categories;        /* Per-row category hash for cardinality grouping */
    double *outlier_pressure;    /* Preserves true magnitude for statistical outliers (Rule 3) */
} logana_feature_matrix_t;

typedef struct logana_job {
    uint64_t job_id;
    _Atomic size_t ref_count;
    _Atomic logana_job_status_t status;
    _Atomic uint64_t created_ms;
    _Atomic uint64_t updated_ms;
    size_t payload_size;
    char *payload;
    logana_algorithm_t algorithm;
    logana_cluster_options_t cluster_options;
    logana_feature_matrix_t matrix;
    logana_analysis_summary_t summary;
    logana_cluster_result_t   result;
    _Atomic(char *) svg;
    _Atomic(char *) html;
    char error[256];
    struct logana_engine *engine;
    size_t processed_lines_count;
} logana_job_t;

typedef struct {
    _Atomic int64_t seq;
    void *item;
} logana_lf_slot_t;

typedef struct {
    logana_lf_slot_t *slots;
    size_t capacity;
    size_t mask;
    _Atomic size_t head;
    _Atomic size_t tail;
    _Atomic bool closed;
} logana_queue_t;

typedef struct {
    struct logana_engine *engine;
    logana_job_t **jobs;
    size_t job_count;
    size_t total_bytes;
} logana_batch_t;

#define LOGANA_MAX_AUTO_CACHE 64

typedef struct {
    uint64_t fingerprint;          /* payload hash + dimensional signature */
    logana_algorithm_t selected;
    uint64_t last_used_ms;
} logana_auto_cache_entry_t;

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
    _Atomic uint32_t jobs_seq;
    _Atomic size_t job_count;
    logana_job_t *jobs[LOGANA_MAX_JOBS];
    _Atomic uint64_t next_job_id;
    logana_auto_cache_entry_t auto_cache[LOGANA_MAX_AUTO_CACHE];
    _Atomic size_t auto_cache_count;
} logana_engine_t;

typedef struct {
    logana_engine_t *engine;
    uint16_t port;
} logana_server_opts_t;

#endif
