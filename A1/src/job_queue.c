#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "job_queue.h"

int job_queue_init(struct job_queue *job_queue, int capacity) {
  if (capacity <= 0) {
    return -1;
  }
  job_queue->buffer = malloc(capacity * sizeof(void *));
  if (job_queue->buffer == NULL) {
    return -1;
  }
  job_queue->head = 0;
  job_queue->tail = 0;
  job_queue->num = 0;
  job_queue->max_capacity = capacity;
  int cond_result = pthread_cond_init(&job_queue->cond, NULL);
  if (cond_result != 0) {
    free(job_queue->buffer);
    return -1;
  }
  int mutex_result = pthread_mutex_init(&job_queue->mutex, NULL);
  if (mutex_result != 0) { 
    free(job_queue->buffer);
    return -1;
  }
  return 0;
}

int job_queue_destroy(struct job_queue *job_queue) {
  assert(0);
}

int job_queue_push(struct job_queue *job_queue, void *data) {
  assert(0);
}

int job_queue_pop(struct job_queue *job_queue, void **data) {
  assert(0);
}
