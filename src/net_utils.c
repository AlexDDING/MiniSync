#include "net_utils.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

long long current_time_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

void sleep_ms(int milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (long)(milliseconds % 1000) * 1000000L;

    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

int set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

const char *minisync_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

ssize_t readn(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t left = n;

    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return (ssize_t)(n - left);
        }
        p += r;
        left -= (size_t)r;
    }

    return (ssize_t)n;
}

ssize_t writen(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t left = n;

    while (left > 0) {
        ssize_t r = write(fd, p, left);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /*
                 * pipe 或 socket 被设置为非阻塞时，写满会返回 EAGAIN。
                 * 这里短暂让出 CPU 后重试，避免日志/事件管道在瞬时高峰时直接丢消息。
                 */
                sleep_ms(10);
                continue;
            }
            return -1;
        }
        if (r == 0) {
            continue;
        }
        p += r;
        left -= (size_t)r;
    }

    return (ssize_t)n;
}

ssize_t read_full(int fd, void *buf, size_t n)
{
    ssize_t r = readn(fd, buf, n);
    return r == (ssize_t)n ? r : -1;
}

ssize_t write_full(int fd, const void *buf, size_t n)
{
    return writen(fd, buf, n);
}

void init_sync_header(SyncHeader *header, uint32_t msg_type)
{
    memset(header, 0, sizeof(*header));
    header->magic = PROTOCOL_MAGIC;
    header->version = PROTOCOL_VERSION;
    header->msg_type = msg_type;
}

int validate_sync_header(const SyncHeader *header)
{
    if (header->magic != PROTOCOL_MAGIC) {
        return -1;
    }
    if (header->version != PROTOCOL_VERSION) {
        return -1;
    }
    if (header->path_len > MAX_NET_PATH) {
        return -1;
    }
    return 0;
}

const char *msg_type_to_string(uint32_t msg_type)
{
    switch (msg_type) {
    case MSG_HELLO:
        return "HELLO";
    case MSG_FILE_BEGIN:
        return "FILE_BEGIN";
    case MSG_FILE_BLOCK:
        return "FILE_BLOCK";
    case MSG_FILE_END:
        return "FILE_END";
    case MSG_FILE_DELETE:
        return "FILE_DELETE";
    case MSG_FILE_RENAME:
        return "FILE_RENAME";
    case MSG_QUERY_OFFSET:
        return "QUERY_OFFSET";
    case MSG_OFFSET_REPLY:
        return "OFFSET_REPLY";
    case MSG_ACK:
        return "ACK";
    case MSG_ERROR:
        return "ERROR";
    case MSG_EXIT:
        return "EXIT";
    default:
        return "UNKNOWN";
    }
}

int send_header(int fd, const SyncHeader *header)
{
    return write_full(fd, header, sizeof(*header)) == (ssize_t)sizeof(*header) ? 0 : -1;
}

int recv_header(int fd, SyncHeader *header)
{
    if (read_full(fd, header, sizeof(*header)) < 0) {
        return -1;
    }
    return validate_sync_header(header);
}

int send_path(int fd, const char *rel_path)
{
    size_t len = strlen(rel_path);
    if (len > MAX_NET_PATH) {
        return -1;
    }
    return write_full(fd, rel_path, len) == (ssize_t)len ? 0 : -1;
}

int recv_path(int fd, char *rel_path, size_t max_len, uint32_t path_len)
{
    if (path_len >= max_len || path_len > MAX_NET_PATH) {
        return -1;
    }
    if (path_len == 0) {
        rel_path[0] = '\0';
        return 0;
    }
    if (read_full(fd, rel_path, path_len) < 0) {
        return -1;
    }
    rel_path[path_len] = '\0';
    return 0;
}

int create_client_socket(const char *server_ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int create_server_socket(int port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int send_ack(int fd, uint64_t offset)
{
    SyncHeader header;
    init_sync_header(&header, MSG_ACK);
    header.offset = offset;
    return send_header(fd, &header);
}

int send_error(int fd, const char *message)
{
    SyncHeader header;
    init_sync_header(&header, MSG_ERROR);
    header.path_len = (uint32_t)strlen(message);
    if (header.path_len > MAX_NET_PATH) {
        header.path_len = MAX_NET_PATH;
    }
    if (send_header(fd, &header) < 0) {
        return -1;
    }
    return write_full(fd, message, header.path_len) == (ssize_t)header.path_len ? 0 : -1;
}
