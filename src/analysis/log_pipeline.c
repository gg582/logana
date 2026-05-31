#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 0. Type Definitions — Robust computation pipe structures
 * -------------------------------------------------------------------------- */

typedef struct {
    double value;
    bool   is_valid;
} LogCell;

typedef struct {
    LogCell *cells;
    size_t   num_fields;
    char     timestamp[32];
    int      cluster_id;
} LogRow;

typedef struct {
    LogRow      *rows;
    size_t       num_rows;
    size_t       num_fields;
    const char **field_names;
} LogDataset;

typedef struct {
    double  entropy;
    double  slope_mem_rss_mb;
    double  slope_latency_ms;
    double  slope_cpu_util;
    size_t  num_clusters;
} AnalyticsResult;

/* --------------------------------------------------------------------------
 * 1. Dynamic Time-Series Converter (Fixing Trend)
 * -------------------------------------------------------------------------- */

/**
 * @brief Zero-allocation ISO 8601 parser using native character offsets.
 *        Extracts Year, Month, Day, Hour, Minute, Second, and Milliseconds,
 *        then returns absolute seconds relative to the Unix epoch.
 */
double parse_iso8601_to_seconds(const char *timestamp_str)
{
    if (!timestamp_str || timestamp_str[0] == '\0')
        return 0.0;

    const char *s = timestamp_str;
    size_t len = strlen(s);

    /* Minimum rigid length: 2026-05-31T18:00:01Z */
    if (len < 19)
        return 0.0;

    /* Strict positional extraction */
    int year   = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int month  = (s[5] - '0') * 10 + (s[6] - '0');
    int day    = (s[8] - '0') * 10 + (s[9] - '0');
    int hour   = (s[11] - '0') * 10 + (s[12] - '0');
    int minute = (s[14] - '0') * 10 + (s[15] - '0');
    int second = (s[17] - '0') * 10 + (s[18] - '0');

    /* Separator validation */
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
        return 0.0;

    int ms = 0;
    if (len >= 23 && s[19] == '.') {
        ms = (s[20] - '0') * 100 + (s[21] - '0') * 10 + (s[22] - '0');
    }

    /* Domain guards */
    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || ms < 0 || ms > 999) {
        return 0.0;
    }

    /* Proleptic Gregorian day count from 0001-01-01 */
    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    int64_t y = (int64_t)year - 1;
    int64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    days += days_before_month[month - 1];
    if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
        days++;
    days += day - 1;

    /* Re-base to Unix epoch (1970-01-01) */
    days -= 719162;

    double seconds = (double)days * 86400.0;
    seconds += hour * 3600.0;
    seconds += minute * 60.0;
    seconds += second;
    seconds += ms / 1000.0;
    return seconds;
}

/* --------------------------------------------------------------------------
 * 2. Multi-Dimensional Cluster Entropy Aggregator (Fixing Entropy)
 * -------------------------------------------------------------------------- */

/**
 * @brief Computes the Shannon entropy of an empirical cluster assignment
 *        distribution.  Scans the active assignments array, derives P(c_i),
 *        and accumulates -P(c_i)*log2(P(c_i)) with an epsilon guard.
 */
double calculate_cluster_distribution_entropy(const int *cluster_assignments,
                                              int total_rows,
                                              int num_clusters)
{
    if (!cluster_assignments || total_rows <= 0 || num_clusters <= 0)
        return 0.0;

    int *counts = calloc((size_t)num_clusters, sizeof(int));
    if (!counts)
        return 0.0;

    for (int i = 0; i < total_rows; ++i) {
        int cid = cluster_assignments[i];
        if (cid >= 0 && cid < num_clusters)
            counts[cid]++;
    }

    double entropy = 0.0;
    const double inv_n = 1.0 / (double)total_rows;

    for (int c = 0; c < num_clusters; ++c) {
        double p = (double)counts[c] * inv_n;
        if (p > 1e-9)
            entropy -= p * log2(p);
    }

    free(counts);
    return entropy;
}


