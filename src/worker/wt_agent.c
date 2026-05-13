#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <hiredis/hiredis.h>
#include <jansson.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WT_DEFAULT_DB "/woventeam/woventeam.db"
#define WT_DEFAULT_REDIS_HOST "127.0.0.1"
#define WT_DEFAULT_REDIS_PORT 6379
#define WT_DEFAULT_TASK_CHANNEL "wt:tasks:new"
#define WT_DEFAULT_ACK_CHANNEL "wt:tasks:ack"
#define WT_DEFAULT_RESULT_CHANNEL "wt:tasks:result"
#define WT_DEFAULT_EVENT_CHANNEL "wt:events:log"
#define WT_DEFAULT_SLACK_WEBHOOK_FILE "/woventeam/config/slack_webhook.txt"
#define WT_DEFAULT_SLACK_WEBHOOK_KEY "integration"
#define WT_OUTPUT_CHUNK 4096
#define WT_OUTPUT_LIMIT (1024 * 1024)
#define WT_TASK_IGNORED 2

typedef struct {
    const char *agent;
    const char *db_path;
    const char *redis_host;
    int redis_port;
    const char *task_channel;
    const char *ack_channel;
    const char *result_channel;
    const char *event_channel;
    const char *slack_webhook_file;
    const char *slack_webhook_key;
    const char *task_json_path;
    bool once;
    bool no_redis;
    bool no_slack;
} wt_config;

typedef struct {
    char *data;
    size_t len;
    int exit_code;
    bool truncated;
} command_result;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static long now_epoch(void) {
    return (long)time(NULL);
}

static const char *env_default(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return (value && *value) ? value : fallback;
}

static void usage(FILE *stream, const char *argv0) {
    fprintf(stream,
            "Usage: %s [--agent NAME] [--once] [--task-json FILE] [--no-redis] [--no-slack]\n"
            "\n"
            "Environment defaults:\n"
            "  WOVENTEAM_DB=%s\n"
            "  WOVENTEAM_REDIS_HOST=%s\n"
            "  WOVENTEAM_REDIS_PORT=%d\n"
            "  WOVENTEAM_SLACK_WEBHOOK_FILE=%s\n"
            "  WOVENTEAM_SLACK_WEBHOOK_KEY=%s\n",
            argv0, WT_DEFAULT_DB, WT_DEFAULT_REDIS_HOST, WT_DEFAULT_REDIS_PORT,
            WT_DEFAULT_SLACK_WEBHOOK_FILE, WT_DEFAULT_SLACK_WEBHOOK_KEY);
}

static int parse_args(int argc, char **argv, wt_config *cfg) {
    cfg->agent = "claude";
    cfg->db_path = env_default("WOVENTEAM_DB", WT_DEFAULT_DB);
    cfg->redis_host = env_default("WOVENTEAM_REDIS_HOST", WT_DEFAULT_REDIS_HOST);
    cfg->redis_port = atoi(env_default("WOVENTEAM_REDIS_PORT", "6379"));
    if (cfg->redis_port <= 0) {
        cfg->redis_port = WT_DEFAULT_REDIS_PORT;
    }
    cfg->task_channel = env_default("WOVENTEAM_TASK_CHANNEL", WT_DEFAULT_TASK_CHANNEL);
    cfg->ack_channel = env_default("WOVENTEAM_ACK_CHANNEL", WT_DEFAULT_ACK_CHANNEL);
    cfg->result_channel = env_default("WOVENTEAM_RESULT_CHANNEL", WT_DEFAULT_RESULT_CHANNEL);
    cfg->event_channel = env_default("WOVENTEAM_EVENT_CHANNEL", WT_DEFAULT_EVENT_CHANNEL);
    cfg->slack_webhook_file = env_default("WOVENTEAM_SLACK_WEBHOOK_FILE", WT_DEFAULT_SLACK_WEBHOOK_FILE);
    cfg->slack_webhook_key = env_default("WOVENTEAM_SLACK_WEBHOOK_KEY", WT_DEFAULT_SLACK_WEBHOOK_KEY);
    cfg->task_json_path = NULL;
    cfg->once = false;
    cfg->no_redis = false;
    cfg->no_slack = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) {
            cfg->agent = argv[++i];
        } else if (strcmp(argv[i], "--once") == 0) {
            cfg->once = true;
        } else if (strcmp(argv[i], "--task-json") == 0 && i + 1 < argc) {
            cfg->task_json_path = argv[++i];
            cfg->once = true;
        } else if (strcmp(argv[i], "--no-redis") == 0) {
            cfg->no_redis = true;
        } else if (strcmp(argv[i], "--no-slack") == 0) {
            cfg->no_slack = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            exit(0);
        } else if (argv[i][0] != '-' && strcmp(cfg->agent, "claude") == 0) {
            cfg->agent = argv[i];
        } else {
            usage(stderr, argv[0]);
            return 2;
        }
    }
    return 0;
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = calloc((size_t)size + 1, 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size && ferror(fp)) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

