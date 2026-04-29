#include "logana/render.h"
#include "logana/logana.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ttak/async/task.h>

static void append_json_escaped(char *dst, size_t dst_len, const char *src) {
    size_t out = strlen(dst);
    for (size_t i = 0; src[i] && out + 2 < dst_len; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = c;
        } else if (c == '\n') {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if ((unsigned char)c >= 32) {
            dst[out++] = c;
        }
    }
    dst[out] = '\0';
}

static char *logana_build_svg(logana_engine_t *engine, logana_job_t *job) {
    size_t cap = 16384;
    char *svg = calloc(1, cap);
    if (!svg) return NULL;
    int w = engine->config.canvas_width;
    int h = engine->config.canvas_height;
    snprintf(svg, cap,
             "<svg viewBox='0 0 %d %d' xmlns='http://www.w3.org/2000/svg'>"
             "<defs><linearGradient id='bg' x1='0' y1='0' x2='1' y2='1'>"
             "<stop offset='0%%' stop-color='#07111a'/><stop offset='100%%' stop-color='#020508'/></linearGradient></defs>"
             "<rect width='%d' height='%d' fill='url(#bg)' rx='28'/>"
             "<g opacity='0.14'>",
             w, h, w, h);
    for (int x = 80; x < w; x += 80) {
        char line[128];
        snprintf(line, sizeof(line), "<line x1='%d' y1='48' x2='%d' y2='%d' stroke='#86a7b9'/>", x, x, h - 48);
        strncat(svg, line, cap - strlen(svg) - 1);
    }
    strncat(svg, "</g><g>", cap - strlen(svg) - 1);
    size_t rows = job->matrix.row_count;
    size_t dims = job->matrix.dimensions;
    double max_y = job->summary.max[0];
    double min_y = job->summary.min[0];
    double span_y = (max_y - min_y) > 0.0001 ? (max_y - min_y) : 1.0;
    for (size_t i = 0; i < rows; ++i) {
        double x = 56.0 + ((double)i / (double)(rows > 1 ? rows - 1 : 1)) * (double)(w - 112);
        double y = (double)(h - 64) - (((double)job->matrix.values[i * dims] - min_y) / span_y) * (double)(h - 120);
        const char *color = engine->config.color_palette[job->matrix.labels ? (size_t)abs(job->matrix.labels[i]) % engine->config.color_count : 0];
        char point[192];
        snprintf(point, sizeof(point), "<circle cx='%.2f' cy='%.2f' r='4.4' fill='%s' opacity='0.9'/>", x, y, color);
        strncat(svg, point, cap - strlen(svg) - 1);
    }
    char footer[1024];
    snprintf(footer, sizeof(footer),
             "</g><g fill='#d6edf8'>"
             "<text x='56' y='36' font-size='24' font-family='IBM Plex Sans, sans-serif'>Clustered Stream Preview</text>"
             "<text x='56' y='%d' font-size='16' fill='#8fb5c8'>rows=%zu dims=%zu algorithm=%s clusters=%zu</text>"
             "</g></svg>",
             h - 20, job->summary.row_count, job->summary.dimensions, logana_algorithm_name(job->algorithm), job->summary.cluster_count);
    strncat(svg, footer, cap - strlen(svg) - 1);
    return svg;
}

int logana_render_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&job->lock);
    job->status = LOGANA_JOB_RENDERING;
    pthread_mutex_unlock(&job->lock);
    job->svg = logana_build_svg(engine, job);
    if (!job->svg) {
        pthread_mutex_lock(&job->lock);
        job->status = LOGANA_JOB_FAILED;
        snprintf(job->error, sizeof(job->error), "%s", "failed to build svg");
        pthread_mutex_unlock(&job->lock);
        return -1;
    }
    size_t html_cap = strlen(job->svg) + 2048;
    job->html = calloc(1, html_cap);
    if (!job->html) return -1;
    snprintf(job->html, html_cap,
             "<section class='report'>"
             "<div class='report-grid'>"
             "<article><span>Rows</span><strong>%zu</strong></article>"
             "<article><span>Clusters</span><strong>%zu</strong></article>"
             "<article><span>Entropy</span><strong>%.3f</strong></article>"
             "<article><span>Trend</span><strong>%.3f</strong></article>"
             "</div>%s</section>",
             job->summary.row_count, job->summary.cluster_count, job->summary.entropy, job->summary.slope, job->svg);
    pthread_mutex_lock(&job->lock);
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
        if (!ctx) continue;
        ctx->engine = engine;
        ctx->job = job;
        ttak_task_t *task = ttak_task_create(logana_render_job_exec, ctx, NULL, now);
        if (!task) {
            free(ctx);
            continue;
        }
        ttak_task_set_domain(task, TTAK_TASK_DOMAIN_IO);
        ttak_task_set_hash(task, logana_hash64(&job->job_id, sizeof(job->job_id)));
        ttak_task_set_urgency(task, 55);
        if (!ttak_thread_pool_schedule_task(engine->render_pool, task, 2, now)) {
            ttak_task_destroy(task, now);
            free(ctx);
        }
    }
    return NULL;
}

char *logana_job_status_json(logana_job_t *job) {
    char *json = calloc(1, 512);
    if (!json) return NULL;
    pthread_mutex_lock(&job->lock);
    snprintf(json, 512,
             "{\"jobId\":\"%llu\",\"status\":\"%s\",\"updatedMs\":%llu,\"error\":\"%s\"}",
             (unsigned long long)job->job_id, logana_status_name(job->status),
             (unsigned long long)job->updated_ms, job->error);
    pthread_mutex_unlock(&job->lock);
    return json;
}

char *logana_job_result_json(logana_job_t *job) {
    size_t cap = (job->html ? strlen(job->html) : 0) + (job->svg ? strlen(job->svg) : 0) + 2048;
    char *json = calloc(1, cap);
    char html[32768] = {0};
    char svg[24576] = {0};
    pthread_mutex_lock(&job->lock);
    if (job->html) append_json_escaped(html, sizeof(html), job->html);
    if (job->svg) append_json_escaped(svg, sizeof(svg), job->svg);
    snprintf(json, cap,
             "{\"jobId\":\"%llu\",\"status\":\"%s\",\"algorithm\":\"%s\",\"rows\":%zu,\"clusters\":%zu,"
             "\"entropy\":%.6f,\"trendSlope\":%.6f,\"outlierRatio\":%.6f,\"html\":\"%s\",\"svg\":\"%s\",\"error\":\"%s\"}",
             (unsigned long long)job->job_id, logana_status_name(job->status), logana_algorithm_name(job->algorithm),
             job->summary.row_count, job->summary.cluster_count, job->summary.entropy, job->summary.slope,
             job->summary.outlier_ratio, html, svg, job->error);
    pthread_mutex_unlock(&job->lock);
    return json;
}
