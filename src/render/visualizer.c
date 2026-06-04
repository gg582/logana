#include "logana/render.h"
#include "logana/logana.h"

#include <cjson/cJSON.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/core/template/template.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ttak/async/task.h>

#define LOGANA_REPORT_FRAGMENT_TEMPLATE "./templates/job_report_fragment.html.tmpl"
#define LOGANA_REPORT_PAGE_TEMPLATE "./templates/job_report_page.html.tmpl"

/**
 * @brief Symmetric log (symlog) transform guard.
 *        Prevents mathematical domain errors when logarithmic scaling is
 *        applied to axes that may contain zero or negative values.
 */
static double logana_symlog(double x, double linthresh) {
    if (x >= linthresh) return log10(x);
    if (x <= -linthresh) return -log10(-x);
    return x / linthresh;
}

typedef struct {
    uint64_t job_id;
    logana_job_status_t status;
    uint64_t updated_ms;
    logana_algorithm_t algorithm;
    size_t row_count;
    size_t dimensions;
    size_t cluster_count;
    double entropy;
    double slope;
    double outlier_ratio;
    double cluster_balance;
    double schema_drift;
    char *svg;   /* owned copy — must call logana_job_snapshot_free */
    char *html;  /* owned copy — must call logana_job_snapshot_free */
    const char *error; /* points into job->error[], never freed */
} logana_job_snapshot_t;

static void logana_job_snapshot(logana_job_t *job, logana_job_snapshot_t *snapshot) {
    pthread_mutex_lock(&job->lock);
    snapshot->job_id = job->job_id;
    snapshot->status = job->status;
    snapshot->updated_ms = job->updated_ms;
    snapshot->algorithm = (job->result.algorithm != LOGANA_ALGO_AUTO) ? job->result.algorithm : job->algorithm;
    snapshot->row_count = job->summary.row_count;
    snapshot->dimensions = job->summary.dimensions;
    snapshot->cluster_count = job->summary.cluster_count;
    snapshot->entropy = job->summary.entropy;
    snapshot->slope = job->summary.slope;
    snapshot->outlier_ratio = job->summary.outlier_ratio;
    snapshot->cluster_balance = job->summary.cluster_balance;
    snapshot->schema_drift = job->summary.schema_drift;
    /* strdup under lock so the pointers remain valid after unlock */
    snapshot->svg  = job->svg  ? strdup(job->svg)  : NULL;
    snapshot->html = job->html ? strdup(job->html) : NULL;
    snapshot->error = job->error; /* static array — valid for life of job */
    pthread_mutex_unlock(&job->lock);
}

static void logana_job_snapshot_free(logana_job_snapshot_t *snapshot) {
    free(snapshot->svg);
    free(snapshot->html);
    snapshot->svg = NULL;
    snapshot->html = NULL;
}

static char *logana_cjson_to_string(cJSON *json) {
    char *rendered = cJSON_PrintUnformatted(json);
    if (!rendered) return NULL;
    char *copy = strdup(rendered);
    cJSON_free(rendered);
    return copy;
}

static const char *logana_trend_summary(double slope) {
    if (slope > 0.05) return "Rising";
    if (slope < -0.05) return "Falling";
    return "Stable";
}

static void logana_add_insight(cJSON *insights, const char *title, const char *detail, const char *tone) {
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "title", title);
    cJSON_AddStringToObject(item, "detail", detail);
    cJSON_AddStringToObject(item, "tone", tone);
    cJSON_AddItemToArray(insights, item);
}

