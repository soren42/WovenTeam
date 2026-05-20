/*
 * wt_roomd.c - Native HTTP/SSE daemon for the Phase 0 shared room.
 *
 * The daemon serves the static browser UI, accepts CEO messages, exposes recent
 * messages as JSON, and streams room updates with Server-Sent Events. It forks
 * one lightweight child per client so an SSE stream cannot block other local
 * requests in the parent accept loop.
 */
#include "wt_config.h"
#include "wt_http.h"
#include "wt_json.h"
#include "wt_message.h"
#include "wt_room_store.h"
#include "wt_task_projection.h"
#include "wt_task_store.h"
#include "wt_time.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int rebuildProjection(const WtConfig *config);

static void usage(const char *programName) {
    fprintf(stderr, "Usage: %s [--config FILE] [--port N] [--bind ADDRESS]\n", programName);
}

static const char *contentTypeForPath(const char *path) {
    if (strstr(path, ".css")) return "text/css; charset=utf-8";
    if (strstr(path, ".js")) return "application/javascript; charset=utf-8";
    if (strstr(path, ".html")) return "text/html; charset=utf-8";
    return "text/plain; charset=utf-8";
}

static void sleepMilliseconds(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int serveFile(int clientFd, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return wtHttpSendText(clientFd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0 || size > 1024 * 1024) {
        fclose(file);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "file error\n");
    }
    char *body = malloc((size_t)size + 1);
    if (!body) {
        fclose(file);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "memory error\n");
    }
    size_t bytesRead = fread(body, 1, (size_t)size, file);
    fclose(file);
    if (bytesRead != (size_t)size) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "file read error\n");
    }
    int rc = wtHttpSendBytes(clientFd, 200, "OK", contentTypeForPath(path), body, (size_t)size);
    free(body);
    return rc;
}

static int sendMessagesJson(int clientFd, const WtConfig *config, int limit) {
    WtMessage messages[256];
    int count = wtRoomReadRecent(config->roomLogPath, limit, messages, 256);
    char body[WT_MESSAGE_JSON_SIZE * 4];
    size_t used = 0;
    body[used++] = '[';
    for (int index = 0; index < count; index++) {
        char json[WT_MESSAGE_JSON_SIZE];
        wtMessageToJson(&messages[index], json, sizeof(json));
        size_t jsonLength = strlen(json);
        if (used + jsonLength + 3 >= sizeof(body)) {
            break;
        }
        if (index > 0) body[used++] = ',';
        memcpy(body + used, json, jsonLength);
        used += jsonLength;
    }
    body[used++] = ']';
    body[used] = '\0';
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int handlePostMessage(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config->roomName);
    if (wtJsonReadString(body, "senderName", message.senderName, sizeof(message.senderName)) != 0 ||
        wtJsonReadString(body, "targetName", message.targetName, sizeof(message.targetName)) != 0 ||
        wtJsonReadString(body, "messageType", message.messageType, sizeof(message.messageType)) != 0 ||
        wtJsonReadString(body, "messageBody", message.messageBody, sizeof(message.messageBody)) != 0 ||
        wtMessageValidateNames(&message) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid message\"}\n");
    }
    if (wtRoomAppendNewMessage(config->roomLogPath, &message, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char json[WT_MESSAGE_JSON_SIZE + 32];
    char messageJson[WT_MESSAGE_JSON_SIZE];
    wtMessageToJson(&message, messageJson, sizeof(messageJson));
    snprintf(json, sizeof(json), "{\"ok\":true,\"message\":%s}\n", messageJson);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", json);
}

static void compactJsonLine(const char *input, char *output, size_t outputSize) {
    size_t used = 0;
    int inString = 0;
    int escaped = 0;
    for (const char *cursor = input; *cursor && used + 1 < outputSize; cursor++) {
        char value = *cursor;
        if (escaped) {
            output[used++] = value;
            escaped = 0;
            continue;
        }
        if (value == '\\' && inString) {
            output[used++] = value;
            escaped = 1;
            continue;
        }
        if (value == '"') {
            inString = !inString;
            output[used++] = value;
            continue;
        }
        if (!inString && (value == '\n' || value == '\r' || value == '\t')) {
            continue;
        }
        output[used++] = value;
    }
    output[used] = '\0';
}

static int appendJsonRaw(char *buffer, size_t bufferSize, size_t *used, const char *text) {
    size_t length = strlen(text);
    if (*used + length + 1 >= bufferSize) {
        return -1;
    }
    memcpy(buffer + *used, text, length);
    *used += length;
    buffer[*used] = '\0';
    return 0;
}

static int appendJsonStringValue(char *buffer, size_t bufferSize, size_t *used, const char *text) {
    size_t inputLength = strlen(text ? text : "");
    size_t escapedSize = inputLength * 6 + 1;
    if (escapedSize < 64) escapedSize = 64;
    char *escaped = malloc(escapedSize);
    if (!escaped) {
        return -1;
    }
    int ok = wtJsonEscape(text ? text : "", escaped, escapedSize) == 0 &&
             appendJsonRaw(buffer, bufferSize, used, "\"") == 0 &&
             appendJsonRaw(buffer, bufferSize, used, escaped) == 0 &&
             appendJsonRaw(buffer, bufferSize, used, "\"") == 0;
    free(escaped);
    return ok ? 0 : -1;
}

static int isSafeArtifactName(const char *value) {
    if (!value || value[0] == '\0') return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '_' || *cursor == '-' || *cursor == '.')) {
            return 0;
        }
    }
    return strstr(value, "..") == NULL;
}

static int readTextSnippet(const char *path, char *buffer, size_t bufferSize, long *fileSize) {
    buffer[0] = '\0';
    if (fileSize) *fileSize = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 1;
    }
    if (fileSize) *fileSize = (long)st.st_size;
    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }
    size_t limit = bufferSize > 0 ? bufferSize - 1 : 0;
    size_t bytes = fread(buffer, 1, limit, file);
    buffer[bytes] = '\0';
    fclose(file);
    return 0;
}

static int appendTaskRoomEvent(const WtConfig *config, const char *taskId, const char *targetName,
                               const char *messageType, const char *status, const char *title) {
    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config->roomName);
    snprintf(message.senderName, sizeof(message.senderName), "%s", "system");
    if (targetName && (strcmp(targetName, "claude") == 0 || strcmp(targetName, "chatgpt") == 0 ||
                       strcmp(targetName, "gemini") == 0 || strcmp(targetName, "ceo") == 0 ||
                       strcmp(targetName, "all") == 0)) {
        snprintf(message.targetName, sizeof(message.targetName), "%s", targetName);
    } else {
        snprintf(message.targetName, sizeof(message.targetName), "%s", "all");
    }
    snprintf(message.messageType, sizeof(message.messageType), "%s", messageType);
    snprintf(message.messageBody, sizeof(message.messageBody), "taskId=%s status=%s title=%s",
             taskId, status, title && title[0] ? title : "Untitled task");
    return wtRoomAppendNewMessage(config->roomLogPath, &message, config->fsyncEachMessage);
}

