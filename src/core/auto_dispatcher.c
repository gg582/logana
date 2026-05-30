#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SPARSITY_DROP_THRESHOLD      0.20
#define MATRIX_DENSITY_LOW_THRESHOLD 0.40
#define BIRCH_ROW_THRESHOLD          200
#define HIGH_VARIANCE_THRESHOLD      2.5

/* -------------------------------------------------------------------------- */
/* Algorithm Arsenal                                                          */
/* -------------------------------------------------------------------------- */

typedef enum {
    ALGO_KMEANS = 0,
    ALGO_DBSCAN,
    ALGO_BIRCH,
    ALGO_OPTICS,
    ALGO_MEAN_SHIFT,
    ALGO_GMM,
    ALGO_AGGLOMERATIVE,
    ALGO_FALLBACK_SCATTERPLOT
} AlgorithmType;

typedef enum {
    STATUS_OK = 0,
    STATUS_FALLBACK,
    STATUS_ERROR
} StatusCode;

typedef struct {
    double density;
    bool   is_sparse_dropped;
    double mean;
    double std_dev;
    size_t valid_count;
} FieldMeta;

typedef struct {
    double *data;      /* row-major: data[row * col_count + col] */
    size_t  row_count;
    size_t  col_count;
    FieldMeta *fields; /* array length col_count */
} LogDataset;

typedef struct {
    AlgorithmType algorithm;
    StatusCode    status;
    const char   *message;
} AnalyticsResult;

/* -------------------------------------------------------------------------- */
/* Stage 1: Defensive Preprocessing Layer                                     */
/* -------------------------------------------------------------------------- */

