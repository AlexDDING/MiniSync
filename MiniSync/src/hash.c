#include "hash.h"

#include <unistd.h>

uint64_t hash_buffer_fnv1a(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint64_t hash = FNV1A_64_OFFSET_BASIS;

    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)p[i];
        hash *= FNV1A_64_PRIME;
    }

    return hash;
}

static uint64_t hash_fd_range(int fd, off_t offset, size_t len, int limited)
{
    unsigned char buf[8192];
    uint64_t hash = FNV1A_64_OFFSET_BASIS;

    if (offset > 0 && lseek(fd, offset, SEEK_SET) < 0) {
        return 0;
    }

    while (1) {
        size_t want = sizeof(buf);
        if (limited) {
            if (len == 0) {
                break;
            }
            if (len < want) {
                want = len;
            }
        }

        ssize_t n = read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (n == 0) {
            break;
        }

        for (ssize_t i = 0; i < n; ++i) {
            hash ^= (uint64_t)buf[i];
            hash *= FNV1A_64_PRIME;
        }

        if (limited) {
            len -= (size_t)n;
        }
    }

    return hash;
}

uint64_t hash_file_fnv1a(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    uint64_t hash = hash_fd_range(fd, 0, 0, 0);
    close(fd);
    return hash;
}

uint64_t hash_file_range_fnv1a(const char *path, off_t offset, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    uint64_t hash = hash_fd_range(fd, offset, len, 1);
    close(fd);
    return hash;
}

void hash_to_hex(uint64_t hash, char out[HASH_STR_LEN])
{
    snprintf(out, HASH_STR_LEN, "%016" PRIx64, hash);
}