static void logana_populate_insights(cJSON *insights, const logana_job_snapshot_t *snapshot, logana_job_t *job) {
    char detail[512];

    snprintf(detail, sizeof(detail), "%zu rows were compressed into %zu cluster bands with %zu numeric dimensions.",
             snapshot->row_count, snapshot->cluster_count, snapshot->dimensions);
    logana_add_insight(insights, "Segmentation", detail, snapshot->cluster_count > 1 ? "warm" : "calm");

    snprintf(detail, sizeof(detail), "Entropy landed at %.3f, which %s categorical spread in the payload.",
             snapshot->entropy, snapshot->entropy >= 1.25 ? "suggests a broad" : "suggests a narrow");
    logana_add_insight(insights, "Distribution", detail, snapshot->entropy >= 1.25 ? "hot" : "calm");

    snprintf(detail, sizeof(detail), "The dominant trend is %s with slope %.4f across the primary timeline.",
             logana_trend_summary(snapshot->slope), snapshot->slope);
    logana_add_insight(insights, "Trend", detail, fabs(snapshot->slope) >= 0.05 ? "warm" : "calm");

    snprintf(detail, sizeof(detail), "Outliers account for %.2f%% of the observed rows.",
             snapshot->outlier_ratio * 100.0);
    logana_add_insight(insights, "Anomaly pressure", detail, snapshot->outlier_ratio >= 0.08 ? "hot" : "calm");

    // Dynamic dimension insight
    if (snapshot->dimensions >= 2) {
        snprintf(detail, sizeof(detail),
                 "A 2D projection of dim 0 and dim 1 is rendered. "
                 "Range: [%.2f, %.2f] x [%.2f, %.2f].",
                 job->summary.min[0], job->summary.max[0],
                 job->summary.min[1], job->summary.max[1]);
        logana_add_insight(insights, "Projection", detail, "calm");
    }

    // Schema drift insight
    if (snapshot->schema_drift > 0.3) {
        snprintf(detail, sizeof(detail),
                 "Mixed schema detected (drift score %.2f). "
                 "JSON, KV, and plain-text formats are interleaved.",
                 snapshot->schema_drift);
        logana_add_insight(insights, "Schema consistency", detail, "hot");
    }

    // Outlier insight with context
    if (snapshot->outlier_ratio >= 0.15) {
        snprintf(detail, sizeof(detail),
                 "High outlier ratio (%.1f%%) indicates noisy or multi-modal data. "
                 "Consider tuning epsilon or switching to OPTICS/GMM.",
                 snapshot->outlier_ratio * 100.0);
        logana_add_insight(insights, "Noise signal", detail, "hot");
    }

    // Cluster balance / collapse guard
    if (snapshot->cluster_count > 1 && snapshot->row_count > 0) {
        double balance = snapshot->cluster_balance;
        snprintf(detail, sizeof(detail),
                 "Cluster balance is %.0f%%. %s",
                 balance * 100.0,
                 balance < 0.3 ? "Highly imbalanced — possible collapse detected." :
                 balance < 0.6 ? "Moderately imbalanced." : "Reasonably balanced.");
        logana_add_insight(insights, "Balance", detail, balance < 0.3 ? "warm" : "calm");
    }
}

static cJSON *logana_build_report_context(const logana_job_snapshot_t *snapshot, logana_job_t *job) {
    cJSON *context = cJSON_CreateObject();
    if (!context) return NULL;

    char job_id[32];
    char entropy[32];
    char slope[32];
    char outlier_ratio[32];

    snprintf(job_id, sizeof(job_id), "%llu", (unsigned long long)snapshot->job_id);
    snprintf(entropy, sizeof(entropy), "%.3f", snapshot->entropy);
    snprintf(slope, sizeof(slope), "%.4f", snapshot->slope);
    snprintf(outlier_ratio, sizeof(outlier_ratio), "%.2f%%", snapshot->outlier_ratio * 100.0);

    cJSON_AddStringToObject(context, "jobId", job_id);
    cJSON_AddStringToObject(context, "status", logana_status_name(snapshot->status));
    cJSON_AddStringToObject(context, "algorithm", logana_algorithm_name(snapshot->algorithm));
    cJSON_AddNumberToObject(context, "rows", (double)snapshot->row_count);
    cJSON_AddNumberToObject(context, "clusters", (double)snapshot->cluster_count);
    cJSON_AddNumberToObject(context, "dimensions", (double)snapshot->dimensions);
    cJSON_AddStringToObject(context, "entropy", entropy);
    cJSON_AddStringToObject(context, "trendSlope", slope);
    cJSON_AddStringToObject(context, "trendLabel", logana_trend_summary(snapshot->slope));
    cJSON_AddStringToObject(context, "outlierRatio", outlier_ratio);
    cJSON_AddStringToObject(context, "svg", snapshot->svg ? snapshot->svg : "");
    cJSON_AddStringToObject(context, "error", snapshot->error ? snapshot->error : "");

    cJSON *insights = cJSON_CreateArray();
    if (insights) {
        logana_populate_insights(insights, snapshot, job);
        cJSON_AddItemToObject(context, "insights", insights);
    }

    return context;
}

