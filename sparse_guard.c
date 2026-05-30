#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_NAME_MAX 64
#define SPARSITY_THRESHOLD 0.20
#define MIN_ROWS_FOR_CLUSTERING 5

/* Per-field telemetry accumulator. Tracks raw observations,
 * nullability bitmap, derived moments, and exclusion state. */
typedef struct FieldMetric {
    char name[FIELD_NAME_MAX];
    double *values;
    bool *valid;
    size_t capacity;
    size_t row_count;
    double mean;
    double std_dev;
    bool is_sparse_dropped;
} FieldMetric;

/* Dataset container. Owns field descriptors and global
 * clustering control flags. */
typedef struct LogDataset {
    FieldMetric *fields;
    size_t field_count;
    size_t row_count;
} LogDataset;

/* ------------------------------------------------------------------ */
/* Construction / destruction                                           */
/* ------------------------------------------------------------------ */

static LogDataset* dataset_alloc(size_t field_count, size_t row_capacity)
{
    LogDataset *ds = calloc(1, sizeof(LogDataset));
    if (!ds) return NULL;

    ds->fields = calloc(field_count, sizeof(FieldMetric));
    if (!ds->fields) {
        free(ds);
        return NULL;
    }
    ds->field_count = field_count;

    for (size_t i = 0; i < field_count; ++i) {
        FieldMetric *f = &ds->fields[i];
        f->values = calloc(row_capacity, sizeof(double));
        f->valid  = calloc(row_capacity, sizeof(bool));
        f->capacity = row_capacity;
        if (!f->values || !f->valid) {
            /* Simplified error path: caller must free partially built ds. */
            return ds;
        }
    }
    return ds;
}

static void dataset_free(LogDataset *ds)
{
    if (!ds) return;
    for (size_t i = 0; i < ds->field_count; ++i) {
        free(ds->fields[i].values);
        free(ds->fields[i].valid);
    }
    free(ds->fields);
    free(ds);
}

/* ------------------------------------------------------------------ */
/* Ingestion helpers                                                    */
/* ------------------------------------------------------------------ */

static void dataset_push_row(LogDataset *ds)
{
    ds->row_count++;
}

static void field_push(FieldMetric *f, size_t row_idx, double value, bool is_valid)
{
    if (row_idx >= f->capacity) return;
    f->values[row_idx] = value;
    f->valid[row_idx] = is_valid;
    if (row_idx >= f->row_count) f->row_count = row_idx + 1;
}

/* ------------------------------------------------------------------ */
/* 1. Sparsity filter: mark fields below density threshold.             */
/* ------------------------------------------------------------------ */