static int roleMatchesAny(const char *role, const char *const *roles, size_t roleCount) {
    for (size_t index = 0; index < roleCount; index++) {
        if (strcmp(role, roles[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int roleMaySpawn(const char *requestingRole, const char *requestedRole) {
    static const char *const pgmRoles[] = {
        "project_manager", "software_architect", "systems_architect"
    };
    static const char *const pmRoles[] = {
        "software_architect", "systems_architect", "mockup_artist",
        "graphic_artist", "frontend_dev", "backend_dev", "database_engineer",
        "code_reviewer", "tester", "performance_engineer",
        "systems_administrator", "network_administrator", "database_administrator",
        "integration_specialist", "deployment_engineer", "technical_writer"
    };
    if (strcmp(requestingRole, "program_manager") == 0) {
        return roleMatchesAny(requestedRole, pgmRoles, sizeof(pgmRoles) / sizeof(pgmRoles[0]));
    }
    if (strcmp(requestingRole, "project_manager") == 0) {
        return roleMatchesAny(requestedRole, pmRoles, sizeof(pmRoles) / sizeof(pmRoles[0]));
    }
    return 0;
}

static int sendConfigJson(int clientFd, const WtConfig *config) {
    char body[4096];
    char configPath[WT_PATH_SIZE * 2];
    char claudeMode[WT_NAME_SIZE * 2];
    char chatgptMode[WT_NAME_SIZE * 2];
    char geminiMode[WT_NAME_SIZE * 2];
    char claudeCommand[WT_PATH_SIZE * 2];
    char gptCommand[WT_PATH_SIZE * 2];
    char geminiCommand[WT_PATH_SIZE * 2];
    if (wtJsonEscape(config->configPath, configPath, sizeof(configPath)) != 0 ||
        wtJsonEscape(config->claudeMode, claudeMode, sizeof(claudeMode)) != 0 ||
        wtJsonEscape(config->chatgptMode, chatgptMode, sizeof(chatgptMode)) != 0 ||
        wtJsonEscape(config->geminiMode, geminiMode, sizeof(geminiMode)) != 0 ||
        wtJsonEscape(config->claudeCommand, claudeCommand, sizeof(claudeCommand)) != 0 ||
        wtJsonEscape(config->gptCommand, gptCommand, sizeof(gptCommand)) != 0 ||
        wtJsonEscape(config->geminiCommand, geminiCommand, sizeof(geminiCommand)) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"config serialization failed\"}\n");
    }
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"configPath\":\"%s\","
             "\"tokenTelemetryEnabled\":%s,"
             "\"tokenDailyBudget\":%ld,"
             "\"tokenMonthlyBudget\":%ld,"
             "\"tokenWarningPercent\":%d,"
             "\"tokenCostPerMillionCents\":%d,"
             "\"contextMessageCount\":%d,"
             "\"agentPollMilliseconds\":%d,"
             "\"adapterTimeoutSeconds\":%d,"
             "\"adapterMaxOutputBytes\":%d,"
             "\"maxActiveTasksPerAgent\":%d,"
             "\"maxSubtasksPerParent\":%d,"
             "\"maxTasksPerInitiative\":%d,"
             "\"roleRoutingEnabled\":%s,"
             "\"enableCodexAdapter\":%s,"
             "\"enableClaudeAdapter\":%s,"
             "\"enableGeminiAdapter\":%s,"
             "\"claudeMode\":\"%s\","
             "\"chatgptMode\":\"%s\","
             "\"geminiMode\":\"%s\","
             "\"claudeCommand\":\"%s\","
             "\"gptCommand\":\"%s\","
             "\"geminiCommand\":\"%s\"}\n",
             configPath,
             config->tokenTelemetryEnabled ? "true" : "false",
             config->tokenDailyBudget,
             config->tokenMonthlyBudget,
             config->tokenWarningPercent,
             config->tokenCostPerMillionCents,
             config->contextMessageCount,
             config->agentPollMilliseconds,
             config->adapterTimeoutSeconds,
             config->adapterMaxOutputBytes,
             config->maxActiveTasksPerAgent,
             config->maxSubtasksPerParent,
             config->maxTasksPerInitiative,
             config->roleRoutingEnabled ? "true" : "false",
             config->enableCodexAdapter ? "true" : "false",
             config->enableClaudeAdapter ? "true" : "false",
             config->enableGeminiAdapter ? "true" : "false",
             claudeMode,
             chatgptMode,
             geminiMode,
             claudeCommand,
             gptCommand,
             geminiCommand);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int resolveCommandPath(const char *command, char *resolved, size_t resolvedSize) {
    if (!command || command[0] == '\0') {
        snprintf(resolved, resolvedSize, "%s", "");
        return 0;
    }
    if (strchr(command, '/')) {
        snprintf(resolved, resolvedSize, "%s", command);
        return access(command, X_OK) == 0;
    }
    const char *path = getenv("PATH");
    if (!path) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    char copy[1024];
    snprintf(copy, sizeof(copy), "%s", path);
    char *saveptr = NULL;
    for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
        char candidate[WT_PATH_SIZE];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", command);
        if (access(candidate, X_OK) == 0) {
            snprintf(resolved, resolvedSize, "%s", candidate);
            return 1;
        }
    }
    snprintf(resolved, resolvedSize, "%s", "");
    return 0;
}

static int pathParentWritable(const char *path) {
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (access(path, W_OK | X_OK) == 0) {
        return 1;
    }
    char copy[WT_PATH_SIZE];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        snprintf(copy, sizeof(copy), ".");
    } else if (slash == copy) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return access(copy, W_OK | X_OK) == 0;
}

static int readLatestAdapterFailure(const WtConfig *config, const char *agentName,
                                    char *failureClass, size_t failureClassSize,
                                    char *message, size_t messageSize) {
    FILE *file = fopen(config->taskLedgerPath, "r");
    if (!file) {
        snprintf(failureClass, failureClassSize, "%s", "");
        snprintf(message, messageSize, "%s", "");
        return 0;
    }
    char actor[WT_TASK_AGENT_SIZE];
    snprintf(actor, sizeof(actor), "wt-agent@%s", agentName);
    char line[WT_TASK_LEDGER_LINE_SIZE];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char schema[128];
        char status[WT_TASK_STATUS_SIZE];
        char createdBy[WT_TASK_AGENT_SIZE];
        char eventMessage[WT_TASK_TITLE_SIZE];
        if (wtJsonReadString(line, "schema", schema, sizeof(schema)) != 0 ||
            strcmp(schema, "woventeam.task_event.v0.1") != 0 ||
            wtJsonReadString(line, "status", status, sizeof(status)) != 0 ||
            strcmp(status, "failed") != 0 ||
            wtJsonReadString(line, "createdBy", createdBy, sizeof(createdBy)) != 0 ||
            strcmp(createdBy, actor) != 0) {
            continue;
        }
        if (wtJsonReadString(line, "message", eventMessage, sizeof(eventMessage)) != 0) {
            snprintf(eventMessage, sizeof(eventMessage), "%s", "adapter failed");
        }
        if (strstr(eventMessage, "timedOut=true")) {
            snprintf(failureClass, failureClassSize, "%s", "timeout");
        } else if (strstr(eventMessage, "exitCode=127")) {
            snprintf(failureClass, failureClassSize, "%s", "missing_cli");
        } else if (strstr(eventMessage, "exitCode=")) {
            snprintf(failureClass, failureClassSize, "%s", "nonzero_exit");
        } else {
            snprintf(failureClass, failureClassSize, "%s", "adapter_failed");
        }
        snprintf(message, messageSize, "%s", eventMessage);
        found = 1;
    }
    fclose(file);
    if (!found) {
        snprintf(failureClass, failureClassSize, "%s", "");
        snprintf(message, messageSize, "%s", "");
    }
    return found;
}

static void appendAdapterJson(char *body, size_t bodySize, const WtConfig *config,
                              const char *agentName, const char *adapterName,
                              int enabled, const char *mode, const char *command,
                              const char *profilesJson) {
    char commandPath[WT_PATH_SIZE];
    int commandLaunchable = resolveCommandPath(command, commandPath, sizeof(commandPath));
    int modeOk = strcmp(adapterName, "codex") == 0 || strcmp(mode, "adapter") == 0;
    int runtimeWritable = pathParentWritable(config->runtimeRootPath);
    int timeoutOk = config->adapterTimeoutSeconds > 0;
    int outputOk = config->adapterMaxOutputBytes > 0;
    int preflightOk = enabled && modeOk && commandLaunchable && runtimeWritable && timeoutOk && outputOk;
    const char *preflightState = !enabled ? "disabled" : preflightOk ? "ready" : "not_ready";
    const char *reason = "ready";
    if (!enabled) {
        reason = "adapter disabled";
    } else if (!modeOk) {
        reason = "adapter mode must be adapter";
    } else if (!commandLaunchable) {
        reason = "command is missing or not executable";
    } else if (!runtimeWritable) {
        reason = "runtime root or parent is not writable";
    } else if (!timeoutOk) {
        reason = "adapter timeout must be greater than zero";
    } else if (!outputOk) {
        reason = "adapter output cap must be greater than zero";
    }

    char failureClass[64];
    char failureMessage[WT_TASK_TITLE_SIZE];
    readLatestAdapterFailure(config, agentName, failureClass, sizeof(failureClass),
                             failureMessage, sizeof(failureMessage));

    char escapedAgent[WT_NAME_SIZE * 2];
    char escapedAdapter[WT_NAME_SIZE * 2];
    char escapedMode[WT_NAME_SIZE * 2];
    char escapedCommand[WT_PATH_SIZE * 2];
    char escapedCommandPath[WT_PATH_SIZE * 2];
    char escapedRuntime[WT_PATH_SIZE * 2];
    char escapedState[64];
    char escapedPreflightState[64];
    char escapedReason[256];
    char escapedFailureClass[128];
    char escapedFailureMessage[WT_TASK_TITLE_SIZE * 2];
    if (wtJsonEscape(agentName, escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(adapterName, escapedAdapter, sizeof(escapedAdapter)) != 0 ||
        wtJsonEscape(mode, escapedMode, sizeof(escapedMode)) != 0 ||
        wtJsonEscape(command, escapedCommand, sizeof(escapedCommand)) != 0 ||
        wtJsonEscape(commandPath, escapedCommandPath, sizeof(escapedCommandPath)) != 0 ||
        wtJsonEscape(config->runtimeRootPath, escapedRuntime, sizeof(escapedRuntime)) != 0 ||
        wtJsonEscape(commandLaunchable ? "launchable" : "missing", escapedState, sizeof(escapedState)) != 0 ||
        wtJsonEscape(preflightState, escapedPreflightState, sizeof(escapedPreflightState)) != 0 ||
        wtJsonEscape(reason, escapedReason, sizeof(escapedReason)) != 0 ||
        wtJsonEscape(failureClass, escapedFailureClass, sizeof(escapedFailureClass)) != 0 ||
        wtJsonEscape(failureMessage, escapedFailureMessage, sizeof(escapedFailureMessage)) != 0) {
        return;
    }

    char item[4096];
    snprintf(item, sizeof(item),
             "{\"agent\":\"%s\",\"adapter\":\"%s\",\"enabled\":%s,\"mode\":\"%s\","
             "\"command\":\"%s\",\"commandPath\":\"%s\",\"state\":\"%s\",\"profiles\":%s,"
             "\"preflight\":{\"ok\":%s,\"state\":\"%s\",\"reason\":\"%s\","
             "\"runtimeRootPath\":\"%s\",\"checks\":{\"enabled\":%s,\"modeReady\":%s,"
             "\"commandExecutable\":%s,\"runtimeWritable\":%s,\"timeoutConfigured\":%s,"
             "\"outputCapConfigured\":%s},\"lastFailure\":{\"class\":\"%s\",\"message\":\"%s\"}}}",
             escapedAgent,
             escapedAdapter,
             enabled ? "true" : "false",
             escapedMode,
             escapedCommand,
             escapedCommandPath,
             escapedState,
             profilesJson,
             preflightOk ? "true" : "false",
             escapedPreflightState,
             escapedReason,
             escapedRuntime,
             enabled ? "true" : "false",
             modeOk ? "true" : "false",
             commandLaunchable ? "true" : "false",
             runtimeWritable ? "true" : "false",
             timeoutOk ? "true" : "false",
             outputOk ? "true" : "false",
             escapedFailureClass,
             escapedFailureMessage);
    strncat(body, item, bodySize - strlen(body) - 1);
}

static int sendAdaptersJson(int clientFd, const WtConfig *config) {
    char body[8192];
    snprintf(body, sizeof(body), "%s", "{\"ok\":true,\"adapters\":[");
    appendAdapterJson(body, sizeof(body), config, "chatgpt", "codex",
                      config->enableCodexAdapter, config->chatgptMode, config->gptCommand,
                      "[\"repo_branch\",\"test_local\"]");
    strncat(body, ",", sizeof(body) - strlen(body) - 1);
    appendAdapterJson(body, sizeof(body), config, "claude", "claude",
                      config->enableClaudeAdapter, config->claudeMode, config->claudeCommand,
                      "[\"observe\",\"ops_read\"]");
    strncat(body, ",", sizeof(body) - strlen(body) - 1);
    appendAdapterJson(body, sizeof(body), config, "gemini", "gemini",
                      config->enableGeminiAdapter, config->geminiMode, config->geminiCommand,
                      "[\"observe\",\"ops_read\"]");
    strncat(body, "]}\n", sizeof(body) - strlen(body) - 1);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int handlePostConfig(int clientFd, WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    if (config->configPath[0] == '\0') {
        return wtHttpSendText(clientFd, 409, "Conflict", "application/json",
                              "{\"ok\":false,\"error\":\"config file path unavailable\"}\n");
    }
    long value = 0;
    if (wtJsonReadLong(body, "tokenTelemetryEnabled", &value) == 0) {
        config->tokenTelemetryEnabled = value != 0;
    }
    if (wtJsonReadLong(body, "tokenDailyBudget", &value) == 0) {
        config->tokenDailyBudget = value < 0 ? 0 : value;
    }
    if (wtJsonReadLong(body, "tokenMonthlyBudget", &value) == 0) {
        config->tokenMonthlyBudget = value < 0 ? 0 : value;
    }
    if (wtJsonReadLong(body, "tokenWarningPercent", &value) == 0) {
        if (value < 1) value = 1;
        if (value > 100) value = 100;
        config->tokenWarningPercent = (int)value;
    }
    if (wtJsonReadLong(body, "tokenCostPerMillionCents", &value) == 0) {
        config->tokenCostPerMillionCents = value < 0 ? 0 : (int)value;
    }
    if (wtJsonReadLong(body, "enableCodexAdapter", &value) == 0) {
        config->enableCodexAdapter = value != 0;
    }
    if (wtJsonReadLong(body, "roleRoutingEnabled", &value) == 0) {
        config->roleRoutingEnabled = value != 0;
    }
    if (wtJsonReadLong(body, "maxActiveTasksPerAgent", &value) == 0) {
        config->maxActiveTasksPerAgent = value < 1 ? 1 : (int)value;
    }
    if (wtJsonReadLong(body, "maxSubtasksPerParent", &value) == 0) {
        config->maxSubtasksPerParent = value < 1 ? 1 : (int)value;
    }
    if (wtJsonReadLong(body, "maxTasksPerInitiative", &value) == 0) {
        config->maxTasksPerInitiative = value < 1 ? 1 : (int)value;
    }
    if (wtJsonReadLong(body, "enableClaudeAdapter", &value) == 0) {
        config->enableClaudeAdapter = value != 0;
    }
    if (wtJsonReadLong(body, "enableGeminiAdapter", &value) == 0) {
        config->enableGeminiAdapter = value != 0;
    }
    wtJsonReadString(body, "claudeMode", config->claudeMode, sizeof(config->claudeMode));
    wtJsonReadString(body, "chatgptMode", config->chatgptMode, sizeof(config->chatgptMode));
    wtJsonReadString(body, "geminiMode", config->geminiMode, sizeof(config->geminiMode));
    wtJsonReadString(body, "claudeCommand", config->claudeCommand, sizeof(config->claudeCommand));
    wtJsonReadString(body, "gptCommand", config->gptCommand, sizeof(config->gptCommand));
    wtJsonReadString(body, "geminiCommand", config->geminiCommand, sizeof(config->geminiCommand));
    if (wtConfigWriteFile(config, config->configPath) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"config write failed\"}\n");
    }
    return sendConfigJson(clientFd, config);
}

static int sendTokenJson(int clientFd, const WtConfig *config) {
    WtTokenSummary summary;
    wtTaskSummarizeTokenBudgets(config->taskLedgerPath, wtNowUnixMilliseconds(), &summary);
    long long dayCostCents = config->tokenCostPerMillionCents > 0 ?
        (summary.dayWindowAllocatedTokens * (long long)config->tokenCostPerMillionCents) / 1000000LL : 0;
    long long monthCostCents = config->tokenCostPerMillionCents > 0 ?
        (summary.monthWindowAllocatedTokens * (long long)config->tokenCostPerMillionCents) / 1000000LL : 0;
    int dayPercent = config->tokenDailyBudget > 0 ?
        (int)((summary.dayWindowAllocatedTokens * 100LL) / config->tokenDailyBudget) : 0;
    int monthPercent = config->tokenMonthlyBudget > 0 ?
        (int)((summary.monthWindowAllocatedTokens * 100LL) / config->tokenMonthlyBudget) : 0;
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"enabled\":%s,"
             "\"tokenDailyBudget\":%ld,"
             "\"tokenMonthlyBudget\":%ld,"
             "\"tokenWarningPercent\":%d,"
             "\"tokenCostPerMillionCents\":%d,"
             "\"dayWindowAllocatedTokens\":%lld,"
             "\"monthWindowAllocatedTokens\":%lld,"
             "\"allTimeAllocatedTokens\":%lld,"
             "\"dayWindowActualTokens\":%lld,"
             "\"monthWindowActualTokens\":%lld,"
             "\"allTimeActualTokens\":%lld,"
             "\"dayWindowPackages\":%d,"
             "\"monthWindowPackages\":%d,"
             "\"allTimePackages\":%d,"
             "\"dayWindowUsageEvents\":%d,"
             "\"monthWindowUsageEvents\":%d,"
             "\"allTimeUsageEvents\":%d,"
             "\"dayWindowCostCents\":%lld,"
             "\"monthWindowCostCents\":%lld,"
             "\"dayWindowActualCostCents\":%lld,"
             "\"monthWindowActualCostCents\":%lld,"
             "\"allTimeActualCostCents\":%lld,"
             "\"dayWindowBudgetPercent\":%d,"
             "\"monthWindowBudgetPercent\":%d}\n",
             config->tokenTelemetryEnabled ? "true" : "false",
             config->tokenDailyBudget,
             config->tokenMonthlyBudget,
             config->tokenWarningPercent,
             config->tokenCostPerMillionCents,
             summary.dayWindowAllocatedTokens,
             summary.monthWindowAllocatedTokens,
             summary.allTimeAllocatedTokens,
             summary.dayWindowActualTokens,
             summary.monthWindowActualTokens,
             summary.allTimeActualTokens,
             summary.dayWindowPackages,
             summary.monthWindowPackages,
             summary.allTimePackages,
             summary.dayWindowUsageEvents,
             summary.monthWindowUsageEvents,
             summary.allTimeUsageEvents,
             dayCostCents,
             monthCostCents,
             summary.dayWindowActualCostCents,
             summary.monthWindowActualCostCents,
             summary.allTimeActualCostCents,
             dayPercent,
             monthPercent);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static void defaultAgentForRole(const char *role, char *agent, size_t agentSize) {
    if (strcmp(role, "backend_dev") == 0 || strcmp(role, "frontend_dev") == 0 ||
        strcmp(role, "database_engineer") == 0 || strcmp(role, "tester") == 0 ||
        strcmp(role, "code_reviewer") == 0) {
        snprintf(agent, agentSize, "%s", "chatgpt");
    } else if (strcmp(role, "technical_writer") == 0 || strcmp(role, "project_manager") == 0) {
        snprintf(agent, agentSize, "%s", "claude");
    } else if (strcmp(role, "software_architect") == 0 || strcmp(role, "systems_architect") == 0 ||
               strcmp(role, "performance_engineer") == 0) {
        snprintf(agent, agentSize, "%s", "gemini");
    } else {
        snprintf(agent, agentSize, "%s", "all");
    }
}

static int enforceTokenBudget(const WtConfig *config, long maxTokens, char *error, size_t errorSize) {
    if (!config->tokenTelemetryEnabled || maxTokens <= 0) {
        return 0;
    }
    WtTokenSummary summary;
    wtTaskSummarizeTokenBudgets(config->taskLedgerPath, wtNowUnixMilliseconds(), &summary);
    if (config->tokenDailyBudget > 0 &&
        summary.dayWindowAllocatedTokens + maxTokens > config->tokenDailyBudget) {
        snprintf(error, errorSize, "daily token budget exceeded");
        return 1;
    }
    if (config->tokenMonthlyBudget > 0 &&
        summary.monthWindowAllocatedTokens + maxTokens > config->tokenMonthlyBudget) {
        snprintf(error, errorSize, "monthly token budget exceeded");
        return 1;
    }
    return 0;
}

static int agentSupportsProfile(const char *agent, const char *profile) {
    if (strcmp(agent, "chatgpt") == 0) {
        return strcmp(profile, "repo_branch") == 0 || strcmp(profile, "test_local") == 0 ||
               strcmp(profile, "observe") == 0;
    }
    if (strcmp(agent, "claude") == 0 || strcmp(agent, "gemini") == 0) {
        return strcmp(profile, "observe") == 0 || strcmp(profile, "ops_read") == 0;
    }
    return 0;
}

static int shouldRouteAgent(const char *agent) {
    return agent[0] == '\0' || strcmp(agent, "all") == 0 || strcmp(agent, "router") == 0;
}

static void routeAgentForTask(const WtConfig *config, const char *role, const char *profile,
                              char *agent, size_t agentSize) {
    if (!config->roleRoutingEnabled || !shouldRouteAgent(agent)) {
        return;
    }
    if (strcmp(profile, "repo_branch") == 0 || strcmp(profile, "test_local") == 0) {
        snprintf(agent, agentSize, "%s", "chatgpt");
        return;
    }
    defaultAgentForRole(role, agent, agentSize);
    if (!agentSupportsProfile(agent, profile)) {
        snprintf(agent, agentSize, "%s", strcmp(profile, "ops_read") == 0 ? "gemini" : "claude");
    }
}

static void defaultModelForAgent(const char *agent, char *modelId, size_t modelSize) {
    if (strcmp(agent, "claude") == 0) {
        snprintf(modelId, modelSize, "%s", "anthropic/claude-sonnet");
    } else if (strcmp(agent, "gemini") == 0) {
        snprintf(modelId, modelSize, "%s", "google/gemini-pro");
    } else if (strcmp(agent, "chatgpt") == 0) {
        snprintf(modelId, modelSize, "%s", "openai/gpt-5.3-codex");
    }
}

static int modelMatchesAgent(const char *agent, const char *modelId) {
    if (!modelId || modelId[0] == '\0') return 0;
    if (strcmp(agent, "claude") == 0) return strncmp(modelId, "anthropic/", 10) == 0;
    if (strcmp(agent, "gemini") == 0) return strncmp(modelId, "google/", 7) == 0;
    if (strcmp(agent, "chatgpt") == 0) return strncmp(modelId, "openai/", 7) == 0;
    return 1;
}

static int enforceCapacity(const WtConfig *config, const char *agent, const char *initiativeId,
                           const char *parentTaskId, char *error, size_t errorSize) {
    if (rebuildProjection(config) != 0) {
        snprintf(error, errorSize, "%s", "projection rebuild failed");
        return -1;
    }
    int count = wtTaskProjectionCountActiveForAgent(config->taskProjectionDbPath, agent);
    if (count < 0) {
        snprintf(error, errorSize, "%s", "capacity read failed");
        return -1;
    }
    if (config->maxActiveTasksPerAgent > 0 && count >= config->maxActiveTasksPerAgent) {
        snprintf(error, errorSize, "agent capacity exceeded for %s", agent);
        return 1;
    }
    if (initiativeId && initiativeId[0]) {
        count = wtTaskProjectionCountActiveForInitiative(config->taskProjectionDbPath, initiativeId);
        if (count < 0) {
            snprintf(error, errorSize, "%s", "capacity read failed");
            return -1;
        }
        if (config->maxTasksPerInitiative > 0 && count >= config->maxTasksPerInitiative) {
            snprintf(error, errorSize, "initiative capacity exceeded for %s", initiativeId);
            return 1;
        }
    }
    if (parentTaskId && parentTaskId[0]) {
        count = wtTaskProjectionCountActiveForParent(config->taskProjectionDbPath, parentTaskId);
        if (count < 0) {
            snprintf(error, errorSize, "%s", "capacity read failed");
            return -1;
        }
        if (config->maxSubtasksPerParent > 0 && count >= config->maxSubtasksPerParent) {
            snprintf(error, errorSize, "parent subtask capacity exceeded for %s", parentTaskId);
            return 1;
        }
    }
    return 0;
}

static int handlePostTaskPackage(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char schema[128];
    char taskId[WT_TASK_ID_SIZE];
    char assignedRole[WT_TASK_AGENT_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char initiativeId[WT_TASK_ID_SIZE];
    char toolProfile[WT_TASK_POLICY_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char title[WT_TASK_TITLE_SIZE];
    long maxTokens = 0;
    if (wtJsonReadString(body, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "woventeam.task_package.v0.1") != 0 ||
        wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0 ||
        wtJsonReadString(body, "assignedRole", assignedRole, sizeof(assignedRole)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid task package\"}\n");
    }
    if (wtJsonReadString(body, "assignedAgent", assignedAgent, sizeof(assignedAgent)) != 0) {
        snprintf(assignedAgent, sizeof(assignedAgent), "%s", "router");
    }
    if (wtJsonReadString(body, "initiativeId", initiativeId, sizeof(initiativeId)) != 0) {
        snprintf(initiativeId, sizeof(initiativeId), "%s", "");
    }
    if (wtJsonReadString(body, "profile", toolProfile, sizeof(toolProfile)) != 0) {
        snprintf(toolProfile, sizeof(toolProfile), "%s", "observe");
    }
    if (wtJsonReadString(body, "status", status, sizeof(status)) != 0) {
        snprintf(status, sizeof(status), "%s", "queued");
    }
    if (wtJsonReadString(body, "title", title, sizeof(title)) != 0) {
        snprintf(title, sizeof(title), "%s", "Untitled task");
    }
    wtJsonReadLong(body, "maxTokens", &maxTokens);
    if (maxTokens < 0) {
        maxTokens = 0;
    }
    routeAgentForTask(config, assignedRole, toolProfile, assignedAgent, sizeof(assignedAgent));
    char budgetError[256];
    int budget = enforceTokenBudget(config, maxTokens, budgetError, sizeof(budgetError));
    if (budget != 0) {
        char response[512];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}\n", budgetError);
        return wtHttpSendText(clientFd, 409, "Conflict", "application/json", response);
    }
    char capacityError[256];
    int capacity = enforceCapacity(config, assignedAgent, initiativeId, "", capacityError, sizeof(capacityError));
    if (capacity != 0) {
        char response[512];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}\n", capacityError);
        return wtHttpSendText(clientFd, capacity > 0 ? 409 : 500,
                              capacity > 0 ? "Conflict" : "Internal Server Error",
                              "application/json", response);
    }
    char compact[WT_TASK_LEDGER_LINE_SIZE];
    if (shouldRouteAgent(assignedAgent)) {
        compactJsonLine(body, compact, sizeof(compact));
    } else {
        char escapedTaskId[WT_TASK_ID_SIZE * 2];
        char escapedRole[WT_TASK_AGENT_SIZE * 2];
        char escapedAgent[WT_TASK_AGENT_SIZE * 2];
        char escapedProfile[WT_TASK_POLICY_SIZE * 2];
        if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
            wtJsonEscape(assignedRole, escapedRole, sizeof(escapedRole)) != 0 ||
            wtJsonEscape(assignedAgent, escapedAgent, sizeof(escapedAgent)) != 0 ||
            wtJsonEscape(toolProfile, escapedProfile, sizeof(escapedProfile)) != 0) {
            return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"task too large\"}\n");
        }
        snprintf(compact, sizeof(compact),
                 "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
                 "\"eventType\":\"routing\",\"status\":\"assigned\","
                 "\"assignedAgent\":\"%s\",\"message\":\"Routed %s task to %s for %s.\","
                 "\"createdBy\":\"wt-roomd\",\"createdAtUnixMs\":%lld}",
                 escapedTaskId, escapedAgent, escapedRole, escapedAgent,
                 escapedProfile, wtNowUnixMilliseconds());
        char original[WT_TASK_LEDGER_LINE_SIZE];
        compactJsonLine(body, original, sizeof(original));
        if (wtTaskAppendRecord(config->taskLedgerPath, original, config->fsyncEachMessage) != 0 ||
            wtTaskAppendRecord(config->taskLedgerPath, compact, config->fsyncEachMessage) != 0 ||
            appendTaskRoomEvent(config, taskId, assignedAgent, "task.assign", "assigned", title) != 0) {
            return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
        }
        char response[512];
        snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"assignedAgent\":\"%s\",\"status\":\"assigned\"}\n",
                 taskId, assignedAgent);
        return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
    }
    if (wtTaskAppendRecord(config->taskLedgerPath, compact, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, taskId, assignedAgent, "task.assign", status, title) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"assignedAgent\":\"%s\",\"status\":\"%s\"}\n",
             taskId, assignedAgent, status);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int handlePostTaskRequest(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char schema[128];
    char parentTaskId[WT_TASK_ID_SIZE];
    char requestedTaskId[WT_TASK_ID_SIZE];
    char initiativeId[WT_TASK_ID_SIZE];
    char requestedByRole[WT_TASK_AGENT_SIZE];
    char requestedBy[WT_TASK_AGENT_SIZE];
    char requestedRole[WT_TASK_AGENT_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char priority[32];
    char title[WT_TASK_TITLE_SIZE];
    char taskBody[WT_TASK_BODY_SIZE];
    char toolProfile[WT_TASK_POLICY_SIZE];
    char modelId[WT_TASK_MODEL_SIZE];
    long maxTokens = 2000000;
    if (wtJsonReadString(body, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "woventeam.task_request.v0.1") != 0 ||
        wtJsonReadString(body, "parentTaskId", parentTaskId, sizeof(parentTaskId)) != 0 ||
        wtJsonReadString(body, "requestedTaskId", requestedTaskId, sizeof(requestedTaskId)) != 0 ||
        wtJsonReadString(body, "initiativeId", initiativeId, sizeof(initiativeId)) != 0 ||
        wtJsonReadString(body, "requestedByRole", requestedByRole, sizeof(requestedByRole)) != 0 ||
        wtJsonReadString(body, "requestedRole", requestedRole, sizeof(requestedRole)) != 0 ||
        wtJsonReadString(body, "title", title, sizeof(title)) != 0 ||
        wtJsonReadString(body, "body", taskBody, sizeof(taskBody)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid task request\"}\n");
    }
    if (!roleMaySpawn(requestedByRole, requestedRole)) {
        return wtHttpSendText(clientFd, 403, "Forbidden", "application/json", "{\"ok\":false,\"error\":\"role spawn policy denied\"}\n");
    }
    if (wtJsonReadString(body, "requestedBy", requestedBy, sizeof(requestedBy)) != 0) {
        snprintf(requestedBy, sizeof(requestedBy), "%s", requestedByRole);
    }
    if (wtJsonReadString(body, "assignedAgent", assignedAgent, sizeof(assignedAgent)) != 0) {
        snprintf(assignedAgent, sizeof(assignedAgent), "%s", "router");
    }
    if (wtJsonReadString(body, "priority", priority, sizeof(priority)) != 0) {
        snprintf(priority, sizeof(priority), "%s", "normal");
    }
    if (wtJsonReadString(body, "profile", toolProfile, sizeof(toolProfile)) != 0) {
        snprintf(toolProfile, sizeof(toolProfile), "%s", "observe");
    }
    if (wtJsonReadString(body, "modelId", modelId, sizeof(modelId)) != 0) {
        snprintf(modelId, sizeof(modelId), "%s", "openai/gpt-5.3-codex");
    }
    wtJsonReadLong(body, "maxTokens", &maxTokens);
    if (maxTokens < 0) {
        maxTokens = 0;
    }
    char budgetError[256];
    int budget = enforceTokenBudget(config, maxTokens, budgetError, sizeof(budgetError));
    if (budget != 0) {
        char response[512];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}\n", budgetError);
        return wtHttpSendText(clientFd, 409, "Conflict", "application/json", response);
    }
    routeAgentForTask(config, requestedRole, toolProfile, assignedAgent, sizeof(assignedAgent));
    if (!modelMatchesAgent(assignedAgent, modelId)) {
        defaultModelForAgent(assignedAgent, modelId, sizeof(modelId));
    }
    char capacityError[256];
    int capacity = enforceCapacity(config, assignedAgent, initiativeId, parentTaskId, capacityError, sizeof(capacityError));
    if (capacity != 0) {
        char response[512];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}\n", capacityError);
        return wtHttpSendText(clientFd, capacity > 0 ? 409 : 500,
                              capacity > 0 ? "Conflict" : "Internal Server Error",
                              "application/json", response);
    }

    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedInitiative[WT_TASK_ID_SIZE * 2];
    char escapedRequestedBy[WT_TASK_AGENT_SIZE * 2];
    char escapedRequestedByRole[WT_TASK_AGENT_SIZE * 2];
    char escapedRole[WT_TASK_AGENT_SIZE * 2];
    char escapedAgent[WT_TASK_AGENT_SIZE * 2];
    char escapedModel[WT_TASK_MODEL_SIZE * 2];
    char escapedPriority[64];
    char escapedTitle[WT_TASK_TITLE_SIZE * 2];
    char escapedBody[WT_TASK_BODY_SIZE * 2];
    char escapedProfile[WT_TASK_POLICY_SIZE * 2];
    char escapedParent[WT_TASK_ID_SIZE * 2];
    if (wtJsonEscape(requestedTaskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(initiativeId, escapedInitiative, sizeof(escapedInitiative)) != 0 ||
        wtJsonEscape(requestedBy, escapedRequestedBy, sizeof(escapedRequestedBy)) != 0 ||
        wtJsonEscape(requestedByRole, escapedRequestedByRole, sizeof(escapedRequestedByRole)) != 0 ||
        wtJsonEscape(requestedRole, escapedRole, sizeof(escapedRole)) != 0 ||
        wtJsonEscape(assignedAgent, escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(modelId, escapedModel, sizeof(escapedModel)) != 0 ||
        wtJsonEscape(priority, escapedPriority, sizeof(escapedPriority)) != 0 ||
        wtJsonEscape(title, escapedTitle, sizeof(escapedTitle)) != 0 ||
        wtJsonEscape(taskBody, escapedBody, sizeof(escapedBody)) != 0 ||
        wtJsonEscape(toolProfile, escapedProfile, sizeof(escapedProfile)) != 0 ||
        wtJsonEscape(parentTaskId, escapedParent, sizeof(escapedParent)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"request too large\"}\n");
    }
    long long createdAtUnixMs = wtNowUnixMilliseconds();
    char taskRequest[WT_TASK_LEDGER_LINE_SIZE];
    snprintf(taskRequest, sizeof(taskRequest),
             "{\"schema\":\"woventeam.task_request.v0.1\",\"taskId\":\"%s\","
             "\"parentTaskId\":\"%s\",\"requestedTaskId\":\"%s\","
             "\"initiativeId\":\"%s\",\"requestedBy\":\"%s\","
             "\"requestedByRole\":\"%s\",\"requestedRole\":\"%s\","
             "\"assignedAgent\":\"%s\",\"modelId\":\"%s\",\"priority\":\"%s\","
             "\"title\":\"%s\",\"body\":\"%s\","
             "\"toolPolicy\":{\"profile\":\"%s\"},\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedParent, escapedTaskId, escapedInitiative,
             escapedRequestedBy, escapedRequestedByRole, escapedRole,
             escapedAgent, escapedModel, escapedPriority, escapedTitle,
             escapedBody, escapedProfile, createdAtUnixMs);
    char taskPackage[WT_TASK_LEDGER_LINE_SIZE];
    snprintf(taskPackage, sizeof(taskPackage),
             "{\"schema\":\"woventeam.task_package.v0.1\",\"taskId\":\"%s\","
             "\"initiativeId\":\"%s\",\"createdBy\":\"%s\",\"assignedRole\":\"%s\","
             "\"assignedAgent\":\"%s\",\"modelId\":\"%s\",\"priority\":\"%s\","
             "\"status\":\"queued\",\"title\":\"%s\",\"body\":\"%s\","
             "\"parentTaskId\":\"%s\",\"requestedByRole\":\"%s\","
             "\"task\":{\"title\":\"%s\",\"body\":\"%s\",\"deliverables\":[]},"
             "\"contextRefs\":[],\"acceptanceCriteria\":[\"Task result is recorded in the room and task ledger.\"],"
             "\"toolPolicy\":{\"profile\":\"%s\",\"filesystem\":\"%s\",\"network\":\"%s\",\"system\":\"none\",\"git\":\"%s\"},"
             "\"budget\":{\"timeoutSeconds\":1800,\"maxOutputBytes\":1048576,\"maxCostUsd\":1.0,\"maxTokens\":%ld},"
             "\"dependencies\":[\"%s\"],\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedInitiative, escapedRequestedBy, escapedRole,
             escapedAgent, escapedModel, escapedPriority, escapedTitle, escapedBody,
             escapedParent, escapedRequestedByRole, escapedTitle, escapedBody, escapedProfile,
             strcmp(toolProfile, "repo_branch") == 0 || strcmp(toolProfile, "test_local") == 0 ? "workspace_write" : "read_only",
             strcmp(toolProfile, "test_local") == 0 ? "loopback" : "none",
             strcmp(toolProfile, "repo_branch") == 0 || strcmp(toolProfile, "test_local") == 0 ? "branch_only" : "none",
             maxTokens,
             escapedParent, createdAtUnixMs);
    if (wtTaskAppendRecord(config->taskLedgerPath, taskRequest, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, requestedTaskId, assignedAgent, "task.request", "requested", title) != 0 ||
        wtTaskAppendRecord(config->taskLedgerPath, taskPackage, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, requestedTaskId, assignedAgent, "task.assign", "queued", title) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"taskId\":\"%s\",\"parentTaskId\":\"%s\",\"assignedRole\":\"%s\",\"assignedAgent\":\"%s\",\"status\":\"queued\"}\n",
             requestedTaskId, parentTaskId, requestedRole, assignedAgent);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int sendTasksJson(int clientFd, const WtConfig *config) {
    char body[WT_TASK_LEDGER_LINE_SIZE * 8];
    if (wtTaskReadAllJsonArray(config->taskLedgerPath, body, sizeof(body)) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int rebuildProjection(const WtConfig *config) {
    return wtTaskProjectionRebuild(config->taskProjectionDbPath, config->taskLedgerPath);
}

static int sendTaskSummariesJson(int clientFd, const WtConfig *config) {
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 16);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    if (wtTaskProjectionReadSummariesJson(config->taskProjectionDbPath, body, WT_TASK_LEDGER_LINE_SIZE * 16) != 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int sendTaskDetailJson(int clientFd, const WtConfig *config, const char *taskId) {
    if (!taskId || taskId[0] == '\0') {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"task id required\"}\n");
    }
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 16);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    int detailRc = wtTaskProjectionReadDetailJson(config->taskProjectionDbPath, taskId, body, WT_TASK_LEDGER_LINE_SIZE * 16);
    if (detailRc < 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, detailRc == 1 ? 404 : 200, detailRc == 1 ? "Not Found" : "OK",
                            "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int sendInitiativesJson(int clientFd, const WtConfig *config) {
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 12);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    if (wtTaskProjectionReadInitiativesJson(config->taskProjectionDbPath, body, WT_TASK_LEDGER_LINE_SIZE * 12) != 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"initiative read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int sendInitiativeDetailJson(int clientFd, const WtConfig *config, const char *initiativeId) {
    if (!initiativeId || initiativeId[0] == '\0') {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"initiative id required\"}\n");
    }
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 16);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    int detailRc = wtTaskProjectionReadInitiativeDetailJson(config->taskProjectionDbPath, initiativeId, body, WT_TASK_LEDGER_LINE_SIZE * 16);
    if (detailRc < 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"initiative detail read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, detailRc == 1 ? 404 : 200, detailRc == 1 ? "Not Found" : "OK",
                            "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int sendCapacityJson(int clientFd, const WtConfig *config) {
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 8);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    if (wtTaskProjectionReadCapacityJson(config->taskProjectionDbPath, body, WT_TASK_LEDGER_LINE_SIZE * 8) != 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"capacity read failed\"}\n");
    }
    size_t length = strlen(body);
    char caps[256];
    snprintf(caps, sizeof(caps),
             ",\"caps\":{\"maxActiveTasksPerAgent\":%d,\"maxSubtasksPerParent\":%d,\"maxTasksPerInitiative\":%d}}\n",
             config->maxActiveTasksPerAgent, config->maxSubtasksPerParent, config->maxTasksPerInitiative);
    if (length > 0 && body[length - 1] == '}') {
        body[length - 1] = '\0';
        strncat(body, caps, WT_TASK_LEDGER_LINE_SIZE * 8 - strlen(body) - 1);
    }
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int sendAgentsJson(int clientFd, const WtConfig *config) {
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 8);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    if (wtTaskProjectionReadAgentsJson(config->taskProjectionDbPath, wtNowUnixMilliseconds(), body,
                                       WT_TASK_LEDGER_LINE_SIZE * 8) != 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"agent workload read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

/*
 * POST /api/task-artifact
 *
 * Sprint 4: artifact promotion endpoint. Records a state transition for an
 * adapter-produced workspace artifact so a reviewed asset becomes a first-class
 * project output. Body shape:
 *   { "taskId": "task_abc",
 *     "state": "draft|reviewed|accepted|rejected|superseded",
 *     "reviewer": "alice",
 *     "notes": "Looks good, shipping.",
 *     "artifactPath": "result.md" }
 *
 * artifactPath is workspace-relative (validated against the artifact-name
 * safety check used by /api/task-artifacts so callers cannot promote arbitrary
 * filesystem paths through the endpoint).
 */
static int handlePostTaskArtifact(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"missing body\"}\n");
    }
    body += 4;
    char taskId[WT_TASK_ID_SIZE];
    char state[64];
    char reviewer[WT_TASK_AGENT_SIZE];
    char notes[WT_TASK_BODY_SIZE];
    char artifactPath[512];
    if (wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"taskId required\"}\n");
    }
    if (wtJsonReadString(body, "state", state, sizeof(state)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"state required\"}\n");
    }
    /* State allowlist: matches the documented lifecycle in
     * docs/launch/phase2-functional-workbench-plan.html stream #4. */
    if (strcmp(state, "draft") != 0 && strcmp(state, "reviewed") != 0 &&
        strcmp(state, "accepted") != 0 && strcmp(state, "rejected") != 0 &&
        strcmp(state, "superseded") != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"unsupported artifact state\"}\n");
    }
    if (wtJsonReadString(body, "reviewer", reviewer, sizeof(reviewer)) != 0) {
        snprintf(reviewer, sizeof(reviewer), "%s", "ceo");
    }
    if (wtJsonReadString(body, "notes", notes, sizeof(notes)) != 0) {
        notes[0] = '\0';
    }
    if (wtJsonReadString(body, "artifactPath", artifactPath, sizeof(artifactPath)) != 0) {
        artifactPath[0] = '\0';
    }
    /* If an artifact path was supplied, validate it before persisting so the
     * inventory and CLI export can trust the projection value. Empty paths are
     * allowed for "reviewed" notes that do not pin a specific file. */
    if (artifactPath[0] != '\0' && !isSafeArtifactName(artifactPath)) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"invalid artifact path\"}\n");
    }
    if (wtTaskAppendArtifactEvent(config->taskLedgerPath, taskId, state, reviewer, notes,
                                  artifactPath, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    /* Echo to the shared room transcript so operators reading the chat panel
     * see promotion activity alongside chat. */
    char roomMessage[WT_TASK_TITLE_SIZE];
    snprintf(roomMessage, sizeof(roomMessage), "artifact %s by %s%s%s",
             state, reviewer,
             artifactPath[0] ? " (" : "",
             artifactPath[0] ? artifactPath : "");
    if (artifactPath[0]) strncat(roomMessage, ")", sizeof(roomMessage) - strlen(roomMessage) - 1);
    appendTaskRoomEvent(config, taskId, "all", "task.artifact", state, roomMessage);
    char response[1024];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"taskId\":\"%s\",\"state\":\"%s\",\"reviewer\":\"%s\",\"artifactPath\":\"%s\"}\n",
             taskId, state, reviewer, artifactPath);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int sendInitiativeArtifactsJson(int clientFd, const WtConfig *config, const char *initiativeId) {
    if (rebuildProjection(config) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"projection rebuild failed\"}\n");
    }
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 8);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    if (wtTaskProjectionReadInitiativeArtifactsJson(config->taskProjectionDbPath, initiativeId,
                                                    body, WT_TASK_LEDGER_LINE_SIZE * 8) != 0) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"artifact inventory read failed\"}\n");
    }
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

