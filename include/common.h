#ifndef MINISYNC_COMMON_H
#define MINISYNC_COMMON_H

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_PATH_LEN 512
#define MAX_LOG_LEN 1024
#define MAX_NET_PATH 512
#define HASH_STR_LEN 32

#define DEFAULT_THREAD_COUNT 4
#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_HASH_THRESHOLD (10 * 1024 * 1024)
#define DEFAULT_DEBOUNCE_MS 1500
#define DEFAULT_PORT 9000
#define DEFAULT_CLIENT_LOG "./logs/client.log"
#define DEFAULT_SERVER_LOG "./logs/server.log"

#define MODE_LOCAL 1
#define MODE_REMOTE 2

#define MINISYNC_OK 0
#define MINISYNC_ERR (-1)

typedef struct {
    int mode;
    char src_root[MAX_PATH_LEN];
    char dst_root[MAX_PATH_LEN];
    char server_ip[64];
    int port;
    int thread_count;
    int block_size;
    int debounce_ms;
    off_t hash_threshold;
    char log_path[MAX_PATH_LEN];
} ClientConfig;

typedef struct {
    int port;
    char root[MAX_PATH_LEN];
    char log_path[MAX_PATH_LEN];
} ServerConfig;

long long current_time_ms(void);
void sleep_ms(int milliseconds);
int set_fd_nonblocking(int fd);
const char *minisync_basename(const char *path);

#endif
