#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define SCHEMA_DENSITY_THRESHOLD  0.20
#define MIN_CLUSTERABLE_ROWS      6
#define MIN_ACTIVE_DIMENSIONS     1
#define CV_SIGNAL_THRESHOLD       0.05
#define DQI_MINIMUM               0.15
#define VIABILITY_MINIMUM         0.10
#define SPATIAL_DENSITY_HIGH      0.50
#define UNIFORMITY_HIGH           0.80
#define SPATIAL_DENSITY_LOW       0.30
#define UNIFORMITY_LOW            0.50
#define KNN_K                     3

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
    double mean;
    double stddev;
    double median;
    double mad;
    double cv;
    double min;
    double max;
    size_t valid_count;
    bool is_active;
} ColumnStats;

typedef struct {
    size_t target_k;
    bool fallback_to_scatterplot;
    ClusterAlgorithm chosen_algo;
    double trend_slope;
    bool trend_bypassed;
    size_t active_dimensions;
    size_t effective_dimensions;
    size_t total_rows;
    double data_quality_index;
    double cluster_viability;
    double adaptive_epsilon;
    const char *abort_reason;
} AnalyticsResult;

static int compare_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static double compute_median(double *arr, size_t n)
{
    if (n == 0) return NAN;
    qsort(arr, n, sizeof(double), compare_double);
    if (n % 2 == 1) {
        return arr[n / 2];
    }
    return (arr[n / 2 - 1] + arr[n / 2]) * 0.5;
}

static LogDataset* create_mock_dataset(void);
static void destroy_dataset(LogDataset *ds);
static size_t run_column_density_inspector(LogDataset *ds, ColumnStats *stats);
static double compute_partial_distance(const LogDataset *ds, size_t i, size_t j, size_t active_dims, const ColumnStats *stats);
static double evaluate_spatial_density(const LogDataset *ds, size_t active_dims, const ColumnStats *stats);
static double evaluate_schema_uniformity(const LogDataset *ds, size_t active_dims, const ColumnStats *stats);
static size_t compute_effective_dimensionality(size_t active_dims, const ColumnStats *stats, size_t num_fields);
static double compute_data_quality_index(const LogDataset *ds, size_t active_dims, const ColumnStats *stats, double spatial_density, double uniformity);
static double evaluate_cluster_viability(const LogDataset *ds, size_t active_dims, size_t effective_dims, double dqi);
static double compute_k_distance_epsilon(const LogDataset *ds, size_t active_dims, const ColumnStats *stats);
static double compute_trend_slope(const LogDataset *ds, bool bypass, const ColumnStats *stats);
static AnalyticsResult execute_log_pipeline(LogDataset *dataset, bool is_auto_mode);

/* Stage 1: Dense Schema Parser & Robust Statistical Validation Layer.
   Computes per-column density, robust mean, standard deviation, median,
   median absolute deviation (MAD), and coefficient of variation (CV).
   Columns below density threshold or with near-zero variance are culled. */
static size_t run_column_density_inspector(LogDataset *ds, ColumnStats *stats)
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

        stats[col].valid_count = present_count;
        stats[col].is_active = false;
        stats[col].mean = 0.0;
        stats[col].stddev = 0.0;
        stats[col].median = 0.0;
        stats[col].mad = 0.0;
        stats[col].cv = 0.0;
        stats[col].min = NAN;
        stats[col].max = NAN;

        if (present_count == 0) {
            ds->is_sparse_dropped[col] = true;
            continue;
        }

        double *valid_values = malloc(present_count * sizeof(double));
        if (!valid_values) {
            ds->is_sparse_dropped[col] = true;
            continue;
        }

        size_t idx = 0;
        double sum = 0.0;
        double min_val = INFINITY;
        double max_val = -INFINITY;
        for (size_t row = 0; row < ds->num_rows; ++row) {
            if (ds->rows[row].cells[col].is_valid) {
                double v = ds->rows[row].cells[col].value;
                valid_values[idx++] = v;
                sum += v;
                if (v < min_val) min_val = v;
                if (v > max_val) max_val = v;
            }
        }

        stats[col].min = min_val;
        stats[col].max = max_val;
        stats[col].mean = sum / (double)present_count;
        stats[col].median = compute_median(valid_values, present_count);

        double mad_sum = 0.0;
        double var_sum = 0.0;
        for (size_t i = 0; i < present_count; ++i) {
            double d = fabs(valid_values[i] - stats[col].median);
            mad_sum += d;
            double dv = valid_values[i] - stats[col].mean;
            var_sum += dv * dv;
        }
        stats[col].mad = mad_sum / (double)present_count;
        stats[col].stddev = sqrt(var_sum / (double)present_count);
        stats[col].cv = (fabs(stats[col].mean) > DBL_EPSILON) ? (stats[col].stddev / fabs(stats[col].mean)) : 0.0;

        free(valid_values);

        if (ds->column_density[col] < SCHEMA_DENSITY_THRESHOLD || stats[col].cv < CV_SIGNAL_THRESHOLD) {
            ds->is_sparse_dropped[col] = true;
        } else {
            ds->is_sparse_dropped[col] = false;
            stats[col].is_active = true;
            active_dimensions++;
        }
    }
    return active_dimensions;
}

