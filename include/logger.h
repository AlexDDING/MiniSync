#ifndef MINISYNC_LOGGER_H
#define MINISYNC_LOGGER_H

#include "common.h"

#define LOG_INFO 1
#define LOG_WARN 2
#define LOG_ERROR 3
#define LOG_DEBUG 4
#define LOG_EXIT 99

typedef struct {
    pid_t pid;
    int level;
    char message[MAX_LOG_LEN];
} LogMessage;

const char *log_level_to_string(int level);
void logger_process(int log_read_fd, const char *log_path);
void log_message(int log_fd, int level, const char *fmt, ...);
void log_message_v(int log_fd, int level, const char *fmt, va_list ap);
int write_log_record(FILE *fp, const LogMessage *msg);

#endif
