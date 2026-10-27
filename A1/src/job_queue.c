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
  job_queue->destroy = 0;
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
  pthread_mutex_lock(&job_queue->mutex);
  while (job_queue->num > 0) {
    pthread_cond_wait(&job_queue->cond, &job_queue->mutex);
  }
  job_queue->destroy = -1;
  pthread_cond_broadcast(&job_queue->cond);
  pthread_mutex_unlock(&job_queue->mutex);
  free(job_queue->buffer);
  return 0;
}

int job_queue_push(struct job_queue *job_queue, void *data) {
  pthread_mutex_lock(&job_queue->mutex);
  if (job_queue->destroy == -1){
   pthread_mutex_unlock(&job_queue->mutex);
   return -1;
  }
  while (job_queue->num == job_queue->max_capacity && job_queue->destroy !=-1) {
    pthread_cond_wait(&job_queue->cond, &job_queue->mutex);
  }
  if (job_queue->destroy == -1){
   pthread_mutex_unlock(&job_queue->mutex);
   return -1;
  }
  job_queue->buffer[job_queue->tail] = data;
  job_queue->num++;
  if (job_queue->tail == (job_queue->max_capacity - 1)){
    job_queue->tail = 0; 
    } else{
    job_queue->tail ++;
  }
  pthread_cond_broadcast(&job_queue->cond);
  pthread_mutex_unlock(&job_queue->mutex);
  return 0;
}

int job_queue_pop(struct job_queue *job_queue, void **data) {
  pthread_mutex_lock(&job_queue->mutex);
  if (job_queue->destroy == -1){
    pthread_mutex_unlock(&job_queue->mutex);
    return -1;
  }
  while (job_queue->num == 0 && job_queue->destroy !=-1){
    pthread_cond_wait(&job_queue->cond, &job_queue->mutex);
  }
  if (job_queue->destroy == -1){
   pthread_mutex_unlock(&job_queue->mutex);
   return -1;
  }
  // copy the stored job, pointer in to data
  *data = job_queue->buffer[job_queue->head];
  job_queue->num --;
  if (job_queue->head == (job_queue->max_capacity-1)){
    job_queue->head =0;
  }else{
    job_queue->head++;
  }
  // tells the waiting threads that the jobqueue is changed
  pthread_cond_broadcast(&job_queue->cond);
  pthread_mutex_unlock(&job_queue->mutex);
  return 0;
}
