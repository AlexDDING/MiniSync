#include "monitor.h"
#include "file_utils.h"
#include "logger.h"
#include "net_utils.h"

#include <dirent.h>
#include <sys/inotify.h>
#include <sys/select.h>

static WatchItem watch_items[MAX_WATCHES];
static int watch_count = 0;
static int g_inotify_fd = -1;
static char g_monitor_root[MAX_PATH_LEN];
static int g_monitor_log_fd = -1;

#define MAX_SNAPSHOTS 8192

typedef struct {
    int used;
    int seen;
    char rel_path[MAX_PATH_LEN];
    char src_path[MAX_PATH_LEN];
    off_t size;
    time_t mtime;
    mode_t mode;
    int is_dir;
} FileSnapshot;

static FileSnapshot snapshots[MAX_SNAPSHOTS];

int add_watch_item(int wd, const char *path)
{
    if (wd < 0 || !path) {
        return -1;
    }

    for (int i = 0; i < watch_count; ++i) {
        if (watch_items[i].wd == wd) {
            snprintf(watch_items[i].path, sizeof(watch_items[i].path), "%s", path);
            return 0;
        }
    }

    if (watch_count >= MAX_WATCHES) {
        return -1;
    }

    watch_items[watch_count].wd = wd;
    if (snprintf(watch_items[watch_count].path, sizeof(watch_items[watch_count].path), "%s", path) >=
        (int)sizeof(watch_items[watch_count].path)) {
        return -1;
    }
    watch_count++;
    return 0;
}

const char *find_path_by_wd(int wd)
{
    for (int i = 0; i < watch_count; ++i) {
        if (watch_items[i].wd == wd) {
            return watch_items[i].path;
        }
    }
    return NULL;
}

static int emit_task(int event_fd, const SyncTask *task)
{
    return write_full(event_fd, task, sizeof(*task)) == (ssize_t)sizeof(*task) ? 0 : -1;
}

static int build_task_from_path(SyncTask *task, int event_type, const char *root, const char *path)
{
    struct stat st;
    init_sync_task(task);
    task->event_type = event_type;

    if (snprintf(task->src_path, sizeof(task->src_path), "%s", path) >= (int)sizeof(task->src_path)) {
        return -1;
    }
    if (get_relative_path(task->rel_path, sizeof(task->rel_path), root, path) < 0) {
        return -1;
    }

    if (lstat(path, &st) == 0) {
        task->file_size = st.st_size;
        task->mode = st.st_mode;
        task->mtime = st.st_mtime;
        task->is_dir = S_ISDIR(st.st_mode);
    }
    return 0;
}

static int snapshot_find(const char *rel_path)
{
    for (int i = 0; i < MAX_SNAPSHOTS; ++i) {
        if (snapshots[i].used && strcmp(snapshots[i].rel_path, rel_path) == 0) {
            return i;
        }
    }
    return -1;
}

static int snapshot_alloc(void)
{
    for (int i = 0; i < MAX_SNAPSHOTS; ++i) {
        if (!snapshots[i].used) {
            return i;
        }
    }
    return -1;
}

static void snapshot_mark_all_unseen(void)
{
    for (int i = 0; i < MAX_SNAPSHOTS; ++i) {
        if (snapshots[i].used) {
            snapshots[i].seen = 0;
        }
    }
}

static void snapshot_update_from_task(int idx, const SyncTask *task)
{
    snapshots[idx].used = 1;
    snapshots[idx].seen = 1;
    snprintf(snapshots[idx].rel_path, sizeof(snapshots[idx].rel_path), "%s", task->rel_path);
    snprintf(snapshots[idx].src_path, sizeof(snapshots[idx].src_path), "%s", task->src_path);
    snapshots[idx].size = task->file_size;
    snapshots[idx].mtime = task->mtime;
    snapshots[idx].mode = task->mode;
    snapshots[idx].is_dir = task->is_dir;
}

static void poll_scan_path(const char *root, const char *dir, int event_fd)
{
    DIR *dp = opendir(dir);
    if (!dp) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[MAX_PATH_LEN];
        if (join_path(path, sizeof(path), dir, entry->d_name) < 0) {
            continue;
        }

        SyncTask task;
        if (build_task_from_path(&task, EVENT_MODIFY, root, path) < 0) {
            continue;
        }

        int idx = snapshot_find(task.rel_path);
        if (idx < 0) {
            idx = snapshot_alloc();
            if (idx >= 0) {
                task.event_type = EVENT_CREATE;
                emit_task(event_fd, &task);
                log_message(g_monitor_log_fd, LOG_DEBUG, "monitor: poll create %s", task.rel_path);
                snapshot_update_from_task(idx, &task);
            }
        } else {
            snapshots[idx].seen = 1;
            if (snapshots[idx].size != task.file_size ||
                snapshots[idx].mtime != task.mtime ||
                snapshots[idx].mode != task.mode) {
                task.event_type = EVENT_MODIFY;
                emit_task(event_fd, &task);
                log_message(g_monitor_log_fd, LOG_DEBUG, "monitor: poll modify %s", task.rel_path);
                snapshot_update_from_task(idx, &task);
            }
        }

        if (task.is_dir) {
            poll_scan_path(root, path, event_fd);
        }
    }

    closedir(dp);
}

