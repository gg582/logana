#ifndef LOGANA_QUEUE_H
#define LOGANA_QUEUE_H

#include "logana/types.h"

int logana_queue_init(logana_queue_t *queue, size_t capacity);
void logana_queue_close(logana_queue_t *queue);
void logana_queue_destroy(logana_queue_t *queue);
bool logana_queue_push(logana_queue_t *queue, void *item, uint32_t wait_ms);
bool logana_queue_pop(logana_queue_t *queue, void **item, uint32_t wait_ms);

void *logana_aggregator_main(void *arg);
void *logana_analyze_batch_task(void *arg);

#endif
