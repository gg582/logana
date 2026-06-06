#ifndef LOGANA_COERCION_H
#define LOGANA_COERCION_H

#include "logana/types.h"

#define LOGANA_COERCION_MAX_TRACKED_KEYS 32
#define LOGANA_COERCION_MAX_HISTOGRAM_BUCKETS 4096
#define LOGANA_COERCION_MAX_STRING_LEN     256
#define LOGANA_COERCION_MAX_OVERFLOW_LEN   65536
#define LOGANA_COERCION_INITIAL_ROW_CAP    1024

typedef struct {
    char   key[64];
    char   value[LOGANA_COERCION_MAX_STRING_LEN];
    uint64_t count;
} logana_coercion_hist_entry_t;

typedef struct {
    logana_coercion_hist_entry_t *entries;
    size_t count;
    size_t capacity;
} logana_coercion_histogram_t;

typedef struct {
    char   key[64];
    size_t numeric_hits;
    size_t categorical_hits;
    double median;
    double iqr;
    double mad;
    double lower_bound;
    double upper_bound;
    bool   is_active;       /* true if this key becomes a matrix dimension */
} logana_coercion_key_meta_t;

typedef struct {
    /* Key registry */
    char keys[LOGANA_COERCION_MAX_TRACKED_KEYS][64];
    logana_coercion_key_meta_t meta[LOGANA_COERCION_MAX_TRACKED_KEYS];
    size_t key_count;

    /* Categorical histograms: one per tracked key (only used if key is purely categorical) */
    logana_coercion_histogram_t histograms[LOGANA_COERCION_MAX_TRACKED_KEYS];
    size_t histogram_count;

    /* Overflow catch-all */
    char   overflow[LOGANA_COERCION_MAX_OVERFLOW_LEN];
    size_t overflow_len;

    /* Dense intermediate matrix (row-major, dims = key_count during build) */
    double  *values;
    uint8_t *valid_mask;
    double  *outlier_pressure;
    uint64_t *timestamps;
    uint64_t *categories;
    uint8_t  *formats;
    size_t row_count;
    size_t row_capacity;
    size_t dims;

    /* Scratch buffers for one row */
    double  row_values[LOGANA_COERCION_MAX_TRACKED_KEYS];
    uint8_t row_valid[LOGANA_COERCION_MAX_TRACKED_KEYS];
} logana_coercion_context_t;

int  logana_coercion_init(logana_coercion_context_t *ctx);
void logana_coercion_destroy(logana_coercion_context_t *ctx);

/* Main entry: raw payload -> coerced context */
int logana_coercion_parse_payload(logana_engine_t *engine, logana_job_t *job,
                                  logana_coercion_context_t *ctx);

/* Export coerced context into the job's official feature matrix */
int logana_coercion_export_matrix(logana_coercion_context_t *ctx, logana_job_t *job,
                                  logana_engine_t *engine);

/* Standalone aggressive scalar coercion (exposed for testing / reuse) */
double logana_coerce_scalar(const char *raw, bool *out_is_numeric,
                            char *out_categorical, size_t cat_cap);

#endif
