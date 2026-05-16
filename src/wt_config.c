/*
 * wt_config.c - Plain key/value configuration for the native Phase 0 room.
 *
 * The parser intentionally accepts only simple KEY=VALUE lines. That keeps the
 * runtime inspectable and avoids carrying a config dependency into the core.
 */
#include "wt_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copyString(char *destination, size_t destinationSize, const char *source) {
    if (destinationSize == 0) {
        return;
    }
    snprintf(destination, destinationSize, "%s", source ? source : "");
}

static char *trimWhitespace(char *value) {
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    size_t length = strlen(value);
    while (length > 0 && isspace((unsigned char)value[length - 1])) {
        value[--length] = '\0';
    }
    return value;
}

void wtConfigInitDefaults(WtConfig *config) {
    copyString(config->roomName, sizeof(config->roomName), "phase0");
    copyString(config->roomLogPath, sizeof(config->roomLogPath), "data/phase0-room.jsonl");
    copyString(config->taskLedgerPath, sizeof(config->taskLedgerPath), "data/task-packages.jsonl");
    copyString(config->httpBindAddress, sizeof(config->httpBindAddress), "0.0.0.0");
    config->httpPort = 8787;
    config->contextMessageCount = 20;
    config->agentPollMilliseconds = 1000;
    config->adapterTimeoutSeconds = 1800;
    config->adapterMaxOutputBytes = 1048576;
    config->fsyncEachMessage = false;
    config->enableCodexAdapter = false;
    copyString(config->runtimeRootPath, sizeof(config->runtimeRootPath), "/woventeam/runtime/tasks");
    copyString(config->claudeMode, sizeof(config->claudeMode), "stub");
    copyString(config->chatgptMode, sizeof(config->chatgptMode), "stub");
    copyString(config->geminiMode, sizeof(config->geminiMode), "stub");
    copyString(config->claudeCommand, sizeof(config->claudeCommand), "claude");
    copyString(config->gptCommand, sizeof(config->gptCommand), "codex");
    copyString(config->geminiCommand, sizeof(config->geminiCommand), "gemini");
}

int wtConfigSetValue(WtConfig *config, const char *key, const char *value) {
    if (strcmp(key, "roomName") == 0) {
        copyString(config->roomName, sizeof(config->roomName), value);
    } else if (strcmp(key, "roomLogPath") == 0) {
        copyString(config->roomLogPath, sizeof(config->roomLogPath), value);
    } else if (strcmp(key, "taskLedgerPath") == 0) {
        copyString(config->taskLedgerPath, sizeof(config->taskLedgerPath), value);
    } else if (strcmp(key, "httpBindAddress") == 0) {
        copyString(config->httpBindAddress, sizeof(config->httpBindAddress), value);
    } else if (strcmp(key, "httpPort") == 0) {
        config->httpPort = atoi(value);
    } else if (strcmp(key, "contextMessageCount") == 0) {
        config->contextMessageCount = atoi(value);
    } else if (strcmp(key, "agentPollMilliseconds") == 0) {
        config->agentPollMilliseconds = atoi(value);
    } else if (strcmp(key, "adapterTimeoutSeconds") == 0) {
        config->adapterTimeoutSeconds = atoi(value);
    } else if (strcmp(key, "adapterMaxOutputBytes") == 0) {
        config->adapterMaxOutputBytes = atoi(value);
    } else if (strcmp(key, "fsyncEachMessage") == 0) {
        config->fsyncEachMessage = atoi(value) != 0;
    } else if (strcmp(key, "enableCodexAdapter") == 0) {
        config->enableCodexAdapter = atoi(value) != 0;
    } else if (strcmp(key, "runtimeRootPath") == 0) {
        copyString(config->runtimeRootPath, sizeof(config->runtimeRootPath), value);
    } else if (strcmp(key, "claudeMode") == 0) {
        copyString(config->claudeMode, sizeof(config->claudeMode), value);
    } else if (strcmp(key, "chatgptMode") == 0) {
        copyString(config->chatgptMode, sizeof(config->chatgptMode), value);
    } else if (strcmp(key, "geminiMode") == 0) {
        copyString(config->geminiMode, sizeof(config->geminiMode), value);
    } else if (strcmp(key, "claudeCommand") == 0) {
        copyString(config->claudeCommand, sizeof(config->claudeCommand), value);
    } else if (strcmp(key, "gptCommand") == 0) {
        copyString(config->gptCommand, sizeof(config->gptCommand), value);
    } else if (strcmp(key, "geminiCommand") == 0) {
        copyString(config->geminiCommand, sizeof(config->geminiCommand), value);
    } else {
        return -1;
    }
    return 0;
}

int wtConfigLoadFile(WtConfig *config, const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char *trimmed = trimWhitespace(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }
        char *equals = strchr(trimmed, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        char *key = trimWhitespace(trimmed);
        char *value = trimWhitespace(equals + 1);
        wtConfigSetValue(config, key, value);
    }
    fclose(file);
    return 0;
}

void wtConfigApplyEnvironment(WtConfig *config) {
    const char *value = NULL;
    if ((value = getenv("WT_ROOM_NAME"))) wtConfigSetValue(config, "roomName", value);
    if ((value = getenv("WT_ROOM_LOG_PATH"))) wtConfigSetValue(config, "roomLogPath", value);
    if ((value = getenv("WT_TASK_LEDGER_PATH"))) wtConfigSetValue(config, "taskLedgerPath", value);
    if ((value = getenv("WT_HTTP_BIND_ADDRESS"))) wtConfigSetValue(config, "httpBindAddress", value);
    if ((value = getenv("WT_HTTP_PORT"))) wtConfigSetValue(config, "httpPort", value);
    if ((value = getenv("WT_CONTEXT_MESSAGE_COUNT"))) wtConfigSetValue(config, "contextMessageCount", value);
    if ((value = getenv("WT_AGENT_POLL_MILLISECONDS"))) wtConfigSetValue(config, "agentPollMilliseconds", value);
    if ((value = getenv("WT_ADAPTER_TIMEOUT_SECONDS"))) wtConfigSetValue(config, "adapterTimeoutSeconds", value);
    if ((value = getenv("WT_ADAPTER_MAX_OUTPUT_BYTES"))) wtConfigSetValue(config, "adapterMaxOutputBytes", value);
    if ((value = getenv("WT_FSYNC_EACH_MESSAGE"))) wtConfigSetValue(config, "fsyncEachMessage", value);
    if ((value = getenv("WT_ENABLE_CODEX_ADAPTER"))) wtConfigSetValue(config, "enableCodexAdapter", value);
    if ((value = getenv("WT_RUNTIME_ROOT_PATH"))) wtConfigSetValue(config, "runtimeRootPath", value);
    if ((value = getenv("WT_CLAUDE_COMMAND"))) wtConfigSetValue(config, "claudeCommand", value);
    if ((value = getenv("WT_GPT_COMMAND"))) wtConfigSetValue(config, "gptCommand", value);
    if ((value = getenv("WT_GEMINI_COMMAND"))) wtConfigSetValue(config, "geminiCommand", value);
}
