#include "task_queue.h"

const char *event_type_to_string(int event_type)
{
    switch (event_type) {
    case EVENT_CREATE:
        return "CREATE";
    case EVENT_MODIFY:
        return "MODIFY";
    case EVENT_DELETE:
        return "DELETE";
    case EVENT_RENAME:
        return "RENAME";
    case EVENT_FULLSCAN:
        return "FULLSCAN";
    case EVENT_EXIT:
        return "EXIT";
    default:
        return "UNKNOWN";
    }
}

void init_sync_task(SyncTask *task)
{
    memset(task, 0, sizeof(*task));
}

int sync_task_same_target(const SyncTask *a, const SyncTask *b)
{
    return strcmp(a->rel_path, b->rel_path) == 0;
}

void task_queue_init(TaskQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void task_queue_destroy(TaskQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    TaskNode *node = q->front;
    while (node) {
        TaskNode *next = node->next;
        free(node);
        node = next;
    }
    q->front = NULL;
    q->rear = NULL;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

int task_queue_push(TaskQueue *q, const SyncTask *task)
{
    TaskNode *node = (TaskNode *)calloc(1, sizeof(TaskNode));
    if (!node) {
        return -1;
    }
    node->task = *task;

    pthread_mutex_lock(&q->mutex);
    if (q->stopped) {
        pthread_mutex_unlock(&q->mutex);
        free(node);
        return -1;
    }

    if (q->rear) {
        q->rear->next = node;
    } else {
        q->front = node;
    }
    q->rear = node;
    q->size++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int task_queue_pop(TaskQueue *q, SyncTask *task)
{
    pthread_mutex_lock(&q->mutex);
    while (q->size == 0 && !q->stopped) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    if (q->size == 0 && q->stopped) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    TaskNode *node = q->front;
    q->front = node->next;
    if (!q->front) {
        q->rear = NULL;
    }
    q->size--;
    *task = node->task;
    pthread_mutex_unlock(&q->mutex);

    free(node);
    return 1;
}

void task_queue_stop(TaskQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->stopped = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}
