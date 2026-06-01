#include "logana/logana.h"
#include "logana/simd.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ttak/async/task.h>
#include <ttak/mem/mem.h>
#include <ttak/timing/timing.h>

typedef struct {
    size_t idx;
    double dist;
} logana_neighbor_t;

static void logana_log_sink(ttak_log_level_t level, const char *msg) {
    const char *tag = "INFO";
    if (level == TTAK_LOG_DEBUG) tag = "DEBUG";
    else if (level == TTAK_LOG_WARN) tag = "WARN";
    else if (level == TTAK_LOG_ERROR) tag = "ERROR";
    fprintf(stderr, "[%s] %s\n", tag, msg);
}

uint64_t logana_now_ms(void) {
    return ttak_get_tick_count();
}

uint64_t logana_hash64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void logana_set_job_status(logana_job_t *job, logana_job_status_t status, const char *error) {
    pthread_mutex_lock(&job->lock);
    job->status = status;
    job->updated_ms = logana_now_ms();
    if (error) snprintf(job->error, sizeof(job->error), "%s", error);
    pthread_mutex_unlock(&job->lock);
}

/* --------------------------------------------------------------------------
 * 1. Numerical Parsing Guard (Anti-NaN & Overflow Protection)
 * -------------------------------------------------------------------------- */

/**
 * @brief Detects injected trap strings before numeric conversion.
 *        Matches "NaN", "Inf", "Infinity", "null" case-insensitively,
 *        optionally prefixed by '+' or '-'.
 */
static bool logana_is_numeric_trap(const char *str) {
    while (*str && (isspace((unsigned char)*str) || *str == '"' || *str == '\'')) ++str;

    char buf[16];
    size_t i = 0;
    while (str[i] && i < sizeof(buf) - 1 &&
           (isalpha((unsigned char)str[i]) || str[i] == '+' || str[i] == '-')) {
        buf[i] = (char)tolower((unsigned char)str[i]);
        ++i;
    }
    buf[i] = '\0';

    if (i == 0) return false;
    /* Reject prefixes like nan123, nullified, etc. */
    if (str[i] && !isspace((unsigned char)str[i]) && str[i] != '"' && str[i] != '\'' &&
        str[i] != ',' && str[i] != ';' && str[i] != '|' && str[i] != '}' && str[i] != ']')
        return false;

    return (strcmp(buf, "nan") == 0 ||
            strcmp(buf, "inf") == 0 ||
            strcmp(buf, "infinity") == 0 ||
            strcmp(buf, "null") == 0 ||
            strcmp(buf, "undefined") == 0);
}

/**
 * @brief Defensive wrapper around strtod().
 * @param str          Input string.
 * @param out          Parsed double output.
 * @param out_valid    If non-NULL, set to true only when value is finite and not a trap.
 * @param clamp_negative If true, negative values are clamped to 0.0 (infra metrics guard).
 * @return 1 if a token was consumed, 0 otherwise.
 */
static int logana_safe_strtod(const char *str, double *out, bool *out_valid, bool clamp_negative) {
    if (!str || !*str) return 0;

    if (logana_is_numeric_trap(str)) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1; /* token consumed but marked invalid */
    }

    const char *num_start = str;
    while (*num_start && (isspace((unsigned char)*num_start) || *num_start == '"' || *num_start == '\'')) ++num_start;
    char *end = NULL;
    double value = strtod(num_start, &end);
    if (end == num_start) return 0;

    /* Strict tail check: reject partial consumptions like "200 OK" or "0x7FFF" */
    for (const char *p = end; *p; ++p) {
        if (!isspace((unsigned char)*p) && *p != '"' && *p != '\'' && *p != ',' &&
            *p != ';' && *p != '|' && *p != '}' && *p != ']' && *p != '%') {
            return 0;
        }
    }

    if (!isfinite(value)) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1;
    }

    /* Magnitude guard: reject values that would dominate statistics or
     * overflow float conversion downstream. 1e15 covers all realistic
     * log metrics (latency, throughput, counts) while catching corrupted
     * timestamps, memory addresses, or parsing artifacts. */
    if (fabs(value) > 1e15) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1;
    }

    if (clamp_negative && value < 0.0) value = 0.0;

    if (out_valid) *out_valid = true;
    *out = value;
    return 1;
}

static const char *logana_find_key_ci(const char *line, const char *key) {
    size_t key_len = strlen(key);
    for (const char *p = line; *p; ++p) {
        if (*p != '"') continue;
        ++p;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i < key_len &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)key[i])) {
            ++i;
        }
        if (i == key_len && p[i] == '"') return p - 1;
    }
    return NULL;
}

/**
 * @brief Extracts a numeric value associated with a JSON key.
 * @param out_valid    Set to false if key found but value is a trap/NaN/Inf.
 * @param clamp_negative Clamps negative results to 0.0.
 */
static int logana_extract_number(const char *line, const char *key, bool case_sensitive,
                                 double *out, bool *out_valid, bool clamp_negative) {
    const char *found = case_sensitive ? strstr(line, key) : logana_find_key_ci(line, key);
    if (!found) return 0;
    const char *colon = strchr(found, ':');
    if (!colon) return 0;

    const char *start = colon + 1;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (!*start) return 0;

    return logana_safe_strtod(start, out, out_valid, clamp_negative);
}

static int logana_extract_string(const char *line, const char *key, bool case_sensitive,
                                 char *out, size_t out_cap) {
    const char *found = case_sensitive ? strstr(line, key) : logana_find_key_ci(line, key);
    if (!found) return 0;
    const char *colon = strchr(found, ':');
    if (!colon) return 0;
    const char *start = colon + 1;
    while (*start && isspace((unsigned char)*start)) ++start;
    if (*start == '"' || *start == '\'') {
        char quote = *start++;
        const char *end = start;
        while (*end && *end != quote) ++end;
        size_t len = (size_t)(end - start);
        if (len >= out_cap) len = out_cap - 1;
        memcpy(out, start, len);
        out[len] = '\0';
        return 1;
    }
    return 0;
}

static size_t logana_count_chars(const char *line, char needle) {
    size_t count = 0;
    for (const char *p = line; *p; ++p) {
        if (*p == needle) ++count;
    }
    return count;
}

/**
 * @brief Strips common unit / punctuation suffixes from a numeric token.
 *        Handles ms, %, KB/MB/GB, Hz, bps, x (multiplier), etc.
 */
static void logana_strip_numeric_suffixes(const char **start, const char **end) {
    while (*end > *start && isspace((unsigned char)(*end)[-1])) --*end;

    static const char *suffixes[] = {
        /* Ordered longest first so that "mbps" is tried before "bps" */
        "mbps", "gbps", "kbps", "tbps", "bps",
        "mhz", "ghz", "khz", "thz", "hz",
        "tps", "qps", "rpm",
        "mib", "gib", "kib", "tib",
        "mb", "gb", "kb", "tb", "pb",
        "ms", "µs", "us", "ns", "ps", "fs",
        "sec", "min", "hr", "hrs", "day", "days",
        "px", "em", "rem", "vw", "vh", "dpi",
        "x", "th", "st", "nd", "rd",
        "%", "b", "s",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        size_t slen = strlen(suffixes[i]);
        if ((size_t)(*end - *start) >= slen) {
            bool match = true;
            for (size_t j = 0; j < slen; ++j) {
                if (tolower((unsigned char)(*end)[-slen + j]) != suffixes[i][j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *end -= slen;
                while (*end > *start && isspace((unsigned char)(*end)[-1])) --*end;
                return; /* strip one suffix only; caller can loop if needed */
            }
        }
    }
}

/**
 * @brief Parses a raw token bounded by [start, end) with trap & overflow guards.
 *        Strips common unit suffixes (ms, %, KB, MB, Hz, etc.) so that
 *        unstructured text like "latency=20.0ms" or "cpu_usage=40%" is accepted.
 */
static int logana_parse_numeric_token(const char *start, const char *end, double *out, bool *out_valid) {
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    while (start < end && (*start == '"' || *start == '\'' || *start == '{' || *start == '[' || *start == '(')) ++start;
    while (end > start && (end[-1] == '"' || end[-1] == '\'' || end[-1] == '}' || end[-1] == ']' || end[-1] == ')' || end[-1] == ',' || end[-1] == ';' || end[-1] == '|')) --end;
    if (end <= start) return 0;

    /* Strip unit suffixes before creating the bounded token */
    logana_strip_numeric_suffixes(&start, &end);
    if (end <= start) return 0;

    char token[128];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(token)) len = sizeof(token) - 1;
    memcpy(token, start, len);
    token[len] = '\0';

    /* Reject hexadecimal memory addresses — they are metrics noise, not numbers */
    if (!strncmp(token, "0x", 2) || !strncmp(token, "0X", 2)) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1; /* token consumed but marked invalid */
    }

    return logana_safe_strtod(token, out, out_valid, false);
}