/*
 * POST /api/task-reclaim
 *
 * Operator-visible reclaim path. Releases the most recent lease on a task so it
 * returns to the queued pool. Body shape:
 *   { "taskId": "task_abc", "reason": "operator"|"lease_expired",
 *     "message": "...", "createdBy": "ceo" }
 *
 * Reason is restricted to a small allowlist so the projection's
 * last_reclaim_reason column stays a stable enum-like value.
 */
static int handlePostTaskReclaim(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"missing body\"}\n");
    }
    body += 4;
    char taskId[WT_TASK_ID_SIZE];
    char reason[64];
    char message[WT_TASK_TITLE_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    if (wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"taskId required\"}\n");
    }
    if (wtJsonReadString(body, "reason", reason, sizeof(reason)) != 0) {
        snprintf(reason, sizeof(reason), "%s", "operator");
    }
    /* Reason allowlist - keeps projection column predictable for the UI. */
    if (strcmp(reason, "operator") != 0 && strcmp(reason, "lease_expired") != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"unsupported reclaim reason\"}\n");
    }
    if (wtJsonReadString(body, "message", message, sizeof(message)) != 0) {
        snprintf(message, sizeof(message), "%s",
                 strcmp(reason, "operator") == 0 ?
                     "Lease reclaimed by operator." :
                     "Lease reclaimed because it expired.");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "ceo");
    }
    /* Look up the existing lease holder so the reclaim event carries provenance. */
    char previousAgent[WT_TASK_AGENT_SIZE] = "";
    long long previousLeaseExpiresAt = 0;
    int previousAttempt = 0;
    wtTaskFindActiveLease(config->taskLedgerPath, taskId, previousAgent, sizeof(previousAgent),
                          &previousLeaseExpiresAt, &previousAttempt);
    if (wtTaskAppendReclaimEvent(config->taskLedgerPath, taskId, previousAgent, createdBy,
                                 reason, message, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json",
                              "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    /* Broadcast a room message so operators watching the transcript see it too. */
    appendTaskRoomEvent(config, taskId, "all", "task.reclaim", "queued", message);
    char response[768];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"taskId\":\"%s\",\"reason\":\"%s\",\"previousAgent\":\"%s\","
             "\"previousAttempt\":%d}\n",
             taskId, reason, previousAgent, previousAttempt);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int handlePostAgentControl(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char agent[WT_TASK_AGENT_SIZE];
    char action[32];
    char message[WT_TASK_TITLE_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    if (wtJsonReadString(body, "agent", agent, sizeof(agent)) != 0 ||
        wtJsonReadString(body, "action", action, sizeof(action)) != 0 ||
        (strcmp(action, "pause") != 0 && strcmp(action, "resume") != 0)) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid agent control\"}\n");
    }
    if (strcmp(agent, "claude") != 0 && strcmp(agent, "chatgpt") != 0 && strcmp(agent, "gemini") != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"unknown agent\"}\n");
    }
    if (wtJsonReadString(body, "message", message, sizeof(message)) != 0) {
        snprintf(message, sizeof(message), "%s", strcmp(action, "pause") == 0 ?
                 "Agent paused by operator." : "Agent resumed by operator.");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "ceo");
    }
    char escapedAgent[WT_TASK_AGENT_SIZE * 2];
    char escapedAction[64];
    char escapedMessage[WT_TASK_TITLE_SIZE * 2];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    if (wtJsonEscape(agent, escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(action, escapedAction, sizeof(escapedAction)) != 0 ||
        wtJsonEscape(message, escapedMessage, sizeof(escapedMessage)) != 0 ||
        wtJsonEscape(createdBy, escapedBy, sizeof(escapedBy)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"agent control too large\"}\n");
    }
    char record[WT_TASK_LEDGER_LINE_SIZE];
    snprintf(record, sizeof(record),
             "{\"schema\":\"woventeam.agent_control.v0.1\",\"agent\":\"%s\","
             "\"action\":\"%s\",\"message\":\"%s\",\"createdBy\":\"%s\","
             "\"createdAtUnixMs\":%lld}",
             escapedAgent, escapedAction, escapedMessage, escapedBy, wtNowUnixMilliseconds());
    if (wtTaskAppendRecord(config->taskLedgerPath, record, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"agent\":\"%s\",\"action\":\"%s\"}\n", agent, action);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static const char *artifactKindForName(const char *name) {
    if (strcmp(name, "result.md") == 0) return "result";
    if (strcmp(name, "stdout.log") == 0) return "stdout";
    if (strcmp(name, "stderr.log") == 0) return "stderr";
    if (strcmp(name, "manifest.json") == 0) return "manifest";
    return "file";
}

static int appendArtifactTextField(char *body, size_t bodySize, size_t *used,
                                   const char *fieldName, const char *workspace,
                                   const char *fileName) {
    char path[WT_PATH_SIZE * 2];
    char text[12001];
    long size = 0;
    snprintf(path, sizeof(path), "%s/%s", workspace, fileName);
    readTextSnippet(path, text, sizeof(text), &size);
    if (appendJsonRaw(body, bodySize, used, ",\"") != 0 ||
        appendJsonRaw(body, bodySize, used, fieldName) != 0 ||
        appendJsonRaw(body, bodySize, used, "\":") != 0 ||
        appendJsonStringValue(body, bodySize, used, text) != 0) {
        return -1;
    }
    char metaName[64];
    snprintf(metaName, sizeof(metaName), "%sBytes", fieldName);
    char number[128];
    snprintf(number, sizeof(number), ",\"%s\":%ld", metaName, size);
    return appendJsonRaw(body, bodySize, used, number);
}

static int sendTaskArtifactsJson(int clientFd, const WtConfig *config, const char *taskId) {
    if (!isSafeArtifactName(taskId)) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json",
                              "{\"ok\":false,\"error\":\"invalid task id\"}\n");
    }
    char workspace[WT_PATH_SIZE * 2];
    snprintf(workspace, sizeof(workspace), "%s/%s", config->runtimeRootPath, taskId);
    char *body = malloc(WT_TASK_LEDGER_LINE_SIZE * 12);
    if (!body) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    size_t bodySize = WT_TASK_LEDGER_LINE_SIZE * 12;
    size_t used = 0;
    struct stat st;
    int exists = stat(workspace, &st) == 0 && S_ISDIR(st.st_mode);
    appendJsonRaw(body, bodySize, &used, "{\"ok\":true,\"taskId\":");
    appendJsonStringValue(body, bodySize, &used, taskId);
    appendJsonRaw(body, bodySize, &used, ",\"workspace\":");
    appendJsonStringValue(body, bodySize, &used, workspace);
    appendJsonRaw(body, bodySize, &used, exists ? ",\"exists\":true,\"files\":[" : ",\"exists\":false,\"files\":[");
    if (exists) {
        DIR *dir = opendir(workspace);
        int first = 1;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (!isSafeArtifactName(entry->d_name)) continue;
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
                char path[WT_PATH_SIZE * 4];
                snprintf(path, sizeof(path), "%s/%s", workspace, entry->d_name);
                struct stat fileStat;
                if (stat(path, &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) continue;
                if (!first) appendJsonRaw(body, bodySize, &used, ",");
                first = 0;
                appendJsonRaw(body, bodySize, &used, "{\"name\":");
                appendJsonStringValue(body, bodySize, &used, entry->d_name);
                appendJsonRaw(body, bodySize, &used, ",\"kind\":");
                appendJsonStringValue(body, bodySize, &used, artifactKindForName(entry->d_name));
                char number[128];
                snprintf(number, sizeof(number), ",\"bytes\":%ld}", (long)fileStat.st_size);
                appendJsonRaw(body, bodySize, &used, number);
            }
            closedir(dir);
        }
    }
    appendJsonRaw(body, bodySize, &used, "]");
    appendArtifactTextField(body, bodySize, &used, "resultText", workspace, "result.md");
    appendArtifactTextField(body, bodySize, &used, "stdoutText", workspace, "stdout.log");
    appendArtifactTextField(body, bodySize, &used, "stderrText", workspace, "stderr.log");
    appendArtifactTextField(body, bodySize, &used, "manifestText", workspace, "manifest.json");
    appendJsonRaw(body, bodySize, &used, "}\n");
    int rc = wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    free(body);
    return rc;
}