static char *trim_inplace(char *s, size_t *out_len) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    if (out_len) *out_len = n;
    return s;
}

/* Read the Slack webhook URL for `key` from a config file that may hold
 * either a single bare URL or one-per-line KEY=VALUE pairs (e.g.
 * `integration=https://hooks.slack.com/...`). If no entry matches `key`,
 * falls back to the first line whose URL begins with
 * `https://hooks.slack.com/`. Returns a malloc'd string the caller must
 * free, or NULL when no usable URL was found. */
static char *read_slack_webhook(const char *path, const char *key) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    const char *URL_PREFIX = "https://hooks.slack.com/";
    char *line = NULL;
    size_t cap = 0;
    char *match = NULL;
    char *fallback = NULL;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) > 0) {
        size_t len = 0;
        char *trimmed = trim_inplace(line, &len);
        if (len == 0 || trimmed[0] == '#') continue;
        char *eq = strchr(trimmed, '=');
        if (eq) {
            *eq = '\0';
            char *k = trim_inplace(trimmed, NULL);
            char *v = trim_inplace(eq + 1, NULL);
            if (key && strcmp(k, key) == 0) {
                match = strdup(v);
                break;
            }
            if (!fallback && strncmp(v, URL_PREFIX, strlen(URL_PREFIX)) == 0) {
                fallback = strdup(v);
            }
        } else if (!fallback &&
                   strncmp(trimmed, URL_PREFIX, strlen(URL_PREFIX)) == 0) {
            fallback = strdup(trimmed);
        }
    }
    free(line);
    fclose(fp);
    if (match) {
        free(fallback);
        return match;
    }
    return fallback;
}

