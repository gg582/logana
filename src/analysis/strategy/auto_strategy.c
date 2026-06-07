#include "logana/cluster_strategy.h"
#include "logana/math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t max_exact_pairwise_rows;
    double min_cluster_ratio;
    double max_cluster_ratio;
    double silhouette_weight;
    double davies_bouldin_weight;
    double density_weight;
    double dimension_weight;
    double scale_weight;
    double sparse_density;
    double high_dimension_ratio;
    double high_variance;
} auto_selection_config_t;

typedef struct {
    size_t rows;
    size_t dims;
    double valid_density;
    double sample_to_dimension_ratio;
    double mean_stddev;
    double outlier_ratio;
} auto_dataset_profile_t;

typedef struct {
    const logana_cluster_strategy_vtable_t *strategy;
    logana_algorithm_t algorithm;
    bool pairwise_heavy;
    double prefers_density;
    double prefers_spherical;
    double prefers_high_dimension;
    logana_cluster_result_t result;
    double metric_score;
    double profile_score;
    double total_score;
    bool valid;
} auto_candidate_t;

static const auto_selection_config_t AUTO_SELECTION_DEFAULTS = {
    .max_exact_pairwise_rows = 32768,
    .min_cluster_ratio = 0.02,
    .max_cluster_ratio = 0.50,
    .silhouette_weight = 0.60,
    .davies_bouldin_weight = 0.40,
    .density_weight = 0.22,
    .dimension_weight = 0.18,
    .scale_weight = 0.14,
    .sparse_density = 0.55,
    .high_dimension_ratio = 10.0,
    .high_variance = 1.75,
};

static auto_dataset_profile_t auto_profile_dataset(const logana_feature_matrix_t *matrix,
                                                   const logana_analysis_summary_t *summary) {
    auto_dataset_profile_t profile = {0};
    profile.rows = matrix ? matrix->row_count : 0;
    profile.dims = matrix ? matrix->dimensions : 0;
    if (!matrix || !matrix->values || profile.rows == 0 || profile.dims == 0) return profile;

    size_t valid = 0;
    size_t cells = profile.rows * profile.dims;
    if (matrix->valid_mask) {
        for (size_t i = 0; i < cells; ++i) {
            if (matrix->valid_mask[i]) valid++;
        }
    } else {
        valid = cells;
    }
    profile.valid_density = cells > 0 ? (double)valid / (double)cells : 0.0;
    profile.sample_to_dimension_ratio = profile.dims > 0 ? (double)profile.rows / (double)profile.dims : 0.0;
    profile.outlier_ratio = summary ? summary->outlier_ratio : 0.0;

    if (summary) {
        double stddev_sum = 0.0;
        size_t stddev_count = 0;
        for (size_t d = 0; d < profile.dims && d < LOGANA_MAX_DIMENSIONS; ++d) {
            if (isfinite(summary->stddev[d])) {
                stddev_sum += summary->stddev[d];
                stddev_count++;
            }
        }
        profile.mean_stddev = stddev_count > 0 ? stddev_sum / (double)stddev_count : 0.0;
    }
    return profile;
}