/* Missing-data-aware partial Euclidean distance with per-dimension
   z-score normalization and expansion penalty for absent coordinates.
   Distance is scaled by sqrt(active_dims / valid_pairs) to prevent
   short-circuit bias on sparse vectors. */
static double compute_partial_distance(const LogDataset *ds, size_t i, size_t j, size_t active_dims, const ColumnStats *stats)
{
    double dist_sq = 0.0;
    size_t pairwise_valid = 0;

    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (ds->is_sparse_dropped[d] || !stats[d].is_active) {
            continue;
        }
        bool vi = ds->rows[i].cells[d].is_valid;
        bool vj = ds->rows[j].cells[d].is_valid;
        if (vi && vj) {
            double zi = 0.0;
            double zj = 0.0;
            if (stats[d].stddev > DBL_EPSILON) {
                zi = (ds->rows[i].cells[d].value - stats[d].mean) / stats[d].stddev;
                zj = (ds->rows[j].cells[d].value - stats[d].mean) / stats[d].stddev;
            }
            double delta = zi - zj;
            dist_sq += delta * delta;
            pairwise_valid++;
        }
    }

    if (pairwise_valid == 0) {
        return INFINITY;
    }
    double scaling = sqrt((double)active_dims / (double)pairwise_valid);
    return sqrt(dist_sq) * scaling;
}

/* Spatial density derived from normalized partial distances.
   Uses the harmonic mean of pairwise distances to reduce outlier
   sensitivity compared to a raw arithmetic mean. */
static double evaluate_spatial_density(const LogDataset *ds, size_t active_dims, const ColumnStats *stats)
{
    if (active_dims == 0 || ds->num_rows < 2) {
        return 0.0;
    }

    double inv_dist_sum = 0.0;
    size_t valid_pairs = 0;

    for (size_t i = 0; i < ds->num_rows; ++i) {
        for (size_t j = i + 1; j < ds->num_rows; ++j) {
            double d = compute_partial_distance(ds, i, j, active_dims, stats);
            if (isfinite(d) && d > DBL_EPSILON) {
                inv_dist_sum += 1.0 / d;
                valid_pairs++;
            }
        }
    }

    if (valid_pairs == 0 || inv_dist_sum < DBL_EPSILON) {
        return 0.0;
    }
    double harmonic_mean = (double)valid_pairs / inv_dist_sum;
    return harmonic_mean;
}

/* Schema uniformity based on the Gini coefficient of column densities
   and the coefficient of variation of non-dropped columns.
   High uniformity approaches 1.0. */
static double evaluate_schema_uniformity(const LogDataset *ds, size_t active_dims, const ColumnStats *stats)
{
    if (active_dims == 0) {
        return 0.0;
    }

    double *densities = malloc(active_dims * sizeof(double));
    if (!densities) return 0.0;

    size_t idx = 0;
    double mean_cv = 0.0;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (stats[d].is_active) {
            densities[idx++] = ds->column_density[d];
            mean_cv += stats[d].cv;
        }
    }
    mean_cv /= (double)active_dims;

    qsort(densities, active_dims, sizeof(double), compare_double);
    double gini = 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < active_dims; ++i) {
        sum += densities[i];
        gini += (2.0 * (double)(i + 1) - (double)active_dims - 1.0) * densities[i];
    }
    double gini_coeff = (sum > DBL_EPSILON) ? (gini / ((double)active_dims * sum)) : 0.0;
    double density_uniformity = 1.0 - gini_coeff;

    free(densities);

    double cv_score = exp(-mean_cv);
    return 0.6 * density_uniformity + 0.4 * cv_score;
}

