#include "server.h"
#include "file_utils.h"
#include "hash.h"
#include "logger.h"

#include <arpa/inet.h>
#include <sys/socket.h>

static pthread_mutex_t server_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t server_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void server_log(const char *log_path, int level, const char *fmt, ...)
{
    pthread_mutex_lock(&server_log_mutex);

    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        fp = stdout;
    }

    LogMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.pid = getpid();
    msg.level = level;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg.message, sizeof(msg.message), fmt, ap);
    va_end(ap);

    write_log_record(fp, &msg);
    if (fp != stdout) {
        write_log_record(stdout, &msg);
        fclose(fp);
    }

    pthread_mutex_unlock(&server_log_mutex);
}

static int current_path(char *out, size_t out_size, const char *root, const char *rel_path)
{
    char current[MAX_PATH_LEN];
    if (sanitize_rel_path(rel_path) < 0) {
        return -1;
    }
    if (join_path(current, sizeof(current), root, "current") < 0) {
        return -1;
    }
    return join_path(out, out_size, current, rel_path);
}

static int recv_file_blocks_until_end(int client_fd, int out_fd, uint64_t expected_hash, char *tmp_path,
                                      size_t tmp_path_size, SyncHeader *end_header)
{
    while (1) {
        SyncHeader h;
        if (recv_header(client_fd, &h) < 0) {
            return -1;
        }

        if (h.msg_type == MSG_FILE_END) {
            *end_header = h;
            if (expected_hash != 0 && h.total_hash != 0 && expected_hash != h.total_hash) {
                return -1;
            }
            (void)tmp_path;
            (void)tmp_path_size;
            return 0;
        }

        if (h.msg_type != MSG_FILE_BLOCK || h.data_len == 0 || h.data_len > 16U * 1024U * 1024U) {
            return -1;
        }

        char *buf = (char *)malloc(h.data_len);
        if (!buf) {
            return -1;
        }
        if (read_full(client_fd, buf, h.data_len) < 0) {
            free(buf);
            return -1;
        }

        uint64_t block_hash = hash_buffer_fnv1a(buf, h.data_len);
        if (h.block_hash != 0 && block_hash != h.block_hash) {
            free(buf);
            return -1;
        }

        if (pwrite(out_fd, buf, h.data_len, (off_t)h.offset) != (ssize_t)h.data_len) {
            free(buf);
            return -1;
        }
        free(buf);
    }
}

int server_handle_file_begin(int client_fd, const SyncHeader *header, const char *root, const char *rel_path)
{
    char dst[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];

    if (current_path(dst, sizeof(dst), root, rel_path) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.syncing", dst) >= (int)sizeof(tmp)) {
        send_error(client_fd, "invalid path");
        return -1;
    }

    if (S_ISDIR((mode_t)header->mode)) {
        pthread_mutex_lock(&server_file_mutex);
        int ok = ensure_dir_exists(dst);
        pthread_mutex_unlock(&server_file_mutex);
        if (ok == 0) {
            send_ack(client_fd, 0);
            return 0;
        }
        send_error(client_fd, "mkdir failed");
        return -1;
    }

    pthread_mutex_lock(&server_file_mutex);
    if (ensure_parent_dir_exists(dst) < 0 || backup_old_version(root, rel_path) < 0) {
        pthread_mutex_unlock(&server_file_mutex);
        send_error(client_fd, "prepare destination failed");
        return -1;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, header->mode ? (mode_t)header->mode : 0644);
    if (fd < 0) {
        pthread_mutex_unlock(&server_file_mutex);
        send_error(client_fd, "open temp failed");
        return -1;
    }

    SyncHeader end_header;
    memset(&end_header, 0, sizeof(end_header));
    int ok = recv_file_blocks_until_end(client_fd, fd, header->total_hash, tmp, sizeof(tmp), &end_header);
    if (fsync(fd) < 0) {
        ok = -1;
    }
    close(fd);

    uint64_t actual_hash = ok == 0 ? hash_file_fnv1a(tmp) : 0;
    uint64_t expected_hash = end_header.total_hash ? end_header.total_hash : header->total_hash;
    if (ok == 0 && expected_hash != 0 && actual_hash != expected_hash) {
        ok = -1;
    }

    if (ok == 0 && safe_rename_or_copy_unlink(tmp, dst) == 0) {
        set_file_metadata(dst, (mode_t)header->mode, (time_t)header->mtime);
        pthread_mutex_unlock(&server_file_mutex);
        send_ack(client_fd, header->file_size);
        return 0;
    }

    pthread_mutex_unlock(&server_file_mutex);
    send_error(client_fd, "file receive failed");
    return -1;
}