static char *logana_render_template(const char *template_path, cJSON *context) {
    cwist_sstring *rendered = cwist_template_render_file(template_path, context);
    if (!rendered) return NULL;
    char *copy = strdup(rendered->data ? rendered->data : "");
    cwist_sstring_destroy(rendered);
    return copy;
}

static void logana_svg_append(char *svg, const char *str, size_t cap) {
    size_t len = strlen(svg);
    size_t str_len = strlen(str);
    if (len + 1 >= cap) return;
    size_t rem = cap - len - 1;
    if (str_len < rem) {
        memcpy(svg + len, str, str_len + 1);
    } else {
        memcpy(svg + len, str, rem);
        svg[cap - 1] = '\0';
    }
}

static char *logana_build_svg(logana_engine_t *engine, logana_job_t *job) {
    size_t cap = 131072;
    char *svg = calloc(1, cap);
    if (!svg) return NULL;

    int w = engine->config.canvas_width;
    int h = engine->config.canvas_height;
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    if (dims == 0 || rows == 0) {
        snprintf(svg, cap,
                 "<svg viewBox='0 0 %d %d' xmlns='http://www.w3.org/2000/svg'>"
                 "<rect width='%d' height='%d' fill='#07111a' rx='28'/>"
                 "<text x='%d' y='%d' fill='#8fb5c8' font-size='20' text-anchor='middle'>No data</text>"
                 "</svg>",
                 w, h, w, h, w / 2, h / 2);
        return svg;
    }

    // Use first two dimensions for 2D projection
    size_t d0 = 0;
    size_t d1 = dims > 1 ? 1 : 0;

    double min_x = job->summary.min[d0];
    double max_x = job->summary.max[d0];
    double span_x = (max_x - min_x) > 0.0001 ? (max_x - min_x) : 1.0;

    double min_y = job->summary.min[d1];
    double max_y = job->summary.max[d1];
    double span_y = (max_y - min_y) > 0.0001 ? (max_y - min_y) : 1.0;

    size_t color_count = engine->config.color_count ? engine->config.color_count : 1;

    // Compute cluster counts for radius sizing
    size_t *cluster_counts = NULL;
    int max_label = 0;
    if (job->result.labels) {
        cluster_counts = calloc(job->result.cluster_count + 1, sizeof(size_t));
        for (size_t i = 0; i < rows; ++i) {
            int label = job->result.labels[i];
            if (label < 0) label = (int)job->result.cluster_count; // noise
            if (label > max_label) max_label = label;
            if (cluster_counts) cluster_counts[label]++;
        }
    }

    snprintf(svg, cap,
             "<svg viewBox='0 0 %d %d' xmlns='http://www.w3.org/2000/svg'>"
             "<defs><linearGradient id='bg' x1='0' y1='0' x2='1' y2='1'>"
             "<stop offset='0%%' stop-color='#07111a'/><stop offset='100%%' stop-color='#020508'/></linearGradient>"
             "<filter id='glow'><feGaussianBlur stdDeviation='2.5' result='coloredBlur'/><feMerge><feMergeNode in='coloredBlur'/><feMergeNode in='SourceGraphic'/></feMerge></filter>"
             "</defs>"
             "<rect width='%d' height='%d' fill='url(#bg)' rx='28'/>"
             "<g opacity='0.10'>",
             w, h, w, h);

    // Grid lines
    for (int x = 80; x < w; x += 80) {
        char line[128];
        snprintf(line, sizeof(line), "<line x1='%d' y1='48' x2='%d' y2='%d' stroke='#86a7b9'/>", x, x, h - 48);
        logana_svg_append(svg, line, cap);
    }
    for (int y = 60; y < h - 40; y += 60) {
        char line[128];
        snprintf(line, sizeof(line), "<line x1='56' y1='%d' x2='%d' y2='%d' stroke='#86a7b9'/>", y, w - 56, y);
        logana_svg_append(svg, line, cap);
    }
    logana_svg_append(svg, "</g>", cap);

    // Axis labels
    char axes[1024];
    snprintf(axes, sizeof(axes),
             "<g fill='#8fb5c8' font-size='13' font-family='IBM Plex Sans, sans-serif'>"
             "<text x='%d' y='%d' text-anchor='middle'>dim 0</text>"
             "<text x='%d' y='%d' text-anchor='middle' transform='rotate(-90 %d %d)'>dim 1</text>"
             "</g>",
             w / 2, h - 14, 18, h / 2, 18, h / 2);
    logana_svg_append(svg, axes, cap);

    // Plot area
    int plot_left = 72;
    int plot_right = w - 72;
    int plot_top = 56;
    int plot_bottom = h - 56;
    int plot_w = plot_right - plot_left;
    int plot_h = plot_bottom - plot_top;

    logana_svg_append(svg, "<g filter='url(#glow)'>", cap);
    for (size_t i = 0; i < rows; ++i) {
        if (job->matrix.valid_mask) {
            if (!job->matrix.valid_mask[i * dims + d0]) continue;
            if (!job->matrix.valid_mask[i * dims + d1]) continue;
        }
        double raw_x = job->matrix.values[i * dims + d0];
        double raw_y = job->matrix.values[i * dims + d1];
        double nx = (raw_x - min_x) / span_x;
        double ny = (raw_y - min_y) / span_y;
        if (!isfinite(nx) || !isfinite(ny)) continue;
        if (nx < 0.0) nx = 0.0;
        if (nx > 1.0) nx = 1.0;
        if (ny < 0.0) ny = 0.0;
        if (ny > 1.0) ny = 1.0;
        double x = plot_left + nx * plot_w;
        double y = plot_bottom - ny * plot_h;

        int label = job->result.labels ? job->result.labels[i] : 0;
        bool is_noise_point = job->result.is_noise ? job->result.is_noise[i] : (label < 0);
        if (label < 0) label = (int)job->result.cluster_count; // noise -> last color
        const char *color = engine->config.color_palette[(size_t)abs(label) % color_count];

        // Vary radius slightly by cluster density
        double radius = 4.2;
        if (cluster_counts && label >= 0 && label <= max_label && cluster_counts[label] > 0) {
            double density = (double)cluster_counts[label] / (double)rows;
            radius = 3.0 + (1.0 - density) * 3.5;
            if (radius < 2.5) radius = 2.5;
            if (radius > 7.0) radius = 7.0;
        }

        bool outlier = false;
        if (job->matrix.dimensions > 0) {
            double score = 0.0;
            size_t valid_d = 0;
            for (size_t d = 0; d < job->matrix.dimensions; ++d) {
                if (job->matrix.valid_mask && !job->matrix.valid_mask[i * job->matrix.dimensions + d]) continue;
                double sigma = job->summary.stddev[d] > 0.0001 ? job->summary.stddev[d] : 1.0;
                double z = (job->matrix.values[i * job->matrix.dimensions + d] - job->summary.mean[d]) / sigma;
                score += z * z;
                ++valid_d;
            }
            outlier = (valid_d > 0 && score > 9.0);
        }

        char point[384];
        snprintf(point, sizeof(point),
                 "<circle cx='%.2f' cy='%.2f' r='%.2f' fill='%s' opacity='0.88' data-row='%zu' data-cluster='%d' data-outlier='%s' data-noise='%s'/>",
                 x, y, radius, color, i, label, outlier ? "true" : "false", is_noise_point ? "true" : "false");
        logana_svg_append(svg, point, cap);
    }
    logana_svg_append(svg, "</g>", cap);

    // Zero baseline layer (dynamic)
    if (min_y < 0.0 && max_y > 0.0) {
        double zero_ny = (0.0 - min_y) / span_y;
        double zero_y = plot_bottom - zero_ny * plot_h;
        char baseline[512];
        snprintf(baseline, sizeof(baseline),
                 "<g>"
                 "<line x1='%d' y1='%.2f' x2='%d' y2='%.2f' stroke='#ff6b6b' stroke-width='1.5' stroke-dasharray='6,4' opacity='0.9'/>"
                 "<text x='%d' y='%.2f' fill='#ff6b6b' font-size='12' font-family='IBM Plex Sans, sans-serif' text-anchor='end' dominant-baseline='middle'>0</text>"
                 "</g>",
                 plot_left, zero_y, plot_right, zero_y,
                 plot_left - 6, zero_y);
        logana_svg_append(svg, baseline, cap);
    }

    // Legend for clusters
    if (job->result.labels && job->result.cluster_count > 0) {
        logana_svg_append(svg, "<g>", cap);
        int lx = plot_right - 140;
        int ly = plot_top + 10;
        for (size_t c = 0; c < job->result.cluster_count && c < color_count; ++c) {
            char legend[256];
            snprintf(legend, sizeof(legend),
                     "<circle cx='%d' cy='%d' r='5' fill='%s'/>"
                     "<text x='%d' y='%d' fill='#d6edf8' font-size='12' font-family='IBM Plex Sans, sans-serif'>cluster %zu</text>",
                     lx, ly + (int)c * 20, engine->config.color_palette[c % color_count],
                     lx + 14, ly + 4 + (int)c * 20, c);
            logana_svg_append(svg, legend, cap);
        }
        logana_svg_append(svg, "</g>", cap);
    }

    // Footer
    char footer[1024];
    snprintf(footer, sizeof(footer),
             "<g fill='#d6edf8'>"
             "<text x='56' y='36' font-size='24' font-family='IBM Plex Sans, sans-serif'>Clustered Stream Preview</text>"
             "<text x='56' y='%d' font-size='16' fill='#8fb5c8'>rows=%zu dims=%zu algorithm=%s clusters=%zu noise=%zu outliers=%.1f%%</text>"
             "</g></svg>",
             h - 20, job->summary.row_count, job->summary.dimensions,
             logana_algorithm_name(job->result.algorithm != LOGANA_ALGO_AUTO ? job->result.algorithm : job->algorithm), job->result.cluster_count,
             job->result.noise_count,
             job->summary.outlier_ratio * 100.0);
    logana_svg_append(svg, footer, cap);

    free(cluster_counts);
    return svg;
}

