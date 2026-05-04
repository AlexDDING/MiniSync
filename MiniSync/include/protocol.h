#ifndef MINISYNC_PROTOCOL_H
#define MINISYNC_PROTOCOL_H

#include "common.h"

#define PROTOCOL_MAGIC 0x4D53594E
#define PROTOCOL_VERSION 1

#define MSG_HELLO 1
#define MSG_FILE_BEGIN 2
#define MSG_FILE_BLOCK 3
#define MSG_FILE_END 4
#define MSG_FILE_DELETE 5
#define MSG_FILE_RENAME 6
#define MSG_QUERY_OFFSET 7
#define MSG_OFFSET_REPLY 8
#define MSG_ACK 9
#define MSG_ERROR 10
#define MSG_EXIT 99

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t msg_type;
    uint32_t path_len;
    uint64_t file_size;
    uint64_t offset;
    uint32_t data_len;
    uint32_t mode;
    int64_t mtime;
    uint64_t total_hash;
    uint64_t block_hash;
} SyncHeader;

void init_sync_header(SyncHeader *header, uint32_t msg_type);
int validate_sync_header(const SyncHeader *header);
const char *msg_type_to_string(uint32_t msg_type);

#endif
