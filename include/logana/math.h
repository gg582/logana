#ifndef LOGANA_MATH_H
#define LOGANA_MATH_H

#include "logana/types.h"

typedef double (*logana_distance_fn_t)(const double *a, const double *b,
                                       const uint8_t *mask_a, const uint8_t *mask_b,
                                       size_t dims, const logana_analysis_summary_t *summary);

double logana_distance_euclidean_sq(const double *a, const double *b,
                                    const uint8_t *mask_a, const uint8_t *mask_b,
                                    size_t dims, const logana_analysis_summary_t *summary);

double logana_distance_manhattan(const double *a, const double *b,
                                 const uint8_t *mask_a, const uint8_t *mask_b,
                                 size_t dims, const logana_analysis_summary_t *summary);

double logana_distance_zscore_sq(const double *a, const double *b,
                                 const uint8_t *mask_a, const uint8_t *mask_b,
                                 size_t dims, const logana_analysis_summary_t *summary);

size_t logana_parse_matrix(logana_engine_t *engine, logana_job_t *job);
void   logana_compute_summary(logana_job_t *job);

uint64_t logana_now_ms(void);
uint64_t logana_hash64(const void *data, size_t len);

/* Knee detection for DBSCAN/OPTICS eps estimation */
double logana_knee_detect_eps(const logana_feature_matrix_t *matrix,
                              size_t k,
                              logana_distance_fn_t dist_fn,
                              const logana_analysis_summary_t *summary);

/* Cluster internal metrics */
double logana_silhouette_score(const logana_feature_matrix_t *matrix,
                               logana_distance_fn_t dist_fn,
                               const logana_analysis_summary_t *summary,
                               const int *labels,
                               size_t cluster_count);

double logana_davies_bouldin_index(const logana_feature_matrix_t *matrix,
                                   logana_distance_fn_t dist_fn,
                                   const logana_analysis_summary_t *summary,
                                   const int *labels,
                                   size_t cluster_count);

#endif
