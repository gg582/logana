#include "logana/logana.h"
#include "logana/math.h"
#include "logana/pipeline.h"
#include "logana/cluster_strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logana_log_sink(ttak_log_level_t level, const char *msg) {
    const char *tag = "INFO";
    if (level == TTAK_LOG_DEBUG) tag = "DEBUG";
    else if (level == TTAK_LOG_WARN) tag = "WARN";
    else if (level == TTAK_LOG_ERROR) tag = "ERROR";
    fprintf(stderr, "[%s] %s\n", tag, msg);
}

static void logana_set_job_status(logana_job_t *job, logana_job_status_t status, const char *error) {
    if (error) {
        snprintf(job->error, sizeof(job->error), "%s", error);
    }
    atomic_store_explicit(&job->updated_ms, logana_now_ms(), memory_order_relaxed);
    atomic_store_explicit(&job->status, status, memory_order_release);
}

void logana_job_ref(logana_job_t *job) {
    if (job) __atomic_fetch_add(&job->ref_count, 1, __ATOMIC_RELAXED);
}

void logana_job_unref(logana_job_t *job) {
    if (!job) return;
    if (__atomic_fetch_sub(&job->ref_count, 1, __ATOMIC_ACQ_REL) == 1) {
        logana_job_destroy(job);
    }
}

/* -------------------------------------------------------------------------- */
/* Auto cache (lock-free approximate)                                         */
/* -------------------------------------------------------------------------- */

static uint64_t logana_compute_fingerprint(const logana_job_t *job, size_t rows, size_t active_dims) {
    size_t sample = job->payload_size > 1024 ? 1024 : job->payload_size;
    uint64_t h = logana_hash64(job->payload, sample);
    h ^= (uint64_t)rows * 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)active_dims * 0xbf58476d1ce4e5b9ULL;
    h ^= (uint64_t)job->matrix.dimensions * 0x94d049bb133111ebULL;
    return h;
}

static void logana_auto_cache_insert(logana_engine_t *engine, uint64_t fp,
                                     logana_algorithm_t selected, uint64_t now_ms) {
    size_t count = atomic_load_explicit(&engine->auto_cache_count, memory_order_relaxed);
    if (count < LOGANA_MAX_AUTO_CACHE) {
        size_t idx = atomic_fetch_add_explicit(&engine->auto_cache_count, 1, memory_order_acq_rel);
        if (idx < LOGANA_MAX_AUTO_CACHE) {
            engine->auto_cache[idx] = (logana_auto_cache_entry_t){fp, selected, now_ms};
            return;
        }
        atomic_fetch_sub_explicit(&engine->auto_cache_count, 1, memory_order_relaxed);
    }
    size_t evict = 0;
    uint64_t oldest = engine->auto_cache[0].last_used_ms;
    for (size_t i = 1; i < LOGANA_MAX_AUTO_CACHE; ++i) {
        if (engine->auto_cache[i].last_used_ms < oldest) {
            oldest = engine->auto_cache[i].last_used_ms;
            evict = i;
        }
    }
    engine->auto_cache[evict] = (logana_auto_cache_entry_t){fp, selected, now_ms};
}

/* -------------------------------------------------------------------------- */
/* SeqLock helpers                                                            */
/* -------------------------------------------------------------------------- */

static inline uint32_t seqlock_read_begin(_Atomic uint32_t *seq) {
    uint32_t s;
    do {
        s = atomic_load_explicit(seq, memory_order_acquire);
    } while (s & 1);
    return s;
}

static inline bool seqlock_read_retry(_Atomic uint32_t *seq, uint32_t start) {
    return atomic_load_explicit(seq, memory_order_acquire) != start;
}

static inline void seqlock_write_begin(_Atomic uint32_t *seq) {
    atomic_fetch_add_explicit(seq, 1, memory_order_release);
}

static inline void seqlock_write_end(_Atomic uint32_t *seq) {
    atomic_fetch_add_explicit(seq, 1, memory_order_release);
}

/* -------------------------------------------------------------------------- */
/* Job analysis via Pipeline                                                  */
/* -------------------------------------------------------------------------- */