static char *logana_render_job_fragment(logana_job_t *job) {
    logana_job_snapshot_t snapshot;
    logana_job_snapshot(job, &snapshot);

    cJSON *context = logana_build_report_context(&snapshot, job);
    logana_job_snapshot_free(&snapshot);
    if (!context) return NULL;
    char *html = logana_render_template(LOGANA_REPORT_FRAGMENT_TEMPLATE, context);
    cJSON_Delete(context);
    return html;
}

int logana_render_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&job->lock);
    job->status = LOGANA_JOB_RENDERING;
    pthread_mutex_unlock(&job->lock);

    char *svg = logana_build_svg(engine, job);
    if (!svg) {
        pthread_mutex_lock(&job->lock);
        job->status = LOGANA_JOB_FAILED;
        snprintf(job->error, sizeof(job->error), "%s", "failed to build svg");
        pthread_mutex_unlock(&job->lock);
        return -1;
    }

    pthread_mutex_lock(&job->lock);
    free(job->svg);
    job->svg = svg;
    job->updated_ms = logana_now_ms();
    pthread_mutex_unlock(&job->lock);

    char *html = logana_render_job_fragment(job);
    if (!html) {
        pthread_mutex_lock(&job->lock);
        job->status = LOGANA_JOB_FAILED;
        snprintf(job->error, sizeof(job->error), "%s", "failed to build report html");
        pthread_mutex_unlock(&job->lock);
        return -1;
    }

    pthread_mutex_lock(&job->lock);
    free(job->html);
    job->html = html;
    job->status = LOGANA_JOB_READY;
    job->updated_ms = logana_now_ms();
    pthread_mutex_unlock(&job->lock);
    return 0;
}

