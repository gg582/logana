#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define SCHEMA_DENSITY_THRESHOLD  0.20
#define MIN_CLUSTERABLE_ROWS      6
#define MIN_ACTIVE_DIMENSIONS     1
#define SPATIAL_DENSITY_HIGH      0.50
#define UNIFORMITY_HIGH           0.80
#define SPATIAL_DENSITY_LOW       0.30
#define UNIFORMITY_LOW            0.50

typedef enum {
    ALGO_NONE = 0,
    ALGO_KMEANS,
    ALGO_BIRCH,
    ALGO_DBSCAN,
    ALGO_GMM,
    ALGO_MEAN_SHIFT,
    ALGO_AGGLOMERATIVE
} ClusterAlgorithm;

typedef struct {
    double value;
    bool is_valid;
} LogCell;

typedef struct {
    LogCell *cells;
    size_t num_fields;
} LogRow;

typedef struct {
    LogRow *rows;
    size_t num_rows;
    size_t num_fields;
    char **field_names;
    bool *is_sparse_dropped;
    double *column_density;
} LogDataset;

typedef struct {
    size_t target_k;
    bool fallback_to_scatterplot;
    ClusterAlgorithm chosen_algo;
    double trend_slope;
    bool trend_bypassed;
    size_t active_dimensions;
    size_t total_rows;
    const char *abort_reason;
} AnalyticsResult;

static LogDataset* create_mock_dataset(void);
static void destroy_dataset(LogDataset *ds);
static size_t run_column_density_inspector(LogDataset *ds);
static double evaluate_spatial_density(const LogDataset *ds, size_t active_dims);
static double evaluate_schema_uniformity(const LogDataset *ds, size_t active_dims);
static double compute_trend_slope(const LogDataset *ds, bool bypass);
static AnalyticsResult execute_log_pipeline(LogDataset *dataset, bool is_auto_mode);

/* Stage 1: Dense Schema Parser & Bitmap Validation Layer.
   Computes per-column presence rates and marks sub-threshold fields
   as structurally dropped. Active dimension count is returned. */
static size_t run_column_density_inspector(LogDataset *ds)
{
    size_t active_dimensions = 0;

    for (size_t col = 0; col < ds->num_fields; ++col) {
        size_t present_count = 0;
        for (size_t row = 0; row < ds->num_rows; ++row) {
            if (ds->rows[row].cells[col].is_valid) {
                present_count++;
            }
        }
        ds->column_density[col] = (double)present_count / (double)ds->num_rows;
        if (ds->column_density[col] < SCHEMA_DENSITY_THRESHOLD) {
            ds->is_sparse_dropped[col] = true;
        } else {
            ds->is_sparse_dropped[col] = false;
            active_dimensions++;
        }
    }
    return active_dimensions;
}

/* Pairwise spatial density heuristic. Only fully observed active-dimension
   vectors contribute to the mean distance accumulator. Returns inverse
   mean Euclidean distance to keep high-is-dense semantics. */
static double evaluate_spatial_density(const LogDataset *ds, size_t active_dims)
{
    if (active_dims == 0 || ds->num_rows == 0) {
        return 0.0;
    }

    double distance_accumulator = 0.0;
    size_t valid_pairs = 0;

    for (size_t i = 0; i < ds->num_rows; ++i) {
        for (size_t j = i + 1; j < ds->num_rows; ++j) {
            double dist_sq = 0.0;
            size_t pairwise_valid = 0;
            for (size_t d = 0; d < ds->num_fields; ++d) {
                if (ds->is_sparse_dropped[d]) {
                    continue;
                }
                bool vi = ds->rows[i].cells[d].is_valid;
                bool vj = ds->rows[j].cells[d].is_valid;
                if (vi && vj) {
                    double delta = ds->rows[i].cells[d].value - ds->rows[j].cells[d].value;
                    dist_sq += delta * delta;
                    pairwise_valid++;
                }
            }
            if (pairwise_valid == active_dims) {
                distance_accumulator += sqrt(dist_sq);
                valid_pairs++;
            }
        }
    }

    if (valid_pairs == 0) {
        return 0.0;
    }
    double mean_distance = distance_accumulator / (double)valid_pairs;
    return (mean_distance > 0.0) ? (1.0 / mean_distance) : 0.0;
}

/* Schema uniformity derived from standard deviation of column densities.
   Exponential decay maps low variance to high uniformity near 1.0. */
