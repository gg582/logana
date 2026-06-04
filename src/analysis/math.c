#include "logana/math.h"
#include "logana/logana.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------- */
/* Hash & timing                                                              */
uint64_t logana_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
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

/* -------------------------------------------------------------------------- */
/* Numeric parsing guard                                                      */
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
    if (str[i] && !isspace((unsigned char)str[i]) && str[i] != '"' && str[i] != '\'' &&
        str[i] != ',' && str[i] != ';' && str[i] != '|' && str[i] != '}' && str[i] != ']')
        return false;
    return (strcmp(buf, "nan") == 0 || strcmp(buf, "inf") == 0 ||
            strcmp(buf, "infinity") == 0 || strcmp(buf, "null") == 0 ||
            strcmp(buf, "undefined") == 0);
}

static int logana_safe_strtod(const char *str, double *out, bool *out_valid, bool clamp_negative) {
    if (!str || !*str) return 0;
    if (logana_is_numeric_trap(str)) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1;
    }
    const char *num_start = str;
    while (*num_start && (isspace((unsigned char)*num_start) || *num_start == '"' || *num_start == '\'')) ++num_start;
    char *end = NULL;
    double value = strtod(num_start, &end);
    if (end == num_start) return 0;
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

static void logana_strip_numeric_suffixes(const char **start, const char **end) {
    while (*end > *start && isspace((unsigned char)(*end)[-1])) --*end;
    static const char *suffixes[] = {
        "mbps", "gbps", "kbps", "tbps", "bps",
        "mhz", "ghz", "khz", "thz", "hz",
        "tps", "qps", "rpm",
        "mib", "gib", "kib", "tib",
        "mb", "gb", "kb", "tb", "pb",
        "ms", "\u00b5s", "us", "ns", "ps", "fs",
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
                return;
            }
        }
    }
}

