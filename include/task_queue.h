#ifndef MINISYNC_TASK_QUEUE_H
#define MINISYNC_TASK_QUEUE_H

#include "task.h"

typedef struct TaskNode {
    SyncTask task;
    struct TaskNode *next;
} TaskNode;

typedef struct {
    TaskNode *front;
    TaskNode *rear;
    int size;
    int stopped;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TaskQueue;

void task_queue_init(TaskQueue *q);
void task_queue_destroy(TaskQueue *q);
int task_queue_push(TaskQueue *q, const SyncTask *task);
int task_queue_pop(TaskQueue *q, SyncTask *task);
void task_queue_stop(TaskQueue *q);

#endif