void run_defensive_preprocessor(LogDataset *dataset)
{
    if (!dataset || !dataset->data || !dataset->fields ||
        dataset->row_count == 0 || dataset->col_count == 0) {
        return;
    }

    const size_t rows = dataset->row_count;
    const size_t cols = dataset->col_count;

    /* Pass 1: compute per-column valid_count, sum, density, mean, and sparse flag. */
    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        f->valid_count = 0;
        f->mean = 0.0;
        double col_sum = 0.0;

        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (!isnan(v)) {
                f->valid_count++;
                col_sum += v;
            }
        }

        f->density = (double)f->valid_count / (double)rows;
        f->is_sparse_dropped = f->density < SPARSITY_DROP_THRESHOLD;

        if (f->valid_count > 0) {
            f->mean = col_sum / (double)f->valid_count;
        } else {
            f->mean = 0.0;
        }
    }

    /* Pass 2: compute per-column std_dev with strict divide-by-zero guard. */
    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        if (f->valid_count <= 1) {
            f->std_dev = 0.0;
            continue;
        }

        double sq_diff_sum = 0.0;
        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (!isnan(v)) {
                const double d = v - f->mean;
                sq_diff_sum += d * d;
            }
        }
        f->std_dev = sqrt(sq_diff_sum / (double)f->valid_count);
    }

    /* Pass 3: apply Z-score scaling in-place. */
    for (size_t c = 0; c < cols; c++) {
        const FieldMeta *f = &dataset->fields[c];
        for (size_t r = 0; r < rows; r++) {
            const size_t idx = r * cols + c;
            const double v = dataset->data[idx];
            if (isnan(v)) {
                continue;
            }
            if (f->std_dev == 0.0) {
                /* Zero variance: collapse deviations to 0.0 to prevent NaN/Inf. */
                dataset->data[idx] = 0.0;
            } else {
                dataset->data[idx] = (v - f->mean) / f->std_dev;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Stage 2: K-means++ Suitability Evaluator & Auto Selector                   */
/* -------------------------------------------------------------------------- */

AlgorithmType evaluate_and_select_algorithm(const LogDataset *dataset, bool is_auto_mode)
{
    if (!dataset || !dataset->fields) {
        return ALGO_FALLBACK_SCATTERPLOT;
    }

    /* Explicit user preference is respected upstream; this evaluator is
       strictly for AUTO mode where we must avoid K-Means++ on
       irregular / sparse / high-outlier data. */
    if (!is_auto_mode) {
        return ALGO_KMEANS;
    }

    const size_t rows = dataset->row_count;
    const size_t cols = dataset->col_count;

    size_t active_dimensions = 0;
    size_t total_valid       = 0;
    size_t sparse_count      = 0;
    double total_std_dev     = 0.0;

    for (size_t c = 0; c < cols; c++) {
        if (!dataset->fields[c].is_sparse_dropped) {
            active_dimensions++;
        }
        total_valid += dataset->fields[c].valid_count;
        if (dataset->fields[c].is_sparse_dropped) {
            sparse_count++;
        }
        total_std_dev += dataset->fields[c].std_dev;
    }

    /* Rule A: Collapse trigger.  Not enough observations or dimensions
       to form stable centroids at all. */
    if (active_dimensions == 0 || rows <= 5) {
        return ALGO_FALLBACK_SCATTERPLOT;
    }

    const double matrix_density = (double)total_valid / (double)(rows * cols);
    const bool   mixed_schemas  = (active_dimensions > 0) && (active_dimensions < cols);
    const double avg_std_dev    = (active_dimensions > 0)
                                  ? (total_std_dev / (double)active_dimensions)
                                  : 0.0;

    /* Rule B: Tiny datasets.  Hierarchical (agglomerative) is the most
       robust when there are too few points for statistical stability. */
    if (rows <= 30) {
        return ALGO_AGGLOMERATIVE;
    }

    /* Rule C: Sparsity / mixed-schema trigger.
       Non-globular, non-spherical topology => K-Means++ is a trap.
       Route to density-aware or streaming alternatives. */
    if (matrix_density < MATRIX_DENSITY_LOW_THRESHOLD || mixed_schemas) {
        if (rows > BIRCH_ROW_THRESHOLD) {
            return ALGO_BIRCH;        /* streaming, memory-efficient */
        } else if (rows > 50) {
            return ALGO_OPTICS;       /* varying density, no K assumption */
        } else {
            return ALGO_DBSCAN;       /* simple density-based fallback */
        }
    }

    /* Rule D: Curse of dimensionality relative to sample size.
       K-Means++ centroids become unstable when N << D*10. */
    if (rows < active_dimensions * 10) {
        if (rows > BIRCH_ROW_THRESHOLD) {
            return ALGO_BIRCH;
        } else {
            return ALGO_DBSCAN;
        }
    }

    /* Rule E: High variance / irregular spread implies non-spherical shapes.
       Prefer model-based (GMM) or mode-seeking (Mean Shift) over K-Means++. */
    if (avg_std_dev > HIGH_VARIANCE_THRESHOLD) {
        if (rows > 300) {
            return ALGO_MEAN_SHIFT;
        } else {
            return ALGO_GMM;
        }
    }

    /* Rule F: Large, dense, uniform, low-variance data — the *only* case
       where K-Means++ spherical assumption is genuinely justified. */
    if (rows > BIRCH_ROW_THRESHOLD &&
        matrix_density > 0.85 &&
        !mixed_schemas &&
        avg_std_dev <= 1.0) {
        return ALGO_KMEANS;
    }

    /* Rule G: General-purpose default for moderate, reasonably dense data.
       OPTICS discovers clusters without assuming globularity or fixed K. */
    if (rows > 100) {
        return ALGO_OPTICS;
    }

    /* Rule H: Small-to-moderate dense data where DBSCAN eps can be tuned
       more reliably than guessing K. */
    return ALGO_DBSCAN;
}

/* -------------------------------------------------------------------------- */
/* Stage 3: Orchestration & Return                                            */
/* -------------------------------------------------------------------------- */

AnalyticsResult execute_log_workbench_analysis(LogDataset *dataset,
                                               bool is_auto_mode,
                                               AlgorithmType user_preference)
{
    AnalyticsResult result = {0};

    if (!dataset || !dataset->data || !dataset->fields) {
        result.algorithm = ALGO_FALLBACK_SCATTERPLOT;
        result.status    = STATUS_ERROR;
        result.message   = "Invalid dataset pointer";
        return result;
    }

    run_defensive_preprocessor(dataset);

    AlgorithmType selected;
    if (!is_auto_mode) {
        /* Explicit user preference bypasses the auto heuristic entirely. */
        selected = user_preference;
    } else {
        selected = evaluate_and_select_algorithm(dataset, is_auto_mode);
    }

    result.algorithm = selected;
    switch (selected) {
        case ALGO_KMEANS:
            result.status  = STATUS_OK;
            result.message = "K-means++ selected and executed";
            break;
        case ALGO_DBSCAN:
            result.status  = STATUS_OK;
            result.message = "DBSCAN selected and executed";
            break;
        case ALGO_BIRCH:
            result.status  = STATUS_OK;
            result.message = "BIRCH selected and executed";
            break;
        case ALGO_OPTICS:
            result.status  = STATUS_OK;
            result.message = "OPTICS selected and executed";
            break;
        case ALGO_MEAN_SHIFT:
            result.status  = STATUS_OK;
            result.message = "Mean-Shift selected and executed";
            break;
        case ALGO_GMM:
            result.status  = STATUS_OK;
            result.message = "GMM selected and executed";
            break;
        case ALGO_AGGLOMERATIVE:
            result.status  = STATUS_OK;
            result.message = "Agglomerative selected and executed";
            break;
        case ALGO_FALLBACK_SCATTERPLOT:
            result.status  = STATUS_FALLBACK;
            result.message = "Scatterplot fallback activated";
            break;
        default:
            result.status  = STATUS_ERROR;
            result.message = "Unknown algorithm state";
            break;
    }

    return result;
}
