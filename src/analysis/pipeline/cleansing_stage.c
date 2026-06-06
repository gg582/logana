#include "logana/pipeline.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */
#define LOGANA_CLAMP_INT64_MAX  1e15
#define LOGANA_SCALE_EXPLODE    1e15
#define LOGANA_COUNTER_MAX      1e9
#define LOGANA_MEM_MB_MAX       1048576.0   /* 1 TiB in MiB */
#define LOGANA_CPU_MAX          100.0

/* -------------------------------------------------------------------------- */
/* Case-insensitive key matcher                                               */
/* -------------------------------------------------------------------------- */
static bool key_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; ++i) {
        bool match = true;
        for (size_t j = 0; j < nlen; ++j) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Metric classification helpers                                              */
/* -------------------------------------------------------------------------- */
typedef enum {
    METRIC_GENERIC = 0,
    METRIC_LATENCY,
    METRIC_CPU,
    METRIC_MEMORY,
    METRIC_ACTIVE_USERS,  /* counters that may suffer scale explosion */
} metric_class_t;

static metric_class_t classify_metric(const char *key) {
    if (!key) return METRIC_GENERIC;
    if (key_contains_ci(key, "latency") || key_contains_ci(key, "duration"))
        return METRIC_LATENCY;
    if (key_contains_ci(key, "cpu") || key_contains_ci(key, "cpu_util"))
        return METRIC_CPU;
    if (key_contains_ci(key, "mem") || key_contains_ci(key, "rss") || key_contains_ci(key, "memory"))
        return METRIC_MEMORY;
    if (key_contains_ci(key, "active_users") || key_contains_ci(key, "user_count") ||
        key_contains_ci(key, "request_count") || key_contains_ci(key, "event_count"))
        return METRIC_ACTIVE_USERS;
    return METRIC_GENERIC;
}

/* -------------------------------------------------------------------------- */
/* Core cleansing logic                                                       */
/* -------------------------------------------------------------------------- */
static void cleanse_cell(double *value, uint8_t *mask, metric_class_t mclass) {
    double v = *value;

    /* Rule 4: NaN, Infinity, -Infinity exclusion */
    if (!isfinite(v)) {
        *mask = 0;
        *value = 0.0;
        return;
    }

    /* Rule 3: Floating-point scale explosion prevention */
    if (fabs(v) > LOGANA_SCALE_EXPLODE) {
        *value = (v > 0.0) ? LOGANA_CLAMP_INT64_MAX : -LOGANA_CLAMP_INT64_MAX;
        /* Keep valid — we rescued it from collapse territory */
        return;
    }

    /* Also clamp anything that would overflow int64 representation */
    if (fabs(v) > LOGANA_CLAMP_INT64_MAX) {
        *value = (v > 0.0) ? LOGANA_CLAMP_INT64_MAX : -LOGANA_CLAMP_INT64_MAX;
        return;
    }

    /* Rule 2: Strict metric boundary boxing */
    switch (mclass) {
        case METRIC_LATENCY:
            /* Negative latency is invalid: convert to absolute value */
            if (v < 0.0) {
                *value = fabs(v);
            }
            break;

        case METRIC_CPU:
            /* Hard physical limits [0.0, 100.0] */
            if (v < 0.0) *value = 0.0;
            else if (v > LOGANA_CPU_MAX) *value = LOGANA_CPU_MAX;
            break;

        case METRIC_MEMORY:
            /* Realistic physical threshold: 0 .. 1 TiB in MB */
            if (v < 0.0) *value = 0.0;
            else if (v > LOGANA_MEM_MB_MAX) *value = LOGANA_MEM_MB_MAX;
            break;

        case METRIC_ACTIVE_USERS:
            /* Counters: no negative, clamp upper bound to prevent axis collapse */
            if (v < 0.0) *value = 0.0;
            else if (v > LOGANA_COUNTER_MAX) *value = LOGANA_COUNTER_MAX;
            break;

        default:
            /* Generic numeric: silently clamp extreme negatives/positives */
            if (v < -LOGANA_CLAMP_INT64_MAX) *value = -LOGANA_CLAMP_INT64_MAX;
            else if (v > LOGANA_CLAMP_INT64_MAX) *value = LOGANA_CLAMP_INT64_MAX;
            break;
    }
}

/* -------------------------------------------------------------------------- */
/* Row sparsity guard                                                         */
/* If a row has zero valid cells after cleansing, mark it for dropping.       */
/* We compact the matrix in-place to avoid an extra allocation.               */
/* -------------------------------------------------------------------------- */
static size_t compact_dropped_rows(logana_feature_matrix_t *m) {
    size_t dims = m->dimensions;
    size_t write = 0;
    for (size_t r = 0; r < m->row_count; ++r) {
        bool any_valid = false;
        for (size_t d = 0; d < dims; ++d) {
            if (m->valid_mask[r * dims + d]) {
                any_valid = true;
                break;
            }
        }
        if (!any_valid) continue; /* drop this row entirely */
        if (write != r) {
            memcpy(m->values + write * dims, m->values + r * dims, dims * sizeof(double));
            memcpy(m->valid_mask + write * dims, m->valid_mask + r * dims, dims * sizeof(uint8_t));
            m->timestamps[write] = m->timestamps[r];
            m->categories[write] = m->categories[r];
            m->formats[write] = m->formats[r];
        }
        ++write;
    }
    return write;
}

/* -------------------------------------------------------------------------- */
/* Pipeline stage implementation                                              */
/* -------------------------------------------------------------------------- */
static int cleansing_init(void *state, const logana_config_t *config) {
    (void)state; (void)config;
    return 0;
}

static int cleansing_process(void *state, logana_pipeline_context_t *ctx) {
    (void)state;
    logana_feature_matrix_t *m = &ctx->working_matrix;
    logana_job_t *job = ctx->job;
    const logana_config_t *cfg = ctx->config;

    size_t rows = m->row_count;
    size_t dims = m->dimensions;
    if (rows == 0 || dims == 0) return 0;

    /* Build per-dimension metric class cache */
    metric_class_t mclasses[LOGANA_MAX_DIMENSIONS];
    for (size_t d = 0; d < dims; ++d) {
        if (d < cfg->numeric_key_count) {
            mclasses[d] = classify_metric(cfg->numeric_keys[d]);
        } else {
            mclasses[d] = METRIC_GENERIC;
        }
    }

    /* Pass 1: cell-level cleansing */
    size_t nullified[LOGANA_MAX_DIMENSIONS] = {0};
    for (size_t r = 0; r < rows; ++r) {
        for (size_t d = 0; d < dims; ++d) {
            size_t idx = r * dims + d;
            if (m->valid_mask && !m->valid_mask[idx]) {
                nullified[d]++;
                continue;
            }
            double old = m->values[idx];
            uint8_t old_mask = m->valid_mask ? m->valid_mask[idx] : 1;
            cleanse_cell(&m->values[idx], &old_mask, mclasses[d]);
            if (m->valid_mask) m->valid_mask[idx] = old_mask;
            if (!old_mask) nullified[d]++;
            (void)old; /* available for debug logging if needed */
        }
    }

    /* Pass 2: drop rows that became entirely invalid after cleansing */
    size_t new_rows = compact_dropped_rows(m);
    if (new_rows < rows) {
        rows = new_rows;
        m->row_count = rows;
    }

    /* Pass 3: per-dimension sparsity guard.
       If a dimension is >80 % null after cleansing, zero it out so
       downstream feature engineering can drop it cleanly.              */
    for (size_t d = 0; d < dims; ++d) {
        if (rows > 0 && (double)nullified[d] / (double)rows > 0.80) {
            for (size_t r = 0; r < rows; ++r) {
                m->valid_mask[r * dims + d] = 0;
            }
        }
    }

    /* Recompute summary so downstream stages see cleansed statistics */
    logana_compute_summary(job);
    ctx->summary = job->summary;

    return 0;
}

static void cleansing_cleanup(void *state) {
    (void)state;
}

static const logana_pipeline_stage_vtable_t cleansing_vtable = {
    .name    = "cleansing",
    .init    = cleansing_init,
    .process = cleansing_process,
    .cleanup = cleansing_cleanup,
};

logana_pipeline_stage_t logana_cleansing_stage(void) {
    return (logana_pipeline_stage_t){ &cleansing_vtable, NULL };
}