static void poll_scan_and_emit_changes(const char *root, int event_fd)
{
    snapshot_mark_all_unseen();
    poll_scan_path(root, root, event_fd);

    for (int i = 0; i < MAX_SNAPSHOTS; ++i) {
        if (snapshots[i].used && !snapshots[i].seen) {
            SyncTask task;
            init_sync_task(&task);
            task.event_type = EVENT_DELETE;
            task.is_dir = snapshots[i].is_dir;
            snprintf(task.rel_path, sizeof(task.rel_path), "%s", snapshots[i].rel_path);
            snprintf(task.src_path, sizeof(task.src_path), "%s", snapshots[i].src_path);
            emit_task(event_fd, &task);
            log_message(g_monitor_log_fd, LOG_DEBUG, "monitor: poll delete %s", task.rel_path);
            memset(&snapshots[i], 0, sizeof(snapshots[i]));
        }
    }
}

int add_watch_recursive(int inotify_fd, const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return -1;
    }

    uint32_t mask = IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
    int wd = inotify_add_watch(inotify_fd, dir_path, mask);
    if (wd < 0) {
        closedir(dir);
        return -1;
    }
    add_watch_item(wd, dir_path);
    log_message(g_monitor_log_fd, LOG_INFO, "monitor: watch added %s", dir_path);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child[MAX_PATH_LEN];
        if (join_path(child, sizeof(child), dir_path, entry->d_name) < 0) {
            continue;
        }
        if (is_directory(child)) {
            add_watch_recursive(inotify_fd, child);
        }
    }

    closedir(dir);
    return 0;
}

int scan_directory_and_emit_tasks(const char *root, const char *dir, int event_fd)
{
    DIR *dp = opendir(dir);
    if (!dp) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[MAX_PATH_LEN];
        if (join_path(path, sizeof(path), dir, entry->d_name) < 0) {
            continue;
        }

        SyncTask task;
        if (build_task_from_path(&task, EVENT_FULLSCAN, root, path) == 0) {
            emit_task(event_fd, &task);
            int idx = snapshot_find(task.rel_path);
            if (idx < 0) {
                idx = snapshot_alloc();
            }
            if (idx >= 0) {
                snapshot_update_from_task(idx, &task);
            }
            log_message(g_monitor_log_fd, LOG_DEBUG, "monitor: fullscan emit %s", task.rel_path);
        }

        if (is_directory(path)) {
            if (g_inotify_fd >= 0) {
                add_watch_recursive(g_inotify_fd, path);
            }
            scan_directory_and_emit_tasks(root, path, event_fd);
        }
    }

    closedir(dp);
    return 0;
}

static int emit_delete_task(const char *root, const char *path, int is_dir, int event_fd)
{
    SyncTask task;
    init_sync_task(&task);
    task.event_type = EVENT_DELETE;
    task.is_dir = is_dir;

    if (snprintf(task.src_path, sizeof(task.src_path), "%s", path) >= (int)sizeof(task.src_path)) {
        return -1;
    }
    if (get_relative_path(task.rel_path, sizeof(task.rel_path), root, path) < 0) {
        return -1;
    }
    return emit_task(event_fd, &task);
}

typedef struct {
    uint32_t cookie;
    char old_path[MAX_PATH_LEN];
    char old_rel_path[MAX_PATH_LEN];
    int is_dir;
    long long ts_ms;
} MoveRecord;

