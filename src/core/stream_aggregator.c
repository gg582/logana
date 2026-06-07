#include "logana/logana.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ttak/async/task.h>

int logana_queue_init(logana_queue_t *q, size_t capacity) {
    memset(q, 0, sizeof(*q));
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        capacity = cap;
    }
    q->slots = calloc(capacity, sizeof(logana_lf_slot_t));
    if (!q->slots) return -1;
    for (size_t i = 0; i < capacity; ++i) {
        atomic_init(&q->slots[i].seq, (int64_t)i);
    }
    q->capacity = capacity;
    q->mask = capacity - 1;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    atomic_init(&q->closed, false);
    return 0;
}

void logana_queue_close(logana_queue_t *q) {
    atomic_store_explicit(&q->closed, true, memory_order_release);
}

void logana_queue_destroy(logana_queue_t *q) {
    free(q->slots);
    memset(q, 0, sizeof(*q));
}

bool logana_queue_push(logana_queue_t *q, void *item, uint32_t wait_ms) {
    uint64_t deadline = logana_now_ms() + wait_ms;
    while (true) {
        if (atomic_load_explicit(&q->closed, memory_order_acquire))
            return false;

        size_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
        logana_lf_slot_t *slot = &q->slots[h & q->mask];
        int64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = seq - (int64_t)h;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->head, &h, h + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                slot->item = item;
                atomic_store_explicit(&slot->seq, h + 1, memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            if (logana_now_ms() >= deadline)
                return false;
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        /* diff > 0 : another thread is ahead, retry */
    }
}

bool logana_queue_pop(logana_queue_t *q, void **out, uint32_t wait_ms) {
    uint64_t deadline = logana_now_ms() + wait_ms;
    while (true) {
        size_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
        logana_lf_slot_t *slot = &q->slots[t & q->mask];
        int64_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = seq - (int64_t)(t + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->tail, &t, t + 1,
                    memory_order_relaxed, memory_order_relaxed)) {
                void *item = slot->item;
                atomic_store_explicit(&slot->seq, t + q->mask + 1, memory_order_release);
                *out = item;
                return true;
            }
        } else if (diff < 0) {
            if (atomic_load_explicit(&q->closed, memory_order_acquire))
                return false;
            if (logana_now_ms() >= deadline)
                return false;
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
        /* diff > 0 : retry */
    }
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
                atomic_store_explicit(&job->status, LOGANA_JOB_FAILED, memory_order_release);
                snprintf(job->error, sizeof(job->error), "%s", "render queue is saturated");
                logana_job_unref(job);
            }
        } else {
            logana_job_unref(job);
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
        if (!batch) {
            logana_job_unref(first);
            continue;
        }
        batch->engine = engine;
        batch->jobs = calloc(LOGANA_MAX_BATCH_JOBS, sizeof(logana_job_t *));
        if (!batch->jobs) {
            logana_job_unref(first);
            free(batch);
            continue;
        }
        uint64_t started = logana_now_ms();
        batch->jobs[batch->job_count++] = first;
        batch->total_bytes += first->payload_size;
        atomic_store_explicit(&first->status, LOGANA_JOB_BATCHING, memory_order_release);

        while (batch->job_count < LOGANA_MAX_BATCH_JOBS &&
               batch->total_bytes < engine->config.min_batch_size_bytes &&
               (logana_now_ms() - started) < engine->config.aggregation_window_ms) {
            logana_job_t *next = NULL;
            if (!logana_queue_pop(&engine->ingress_queue, (void **)&next, engine->config.aggregation_window_ms)) {
                break;
            }
            batch->jobs[batch->job_count++] = next;
            batch->total_bytes += next->payload_size;
            atomic_store_explicit(&next->status, LOGANA_JOB_BATCHING, memory_order_release);
        }

        if (logana_schedule_batch(engine, batch) != 0) {
            for (size_t i = 0; i < batch->job_count; ++i) {
                atomic_store_explicit(&batch->jobs[i]->status, LOGANA_JOB_FAILED, memory_order_release);
                snprintf(batch->jobs[i]->error, sizeof(batch->jobs[i]->error), "%s", "failed to schedule analysis task");
                logana_job_unref(batch->jobs[i]);
            }
            free(batch->jobs);
            free(batch);
        }
    }
    return NULL;
}