/* Effective dimensionality: count of active columns that exhibit
   non-degenerate variance after robust standardization. */
static size_t compute_effective_dimensionality(size_t active_dims, const ColumnStats *stats, size_t num_fields)
{
    size_t eff = 0;
    for (size_t d = 0; d < num_fields; ++d) {
        if (stats[d].is_active && stats[d].stddev > DBL_EPSILON) {
            eff++;
        }
    }
    return (eff > 0) ? eff : active_dims;
}

/* Data Quality Index (DQI): composite [0,1] score aggregating
   fill ratio, signal ratio (CV-based), and spatial separation index.
   Low DQI blocks fragile clustering attempts. */
static double compute_data_quality_index(const LogDataset *ds, size_t active_dims, const ColumnStats *stats, double spatial_density, double uniformity)
{
    if (active_dims == 0 || ds->num_rows == 0) {
        return 0.0;
    }

    double fill_sum = 0.0;
    size_t signal_count = 0;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (stats[d].is_active) {
            fill_sum += ds->column_density[d];
            if (stats[d].cv >= CV_SIGNAL_THRESHOLD) {
                signal_count++;
            }
        }
    }
    double fill_ratio = fill_sum / (double)active_dims;
    double signal_ratio = (double)signal_count / (double)active_dims;
    double separation_index = spatial_density * uniformity;

    return 0.35 * fill_ratio + 0.35 * signal_ratio + 0.30 * separation_index;
}

/* Cluster Viability Score: fuses DQI with the effective sample-to-dimension
   density ratio. Scores below VIABILITY_MINIMUM trigger mandatory abort
   to prevent collapse on statistically underpowered matrices. */
static double evaluate_cluster_viability(const LogDataset *ds, size_t active_dims, size_t effective_dims, double dqi)
{
    if (active_dims == 0 || effective_dims == 0 || ds->num_rows == 0) {
        return 0.0;
    }
    double sample_density = (double)ds->num_rows / (double)effective_dims;
    double density_score = 1.0 - exp(-sample_density / 10.0);
    return dqi * density_score;
}

/* k-Distance Profile: computes the KNN_K-th nearest neighbor distance for
   every row using normalized partial distances, sorts the profile, and
   returns the 80th percentile as a statistically grounded epsilon for
   density-based clustering. */
static double compute_k_distance_epsilon(const LogDataset *ds, size_t active_dims, const ColumnStats *stats)
{
    if (ds->num_rows <= KNN_K + 1 || active_dims == 0) {
        return 0.5;
    }

    double *k_distances = calloc(ds->num_rows, sizeof(double));
    if (!k_distances) return 0.5;

    for (size_t i = 0; i < ds->num_rows; ++i) {
        double *distances = calloc(ds->num_rows - 1, sizeof(double));
        if (!distances) {
            free(k_distances);
            return 0.5;
        }
        size_t idx = 0;
        for (size_t j = 0; j < ds->num_rows; ++j) {
            if (i == j) continue;
            double d = compute_partial_distance(ds, i, j, active_dims, stats);
            distances[idx++] = isfinite(d) ? d : INFINITY;
        }
        qsort(distances, idx, sizeof(double), compare_double);
        k_distances[i] = distances[KNN_K - 1];
        free(distances);
    }

    qsort(k_distances, ds->num_rows, sizeof(double), compare_double);
    size_t p80 = (size_t)floor(0.8 * (double)(ds->num_rows - 1));
    double epsilon = k_distances[p80];
    free(k_distances);

    return (epsilon > DBL_EPSILON && isfinite(epsilon)) ? epsilon : 0.5;
}

/* Stage 3: Domain Isolation for Trend Estimation.
   Linear regression on the first active dimension, row index as the
   independent variable. Bypass emits NAN to suppress artifacts. */
