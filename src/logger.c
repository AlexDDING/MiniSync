#include "logger.h"
#include "net_utils.h"

const char *log_level_to_string(int level)
{
    switch (level) {
    case LOG_INFO:
        return "INFO";
    case LOG_WARN:
        return "WARN";
    case LOG_ERROR:
        return "ERROR";
    case LOG_DEBUG:
        return "DEBUG";
    case LOG_EXIT:
        return "EXIT";
    default:
        return "UNKNOWN";
    }
}

static void format_time(char *out, size_t out_size)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm_now);
}

int write_log_record(FILE *fp, const LogMessage *msg)
{
    char ts[32];
    format_time(ts, sizeof(ts));

    int r = fprintf(fp, "[%s] [%s] [pid=%ld] %s\n",
                    ts,
                    log_level_to_string(msg->level),
                    (long)msg->pid,
                    msg->message);
    fflush(fp);
    return r < 0 ? -1 : 0;
}

void logger_process(int log_read_fd, const char *log_path)
{
    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        fp = stdout;
    }

    while (1) {
        LogMessage msg;
        ssize_t n = readn(log_read_fd, &msg, sizeof(msg));
        if (n == 0) {
            break;
        }
        if (n != (ssize_t)sizeof(msg)) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        write_log_record(fp, &msg);
        if (fp != stdout) {
            write_log_record(stdout, &msg);
        }

        if (msg.level == LOG_EXIT) {
            break;
        }
    }

    if (fp && fp != stdout) {
        fclose(fp);
    }
    close(log_read_fd);
    _exit(0);
}

void log_message_v(int log_fd, int level, const char *fmt, va_list ap)
{
    if (log_fd < 0) {
        return;
    }

    LogMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.pid = getpid();
    msg.level = level;
    vsnprintf(msg.message, sizeof(msg.message), fmt, ap);

    /*
     * LogMessage 小于常见 Linux PIPE_BUF，单次 write 通常具备原子性。
     * 仍然使用 write_full 循环写入，兜底处理 EINTR 和非阻塞管道写满。
     */
    (void)write_full(log_fd, &msg, sizeof(msg));
}

void log_message(int log_fd, int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_message_v(log_fd, level, fmt, ap);
    va_end(ap);
}