static size_t logana_collect_freeform_numbers(const char *line, double *values, size_t cap) {
    size_t count = 0;
    const char *p = line;
    while (*p && count < cap) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == '|')) ++p;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';' && *p != '|') ++p;
        const char *end = p;
        const char *value_start = start;
        for (const char *q = end; q > start; --q) {
            if (q[-1] == '=' || q[-1] == ':') {
                value_start = q;
                break;
            }
        }
        double value = 0.0;
        if (logana_parse_numeric_token(value_start, end, &value, NULL)) {
            values[count++] = value;
        }
    }
    return count;
}

/* --------------------------------------------------------------------------
 * 2. Timestamp Normalization (ISO-8601 vs Unix Epoch)
 * -------------------------------------------------------------------------- */

/**
 * @brief Auto-detects timestamp format and normalizes to milliseconds since epoch.
 *        Supports Unix epoch integers (sec/ms/us) and ISO-8601 strings.
 * @param str  Raw JSON value (may contain surrounding quotes).
 * @param out_ms  Output normalized timestamp in milliseconds.
 * @return 1 on success, 0 on failure.
 */
static int logana_normalize_timestamp(const char *str, uint64_t *out_ms) {
    if (!str || !*str) return 0;

    /* Strip leading whitespace/quotes/braces */
    while (*str && (isspace((unsigned char)*str) || *str == '"' || *str == '\'' || *str == ':')) ++str;
    if (!*str) return 0;

    /* Find strict token boundary so we don't swallow trailing JSON fields */
    const char *token_end = str;
    while (*token_end && !isspace((unsigned char)*token_end) && *token_end != ',' && *token_end != '}' && *token_end != ']' && *token_end != '"') ++token_end;
    if (token_end <= str) return 0;

    size_t len = (size_t)(token_end - str);

    /* Copy token to bounded buffer for safe parsing */
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, str, len);
    buf[len] = '\0';

    /* Case A: Numeric epoch (integer or floating-point seconds) */
    bool is_numeric = true;
    bool has_dot = false;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '.') { has_dot = true; continue; }
        if (!isdigit((unsigned char)buf[i])) { is_numeric = false; break; }
    }

    if (is_numeric && len > 0) {
        if (has_dot) {
            double sec = strtod(buf, NULL);
            uint64_t ms = (uint64_t)(sec * 1000.0);
            if (ms >= 946684800000ULL && ms <= 2208988800000ULL) {
                *out_ms = ms;
                return 1;
            }
        } else {
            unsigned long long val = strtoull(buf, NULL, 10);
            uint64_t ms;
            if (val < 10000000000ULL) {
                ms = val * 1000ULL;               /* seconds -> ms */
            } else if (val > 2000000000000ULL) {
                ms = val / 1000ULL;               /* microseconds -> ms */
            } else {
                ms = val;                         /* milliseconds */
            }
            /* Reject epochs outside 2000-2040 UTC */
            if (ms >= 946684800000ULL && ms <= 2208988800000ULL) {
                *out_ms = ms;
                return 1;
            }
        }
        return 0;
    }

    /* Case B: ISO-8601 string */
    struct tm tm = {0};
    const char *rest = strptime(buf, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!rest && len >= 10) {
        rest = strptime(buf, "%Y-%m-%d", &tm);   /* date-only fallback */
    }
    if (rest) {
        /* Skip 'Z' or simple timezone offset (+HH:MM / -HH:MM) */
        if (*rest == 'Z' || *rest == 'z') {
            rest++;
        } else if ((*rest == '+' || *rest == '-') && len >= (size_t)(rest - buf + 6)) {
            rest += 6;
        }
        time_t sec = timegm(&tm);
        if (sec == (time_t)-1) return 0;
        uint64_t ms = (uint64_t)sec * 1000ULL;
        if (*rest == '.') {
            char *frac_end = NULL;
            double frac = strtod(rest, &frac_end);
            (void)frac_end;
            ms += (uint64_t)(frac * 1000.0);
        }
        *out_ms = ms;
        return 1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * 3. Distance Matrix with Dimension Masking, Scaling & Normalization
 * -------------------------------------------------------------------------- */

typedef double (*logana_distance_fn_t)(const double *a, const double *b,
                                       const uint8_t *mask_a, const uint8_t *mask_b,
                                       size_t dims, const logana_analysis_summary_t *summary);

/**
 * @brief Scaled Euclidean distance (squared) with per-dimension validity masking.
 */
static double logana_distance_euclidean_sq(const double *a, const double *b,
                                           const uint8_t *mask_a, const uint8_t *mask_b,
                                           size_t dims, const logana_analysis_summary_t *summary) {
    (void)summary;
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        double d = (double)a[i] - (double)b[i];
        total += d * d;
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

/**
 * @brief Manhattan distance ($L_1$ norm) with per-dimension validity masking
 *        and dynamic scaling.  Avoids geometric amplification of negative
 *        coordinates because it uses absolute differences instead of squares.
 */
static double logana_distance_manhattan(const double *a, const double *b,
                                        const uint8_t *mask_a, const uint8_t *mask_b,
                                        size_t dims, const logana_analysis_summary_t *summary) {
    (void)summary;
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        total += fabs((double)a[i] - (double)b[i]);
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

/**
 * @brief Z-score normalized squared Euclidean distance.  Prevents negative
 *        coordinates from exploding distance because each dimension is
 *        standardised before differencing.
 */
static double logana_distance_zscore_sq(const double *a, const double *b,
                                        const uint8_t *mask_a, const uint8_t *mask_b,
                                        size_t dims, const logana_analysis_summary_t *summary) {
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        double sigma = summary && summary->stddev[i] > 0.0001 ? summary->stddev[i] : 1.0;
        double za = ((double)a[i] - (summary ? summary->mean[i] : 0.0)) / sigma;
        double zb = ((double)b[i] - (summary ? summary->mean[i] : 0.0)) / sigma;
        double d = za - zb;
        total += d * d;
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

static size_t logana_parse_matrix(logana_engine_t *engine, logana_job_t *job) {
    size_t dims = engine->config.numeric_key_count;
    if (dims == 0) dims = LOGANA_MAX_DIMENSIONS;
    if (dims > LOGANA_MAX_DIMENSIONS) dims = LOGANA_MAX_DIMENSIONS;

    size_t row_capacity = 1024;
    double *values = malloc(row_capacity * dims * sizeof(double));
    uint64_t *timestamps = malloc(row_capacity * sizeof(uint64_t));
    uint8_t *valid_mask = malloc(row_capacity * dims * sizeof(uint8_t));
    uint64_t *categories = calloc(row_capacity, sizeof(uint64_t));
    uint8_t *formats = calloc(row_capacity, sizeof(uint8_t));
    if (!values || !timestamps || !valid_mask || !categories || !formats) {
        free(values); free(timestamps); free(valid_mask); free(categories); free(formats);
        return 0;
    }

    size_t rows = 0;
    char *cursor = job->payload;
    while (cursor && *cursor && rows < engine->config.max_rows_per_analysis) {
        char *next = strchr(cursor, '\n');
        if (next) *next = '\0';
        if (*cursor) {
            if (rows == row_capacity) {
                size_t new_cap = row_capacity * 2;
                double *grown_v = realloc(values, new_cap * dims * sizeof(double));
                uint64_t *grown_t = realloc(timestamps, new_cap * sizeof(uint64_t));
                uint8_t *grown_m = realloc(valid_mask, new_cap * dims * sizeof(uint8_t));
                uint64_t *grown_c = realloc(categories, new_cap * sizeof(uint64_t));
                uint8_t *grown_f = realloc(formats, new_cap * sizeof(uint8_t));
                if (!grown_v || !grown_t || !grown_m || !grown_c || !grown_f) break;
                values = grown_v;
                timestamps = grown_t;
                valid_mask = grown_m;
                categories = grown_c;
                formats = grown_f;
                row_capacity = new_cap;
            }

            /* Detect row format: JSON = 0, KV = 1, Text = 2 */
            {
                const char *p = cursor;
                while (*p && isspace((unsigned char)*p)) ++p;
                if (*p == '{' || *p == '[') {
                    formats[rows] = 0;
                } else if (strchr(cursor, '=') != NULL || (strchr(cursor, ':') != NULL && strstr(cursor, ", ") != NULL)) {
                    formats[rows] = 1;
                } else {
                    formats[rows] = 2;
                }
            }

            /* Initialize current row validity to all-valid */
            memset(valid_mask + rows * dims, 1, dims * sizeof(uint8_t));

            double extracted[LOGANA_MAX_DIMENSIONS] = {0};
            bool extracted_valid[LOGANA_MAX_DIMENSIONS] = {false};
            size_t captured = 0;

            /* 1) Extract configured numeric keys with anti-trap guards.
             *    If a key is missing, explicitly mark as NaN / invalid so the
             *    scheduler sees a sparse matrix instead of a dense fake one.
             *    We track how many configured keys actually yielded valid numbers
             *    so that rows with a total miss can fall back to free-form
             *    parsing (essential for generic "paste any JSONL" workbenches
             *    where the user has not tuned the key list). */
            size_t configured_valid_found = 0;
            for (size_t d = 0; d < engine->config.numeric_key_count && captured < dims; ++d) {
                bool valid = true;
                double number = 0.0;
                int found = logana_extract_number(cursor, engine->config.numeric_keys[d],
                                                  engine->config.case_sensitive,
                                                  &number, &valid, false);
                if (found) {
                    extracted[captured] = number;
                    extracted_valid[captured] = valid;
                    if (!valid) valid_mask[rows * dims + captured] = 0;
                    else configured_valid_found++;
                } else {
                    extracted[captured] = NAN;
                    extracted_valid[captured] = false;
                    valid_mask[rows * dims + captured] = 0;
                }
                ++captured;
            }

            /* 2) Timestamp extraction & normalization */
            uint64_t ts_ms = 0;
            bool ts_found = false;
            for (size_t t = 0; t < engine->config.timestamp_key_count; ++t) {
                const char *found = engine->config.case_sensitive
                    ? strstr(cursor, engine->config.timestamp_keys[t])
                    : logana_find_key_ci(cursor, engine->config.timestamp_keys[t]);
                if (found) {
                    const char *colon = strchr(found, ':');
                    if (colon && logana_normalize_timestamp(colon + 1, &ts_ms)) {
                        ts_found = true;
                        break;
                    }
                }
            }
            if (!ts_found) {
                /* Fallback: preserve temporal ordering using synthetic ms */
                ts_ms = (uint64_t)rows * 1000ULL;
            }
            timestamps[rows] = ts_ms;

            /* Mask timestamp values so freeform collector doesn't ingest them as metrics */
            for (size_t t = 0; t < engine->config.timestamp_key_count; ++t) {
                const char *found = engine->config.case_sensitive
                    ? strstr(cursor, engine->config.timestamp_keys[t])
                    : logana_find_key_ci(cursor, engine->config.timestamp_keys[t]);
                if (found) {
                    const char *colon = strchr(found, ':');
                    if (colon) {
                        char *p = (char *)(colon + 1);
                        while (*p && *p != ',' && *p != '}') { *p = ' '; ++p; }
                        break;
                    }
                }
            }

            /* 3) Free-form number fallback — when no numeric keys are configured,
             *    OR when every configured key missed this row.  In the latter
             *    case we rewind captured to 0 so the NaN placeholders don't
             *    exhaust the dimension budget, allowing free-form numbers to
             *    populate the matrix from the first slot. */
            if (engine->config.numeric_key_count == 0 || configured_valid_found == 0) {
                if (configured_valid_found == 0) {
                    captured = 0; /* discard NaN placeholders */
                    memset(valid_mask + rows * dims, 1, dims * sizeof(uint8_t));
                }
                double freeform[LOGANA_MAX_DIMENSIONS] = {0};
                size_t freeform_count = logana_collect_freeform_numbers(cursor, freeform, dims - captured);
                for (size_t i = 0; i < freeform_count && captured < dims; ++i) {
                    extracted[captured] = freeform[i];
                    extracted_valid[captured] = true; /* freeform parser already rejects traps */
                    ++captured;
                }
            }

            /* 4) Category keys for cardinality-aware grouping */
            char cat_buf[256] = {0};
            size_t cat_off = 0;
            for (size_t c = 0; c < engine->config.category_key_count; ++c) {
                char val[64] = {0};
                if (logana_extract_string(cursor, engine->config.category_keys[c],
                                          engine->config.case_sensitive, val, sizeof(val))) {
                    size_t vlen = strlen(val);
                    if (cat_off + vlen + 1 < sizeof(cat_buf)) {
                        if (cat_off > 0) cat_buf[cat_off++] = '|';
                        memcpy(cat_buf + cat_off, val, vlen);
                        cat_off += vlen;
                    }
                }
            }
            categories[rows] = cat_off > 0 ? logana_hash64(cat_buf, cat_off) : 0;

            /* 5) Synthetic fallback so the row is never completely empty.
             *    Only allowed when the user has NOT configured explicit keys,
             *    or when every configured key missed this row. */
            if ((engine->config.numeric_key_count == 0 || configured_valid_found == 0) && captured == 0) {
                extracted[captured] = (double)(logana_hash64(cursor, strlen(cursor)) % 1000000ULL) / 1000.0;
                extracted_valid[captured] = true;
                if (captured < dims) { extracted[++captured] = (double)strlen(cursor); extracted_valid[captured] = true; }
                if (captured < dims) { extracted[++captured] = (double)(logana_count_chars(cursor, '=') + logana_count_chars(cursor, ':')); extracted_valid[captured] = true; }
            }

            /* Pad remaining dimensions with NaN / invalid.  Replicating the
             * last real value makes the matrix look artificially dense. */
            while (captured < dims) {
                extracted[captured] = NAN;
                extracted_valid[captured] = false;
                valid_mask[rows * dims + captured] = 0;
                ++captured;
            }

            for (size_t d = 0; d < dims; ++d) {
                values[rows * dims + d] = extracted[d];
                if (!extracted_valid[d]) valid_mask[rows * dims + d] = 0;
            }

            ++rows;
        }
        if (!next) break;
        *next = '\n';
        cursor = next + 1;
    }

    job->matrix.values = values;
    job->matrix.timestamps = timestamps;
    job->matrix.valid_mask = valid_mask;
    job->matrix.categories = categories;
    job->matrix.formats = formats;
    job->matrix.row_count = rows;
    job->matrix.dimensions = dims;
    return rows;
}

static int logana_compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void logana_compute_summary(logana_job_t *job) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    job->summary.row_count = rows;
    job->summary.dimensions = dims;
    if (rows == 0 || dims == 0) return;

    size_t valid_counts[LOGANA_MAX_DIMENSIONS] = {0};
    for (size_t d = 0; d < dims; ++d) {
        job->summary.min[d] = INFINITY;
        job->summary.max[d] = -INFINITY;
    }

    double entropy_hist[32] = {0};

    /* --- Timestamp-based trend (slope) --- */
    bool has_real_ts = false;
    uint64_t min_ts = UINT64_MAX;
    uint64_t max_ts = 0;
    for (size_t r = 0; r < rows; ++r) {
        if (job->matrix.timestamps[r] > 0 && job->matrix.timestamps[r] != (uint64_t)r * 1000ULL) {
            has_real_ts = true;
            if (job->matrix.timestamps[r] < min_ts) min_ts = job->matrix.timestamps[r];
            if (job->matrix.timestamps[r] > max_ts) max_ts = job->matrix.timestamps[r];
        }
    }
    /* If real timestamps span more than 1 year, treat as garbage/mixed format */
    if (has_real_ts && max_ts > min_ts && (max_ts - min_ts) > (uint64_t)(365LL * 86400 * 1000)) {
        has_real_ts = false;
    }
    double t_mean = 0.0;
    if (has_real_ts) {
        double t_sum = 0.0;
        for (size_t r = 0; r < rows; ++r) t_sum += ((double)job->matrix.timestamps[r] - (double)min_ts) / 1000.0;
        t_mean = t_sum / (double)rows;
    } else {
        t_mean = ((double)rows - 1.0) / 2.0;
    }

    /* Pre-compute primary dimension range for adaptive entropy binning */
    double primary_min = INFINITY;
    double primary_max = -INFINITY;
    for (size_t r = 0; r < rows; ++r) {
        bool primary_valid = (!job->matrix.valid_mask || job->matrix.valid_mask[r * dims]);
        double primary = job->matrix.values[r * dims];
        if (primary_valid && isfinite(primary)) {
            if (primary < primary_min) primary_min = primary;
            if (primary > primary_max) primary_max = primary;
        }
    }
    double primary_range = (primary_max > primary_min && isfinite(primary_max - primary_min))
                           ? (primary_max - primary_min) : 1.0;

    double slope_num = 0.0;
    double slope_den = 0.0;

    for (size_t r = 0; r < rows; ++r) {
        bool primary_valid = (!job->matrix.valid_mask || job->matrix.valid_mask[r * dims]);
        double primary = job->matrix.values[r * dims];
        if (primary_valid && isfinite(primary)) {
            double bucket = 31.0 * (primary - primary_min) / primary_range;
            bucket = fmin(31.0, fmax(0.0, floor(bucket)));
            entropy_hist[(size_t)bucket] += 1.0;
        }

        double t = has_real_ts
            ? ((double)job->matrix.timestamps[r] - (double)min_ts) / 1000.0
            : (double)r;
        double dt = t - t_mean;
        if (primary_valid) {
            slope_num += dt * primary;
            slope_den += dt * dt;
        }

        for (size_t d = 0; d < dims; ++d) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            double value = job->matrix.values[r * dims + d];
            if (valid_counts[d] == 0) {
                job->summary.min[d] = value;
                job->summary.max[d] = value;
            } else {
                if (value < job->summary.min[d]) job->summary.min[d] = value;
                if (value > job->summary.max[d]) job->summary.max[d] = value;
            }
            job->summary.mean[d] += value;
            ++valid_counts[d];
        }
    }

    for (size_t d = 0; d < dims; ++d) {
        if (valid_counts[d] > 0) {
            job->summary.mean[d] /= (double)valid_counts[d];
        } else {
            job->summary.min[d] = 0.0;
            job->summary.max[d] = 0.0;
            job->summary.mean[d] = 0.0;
        }
    }

    /* ------------------------------------------------------------------
     * Robust statistics: winsorize each dimension at median ± 5·MAD
     * before computing mean and stddev.  This prevents a single extreme
     * value (e.g. a corrupted counter or mis-parsed timestamp) from
     * inflating variance and destroying z-score based outlier detection.
     * ------------------------------------------------------------------ */
    for (size_t d = 0; d < dims; ++d) {
        if (valid_counts[d] == 0) continue;

        size_t n = valid_counts[d];
        double *vals = malloc(n * sizeof(double));
        if (!vals) continue;

        size_t idx = 0;
        for (size_t r = 0; r < rows; ++r) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            vals[idx++] = job->matrix.values[r * dims + d];
        }

        qsort(vals, n, sizeof(double), logana_compare_double);

        double median = vals[n / 2];
        if ((n % 2) == 0) median = (vals[n / 2 - 1] + vals[n / 2]) * 0.5;

        for (size_t i = 0; i < n; ++i) vals[i] = fabs(vals[i] - median);
        qsort(vals, n, sizeof(double), logana_compare_double);
        double mad = vals[n / 2];
        if ((n % 2) == 0) mad = (vals[n / 2 - 1] + vals[n / 2]) * 0.5;

        double mad_sigma = mad > 0.0001 ? mad * 1.4826 : 0.0001;
        double lower = median - 5.0 * mad_sigma;
        double upper = median + 5.0 * mad_sigma;

        double wsum = 0.0;
        for (size_t r = 0; r < rows; ++r) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            double v = job->matrix.values[r * dims + d];
            if (v < lower) v = lower;
            if (v > upper) v = upper;
            wsum += v;
        }
        job->summary.mean[d] = wsum / (double)n;

        double wvar = 0.0;
        for (size_t r = 0; r < rows; ++r) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            double v = job->matrix.values[r * dims + d];
            if (v < lower) v = lower;
            if (v > upper) v = upper;
            double delta = v - job->summary.mean[d];
            wvar += delta * delta;
        }
        job->summary.stddev[d] = sqrt(wvar / (double)n);

        free(vals);
    }

    size_t outliers = 0;
    for (size_t r = 0; r < rows; ++r) {
        double score = 0.0;
        size_t valid_d = 0;
        for (size_t d = 0; d < dims; ++d) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            double sigma = job->summary.stddev[d] > 0.0001 ? job->summary.stddev[d] : 1.0;
            double z = (job->matrix.values[r * dims + d] - job->summary.mean[d]) / sigma;
            score += z * z;
            ++valid_d;
        }
        if (valid_d > 0 && score > 9.0) ++outliers;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (entropy_hist[i] <= 0.0) continue;
        double p = entropy_hist[i] / (double)rows;
        job->summary.entropy -= p * log2(p);
    }

    job->summary.slope = slope_den > 0.0 ? (slope_num / slope_den) : 0.0;
    job->summary.outlier_ratio = rows ? ((double)outliers / (double)rows) : 0.0;

    /* Format consistency / schema drift score */
    if (job->matrix.formats && rows > 0) {
        size_t fmt_counts[3] = {0};
        for (size_t r = 0; r < rows; ++r) {
            uint8_t f = job->matrix.formats[r];
            if (f < 3) fmt_counts[f]++;
        }
        double fmt_entropy = 0.0;
        for (size_t f = 0; f < 3; ++f) {
            if (fmt_counts[f] == 0) continue;
            double p = (double)fmt_counts[f] / (double)rows;
            fmt_entropy -= p * log2(p);
        }
        double max_entropy = log2(3.0);
        job->summary.schema_drift = max_entropy > 0.0 ? (fmt_entropy / max_entropy) : 0.0;
    } else {
        job->summary.schema_drift = 0.0;
    }
}

