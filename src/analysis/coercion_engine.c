#include "logana/coercion.h"
#include "logana/math.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* -------------------------------------------------------------------------- */
/* Rule 1: Polymorphic scalar coercion helpers                                */
/* -------------------------------------------------------------------------- */

static bool is_boolean_true(const char *s) {
    return (strcmp(s, "true") == 0 || strcmp(s, "TRUE") == 0 ||
            strcmp(s, "True") == 0 || strcmp(s, "yes") == 0 ||
            strcmp(s, "YES") == 0 || strcmp(s, "Yes") == 0 ||
            strcmp(s, "1") == 0);
}

static bool is_boolean_false(const char *s) {
    return (strcmp(s, "false") == 0 || strcmp(s, "FALSE") == 0 ||
            strcmp(s, "False") == 0 || strcmp(s, "no") == 0 ||
            strcmp(s, "NO") == 0 || strcmp(s, "No") == 0 ||
            strcmp(s, "0") == 0);
}

static bool is_categorical_key(const char *key) {
    if (!key || !*key) return false;
    static const char *guards[] = {
        "level", "lvl", "severity", "priority", "log_level",
        "service", "svc", "source", "app", "module", "component",
        "host", "node", "pod", "container",
        "msg", "message", "text", "event", "detail", "description",
        "error", "reason", "log",
        NULL
    };
    for (size_t i = 0; guards[i]; ++i) {
        if (strcasecmp(key, guards[i]) == 0) return true;
    }
    return false;
}

static bool is_numeric_trap_token(const char *s) {
    if (!s || !*s) return false;
    size_t i = 0;
    while (s[i] && (isspace((unsigned char)s[i]) || s[i] == '"' || s[i] == '\'')) ++i;
    const char *p = s + i;
    if (strcmp(p, "nan") == 0 || strcmp(p, "NaN") == 0 ||
        strcmp(p, "inf") == 0 || strcmp(p, "INF") == 0 ||
        strcmp(p, "infinity") == 0 || strcmp(p, "Infinity") == 0 ||
        strcmp(p, "null") == 0 || strcmp(p, "undefined") == 0 ||
        strcmp(p, "-999999") == 0) {
        return true;
    }
    return false;
}

/* Aggressive continuous-digit filtering.
 * Scans for the first embedded numeric sequence (supports decimal, leading +/-).
 * Strips common unit suffixes.
 */
static bool extract_leading_number(const char *raw, double *out) {
    if (!raw || !*raw) return false;
    const char *p = raw;
    while (*p && (isspace((unsigned char)*p) || *p == '"' || *p == '\'')) ++p;
    if (!*p) return false;

    /* Try direct strtod first */
    char *end = NULL;
    double val = strtod(p, &end);
    if (end != p && isfinite(val)) {
        /* Validate trailing chars are benign */
        bool ok = true;
        for (const char *t = end; *t; ++t) {
            if (!isspace((unsigned char)*t) && *t != '"' && *t != '\'' &&
                *t != ',' && *t != ';' && *t != '|' && *t != '}' && *t != ']' &&
                *t != '%' && *t != '_') {
                ok = false;
                break;
            }
        }
        if (ok) {
            *out = val;
            return true;
        }
    }

    /* Embedded digit extraction: scan for a digit or sign+digit pattern */
    for (const char *s = p; *s; ++s) {
        if (isdigit((unsigned char)*s) || ((*s == '-' || *s == '+') && isdigit((unsigned char)s[1]))) {
            char *e2 = NULL;
            double v2 = strtod(s, &e2);
            if (e2 != s && isfinite(v2)) {
                *out = v2;
                return true;
            }
        }
    }
    return false;
}

/* Strip common unit suffixes in-place on a copied buffer */
static void strip_unit_suffixes(char *buf) {
    size_t len = strlen(buf);
    if (len == 0) return;
    static const char *suffs[] = {
        "mbps","gbps","kbps","tbps","bps",
        "mhz","ghz","khz","thz","hz",
        "tps","qps","rpm",
        "mib","gib","kib","tib",
        "mb","gb","kb","tb","pb",
        "ms","us","ns","ps","fs",
        "sec","min","hr","hrs","day","days",
        "px","em","rem","vw","vh","dpi",
        "x","th","st","nd","rd",
        "%","b","s",
    };
    for (size_t i = 0; i < sizeof(suffs)/sizeof(suffs[0]); ++i) {
        size_t sl = strlen(suffs[i]);
        if (len >= sl && strncasecmp(buf + len - sl, suffs[i], sl) == 0) {
            buf[len - sl] = '\0';
            return;
        }
    }
}