static int sqlite_set_status(const wt_config *cfg, const char *task_id, const char *status) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_open(cfg->db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite open failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    rc = sqlite3_prepare_v2(db, "UPDATE tasks SET status = ? WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite prepare failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "sqlite update failed: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    if (sqlite3_changes(db) != 1) {
        fprintf(stderr, "sqlite update failed: task %s did not match exactly one row\n", task_id);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

static int redis_publish(redisContext *redis, const char *channel, json_t *message) {
    if (!redis) {
        return 0;
    }
    char *encoded = json_dumps(message, JSON_COMPACT);
    if (!encoded) {
        return -1;
    }
    redisReply *reply = redisCommand(redis, "PUBLISH %s %s", channel, encoded);
    free(encoded);
    if (!reply) {
        fprintf(stderr, "redis publish failed: %s\n", redis->errstr);
        return -1;
    }
    freeReplyObject(reply);
    return 0;
}

static json_t *envelope(const char *id, const char *from, const char *type, const char *initiative) {
    json_t *root = json_object();
    json_object_set_new(root, "id", json_string(id ? id : ""));
    json_object_set_new(root, "ts", json_integer(now_epoch()));
    json_object_set_new(root, "from", json_string(from ? from : "unknown"));
    json_object_set_new(root, "type", json_string(type ? type : "log"));
    json_object_set_new(root, "initiative", json_string(initiative ? initiative : "phase0-spike"));
    return root;
}

static json_t *event_message(const char *agent, const char *task_id, const char *level, const char *text) {
    json_t *root = envelope(task_id ? task_id : "system", agent, "log", "phase0-spike");
    json_t *payload = json_object();
    json_object_set_new(payload, "level", json_string(level));
    json_object_set_new(payload, "message", json_string(text));
    json_object_set_new(root, "payload", payload);
    return root;
}

static void shell_quote(FILE *fp, const char *value) {
    fputc('\'', fp);
    for (const char *p = value; *p; p++) {
        if (*p == '\'') {
            fputs("'\\''", fp);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('\'', fp);
}

static int post_slack(const wt_config *cfg, const char *message) {
    if (cfg->no_slack) {
        return 0;
    }
    char *webhook = read_slack_webhook(cfg->slack_webhook_file,
                                       cfg->slack_webhook_key);
    if (!webhook) {
        return 0;
    }

    char tmp_template[] = "/tmp/wt-agent-slack-XXXXXX";
    int fd = mkstemp(tmp_template);
    if (fd < 0) {
        free(webhook);
        return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(tmp_template);
        free(webhook);
        return -1;
    }
    json_t *body = json_object();
    json_object_set_new(body, "text", json_string(message));
    json_dumpf(body, fp, JSON_COMPACT);
    json_decref(body);
    fclose(fp);

    char command[4096];
    FILE *cmd = fmemopen(command, sizeof(command), "w");
    if (!cmd) {
        unlink(tmp_template);
        free(webhook);
        return -1;
    }
    fputs("curl -fsS -X POST -H 'Content-type: application/json' --data @", cmd);
    shell_quote(cmd, tmp_template);
    fputc(' ', cmd);
    shell_quote(cmd, webhook);
    fclose(cmd);

    int rc = system(command);
    unlink(tmp_template);
    free(webhook);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        return -1;
    }
    return 0;
}

static int run_command(const char *command, command_result *result) {
    memset(result, 0, sizeof(*result));
    result->data = calloc(1, 1);
    if (!result->data) {
        return -1;
    }

    char *wrapped = NULL;
    size_t wrapped_len = strlen(command) + 16;
    wrapped = malloc(wrapped_len);
    if (!wrapped) {
        free(result->data);
        return -1;
    }
    snprintf(wrapped, wrapped_len, "%s 2>&1", command);

    FILE *pipe = popen(wrapped, "r");
    free(wrapped);
    if (!pipe) {
        free(result->data);
        return -1;
    }

    char chunk[WT_OUTPUT_CHUNK];
    while (!feof(pipe)) {
        size_t n = fread(chunk, 1, sizeof(chunk), pipe);
        if (n == 0) {
            break;
        }
        if (result->len < WT_OUTPUT_LIMIT) {
            size_t keep = n;
            if (result->len + keep > WT_OUTPUT_LIMIT) {
                keep = WT_OUTPUT_LIMIT - result->len;
                result->truncated = true;
            }
            char *next = realloc(result->data, result->len + keep + 1);
            if (!next) {
                pclose(pipe);
                free(result->data);
                return -1;
            }
            result->data = next;
            memcpy(result->data + result->len, chunk, keep);
            result->len += keep;
            result->data[result->len] = '\0';
            if (keep < n) {
                result->truncated = true;
            }
        } else {
            result->truncated = true;
        }
    }

    int rc = pclose(pipe);
    if (rc == -1) {
        result->exit_code = 127;
    } else if (WIFEXITED(rc)) {
        result->exit_code = WEXITSTATUS(rc);
    } else {
        result->exit_code = 128;
    }
    return 0;
}

static const char *json_string_at(json_t *root, const char *key, const char *fallback) {
    json_t *value = json_object_get(root, key);
    return json_is_string(value) ? json_string_value(value) : fallback;
}

static int process_task(const wt_config *cfg, redisContext *redis, const char *json_text) {
    json_error_t err;
    json_t *task = json_loads(json_text, 0, &err);
    if (!task) {
        fprintf(stderr, "invalid task json: %s\n", err.text);
        return -1;
    }

    json_t *payload = json_object_get(task, "payload");
    const char *assigned_to = json_string_at(payload, "assigned_to", "");
    if (strcmp(assigned_to, cfg->agent) != 0) {
        json_decref(task);
        return WT_TASK_IGNORED;
    }

    const char *task_id = json_string_at(task, "id", "");
    const char *initiative = json_string_at(task, "initiative", "phase0-spike");
    const char *command = json_string_at(payload, "command", "");
    if (!*task_id || !*command) {
        fprintf(stderr, "task missing id or payload.command\n");
        json_decref(task);
        return -1;
    }

    printf("[%s] executing task %s: %s\n", cfg->agent, task_id, command);
    if (sqlite_set_status(cfg, task_id, "in_progress") != 0) {
        json_decref(task);
        return -1;
    }

    json_t *ack = envelope(task_id, cfg->agent, "ack", initiative);
    json_t *ack_payload = json_object();
    json_object_set_new(ack_payload, "status", json_string("in_progress"));
    json_object_set_new(ack, "payload", ack_payload);
    redis_publish(redis, cfg->ack_channel, ack);
    json_decref(ack);

    command_result result;
    if (run_command(command, &result) != 0) {
        json_decref(task);
        return -1;
    }

    const char *status = result.exit_code == 0 ? "complete" : "error";
    if (sqlite_set_status(cfg, task_id, status) != 0) {
        free(result.data);
        json_decref(task);
        return -1;
    }

    json_t *out = envelope(task_id, cfg->agent, "result", initiative);
    json_t *out_payload = json_object();
    json_object_set_new(out_payload, "status", json_string(status));
    json_object_set_new(out_payload, "exit_code", json_integer(result.exit_code));
    json_object_set_new(out_payload, "output", json_string(result.data ? result.data : ""));
    json_object_set_new(out_payload, "truncated", json_boolean(result.truncated));
    json_object_set_new(out, "payload", out_payload);
    redis_publish(redis, cfg->result_channel, out);
    json_decref(out);

    char slack_text[1024];
    snprintf(slack_text, sizeof(slack_text), "[%s] Task `%s` %s: `%.*s`", cfg->agent, task_id,
             status, 500, result.data ? result.data : "");
    post_slack(cfg, slack_text);

    json_t *event = event_message(cfg->agent, task_id, result.exit_code == 0 ? "info" : "error",
                                  result.exit_code == 0 ? "task completed" : "task failed");
    redis_publish(redis, cfg->event_channel, event);
    json_decref(event);

    printf("[%s] task %s %s\n", cfg->agent, task_id, status);
    free(result.data);
    json_decref(task);
    return result.exit_code == 0 ? 0 : 1;
}

static redisContext *connect_redis(const wt_config *cfg) {
    struct timeval timeout = {2, 0};
    redisContext *redis = redisConnectWithTimeout(cfg->redis_host, cfg->redis_port, timeout);
    if (!redis || redis->err) {
        fprintf(stderr, "redis connect failed: %s\n", redis ? redis->errstr : "allocation failed");
        if (redis) {
            redisFree(redis);
        }
        return NULL;
    }
    return redis;
}

static int subscribe_loop(const wt_config *cfg) {
    redisContext *sub_redis = connect_redis(cfg);
    if (!sub_redis) {
        return 1;
    }
    redisContext *pub_redis = connect_redis(cfg);
    if (!pub_redis) {
        redisFree(sub_redis);
        return 1;
    }

    json_t *started = event_message(cfg->agent, NULL, "info", "wt-agent started");
    redis_publish(pub_redis, cfg->event_channel, started);
    json_decref(started);

    redisReply *reply = redisCommand(sub_redis, "SUBSCRIBE %s", cfg->task_channel);
    if (!reply) {
        fprintf(stderr, "redis subscribe failed: %s\n", sub_redis->errstr);
        redisFree(pub_redis);
        redisFree(sub_redis);
        return 1;
    }
    freeReplyObject(reply);
    printf("Agent '%s' listening on %s...\n", cfg->agent, cfg->task_channel);

    int rc = 0;
    while (!g_stop) {
        void *raw_reply = NULL;
        if (redisGetReply(sub_redis, &raw_reply) != REDIS_OK) {
            fprintf(stderr, "redis read failed: %s\n", sub_redis->errstr);
            rc = 1;
            break;
        }
        reply = raw_reply;
        if (!reply) {
            continue;
        }
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3 &&
            reply->element[0]->type == REDIS_REPLY_STRING &&
            strcmp(reply->element[0]->str, "message") == 0 &&
            reply->element[2]->type == REDIS_REPLY_STRING) {
            int task_rc = process_task(cfg, pub_redis, reply->element[2]->str);
            if (task_rc != 0 && task_rc != WT_TASK_IGNORED) {
                rc = task_rc;
            }
            if (cfg->once && task_rc != WT_TASK_IGNORED) {
                freeReplyObject(reply);
                break;
            }
        }
        freeReplyObject(reply);
    }

    redisFree(pub_redis);
    redisFree(sub_redis);
    return rc;
}

int main(int argc, char **argv) {
    wt_config cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc != 0) {
        return rc;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (cfg.task_json_path) {
        char *json_text = read_file(cfg.task_json_path);
        if (!json_text) {
            perror("read task json");
            return 1;
        }
        redisContext *redis = cfg.no_redis ? NULL : connect_redis(&cfg);
        rc = process_task(&cfg, redis, json_text);
        if (redis) {
            redisFree(redis);
        }
        free(json_text);
        return rc;
    }

    if (cfg.no_redis) {
        fprintf(stderr, "--no-redis requires --task-json FILE\n");
        return 2;
    }
    return subscribe_loop(&cfg);
}