int server_handle_file_delete(int client_fd, const char *root, const char *rel_path)
{
    pthread_mutex_lock(&server_file_mutex);
    int ok = move_to_trash(root, rel_path);
    pthread_mutex_unlock(&server_file_mutex);

    if (ok == 0) {
        send_ack(client_fd, 0);
        return 0;
    }
    send_error(client_fd, "delete failed");
    return -1;
}

int server_handle_file_rename(int client_fd, const char *root, const char *combined_path, uint32_t path_len)
{
    const char *old_rel = combined_path;
    size_t old_len = strnlen(old_rel, path_len);
    if (old_len >= path_len) {
        send_error(client_fd, "invalid rename payload");
        return -1;
    }
    const char *new_rel = combined_path + old_len + 1;
    if (new_rel >= combined_path + path_len || sanitize_rel_path(old_rel) < 0 || sanitize_rel_path(new_rel) < 0) {
        send_error(client_fd, "invalid rename path");
        return -1;
    }

    char old_path[MAX_PATH_LEN];
    char new_path[MAX_PATH_LEN];
    if (current_path(old_path, sizeof(old_path), root, old_rel) < 0 ||
        current_path(new_path, sizeof(new_path), root, new_rel) < 0) {
        send_error(client_fd, "rename path too long");
        return -1;
    }

    pthread_mutex_lock(&server_file_mutex);
    int ok = ensure_parent_dir_exists(new_path);
    if (ok == 0) {
        ok = safe_rename_or_copy_unlink(old_path, new_path);
    }
    pthread_mutex_unlock(&server_file_mutex);

    if (ok == 0) {
        send_ack(client_fd, 0);
        return 0;
    }
    send_error(client_fd, "rename failed");
    return -1;
}

