#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
    double *data;       /* row-major: data[row * col_count + col] */
    size_t  row_count;
    size_t  col_count;
    FieldMeta *fields; /* array length col_count */
} LogDataset;

typedef struct {
    AlgorithmType algorithm;
    StatusCode    status;
    const char   *message;
} AnalyticsResult;

typedef struct {
    double sparsity_drop_density;
    double low_matrix_density;
    double high_variance_stddev;
    double high_dimension_sample_ratio;
    double density_weight;
    double dimension_weight;
    double variance_weight;
    double scale_weight;
    double zscore_clip;
} RoutingConfig;

typedef struct {
    size_t rows;
    size_t cols;
    size_t active_dimensions;
    size_t sparse_dimensions;
    double matrix_density;
    double active_ratio;
    double sample_to_dimension_ratio;
    double avg_stddev;
} DatasetProfile;

typedef struct {
    AlgorithmType algorithm;
    double density_affinity;
    double high_dimension_affinity;
    double variance_affinity;
    double scale_affinity;
    double score;
} AlgorithmScore;

static const RoutingConfig ROUTING_DEFAULTS = {
    .sparsity_drop_density = 0.20,
    .low_matrix_density = 0.40,
    .high_variance_stddev = 2.50,
    .high_dimension_sample_ratio = 10.0,
    .density_weight = 0.30,
    .dimension_weight = 0.25,
    .variance_weight = 0.25,
    .scale_weight = 0.20,
    .zscore_clip = 10.0,
};

static bool log_dataset_valid(const LogDataset *dataset) {
    return dataset &&
           dataset->data &&
           dataset->fields &&
           dataset->row_count > 0 &&
           dataset->col_count > 0;
}

/* -------------------------------------------------------------------------- */
/* Stage 1: Defensive Preprocessing Layer                                     */
/* -------------------------------------------------------------------------- */

static void run_defensive_preprocessor_with_config(LogDataset *dataset,
                                                   const RoutingConfig *config) {
    if (!log_dataset_valid(dataset) || !config) return;

    const size_t rows = dataset->row_count;
    const size_t cols = dataset->col_count;

    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        f->valid_count = 0;
        f->mean = 0.0;
        double col_sum = 0.0;

        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (isfinite(v)) {
                f->valid_count++;
                col_sum += v;
            }
        }

        f->density = (double)f->valid_count / (double)rows;
        f->is_sparse_dropped = f->density < config->sparsity_drop_density;
        f->mean = f->valid_count > 0 ? col_sum / (double)f->valid_count : 0.0;
    }

    for (size_t c = 0; c < cols; c++) {
        FieldMeta *f = &dataset->fields[c];
        if (f->valid_count <= 1) {
            f->std_dev = 0.0;
            continue;
        }

        double sq_diff_sum = 0.0;
        for (size_t r = 0; r < rows; r++) {
            const double v = dataset->data[r * cols + c];
            if (isfinite(v)) {
                const double d = v - f->mean;
                sq_diff_sum += d * d;
            }
        }
        f->std_dev = sqrt(sq_diff_sum / (double)(f->valid_count - 1));
    }

    for (size_t c = 0; c < cols; c++) {
        const FieldMeta *f = &dataset->fields[c];
        for (size_t r = 0; r < rows; r++) {
            const size_t idx = r * cols + c;
            const double v = dataset->data[idx];
            if (!isfinite(v)) continue;
            if (f->std_dev == 0.0) {
                dataset->data[idx] = 0.0;
            } else {
                double z = (v - f->mean) / f->std_dev;
                if (!isfinite(z)) z = 0.0;
                if (z > config->zscore_clip) z = config->zscore_clip;
                if (z < -config->zscore_clip) z = -config->zscore_clip;
                dataset->data[idx] = z;
            }
        }
    }
}

void run_defensive_preprocessor(LogDataset *dataset) {
    run_defensive_preprocessor_with_config(dataset, &ROUTING_DEFAULTS);
}

/* -------------------------------------------------------------------------- */
/* Stage 2: Metadata Extraction & Weighted Routing                            */
/* -------------------------------------------------------------------------- */

static DatasetProfile extract_dataset_profile(const LogDataset *dataset) {
    DatasetProfile profile = {0};
    if (!log_dataset_valid(dataset)) return profile;

    profile.rows = dataset->row_count;
    profile.cols = dataset->col_count;
    size_t total_valid = 0;
    double stddev_sum = 0.0;

    for (size_t c = 0; c < profile.cols; c++) {
        const FieldMeta *field = &dataset->fields[c];
        total_valid += field->valid_count;
        if (!field->is_sparse_dropped) profile.active_dimensions++;
        else profile.sparse_dimensions++;
        if (isfinite(field->std_dev)) stddev_sum += field->std_dev;
    }

    const size_t cells = profile.rows * profile.cols;
    profile.matrix_density = cells > 0 ? (double)total_valid / (double)cells : 0.0;
    profile.active_ratio = profile.cols > 0 ? (double)profile.active_dimensions / (double)profile.cols : 0.0;
    profile.sample_to_dimension_ratio = profile.active_dimensions > 0
        ? (double)profile.rows / (double)profile.active_dimensions
        : 0.0;
    profile.avg_stddev = profile.active_dimensions > 0
        ? stddev_sum / (double)profile.active_dimensions
        : 0.0;
    return profile;
}

