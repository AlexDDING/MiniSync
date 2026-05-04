#ifndef MINISYNC_FILE_UTILS_H
#define MINISYNC_FILE_UTILS_H

#include "common.h"

int ensure_dir_exists(const char *dir);
int ensure_parent_dir_exists(const char *file_path);
int join_path(char *out, size_t out_size, const char *base, const char *rel);
int get_relative_path(char *out, size_t out_size, const char *root, const char *path);
int sanitize_rel_path(const char *rel_path);
int copy_file_atomic(const char *src, const char *dst);
int copy_file_with_hash_check(const char *src, const char *dst);
int set_file_metadata(const char *path, mode_t mode, time_t mtime);
int is_regular_file(const char *path);
int is_directory(const char *path);
int get_file_size(const char *path, off_t *size);
int backup_old_version(const char *backup_root, const char *rel_path);
int move_to_trash(const char *backup_root, const char *rel_path);
int safe_rename_or_copy_unlink(const char *src, const char *dst);
int try_read_lock(int fd);
void unlock_file(int fd);
int make_timestamp_dirname(char *out, size_t out_size);

#endif