int logana_analyze_job(logana_engine_t *engine, logana_job_t *job) {
    logana_set_job_status(job, LOGANA_JOB_ANALYZING, NULL);

    logana_pipeline_context_t ctx = {0};
    ctx.job = job;
    ctx.config = &engine->config;
    ctx.engine = engine;

    logana_pipeline_stage_t stages[] = {
        logana_preprocessing_stage(),
        logana_cleansing_stage(),
        logana_feature_stage(),
        logana_model_stage(),
        logana_eval_stage(),
    };

    int rc = logana_pipeline_execute(stages, 5, &ctx);
    if (rc != 0) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "pipeline execution failed");
        return -1;
    }

    /* Cache auto winner for fast replay */
    if (job->algorithm == LOGANA_ALGO_AUTO) {
        uint64_t fp = logana_compute_fingerprint(job, job->matrix.row_count, job->matrix.dimensions);
        logana_auto_cache_insert(engine, fp, job->result.algorithm, logana_now_ms());
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Engine lifecycle                                                           */
/* -------------------------------------------------------------------------- */

static bool logana_register_job(logana_engine_t *engine, logana_job_t *job) {
    logana_job_t *to_remove = NULL;

    seqlock_write_begin(&engine->jobs_seq);
    size_t count = atomic_load_explicit(&engine->job_count, memory_order_relaxed);
    if (count >= LOGANA_MAX_JOBS) {
        for (size_t i = 0; i < count; ++i) {
            logana_job_t *j = engine->jobs[i];
            if (!j) continue;
            logana_job_status_t st = atomic_load_explicit(&j->status, memory_order_acquire);
            bool done = (st == LOGANA_JOB_READY || st == LOGANA_JOB_FAILED);
            size_t refs = atomic_load_explicit(&j->ref_count, memory_order_acquire);
            if (done && refs == 1) {
                to_remove = j;
                memmove(&engine->jobs[i], &engine->jobs[i + 1],
                        (count - i - 1) * sizeof(logana_job_t *));
                atomic_fetch_sub_explicit(&engine->job_count, 1, memory_order_release);
                break;
            }
        }
        count = atomic_load_explicit(&engine->job_count, memory_order_relaxed);
    }
    bool ok = false;
    if (count < LOGANA_MAX_JOBS) {
        engine->jobs[count] = job;
        atomic_fetch_add_explicit(&engine->job_count, 1, memory_order_release);
        ok = true;
    }
    seqlock_write_end(&engine->jobs_seq);

    if (to_remove) {
        logana_job_unref(to_remove);
    }
    return ok;
}

logana_job_t *logana_engine_find_job(logana_engine_t *engine, uint64_t job_id) {
    while (true) {
        uint32_t seq = seqlock_read_begin(&engine->jobs_seq);
        size_t count = atomic_load_explicit(&engine->job_count, memory_order_acquire);
        logana_job_t *found = NULL;
        for (size_t i = 0; i < count; ++i) {
            if (engine->jobs[i] && engine->jobs[i]->job_id == job_id) {
                found = engine->jobs[i];
                logana_job_ref(found);
                break;
            }
        }
        if (!seqlock_read_retry(&engine->jobs_seq, seq)) {
            return found;
        }
        if (found) logana_job_unref(found);
    }
}

int logana_engine_init(logana_engine_t *engine, const logana_config_t *config) {
    memset(engine, 0, sizeof(*engine));
    engine->config = *config;
    ttak_logger_init(&engine->logger, logana_log_sink, TTAK_LOG_INFO);
    atomic_init(&engine->jobs_seq, 0);
    atomic_init(&engine->job_count, 0);
    if (logana_queue_init(&engine->ingress_queue, 2048) != 0) return -1;
    if (logana_queue_init(&engine->render_queue, 2048) != 0) return -1;
    uint64_t now = logana_now_ms();
    engine->analysis_pool = ttak_thread_pool_create(engine->config.worker_threads, 0, now);
    engine->render_pool = ttak_thread_pool_create(engine->config.async_render_threads, 0, now);
    if (!engine->analysis_pool || !engine->render_pool) return -1;
    if (pthread_create(&engine->aggregator_thread, NULL, logana_aggregator_main, engine) != 0) return -1;
    if (pthread_create(&engine->render_dispatcher_thread, NULL, logana_render_dispatcher_main, engine) != 0) return -1;
    atomic_init(&engine->next_job_id, 1);
    atomic_init(&engine->auto_cache_count, 0);
    ttak_logger_log(&engine->logger, TTAK_LOG_INFO, "log analytics engine initialized with %zu analysis workers and %zu render workers",
                    engine->config.worker_threads, engine->config.async_render_threads);
    return 0;
}

logana_job_t *logana_engine_submit(logana_engine_t *engine, const char *payload, size_t payload_size, logana_algorithm_t algorithm) {
    logana_job_t *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->payload = malloc(payload_size + 1);
    if (!job->payload) {
        free(job);
        return NULL;
    }
    memcpy(job->payload, payload, payload_size);
    job->payload[payload_size] = '\0';
    job->job_id = atomic_fetch_add_explicit(&engine->next_job_id, 1, memory_order_relaxed);
    job->payload_size = payload_size;
    job->algorithm = algorithm;
    uint64_t now = logana_now_ms();
    atomic_init(&job->ref_count, 1);
    atomic_init(&job->status, LOGANA_JOB_QUEUED);
    atomic_init(&job->created_ms, now);
    atomic_init(&job->updated_ms, now);
    atomic_init(&job->svg, NULL);
    atomic_init(&job->html, NULL);
    job->engine = engine;
    if (!logana_register_job(engine, job)) {
        logana_job_unref(job);
        return NULL;
    }
    if (!logana_queue_push(&engine->ingress_queue, job, 100)) {
        logana_set_job_status(job, LOGANA_JOB_FAILED, "ingress queue is saturated");
        logana_job_unref(job);
        return NULL;
    }
    return job;
}

void logana_job_destroy(logana_job_t *job) {
    if (!job) return;
    logana_engine_t *engine = job->engine;
    seqlock_write_begin(&engine->jobs_seq);
    size_t count = atomic_load_explicit(&engine->job_count, memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (engine->jobs[i] == job) {
            memmove(&engine->jobs[i], &engine->jobs[i + 1],
                    (count - i - 1) * sizeof(logana_job_t *));
            atomic_fetch_sub_explicit(&engine->job_count, 1, memory_order_release);
            break;
        }
    }
    seqlock_write_end(&engine->jobs_seq);
    free(job->payload);
    free(job->matrix.values);
    free(job->matrix.timestamps);
    free(job->matrix.valid_mask);
    free(job->matrix.formats);
    free(job->matrix.labels);
    free(job->matrix.categories);
    free(job->matrix.outlier_pressure);
    free(job->result.labels);
    free(job->result.is_noise);
    char *svg = atomic_exchange_explicit(&job->svg, NULL, memory_order_acq_rel);
    char *html = atomic_exchange_explicit(&job->html, NULL, memory_order_acq_rel);
    free(svg);
    free(html);
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
    while (atomic_load_explicit(&engine->job_count, memory_order_acquire) > 0) {
        logana_job_unref(engine->jobs[0]);
    }
    logana_queue_destroy(&engine->ingress_queue);
    logana_queue_destroy(&engine->render_queue);
}