static int logana_parse_numeric_token(const char *start, const char *end, double *out, bool *out_valid) {
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)end[-1])) --end;
    while (start < end && (*start == '"' || *start == '\'' || *start == '{' || *start == '[' || *start == '(')) ++start;
    while (end > start && (end[-1] == '"' || end[-1] == '\'' || end[-1] == '}' || end[-1] == ']' || end[-1] == ')' || end[-1] == ',' || end[-1] == ';' || end[-1] == '|')) --end;
    if (end <= start) return 0;
    logana_strip_numeric_suffixes(&start, &end);
    if (end <= start) return 0;
    char token[128];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(token)) len = sizeof(token) - 1;
    memcpy(token, start, len);
    token[len] = '\0';
    if (!strncmp(token, "0x", 2) || !strncmp(token, "0X", 2)) {
        if (out_valid) *out_valid = false;
        *out = 0.0;
        return 1;
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

/* -------------------------------------------------------------------------- */
/* Timestamp normalization                                                    */
static int logana_normalize_timestamp(const char *str, uint64_t *out_ms) {
    if (!str || !*str) return 0;
    while (*str && (isspace((unsigned char)*str) || *str == '"' || *str == '\'' || *str == ':')) ++str;
    if (!*str) return 0;
    const char *token_end = str;
    while (*token_end && !isspace((unsigned char)*token_end) && *token_end != ',' && *token_end != '}' && *token_end != ']' && *token_end != '"') ++token_end;
    if (token_end <= str) return 0;
    size_t len = (size_t)(token_end - str);
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, str, len);
    buf[len] = '\0';
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
            if (ms >= 946684800000ULL && ms <= 2208988800000ULL) { *out_ms = ms; return 1; }
        } else {
            unsigned long long val = strtoull(buf, NULL, 10);
            uint64_t ms;
            if (val < 10000000000ULL) ms = val * 1000ULL;
            else if (val > 2000000000000ULL) ms = val / 1000ULL;
            else ms = val;
            if (ms >= 946684800000ULL && ms <= 2208988800000ULL) { *out_ms = ms; return 1; }
        }
        return 0;
    }
    struct tm tm = {0};
    const char *rest = strptime(buf, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!rest && len >= 10) rest = strptime(buf, "%Y-%m-%d", &tm);
    if (rest) {
        if (*rest == 'Z' || *rest == 'z') rest++;
        else if ((*rest == '+' || *rest == '-') && len >= (size_t)(rest - buf + 6)) rest += 6;
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

/* -------------------------------------------------------------------------- */
/* Distance functions                                                         */
double logana_distance_euclidean_sq(const double *a, const double *b,
                                    const uint8_t *mask_a, const uint8_t *mask_b,
                                    size_t dims, const logana_analysis_summary_t *summary) {
    (void)summary;
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        double d = a[i] - b[i];
        total += d * d;
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

double logana_distance_manhattan(const double *a, const double *b,
                                 const uint8_t *mask_a, const uint8_t *mask_b,
                                 size_t dims, const logana_analysis_summary_t *summary) {
    (void)summary;
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        total += fabs(a[i] - b[i]);
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

double logana_distance_zscore_sq(const double *a, const double *b,
                                 const uint8_t *mask_a, const uint8_t *mask_b,
                                 size_t dims, const logana_analysis_summary_t *summary) {
    double total = 0.0;
    size_t valid_count = 0;
    for (size_t i = 0; i < dims; ++i) {
        if (mask_a && !mask_a[i]) continue;
        if (mask_b && !mask_b[i]) continue;
        double sigma = summary && summary->stddev[i] > 0.0001 ? summary->stddev[i] : 1.0;
        double za = (a[i] - (summary ? summary->mean[i] : 0.0)) / sigma;
        double zb = (b[i] - (summary ? summary->mean[i] : 0.0)) / sigma;
        double d = za - zb;
        total += d * d;
        ++valid_count;
    }
    if (valid_count == 0) return HUGE_VAL;
    total *= (double)dims / (double)valid_count;
    return total;
}

/* -------------------------------------------------------------------------- */
/* Matrix parsing                                                             */
static bool logana_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out) return false;
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}

static void *logana_memdup_bytes(const void *src, size_t bytes) {
    void *dst = malloc(bytes);
    if (!dst) return NULL;
    memcpy(dst, src, bytes);
    return dst;
}

static bool logana_grow_matrix_buffers(double **values,
                                       uint64_t **timestamps,
                                       uint8_t **valid_mask,
                                       uint64_t **categories,
                                       uint8_t **formats,
                                       size_t old_cap,
                                       size_t new_cap,
                                       size_t dims) {
    size_t old_cells = 0;
    size_t new_cells = 0;
    if (!logana_checked_mul_size(old_cap, dims, &old_cells)) return false;
    if (!logana_checked_mul_size(new_cap, dims, &new_cells)) return false;

    double *new_values = malloc(new_cells * sizeof(double));
    uint64_t *new_timestamps = malloc(new_cap * sizeof(uint64_t));
    uint8_t *new_valid_mask = malloc(new_cells * sizeof(uint8_t));
    uint64_t *new_categories = calloc(new_cap, sizeof(uint64_t));
    uint8_t *new_formats = calloc(new_cap, sizeof(uint8_t));
    if (!new_values || !new_timestamps || !new_valid_mask || !new_categories || !new_formats) {
        free(new_values);
        free(new_timestamps);
        free(new_valid_mask);
        free(new_categories);
        free(new_formats);
        return false;
    }

    memcpy(new_values, *values, old_cells * sizeof(double));
    memcpy(new_timestamps, *timestamps, old_cap * sizeof(uint64_t));
    memcpy(new_valid_mask, *valid_mask, old_cells * sizeof(uint8_t));
    memcpy(new_categories, *categories, old_cap * sizeof(uint64_t));
    memcpy(new_formats, *formats, old_cap * sizeof(uint8_t));

    free(*values);
    free(*timestamps);
    free(*valid_mask);
    free(*categories);
    free(*formats);

    *values = new_values;
    *timestamps = new_timestamps;
    *valid_mask = new_valid_mask;
    *categories = new_categories;
    *formats = new_formats;
    return true;
}

size_t logana_parse_matrix(logana_engine_t *engine, logana_job_t *job) {
    size_t dims = engine->config.numeric_key_count;
    if (dims == 0) dims = LOGANA_MAX_DIMENSIONS;
    if (dims > LOGANA_MAX_DIMENSIONS) dims = LOGANA_MAX_DIMENSIONS;

    size_t row_capacity = 1024;
    size_t cell_capacity = 0;
    if (!logana_checked_mul_size(row_capacity, dims, &cell_capacity)) return 0;

    double *values = malloc(cell_capacity * sizeof(double));
    uint64_t *timestamps = malloc(row_capacity * sizeof(uint64_t));
    uint8_t *valid_mask = malloc(cell_capacity * sizeof(uint8_t));
    uint64_t *categories = calloc(row_capacity, sizeof(uint64_t));
    uint8_t *formats = calloc(row_capacity, sizeof(uint8_t));
    char *scratch = logana_memdup_bytes(job->payload, job->payload_size + 1);
    if (!values || !timestamps || !valid_mask || !categories || !formats || !scratch) {
        free(values); free(timestamps); free(valid_mask); free(categories); free(formats); free(scratch);
        return 0;
    }

    size_t rows = 0;
    char *cursor = scratch;
    while (cursor && *cursor && rows < engine->config.max_rows_per_analysis) {
        char *next = strchr(cursor, '\n');
        if (next) *next = '\0';
        if (*cursor) {
            if (rows == row_capacity) {
                size_t new_cap = row_capacity * 2;
                if (new_cap <= row_capacity ||
                    !logana_grow_matrix_buffers(&values, &timestamps, &valid_mask,
                                                &categories, &formats, row_capacity,
                                                new_cap, dims)) {
                    break;
                }
                row_capacity = new_cap;
            }
            {
                const char *p = cursor;
                while (*p && isspace((unsigned char)*p)) ++p;
                if (*p == '{' || *p == '[') formats[rows] = 0;
                else if (strchr(cursor, '=') != NULL || (strchr(cursor, ':') != NULL && strstr(cursor, ", ") != NULL)) formats[rows] = 1;
                else formats[rows] = 2;
            }
            memset(valid_mask + rows * dims, 1, dims * sizeof(uint8_t));
            double extracted[LOGANA_MAX_DIMENSIONS] = {0};
            bool extracted_valid[LOGANA_MAX_DIMENSIONS] = {false};
            size_t captured = 0;
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
            uint64_t ts_ms = 0;
            bool ts_found = false;
            for (size_t t = 0; t < engine->config.timestamp_key_count; ++t) {
                const char *found = engine->config.case_sensitive
                    ? strstr(cursor, engine->config.timestamp_keys[t])
                    : logana_find_key_ci(cursor, engine->config.timestamp_keys[t]);
                if (found) {
                    const char *colon = strchr(found, ':');
                    if (colon && logana_normalize_timestamp(colon + 1, &ts_ms)) { ts_found = true; break; }
                }
            }
            if (!ts_found) ts_ms = (uint64_t)rows * 1000ULL;
            timestamps[rows] = ts_ms;
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
            if (engine->config.numeric_key_count == 0 || configured_valid_found == 0) {
                if (configured_valid_found == 0) {
                    captured = 0;
                    memset(valid_mask + rows * dims, 1, dims * sizeof(uint8_t));
                }
                double freeform[LOGANA_MAX_DIMENSIONS] = {0};
                size_t freeform_count = logana_collect_freeform_numbers(cursor, freeform, dims - captured);
                for (size_t i = 0; i < freeform_count && captured < dims; ++i) {
                    extracted[captured] = freeform[i];
                    extracted_valid[captured] = true;
                    ++captured;
                }
            }
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
            if ((engine->config.numeric_key_count == 0 || configured_valid_found == 0) && captured == 0) {
                if (captured < dims) {
                    extracted[captured] = (double)(logana_hash64(cursor, strlen(cursor)) % 1000000ULL) / 1000.0;
                    extracted_valid[captured] = true;
                    ++captured;
                }
                if (captured < dims) {
                    extracted[captured] = (double)strlen(cursor);
                    extracted_valid[captured] = true;
                    ++captured;
                }
                if (captured < dims) {
                    extracted[captured] = (double)(logana_count_chars(cursor, '=') + logana_count_chars(cursor, ':'));
                    extracted_valid[captured] = true;
                    ++captured;
                }
            }
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

    free(scratch);
    job->matrix.values = values;
    job->matrix.timestamps = timestamps;
    job->matrix.valid_mask = valid_mask;
    job->matrix.categories = categories;
    job->matrix.formats = formats;
    job->matrix.row_count = rows;
    job->matrix.dimensions = dims;
    return rows;
}

/* -------------------------------------------------------------------------- */
/* Summary computation                                                        */
static int logana_compare_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void logana_compute_summary(logana_job_t *job) {
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
    if (has_real_ts && max_ts > min_ts && (max_ts - min_ts) > (uint64_t)(365LL * 86400 * 1000)) has_real_ts = false;
    double t_mean = 0.0;
    if (has_real_ts) {
        double t_sum = 0.0;
        for (size_t r = 0; r < rows; ++r) t_sum += ((double)job->matrix.timestamps[r] - (double)min_ts) / 1000.0;
        t_mean = t_sum / (double)rows;
    } else {
        t_mean = ((double)rows - 1.0) / 2.0;
    }

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
    double primary_range = (primary_max > primary_min && isfinite(primary_max - primary_min)) ? (primary_max - primary_min) : 1.0;

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
        double t = has_real_ts ? ((double)job->matrix.timestamps[r] - (double)min_ts) / 1000.0 : (double)r;
        double dt = t - t_mean;
        if (primary_valid) { slope_num += dt * primary; slope_den += dt * dt; }
        for (size_t d = 0; d < dims; ++d) {
            if (job->matrix.valid_mask && !job->matrix.valid_mask[r * dims + d]) continue;
            double value = job->matrix.values[r * dims + d];
            if (valid_counts[d] == 0) { job->summary.min[d] = value; job->summary.max[d] = value; }
            else { if (value < job->summary.min[d]) job->summary.min[d] = value; if (value > job->summary.max[d]) job->summary.max[d] = value; }
            job->summary.mean[d] += value;
            ++valid_counts[d];
        }
    }

    for (size_t d = 0; d < dims; ++d) {
        if (valid_counts[d] > 0) job->summary.mean[d] /= (double)valid_counts[d];
        else { job->summary.min[d] = 0.0; job->summary.max[d] = 0.0; job->summary.mean[d] = 0.0; }
    }

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

    if (job->matrix.formats && rows > 0) {
        size_t fmt_counts[3] = {0};
        for (size_t r = 0; r < rows; ++r) { uint8_t f = job->matrix.formats[r]; if (f < 3) fmt_counts[f]++; }
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
