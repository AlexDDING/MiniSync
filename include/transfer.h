#ifndef MINISYNC_TRANSFER_H
#define MINISYNC_TRANSFER_H

#include "task_queue.h"

#define MAX_PENDING_TASKS 4096

typedef struct {
    int used;
    SyncTask task;
    long long last_update_ms;
} PendingTask;

typedef struct {
    ClientConfig config;
    int event_read_fd;
    int log_fd;
    TaskQueue queue;
    PendingTask pending[MAX_PENDING_TASKS];
    pthread_t scheduler_thread;
    pthread_t *worker_threads;
    volatile sig_atomic_t stop;
} TransferContext;

typedef struct {
    TransferContext *ctx;
    int worker_id;
} WorkerContext;

void transfer_process(const ClientConfig *config, int event_read_fd, int log_write_fd);
void pending_update_or_insert(PendingTask *table, const SyncTask *task);
void pending_flush_stable_tasks(PendingTask *table, TaskQueue *queue, int debounce_ms);
void *scheduler_thread_main(void *arg);
void *worker_thread_main(void *arg);
int sync_task_local(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id);
int sync_task_remote(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id);

#endif
