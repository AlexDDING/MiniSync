#ifndef MINISYNC_HASH_H
#define MINISYNC_HASH_H

#include "common.h"

#define FNV1A_64_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV1A_64_PRIME UINT64_C(1099511628211)

uint64_t hash_buffer_fnv1a(const void *data, size_t len);
uint64_t hash_file_fnv1a(const char *path);
uint64_t hash_file_range_fnv1a(const char *path, off_t offset, size_t len);
void hash_to_hex(uint64_t hash, char out[HASH_STR_LEN]);

#endif
