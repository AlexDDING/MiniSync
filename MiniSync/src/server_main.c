#include "common.h"
#include "file_utils.h"
#include "server.h"

static void print_server_help(const char *prog)
{
    printf("Usage:\n");
    printf("  %s --port 9000 --root ./server_backup [--log ./logs/server.log]\n", prog);
}

static void init_default_server_config(ServerConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->port = DEFAULT_PORT;
    snprintf(config->root, sizeof(config->root), "./server_backup");
    snprintf(config->log_path, sizeof(config->log_path), "%s", DEFAULT_SERVER_LOG);
}

static int next_arg(int argc, char **argv, int *i, const char **out)
{
    if (*i + 1 >= argc) {
        return -1;
    }
    *out = argv[++(*i)];
    return 0;
}

static int parse_server_args(int argc, char **argv, ServerConfig *config)
{
    init_default_server_config(config);

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = NULL;

        if (strcmp(arg, "--help") == 0) {
            return 1;
        } else if (strcmp(arg, "--port") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0) {
                return -1;
            }
            config->port = atoi(value);
        } else if (strcmp(arg, "--root") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->root, sizeof(config->root), "%s", value) >= (int)sizeof(config->root)) {
                return -1;
            }
        } else if (strcmp(arg, "--log") == 0) {
            if (next_arg(argc, argv, &i, &value) < 0 ||
                snprintf(config->log_path, sizeof(config->log_path), "%s", value) >= (int)sizeof(config->log_path)) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    if (config->port <= 0 || config->root[0] == '\0' || config->log_path[0] == '\0') {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    ServerConfig config;
    int parse_rc = parse_server_args(argc, argv, &config);
    if (parse_rc != 0) {
        print_server_help(argv[0]);
        return parse_rc > 0 ? 0 : 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (ensure_dir_exists(config.root) < 0 || ensure_parent_dir_exists(config.log_path) < 0) {
        fprintf(stderr, "failed to prepare server directories: %s\n", strerror(errno));
        return 1;
    }

    return run_server(&config) == 0 ? 0 : 1;
}
