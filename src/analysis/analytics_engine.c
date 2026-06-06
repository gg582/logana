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
    pthread_mutex_lock(&job->lock);
    job->status = status;
    job->updated_ms = logana_now_ms();
    if (error) snprintf(job->error, sizeof(job->error), "%s", error);
    pthread_mutex_unlock(&job->lock);
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
/* Auto cache (retained for compatibility but no longer drives selection)   */
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
    if (engine->auto_cache_count < LOGANA_MAX_AUTO_CACHE) {
        engine->auto_cache[engine->auto_cache_count++] =
            (logana_auto_cache_entry_t){fp, selected, now_ms};
        return;
    }
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

/* -------------------------------------------------------------------------- */
/* Job analysis via Pipeline                                                  */
/* -------------------------------------------------------------------------- */

int logana_analyze_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&engine->analysis_mutex);
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
        pthread_mutex_unlock(&engine->analysis_mutex);
        return -1;
    }

    /* Cache auto winner for fast replay */
    if (job->algorithm == LOGANA_ALGO_AUTO) {
        uint64_t fp = logana_compute_fingerprint(job, job->matrix.row_count, job->matrix.dimensions);
        logana_auto_cache_insert(engine, fp, job->result.algorithm, logana_now_ms());
    }

    pthread_mutex_unlock(&engine->analysis_mutex);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Engine lifecycle                                                           */
/* -------------------------------------------------------------------------- */

static bool logana_register_job(logana_engine_t *engine, logana_job_t *job) {
    pthread_mutex_lock(&engine->jobs_lock);
    logana_job_t *to_remove = NULL;
    if (engine->job_count >= LOGANA_MAX_JOBS) {
        for (size_t i = 0; i < engine->job_count; ++i) {
            logana_job_t *j = engine->jobs[i];
            if (!j) continue;
            pthread_mutex_lock(&j->lock);
            bool done = (j->status == LOGANA_JOB_READY || j->status == LOGANA_JOB_FAILED);
            pthread_mutex_unlock(&j->lock);
            if (done) {
                size_t refs = __atomic_load_n(&j->ref_count, __ATOMIC_ACQUIRE);
                if (refs == 1) {
                    to_remove = j;
                    memmove(&engine->jobs[i], &engine->jobs[i + 1],
                            (engine->job_count - i - 1) * sizeof(logana_job_t *));
                    engine->job_count--;
                    break;
                }
            }
        }
    }
    if (engine->job_count < LOGANA_MAX_JOBS) {
        engine->jobs[engine->job_count++] = job;
        pthread_mutex_unlock(&engine->jobs_lock);
        if (to_remove) {
            logana_job_unref(to_remove);
        }
        return true;
    }
    pthread_mutex_unlock(&engine->jobs_lock);
    if (to_remove) {
        logana_job_unref(to_remove);
    }
    return false;
}

logana_job_t *logana_engine_find_job(logana_engine_t *engine, uint64_t job_id) {
    pthread_mutex_lock(&engine->jobs_lock);
    for (size_t i = 0; i < engine->job_count; ++i) {
        if (engine->jobs[i] && engine->jobs[i]->job_id == job_id) {
            logana_job_t *job = engine->jobs[i];
            logana_job_ref(job);
            pthread_mutex_unlock(&engine->jobs_lock);
            return job;
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
    pthread_mutex_init(&engine->analysis_mutex, NULL);
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
    job->ref_count = 1;
    job->status = LOGANA_JOB_QUEUED;
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
    pthread_mutex_lock(&engine->jobs_lock);
    for (size_t i = 0; i < engine->job_count; ++i) {
        if (engine->jobs[i] == job) {
            memmove(&engine->jobs[i], &engine->jobs[i + 1],
                    (engine->job_count - i - 1) * sizeof(logana_job_t *));
            engine->job_count--;
            break;
        }
    }
    pthread_mutex_unlock(&engine->jobs_lock);
    pthread_mutex_destroy(&job->lock);
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
    while (engine->job_count > 0) {
        logana_job_unref(engine->jobs[0]);
    }
    logana_queue_destroy(&engine->ingress_queue);
    logana_queue_destroy(&engine->render_queue);
    pthread_mutex_destroy(&engine->jobs_lock);
    pthread_mutex_destroy(&engine->analysis_mutex);
}