static double compute_trend_slope(const LogDataset *ds, bool bypass, const ColumnStats *stats)
{
    if (bypass) {
        return NAN;
    }

    size_t target_col = (size_t)-1;
    for (size_t d = 0; d < ds->num_fields; ++d) {
        if (stats[d].is_active) {
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
   Statistical pre-validation (DQI, viability, effective dimensions)
   intercepts degenerate inputs before any clustering unit executes.
   Rule A: viability or dimension collapse.
   Rule B: sparse / heterogeneous routing to DBSCAN/BIRCH.
   Rule C: dense / uniform routing to K-means++. */
static AnalyticsResult execute_log_pipeline(LogDataset *dataset, bool is_auto_mode)
{
    AnalyticsResult res = {0};
    res.total_rows = dataset->num_rows;
    res.chosen_algo = ALGO_NONE;
    res.trend_slope = NAN;
    res.trend_bypassed = false;
    res.fallback_to_scatterplot = false;
    res.target_k = 0;
    res.data_quality_index = 0.0;
    res.cluster_viability = 0.0;
    res.adaptive_epsilon = 0.5;
    res.abort_reason = NULL;

    ColumnStats *stats = calloc(dataset->num_fields, sizeof(ColumnStats));
    if (!stats) {
        res.fallback_to_scatterplot = true;
        res.trend_bypassed = true;
        res.target_k = 1;
        res.abort_reason = "RULE_A: memory allocation failure for statistics";
        return res;
    }

    size_t active_dims = run_column_density_inspector(dataset, stats);
    res.active_dimensions = active_dims;

    if (active_dims < MIN_ACTIVE_DIMENSIONS) {
        res.target_k = 1;
        res.fallback_to_scatterplot = true;
        res.trend_bypassed = true;
        res.trend_slope = NAN;
        res.abort_reason = "RULE_A: zero active dimensions after robust culling";
        free(stats);
        return res;
    }

    double spatial_density = evaluate_spatial_density(dataset, active_dims, stats);
    double uniformity = evaluate_schema_uniformity(dataset, active_dims, stats);
    size_t effective_dims = compute_effective_dimensionality(active_dims, stats, dataset->num_fields);
    res.effective_dimensions = effective_dims;

    double dqi = compute_data_quality_index(dataset, active_dims, stats, spatial_density, uniformity);
    res.data_quality_index = dqi;

    double viability = evaluate_cluster_viability(dataset, active_dims, effective_dims, dqi);
    res.cluster_viability = viability;

    res.adaptive_epsilon = compute_k_distance_epsilon(dataset, active_dims, stats);

    /* Augmented Rule A: statistically underpowered or low-quality matrices
       are trapped regardless of raw row count to prevent variance collapse. */
    if (dataset->num_rows < MIN_CLUSTERABLE_ROWS || viability < VIABILITY_MINIMUM || dqi < DQI_MINIMUM) {
        res.target_k = 1;
        res.fallback_to_scatterplot = true;
        res.trend_bypassed = true;
        res.trend_slope = NAN;
        if (dataset->num_rows < MIN_CLUSTERABLE_ROWS) {
            res.abort_reason = "RULE_A: row count below clustering threshold";
        } else if (dqi < DQI_MINIMUM) {
            res.abort_reason = "RULE_A: data quality index below minimum";
        } else {
            res.abort_reason = "RULE_A: cluster viability below minimum";
        }
        free(stats);
        return res;
    }

    if (is_auto_mode) {
        bool dense_uniform = (spatial_density > SPATIAL_DENSITY_HIGH) && (uniformity > UNIFORMITY_HIGH);
        bool sparse_heterogeneous = (spatial_density < SPATIAL_DENSITY_LOW) || (uniformity < UNIFORMITY_LOW);

        if (dense_uniform) {
            res.chosen_algo = ALGO_KMEANS;
            double raw_k = sqrt((double)dataset->num_rows / (2.0 * (double)effective_dims));
            res.target_k = (raw_k < 2.0) ? 2 : (size_t)raw_k;
        } else if (sparse_heterogeneous) {
            if (spatial_density < 0.2 || effective_dims > dataset->num_rows / 3) {
                res.chosen_algo = ALGO_DBSCAN;
                res.target_k = 0;
            } else {
                res.chosen_algo = ALGO_BIRCH;
                double raw_k = sqrt((double)dataset->num_rows / (2.0 * (double)effective_dims));
                res.target_k = (raw_k < 2.0) ? 2 : (size_t)raw_k;
            }
        } else {
            res.chosen_algo = ALGO_BIRCH;
            res.target_k = 2;
        }
    } else {
        res.chosen_algo = ALGO_KMEANS;
        res.target_k = 3;
    }

    res.trend_slope = compute_trend_slope(dataset, false, stats);
    free(stats);
    return res;
}

/* Mock 5-row heterogeneous sparse matrix engineered to trigger
   Rule A via row-count exhaustion, verifying interceptor logic. */
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


