#include "common.h"
#include "file_utils.h"
#include "logger.h"
#include "monitor.h"
#include "net_utils.h"
#include "task.h"
#include "transfer.h"

#include <sys/wait.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void print_client_help(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --mode local --src ./test_dir --dst ./backup_dir [options]\n", prog);
    printf("  %s --mode remote --src ./test_dir --server 127.0.0.1 --port 9000 [options]\n", prog);
    printf("\nOptions:\n");
    printf("  --threads N              worker thread count, default 4\n");
    printf("  --block-size N           transfer block size, default 4096\n");
    printf("  --hash-threshold N       large-file threshold, default 10485760\n");
    printf("  --debounce-ms N          modify debounce delay, default 1500\n");
    printf("  --log PATH               log path, default ./logs/client.log\n");
    printf("  --help                   show this help\n");
}

static void init_default_config(ClientConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->mode = 0;
    config->port = DEFAULT_PORT;
    config->thread_count = DEFAULT_THREAD_COUNT;
    config->block_size = DEFAULT_BLOCK_SIZE;
    config->hash_threshold = DEFAULT_HASH_THRESHOLD;
    config->debounce_ms = DEFAULT_DEBOUNCE_MS;
    snprintf(config->server_ip, sizeof(config->server_ip), "127.0.0.1");
    snprintf(config->log_path, sizeof(config->log_path), "%s", DEFAULT_CLIENT_LOG);
}

static int next_arg(int argc, char **argv, int *i, const char **out)
{
    if (*i + 1 >= argc) {
        return -1;
    }
    *out = argv[++(*i)];
    return 0;
}

static int parse_client_args(int argc, char **argv, ClientConfig *config)
{
    init_default_config(config);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "--help") == 0) {
            return 1;
        } else if (strcmp(arg, "--mode") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            if (strcmp(value, "local") == 0) {
                config->mode = MODE_LOCAL;
            } else if (strcmp(value, "remote") == 0) {
                config->mode = MODE_REMOTE;
            } else {
                return -1;
            }
        } else if (strcmp(arg, "--src") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->src_root, sizeof(config->src_root), "%s", value) >= (int)sizeof(config->src_root)) {
                return -1;
            }
        } else if (strcmp(arg, "--dst") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->dst_root, sizeof(config->dst_root), "%s", value) >= (int)sizeof(config->dst_root)) {
                return -1;
            }
        } else if (strcmp(arg, "--server") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->server_ip, sizeof(config->server_ip), "%s", value) >= (int)sizeof(config->server_ip)) {
                return -1;
            }
        } else if (strcmp(arg, "--port") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->port = atoi(value);
        } else if (strcmp(arg, "--threads") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->thread_count = atoi(value);
        } else if (strcmp(arg, "--block-size") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->block_size = atoi(value);
        } else if (strcmp(arg, "--hash-threshold") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->hash_threshold = (off_t)atoll(value);
        } else if (strcmp(arg, "--debounce-ms") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->debounce_ms = atoi(value);
        } else if (strcmp(arg, "--log") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->log_path, sizeof(config->log_path), "%s", value) >= (int)sizeof(config->log_path)) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (config->mode != MODE_LOCAL && config->mode != MODE_REMOTE) {
        return -1;
    }
    if (config->src_root[0] == '\0') {
        return -1;
    }
    if (config->mode == MODE_LOCAL && config->dst_root[0] == '\0') {
        return -1;
    }
    if (config->mode == MODE_REMOTE && (config->server_ip[0] == '\0' || config->port <= 0)) {
        return -1;
    }
    if (config->thread_count <= 0 || config->block_size <= 0 || config->debounce_ms <= 0 || config->hash_threshold <= 0) {
        return -1;
    }

    return 0;
}

static void send_exit_task(int event_fd)
{
    SyncTask task;
    init_sync_task(&task);
    task.event_type = EVENT_EXIT;
    (void)write_full(event_fd, &task, sizeof(task));
}

int main(int argc, char **argv)
{
    ClientConfig config;
    int parse_rc = parse_client_args(argc, argv, &config);
    if (parse_rc != 0) {
        print_client_help(argv[0]);
        return parse_rc > 0 ? 0 : 1;
    }

    if (ensure_dir_exists(config.src_root) < 0 ||
        ensure_parent_dir_exists(config.log_path) < 0 ||
        (config.mode == MODE_LOCAL && ensure_dir_exists(config.dst_root) < 0)) {
        fprintf(stderr, "failed to prepare directories: %s\n", strerror(errno));
        return 1;
    }

    int event_pipe[2];
    int log_pipe[2];
    if (pipe(event_pipe) < 0 || pipe(log_pipe) < 0) {
        perror("pipe");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    pid_t logger_pid = fork();
    if (logger_pid < 0) {
        perror("fork logger");
        return 1;
    }
    if (logger_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        close(event_pipe[0]);
        close(event_pipe[1]);
        close(log_pipe[1]);
        logger_process(log_pipe[0], config.log_path);
    }

    pid_t monitor_pid = fork();
    if (monitor_pid < 0) {
        perror("fork monitor");
        return 1;
    }
    if (monitor_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        close(event_pipe[0]);
        close(log_pipe[0]);
        monitor_process(config.src_root, event_pipe[1], log_pipe[1]);
    }

    pid_t transfer_pid = fork();
    if (transfer_pid < 0) {
        perror("fork transfer");
        return 1;
    }
    if (transfer_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        close(event_pipe[1]);
        close(log_pipe[0]);
        transfer_process(&config, event_pipe[0], log_pipe[1]);
    }

    close(event_pipe[0]);
    close(log_pipe[0]);

    log_message(log_pipe[1], LOG_INFO, "main: MiniSync client started mode=%s src=%s",
                config.mode == MODE_LOCAL ? "local" : "remote", config.src_root);

    while (!g_stop) {
        sleep_ms(200);
        int status;
        pid_t done = waitpid(-1, &status, WNOHANG);
        if (done == monitor_pid || done == transfer_pid) {
            g_stop = 1;
        }
    }

    log_message(log_pipe[1], LOG_INFO, "main: stopping client");
    send_exit_task(event_pipe[1]);
    kill(monitor_pid, SIGTERM);

    int status;
    waitpid(monitor_pid, &status, 0);
    waitpid(transfer_pid, &status, 0);

    log_message(log_pipe[1], LOG_EXIT, "main: logger exit");
    close(event_pipe[1]);
    close(log_pipe[1]);
    waitpid(logger_pid, &status, 0);

    return 0;
}