double logana_coerce_scalar(const char *raw, bool *out_is_numeric,
                            char *out_categorical, size_t cat_cap) {
    if (!raw) {
        *out_is_numeric = false;
        if (out_categorical && cat_cap) out_categorical[0] = '\0';
        return 0.0;
    }

    /* Trim outer quotes and whitespace into a scratch buffer */
    char scratch[LOGANA_COERCION_MAX_STRING_LEN];
    size_t rp = 0;
    while (raw[rp] && (isspace((unsigned char)raw[rp]) || raw[rp] == '"' || raw[rp] == '\'')) ++rp;
    size_t rlen = strlen(raw + rp);
    while (rlen > 0 && (isspace((unsigned char)(raw + rp)[rlen - 1]) ||
                        (raw + rp)[rlen - 1] == '"' || (raw + rp)[rlen - 1] == '\'')) --rlen;
    if (rlen >= sizeof(scratch)) rlen = sizeof(scratch) - 1;
    memcpy(scratch, raw + rp, rlen);
    scratch[rlen] = '\0';

    if (rlen == 0) {
        *out_is_numeric = false;
        if (out_categorical && cat_cap) out_categorical[0] = '\0';
        return 0.0;
    }

    /* Boolean mapping */
    if (is_boolean_true(scratch)) {
        *out_is_numeric = true;
        if (out_categorical && cat_cap) out_categorical[0] = '\0';
        return 1.0;
    }
    if (is_boolean_false(scratch)) {
        *out_is_numeric = true;
        if (out_categorical && cat_cap) out_categorical[0] = '\0';
        return 0.0;
    }

    /* Numeric trap detection (NaN, Infinity, -999999 placeholders) */
    if (is_numeric_trap_token(scratch)) {
        *out_is_numeric = false;
        if (out_categorical && cat_cap) {
            size_t cl = strlen(scratch);
            if (cl >= cat_cap) cl = cat_cap - 1;
            memcpy(out_categorical, scratch, cl);
            out_categorical[cl] = '\0';
        }
        return 0.0;
    }

    /* Try stripping suffixes and parsing */
    char suff_stripped[LOGANA_COERCION_MAX_STRING_LEN];
    memcpy(suff_stripped, scratch, rlen + 1);
    strip_unit_suffixes(suff_stripped);

    double val = 0.0;
    if (extract_leading_number(suff_stripped, &val)) {
        *out_is_numeric = true;
        if (out_categorical && cat_cap) out_categorical[0] = '\0';
        return val;
    }

    /* Fallback: return as categorical */
    *out_is_numeric = false;
    if (out_categorical && cat_cap) {
        size_t cl = strlen(scratch);
        if (cl >= cat_cap) cl = cat_cap - 1;
        memcpy(out_categorical, scratch, cl);
        out_categorical[cl] = '\0';
    }
    return 0.0;
}

/* -------------------------------------------------------------------------- */
/* Rule 4: Nested array/object flattening                                     */
/* -------------------------------------------------------------------------- */

/* Iteratively unwrap cJSON nesting until a primitive is found.
 * Returns a borrowed reference (do NOT delete).
 * If the nest is empty or entirely unresolvable, returns NULL.
 */