static size_t logana_assign_cluster_modes(const double *modes, size_t mode_count, size_t dims,
                                          double *candidate, logana_distance_fn_t dist_fn,
                                          const logana_analysis_summary_t *summary) {
    for (size_t i = 0; i < mode_count; ++i) {
        if (dist_fn(modes + i * dims, candidate, NULL, NULL, dims, summary) < 0.04) return i;
    }
    memcpy((double *)(modes + mode_count * dims), candidate, dims * sizeof(double));
    return mode_count;
}

/* --------------------------------------------------------------------------
 * Standard K-Means++ : probabilistic D(x)^2 seeding with LCG
 * -------------------------------------------------------------------------- */

static uint64_t logana_lcg_next(uint64_t *state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

static double logana_lcg_double01(uint64_t *state) {
    /* 53-bit precision uniform in [0, 1) */
    return (double)(logana_lcg_next(state) >> 11) * (1.0 / (double)(1ULL << 53));
}

static double logana_kmeans_inertia(logana_job_t *job, size_t k, double *centers,
                                    logana_distance_fn_t dist_fn,
                                    const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    double inertia = 0.0;
    for (size_t r = 0; r < rows; ++r) {
        double nearest = 1e18;
        for (size_t c = 0; c < k; ++c) {
            double dist = dist_fn(job->matrix.values + r * dims, centers + c * dims,
                                  job->matrix.valid_mask + r * dims, NULL, dims, summary);
            if (dist < nearest) nearest = dist;
        }
        inertia += nearest;
    }
    return inertia;
}

static void logana_kmeans_pp_init(logana_job_t *job, size_t k, uint64_t seed,
                                  double *centers,
                                  logana_distance_fn_t dist_fn,
                                  const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (!rows || !k) return;

    uint64_t rng = seed;

    /* 1. Choose first center uniformly at random */
    size_t first_idx = (size_t)(logana_lcg_double01(&rng) * (double)rows);
    if (first_idx >= rows) first_idx = rows - 1;
    memcpy(centers, job->matrix.values + first_idx * dims, dims * sizeof(double));

    if (k == 1) return;

    double *dists = malloc(rows * sizeof(double));
    if (!dists) {
        /* Fallback: deterministic spacing if malloc fails */
        for (size_t c = 1; c < k; ++c) {
            size_t idx = (rows * c) / k;
            memcpy(centers + c * dims, job->matrix.values + idx * dims, dims * sizeof(double));
        }
        return;
    }

    for (size_t c = 1; c < k; ++c) {
        double dist_sum = 0.0;
        for (size_t r = 0; r < rows; ++r) {
            double nearest = 1e18;
            for (size_t j = 0; j < c; ++j) {
                double dist = dist_fn(job->matrix.values + r * dims, centers + j * dims,
                                      job->matrix.valid_mask + r * dims, NULL, dims, summary);
                if (dist < nearest) nearest = dist;
            }
            dists[r] = nearest * nearest; /* D(x)^2 */
            dist_sum += dists[r];
        }

        if (dist_sum <= 0.0) {
            /* All remaining points coincide with existing centers */
            for (size_t j = c; j < k; ++j) {
                memcpy(centers + j * dims, centers, dims * sizeof(double));
            }
            break;
        }

        /* 2. Roulette-wheel selection proportional to D(x)^2 */
        double pick = logana_lcg_double01(&rng) * dist_sum;
        double accum = 0.0;
        size_t chosen = 0;
        for (size_t r = 0; r < rows; ++r) {
            accum += dists[r];
            if (accum >= pick) {
                chosen = r;
                break;
            }
        }
        memcpy(centers + c * dims, job->matrix.values + chosen * dims, dims * sizeof(double));
    }

    free(dists);
}

static size_t logana_run_kmeans(logana_job_t *job, size_t k, size_t seed_offset,
                                logana_distance_fn_t dist_fn,
                                const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (!rows) return 0;
    if (k > rows) k = rows;
    if (k == 0) k = 1;
    if (k > 8) k = 8;

    double centers[LOGANA_MAX_DIMENSIONS * 8] = {0};
    double best_centers[LOGANA_MAX_DIMENSIONS * 8] = {0};
    int *tmp_labels = malloc(rows * sizeof(int));
    if (!tmp_labels) return 0;

    /* Standard K-Means++: multiple initialisations, keep best inertia */
    const size_t n_init = (rows > 1000) ? 3 : 5;
    double best_inertia = 1e300;

    for (size_t trial = 0; trial < n_init; ++trial) {
        uint64_t seed = (uint64_t)(seed_offset + trial * 31 + 0x9e3779b9);
        logana_kmeans_pp_init(job, k, seed, centers, dist_fn, summary);

        /* Lloyd's algorithm */
        for (size_t iter = 0; iter < 30; ++iter) {
            double accum[LOGANA_MAX_DIMENSIONS * 8] = {0};
            size_t counts[8] = {0};
            for (size_t r = 0; r < rows; ++r) {
                double best_dist = 1e18;
                int best = 0;
                for (size_t c = 0; c < k; ++c) {
                    double dist = dist_fn(job->matrix.values + r * dims, centers + c * dims,
                                          job->matrix.valid_mask + r * dims, NULL, dims, summary);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best = (int)c;
                    }
                }
                tmp_labels[r] = best;
                counts[best]++;
                for (size_t d = 0; d < dims; ++d) {
                    if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
                    accum[best * dims + d] += job->matrix.values[r * dims + d];
                }
            }
            for (size_t c = 0; c < k; ++c) {
                if (!counts[c]) continue;
                for (size_t d = 0; d < dims; ++d) {
                    if (job->matrix.valid_mask) {
                        size_t valid_cnt = 0;
                        for (size_t r = 0; r < rows; ++r) {
                            if (tmp_labels[r] == (int)c && job->matrix.valid_mask[r * dims + d]) valid_cnt++;
                        }
                        if (valid_cnt) centers[c * dims + d] = (double)(accum[c * dims + d] / (double)valid_cnt);
                    } else {
                        centers[c * dims + d] = (double)(accum[c * dims + d] / (double)counts[c]);
                    }
                }
            }
        }

        double inertia = logana_kmeans_inertia(job, k, centers, dist_fn, summary);
        if (inertia < best_inertia) {
            best_inertia = inertia;
            memcpy(best_centers, centers, sizeof(centers));
            memcpy(job->matrix.labels, tmp_labels, rows * sizeof(int));
        }
    }

    /* Final assignment using the best discovered centroids */
    memcpy(centers, best_centers, sizeof(best_centers));
    for (size_t r = 0; r < rows; ++r) {
        double best_dist = 1e18;
        int best = 0;
        for (size_t c = 0; c < k; ++c) {
            double dist = dist_fn(job->matrix.values + r * dims, centers + c * dims,
                                  job->matrix.valid_mask + r * dims, NULL, dims, summary);
            if (dist < best_dist) {
                best_dist = dist;
                best = (int)c;
            }
        }
        job->matrix.labels[r] = best;
    }

    free(tmp_labels);
    return k;
}