static int handle_event(const struct inotify_event *ev, int event_fd, MoveRecord *move)
{
    const char *parent = find_path_by_wd(ev->wd);
    if (!parent || ev->len == 0) {
        return 0;
    }

    char path[MAX_PATH_LEN];
    if (join_path(path, sizeof(path), parent, ev->name) < 0) {
        return -1;
    }

    int is_dir = (ev->mask & IN_ISDIR) != 0;

    if (ev->mask & IN_MOVED_FROM) {
        memset(move, 0, sizeof(*move));
        move->cookie = ev->cookie;
        move->is_dir = is_dir;
        move->ts_ms = current_time_ms();
        snprintf(move->old_path, sizeof(move->old_path), "%s", path);
        get_relative_path(move->old_rel_path, sizeof(move->old_rel_path), g_monitor_root, path);
        return 0;
    }

    if (ev->mask & IN_MOVED_TO) {
        SyncTask task;
        if (build_task_from_path(&task, EVENT_RENAME, g_monitor_root, path) < 0) {
            return -1;
        }
        if (move->cookie != 0 && move->cookie == ev->cookie) {
            snprintf(task.old_rel_path, sizeof(task.old_rel_path), "%s", move->old_rel_path);
        }
        emit_task(event_fd, &task);
        log_message(g_monitor_log_fd, LOG_INFO, "monitor: rename %s -> %s", task.old_rel_path, task.rel_path);

        if (is_dir) {
            /*
             * inotify 不递归。目录被移动进监听树后，move 到 add_watch 之间的内部文件事件可能丢失。
             * 所以立刻 add_watch_recursive，再短扫描该目录，把已经存在的文件补充为任务。
             */
            add_watch_recursive(g_inotify_fd, path);
            scan_directory_and_emit_tasks(g_monitor_root, path, event_fd);
        }
        memset(move, 0, sizeof(*move));
        return 0;
    }

    if (ev->mask & IN_DELETE) {
        emit_delete_task(g_monitor_root, path, is_dir, event_fd);
        log_message(g_monitor_log_fd, LOG_INFO, "monitor: delete %s", path);
        return 0;
    }

    if (ev->mask & IN_CREATE) {
        SyncTask task;
        if (build_task_from_path(&task, EVENT_CREATE, g_monitor_root, path) == 0) {
            emit_task(event_fd, &task);
            log_message(g_monitor_log_fd, LOG_INFO, "monitor: create %s", task.rel_path);
        }

        if (is_dir) {
            /*
             * 新建目录竞态兜底：用户可能在 add_watch 前已经向目录内写入文件。
             * 立即递归加 watch 并短扫描，补齐可能丢失的 CREATE/MODIFY 事件。
             */
            add_watch_recursive(g_inotify_fd, path);
            scan_directory_and_emit_tasks(g_monitor_root, path, event_fd);
        }
        return 0;
    }

    if ((ev->mask & IN_CLOSE_WRITE) || (ev->mask & IN_MODIFY)) {
        if (!is_dir) {
            SyncTask task;
            if (build_task_from_path(&task, EVENT_MODIFY, g_monitor_root, path) == 0) {
                emit_task(event_fd, &task);
                log_message(g_monitor_log_fd, LOG_DEBUG, "monitor: modify %s", task.rel_path);
            }
        }
        return 0;
    }

    return 0;
}

void monitor_process(const char *src_root, int event_write_fd, int log_write_fd)
{
    g_monitor_log_fd = log_write_fd;
    snprintf(g_monitor_root, sizeof(g_monitor_root), "%s", src_root);

    g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify_fd < 0) {
        log_message(log_write_fd, LOG_ERROR, "monitor: inotify_init failed: %s", strerror(errno));
        _exit(1);
    }

    if (add_watch_recursive(g_inotify_fd, src_root) < 0) {
        log_message(log_write_fd, LOG_ERROR, "monitor: add_watch_recursive failed: %s", strerror(errno));
    }

    scan_directory_and_emit_tasks(src_root, src_root, event_write_fd);
    log_message(log_write_fd, LOG_INFO, "monitor: initial scan completed root=%s", src_root);

    MoveRecord move;
    memset(&move, 0, sizeof(move));
    char buf[sizeof(struct inotify_event) + NAME_MAX + 1];

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_inotify_fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(g_inotify_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            if (move.cookie != 0 && current_time_ms() - move.ts_ms > 2000) {
                emit_delete_task(g_monitor_root, move.old_path, move.is_dir, event_write_fd);
                memset(&move, 0, sizeof(move));
            }
            poll_scan_and_emit_changes(g_monitor_root, event_write_fd);
            continue;
        }

        while (1) {
            ssize_t n = read(g_inotify_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                log_message(log_write_fd, LOG_ERROR, "monitor: read inotify failed: %s", strerror(errno));
                close(g_inotify_fd);
                _exit(1);
            }

            ssize_t off = 0;
            while (off < n) {
                struct inotify_event *ev = (struct inotify_event *)(void *)(buf + off);
                handle_event(ev, event_write_fd, &move);
                off += (ssize_t)sizeof(struct inotify_event) + ev->len;
            }
        }
    }

    close(g_inotify_fd);
    close(event_write_fd);
    _exit(0);
}