static cJSON *flatten_cjson_item(cJSON *item) {
    if (!item) return NULL;
    cJSON *cur = item;
    int depth = 0;
    while (depth < 64) {
        if (cJSON_IsArray(cur)) {
            if (cJSON_GetArraySize(cur) == 0) return NULL;
            cur = cJSON_GetArrayItem(cur, 0);
            if (!cur) return NULL;
        } else if (cJSON_IsObject(cur)) {
            cJSON *child = cur->child;
            if (!child) return NULL;
            cur = child;
        } else {
            return cur;
        }
        ++depth;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Histogram helpers                                                          */
/* -------------------------------------------------------------------------- */

static void histogram_ensure_capacity(logana_coercion_histogram_t *hist, size_t need) {
    if (need <= hist->capacity) return;
    size_t new_cap = hist->capacity ? hist->capacity * 2 : 16;
    while (new_cap < need) new_cap *= 2;
    logana_coercion_hist_entry_t *n = realloc(hist->entries, new_cap * sizeof(*n));
    if (!n) return;
    hist->entries = n;
    hist->capacity = new_cap;
}

static void histogram_increment(logana_coercion_histogram_t *hist,
                                const char *key, const char *value) {
    (void)key;
    for (size_t i = 0; i < hist->count; ++i) {
        if (strcmp(hist->entries[i].value, value) == 0) {
            hist->entries[i].count++;
            return;
        }
    }
    histogram_ensure_capacity(hist, hist->count + 1);
    if (hist->count >= hist->capacity) return;
    logana_coercion_hist_entry_t *e = &hist->entries[hist->count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    snprintf(e->value, sizeof(e->value), "%s", value);
    e->count = 1;
}

/* -------------------------------------------------------------------------- */
/* Context lifecycle                                                          */
/* -------------------------------------------------------------------------- */

int logana_coercion_init(logana_coercion_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->row_capacity = LOGANA_COERCION_INITIAL_ROW_CAP;
    size_t cells = ctx->row_capacity * LOGANA_COERCION_MAX_TRACKED_KEYS;
    ctx->values = calloc(cells, sizeof(double));
    ctx->valid_mask = calloc(cells, sizeof(uint8_t));
    ctx->outlier_pressure = calloc(cells, sizeof(double));
    ctx->timestamps = calloc(ctx->row_capacity, sizeof(uint64_t));
    ctx->categories = calloc(ctx->row_capacity, sizeof(uint64_t));
    ctx->formats = calloc(ctx->row_capacity, sizeof(uint8_t));
    if (!ctx->values || !ctx->valid_mask || !ctx->outlier_pressure ||
        !ctx->timestamps || !ctx->categories || !ctx->formats) {
        logana_coercion_destroy(ctx);
        return -1;
    }
    return 0;
}

void logana_coercion_destroy(logana_coercion_context_t *ctx) {
    if (!ctx) return;
    free(ctx->values);
    free(ctx->valid_mask);
    free(ctx->outlier_pressure);
    free(ctx->timestamps);
    free(ctx->categories);
    free(ctx->formats);
    for (size_t i = 0; i < ctx->histogram_count; ++i) {
        free(ctx->histograms[i].entries);
    }
    memset(ctx, 0, sizeof(*ctx));
}

static bool grow_context_buffers(logana_coercion_context_t *ctx, size_t new_cap) {
    size_t old_cap = ctx->row_capacity;
    size_t old_cells = old_cap * LOGANA_COERCION_MAX_TRACKED_KEYS;
    size_t new_cells = new_cap * LOGANA_COERCION_MAX_TRACKED_KEYS;
    double  *nv = realloc(ctx->values, new_cells * sizeof(double));
    uint8_t *nm = realloc(ctx->valid_mask, new_cells * sizeof(uint8_t));
    double  *no = realloc(ctx->outlier_pressure, new_cells * sizeof(double));
    uint64_t *nts = realloc(ctx->timestamps, new_cap * sizeof(uint64_t));
    uint64_t *nc = realloc(ctx->categories, new_cap * sizeof(uint64_t));
    uint8_t  *nf = realloc(ctx->formats, new_cap * sizeof(uint8_t));
    if (!nv || !nm || !no || !nts || !nc || !nf) {
        free(nv); free(nm); free(no); free(nts); free(nc); free(nf);
        return false;
    }
    ctx->values = nv;
    ctx->valid_mask = nm;
    ctx->outlier_pressure = no;
    ctx->timestamps = nts;
    ctx->categories = nc;
    ctx->formats = nf;
    memset(ctx->valid_mask + old_cells, 0, (new_cells - old_cells) * sizeof(uint8_t));
    memset(ctx->outlier_pressure + old_cells, 0, (new_cells - old_cells) * sizeof(double));
    memset(ctx->timestamps + old_cap, 0, (new_cap - old_cap) * sizeof(uint64_t));
    memset(ctx->categories + old_cap, 0, (new_cap - old_cap) * sizeof(uint64_t));
    memset(ctx->formats + old_cap, 0, (new_cap - old_cap) * sizeof(uint8_t));
    ctx->row_capacity = new_cap;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Key registry (Rule 5 gatekeeping)                                          */
/* -------------------------------------------------------------------------- */

static int find_or_register_key(logana_coercion_context_t *ctx,
                                logana_engine_t *engine,
                                const char *key) {
    if (!key || !*key) return -1;
    /* Search existing */
    for (size_t i = 0; i < ctx->key_count; ++i) {
        if (strcmp(ctx->keys[i], key) == 0) return (int)i;
    }
    /* Check configured keys for canonical ordering priority */
    for (size_t i = 0; i < ctx->key_count; ++i) {
        if (engine && strcasecmp(ctx->keys[i], key) == 0) {
            if (strcmp(ctx->keys[i], key) != 0) {
                /* case variant: keep original registration */
            }
            return (int)i;
        }
    }
    /* Hard limit: 32 tracking matrices */
    if (ctx->key_count >= LOGANA_COERCION_MAX_TRACKED_KEYS) {
        return -1;
    }
    size_t idx = ctx->key_count++;
    snprintf(ctx->keys[idx], sizeof(ctx->keys[idx]), "%s", key);
    memset(&ctx->meta[idx], 0, sizeof(ctx->meta[idx]));
    ctx->meta[idx].is_active = true;
    return (int)idx;
}

static void append_overflow(logana_coercion_context_t *ctx,
                            const char *key, const char *raw_value) {
    if (!key || !raw_value) return;
    char pair[512];
    int n = snprintf(pair, sizeof(pair), "%s=%s;", key, raw_value);
    if (n <= 0 || (size_t)n >= sizeof(pair)) return;
    size_t need = ctx->overflow_len + (size_t)n;
    if (need >= LOGANA_COERCION_MAX_OVERFLOW_LEN) {
        /* Overflow of overflow: truncate with marker */
        if (ctx->overflow_len + 3 < LOGANA_COERCION_MAX_OVERFLOW_LEN) {
            memcpy(ctx->overflow + ctx->overflow_len, "...", 3);
            ctx->overflow_len += 3;
            ctx->overflow[ctx->overflow_len] = '\0';
        }
        return;
    }
    memcpy(ctx->overflow + ctx->overflow_len, pair, (size_t)n);
    ctx->overflow_len += (size_t)n;
    ctx->overflow[ctx->overflow_len] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Timestamp helpers (reused from math.c internals)                           */
/* -------------------------------------------------------------------------- */

extern uint64_t logana_hash64(const void *data, size_t len);

/* Copied from math.c because the original is file-static */
static int coercion_normalize_timestamp(const char *str, uint64_t *out_ms) {
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
/* Row processing                                                             */
/* -------------------------------------------------------------------------- */

static void reset_row_scratch(logana_coercion_context_t *ctx) {
    for (size_t i = 0; i < LOGANA_COERCION_MAX_TRACKED_KEYS; ++i) {
        ctx->row_values[i] = 0.0;
        ctx->row_valid[i] = 0;
    }
}

static void process_scalar_value(logana_coercion_context_t *ctx,
                                 logana_engine_t *engine,
                                 const char *key,
                                 const char *raw_value,
                                 size_t row) {
    (void)engine; (void)row;
    int idx = find_or_register_key(ctx, engine, key);
    if (idx < 0) {
        /* Rule 5: overflow gate */
        append_overflow(ctx, key, raw_value);
        return;
    }

    /* Known categorical keys must never be coerced to numeric */
    if (is_categorical_key(key)) {
        ctx->meta[idx].categorical_hits++;
        if (raw_value && raw_value[0]) {
            size_t hidx = (size_t)idx;
            if (hidx >= ctx->histogram_count) ctx->histogram_count = hidx + 1;
            histogram_increment(&ctx->histograms[hidx], key, raw_value);
        }
        return;
    }

    bool is_num = false;
    char categorical[LOGANA_COERCION_MAX_STRING_LEN];
    double val = logana_coerce_scalar(raw_value, &is_num, categorical, sizeof(categorical));

    if (is_num) {
        ctx->row_values[idx] = val;
        ctx->row_valid[idx] = 1;
        ctx->meta[idx].numeric_hits++;
    } else {
        /* Rule 2: categorical routing */
        ctx->meta[idx].categorical_hits++;
        if (categorical[0]) {
            /* Ensure histogram exists for this key */
            size_t hidx = (size_t)idx;
            if (hidx >= ctx->histogram_count) ctx->histogram_count = hidx + 1;
            histogram_increment(&ctx->histograms[hidx], key, categorical);
        }
        /* Numeric slot stays invalid for this row */
    }
}

static void process_cjson_item(logana_coercion_context_t *ctx,
                               logana_engine_t *engine,
                               const char *key,
                               cJSON *item,
                               size_t row) {
    (void)row;
    /* Rule 4: flatten nesting */
    cJSON *flat = flatten_cjson_item(item);
    if (!flat) {
        /* Empty nest: substitute 0 / preserve last known valid */
        int idx = find_or_register_key(ctx, engine, key);
        if (idx >= 0) {
            ctx->row_values[idx] = 0.0;
            ctx->row_valid[idx] = 1; /* zero is a valid substitute */
            ctx->meta[idx].numeric_hits++;
        }
        return;
    }

    if (cJSON_IsNumber(flat)) {
        double v = flat->valuedouble;
        if (!isfinite(v)) {
            /* Treat non-finite as categorical trap */
            int idx = find_or_register_key(ctx, engine, key);
            if (idx >= 0) {
                ctx->meta[idx].categorical_hits++;
                histogram_increment(&ctx->histograms[idx], key, "nonfinite");
            }
            return;
        }
        /* Known categorical keys must stay categorical even if JSON holds a number */
        if (is_categorical_key(key)) {
            int idx = find_or_register_key(ctx, engine, key);
            if (idx >= 0) {
                ctx->meta[idx].categorical_hits++;
                char buf[64];
                snprintf(buf, sizeof(buf), "%.6g", v);
                histogram_increment(&ctx->histograms[idx], key, buf);
            }
            return;
        }
        int idx = find_or_register_key(ctx, engine, key);
        if (idx >= 0) {
            ctx->row_values[idx] = v;
            ctx->row_valid[idx] = 1;
            ctx->meta[idx].numeric_hits++;
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.6g", v);
            append_overflow(ctx, key, buf);
        }
    } else if (cJSON_IsString(flat)) {
        process_scalar_value(ctx, engine, key, flat->valuestring, ctx->row_count);
    } else if (cJSON_IsBool(flat)) {
        process_scalar_value(ctx, engine, key, cJSON_IsTrue(flat) ? "true" : "false", ctx->row_count);
    } else if (cJSON_IsNull(flat)) {
        /* Null is neither numeric nor categorical; just leave invalid */
    } else {
        /* Should not happen after flattening, but guard anyway */
        int idx = find_or_register_key(ctx, engine, key);
        if (idx >= 0) {
            ctx->meta[idx].categorical_hits++;
            histogram_increment(&ctx->histograms[idx], key, "unresolved");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Text/KV line discovery (fallback for non-JSON)                             */
/* -------------------------------------------------------------------------- */

static void discover_text_pairs(const char *line,
                                char keys[][64],
                                char values[][LOGANA_COERCION_MAX_STRING_LEN],
                                size_t *count,
                                size_t max_count) {
    const char *p = line;
    *count = 0;
    while (*p && *count < max_count) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == '|')) ++p;
        if (!*p) break;

        const char *key_start = NULL;
        const char *key_end = NULL;
        if (*p == '"') {
            key_start = ++p;
            while (*p && *p != '"') ++p;
            key_end = p;
            if (*p == '"') ++p;
        } else {
            key_start = p;
            while (*p && *p != '=' && *p != ':' && !isspace((unsigned char)*p)) ++p;
            key_end = p;
        }

        while (*p && (*p == '=' || *p == ':' || isspace((unsigned char)*p))) ++p;
        if (!*p) break;

        const char *val_start = p;
        size_t vlen = 0;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            val_start = p;
            while (*p && *p != quote) ++p;
            vlen = (size_t)(p - val_start);
            if (*p == quote) ++p;
        } else {
            while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';' && *p != '|') ++p;
            vlen = (size_t)(p - val_start);
        }

        size_t klen = (size_t)(key_end - key_start);
        if (klen > 63) klen = 63;
        if (klen == 0) continue;
        memcpy(keys[*count], key_start, klen);
        keys[*count][klen] = '\0';

        if (vlen >= LOGANA_COERCION_MAX_STRING_LEN) vlen = LOGANA_COERCION_MAX_STRING_LEN - 1;
        memcpy(values[*count], val_start, vlen);
        values[*count][vlen] = '\0';
        (*count)++;
    }
}

static void process_text_line(logana_coercion_context_t *ctx,
                              logana_engine_t *engine,
                              const char *line) {
    char tkeys[LOGANA_COERCION_MAX_TRACKED_KEYS][64];
    char tvals[LOGANA_COERCION_MAX_TRACKED_KEYS][LOGANA_COERCION_MAX_STRING_LEN];
    size_t tcount = 0;
    discover_text_pairs(line, tkeys, tvals, &tcount, LOGANA_COERCION_MAX_TRACKED_KEYS);
    for (size_t i = 0; i < tcount; ++i) {
        process_scalar_value(ctx, engine, tkeys[i], tvals[i], ctx->row_count);
    }
}

/* -------------------------------------------------------------------------- */
/* Timestamp extraction (reuses existing logic via engine config)             */
/* -------------------------------------------------------------------------- */

static uint64_t extract_row_timestamp(logana_engine_t *engine, const char *line) {
    uint64_t ts_ms = 0;
    bool ts_found = false;
    for (size_t t = 0; t < engine->config.timestamp_key_count; ++t) {
        const char *tk = engine->config.timestamp_keys[t];
        const char *found = engine->config.case_sensitive ? strstr(line, tk) : NULL;
        if (!found && !engine->config.case_sensitive) {
            /* simple CI search */
            size_t tlen = strlen(tk);
            for (const char *p = line; *p; ++p) {
                if (strncasecmp(p, tk, tlen) == 0) { found = p; break; }
            }
        }
        if (found) {
            const char *colon = strchr(found, ':');
            if (colon && coercion_normalize_timestamp(colon + 1, &ts_ms)) {
                ts_found = true;
                break;
            }
        }
    }
    if (!ts_found) ts_ms = 0;
    return ts_ms;
}

/* -------------------------------------------------------------------------- */
/* Category hash for row (concatenate configured category keys)               */
/* -------------------------------------------------------------------------- */

static uint64_t extract_row_category(logana_engine_t *engine, const char *line) {
    char cat_buf[256] = {0};
    size_t cat_off = 0;
    for (size_t c = 0; c < engine->config.category_key_count; ++c) {
        const char *ck = engine->config.category_keys[c];
        const char *found = engine->config.case_sensitive ? strstr(line, ck) : NULL;
        if (!found && !engine->config.case_sensitive) {
            size_t clen = strlen(ck);
            for (const char *p = line; *p; ++p) {
                if (strncasecmp(p, ck, clen) == 0) { found = p; break; }
            }
        }
        if (found) {
            const char *colon = strchr(found, ':');
            if (colon) {
                const char *start = colon + 1;
                while (*start && isspace((unsigned char)*start)) ++start;
                const char *end = start;
                if (*start == '"' || *start == '\'') {
                    char quote = *start++;
                    end = start;
                    while (*end && *end != quote) ++end;
                } else {
                    while (*end && !isspace((unsigned char)*end) && *end != ',' && *end != ';' && *end != '|') ++end;
                }
                size_t vlen = (size_t)(end - start);
                if (vlen && cat_off + vlen + 1 < sizeof(cat_buf)) {
                    if (cat_off > 0) cat_buf[cat_off++] = '|';
                    memcpy(cat_buf + cat_off, start, vlen);
                    cat_off += vlen;
                }
            }
        }
    }
    return cat_off > 0 ? logana_hash64(cat_buf, cat_off) : 0;
}

/* -------------------------------------------------------------------------- */
/* Main payload parser                                                        */
/* -------------------------------------------------------------------------- */

int logana_coercion_parse_payload(logana_engine_t *engine, logana_job_t *job,
                                  logana_coercion_context_t *ctx) {
    if (!engine || !job || !ctx || !job->payload) return -1;

    char *scratch = malloc(job->payload_size + 1);
    if (!scratch) return -1;
    memcpy(scratch, job->payload, job->payload_size);
    scratch[job->payload_size] = '\0';

    char *cursor = scratch;
    while (cursor && *cursor && ctx->row_count < engine->config.max_rows_per_analysis) {
        while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;

        char *next = NULL;
        char saved = '\0';
        const char *parse_end = NULL;
        cJSON *probe = NULL;

        if (*cursor == '{' || *cursor == '[') {
            probe = cJSON_ParseWithOpts(cursor, &parse_end, false);
        }

        if (probe && parse_end && parse_end > cursor) {
            next = (char *)parse_end;
            saved = *next;
            *next = '\0';
            cJSON_Delete(probe);
        } else {
            if (probe) cJSON_Delete(probe);
            next = strchr(cursor, '\n');
            if (next) {
                saved = *next;
                *next = '\0';
            }
        }

        if (*cursor) {
            if (ctx->row_count == ctx->row_capacity) {
                size_t new_cap = ctx->row_capacity * 2;
                if (new_cap <= ctx->row_capacity || !grow_context_buffers(ctx, new_cap)) {
                    break;
                }
            }

            /* Format detection */
            const char *p = cursor;
            while (*p && isspace((unsigned char)*p)) ++p;
            if (*p == '{' || *p == '[') ctx->formats[ctx->row_count] = 0;
            else if (strchr(cursor, '=') != NULL || (strchr(cursor, ':') != NULL && strstr(cursor, ", ") != NULL))
                ctx->formats[ctx->row_count] = 1;
            else ctx->formats[ctx->row_count] = 2;

            reset_row_scratch(ctx);
            ctx->timestamps[ctx->row_count] = extract_row_timestamp(engine, cursor);
            ctx->categories[ctx->row_count] = extract_row_category(engine, cursor);

            if (*p == '{' || *p == '[') {
                cJSON *root = cJSON_Parse(cursor);
                if (root) {
                    if (cJSON_IsObject(root)) {
                        cJSON *child = NULL;
                        cJSON_ArrayForEach(child, root) {
                            if (child->string) {
                                process_cjson_item(ctx, engine, child->string, child, ctx->row_count);
                            }
                        }
                    } else if (cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
                        /* Top-level array: treat indices as keys? No, flatten first element */
                        cJSON *first = cJSON_GetArrayItem(root, 0);
                        if (first && cJSON_IsObject(first)) {
                            cJSON *child = NULL;
                            cJSON_ArrayForEach(child, first) {
                                if (child->string) {
                                    process_cjson_item(ctx, engine, child->string, child, ctx->row_count);
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                } else {
                    /* Invalid JSON fallback to text */
                    process_text_line(ctx, engine, cursor);
                }
            } else {
                process_text_line(ctx, engine, cursor);
            }

            /* Flush row scratch into dense buffers */
            size_t base = ctx->row_count * LOGANA_COERCION_MAX_TRACKED_KEYS;
            for (size_t d = 0; d < LOGANA_COERCION_MAX_TRACKED_KEYS; ++d) {
                ctx->values[base + d] = ctx->row_values[d];
                ctx->valid_mask[base + d] = ctx->row_valid[d];
            }
            ++ctx->row_count;
            job->processed_lines_count = ctx->row_count;
        }

        if (!next) break;
        *next = saved;
        cursor = next;
    }

    free(scratch);
    ctx->dims = ctx->key_count;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Rule 3: Winsorization (IQR / MAD boundary boxing)                          */
/* -------------------------------------------------------------------------- */

static int compare_double_ptr(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void compute_column_stats(logana_coercion_context_t *ctx, size_t col) {
    logana_coercion_key_meta_t *m = &ctx->meta[col];
    size_t n = 0;
    for (size_t r = 0; r < ctx->row_count; ++r) {
        if (ctx->valid_mask[r * LOGANA_COERCION_MAX_TRACKED_KEYS + col]) ++n;
    }
    if (n == 0) {
        m->is_active = false;
        return;
    }

    double *tmp = malloc(n * sizeof(double));
    if (!tmp) { m->is_active = false; return; }
    size_t idx = 0;
    for (size_t r = 0; r < ctx->row_count; ++r) {
        if (ctx->valid_mask[r * LOGANA_COERCION_MAX_TRACKED_KEYS + col]) {
            tmp[idx++] = ctx->values[r * LOGANA_COERCION_MAX_TRACKED_KEYS + col];
        }
    }
    qsort(tmp, n, sizeof(double), compare_double_ptr);

    double median = tmp[n / 2];
    if ((n % 2) == 0 && n > 1) median = (tmp[n / 2 - 1] + tmp[n / 2]) * 0.5;
    m->median = median;

    /* IQR */
    size_t q1_idx = n / 4;
    size_t q3_idx = (3 * n) / 4;
    double q1 = tmp[q1_idx];
    double q3 = tmp[q3_idx];
    m->iqr = q3 - q1;

    /* MAD */
    for (size_t i = 0; i < n; ++i) tmp[i] = fabs(tmp[i] - median);
    qsort(tmp, n, sizeof(double), compare_double_ptr);
    double mad = tmp[n / 2];
    if ((n % 2) == 0 && n > 1) mad = (tmp[n / 2 - 1] + tmp[n / 2]) * 0.5;
    m->mad = mad;

    double mad_sigma = mad > 0.0001 ? mad * 1.4826 : 0.0001;
    double iqr_span = m->iqr > 0.0001 ? m->iqr : mad_sigma;

    /* Use the wider bound of IQR-based or MAD-based to be conservative */
    double bound_iqr = 5.0 * iqr_span;
    double bound_mad = 5.0 * mad_sigma;
    double bound = bound_iqr > bound_mad ? bound_iqr : bound_mad;

    m->lower_bound = median - bound;
    m->upper_bound = median + bound;

    free(tmp);
}

static void apply_winsorization(logana_coercion_context_t *ctx) {
    for (size_t c = 0; c < ctx->key_count; ++c) {
        compute_column_stats(ctx, c);
        if (!ctx->meta[c].is_active) continue;

        /* Rule 2: if zero numeric hits, deactivate column */
        if (ctx->meta[c].numeric_hits == 0) {
            ctx->meta[c].is_active = false;
            continue;
        }

        double lb = ctx->meta[c].lower_bound;
        double ub = ctx->meta[c].upper_bound;
        for (size_t r = 0; r < ctx->row_count; ++r) {
            size_t idx = r * LOGANA_COERCION_MAX_TRACKED_KEYS + c;
            if (!ctx->valid_mask[idx]) continue;
            double v = ctx->values[idx];
            if (v < lb || v > ub) {
                /* Preserve true magnitude in outlier_pressure */
                ctx->outlier_pressure[idx] = v;
                /* Clip to boundary for global stats */
                if (v < lb) ctx->values[idx] = lb;
                else ctx->values[idx] = ub;
            } else {
                ctx->outlier_pressure[idx] = 0.0;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Export to official feature matrix                                          */
/* -------------------------------------------------------------------------- */

int logana_coercion_export_matrix(logana_coercion_context_t *ctx, logana_job_t *job,
                                  logana_engine_t *engine) {
    if (!ctx || !job) return -1;
    if (ctx->row_count == 0) return 0;

    /* Apply statistical boundary boxing before export */
    apply_winsorization(ctx);

    /* Count active dimensions */
    size_t active_dims = 0;
    size_t col_map[LOGANA_COERCION_MAX_TRACKED_KEYS];
    for (size_t c = 0; c < ctx->key_count; ++c) {
        if (ctx->meta[c].is_active && ctx->meta[c].numeric_hits > 0) {
            col_map[c] = active_dims++;
        } else {
            col_map[c] = (size_t)-1;
        }
    }

    if (active_dims == 0) {
        /* Fallback: synthesize a single dimension */
        active_dims = 1;
        if (ctx->key_count > 0) {
            for (size_t c = 0; c < ctx->key_count; ++c) {
                if (ctx->meta[c].categorical_hits > 0) { col_map[c] = 0; break; }
            }
            /* If still none, just use first slot */
            if (col_map[0] == (size_t)-1) col_map[0] = 0;
        }
    }

    size_t cells = ctx->row_count * active_dims;
    double  *values = malloc(cells * sizeof(double));
    uint8_t *valid_mask = malloc(cells * sizeof(uint8_t));
    double  *pressure = calloc(cells, sizeof(double));
    uint64_t *timestamps = malloc(ctx->row_count * sizeof(uint64_t));
    uint64_t *categories = malloc(ctx->row_count * sizeof(uint64_t));
    uint8_t  *formats = malloc(ctx->row_count * sizeof(uint8_t));
    if (!values || !valid_mask || !pressure || !timestamps || !categories || !formats) {
        free(values); free(valid_mask); free(pressure); free(timestamps); free(categories); free(formats);
        return -1;
    }

    for (size_t r = 0; r < ctx->row_count; ++r) {
        timestamps[r] = ctx->timestamps[r];
        categories[r] = ctx->categories[r];
        formats[r] = ctx->formats[r];
        for (size_t d = 0; d < active_dims; ++d) {
            values[r * active_dims + d] = 0.0;
            valid_mask[r * active_dims + d] = 0;
        }
    }

    for (size_t c = 0; c < ctx->key_count; ++c) {
        size_t nd = col_map[c];
        if (nd == (size_t)-1) continue;
        for (size_t r = 0; r < ctx->row_count; ++r) {
            size_t src_idx = r * LOGANA_COERCION_MAX_TRACKED_KEYS + c;
            size_t dst_idx = r * active_dims + nd;
            if (ctx->valid_mask[src_idx]) {
                values[dst_idx] = ctx->values[src_idx];
                valid_mask[dst_idx] = 1;
                pressure[dst_idx] = ctx->outlier_pressure[src_idx];
            } else if (active_dims == 1 && ctx->meta[c].categorical_hits > 0) {
                /* Synthetic fallback for purely categorical key in 1-dim mode */
                values[dst_idx] = 0.0;
                valid_mask[dst_idx] = 0;
            }
        }
    }

    /* If every row ended up invalid in the only dimension, synthesize
       from timestamp hash so downstream stages don't choke on an empty matrix. */
    if (active_dims == 1) {
        bool any_valid = false;
        for (size_t r = 0; r < ctx->row_count; ++r) {
            if (valid_mask[r]) { any_valid = true; break; }
        }
        if (!any_valid) {
            for (size_t r = 0; r < ctx->row_count; ++r) {
                values[r] = (double)(timestamps[r] % 1000000ULL) / 1000.0;
                valid_mask[r] = 1;
            }
        }
    }

    /* Transfer ownership to job */
    free(job->matrix.values);
    free(job->matrix.valid_mask);
    free(job->matrix.outlier_pressure);
    free(job->matrix.timestamps);
    free(job->matrix.categories);
    free(job->matrix.formats);

    job->matrix.values = values;
    job->matrix.valid_mask = valid_mask;
    job->matrix.outlier_pressure = pressure;
    job->matrix.timestamps = timestamps;
    job->matrix.categories = categories;
    job->matrix.formats = formats;
    job->matrix.row_count = ctx->row_count;
    job->matrix.dimensions = active_dims;
    job->matrix.labels = NULL;

    (void)engine;
    return 0;
}
