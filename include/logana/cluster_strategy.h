#ifndef LOGANA_CLUSTER_STRATEGY_H
#define LOGANA_CLUSTER_STRATEGY_H

#include "logana/types.h"
#include "logana/math.h"

typedef struct logana_cluster_strategy_vtable {
    const char *name;
    int (*fit)(const struct logana_cluster_strategy_vtable *self,
               const logana_feature_matrix_t *matrix,
               const logana_analysis_summary_t *summary,
               logana_cluster_result_t *out);
} logana_cluster_strategy_vtable_t;

/* Convenience wrapper to hide casting */
static inline int logana_strategy_fit(const logana_cluster_strategy_vtable_t *strategy,
                                      const logana_feature_matrix_t *matrix,
                                      const logana_analysis_summary_t *summary,
                                      logana_cluster_result_t *out) {
    return strategy->fit(strategy, matrix, summary, out);
}

/* Individual strategy constructors (return a vtable singleton) */
const logana_cluster_strategy_vtable_t *logana_strategy_kmeans(void);
const logana_cluster_strategy_vtable_t *logana_strategy_dbscan(void);
const logana_cluster_strategy_vtable_t *logana_strategy_optics(void);
const logana_cluster_strategy_vtable_t *logana_strategy_gmm(void);
const logana_cluster_strategy_vtable_t *logana_strategy_agglomerative(void);
const logana_cluster_strategy_vtable_t *logana_strategy_birch(void);
const logana_cluster_strategy_vtable_t *logana_strategy_mean_shift(void);
const logana_cluster_strategy_vtable_t *logana_strategy_fallback(void);

/* Auto strategy that runs multiple candidates and picks by metric */
const logana_cluster_strategy_vtable_t *logana_strategy_auto(void);

#endif