static double clamp01(double value) {
    if (!isfinite(value)) return 0.0;
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double routing_score(const RoutingConfig *config,
                            const DatasetProfile *profile,
                            const AlgorithmScore *candidate) {
    if (!config || !profile || !candidate || profile->active_dimensions == 0 || profile->rows < 2) {
        return -INFINITY;
    }

    double sparsity_signal = profile->matrix_density < config->low_matrix_density
        ? 1.0 - (profile->matrix_density / config->low_matrix_density)
        : 0.0;
    double dimension_signal = profile->sample_to_dimension_ratio < config->high_dimension_sample_ratio
        ? 1.0 - (profile->sample_to_dimension_ratio / config->high_dimension_sample_ratio)
        : 0.0;
    double variance_signal = profile->avg_stddev > config->high_variance_stddev
        ? clamp01(profile->avg_stddev / (config->high_variance_stddev * 2.0))
        : 0.0;
    double scale_signal = clamp01(log1p((double)profile->rows) / log1p(100000.0));

    return config->density_weight * clamp01(candidate->density_affinity * sparsity_signal) +
           config->dimension_weight * clamp01(candidate->high_dimension_affinity * dimension_signal) +
           config->variance_weight * clamp01(candidate->variance_affinity * variance_signal) +
           config->scale_weight * clamp01(candidate->scale_affinity * scale_signal);
}

static AlgorithmType select_algorithm_by_score(const RoutingConfig *config,
                                               const DatasetProfile *profile) {
    if (!profile || profile->active_dimensions == 0 || profile->rows < 2) {
        return ALGO_FALLBACK_SCATTERPLOT;
    }

    AlgorithmScore candidates[] = {
        { ALGO_KMEANS,       0.05, 0.10, 0.15, 0.65, 0.0 },
        { ALGO_DBSCAN,       0.90, 0.35, 0.70, 0.20, 0.0 },
        { ALGO_BIRCH,        0.35, 0.80, 0.30, 0.95, 0.0 },
        { ALGO_OPTICS,       0.95, 0.70, 0.75, 0.35, 0.0 },
        { ALGO_MEAN_SHIFT,   0.75, 0.20, 0.90, 0.15, 0.0 },
        { ALGO_GMM,          0.20, 0.55, 0.80, 0.45, 0.0 },
        { ALGO_AGGLOMERATIVE,0.60, 0.30, 0.50, 0.05, 0.0 },
    };

    size_t best = 0;
    double best_score = -INFINITY;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        candidates[i].score = routing_score(config, profile, &candidates[i]);
        if (candidates[i].score > best_score) {
            best_score = candidates[i].score;
            best = i;
        }
    }

    return isfinite(best_score) ? candidates[best].algorithm : ALGO_FALLBACK_SCATTERPLOT;
}

AlgorithmType evaluate_and_select_algorithm(const LogDataset *dataset, bool is_auto_mode) {
    if (!log_dataset_valid(dataset)) return ALGO_FALLBACK_SCATTERPLOT;
    if (!is_auto_mode) return ALGO_KMEANS;
    DatasetProfile profile = extract_dataset_profile(dataset);
    return select_algorithm_by_score(&ROUTING_DEFAULTS, &profile);
}

/* -------------------------------------------------------------------------- */
/* Stage 3: Orchestration & Return                                            */
/* -------------------------------------------------------------------------- */

static const char *algorithm_message(AlgorithmType algorithm) {
    switch (algorithm) {
        case ALGO_KMEANS: return "K-means++ selected and executed";
        case ALGO_DBSCAN: return "DBSCAN selected and executed";
        case ALGO_BIRCH: return "BIRCH selected and executed";
        case ALGO_OPTICS: return "OPTICS selected and executed";
        case ALGO_MEAN_SHIFT: return "Mean-Shift selected and executed";
        case ALGO_GMM: return "GMM selected and executed";
        case ALGO_AGGLOMERATIVE: return "Agglomerative selected and executed";
        case ALGO_FALLBACK_SCATTERPLOT: return "Scatterplot fallback activated";
        default: return "Unknown algorithm state";
    }
}

AnalyticsResult execute_log_workbench_analysis(LogDataset *dataset,
                                               bool is_auto_mode,
                                               AlgorithmType user_preference) {
    AnalyticsResult result = {
        .algorithm = ALGO_FALLBACK_SCATTERPLOT,
        .status = STATUS_ERROR,
        .message = "Invalid dataset pointer",
    };

    if (!log_dataset_valid(dataset)) return result;

    run_defensive_preprocessor_with_config(dataset, &ROUTING_DEFAULTS);
    if (is_auto_mode) {
        DatasetProfile profile = extract_dataset_profile(dataset);
        result.algorithm = select_algorithm_by_score(&ROUTING_DEFAULTS, &profile);
    } else {
        result.algorithm = user_preference;
    }
    result.status = result.algorithm == ALGO_FALLBACK_SCATTERPLOT ? STATUS_FALLBACK : STATUS_OK;
    result.message = algorithm_message(result.algorithm);
    return result;
}
