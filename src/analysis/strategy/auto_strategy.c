#include "logana/cluster_strategy.h"
#include "logana/math.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

typedef struct {
    const logana_cluster_strategy_vtable_t *strategy;
    logana_cluster_result_t result;
    bool valid;
} auto_candidate_t;

static void auto_candidate_reset(auto_candidate_t *c) {
    if (!c->valid) return;
    free(c->result.labels);
    free(c->result.is_noise);
    memset(&c->result, 0, sizeof(c->result));
    c->valid = false;
}

static int auto_run_candidate(const logana_cluster_strategy_vtable_t *strategy,
                              const logana_feature_matrix_t *matrix,
                              const logana_analysis_summary_t *summary,
                              auto_candidate_t *out) {
    out->strategy = strategy;
    out->valid = false;
    memset(&out->result, 0, sizeof(out->result));
    int rc = logana_strategy_fit(strategy, matrix, summary, &out->result);
    if (rc != 0) {
        fprintf(stderr, "[auto] strategy %s fit failed (rc=%d)\n", strategy->name, rc);
        return -1;
    }
    if (out->result.cluster_count < 2 || out->result.cluster_count > matrix->row_count / 2) {
        fprintf(stderr, "[auto] strategy %s rejected: cluster_count=%zu rows=%zu\n",
                strategy->name, out->result.cluster_count, matrix->row_count);
        free(out->result.labels);
        free(out->result.is_noise);
        memset(&out->result, 0, sizeof(out->result));
        return -1;
    }
    out->valid = true;
    return 0;
}

static void auto_score_candidate(auto_candidate_t *c,
                                 const logana_feature_matrix_t *matrix,
                                 const logana_analysis_summary_t *summary) {
    if (!c->valid) return;
    logana_distance_fn_t dist_fn = logana_distance_euclidean_sq;
    c->result.silhouette_score = logana_silhouette_score(matrix, dist_fn, summary,
                                                          c->result.labels,
                                                          c->result.cluster_count);
    c->result.davies_bouldin_index = logana_davies_bouldin_index(matrix, dist_fn, summary,
                                                                  c->result.labels,
                                                                  c->result.cluster_count);
}

static double auto_composite_score(const auto_candidate_t *c) {
    if (!c->valid) return -1e300;
    double s = c->result.silhouette_score;
    double dbi = c->result.davies_bouldin_index;
    if (!isfinite(s)) s = -1.0;
    if (!isfinite(dbi) || dbi <= 0.0) dbi = 10.0;
    return s * 0.6 - dbi * 0.4;
}

static int auto_fit(const logana_cluster_strategy_vtable_t *self,
                    const logana_feature_matrix_t *matrix,
                    const logana_analysis_summary_t *summary,
                    logana_cluster_result_t *out) {
    (void)self;
    size_t rows = matrix->row_count;
    if (!rows) return -1;

    auto_candidate_t candidates[7];
    size_t cand_count = 0;
    memset(candidates, 0, sizeof(candidates));

    const logana_cluster_strategy_vtable_t *strategies[] = {
        logana_strategy_dbscan(),
        logana_strategy_gmm(),
        logana_strategy_optics(),
        logana_strategy_kmeans(),
        logana_strategy_agglomerative(),
        logana_strategy_birch(),
        logana_strategy_mean_shift(),
    };

    size_t effective_rows = matrix->row_count > 65536 ? 65536 : matrix->row_count;
    for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]); ++i) {
        /* Skip heavy O(n^2) strategies on large matrices before they burn CPU */
        if (effective_rows > 32768 &&
            (strcmp(strategies[i]->name, "dbscan") == 0 ||
             strcmp(strategies[i]->name, "optics") == 0 ||
             strcmp(strategies[i]->name, "mean_shift") == 0)) {
            fprintf(stderr, "[auto] skipping %s on large matrix (rows=%zu)\n",
                    strategies[i]->name, matrix->row_count);
            continue;
        }
        if (effective_rows > 65536 && strcmp(strategies[i]->name, "kmeans++") == 0) {
            fprintf(stderr, "[auto] skipping %s on large matrix (rows=%zu)\n",
                    strategies[i]->name, matrix->row_count);
            continue;
        }
        if (auto_run_candidate(strategies[i], matrix, summary, &candidates[cand_count]) == 0) {
            auto_score_candidate(&candidates[cand_count], matrix, summary);
            cand_count++;
        }
    }
    fprintf(stderr, "[auto] cand_count=%zu rows=%zu dims=%zu\n",
            cand_count, matrix->row_count, matrix->dimensions);

    if (cand_count == 0) {
        /* All failed — fallback */
        int rc = logana_strategy_fit(logana_strategy_fallback(), matrix, summary, out);
        if (rc == 0) out->algorithm = LOGANA_ALGO_FALLBACK_SCATTERPLOT;
        return rc;
    }

    size_t best_idx = 0;
    double best_score = auto_composite_score(&candidates[0]);
    for (size_t i = 1; i < cand_count; ++i) {
        double score = auto_composite_score(&candidates[i]);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    /* Copy winner to out; reset losers */
    for (size_t i = 0; i < cand_count; ++i) {
        if (i == best_idx) {
            *out = candidates[i].result;
            /* Mark algorithm as the winning strategy's algorithm */
            if (out->algorithm == LOGANA_ALGO_AUTO || out->algorithm == LOGANA_ALGO_KMEANS_PP) {
                /* map strategy name back to enum heuristically */
                const char *name = candidates[i].strategy->name;
                if (strcmp(name, "dbscan") == 0) out->algorithm = LOGANA_ALGO_DBSCAN;
                else if (strcmp(name, "gmm") == 0) out->algorithm = LOGANA_ALGO_GMM;
                else if (strcmp(name, "optics") == 0) out->algorithm = LOGANA_ALGO_OPTICS;
                else if (strcmp(name, "kmeans++") == 0) out->algorithm = LOGANA_ALGO_KMEANS_PP;
                else if (strcmp(name, "agglomerative") == 0) out->algorithm = LOGANA_ALGO_AGGLOMERATIVE;
                else if (strcmp(name, "birch") == 0) out->algorithm = LOGANA_ALGO_BIRCH;
                else if (strcmp(name, "mean_shift") == 0) out->algorithm = LOGANA_ALGO_MEAN_SHIFT;
                else out->algorithm = LOGANA_ALGO_FALLBACK_SCATTERPLOT;
            }
        } else {
            auto_candidate_reset(&candidates[i]);
        }
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