static double logana_compute_collapse_score(size_t rows, int *labels, size_t k) {
    if (rows == 0 || k == 0) return 1.0;
    size_t *counts = calloc(k, sizeof(size_t));
    if (!counts) return 1.0;
    for (size_t i = 0; i < rows; ++i) {
        int lb = labels[i];
        if (lb >= 0 && (size_t)lb < k) counts[lb]++;
    }
    size_t max_count = 0;
    size_t empty = 0;
    for (size_t c = 0; c < k; ++c) {
        if (counts[c] > max_count) max_count = counts[c];
        if (counts[c] == 0) empty++;
    }
    free(counts);
    double dominance = (double)max_count / (double)rows;
    double empty_penalty = (double)empty / (double)k;
    return dominance * 0.6 + empty_penalty * 0.4;
}

static size_t logana_run_kmeans_auto(logana_job_t *job,
                                     logana_distance_fn_t dist_fn,
                                     const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    if (!rows) return 0;
    if (rows == 1) {
        job->matrix.labels[0] = 0;
        return 1;
    }

    int *tmp_labels = calloc(rows, sizeof(int));
    int *best_labels = calloc(rows, sizeof(int));
    if (!tmp_labels || !best_labels) {
        free(tmp_labels); free(best_labels);
        return logana_run_kmeans(job, 2, 0, dist_fn, summary);
    }

    double best_score = 2.0;
    size_t best_actual_k = 1;

    for (size_t k = 2; k <= 4; ++k) {
        if (k > rows) break;
        for (size_t seed = 0; seed < 3; ++seed) {
            memset(tmp_labels, 0, rows * sizeof(int));
            logana_job_t trial_job = *job;
            trial_job.matrix.labels = tmp_labels;
            size_t actual_k = logana_run_kmeans(&trial_job, k, seed * 7, dist_fn, summary);
            double score = logana_compute_collapse_score(rows, tmp_labels, actual_k);
            if (score < best_score) {
                best_score = score;
                best_actual_k = actual_k;
                memcpy(best_labels, tmp_labels, rows * sizeof(int));
            }
        }
    }

    memcpy(job->matrix.labels, best_labels, rows * sizeof(int));
    free(tmp_labels);
    free(best_labels);
    return best_actual_k;
}