static void filter_sparse_fields(LogDataset *ds, double threshold)
{
    for (size_t i = 0; i < ds->field_count; ++i) {
        FieldMetric *f = &ds->fields[i];
        size_t present = 0;
        size_t eval_rows = ds->row_count < f->row_count ? ds->row_count : f->row_count;

        for (size_t r = 0; r < eval_rows; ++r) {
            if (f->valid[r]) present++;
        }

        double density = (eval_rows > 0) ? ((double)present / (double)eval_rows) : 0.0;
        /* Hard cutoff: at or below threshold is treated as too sparse
         * to contribute to geometric distance. */
        if (density <= threshold) {
            f->is_sparse_dropped = true;
        } else {
            f->is_sparse_dropped = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. Statistical profiler: mean/std_dev with n-1 guard.                */
/* ------------------------------------------------------------------ */

static void compute_field_statistics(FieldMetric *f)
{
    size_t valid_count = 0;
    double sum = 0.0;

    size_t eval_rows = f->row_count;
    for (size_t r = 0; r < eval_rows; ++r) {
        if (f->valid[r]) {
            sum += f->values[r];
            valid_count++;
        }
    }

    f->mean = (valid_count > 0) ? (sum / (double)valid_count) : 0.0;

    /* Guard: std_dev requires n-1 > 0. Bypass if insufficient samples. */
    if (valid_count <= 1) {
        f->std_dev = 0.0;
        return;
    }

    double sq_err_sum = 0.0;
    for (size_t r = 0; r < eval_rows; ++r) {
        if (f->valid[r]) {
            double delta = f->values[r] - f->mean;
            sq_err_sum += delta * delta;
        }
    }

    f->std_dev = sqrt(sq_err_sum / (double)(valid_count - 1));
}

static void profile_dataset(LogDataset *ds)
{
    for (size_t i = 0; i < ds->field_count; ++i) {
        if (!ds->fields[i].is_sparse_dropped) {
            compute_field_statistics(&ds->fields[i]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 3. Adaptive clustering strategy controller.                          */
/* ------------------------------------------------------------------ */

typedef struct ClusteringControl {
    size_t target_k;
    bool fallback_to_scatterplot;
    size_t active_dimensions;
} ClusteringControl;

static ClusteringControl evaluate_clustering_strategy(const LogDataset *ds)
{
    ClusteringControl ctrl = {
        .target_k = 2,
        .fallback_to_scatterplot = false,
        .active_dimensions = 0
    };

    /* Count dimensions that survived sparsity filtering. */
    for (size_t i = 0; i < ds->field_count; ++i) {
        if (!ds->fields[i].is_sparse_dropped) {
            ctrl.active_dimensions++;
        }
    }

    /* Structural collapse guards:
     * - Zero geometry: no features to cluster.
     * - Under-populated stream: K-means degenerates when K >= N. */
    if (ctrl.active_dimensions == 0 || ds->row_count <= MIN_ROWS_FOR_CLUSTERING) {
        ctrl.target_k = 1;
        ctrl.fallback_to_scatterplot = true;
    }

    return ctrl;
}

/* ------------------------------------------------------------------ */
/* Diagnostic emitter                                                   */
/* ------------------------------------------------------------------ */

static void emit_diagnostics(const LogDataset *ds, const ClusteringControl *ctrl)
{
    printf("=== Sparse Guard Diagnostics ===\n");
    printf("Rows ingested: %zu\n", ds->row_count);
    printf("Active dimensions: %zu\n", ctrl->active_dimensions);
    printf("Target K: %zu\n", ctrl->target_k);
    printf("Fallback to scatterplot: %s\n",
           ctrl->fallback_to_scatterplot ? "true" : "false");
    printf("\nPer-field summary:\n");

    for (size_t i = 0; i < ds->field_count; ++i) {
        const FieldMetric *f = &ds->fields[i];
        size_t valid_count = 0;
        for (size_t r = 0; r < f->row_count; ++r) {
            if (f->valid[r]) valid_count++;
        }
        printf("  %-16s | valid: %zu | mean: %8.3f | std: %8.3f | dropped: %s\n",
               f->name,
               valid_count,
               f->mean,
               f->std_dev,
               f->is_sparse_dropped ? "yes" : "no");
    }
    printf("================================\n");
}

/* ------------------------------------------------------------------ */
/* 4. Cardinality guard: group rows with identical validity patterns.   */
/* ------------------------------------------------------------------ */

typedef struct RowGroup {
    uint32_t pattern;   /* bitmask of valid dimensions */
    size_t *indices;
    size_t count;
    size_t capacity;
} RowGroup;

static uint32_t compute_validity_pattern(const LogDataset *ds, size_t row)
{
    uint32_t pattern = 0;
    for (size_t i = 0; i < ds->field_count; ++i) {
        if (row < ds->fields[i].row_count && ds->fields[i].valid[row]) {
            pattern |= (1u << (unsigned)i);
        }
    }
    return pattern;
}

static void cardinality_guard(const LogDataset *ds)
{
    RowGroup groups[32] = {0};
    size_t group_count = 0;

    for (size_t r = 0; r < ds->row_count; ++r) {
        uint32_t pat = compute_validity_pattern(ds, r);
        size_t g = 0;
        for (; g < group_count; ++g) {
            if (groups[g].pattern == pat) break;
        }
        if (g == group_count) {
            if (group_count >= 32) break;
            groups[g].pattern = pat;
            groups[g].capacity = 8;
            groups[g].indices = malloc(groups[g].capacity * sizeof(size_t));
            group_count++;
        }
        if (groups[g].count == groups[g].capacity) {
            groups[g].capacity *= 2;
            groups[g].indices = realloc(groups[g].indices, groups[g].capacity * sizeof(size_t));
        }
        groups[g].indices[groups[g].count++] = r;
    }

    printf("\n=== Cardinality Guard ===\n");
    printf("Groups by validity pattern: %zu\n", group_count);
    for (size_t g = 0; g < group_count; ++g) {
        printf("  Pattern 0x%04x | rows: %zu | fields: ", groups[g].pattern, groups[g].count);
        for (size_t f = 0; f < ds->field_count; ++f) {
            if (groups[g].pattern & (1u << (unsigned)f)) printf("%s ", ds->fields[f].name);
        }
        printf("\n");
    }
    printf("=========================\n");

    for (size_t g = 0; g < group_count; ++g) {
        free(groups[g].indices);
    }
}

/* ------------------------------------------------------------------ */
/* Main: mock 5-row sparse collapse scenario.                           */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Setup: 5 fields, capacity for 5 rows. */
    size_t n_fields = 5;
    size_t n_rows = 5;
    LogDataset *ds = dataset_alloc(n_fields, n_rows);
    if (!ds) {
        return EXIT_FAILURE;
    }

    strncpy(ds->fields[0].name, "cpu_temp", FIELD_NAME_MAX);
    strncpy(ds->fields[1].name, "mem_mbps", FIELD_NAME_MAX);
    strncpy(ds->fields[2].name, "disk_iops", FIELD_NAME_MAX);
    strncpy(ds->fields[3].name, "net_pps", FIELD_NAME_MAX);
    strncpy(ds->fields[4].name, "fan_rpm", FIELD_NAME_MAX);

    /* Row 0: only cpu_temp present. */
    dataset_push_row(ds);
    field_push(&ds->fields[0], 0, 72.5, true);
    field_push(&ds->fields[1], 0, 0.0, false);
    field_push(&ds->fields[2], 0, 0.0, false);
    field_push(&ds->fields[3], 0, 0.0, false);
    field_push(&ds->fields[4], 0, 0.0, false);

    /* Row 1: only mem_mbps present. */
    dataset_push_row(ds);
    field_push(&ds->fields[0], 1, 0.0, false);
    field_push(&ds->fields[1], 1, 2048.0, true);
    field_push(&ds->fields[2], 1, 0.0, false);
    field_push(&ds->fields[3], 1, 0.0, false);
    field_push(&ds->fields[4], 1, 0.0, false);

    /* Row 2: only disk_iops present. */
    dataset_push_row(ds);
    field_push(&ds->fields[0], 2, 0.0, false);
    field_push(&ds->fields[1], 2, 0.0, false);
    field_push(&ds->fields[2], 2, 150.0, true);
    field_push(&ds->fields[3], 2, 0.0, false);
    field_push(&ds->fields[4], 2, 0.0, false);

    /* Row 3: only net_pps present. */
    dataset_push_row(ds);
    field_push(&ds->fields[0], 3, 0.0, false);
    field_push(&ds->fields[1], 3, 0.0, false);
    field_push(&ds->fields[2], 3, 0.0, false);
    field_push(&ds->fields[3], 3, 45000.0, true);
    field_push(&ds->fields[4], 3, 0.0, false);

    /* Row 4: only fan_rpm present. */
    dataset_push_row(ds);
    field_push(&ds->fields[0], 4, 0.0, false);
    field_push(&ds->fields[1], 4, 0.0, false);
    field_push(&ds->fields[2], 4, 0.0, false);
    field_push(&ds->fields[3], 4, 0.0, false);
    field_push(&ds->fields[4], 4, 3200.0, true);

    /* Pipeline execution order:
     * 1. Sparsity filter isolates single-occurrence fields.
     * 2. Statistical profiler computes moments only on retained fields.
     * 3. Clustering controller evaluates geometric viability. */
    filter_sparse_fields(ds, SPARSITY_THRESHOLD);
    profile_dataset(ds);
    ClusteringControl ctrl = evaluate_clustering_strategy(ds);

    emit_diagnostics(ds, &ctrl);
    cardinality_guard(ds);

    dataset_free(ds);
    return EXIT_SUCCESS;
}