static double evaluate_schema_uniformity(const LogDataset *ds, size_t active_dims)
{
    if (active_dims == 0) {
        return 0.0;
    }

    double mean_density = 0.0;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (!ds->is_sparse_dropped[d]) {
            mean_density += ds->column_density[d];
        }
    }
    mean_density /= (double)active_dims;

    double variance = 0.0;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (!ds->is_sparse_dropped[d]) {
            double delta = ds->column_density[d] - mean_density;
            variance += delta * delta;
        }
    }
    variance /= (double)active_dims;

    return exp(-sqrt(variance));
}

/* Stage 3: Domain Isolation for Trend Estimation.
   Operates exclusively on the first retained dimension to prevent
   cross-domain metric leakage. Bypass emits NAN to block artifact
   generation when the pipeline aborts to scatterplot fallback. */
static double compute_trend_slope(const LogDataset *ds, bool bypass)
{
    if (bypass) {
        return NAN;
    }

    size_t target_col = (size_t)-1;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (!ds->is_sparse_dropped[d]) {
            target_col = d;
            break;
        }
    }
    if (target_col == (size_t)-1) {
        return NAN;
    }

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xy = 0.0;
    double sum_xx = 0.0;
    size_t valid_n = 0;

    for (size_t i = 0; i < ds->num_rows; ++i) {
        if (ds->rows[i].cells[target_col].is_valid) {
            double x = (double)i;
            double y = ds->rows[i].cells[target_col].value;
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_xx += x * x;
            valid_n++;
        }
    }

    if (valid_n < 2) {
        return NAN;
    }

    double denom = valid_n * sum_xx - sum_x * sum_x;
    if (fabs(denom) < DBL_EPSILON) {
        return NAN;
    }

    return (valid_n * sum_xy - sum_x * sum_y) / denom;
}

/* Stage 2: Adaptive Auto-Mode Dispatcher & Heuristic Guardrails.
   Density and uniformity metrics drive algorithmic routing.
   Rule A traps undersampled or degenerate matrices before any
   clustering unit is invoked. */
static AnalyticsResult execute_log_pipeline(LogDataset *dataset, bool is_auto_mode)
{
    AnalyticsResult res = {0};
    res.total_rows = dataset->num_rows;
    res.chosen_algo = ALGO_NONE;
    res.trend_slope = NAN;
    res.trend_bypassed = false;
    res.fallback_to_scatterplot = false;
    res.target_k = 0;
    res.abort_reason = NULL;

    size_t active_dims = run_column_density_inspector(dataset);
    res.active_dimensions = active_dims;

    /* Rule A (Early Trap): degenerate input envelope. */
    if (active_dims < MIN_ACTIVE_DIMENSIONS || dataset->num_rows < MIN_CLUSTERABLE_ROWS) {
        res.target_k = 1;
        res.fallback_to_scatterplot = true;
        res.trend_bypassed = true;
        res.trend_slope = NAN;
        if (dataset->num_rows < MIN_CLUSTERABLE_ROWS) {
            res.abort_reason = "RULE_A: row count below clustering threshold";
        } else {
            res.abort_reason = "RULE_A: zero active dimensions after sparsity cull";
        }
        return res;
    }

    double spatial_density = evaluate_spatial_density(dataset, active_dims);
    double uniformity = evaluate_schema_uniformity(dataset, active_dims);

    if (is_auto_mode) {
        bool dense_uniform = (spatial_density > SPATIAL_DENSITY_HIGH) && (uniformity > UNIFORMITY_HIGH);
        bool sparse_heterogeneous = (spatial_density < SPATIAL_DENSITY_LOW) || (uniformity < UNIFORMITY_LOW);

        if (dense_uniform) {
            /* Rule C (Dense Fall-Through): K-means++ viable. */
            res.chosen_algo = ALGO_KMEANS;
            double raw_k = sqrt((double)dataset->num_rows / 2.0);
            res.target_k = (raw_k < 2.0) ? 2 : (size_t)raw_k;
        } else if (sparse_heterogeneous) {
            /* Rule B (Sparsity Bypass): avoid centroid-based distortion. */
            if (spatial_density < 0.2) {
                res.chosen_algo = ALGO_DBSCAN;
                res.target_k = 0;
            } else {
                res.chosen_algo = ALGO_BIRCH;
                double raw_k = sqrt((double)dataset->num_rows / 2.0);
                res.target_k = (raw_k < 2.0) ? 2 : (size_t)raw_k;
            }
        } else {
            res.chosen_algo = ALGO_BIRCH;
            res.target_k = 2;
        }
    } else {
        /* Manual override defaults to K-means++ with fixed K. */
        res.chosen_algo = ALGO_KMEANS;
        res.target_k = 3;
    }

    res.trend_slope = compute_trend_slope(dataset, false);
    return res;
}

