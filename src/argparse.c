/**
 * NEURON IIoT System for Industry 4.0
 * Copyright (C) 2020-2022 EMQ Technologies Co., Ltd All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 **/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zlog.h>

#include "argparse.h"
#include "persist/persist.h"
#include "version.h"
#include "json/json.h"
#include "json/neu_json_param.h"

#define OPTIONAL_ARGUMENT_IS_PRESENT                             \
    ((optarg == NULL && optind < argc && argv[optind][0] != '-') \
         ? (bool) (optarg = argv[optind++])                      \
         : (optarg != NULL))

#define STRDUP(var, str)                                                       \
    do {                                                                       \
        (var) = strdup(str);                                                   \
        if (NULL == (var)) {                                                   \
            fprintf(                                                           \
                stderr,                                                        \
                "Neuron argument parser fail strdup string: %s, reason: %s\n", \
                (str), strerror(errno));                                       \
            goto quit;                                                         \
        }                                                                      \
    } while (0)

const char *g_config_dir = NULL;
const char *g_plugin_dir = NULL;

// clang-format off
const char *usage_text =
"USAGE:\n"
"    neuron [OPTIONS]\n\n"
"OPTIONS:\n"
"    -d, --daemon         run as daemon process\n"
"    -h, --help           show this help message\n"
"    --stop               stop running neuron\n"
"    --log                log to the stdout\n"
"    --log_level <LEVEL>  default log level(DEBUG,NOTICE)\n"
"    --reset-password     reset dashboard to use default password\n"
"    --restart <POLICY>   restart policy to apply when neuron daemon terminates,\n"
"                           - never,      never restart (default)\n"
"                           - always,     always restart\n"
"                           - on-failure, restart only if failure\n"
"                           - NUMBER,     restart max NUMBER of times\n"
"    --version            print version information\n"
"    --disable_auth       disable http api auth\n"
"    --config_file <PATH> startup parameter configuration file\n"
"    --config_dir <DIR>   directory from which neuron reads configuration\n"
"    --plugin_dir <DIR>   directory from which neuron loads plugin lib files\n"
"\n";
// clang-format on

static inline void usage()
{
    fprintf(stderr, "%s", usage_text);
}

static inline void version()
{
    printf("Neuron %s (%s %s)\n", NEURON_VERSION,
           NEURON_GIT_REV NEURON_GIT_DIFF, NEURON_BUILD_DATE);
}

static inline int reset_password()
{
    neu_persist_user_info_t info = {
        .name = "admin",
        .hash =
            "$5$PwFeXpBBIBZuZdZl$fP8fFPWCLoaWcnVXVSR.3Xi8TEqCvX92gjhowNNn6S4",
    };

    int rv = neu_persister_create(g_config_dir);
    if (0 != rv) {
        return rv;
    }

    rv = neu_persister_update_user(&info);
    return rv;
}

static inline size_t parse_restart_policy(const char *s, size_t *out)
{
    if (0 == strcmp(s, "always")) {
        *out = NEU_RESTART_ALWAYS;
    } else if (0 == strcmp(s, "never")) {
        *out = NEU_RESTART_NEVER;
    } else if (0 == strcmp(s, "on-failure")) {
        *out = NEU_RESTART_ONFAILURE;
    } else {
        errno         = 0;
        char *    end = NULL;
        uintmax_t n   = strtoumax(s, &end, 0);
        // the entire string should be a number within range
        if (0 != errno || '\0' == *s || '\0' != *end ||
            n > NEU_RESTART_ALWAYS) {
            return -1;
        }
        *out = n;
    }

    return 0;
}

static inline bool file_exists(const char *const path)
{
    struct stat buf = { 0 };
    return -1 != stat(path, &buf);
}

static inline int load_env(neu_cli_args_t *args, char **log_level_out,
                           char **config_dir_out, char **plugin_dir_out)
{

    int   ret    = 0;
    char *daemon = getenv(NEU_ENV_DAEMON);
    if (daemon != NULL) {
        if (strcmp(daemon, "1") == 0) {
            args->daemonized = true;
        } else {
            args->daemonized = false;
        }
    }

    char *log = getenv(NEU_ENV_LOG);
    if (log != NULL) {
        if (strcmp(log, "1") == 0) {
            args->dev_log = true;
        } else {
            args->dev_log = false;
        }
    }

    char *log_level = getenv(NEU_ENV_LOG_LEVEL);
    if (log_level != NULL) {
        if (*log_level_out != NULL) {
            free(*log_level_out);
        }
        *log_level_out = strdup(log_level);
    }

    char *restart = getenv(NEU_ENV_RESTART);
    if (restart != NULL) {
        int t = parse_restart_policy(restart, &args->restart);
        if (t < 0) {
            printf("neuron NEU_ENV_RESTART setting error!\n");
            ret = -1;
        }
    }

    char *disable_auth = getenv(NEU_ENV_DISABLE_AUTH);
    if (disable_auth != NULL) {
        if (strcmp(disable_auth, "1") == 0) {
            args->disable_auth = true;
        } else {
            args->disable_auth = false;
        }
    }

    char *config_dir = getenv(NEU_ENV_CONFIG_DIR);
    if (config_dir != NULL) {
        if (*config_dir_out != NULL) {
            free(*config_dir_out);
        }
        *config_dir_out = strdup(config_dir);
    }

    char *plugin_dir = getenv(NEU_ENV_PLUGIN_DIR);
    if (plugin_dir != NULL) {
        if (*plugin_dir_out != NULL) {
            free(*plugin_dir_out);
        }
        *plugin_dir_out = strdup(plugin_dir);
    }

    return ret;
}