static double clamp01(double value) {
    if (!isfinite(value)) return 0.0;
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

static double auto_metric_score(const auto_selection_config_t *config,
                                const logana_cluster_result_t *result) {
    double silhouette = result->silhouette_score;
    double dbi = result->davies_bouldin_index;
    if (!isfinite(silhouette)) silhouette = -1.0;
    if (!isfinite(dbi) || dbi < 0.0) dbi = 10.0;

    double normalized_silhouette = (silhouette + 1.0) * 0.5;
    double normalized_dbi = 1.0 / (1.0 + dbi);
    return config->silhouette_weight * clamp01(normalized_silhouette) +
           config->davies_bouldin_weight * clamp01(normalized_dbi);
}

static double auto_profile_score(const auto_selection_config_t *config,
                                 const auto_dataset_profile_t *profile,
                                 const auto_candidate_t *candidate) {
    double sparse_signal = profile->valid_density < config->sparse_density
        ? 1.0 - (profile->valid_density / config->sparse_density)
        : 0.0;
    double high_dim_signal = profile->sample_to_dimension_ratio < config->high_dimension_ratio
        ? 1.0 - (profile->sample_to_dimension_ratio / config->high_dimension_ratio)
        : 0.0;
    double variance_signal = profile->mean_stddev > config->high_variance
        ? clamp01(profile->mean_stddev / (config->high_variance * 2.0))
        : 0.0;

    double density_score = candidate->prefers_density * sparse_signal +
                           candidate->prefers_spherical * (1.0 - sparse_signal);
    double dimension_score = candidate->prefers_high_dimension * high_dim_signal +
                             candidate->prefers_spherical * (1.0 - high_dim_signal);
    double scale_score = (1.0 - candidate->prefers_spherical) * variance_signal +
                         candidate->prefers_spherical * (1.0 - variance_signal);

    return config->density_weight * clamp01(density_score) +
           config->dimension_weight * clamp01(dimension_score) +
           config->scale_weight * clamp01(scale_score) -
           clamp01(profile->outlier_ratio) * 0.10;
}

static bool auto_candidate_allowed(const auto_selection_config_t *config,
                                   const auto_dataset_profile_t *profile,
                                   const auto_candidate_t *candidate) {
    if (!candidate->strategy || !profile || profile->rows == 0 || profile->dims == 0) return false;
    if (candidate->pairwise_heavy && profile->rows > config->max_exact_pairwise_rows) return false;
    return true;
}

static bool auto_result_shape_allowed(const auto_selection_config_t *config,
                                      const logana_cluster_result_t *result,
                                      size_t rows) {
    if (!result || rows == 0 || result->cluster_count == 0) return false;
    double ratio = (double)result->cluster_count / (double)rows;
    return ratio >= config->min_cluster_ratio && ratio <= config->max_cluster_ratio;
}

static void auto_candidate_reset(auto_candidate_t *c) {
    if (!c || !c->valid) return;
    free(c->result.labels);
    free(c->result.is_noise);
    memset(&c->result, 0, sizeof(c->result));
    c->valid = false;
}

static int auto_run_candidate(const auto_selection_config_t *config,
                              const auto_dataset_profile_t *profile,
                              const logana_feature_matrix_t *matrix,
                              const logana_analysis_summary_t *summary,
                              auto_candidate_t *candidate) {
    if (!auto_candidate_allowed(config, profile, candidate)) return -1;
    memset(&candidate->result, 0, sizeof(candidate->result));
    int rc = logana_strategy_fit(candidate->strategy, matrix, summary, &candidate->result);
    if (rc != 0) {
        fprintf(stderr, "[auto] strategy %s fit failed (rc=%d)\n", candidate->strategy->name, rc);
        return -1;
    }
    if (!auto_result_shape_allowed(config, &candidate->result, profile->rows)) {
        fprintf(stderr, "[auto] strategy %s rejected: cluster_count=%zu rows=%zu\n",
                candidate->strategy->name, candidate->result.cluster_count, profile->rows);
        free(candidate->result.labels);
        free(candidate->result.is_noise);
        memset(&candidate->result, 0, sizeof(candidate->result));
        return -1;
    }
    candidate->valid = true;
    return 0;
}

static void auto_score_candidate(const auto_selection_config_t *config,
                                 const auto_dataset_profile_t *profile,
                                 const logana_feature_matrix_t *matrix,
                                 const logana_analysis_summary_t *summary,
                                 auto_candidate_t *candidate) {
    if (!candidate->valid) return;
    logana_distance_fn_t dist_fn = logana_distance_euclidean_sq;
    candidate->result.silhouette_score = logana_silhouette_score(matrix, dist_fn, summary,
                                                                  candidate->result.labels,
                                                                  candidate->result.cluster_count);
    candidate->result.davies_bouldin_index = logana_davies_bouldin_index(matrix, dist_fn, summary,
                                                                          candidate->result.labels,
                                                                          candidate->result.cluster_count);
    candidate->metric_score = auto_metric_score(config, &candidate->result);
    candidate->profile_score = auto_profile_score(config, profile, candidate);
    candidate->total_score = candidate->metric_score + candidate->profile_score;
}

static int auto_fit(const logana_cluster_strategy_vtable_t *self,
                    const logana_feature_matrix_t *matrix,
                    const logana_analysis_summary_t *summary,
                    logana_cluster_result_t *out) {
    if (!self || !matrix || !matrix->values || !out || matrix->row_count == 0 || matrix->dimensions == 0) return -1;

    const auto_selection_config_t config = AUTO_SELECTION_DEFAULTS;
    const auto_dataset_profile_t profile = auto_profile_dataset(matrix, summary);

    auto_candidate_t candidates[] = {
        { .strategy = logana_strategy_dbscan(), .algorithm = LOGANA_ALGO_DBSCAN, .pairwise_heavy = true,  .prefers_density = 1.0, .prefers_spherical = 0.0, .prefers_high_dimension = 0.4 },
        { .strategy = logana_strategy_gmm(), .algorithm = LOGANA_ALGO_GMM, .pairwise_heavy = false, .prefers_density = 0.2, .prefers_spherical = 0.4, .prefers_high_dimension = 0.5 },
        { .strategy = logana_strategy_optics(), .algorithm = LOGANA_ALGO_OPTICS, .pairwise_heavy = true,  .prefers_density = 1.0, .prefers_spherical = 0.0, .prefers_high_dimension = 0.7 },
        { .strategy = logana_strategy_kmeans(), .algorithm = LOGANA_ALGO_KMEANS_PP, .pairwise_heavy = false, .prefers_density = 0.0, .prefers_spherical = 1.0, .prefers_high_dimension = 0.1 },
        { .strategy = logana_strategy_agglomerative(), .algorithm = LOGANA_ALGO_AGGLOMERATIVE, .pairwise_heavy = true, .prefers_density = 0.5, .prefers_spherical = 0.2, .prefers_high_dimension = 0.3 },
        { .strategy = logana_strategy_birch(), .algorithm = LOGANA_ALGO_BIRCH, .pairwise_heavy = false, .prefers_density = 0.4, .prefers_spherical = 0.5, .prefers_high_dimension = 0.8 },
        { .strategy = logana_strategy_mean_shift(), .algorithm = LOGANA_ALGO_MEAN_SHIFT, .pairwise_heavy = true, .prefers_density = 0.8, .prefers_spherical = 0.1, .prefers_high_dimension = 0.2 },
    };
    const size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);

    size_t best_idx = candidate_count;
    double best_score = -INFINITY;
    size_t valid_count = 0;

    for (size_t i = 0; i < candidate_count; ++i) {
        if (auto_run_candidate(&config, &profile, matrix, summary, &candidates[i]) != 0) continue;
        auto_score_candidate(&config, &profile, matrix, summary, &candidates[i]);
        valid_count++;
        if (candidates[i].total_score > best_score) {
            best_score = candidates[i].total_score;
            best_idx = i;
        }
    }

    fprintf(stderr, "[auto] candidates=%zu rows=%zu dims=%zu density=%.3f winner=%s score=%.3f\n",
            valid_count, profile.rows, profile.dims, profile.valid_density,
            best_idx < candidate_count ? candidates[best_idx].strategy->name : "fallback",
            best_score);

    if (best_idx == candidate_count) {
        int rc = logana_strategy_fit(logana_strategy_fallback(), matrix, summary, out);
        if (rc == 0) out->algorithm = LOGANA_ALGO_FALLBACK_SCATTERPLOT;
        return rc;
    }

    *out = candidates[best_idx].result;
    out->algorithm = candidates[best_idx].algorithm;
    candidates[best_idx].valid = false;

    for (size_t i = 0; i < candidate_count; ++i) {
        auto_candidate_reset(&candidates[i]);
    }
    return 0;
}

static const logana_cluster_strategy_vtable_t auto_vtable = {
    .name = "auto",
    .fit  = auto_fit,
};

const logana_cluster_strategy_vtable_t *logana_strategy_auto(void) {
    return &auto_vtable;
}