/* Mock 5-row heterogeneous sparse matrix. Fields are deliberately
   non-overlapping to stress the bitmap validation and density inspector.
   Row count is set to 5 to guarantee Rule A interception. */
static LogDataset* create_mock_dataset(void)
{
    static char *field_names[] = {
        "timestamp", "entropy", "throughput", "latency", "error_rate"
    };
    const size_t num_fields = 5;
    const size_t num_rows = 5;

    LogDataset *ds = malloc(sizeof(LogDataset));
    if (!ds) {
        return NULL;
    }
    ds->num_rows = num_rows;
    ds->num_fields = num_fields;
    ds->field_names = field_names;
    ds->rows = calloc(num_rows, sizeof(LogRow));
    ds->is_sparse_dropped = calloc(num_fields, sizeof(bool));
    ds->column_density = calloc(num_fields, sizeof(double));

    if (!ds->rows || !ds->is_sparse_dropped || !ds->column_density) {
        destroy_dataset(ds);
        return NULL;
    }

    for (size_t r = 0; r < num_rows; ++r) {
        ds->rows[r].num_fields = num_fields;
        ds->rows[r].cells = calloc(num_fields, sizeof(LogCell));
        if (!ds->rows[r].cells) {
            destroy_dataset(ds);
            return NULL;
        }
        for (size_t c = 0; c < num_fields; ++c) {
            ds->rows[r].cells[c].is_valid = false;
            ds->rows[r].cells[c].value = 0.0;
        }
    }

    /* Row 0: timestamp and entropy present. */
    ds->rows[0].cells[0].value = 1.0;
    ds->rows[0].cells[0].is_valid = true;
    ds->rows[0].cells[1].value = 0.82;
    ds->rows[0].cells[1].is_valid = true;

    /* Row 1: timestamp only. */
    ds->rows[1].cells[0].value = 2.0;
    ds->rows[1].cells[0].is_valid = true;

    /* Row 2: entropy and latency present. */
    ds->rows[2].cells[1].value = 0.61;
    ds->rows[2].cells[1].is_valid = true;
    ds->rows[2].cells[3].value = 124.0;
    ds->rows[2].cells[3].is_valid = true;

    /* Row 3: throughput and error_rate present. */
    ds->rows[3].cells[2].value = 512.0;
    ds->rows[3].cells[2].is_valid = true;
    ds->rows[3].cells[4].value = 0.015;
    ds->rows[3].cells[4].is_valid = true;

    /* Row 4: timestamp, throughput, latency present. */
    ds->rows[4].cells[0].value = 5.0;
    ds->rows[4].cells[0].is_valid = true;
    ds->rows[4].cells[2].value = 498.0;
    ds->rows[4].cells[2].is_valid = true;
    ds->rows[4].cells[3].value = 97.0;
    ds->rows[4].cells[3].is_valid = true;

    return ds;
}

static void destroy_dataset(LogDataset *ds)
{
    if (!ds) {
        return;
    }
    if (ds->rows) {
        for (size_t i = 0; i < ds->num_rows; ++i) {
            free(ds->rows[i].cells);
        }
        free(ds->rows);
    }
    free(ds->is_sparse_dropped);
    free(ds->column_density);
    free(ds);
}

int main(void)
{
    LogDataset *dataset = create_mock_dataset();
    if (!dataset) {
        return EXIT_FAILURE;
    }

    AnalyticsResult result = execute_log_pipeline(dataset, true);

    printf("=== Log Pipeline Analytics Result ===\n");
    printf("Total Rows:          %zu\n", result.total_rows);
    printf("Active Dimensions:   %zu\n", result.active_dimensions);
    printf("Target K:            %zu\n", result.target_k);
    printf("Fallback Scatter:    %s\n", result.fallback_to_scatterplot ? "true" : "false");
    printf("Trend Bypassed:      %s\n", result.trend_bypassed ? "true" : "false");
    printf("Chosen Algorithm:    %d\n", (int)result.chosen_algo);
    printf("Trend Slope:         %f\n", result.trend_slope);
    printf("Abort Reason:        %s\n", result.abort_reason ? result.abort_reason : "N/A");
    printf("\n=== Column Density Inspector ===\n");
    for (size_t i = 0; i < dataset->num_fields; ++i) {
        printf("Field %-12s  density=%.2f  dropped=%s\n",
               dataset->field_names[i],
               dataset->column_density[i],
               dataset->is_sparse_dropped[i] ? "true" : "false");
    }

    destroy_dataset(dataset);
    return EXIT_SUCCESS;
}
