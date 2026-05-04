#include "transfer.h"
#include "file_utils.h"
#include "hash.h"
#include "logger.h"
#include "net_utils.h"
#include "protocol.h"

#include <sys/select.h>

static int copy_config(ClientConfig *dst, const ClientConfig *src)
{
    if (!dst || !src) {
        return -1;
    }
    *dst = *src;
    return 0;
}

static int pending_find(PendingTask *table, const char *rel_path)
{
    for (int i = 0; i < MAX_PENDING_TASKS; ++i) {
        if (table[i].used && strcmp(table[i].task.rel_path, rel_path) == 0) {
            return i;
        }
    }
    return -1;
}

static void pending_remove(PendingTask *table, const char *rel_path)
{
    int idx = pending_find(table, rel_path);
    if (idx >= 0) {
        memset(&table[idx], 0, sizeof(table[idx]));
    }
}

void pending_update_or_insert(PendingTask *table, const SyncTask *task)
{
    int idx = pending_find(table, task->rel_path);
    if (idx < 0) {
        for (int i = 0; i < MAX_PENDING_TASKS; ++i) {
            if (!table[i].used) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        return;
    }

    table[idx].used = 1;
    table[idx].task = *task;
    table[idx].last_update_ms = current_time_ms();
}

void pending_flush_stable_tasks(PendingTask *table, TaskQueue *queue, int debounce_ms)
{
    long long now = current_time_ms();
    for (int i = 0; i < MAX_PENDING_TASKS; ++i) {
        if (!table[i].used) {
            continue;
        }
        if (now - table[i].last_update_ms >= debounce_ms) {
            task_queue_push(queue, &table[i].task);
            memset(&table[i], 0, sizeof(table[i]));
        }
    }
}

static void pending_flush_all(PendingTask *table, TaskQueue *queue)
{
    for (int i = 0; i < MAX_PENDING_TASKS; ++i) {
        if (table[i].used) {
            task_queue_push(queue, &table[i].task);
            memset(&table[i], 0, sizeof(table[i]));
        }
    }
}

static int should_debounce(const SyncTask *task)
{
    return task->event_type == EVENT_MODIFY ||
           task->event_type == EVENT_CREATE ||
           task->event_type == EVENT_FULLSCAN;
}

void *scheduler_thread_main(void *arg)
{
    TransferContext *ctx = (TransferContext *)arg;

    while (!ctx->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx->event_read_fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ready = select(ctx->event_read_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_message(ctx->log_fd, LOG_ERROR, "scheduler: select failed: %s", strerror(errno));
            break;
        }

        if (ready > 0 && FD_ISSET(ctx->event_read_fd, &rfds)) {
            SyncTask task;
            ssize_t n = read_full(ctx->event_read_fd, &task, sizeof(task));
            if (n < 0) {
                log_message(ctx->log_fd, LOG_WARN, "scheduler: event pipe closed or partial read");
                break;
            }

            if (task.event_type == EVENT_EXIT) {
                log_message(ctx->log_fd, LOG_INFO, "scheduler: received EXIT");
                break;
            }

            if (task.event_type == EVENT_DELETE || task.event_type == EVENT_RENAME) {
                pending_remove(ctx->pending, task.rel_path);
                if (task.old_rel_path[0] != '\0') {
                    pending_remove(ctx->pending, task.old_rel_path);
                }
                task_queue_push(&ctx->queue, &task);
                log_message(ctx->log_fd, LOG_DEBUG, "scheduler: enqueue immediate %s %s",
                            event_type_to_string(task.event_type), task.rel_path);
            } else if (should_debounce(&task)) {
                pending_update_or_insert(ctx->pending, &task);
                log_message(ctx->log_fd, LOG_DEBUG, "scheduler: debounce update %s %s",
                            event_type_to_string(task.event_type), task.rel_path);
            } else {
                task_queue_push(&ctx->queue, &task);
            }
        }

        pending_flush_stable_tasks(ctx->pending, &ctx->queue, ctx->config.debounce_ms);
    }

    pending_flush_all(ctx->pending, &ctx->queue);
    task_queue_stop(&ctx->queue);
    ctx->stop = 1;
    return NULL;
}

static int requeue_or_fail(TransferContext *ctx, SyncTask *task, int worker_id)
{
    if (task->retry_count < MAX_RETRY_COUNT) {
        task->retry_count++;
        sleep_ms(300);
        task_queue_push(&ctx->queue, task);
        log_message(ctx->log_fd, LOG_WARN, "worker-%d: retry %s count=%d",
                    worker_id, task->rel_path, task->retry_count);
        return 0;
    }

    log_message(ctx->log_fd, LOG_ERROR, "worker-%d: give up %s after %d retries",
                worker_id, task->rel_path, task->retry_count);
    return -1;
}

void *worker_thread_main(void *arg)
{
    WorkerContext *worker = (WorkerContext *)arg;
    TransferContext *ctx = worker->ctx;
    int worker_id = worker->worker_id;
    free(worker);

    while (!ctx->stop) {
        SyncTask task;
        int popped = task_queue_pop(&ctx->queue, &task);
        if (popped == 0) {
            break;
        }

        int rc;
        if (ctx->config.mode == MODE_LOCAL) {
            rc = sync_task_local(&ctx->config, &task, ctx->log_fd, worker_id);
        } else {
            rc = sync_task_remote(&ctx->config, &task, ctx->log_fd, worker_id);
        }

        if (rc < 0) {
            requeue_or_fail(ctx, &task, worker_id);
        }
    }

    log_message(ctx->log_fd, LOG_INFO, "worker-%d: exit", worker_id);
    return NULL;
}

static int ensure_backup_layout(const char *root)
{
    char current[MAX_PATH_LEN];
    char versions[MAX_PATH_LEN];
    char trash[MAX_PATH_LEN];

    return join_path(current, sizeof(current), root, "current") == 0 &&
           join_path(versions, sizeof(versions), root, ".versions") == 0 &&
           join_path(trash, sizeof(trash), root, ".trash") == 0 &&
           ensure_dir_exists(current) == 0 &&
           ensure_dir_exists(versions) == 0 &&
           ensure_dir_exists(trash) == 0
               ? 0
               : -1;
}

static int local_target_path(char *out, size_t out_size, const char *backup_root, const char *rel_path)
{
    char current[MAX_PATH_LEN];
    if (sanitize_rel_path(rel_path) < 0) {
        return -1;
    }
    if (join_path(current, sizeof(current), backup_root, "current") < 0) {
        return -1;
    }
    return join_path(out, out_size, current, rel_path);
}

static int sync_local_file(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id)
{
    if (!is_regular_file(task->src_path)) {
        log_message(log_fd, LOG_WARN, "worker-%d: source disappeared %s", worker_id, task->rel_path);
        return 0;
    }

    int fd = open(task->src_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    if (try_read_lock(fd) < 0) {
        close(fd);
        log_message(log_fd, LOG_WARN, "worker-%d: file busy, delay %s", worker_id, task->rel_path);
        return -1;
    }
    unlock_file(fd);
    close(fd);

    char dst[MAX_PATH_LEN];
    if (local_target_path(dst, sizeof(dst), config->dst_root, task->rel_path) < 0) {
        return -1;
    }

    if (backup_old_version(config->dst_root, task->rel_path) < 0) {
        return -1;
    }

    uint64_t before = hash_file_fnv1a(task->src_path);
    if (copy_file_atomic(task->src_path, dst) < 0) {
        return -1;
    }
    uint64_t after = hash_file_fnv1a(dst);
    if (before != after) {
        return -1;
    }

    set_file_metadata(dst, task->mode, task->mtime);

    const char *strategy = task->file_size > config->hash_threshold ? "large-file" : "small-file";
    log_message(log_fd, LOG_INFO, "worker-%d: local sync success %s strategy=%s hash=%016" PRIx64,
                worker_id, task->rel_path, strategy, after);
    return 0;
}

int sync_task_local(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id)
{
    if (sanitize_rel_path(task->rel_path) < 0) {
        return -1;
    }

    if (task->event_type == EVENT_DELETE) {
        if (move_to_trash(config->dst_root, task->rel_path) == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: local trash %s", worker_id, task->rel_path);
            return 0;
        }
        return -1;
    }

    if (task->event_type == EVENT_RENAME && task->old_rel_path[0] != '\0') {
        char old_path[MAX_PATH_LEN];
        char new_path[MAX_PATH_LEN];
        if (local_target_path(old_path, sizeof(old_path), config->dst_root, task->old_rel_path) == 0 &&
            local_target_path(new_path, sizeof(new_path), config->dst_root, task->rel_path) == 0 &&
            ensure_parent_dir_exists(new_path) == 0 &&
            safe_rename_or_copy_unlink(old_path, new_path) == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: local rename %s -> %s",
                        worker_id, task->old_rel_path, task->rel_path);
            return 0;
        }
        log_message(log_fd, LOG_WARN, "worker-%d: rename fallback sync new path %s", worker_id, task->rel_path);
    }

    if (task->is_dir) {
        char dst[MAX_PATH_LEN];
        if (local_target_path(dst, sizeof(dst), config->dst_root, task->rel_path) < 0) {
            return -1;
        }
        if (ensure_dir_exists(dst) == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: local mkdir %s", worker_id, task->rel_path);
            return 0;
        }
        return -1;
    }

    return sync_local_file(config, task, log_fd, worker_id);
}

static int wait_for_ack(int fd)
{
    SyncHeader reply;
    if (recv_header(fd, &reply) < 0) {
        return -1;
    }
    return reply.msg_type == MSG_ACK || reply.msg_type == MSG_OFFSET_REPLY ? 0 : -1;
}

static int send_header_with_path(int fd, uint32_t msg_type, const char *rel_path, SyncHeader *out)
{
    SyncHeader header;
    init_sync_header(&header, msg_type);
    header.path_len = (uint32_t)strlen(rel_path);
    if (header.path_len > MAX_NET_PATH) {
        return -1;
    }
    if (out) {
        *out = header;
    }
    if (send_header(fd, &header) < 0) {
        return -1;
    }
    return send_path(fd, rel_path);
}

static int send_remote_delete(const ClientConfig *config, const SyncTask *task)
{
    int fd = create_client_socket(config->server_ip, config->port);
    if (fd < 0) {
        return -1;
    }
    int ok = send_header_with_path(fd, MSG_FILE_DELETE, task->rel_path, NULL);
    if (ok == 0) {
        ok = wait_for_ack(fd);
    }
    close(fd);
    return ok;
}

static int send_remote_rename(const ClientConfig *config, const SyncTask *task)
{
    if (task->old_rel_path[0] == '\0') {
        return -1;
    }

    char payload[MAX_NET_PATH + 1];
    size_t old_len = strlen(task->old_rel_path);
    size_t new_len = strlen(task->rel_path);
    if (old_len + 1 + new_len > MAX_NET_PATH) {
        return -1;
    }
    memcpy(payload, task->old_rel_path, old_len);
    payload[old_len] = '\0';
    memcpy(payload + old_len + 1, task->rel_path, new_len);

    int fd = create_client_socket(config->server_ip, config->port);
    if (fd < 0) {
        return -1;
    }

    SyncHeader header;
    init_sync_header(&header, MSG_FILE_RENAME);
    header.path_len = (uint32_t)(old_len + 1 + new_len);
    int ok = send_header(fd, &header);
    if (ok == 0) {
        ok = write_full(fd, payload, header.path_len) == (ssize_t)header.path_len ? 0 : -1;
    }
    if (ok == 0) {
        ok = wait_for_ack(fd);
    }
    close(fd);
    return ok;
}

static int send_remote_dir_create(const ClientConfig *config, const SyncTask *task)
{
    int fd = create_client_socket(config->server_ip, config->port);
    if (fd < 0) {
        return -1;
    }

    SyncHeader header;
    init_sync_header(&header, MSG_FILE_BEGIN);
    header.path_len = (uint32_t)strlen(task->rel_path);
    header.mode = (uint32_t)(task->mode | S_IFDIR);
    header.mtime = (int64_t)task->mtime;

    int ok = send_header(fd, &header);
    if (ok == 0) {
        ok = send_path(fd, task->rel_path);
    }
    if (ok == 0) {
        ok = wait_for_ack(fd);
    }
    close(fd);
    return ok;
}

static int send_file_block(int fd, const void *buf, uint32_t len, uint64_t offset,
                           uint64_t file_size, uint64_t total_hash, mode_t mode, time_t mtime)
{
    SyncHeader h;
    init_sync_header(&h, MSG_FILE_BLOCK);
    h.file_size = file_size;
    h.offset = offset;
    h.data_len = len;
    h.mode = (uint32_t)mode;
    h.mtime = (int64_t)mtime;
    h.total_hash = total_hash;
    h.block_hash = hash_buffer_fnv1a(buf, len);

    if (send_header(fd, &h) < 0) {
        return -1;
    }
    return write_full(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

static int send_file_end(int fd, uint64_t file_size, uint64_t total_hash, mode_t mode, time_t mtime)
{
    SyncHeader h;
    init_sync_header(&h, MSG_FILE_END);
    h.file_size = file_size;
    h.offset = file_size;
    h.mode = (uint32_t)mode;
    h.mtime = (int64_t)mtime;
    h.total_hash = total_hash;
    return send_header(fd, &h);
}

static int send_remote_small_file(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id)
{
    int fd = create_client_socket(config->server_ip, config->port);
    if (fd < 0) {
        return -1;
    }

    uint64_t total_hash = hash_file_fnv1a(task->src_path);
    SyncHeader begin;
    init_sync_header(&begin, MSG_FILE_BEGIN);
    begin.path_len = (uint32_t)strlen(task->rel_path);
    begin.file_size = (uint64_t)task->file_size;
    begin.mode = (uint32_t)task->mode;
    begin.mtime = (int64_t)task->mtime;
    begin.total_hash = total_hash;

    int ok = send_header(fd, &begin);
    if (ok == 0) {
        ok = send_path(fd, task->rel_path);
    }

    int in = -1;
    if (ok == 0) {
        in = open(task->src_path, O_RDONLY);
        if (in < 0) {
            ok = -1;
        }
    }

    char *buf = NULL;
    if (ok == 0) {
        size_t size = (size_t)task->file_size;
        buf = (char *)malloc(size == 0 ? 1 : size);
        if (!buf) {
            ok = -1;
        } else if (size > 0 && read_full(in, buf, size) < 0) {
            ok = -1;
        }
        if (ok == 0 && size > 0) {
            ok = send_file_block(fd, buf, (uint32_t)size, 0, (uint64_t)task->file_size,
                                 total_hash, task->mode, task->mtime);
        }
    }

    if (ok == 0) {
        ok = send_file_end(fd, (uint64_t)task->file_size, total_hash, task->mode, task->mtime);
    }
    if (ok == 0) {
        ok = wait_for_ack(fd);
    }

    free(buf);
    if (in >= 0) {
        close(in);
    }
    close(fd);

    if (ok == 0) {
        log_message(log_fd, LOG_INFO, "worker-%d: remote sync success %s strategy=small-file hash=%016" PRIx64,
                    worker_id, task->rel_path, total_hash);
    }
    return ok;
}

static int query_remote_offset(int fd, const SyncTask *task, uint64_t *offset)
{
    if (send_header_with_path(fd, MSG_QUERY_OFFSET, task->rel_path, NULL) < 0) {
        return -1;
    }

    SyncHeader reply;
    if (recv_header(fd, &reply) < 0 || reply.msg_type != MSG_OFFSET_REPLY) {
        return -1;
    }
    *offset = reply.offset;
    return 0;
}

static int send_remote_large_file(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id)
{
    int fd = create_client_socket(config->server_ip, config->port);
    if (fd < 0) {
        return -1;
    }

    uint64_t offset = 0;
    int ok = query_remote_offset(fd, task, &offset);
    if (ok < 0) {
        close(fd);
        return -1;
    }

    int in = open(task->src_path, O_RDONLY);
    if (in < 0) {
        close(fd);
        return -1;
    }

    uint64_t total_hash = hash_file_fnv1a(task->src_path);
    if (lseek(in, (off_t)offset, SEEK_SET) < 0) {
        close(in);
        close(fd);
        return -1;
    }

    char *buf = (char *)malloc((size_t)config->block_size);
    if (!buf) {
        close(in);
        close(fd);
        return -1;
    }

    uint64_t pos = offset;
    while (ok == 0) {
        ssize_t n = read(in, buf, (size_t)config->block_size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = -1;
            break;
        }
        if (n == 0) {
            break;
        }
        ok = send_file_block(fd, buf, (uint32_t)n, pos, (uint64_t)task->file_size,
                             total_hash, task->mode, task->mtime);
        pos += (uint64_t)n;
    }

    if (ok == 0) {
        ok = send_file_end(fd, (uint64_t)task->file_size, total_hash, task->mode, task->mtime);
    }
    if (ok == 0) {
        ok = wait_for_ack(fd);
    }

    free(buf);
    close(in);
    close(fd);

    if (ok == 0) {
        log_message(log_fd, LOG_INFO,
                    "worker-%d: remote sync success %s strategy=large-file resume offset=%" PRIu64 " hash=%016" PRIx64,
                    worker_id, task->rel_path, offset, total_hash);
    }
    return ok;
}

int sync_task_remote(const ClientConfig *config, const SyncTask *task, int log_fd, int worker_id)
{
    if (sanitize_rel_path(task->rel_path) < 0) {
        return -1;
    }

    if (task->event_type == EVENT_DELETE) {
        int rc = send_remote_delete(config, task);
        if (rc == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: remote delete %s", worker_id, task->rel_path);
        }
        return rc;
    }

    if (task->event_type == EVENT_RENAME && task->old_rel_path[0] != '\0') {
        int rc = send_remote_rename(config, task);
        if (rc == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: remote rename %s -> %s",
                        worker_id, task->old_rel_path, task->rel_path);
            return 0;
        }
        log_message(log_fd, LOG_WARN, "worker-%d: remote rename fallback sync %s", worker_id, task->rel_path);
    }

    if (task->is_dir) {
        int rc = send_remote_dir_create(config, task);
        if (rc == 0) {
            log_message(log_fd, LOG_INFO, "worker-%d: remote mkdir %s", worker_id, task->rel_path);
        }
        return rc;
    }

    if (!is_regular_file(task->src_path)) {
        log_message(log_fd, LOG_WARN, "worker-%d: source disappeared %s", worker_id, task->rel_path);
        return 0;
    }

    int lock_fd = open(task->src_path, O_RDONLY);
    if (lock_fd < 0) {
        return -1;
    }
    if (try_read_lock(lock_fd) < 0) {
        close(lock_fd);
        return -1;
    }
    unlock_file(lock_fd);
    close(lock_fd);

    if (task->file_size > config->hash_threshold) {
        return send_remote_large_file(config, task, log_fd, worker_id);
    }
    return send_remote_small_file(config, task, log_fd, worker_id);
}

void transfer_process(const ClientConfig *config, int event_read_fd, int log_write_fd)
{
    TransferContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    copy_config(&ctx.config, config);
    ctx.event_read_fd = event_read_fd;
    ctx.log_fd = log_write_fd;

    if (ctx.config.thread_count <= 0) {
        ctx.config.thread_count = DEFAULT_THREAD_COUNT;
    }
    if (ctx.config.block_size <= 0) {
        ctx.config.block_size = DEFAULT_BLOCK_SIZE;
    }
    if (ctx.config.debounce_ms <= 0) {
        ctx.config.debounce_ms = DEFAULT_DEBOUNCE_MS;
    }

    if (ctx.config.mode == MODE_LOCAL && ensure_backup_layout(ctx.config.dst_root) < 0) {
        log_message(log_write_fd, LOG_ERROR, "transfer: ensure backup layout failed root=%s", ctx.config.dst_root);
        _exit(1);
    }

    task_queue_init(&ctx.queue);
    ctx.worker_threads = (pthread_t *)calloc((size_t)ctx.config.thread_count, sizeof(pthread_t));
    if (!ctx.worker_threads) {
        log_message(log_write_fd, LOG_ERROR, "transfer: alloc worker threads failed");
        _exit(1);
    }

    if (pthread_create(&ctx.scheduler_thread, NULL, scheduler_thread_main, &ctx) != 0) {
        log_message(log_write_fd, LOG_ERROR, "transfer: create scheduler failed");
        free(ctx.worker_threads);
        _exit(1);
    }

    for (int i = 0; i < ctx.config.thread_count; ++i) {
        WorkerContext *worker = (WorkerContext *)calloc(1, sizeof(*worker));
        if (!worker) {
            continue;
        }
        worker->ctx = &ctx;
        worker->worker_id = i + 1;
        if (pthread_create(&ctx.worker_threads[i], NULL, worker_thread_main, worker) != 0) {
            free(worker);
        }
    }

    pthread_join(ctx.scheduler_thread, NULL);
    for (int i = 0; i < ctx.config.thread_count; ++i) {
        if (ctx.worker_threads[i]) {
            pthread_join(ctx.worker_threads[i], NULL);
        }
    }

    task_queue_destroy(&ctx.queue);
    free(ctx.worker_threads);
    close(event_read_fd);
    log_message(log_write_fd, LOG_INFO, "transfer: process exit");
    _exit(0);
}