static void *logana_render_job_exec(void *arg) {
    struct {
        logana_engine_t *engine;
        logana_job_t *job;
    } *ctx = arg;
    logana_render_job(ctx->engine, ctx->job);
    logana_job_unref(ctx->job);
    free(ctx);
    return NULL;
}

void *logana_render_dispatcher_main(void *arg) {
    logana_engine_t *engine = (logana_engine_t *)arg;
    while (!engine->shutting_down) {
        logana_job_t *job = NULL;
        if (!logana_queue_pop(&engine->render_queue, (void **)&job, 100)) continue;
        uint64_t now = logana_now_ms();
        struct {
            logana_engine_t *engine;
            logana_job_t *job;
        } *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            logana_job_unref(job);
            continue;
        }
        ctx->engine = engine;
        ctx->job = job;
        ttak_task_t *task = ttak_task_create(logana_render_job_exec, ctx, NULL, now);
        if (!task) {
            free(ctx);
            logana_job_unref(job);
            continue;
        }
        ttak_task_set_domain(task, TTAK_TASK_DOMAIN_IO);
        ttak_task_set_hash(task, logana_hash64(&job->job_id, sizeof(job->job_id)));
        ttak_task_set_urgency(task, 55);
        if (!ttak_thread_pool_schedule_task(engine->render_pool, task, 2, now)) {
            ttak_task_destroy(task, now);
            free(ctx);
            logana_job_unref(job);
        }
    }
    return NULL;
}