static int handlePostTaskEvent(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char schema[128];
    char taskId[WT_TASK_ID_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char message[WT_TASK_TITLE_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    if (wtJsonReadString(body, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "woventeam.task_event.v0.1") != 0 ||
        wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0 ||
        wtJsonReadString(body, "status", status, sizeof(status)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid task event\"}\n");
    }
    if (wtJsonReadString(body, "message", message, sizeof(message)) != 0) {
        snprintf(message, sizeof(message), "%s", "Task status updated.");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "system");
    }
    char compact[WT_TASK_LEDGER_LINE_SIZE];
    compactJsonLine(body, compact, sizeof(compact));
    const char *messageType = strcmp(status, "complete") == 0 || strcmp(status, "failed") == 0 ||
                              strcmp(status, "cancelled") == 0 || strcmp(status, "closed") == 0 ?
                              "task.result" : "task.status";
    if (wtTaskAppendRecord(config->taskLedgerPath, compact, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, taskId, "all", messageType, status, message) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    if (strcmp(status, "blocked") == 0 &&
        wtTaskAppendBlockedDependents(config->taskLedgerPath, taskId, createdBy, config->fsyncEachMessage) < 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"dependency propagation failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"status\":\"%s\",\"createdBy\":\"%s\"}\n",
             taskId, status, createdBy);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int handlePostTaskGate(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char taskId[WT_TASK_ID_SIZE];
    char action[32];
    char message[WT_TASK_TITLE_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    if (wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0 ||
        wtJsonReadString(body, "action", action, sizeof(action)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid gate action\"}\n");
    }
    if (strcmp(action, "approve") != 0 && strcmp(action, "reject") != 0 &&
        strcmp(action, "revision") != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"unsupported gate action\"}\n");
    }
    if (wtJsonReadString(body, "message", message, sizeof(message)) != 0) {
        snprintf(message, sizeof(message), "%s",
                 strcmp(action, "approve") == 0 ? "Task approved from review gate." :
                 strcmp(action, "reject") == 0 ? "Task rejected from review gate." :
                 "Revision requested from review gate.");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "ceo");
    }
    const char *status = strcmp(action, "approve") == 0 ? "approved" :
                         strcmp(action, "reject") == 0 ? "rejected" : "revision_requested";
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedStatus[WT_TASK_STATUS_SIZE * 2];
    char escapedMessage[WT_TASK_TITLE_SIZE * 2];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(status, escapedStatus, sizeof(escapedStatus)) != 0 ||
        wtJsonEscape(message, escapedMessage, sizeof(escapedMessage)) != 0 ||
        wtJsonEscape(createdBy, escapedBy, sizeof(escapedBy)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"gate action too large\"}\n");
    }
    char record[WT_TASK_LEDGER_LINE_SIZE];
    snprintf(record, sizeof(record),
             "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
             "\"eventType\":\"review_gate\",\"status\":\"%s\",\"message\":\"%s\","
             "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedStatus, escapedMessage, escapedBy, wtNowUnixMilliseconds());
    if (wtTaskAppendRecord(config->taskLedgerPath, record, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, taskId, "all", "task.status", status, message) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"status\":\"%s\"}\n", taskId, status);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int handlePostTaskUsage(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char taskId[WT_TASK_ID_SIZE];
    char provider[WT_NAME_SIZE];
    char modelId[WT_TASK_MODEL_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    long inputTokens = 0;
    long outputTokens = 0;
    long totalTokens = 0;
    long estimatedCostCents = 0;
    if (wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"task id required\"}\n");
    }
    if (wtJsonReadString(body, "provider", provider, sizeof(provider)) != 0) {
        snprintf(provider, sizeof(provider), "%s", "unknown");
    }
    if (wtJsonReadString(body, "modelId", modelId, sizeof(modelId)) != 0) {
        snprintf(modelId, sizeof(modelId), "%s", "");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "adapter");
    }
    wtJsonReadLong(body, "inputTokens", &inputTokens);
    wtJsonReadLong(body, "outputTokens", &outputTokens);
    wtJsonReadLong(body, "totalTokens", &totalTokens);
    wtJsonReadLong(body, "estimatedCostCents", &estimatedCostCents);
    if (inputTokens < 0) inputTokens = 0;
    if (outputTokens < 0) outputTokens = 0;
    if (totalTokens < 0) totalTokens = 0;
    if (estimatedCostCents < 0) estimatedCostCents = 0;
    if (totalTokens == 0) totalTokens = inputTokens + outputTokens;

    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedProvider[WT_NAME_SIZE * 2];
    char escapedModel[WT_TASK_MODEL_SIZE * 2];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(provider, escapedProvider, sizeof(escapedProvider)) != 0 ||
        wtJsonEscape(modelId, escapedModel, sizeof(escapedModel)) != 0 ||
        wtJsonEscape(createdBy, escapedBy, sizeof(escapedBy)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"usage too large\"}\n");
    }
    char record[WT_TASK_LEDGER_LINE_SIZE];
    snprintf(record, sizeof(record),
             "{\"schema\":\"woventeam.task_usage.v0.1\",\"taskId\":\"%s\","
             "\"provider\":\"%s\",\"modelId\":\"%s\",\"inputTokens\":%ld,"
             "\"outputTokens\":%ld,\"totalTokens\":%ld,\"estimatedCostCents\":%ld,"
             "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedProvider, escapedModel, inputTokens, outputTokens,
             totalTokens, estimatedCostCents, escapedBy, wtNowUnixMilliseconds());
    if (wtTaskAppendRecord(config->taskLedgerPath, record, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"totalTokens\":%ld,\"estimatedCostCents\":%ld}\n",
             taskId, totalTokens, estimatedCostCents);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static void sendSseEvent(int clientFd, const WtMessage *message) {
    char json[WT_MESSAGE_JSON_SIZE];
    char frame[WT_MESSAGE_JSON_SIZE + 64];
    wtMessageToJson(message, json, sizeof(json));
    int length = snprintf(frame, sizeof(frame), "event: message\ndata: %s\n\n", json);
    if (length > 0) {
        send(clientFd, frame, (size_t)length, 0);
    }
}

static int handleEvents(int clientFd, const WtConfig *config) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    if (send(clientFd, header, strlen(header), 0) < 0) {
        return -1;
    }
    long lastSentId = 0;
    for (int tick = 0; tick < 7200; tick++) {
        WtMessage messages[64];
        int count = wtRoomReadMessagesAfter(config->roomLogPath, lastSentId, messages, 64);
        for (int index = 0; index < count; index++) {
            sendSseEvent(clientFd, &messages[index]);
            lastSentId = messages[index].messageId;
        }
        if (count == 0) {
            send(clientFd, ": heartbeat\n\n", 13, 0);
        }
        sleepMilliseconds(500);
    }
    return 0;
}

static void handleClient(int clientFd, WtConfig *config) {
    char request[16384];
    size_t requestLength = 0;
    if (wtHttpReadRequest(clientFd, request, sizeof(request), &requestLength) != 0) {
        close(clientFd);
        return;
    }
    (void)requestLength;
    char method[16] = "";
    char path[256] = "";
    sscanf(request, "%15s %255s", method, path);
    if (config->configPath[0] != '\0') {
        wtConfigLoadFile(config, config->configPath);
        wtConfigApplyEnvironment(config);
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serveFile(clientFd, "web/index.html");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/app.js") == 0) {
        serveFile(clientFd, "web/app.js");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/style.css") == 0) {
        serveFile(clientFd, "web/style.css");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
        handleEvents(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/messages", 13) == 0) {
        int limit = 50;
        char *limitText = strstr(path, "limit=");
        if (limitText) limit = atoi(limitText + 6);
        sendMessagesJson(clientFd, config, limit);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tasks") == 0) {
        sendTasksJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/task-summaries") == 0) {
        sendTaskSummariesJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/task-detail", 16) == 0) {
        char taskId[WT_TASK_ID_SIZE] = "";
        char *taskIdText = strstr(path, "taskId=");
        if (taskIdText) {
            snprintf(taskId, sizeof(taskId), "%s", taskIdText + 7);
            char *amp = strchr(taskId, '&');
            if (amp) *amp = '\0';
        }
        sendTaskDetailJson(clientFd, config, taskId);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/initiatives") == 0) {
        sendInitiativesJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/initiative-detail", 22) == 0) {
        char initiativeId[WT_TASK_ID_SIZE] = "";
        char *idText = strstr(path, "initiativeId=");
        if (idText) {
            snprintf(initiativeId, sizeof(initiativeId), "%s", idText + 13);
            char *amp = strchr(initiativeId, '&');
            if (amp) *amp = '\0';
        }
        sendInitiativeDetailJson(clientFd, config, initiativeId);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/task-artifacts", 19) == 0) {
        char taskId[WT_TASK_ID_SIZE] = "";
        char *taskIdText = strstr(path, "taskId=");
        if (taskIdText) {
            snprintf(taskId, sizeof(taskId), "%s", taskIdText + 7);
            char *amp = strchr(taskId, '&');
            if (amp) *amp = '\0';
        }
        sendTaskArtifactsJson(clientFd, config, taskId);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/capacity") == 0) {
        sendCapacityJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/agents") == 0) {
        sendAgentsJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tokens") == 0) {
        sendTokenJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
        sendConfigJson(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/adapters") == 0) {
        sendAdaptersJson(clientFd, config);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/message") == 0) {
        handlePostMessage(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-package") == 0) {
        handlePostTaskPackage(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-request") == 0) {
        handlePostTaskRequest(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-event") == 0) {
        handlePostTaskEvent(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-gate") == 0) {
        handlePostTaskGate(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-usage") == 0) {
        handlePostTaskUsage(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/agent-control") == 0) {
        handlePostAgentControl(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-reclaim") == 0) {
        handlePostTaskReclaim(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-artifact") == 0) {
        handlePostTaskArtifact(clientFd, config, request);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/initiative-artifacts", 25) == 0) {
        char initiativeId[WT_TASK_ID_SIZE] = "";
        char *idText = strstr(path, "initiativeId=");
        if (idText) {
            snprintf(initiativeId, sizeof(initiativeId), "%s", idText + 13);
            char *amp = strchr(initiativeId, '&');
            if (amp) *amp = '\0';
        }
        sendInitiativeArtifactsJson(clientFd, config, initiativeId);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        handlePostConfig(clientFd, config, request);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/health") == 0) {
        int projectionOk = rebuildProjection(config) == 0;
        int ledgerOk = access(config->taskLedgerPath, F_OK) == 0;
        int roomLogOk = access(config->roomLogPath, F_OK) == 0;
        char body[2048];
        snprintf(body, sizeof(body),
                 "{\"ok\":%s,\"roomName\":\"%s\",\"ledger\":{\"path\":\"%s\",\"exists\":%s},"
                 "\"roomLog\":{\"path\":\"%s\",\"exists\":%s},"
                 "\"projection\":{\"path\":\"%s\",\"ok\":%s},"
                 "\"adapters\":{\"codexEnabled\":%s,\"claudeEnabled\":%s,\"geminiEnabled\":%s}}\n",
                 projectionOk ? "true" : "false",
                 config->roomName,
                 config->taskLedgerPath, ledgerOk ? "true" : "false",
                 config->roomLogPath, roomLogOk ? "true" : "false",
                 config->taskProjectionDbPath, projectionOk ? "true" : "false",
                 config->enableCodexAdapter ? "true" : "false",
                 config->enableClaudeAdapter ? "true" : "false",
                 config->enableGeminiAdapter ? "true" : "false");
        wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    } else {
        wtHttpSendText(clientFd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
    }
    close(clientFd);
}

static int createServerSocket(const WtConfig *config) {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config->httpPort);
    if (inet_pton(AF_INET, config->httpBindAddress, &address.sin_addr) != 1) {
        close(serverFd);
        return -1;
    }
    if (bind(serverFd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(serverFd, 32) != 0) {
        close(serverFd);
        return -1;
    }
    return serverFd;
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            wtConfigLoadFile(&config, argv[++index]);
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            config.httpPort = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            snprintf(config.httpBindAddress, sizeof(config.httpBindAddress), "%s", argv[++index]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    wtConfigApplyEnvironment(&config);
    if (rebuildProjection(&config) != 0) {
        fprintf(stderr, "warning: task projection rebuild failed; ledger APIs remain available\n");
    }
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    int serverFd = createServerSocket(&config);
    if (serverFd < 0) {
        perror("start wt-roomd");
        return 1;
    }
    printf("wt-roomd listening on http://%s:%d/ using %s\n",
           config.httpBindAddress, config.httpPort, config.roomLogPath);
    fflush(stdout);
    while (1) {
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pid_t child = fork();
        if (child == 0) {
            close(serverFd);
            handleClient(clientFd, &config);
            _exit(0);
        }
        close(clientFd);
    }
    close(serverFd);
    return 1;
}
