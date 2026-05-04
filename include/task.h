#ifndef MINISYNC_TASK_H
#define MINISYNC_TASK_H

#include "common.h"

#define EVENT_CREATE 1
#define EVENT_MODIFY 2
#define EVENT_DELETE 3
#define EVENT_RENAME 4
#define EVENT_FULLSCAN 5
#define EVENT_EXIT 99

#define MAX_RETRY_COUNT 3

typedef struct {
    int event_type;
    char src_path[MAX_PATH_LEN];
    char rel_path[MAX_PATH_LEN];
    char old_rel_path[MAX_PATH_LEN];
    off_t file_size;
    mode_t mode;
    time_t mtime;
    int is_dir;
    int retry_count;
} SyncTask;

const char *event_type_to_string(int event_type);
void init_sync_task(SyncTask *task);
int sync_task_same_target(const SyncTask *a, const SyncTask *b);

#endif