int server_handle_query_offset(int client_fd, const char *root, const char *rel_path)
{
    char dst[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    off_t size = 0;

    if (current_path(dst, sizeof(dst), root, rel_path) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.syncing", dst) >= (int)sizeof(tmp)) {
        send_error(client_fd, "invalid query path");
        return -1;
    }

    if (get_file_size(tmp, &size) < 0) {
        size = 0;
    }

    SyncHeader reply;
    init_sync_header(&reply, MSG_OFFSET_REPLY);
    reply.offset = (uint64_t)size;
    return send_header(client_fd, &reply);
}

static int handle_large_block_stream(int client_fd, const SyncHeader *first_block, const char *root,
                                     const char *rel_path)
{
    char dst[MAX_PATH_LEN];
    char tmp[MAX_PATH_LEN];
    if (current_path(dst, sizeof(dst), root, rel_path) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.syncing", dst) >= (int)sizeof(tmp)) {
        send_error(client_fd, "invalid path");
        return -1;
    }

    pthread_mutex_lock(&server_file_mutex);
    if (ensure_parent_dir_exists(dst) < 0) {
        pthread_mutex_unlock(&server_file_mutex);
        send_error(client_fd, "prepare destination failed");
        return -1;
    }

    int fd = open(tmp, O_WRONLY | O_CREAT, first_block->mode ? (mode_t)first_block->mode : 0644);
    if (fd < 0) {
        pthread_mutex_unlock(&server_file_mutex);
        send_error(client_fd, "open temp failed");
        return -1;
    }

    char *buf = (char *)malloc(first_block->data_len);
    if (!buf) {
        close(fd);
        pthread_mutex_unlock(&server_file_mutex);
        send_error(client_fd, "alloc failed");
        return -1;
    }
    int ok = 0;
    if (read_full(client_fd, buf, first_block->data_len) < 0 ||
        hash_buffer_fnv1a(buf, first_block->data_len) != first_block->block_hash ||
        pwrite(fd, buf, first_block->data_len, (off_t)first_block->offset) != (ssize_t)first_block->data_len) {
        ok = -1;
    }
    free(buf);

    SyncHeader end_header;
    memset(&end_header, 0, sizeof(end_header));
    if (ok == 0) {
        ok = recv_file_blocks_until_end(client_fd, fd, 0, tmp, sizeof(tmp), &end_header);
    }
    if (fsync(fd) < 0) {
        ok = -1;
    }
    close(fd);

    uint64_t expected_hash = end_header.total_hash ? end_header.total_hash : first_block->total_hash;
    if (ok == 0 && expected_hash != 0 && hash_file_fnv1a(tmp) != expected_hash) {
        ok = -1;
    }

    if (ok == 0) {
        backup_old_version(root, rel_path);
        ok = safe_rename_or_copy_unlink(tmp, dst);
        if (ok == 0) {
            set_file_metadata(dst, (mode_t)first_block->mode, (time_t)first_block->mtime);
        }
    }
    pthread_mutex_unlock(&server_file_mutex);

    if (ok == 0) {
        send_ack(client_fd, first_block->file_size);
        return 0;
    }
    send_error(client_fd, "large file receive failed");
    return -1;
}

int server_handle_connection(int client_fd, const char *root, const char *log_path)
{
    char last_rel_path[MAX_NET_PATH + 1] = {0};

    while (1) {
        SyncHeader header;
        if (recv_header(client_fd, &header) < 0) {
            return -1;
        }

        if (header.msg_type == MSG_EXIT) {
            return 0;
        }

        char rel_path[MAX_NET_PATH + 1];
        memset(rel_path, 0, sizeof(rel_path));
        if (header.path_len > 0 && recv_path(client_fd, rel_path, sizeof(rel_path), header.path_len) < 0) {
            send_error(client_fd, "read path failed");
            return -1;
        }

        int rc = 0;
        switch (header.msg_type) {
        case MSG_HELLO:
            rc = send_ack(client_fd, 0);
            break;
        case MSG_FILE_BEGIN:
            snprintf(last_rel_path, sizeof(last_rel_path), "%s", rel_path);
            rc = server_handle_file_begin(client_fd, &header, root, rel_path);
            break;
        case MSG_QUERY_OFFSET:
            snprintf(last_rel_path, sizeof(last_rel_path), "%s", rel_path);
            rc = server_handle_query_offset(client_fd, root, rel_path);
            break;
        case MSG_FILE_BLOCK:
            if (last_rel_path[0] == '\0') {
                rc = -1;
                send_error(client_fd, "missing path before block");
            } else {
                rc = handle_large_block_stream(client_fd, &header, root, last_rel_path);
            }
            break;
        case MSG_FILE_DELETE:
            rc = server_handle_file_delete(client_fd, root, rel_path);
            break;
        case MSG_FILE_RENAME:
            rc = server_handle_file_rename(client_fd, root, rel_path, header.path_len);
            break;
        default:
            send_error(client_fd, "unknown message type");
            rc = -1;
            break;
        }

        server_log(log_path, rc == 0 ? LOG_INFO : LOG_ERROR, "server: msg=%s path=%s rc=%d",
                   msg_type_to_string(header.msg_type), rel_path, rc);
        if (rc < 0) {
            return rc;
        }

        if (header.msg_type != MSG_HELLO && header.msg_type != MSG_QUERY_OFFSET) {
            return 0;
        }
    }
}

void *server_client_thread(void *arg)
{
    ServerClientContext *ctx = (ServerClientContext *)arg;
    server_handle_connection(ctx->client_fd, ctx->root, ctx->log_path);
    close(ctx->client_fd);
    free(ctx);
    return NULL;
}

int run_server(const ServerConfig *config)
{
    char current[MAX_PATH_LEN];
    char versions[MAX_PATH_LEN];
    char trash[MAX_PATH_LEN];

    if (join_path(current, sizeof(current), config->root, "current") < 0 ||
        join_path(versions, sizeof(versions), config->root, ".versions") < 0 ||
        join_path(trash, sizeof(trash), config->root, ".trash") < 0 ||
        ensure_dir_exists(current) < 0 ||
        ensure_dir_exists(versions) < 0 ||
        ensure_dir_exists(trash) < 0 ||
        ensure_parent_dir_exists(config->log_path) < 0) {
        return -1;
    }

    int listen_fd = create_server_socket(config->port, 64);
    if (listen_fd < 0) {
        server_log(config->log_path, LOG_ERROR, "server: listen failed port=%d error=%s", config->port, strerror(errno));
        return -1;
    }

    server_log(config->log_path, LOG_INFO, "server: listening port=%d root=%s", config->port, config->root);

    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            server_log(config->log_path, LOG_ERROR, "server: accept failed: %s", strerror(errno));
            break;
        }

        ServerClientContext *ctx = (ServerClientContext *)calloc(1, sizeof(*ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->client_fd = client_fd;
        snprintf(ctx->root, sizeof(ctx->root), "%s", config->root);
        snprintf(ctx->log_path, sizeof(ctx->log_path), "%s", config->log_path);

        pthread_t tid;
        if (pthread_create(&tid, NULL, server_client_thread, ctx) != 0) {
            close(client_fd);
            free(ctx);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    return 0;
}