static size_t logana_cluster_by_category(logana_job_t *job, size_t target_k,
                                         logana_distance_fn_t dist_fn,
                                         const logana_analysis_summary_t *summary,
                                         bool use_auto) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    uint64_t *cats = job->matrix.categories;
    if (!cats || rows == 0) {
        if (use_auto) return logana_run_kmeans_auto(job, dist_fn, summary);
        return logana_run_kmeans(job, target_k, 0, dist_fn, summary);
    }

    uint64_t unique_cats[256];
    size_t cat_idx_map[LOGANA_MAX_ROWS < 4096 ? LOGANA_MAX_ROWS : 4096];
    size_t unique_count = 0;

    if (rows > 4096) {
        if (use_auto) return logana_run_kmeans_auto(job, dist_fn, summary);
        return logana_run_kmeans(job, target_k, 0, dist_fn, summary);
    }

    for (size_t r = 0; r < rows; ++r) {
        size_t found = (size_t)-1;
        for (size_t u = 0; u < unique_count; ++u) {
            if (unique_cats[u] == cats[r]) { found = u; break; }
        }
        if (found == (size_t)-1) {
            if (unique_count >= 256) {
                if (use_auto) return logana_run_kmeans_auto(job, dist_fn, summary);
                return logana_run_kmeans(job, target_k, 0, dist_fn, summary);
            }
            found = unique_count;
            unique_cats[unique_count++] = cats[r];
        }
        cat_idx_map[r] = found;
    }

    /* High cardinality guard: if more than half the rows are unique,
     * the category field is effectively an ID and provides no grouping signal. */
    if (unique_count <= 1 || unique_count > rows / 2) {
        if (use_auto) return logana_run_kmeans_auto(job, dist_fn, summary);
        return logana_run_kmeans(job, target_k, 0, dist_fn, summary);
    }

    size_t cat_rows[256] = {0};
    for (size_t r = 0; r < rows; ++r) cat_rows[cat_idx_map[r]]++;

    size_t global_label_offset = 0;
    for (size_t u = 0; u < unique_count; ++u) {
        size_t sub_rows = cat_rows[u];
        if (sub_rows == 0) continue;

        double *sub_values = malloc(sub_rows * dims * sizeof(double));
        uint8_t *sub_mask = malloc(sub_rows * dims * sizeof(uint8_t));
        uint64_t *sub_ts = malloc(sub_rows * sizeof(uint64_t));
        int *sub_labels = calloc(sub_rows, sizeof(int));
        if (!sub_values || !sub_mask || !sub_ts || !sub_labels) {
            free(sub_values); free(sub_mask); free(sub_ts); free(sub_labels);
            continue;
        }

        size_t sr = 0;
        for (size_t r = 0; r < rows; ++r) {
            if (cat_idx_map[r] != u) continue;
            memcpy(sub_values + sr * dims, job->matrix.values + r * dims, dims * sizeof(double));
            memcpy(sub_mask + sr * dims, job->matrix.valid_mask + r * dims, dims * sizeof(uint8_t));
            sub_ts[sr] = job->matrix.timestamps[r];
            sr++;
        }

        logana_job_t sub_job = {0};
        sub_job.matrix.values = sub_values;
        sub_job.matrix.valid_mask = sub_mask;
        sub_job.matrix.timestamps = sub_ts;
        sub_job.matrix.row_count = sub_rows;
        sub_job.matrix.dimensions = dims;
        sub_job.matrix.labels = sub_labels;

        size_t sub_k = target_k;
        if (sub_k > sub_rows) sub_k = sub_rows;
        if (sub_k < 1) sub_k = 1;

        size_t sub_clusters;
        if (use_auto) {
            sub_clusters = logana_run_kmeans_auto(&sub_job, dist_fn, summary);
        } else {
            sub_clusters = logana_run_kmeans(&sub_job, sub_k, 0, dist_fn, summary);
        }

        sr = 0;
        for (size_t r = 0; r < rows; ++r) {
            if (cat_idx_map[r] != u) continue;
            job->matrix.labels[r] = (int)(sub_labels[sr] + global_label_offset);
            sr++;
        }
        global_label_offset += sub_clusters;

        free(sub_values); free(sub_mask); free(sub_ts); free(sub_labels);
    }

    return global_label_offset;
}

