#ifndef MINISYNC_NET_UTILS_H
#define MINISYNC_NET_UTILS_H

#include <netinet/in.h>
#include "protocol.h"

ssize_t readn(int fd, void *buf, size_t n);
ssize_t writen(int fd, const void *buf, size_t n);
ssize_t read_full(int fd, void *buf, size_t n);
ssize_t write_full(int fd, const void *buf, size_t n);
int send_header(int fd, const SyncHeader *header);
int recv_header(int fd, SyncHeader *header);
int send_path(int fd, const char *rel_path);
int recv_path(int fd, char *rel_path, size_t max_len, uint32_t path_len);
int create_client_socket(const char *server_ip, int port);
int create_server_socket(int port, int backlog);
int send_ack(int fd, uint64_t offset);
int send_error(int fd, const char *message);

#endif
