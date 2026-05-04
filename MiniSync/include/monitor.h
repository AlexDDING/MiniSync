#ifndef MINISYNC_MONITOR_H
#define MINISYNC_MONITOR_H

#include "task.h"

#define MAX_WATCHES 4096

typedef struct {
    int wd;
    char path[MAX_PATH_LEN];
} WatchItem;

typedef struct {
    char root[MAX_PATH_LEN];
    int event_fd;
    int log_fd;
    volatile sig_atomic_t *running;
} MonitorContext;

int add_watch_item(int wd, const char *path);
const char *find_path_by_wd(int wd);
int add_watch_recursive(int inotify_fd, const char *dir_path);
int scan_directory_and_emit_tasks(const char *root, const char *dir, int event_fd);
void monitor_process(const char *src_root, int event_write_fd, int log_write_fd);

#endif