char *logana_job_status_json(logana_job_t *job) {
    logana_job_snapshot_t snapshot;
    logana_job_snapshot(job, &snapshot);

    cJSON *json = cJSON_CreateObject();
    if (!json) { logana_job_snapshot_free(&snapshot); return NULL; }
    char job_id[32];
    snprintf(job_id, sizeof(job_id), "%llu", (unsigned long long)snapshot.job_id);
    cJSON_AddStringToObject(json, "jobId", job_id);
    cJSON_AddStringToObject(json, "status", logana_status_name(snapshot.status));
    cJSON_AddNumberToObject(json, "updatedMs", (double)snapshot.updated_ms);
    cJSON_AddStringToObject(json, "error", snapshot.error ? snapshot.error : "");

    char *rendered = logana_cjson_to_string(json);
    cJSON_Delete(json);
    logana_job_snapshot_free(&snapshot);
    return rendered;
}

char *logana_job_result_json(logana_job_t *job) {
    logana_job_snapshot_t snapshot;
    logana_job_snapshot(job, &snapshot);

    cJSON *json = cJSON_CreateObject();
    if (!json) { logana_job_snapshot_free(&snapshot); return NULL; }
    char job_id[32];
    snprintf(job_id, sizeof(job_id), "%llu", (unsigned long long)snapshot.job_id);
    cJSON_AddStringToObject(json, "jobId", job_id);
    cJSON_AddStringToObject(json, "status", logana_status_name(snapshot.status));
    cJSON_AddStringToObject(json, "algorithm", logana_algorithm_name(snapshot.algorithm));
    cJSON_AddNumberToObject(json, "rows", (double)snapshot.row_count);
    cJSON_AddNumberToObject(json, "clusters", (double)job->result.cluster_count);
    cJSON_AddNumberToObject(json, "entropy", snapshot.entropy);
    cJSON_AddNumberToObject(json, "trendSlope", snapshot.slope);
    cJSON_AddNumberToObject(json, "outlierRatio", snapshot.outlier_ratio);
    cJSON_AddNumberToObject(json, "schemaDrift", snapshot.schema_drift);
    cJSON_AddStringToObject(json, "html", snapshot.html ? snapshot.html : "");
    cJSON_AddStringToObject(json, "svg", snapshot.svg ? snapshot.svg : "");
    cJSON_AddStringToObject(json, "error", snapshot.error ? snapshot.error : "");

    cJSON *points = cJSON_CreateArray();
    if (points && job->matrix.values && job->result.labels) {
        size_t d0 = 0;
        size_t d1 = job->matrix.dimensions > 1 ? 1 : 0;
        for (size_t i = 0; i < job->matrix.row_count; ++i) {
            cJSON *pt = cJSON_CreateObject();
            if (!pt) continue;
            double xv = 0.0, yv = 0.0;
            bool valid = true;
            if (job->matrix.valid_mask) {
                if (!job->matrix.valid_mask[i * job->matrix.dimensions + d0]) valid = false;
                if (!job->matrix.valid_mask[i * job->matrix.dimensions + d1]) valid = false;
            }
            if (valid) {
                xv = job->matrix.values[i * job->matrix.dimensions + d0];
                yv = job->matrix.values[i * job->matrix.dimensions + d1];
            }
            cJSON_AddNumberToObject(pt, "x", xv);
            cJSON_AddNumberToObject(pt, "y", yv);
            cJSON_AddNumberToObject(pt, "label", job->result.labels[i]);
            bool is_noise_point = job->result.is_noise ? job->result.is_noise[i] : (job->result.labels[i] < 0);
            cJSON_AddBoolToObject(pt, "isNoise", is_noise_point);
            bool outlier = false;
            if (valid && job->matrix.dimensions > 0) {
                double score = 0.0;
                size_t valid_d = 0;
                for (size_t d = 0; d < job->matrix.dimensions; ++d) {
                    if (job->matrix.valid_mask && !job->matrix.valid_mask[i * job->matrix.dimensions + d]) continue;
                    double sigma = job->summary.stddev[d] > 0.0001 ? job->summary.stddev[d] : 1.0;
                    double z = (job->matrix.values[i * job->matrix.dimensions + d] - job->summary.mean[d]) / sigma;
                    score += z * z;
                    ++valid_d;
                }
                outlier = (valid_d > 0 && score > 9.0);
            }
            cJSON_AddBoolToObject(pt, "outlier", outlier);
            cJSON_AddItemToArray(points, pt);
        }
        cJSON_AddItemToObject(json, "points", points);
    }

    char *rendered = logana_cjson_to_string(json);
    cJSON_Delete(json);
    logana_job_snapshot_free(&snapshot);
    return rendered;
}

char *logana_job_report_page(logana_job_t *job) {
    logana_job_snapshot_t snapshot;
    logana_job_snapshot(job, &snapshot);

    cJSON *context = cJSON_CreateObject();
    if (!context) { logana_job_snapshot_free(&snapshot); return NULL; }
    char job_id[32];
    snprintf(job_id, sizeof(job_id), "%llu", (unsigned long long)snapshot.job_id);
    cJSON_AddStringToObject(context, "jobId", job_id);
    cJSON_AddStringToObject(context, "status", logana_status_name(snapshot.status));
    cJSON_AddStringToObject(context, "body", snapshot.html ? snapshot.html : "");

    char *page = logana_render_template(LOGANA_REPORT_PAGE_TEMPLATE, context);
    cJSON_Delete(context);
    logana_job_snapshot_free(&snapshot);
    return page;
}
