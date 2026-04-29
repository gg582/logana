#include "logana/logana.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ttak/async/task.h>

static void logana_timespec_after_ms(struct timespec *ts, uint32_t wait_ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += wait_ms / 1000U;
    ts->tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

int logana_queue_init(logana_queue_t *queue, size_t capacity) {
    memset(queue, 0, sizeof(*queue));
    queue->ring = ttak_ringbuf_create(capacity, sizeof(void *));
    if (!queue->ring) return -1;
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    return 0;
}

void logana_queue_close(logana_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

void logana_queue_destroy(logana_queue_t *queue) {
    if (queue->ring) ttak_ringbuf_destroy(queue->ring);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
}

bool logana_queue_push(logana_queue_t *queue, void *item, uint32_t wait_ms) {
    struct timespec deadline;
    logana_timespec_after_ms(&deadline, wait_ms);
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && ttak_ringbuf_is_full(queue->ring)) {
        if (pthread_cond_timedwait(&queue->cond, &queue->mutex, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return false;
        }
    }
    if (queue->closed) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    bool ok = ttak_ringbuf_push(queue->ring, &item);
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return ok;
}

bool logana_queue_pop(logana_queue_t *queue, void **item, uint32_t wait_ms) {
    struct timespec deadline;
    logana_timespec_after_ms(&deadline, wait_ms);
    pthread_mutex_lock(&queue->mutex);
    while (!queue->closed && ttak_ringbuf_is_empty(queue->ring)) {
        if (pthread_cond_timedwait(&queue->cond, &queue->mutex, &deadline) == ETIMEDOUT) {
            pthread_mutex_unlock(&queue->mutex);
            return false;
        }
    }
    if (ttak_ringbuf_is_empty(queue->ring)) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    bool ok = ttak_ringbuf_pop(queue->ring, item);
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return ok;
}

static int logana_schedule_batch(logana_engine_t *engine, logana_batch_t *batch) {
    uint64_t now = logana_now_ms();
    ttak_task_t *task = ttak_task_create(logana_analyze_batch_task, batch, NULL, now);
    if (!task) return -1;
    ttak_task_set_domain(task, TTAK_TASK_DOMAIN_THREAD);
    ttak_task_set_hash(task, logana_hash64(&batch->total_bytes, sizeof(batch->total_bytes)));
    ttak_task_set_urgency(task, 80);
    if (!ttak_thread_pool_schedule_task(engine->analysis_pool, task, 3, now)) {
        ttak_task_destroy(task, now);
        return -1;
    }
    return 0;
}

void *logana_analyze_batch_task(void *arg) {
    logana_batch_t *batch = (logana_batch_t *)arg;
    for (size_t i = 0; i < batch->job_count; ++i) {
        logana_job_t *job = batch->jobs[i];
        if (logana_analyze_job(batch->engine, job) == 0) {
            if (!logana_queue_push(&batch->engine->render_queue, job, 100)) {
                pthread_mutex_lock(&job->lock);
                job->status = LOGANA_JOB_FAILED;
                snprintf(job->error, sizeof(job->error), "%s", "render queue is saturated");
                pthread_mutex_unlock(&job->lock);
            }
        }
    }
    free(batch->jobs);
    free(batch);
    return NULL;
}

void *logana_aggregator_main(void *arg) {
    logana_engine_t *engine = (logana_engine_t *)arg;
    while (!engine->shutting_down) {
        logana_job_t *first = NULL;
        if (!logana_queue_pop(&engine->ingress_queue, (void **)&first, 100)) continue;
        logana_batch_t *batch = calloc(1, sizeof(*batch));
        if (!batch) continue;
        batch->engine = engine;
        batch->jobs = calloc(LOGANA_MAX_BATCH_JOBS, sizeof(logana_job_t *));
        if (!batch->jobs) {
            free(batch);
            continue;
        }
        uint64_t started = logana_now_ms();
        batch->jobs[batch->job_count++] = first;
        batch->total_bytes += first->payload_size;
        pthread_mutex_lock(&first->lock);
        first->status = LOGANA_JOB_BATCHING;
        pthread_mutex_unlock(&first->lock);

        while (batch->job_count < LOGANA_MAX_BATCH_JOBS &&
               batch->total_bytes < engine->config.min_batch_size_bytes &&
               (logana_now_ms() - started) < engine->config.aggregation_window_ms) {
            logana_job_t *next = NULL;
            if (!logana_queue_pop(&engine->ingress_queue, (void **)&next, engine->config.aggregation_window_ms)) {
                break;
            }
            batch->jobs[batch->job_count++] = next;
            batch->total_bytes += next->payload_size;
            pthread_mutex_lock(&next->lock);
            next->status = LOGANA_JOB_BATCHING;
            pthread_mutex_unlock(&next->lock);
        }

        if (logana_schedule_batch(engine, batch) != 0) {
            for (size_t i = 0; i < batch->job_count; ++i) {
                pthread_mutex_lock(&batch->jobs[i]->lock);
                batch->jobs[i]->status = LOGANA_JOB_FAILED;
                snprintf(batch->jobs[i]->error, sizeof(batch->jobs[i]->error), "%s", "failed to schedule analysis task");
                pthread_mutex_unlock(&batch->jobs[i]->lock);
            }
            free(batch->jobs);
            free(batch);
        }
    }
    return NULL;
}