static inline void resolve_config_file_path(int argc, char *argv[],
                                            struct option *long_options,
                                            char *opts, char **config_file)
{
    int c            = 0;
    int option_index = 0;
    while ((c = getopt_long(argc, argv, opts, long_options, &option_index)) !=
           -1) {
        switch (c) {
        case 'c':
            if (0 == strcmp("config_file", long_options[option_index].name)) {
                *config_file = strdup(optarg);
                goto end;
            }
            break;
        default:
            break;
        }
    }
end:
    optind = 0;
}

static inline int load_config_file(int argc, char *argv[],
                                   struct option *long_options, char *opts,
                                   neu_cli_args_t *args, char **log_level_out,
                                   char **config_dir_out, char **plugin_dir_out)
{
    char *config_file = NULL;
    int   ret         = -1;
    int   fd          = -1;
    void *root        = NULL;

    resolve_config_file_path(argc, argv, long_options, opts, &config_file);

    do {
        if (!config_file) {
            ret = 0;
            break;
        }

        if (!file_exists(config_file)) {
            fprintf(stderr, "configuration file `%s` not exists\n",
                    config_file);
            break;
        }

        char buf[512] = { 0 };
        fd            = open(config_file, O_RDONLY);
        if (fd < 0) {
            printf("cannot open %s reason: %s\n", config_file, strerror(errno));
            break;
        }
        int size = read(fd, buf, sizeof(buf) - 1);
        if (size <= 0) {
            printf("cannot read %s reason: %s\n", config_file, strerror(errno));
            break;
        }

        root = neu_json_decode_new(buf);
        if (root == NULL) {
            printf("config file %s foramt error!\n", config_file);
            break;
        }

        neu_json_elem_t restart = { .name = "restart", .t = NEU_JSON_STR };
        ret                     = neu_json_decode_value(root, &restart);
        if (ret == 0) {
            ret = parse_restart_policy(restart.v.val_str, &args->restart);
            if (ret != 0) {
                printf("config file restart is invalid policy!\n");
                free(restart.v.val_str);
                ret = -1;
                break;
            }
        }

        neu_json_elem_t daemon = { .name = "daemon", .t = NEU_JSON_INT };
        ret                    = neu_json_decode_value(root, &daemon);
        if (ret == 0) {
            if (daemon.v.val_int != 0) {
                args->daemonized = true;
            } else {
                args->daemonized = false;
            }
        }

        neu_json_elem_t log = { .name = "log", .t = NEU_JSON_INT };
        ret                 = neu_json_decode_value(root, &log);
        if (ret == 0) {
            if (log.v.val_int != 0) {
                args->dev_log = true;
            } else {
                args->dev_log = false;
            }
        }

        neu_json_elem_t log_level = { .name = "log_level", .t = NEU_JSON_STR };
        ret                       = neu_json_decode_value(root, &log_level);
        if (ret == 0) {
            if (*log_level_out != NULL) {
                free(*log_level_out);
            }
            *log_level_out = strdup(log_level.v.val_str);
        }

        neu_json_elem_t disable_auth = { .name = "disable_auth",
                                         .t    = NEU_JSON_INT };
        ret = neu_json_decode_value(root, &disable_auth);
        if (ret == 0) {
            if (disable_auth.v.val_int != 0) {
                args->disable_auth = true;
            } else {
                args->disable_auth = false;
            }
        }

        neu_json_elem_t config_dir = { .name = "config_dir",
                                       .t    = NEU_JSON_STR };
        ret                        = neu_json_decode_value(root, &config_dir);
        if (ret == 0) {
            if (*config_dir_out != NULL) {
                free(*config_dir_out);
            }
            *config_dir_out = strdup(config_dir.v.val_str);
        }

        neu_json_elem_t plugin_dir = { .name = "plugin_dir",
                                       .t    = NEU_JSON_STR };
        ret                        = neu_json_decode_value(root, &plugin_dir);
        if (ret == 0) {
            if (*plugin_dir_out != NULL) {
                free(*plugin_dir_out);
            }
            *plugin_dir_out = strdup(plugin_dir.v.val_str);
        }

        free(log_level.v.val_str);
        free(restart.v.val_str);
        free(config_dir.v.val_str);
        free(plugin_dir.v.val_str);
        ret = 0;

    } while (0);

    if (config_file) {
        free(config_file);
    }

    if (fd) {
        close(fd);
    }

    if (root) {
        neu_json_decode_free(root);
    }

    return ret;
}