static size_t logana_run_dbscan(logana_job_t *job, double eps, size_t min_samples,
                                logana_distance_fn_t dist_fn,
                                const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    int *labels = job->matrix.labels;
    for (size_t i = 0; i < rows; ++i) labels[i] = -2;
    size_t cluster_id = 0;
    for (size_t i = 0; i < rows; ++i) {
        if (labels[i] != -2) continue;
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            if (dist_fn(job->matrix.values + i * dims, job->matrix.values + j * dims,
                        job->matrix.valid_mask + i * dims, job->matrix.valid_mask + j * dims, dims, summary) <= eps)
                count++;
        }
        if (count < min_samples) {
            labels[i] = -1;
            continue;
        }
        labels[i] = (int)cluster_id;
        for (size_t j = 0; j < rows; ++j) {
            if (labels[j] == -2 &&
                dist_fn(job->matrix.values + i * dims, job->matrix.values + j * dims,
                        job->matrix.valid_mask + i * dims, job->matrix.valid_mask + j * dims, dims, summary) <= eps) {
                labels[j] = (int)cluster_id;
            }
        }
        cluster_id++;
    }
    return cluster_id;
}

static size_t logana_run_birch(logana_job_t *job, double threshold,
                               logana_distance_fn_t dist_fn,
                               const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    double centroids[LOGANA_MAX_DIMENSIONS * 8] = {0};
    size_t counts[8] = {0};
    size_t cluster_count = 0;
    for (size_t r = 0; r < rows; ++r) {
        size_t best = 0;
        double best_dist = 1e18;
        for (size_t c = 0; c < cluster_count; ++c) {
            double dist = dist_fn(job->matrix.values + r * dims, centroids + c * dims,
                                  job->matrix.valid_mask + r * dims, NULL, dims, summary);
            if (dist < best_dist) {
                best_dist = dist;
                best = c;
            }
        }
        if (cluster_count == 0 || best_dist > threshold) {
            best = cluster_count < 8 ? cluster_count++ : 7;
            memcpy(centroids + best * dims, job->matrix.values + r * dims, dims * sizeof(double));
        }
        job->matrix.labels[r] = (int)best;
        counts[best]++;
        for (size_t d = 0; d < dims; ++d) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            centroids[best * dims + d] =
                (double)(((double)centroids[best * dims + d] * (double)(counts[best] - 1) +
                         (double)job->matrix.values[r * dims + d]) / (double)counts[best]);
        }
    }
    return cluster_count;
}

static size_t logana_run_mean_shift(logana_job_t *job, double bandwidth,
                                    logana_distance_fn_t dist_fn,
                                    const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    double modes[LOGANA_MAX_DIMENSIONS * 16] = {0};
    size_t mode_count = 0;
    for (size_t r = 0; r < rows; ++r) {
        double point[LOGANA_MAX_DIMENSIONS];
        memcpy(point, job->matrix.values + r * dims, dims * sizeof(double));
        for (size_t iter = 0; iter < 6; ++iter) {
            double accum[LOGANA_MAX_DIMENSIONS] = {0};
            size_t count = 0;
            for (size_t j = 0; j < rows; ++j) {
                if (dist_fn(point, job->matrix.values + j * dims,
                            NULL, job->matrix.valid_mask + j * dims, dims, summary) <= bandwidth) {
                    for (size_t d = 0; d < dims; ++d) {
                        if (job->matrix.valid_mask && !job->matrix.valid_mask[j * dims + d]) continue;
                        accum[d] += job->matrix.values[j * dims + d];
                    }
                    count++;
                }
            }
            if (!count) break;
            for (size_t d = 0; d < dims; ++d) point[d] = (double)(accum[d] / (double)count);
        }
        size_t label = logana_assign_cluster_modes(modes, mode_count, dims, point, dist_fn, summary);
        if (label == mode_count && mode_count < 16) mode_count++;
        job->matrix.labels[r] = (int)label;
    }
    return mode_count;
}

static size_t logana_run_optics(logana_job_t *job, double eps, size_t min_samples,
                                logana_distance_fn_t dist_fn,
                                const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    logana_neighbor_t neighbors[LOGANA_MAX_ROWS < 4096 ? LOGANA_MAX_ROWS : 4096];
    if (rows > 4096) rows = 4096;
    for (size_t i = 0; i < rows; ++i) {
        size_t count = 0;
        for (size_t j = 0; j < rows; ++j) {
            double dist = dist_fn(job->matrix.values + i * dims, job->matrix.values + j * dims,
                                  job->matrix.valid_mask + i * dims, job->matrix.valid_mask + j * dims, dims, summary);
            if (dist <= eps) count++;
        }
        neighbors[i].idx = i;
        neighbors[i].dist = -(double)count;
    }
    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = i + 1; j < rows; ++j) {
            if (neighbors[j].dist < neighbors[i].dist) {
                logana_neighbor_t tmp = neighbors[i];
                neighbors[i] = neighbors[j];
                neighbors[j] = tmp;
            }
        }
    }
    size_t cluster = 0;
    for (size_t rank = 0; rank < rows; ++rank) {
        size_t idx = neighbors[rank].idx;
        job->matrix.labels[idx] = (rank < min_samples) ? 0 : (int)cluster;
        if ((rank + 1) % min_samples == 0) cluster++;
    }
    return cluster + 1;
}

