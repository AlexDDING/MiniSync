#include "file_utils.h"
#include "hash.h"

#include <dirent.h>
#include <libgen.h>
#include <sys/time.h>
#include <utime.h>

int sanitize_rel_path(const char *rel_path)
{
    if (!rel_path || rel_path[0] == '\0') {
        return -1;
    }
    if (rel_path[0] == '/') {
        return -1;
    }
    if (strcmp(rel_path, ".") == 0 || strcmp(rel_path, "..") == 0) {
        return -1;
    }
    if (strstr(rel_path, "../") || strstr(rel_path, "/..") || strstr(rel_path, "//")) {
        return -1;
    }
    return 0;
}

int join_path(char *out, size_t out_size, const char *base, const char *rel)
{
    if (!out || !base || !rel || out_size == 0) {
        return -1;
    }

    while (*rel == '/') {
        rel++;
    }

    int n;
    size_t base_len = strlen(base);
    if (base_len > 0 && base[base_len - 1] == '/') {
        n = snprintf(out, out_size, "%s%s", base, rel);
    } else {
        n = snprintf(out, out_size, "%s/%s", base, rel);
    }
    return n > 0 && (size_t)n < out_size ? 0 : -1;
}

int ensure_dir_exists(const char *dir)
{
    if (!dir || dir[0] == '\0') {
        return -1;
    }

    char tmp[MAX_PATH_LEN];
    if (snprintf(tmp, sizeof(tmp), "%s", dir) >= (int)sizeof(tmp)) {
        return -1;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ensure_parent_dir_exists(const char *file_path)
{
    char tmp[MAX_PATH_LEN];
    if (!file_path || snprintf(tmp, sizeof(tmp), "%s", file_path) >= (int)sizeof(tmp)) {
        return -1;
    }

    char *slash = strrchr(tmp, '/');
    if (!slash) {
        return 0;
    }
    if (slash == tmp) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return ensure_dir_exists(tmp);
}

int get_relative_path(char *out, size_t out_size, const char *root, const char *path)
{
    if (!out || !root || !path || out_size == 0) {
        return -1;
    }

    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') {
        root_len--;
    }

    if (strncmp(root, path, root_len) != 0) {
        return -1;
    }

    const char *rel = path + root_len;
    if (*rel == '/') {
        rel++;
    }
    if (*rel == '\0') {
        rel = ".";
    }

    if (snprintf(out, out_size, "%s", rel) >= (int)out_size) {
        return -1;
    }
    return sanitize_rel_path(out);
}

int is_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int is_directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int get_file_size(const char *path, off_t *size)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        return -1;
    }
    if (size) {
        *size = st.st_size;
    }
    return 0;
}

static int copy_file_plain(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }

    if (ensure_parent_dir_exists(dst) < 0) {
        close(in);
        return -1;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buf[8192];
    int ok = 0;
    while (1) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = -1;
            break;
        }
        if (r == 0) {
            break;
        }

        char *p = buf;
        ssize_t left = r;
        while (left > 0) {
            ssize_t w = write(out, p, (size_t)left);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = -1;
                break;
            }
            p += w;
            left -= w;
        }
        if (ok < 0) {
            break;
        }
    }

    if (fsync(out) < 0) {
        ok = -1;
    }
    close(out);
    close(in);
    return ok;
}

int safe_rename_or_copy_unlink(const char *src, const char *dst)
{
    if (ensure_parent_dir_exists(dst) < 0) {
        return -1;
    }
    if (rename(src, dst) == 0) {
        return 0;
    }
    if (errno != EXDEV) {
        return -1;
    }

    /*
     * rename 跨文件系统时会返回 EXDEV，无法保证内核级原子移动。
     * 这里退化为完整复制、fsync、再 unlink 源文件，保证功能正确。
     */
    if (copy_file_plain(src, dst) < 0) {
        return -1;
    }
    return unlink(src);
}

int copy_file_atomic(const char *src, const char *dst)
{
    char tmp[MAX_PATH_LEN];
    if (snprintf(tmp, sizeof(tmp), "%s.syncing", dst) >= (int)sizeof(tmp)) {
        return -1;
    }

    if (copy_file_plain(src, tmp) < 0) {
        unlink(tmp);
        return -1;
    }
    if (safe_rename_or_copy_unlink(tmp, dst) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int copy_file_with_hash_check(const char *src, const char *dst)
{
    uint64_t before = hash_file_fnv1a(src);
    if (copy_file_atomic(src, dst) < 0) {
        return -1;
    }
    uint64_t after = hash_file_fnv1a(dst);
    return before == after ? 0 : -1;
}

int set_file_metadata(const char *path, mode_t mode, time_t mtime)
{
    chmod(path, mode & 07777);

    struct utimbuf times;
    times.actime = mtime;
    times.modtime = mtime;
    return utime(path, &times);
}

int make_timestamp_dirname(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return strftime(out, out_size, "%Y-%m-%d_%H-%M-%S", &tm_now) > 0 ? 0 : -1;
}

int backup_old_version(const char *backup_root, const char *rel_path)
{
    if (sanitize_rel_path(rel_path) < 0) {
        return -1;
    }

    char current_root[MAX_PATH_LEN];
    char src[MAX_PATH_LEN];
    if (join_path(current_root, sizeof(current_root), backup_root, "current") < 0 ||
        join_path(src, sizeof(src), current_root, rel_path) < 0) {
        return -1;
    }
    if (access(src, F_OK) != 0) {
        return 0;
    }

    char stamp[64];
    char versions_root[MAX_PATH_LEN];
    char version_dir[MAX_PATH_LEN];
    char dst[MAX_PATH_LEN];
    if (make_timestamp_dirname(stamp, sizeof(stamp)) < 0 ||
        join_path(versions_root, sizeof(versions_root), backup_root, ".versions") < 0 ||
        join_path(version_dir, sizeof(version_dir), versions_root, stamp) < 0 ||
        join_path(dst, sizeof(dst), version_dir, rel_path) < 0) {
        return -1;
    }

    if (is_directory(src)) {
        return ensure_dir_exists(dst);
    }
    return copy_file_atomic(src, dst);
}

int move_to_trash(const char *backup_root, const char *rel_path)
{
    if (sanitize_rel_path(rel_path) < 0) {
        return -1;
    }

    char current_root[MAX_PATH_LEN];
    char src[MAX_PATH_LEN];
    if (join_path(current_root, sizeof(current_root), backup_root, "current") < 0 ||
        join_path(src, sizeof(src), current_root, rel_path) < 0) {
        return -1;
    }
    if (access(src, F_OK) != 0) {
        return 0;
    }

    char stamp[64];
    char trash_root[MAX_PATH_LEN];
    char trash_dir[MAX_PATH_LEN];
    char dst[MAX_PATH_LEN];
    if (make_timestamp_dirname(stamp, sizeof(stamp)) < 0 ||
        join_path(trash_root, sizeof(trash_root), backup_root, ".trash") < 0 ||
        join_path(trash_dir, sizeof(trash_dir), trash_root, stamp) < 0 ||
        join_path(dst, sizeof(dst), trash_dir, rel_path) < 0) {
        return -1;
    }

    return safe_rename_or_copy_unlink(src, dst);
}

int try_read_lock(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &lock);
}

void unlock_file(int fd)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    (void)fcntl(fd, F_SETLK, &lock);
}
