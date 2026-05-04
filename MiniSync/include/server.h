#ifndef MINISYNC_SERVER_H
#define MINISYNC_SERVER_H

#include "net_utils.h"
#include "task.h"

typedef struct {
    int client_fd;
    char root[MAX_PATH_LEN];
    char log_path[MAX_PATH_LEN];
} ServerClientContext;

int run_server(const ServerConfig *config);
void *server_client_thread(void *arg);
int server_handle_connection(int client_fd, const char *root, const char *log_path);
int server_handle_file_begin(int client_fd, const SyncHeader *header, const char *root, const char *rel_path);
int server_handle_file_delete(int client_fd, const char *root, const char *rel_path);
int server_handle_file_rename(int client_fd, const char *root, const char *combined_path, uint32_t path_len);
int server_handle_query_offset(int client_fd, const char *root, const char *rel_path);
void server_log(const char *log_path, int level, const char *fmt, ...);

#endif