static size_t logana_run_gmm(logana_job_t *job, size_t k,
                             const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (k > 3) k = 3;
    double means[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double variances[LOGANA_MAX_DIMENSIONS * 3] = {0};
    double weights[3] = {0.34, 0.33, 0.33};
    for (size_t c = 0; c < k; ++c) {
        memcpy(means + c * dims, job->matrix.values + (c % rows) * dims, dims * sizeof(double));
        for (size_t d = 0; d < dims; ++d) variances[c * dims + d] = 1.0;
    }
    for (size_t iter = 0; iter < 8; ++iter) {
        double resp_sum[3] = {0};
        double mean_accum[LOGANA_MAX_DIMENSIONS * 3] = {0};
        for (size_t r = 0; r < rows; ++r) {
            double probs[3] = {0};
            double norm = 0.0;
            for (size_t c = 0; c < k; ++c) {
                double dist = 0.0;
                size_t valid_d = 0;
                for (size_t d = 0; d < dims; ++d) {
                    if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
                    double sigma = summary && summary->stddev[d] > 0.0001 ? summary->stddev[d] : 1.0;
                    double delta = ((double)job->matrix.values[r * dims + d] - means[c * dims + d]) / sigma;
                    dist += (delta * delta) / variances[c * dims + d];
                    ++valid_d;
                }
                if (valid_d > 0) dist *= (double)dims / (double)valid_d;
                probs[c] = weights[c] * exp(-0.5 * dist);
                norm += probs[c];
            }
            if (norm == 0.0) norm = 1.0;
            int best = 0;
            double best_resp = -1.0;
            for (size_t c = 0; c < k; ++c) {
                double resp = probs[c] / norm;
                resp_sum[c] += resp;
                if (resp > best_resp) {
                    best_resp = resp;
                    best = (int)c;
                }
                for (size_t d = 0; d < dims; ++d) {
                    if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
                    mean_accum[c * dims + d] += resp * job->matrix.values[r * dims + d];
                }
            }
            job->matrix.labels[r] = best;
        }
        for (size_t c = 0; c < k; ++c) {
            double denom = resp_sum[c] > 0.0 ? resp_sum[c] : 1.0;
            weights[c] = resp_sum[c] / (double)rows;
            for (size_t d = 0; d < dims; ++d) means[c * dims + d] = (double)(mean_accum[c * dims + d] / denom);
        }
    }
    return k;
}

static size_t logana_run_agglomerative(logana_job_t *job, size_t target_clusters,
                                       logana_distance_fn_t dist_fn,
                                       const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    int active[1024];
    if (rows > 1024) rows = 1024;
    for (size_t i = 0; i < rows; ++i) active[i] = (int)i;
    size_t clusters = rows;
    while (clusters > target_clusters && clusters > 1) {
        double best = 1e18;
        size_t best_i = 0, best_j = 1;
        for (size_t i = 0; i < rows; ++i) {
            if (active[i] < 0) continue;
            for (size_t j = i + 1; j < rows; ++j) {
                if (active[j] < 0) continue;
                double dist = dist_fn(job->matrix.values + i * dims, job->matrix.values + j * dims,
                                      job->matrix.valid_mask + i * dims, job->matrix.valid_mask + j * dims, dims, summary);
                if (dist < best) {
                    best = dist;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        active[best_j] = active[best_i];
        clusters--;
    }
    int label_map[1024];
    memset(label_map, -1, sizeof(label_map));
    int next_label = 0;
    for (size_t i = 0; i < rows; ++i) {
        int root = active[i];
        if (root < 0) root = (int)i;
        if (label_map[root] < 0) label_map[root] = next_label++;
        job->matrix.labels[i] = label_map[root];
    }
    return (size_t)next_label;
}

static size_t logana_cluster(logana_engine_t *engine, logana_job_t *job,
                             const logana_analysis_summary_t *summary) {
    size_t rows = job->matrix.row_count;
    if (!rows) return 0;
    job->matrix.labels = calloc(rows, sizeof(int));
    if (!job->matrix.labels) return 0;

    logana_distance_fn_t dist_fn = logana_distance_euclidean_sq;
    switch (engine->config.distance_metric) {
        case LOGANA_DIST_MANHATTAN: dist_fn = logana_distance_manhattan; break;
        case LOGANA_DIST_ZSCORE:    dist_fn = logana_distance_zscore_sq; break;
        default:                    dist_fn = logana_distance_euclidean_sq; break;
    }

    switch (job->algorithm) {
        case LOGANA_ALGO_KMEANS_PP:
            if (job->matrix.categories && job->matrix.row_count > 0) {
                return logana_cluster_by_category(job, 3, dist_fn, summary, false);
            }
            return logana_run_kmeans(job, 3, 0, dist_fn, summary);
        case LOGANA_ALGO_AUTO:
            if (job->matrix.categories && job->matrix.row_count > 0) {
                return logana_cluster_by_category(job, 3, dist_fn, summary, true);
            }
            return logana_run_kmeans_auto(job, dist_fn, summary);
        case LOGANA_ALGO_DBSCAN: return logana_run_dbscan(job, engine->config.dbscan_eps, engine->config.dbscan_min_samples, dist_fn, summary);
        case LOGANA_ALGO_BIRCH: return logana_run_birch(job, engine->config.dbscan_eps, dist_fn, summary);
        case LOGANA_ALGO_MEAN_SHIFT: return logana_run_mean_shift(job, engine->config.dbscan_eps * 2.0, dist_fn, summary);
        case LOGANA_ALGO_OPTICS: return logana_run_optics(job, engine->config.dbscan_eps * 1.2, engine->config.dbscan_min_samples, dist_fn, summary);
        case LOGANA_ALGO_GMM: return logana_run_gmm(job, 3, summary);
        case LOGANA_ALGO_AGGLOMERATIVE: return logana_run_agglomerative(job, 3, dist_fn, summary);
        case LOGANA_ALGO_FALLBACK_SCATTERPLOT:
            for (size_t r = 0; r < rows; ++r) job->matrix.labels[r] = -1;
            return 0;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Auto-mode algorithm selector with LRU cache
 * -------------------------------------------------------------------------- */

static uint64_t logana_compute_fingerprint(const logana_job_t *job, size_t rows, size_t active_dims)
{
    size_t sample = job->payload_size > 1024 ? 1024 : job->payload_size;
    uint64_t h = logana_hash64(job->payload, sample);
    h ^= (uint64_t)rows * 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)active_dims * 0xbf58476d1ce4e5b9ULL;
    h ^= (uint64_t)job->matrix.dimensions * 0x94d049bb133111ebULL;
    return h;
}

static logana_algorithm_t logana_auto_cache_lookup(logana_engine_t *engine, uint64_t fp, uint64_t now_ms)
{
    for (size_t i = 0; i < engine->auto_cache_count; ++i) {
        if (engine->auto_cache[i].fingerprint == fp) {
            engine->auto_cache[i].last_used_ms = now_ms;
            return engine->auto_cache[i].selected;
        }
    }
    return LOGANA_ALGO_AUTO; /* sentinel: cache miss */
}

static void logana_auto_cache_insert(logana_engine_t *engine, uint64_t fp,
                                     logana_algorithm_t selected, uint64_t now_ms)
{
    if (engine->auto_cache_count < LOGANA_MAX_AUTO_CACHE) {
        engine->auto_cache[engine->auto_cache_count++] =
            (logana_auto_cache_entry_t){fp, selected, now_ms};
        return;
    }
    /* LRU eviction */
    size_t evict = 0;
    uint64_t oldest = engine->auto_cache[0].last_used_ms;
    for (size_t i = 1; i < engine->auto_cache_count; ++i) {
        if (engine->auto_cache[i].last_used_ms < oldest) {
            oldest = engine->auto_cache[i].last_used_ms;
            evict = i;
        }
    }
    engine->auto_cache[evict] = (logana_auto_cache_entry_t){fp, selected, now_ms};
}

static logana_algorithm_t logana_select_algorithm_auto(const logana_engine_t *engine,
                                                       const logana_job_t *job,
                                                       size_t rows,
                                                       size_t active_dims,
                                                       double matrix_density,
                                                       size_t unique_cat_count)
{
    (void)engine;

    /* Stage 0: Hard guardrails */
    if (rows < 2 || active_dims == 0) {
        return LOGANA_ALGO_FALLBACK_SCATTERPLOT;
    }

    /* Stage 0.5: Tiny datasets — agglomerative is the only robust
     * method when there are too few points for statistical stability. */
    if (rows <= 5) {
        return LOGANA_ALGO_AGGLOMERATIVE;
    }

    /* Stage 1: Large-scale dense streaming -> BIRCH
     * BIRCH excels on high-volume data because it builds a CF-tree in a
     * single pass and avoids O(N^2) distance matrix materialisation. */
    if (rows > 1000 && matrix_density > 0.5) {
        return LOGANA_ALGO_BIRCH;
    }

    /* Stage 2: Noisy data with arbitrary cluster shapes -> DBSCAN
     * DBSCAN does not assume globular clusters and naturally labels
     * sparse outliers as noise (-1). */
    if (job->summary.outlier_ratio > 0.15 && rows > 50) {
        return LOGANA_ALGO_DBSCAN;
    }

    /* Stage 3: Hierarchical / categorical structure -> Agglomerative
     * When the data naturally splits into a small number of categories,
     * agglomerative clustering preserves the dendrogram semantics. */
    if (unique_cat_count > 1 && unique_cat_count <= 8 && rows <= 1024) {
        return LOGANA_ALGO_AGGLOMERATIVE;
    }

    /* Stage 4: Small, low-dimensional, non-parametric -> Mean-Shift
     * Mean-Shift requires no prior cluster count and works well when
     * the sample size is small enough to keep the mode search cheap. */
    if (rows <= 200 && active_dims <= 3 && matrix_density < 0.7) {
        return LOGANA_ALGO_MEAN_SHIFT;
    }

    /* Stage 5: Variable-density clusters -> OPTICS
     * OPTICS generalises DBSCAN by ordering points according to
     * reachability; ideal when local densities vary widely. */
    if (rows > 100 && matrix_density < 0.6) {
        return LOGANA_ALGO_OPTICS;
    }

    /* Stage 6: Moderate-size soft clustering -> GMM
     * Gaussian Mixture Models provide probabilistic assignments and
     * handle elliptical covariance better than hard K-means. */
    if (rows <= 500 && active_dims >= 2 && matrix_density > 0.4) {
        return LOGANA_ALGO_GMM;
    }

    /* Stage 7: K-means++ territory — ONLY when the data is explicitly
     * dense, uniform, globular, high-dimensional, and low-noise.
     * K-means++ remains a dominant active-duty algorithm, but it is a
     * specialist, not a universal default.  We dispatch it only when
     * the topology is unambiguously favourable. */
    if (rows > 100 && active_dims >= 3 && matrix_density > 0.75 && job->summary.outlier_ratio < 0.05) {
        return LOGANA_ALGO_KMEANS_PP;
    }

    /* Stage 8: Default — ambiguous or non-globular topology.
     * DBSCAN is more forgiving than K-means++ on irregular shapes,
     * mixed densities, and unlabeled noise points common in logs.
     * This makes the auto-mode default resilient against misuse. */
    return LOGANA_ALGO_DBSCAN;
}

int logana_analyze_job(logana_engine_t *engine, logana_job_t *job) {
    logana_set_job_status(job, LOGANA_JOB_ANALYZING, NULL);
    size_t rows = logana_parse_matrix(engine, job);
    if (!rows) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "no analyzable rows found");
        return -1;
    }
    logana_compute_summary(job);

    /* ------------------------------------------------------------------
     * Auto-mode defensive scheduler + smart algorithm selection
     * ------------------------------------------------------------------ */
    if (job->algorithm == LOGANA_ALGO_AUTO) {
        const double SPARSITY_DROP_THRESHOLD = 0.20;
        size_t active_dims = 0;
        size_t total_valid = 0;
        for (size_t d = 0; d < job->matrix.dimensions; ++d) {
            size_t valid_cnt = 0;
            for (size_t r = 0; r < rows; ++r) {
                if (job->matrix.valid_mask[r * job->matrix.dimensions + d]) ++valid_cnt;
            }
            total_valid += valid_cnt;
            double density = (double)valid_cnt / (double)rows;
            if (density >= SPARSITY_DROP_THRESHOLD) ++active_dims;
        }
        double matrix_density = (double)total_valid / (double)(rows * job->matrix.dimensions);

        /* Early exit: insufficient rows or no viable dimensions */
        if (rows < 2 || active_dims == 0) {
            job->algorithm = LOGANA_ALGO_FALLBACK_SCATTERPLOT;
            job->summary.cluster_count = 0;
            job->summary.slope = 0.0;
            job->summary.cluster_balance = 0.0;
            job->summary.outlier_ratio = 0.0;
            if (job->matrix.labels) { free(job->matrix.labels); job->matrix.labels = NULL; }
            job->matrix.labels = calloc(rows, sizeof(int));
            if (job->matrix.labels) {
                for (size_t r = 0; r < rows; ++r) job->matrix.labels[r] = -1;
            }
            return 0;
        }

        /* Count unique categories for hierarchical heuristics */
        size_t unique_cat_count = 0;
        if (job->matrix.categories) {
            uint64_t seen[256] = {0};
            for (size_t r = 0; r < rows && unique_cat_count < 256; ++r) {
                uint64_t cat = job->matrix.categories[r];
                bool found = false;
                for (size_t i = 0; i < unique_cat_count; ++i) {
                    if (seen[i] == cat) { found = true; break; }
                }
                if (!found) seen[unique_cat_count++] = cat;
            }
        }

        uint64_t now_ms = logana_now_ms();
        uint64_t fp = logana_compute_fingerprint(job, rows, active_dims);
        logana_algorithm_t cached = logana_auto_cache_lookup(engine, fp, now_ms);

        if (cached != LOGANA_ALGO_AUTO) {
            ttak_logger_log(&engine->logger, TTAK_LOG_DEBUG,
                            "auto cache hit: fp=%llx -> %s",
                            (unsigned long long)fp,
                            logana_algorithm_name(cached));
            job->algorithm = cached;
        } else {
            logana_algorithm_t selected = logana_select_algorithm_auto(
                engine, job, rows, active_dims, matrix_density, unique_cat_count);
            job->algorithm = selected;
            logana_auto_cache_insert(engine, fp, selected, now_ms);
            ttak_logger_log(&engine->logger, TTAK_LOG_INFO,
                            "auto selected: rows=%zu active_dims=%zu density=%.3f cats=%zu -> %s",
                            rows, active_dims, matrix_density, unique_cat_count,
                            logana_algorithm_name(selected));
        }
    }

    job->summary.cluster_count = logana_cluster(engine, job, &job->summary);

    /* Compute cluster balance (collapse metric) */
    if (job->summary.cluster_count > 1 && job->matrix.labels) {
        size_t *counts = calloc(job->summary.cluster_count, sizeof(size_t));
        if (counts) {
            for (size_t i = 0; i < job->matrix.row_count; ++i) {
                int lb = job->matrix.labels[i];
                if (lb >= 0 && (size_t)lb < job->summary.cluster_count) counts[lb]++;
            }
            size_t max_c = 0, min_c = job->matrix.row_count;
            for (size_t c = 0; c < job->summary.cluster_count; ++c) {
                if (counts[c] > max_c) max_c = counts[c];
                if (counts[c] < min_c) min_c = counts[c];
            }
            job->summary.cluster_balance = max_c > 0 ? (double)min_c / (double)max_c : 0.0;
            free(counts);
        }
    } else {
        job->summary.cluster_balance = job->summary.cluster_count == 1 ? 1.0 : 0.0;
    }

    return 0;
}

static void logana_register_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&engine->jobs_lock);
    /* Evict oldest completed jobs to prevent stale data pollution */
    if (engine->job_count >= LOGANA_MAX_JOBS) {
        for (size_t i = 0; i < engine->job_count; ++i) {
            logana_job_t *j = engine->jobs[i];
            if (!j) continue;
            pthread_mutex_lock(&j->lock);
            bool done = (j->status == LOGANA_JOB_READY || j->status == LOGANA_JOB_FAILED);
            pthread_mutex_unlock(&j->lock);
            if (done) {
                logana_job_destroy(j);
                memmove(&engine->jobs[i], &engine->jobs[i + 1],
                        (engine->job_count - i - 1) * sizeof(logana_job_t *));
                engine->job_count--;
                break;
            }
        }
    }
    if (engine->job_count < LOGANA_MAX_JOBS) {
        engine->jobs[engine->job_count++] = job;
    }
    pthread_mutex_unlock(&engine->jobs_lock);
}

logana_job_t *logana_engine_find_job(logana_engine_t *engine, uint64_t job_id) {
    pthread_mutex_lock(&engine->jobs_lock);
    for (size_t i = 0; i < engine->job_count; ++i) {
        if (engine->jobs[i] && engine->jobs[i]->job_id == job_id) {
            pthread_mutex_unlock(&engine->jobs_lock);
            return engine->jobs[i];
        }
    }
    pthread_mutex_unlock(&engine->jobs_lock);
    return NULL;
}

int logana_engine_init(logana_engine_t *engine, const logana_config_t *config) {
    memset(engine, 0, sizeof(*engine));
    engine->config = *config;
    ttak_logger_init(&engine->logger, logana_log_sink, TTAK_LOG_INFO);
    pthread_mutex_init(&engine->jobs_lock, NULL);
    if (logana_queue_init(&engine->ingress_queue, 2048) != 0) return -1;
    if (logana_queue_init(&engine->render_queue, 2048) != 0) return -1;
    uint64_t now = logana_now_ms();
    engine->analysis_pool = ttak_thread_pool_create(engine->config.worker_threads, 0, now);
    engine->render_pool = ttak_thread_pool_create(engine->config.async_render_threads, 0, now);
    if (!engine->analysis_pool || !engine->render_pool) return -1;
    if (pthread_create(&engine->aggregator_thread, NULL, logana_aggregator_main, engine) != 0) return -1;
    if (pthread_create(&engine->render_dispatcher_thread, NULL, logana_render_dispatcher_main, engine) != 0) return -1;
    engine->next_job_id = 1;
    ttak_logger_log(&engine->logger, TTAK_LOG_INFO, "log analytics engine initialized with %zu analysis workers and %zu render workers",
                    engine->config.worker_threads, engine->config.async_render_threads);
    return 0;
}

logana_job_t *logana_engine_submit(logana_engine_t *engine, const char *payload, size_t payload_size, logana_algorithm_t algorithm) {
    logana_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    pthread_mutex_init(&job->lock, NULL);
    job->payload = malloc(payload_size + 1);
    if (!job->payload) {
        free(job);
        return NULL;
    }
    memcpy(job->payload, payload, payload_size);
    job->payload[payload_size] = '\0';
    job->job_id = __atomic_fetch_add(&engine->next_job_id, 1, __ATOMIC_RELAXED);
    job->payload_size = payload_size;
    job->algorithm = algorithm;
    job->created_ms = logana_now_ms();
    job->updated_ms = job->created_ms;
    job->status = LOGANA_JOB_QUEUED;
    logana_register_job(engine, job);
    if (!logana_queue_push(&engine->ingress_queue, job, 100)) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "ingress queue is saturated");
    }
    return job;
}

void logana_job_destroy(logana_job_t *job) {
    if (!job) return;
    pthread_mutex_destroy(&job->lock);
    free(job->payload);
    free(job->matrix.values);
    free(job->matrix.timestamps);
    free(job->matrix.valid_mask);
    free(job->matrix.formats);
    free(job->matrix.labels);
    free(job->matrix.categories);
    free(job->svg);
    free(job->html);
    free(job);
}

void logana_engine_shutdown(logana_engine_t *engine) {
    engine->shutting_down = true;
    logana_queue_close(&engine->ingress_queue);
    logana_queue_close(&engine->render_queue);
    pthread_join(engine->aggregator_thread, NULL);
    pthread_join(engine->render_dispatcher_thread, NULL);
    if (engine->analysis_pool) ttak_thread_pool_destroy(engine->analysis_pool);
    if (engine->render_pool) ttak_thread_pool_destroy(engine->render_pool);
    for (size_t i = 0; i < engine->job_count; ++i) logana_job_destroy(engine->jobs[i]);
    logana_queue_destroy(&engine->ingress_queue);
    logana_queue_destroy(&engine->render_queue);
    pthread_mutex_destroy(&engine->jobs_lock);
}