void neu_cli_args_init(neu_cli_args_t *args, int argc, char *argv[])
{
    int           ret                 = 0;
    bool          reset_password_flag = false;
    char *        config_dir          = NULL;
    char *        plugin_dir          = NULL;
    char *        log_level           = NULL;
    char *        opts                = "dh";
    struct option long_options[]      = {
        { "help", no_argument, NULL, 'h' },
        { "daemon", no_argument, NULL, 'd' },
        { "log", no_argument, NULL, 'l' },
        { "log_level", required_argument, NULL, 'o' },
        { "reset-password", no_argument, NULL, 'r' },
        { "restart", required_argument, NULL, 'r' },
        { "version", no_argument, NULL, 'v' },
        { "disable_auth", no_argument, NULL, 'a' },
        { "config_file", required_argument, NULL, 'c' },
        { "config_dir", required_argument, NULL, 'c' },
        { "plugin_dir", required_argument, NULL, 'p' },
        { "stop", no_argument, NULL, 's' },
        { NULL, 0, NULL, 0 },
    };

    memset(args, 0, sizeof(*args));

    int c            = 0;
    int option_index = 0;

    // load config file
    if (load_config_file(argc, argv, long_options, opts, args, &log_level,
                         &config_dir, &plugin_dir) < 0) {
        ret = 1;
        goto quit;
    }

    // load env
    if (load_env(args, &log_level, &config_dir, &plugin_dir) < 0) {
        ret = 1;
        goto quit;
    }

    while ((c = getopt_long(argc, argv, opts, long_options, &option_index)) !=
           -1) {
        switch (c) {
        case 'h':
            usage();
            goto quit;
        case 'd':
            args->daemonized = true;
            break;
        case ':':
            usage();
            goto quit;
        case 'l':
            args->dev_log = true;
            break;
        case 'o':
            if (log_level != NULL) {
                free(log_level);
            }
            log_level = strdup(optarg);
            break;
        case 'r':
            if (0 ==
                strcmp("reset-password", long_options[option_index].name)) {
                reset_password_flag = true;
            } else if (0 != parse_restart_policy(optarg, &args->restart)) {
                fprintf(stderr, "%s: option '--restart' invalid policy: `%s`\n",
                        argv[0], optarg);
                ret = 1;
                goto quit;
            }
            break;
        case 'v':
            version();
            goto quit;
        case 'a':
            args->disable_auth = true;
            break;
        case 'c':
            if (0 == strcmp("config_dir", long_options[option_index].name)) {
                if (config_dir != NULL) {
                    free(config_dir);
                }
                config_dir = strdup(optarg);
            }
            break;
        case 'p':
            if (plugin_dir != NULL) {
                free(plugin_dir);
            }
            plugin_dir = strdup(optarg);
            break;
        case 's':
            args->stop = true;
            break;
        case '?':
        default:
            usage();
            ret = 1;
            goto quit;
        }
    }

    if (!args->daemonized && args->restart != NEU_RESTART_NEVER) {
        fprintf(stderr,
                "%s: option '--restart' has no effects without '--daemon'\n",
                argv[0]);
        args->restart = NEU_RESTART_NEVER;
    }

    args->config_dir = config_dir ? config_dir : strdup("./config");
    if (!file_exists(args->config_dir)) {
        fprintf(stderr, "configuration directory `%s` not exists\n",
                args->config_dir);
        ret = 1;
        goto quit;
    }

    args->plugin_dir = plugin_dir ? plugin_dir : strdup("./plugins");
    if (!file_exists(args->plugin_dir)) {
        fprintf(stderr, "plugin directory `%s` not exists\n", args->plugin_dir);
        ret = 1;
        goto quit;
    }

    const char *zlog_conf = args->dev_log ? "dev.conf" : "zlog.conf";
    int         n = 1 + snprintf(NULL, 0, "%s/%s", args->config_dir, zlog_conf);
    char *      log_init_file = malloc(n);
    snprintf(log_init_file, n, "%s/%s", args->config_dir, zlog_conf);
    args->log_init_file = log_init_file;
    if (!file_exists(args->log_init_file)) {
        fprintf(stderr, "log init file `%s` not exists\n", args->log_init_file);
        ret = 1;
        goto quit;
    }

    // passing information by global variable is not a good style
    g_config_dir = args->config_dir;
    g_plugin_dir = args->plugin_dir;

    if (reset_password_flag) {
        ret = reset_password();
        goto quit;
    }

    if (log_level != NULL) {
        if (strcmp(log_level, "DEBUG") == 0) {
            default_log_level = ZLOG_LEVEL_DEBUG;
        }
        if (strcmp(log_level, "NOTICE") == 0) {
            default_log_level = ZLOG_LEVEL_NOTICE;
        }
        free(log_level);
    }

    return;

quit:
    neu_cli_args_fini(args);
    exit(ret);
}

void neu_cli_args_fini(neu_cli_args_t *args)
{
    if (args) {
        free(args->log_init_file);
        free(args->config_dir);
        free(args->plugin_dir);
    }
}
