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
    copyString(config->configPath, sizeof(config->configPath), "");
    copyString(config->roomName, sizeof(config->roomName), "phase0");
    copyString(config->roomLogPath, sizeof(config->roomLogPath), "data/phase0-room.jsonl");
    copyString(config->taskLedgerPath, sizeof(config->taskLedgerPath), "data/task-packages.jsonl");
    copyString(config->taskProjectionDbPath, sizeof(config->taskProjectionDbPath), "data/task-projection.sqlite");
    copyString(config->httpBindAddress, sizeof(config->httpBindAddress), "0.0.0.0");
    config->httpPort = 8787;
    config->contextMessageCount = 20;
    config->agentPollMilliseconds = 1000;
    config->adapterTimeoutSeconds = 1800;
    config->adapterMaxOutputBytes = 1048576;
    config->maxActiveTasksPerAgent = 4;
    config->maxSubtasksPerParent = 8;
    config->maxTasksPerInitiative = 32;
    config->fsyncEachMessage = false;
    config->roleRoutingEnabled = true;
    config->enableCodexAdapter = false;
    config->enableClaudeAdapter = false;
    config->enableGeminiAdapter = false;
    config->tokenTelemetryEnabled = true;
    config->tokenDailyBudget = 2000000;
    config->tokenMonthlyBudget = 50000000;
    config->tokenWarningPercent = 80;
    config->tokenCostPerMillionCents = 1000;
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
    } else if (strcmp(key, "taskProjectionDbPath") == 0) {
        copyString(config->taskProjectionDbPath, sizeof(config->taskProjectionDbPath), value);
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
    } else if (strcmp(key, "maxActiveTasksPerAgent") == 0) {
        config->maxActiveTasksPerAgent = atoi(value);
    } else if (strcmp(key, "maxSubtasksPerParent") == 0) {
        config->maxSubtasksPerParent = atoi(value);
    } else if (strcmp(key, "maxTasksPerInitiative") == 0) {
        config->maxTasksPerInitiative = atoi(value);
    } else if (strcmp(key, "fsyncEachMessage") == 0) {
        config->fsyncEachMessage = atoi(value) != 0;
    } else if (strcmp(key, "roleRoutingEnabled") == 0) {
        config->roleRoutingEnabled = atoi(value) != 0;
    } else if (strcmp(key, "enableCodexAdapter") == 0) {
        config->enableCodexAdapter = atoi(value) != 0;
    } else if (strcmp(key, "enableClaudeAdapter") == 0) {
        config->enableClaudeAdapter = atoi(value) != 0;
    } else if (strcmp(key, "enableGeminiAdapter") == 0) {
        config->enableGeminiAdapter = atoi(value) != 0;
    } else if (strcmp(key, "tokenTelemetryEnabled") == 0) {
        config->tokenTelemetryEnabled = atoi(value) != 0;
    } else if (strcmp(key, "tokenDailyBudget") == 0) {
        config->tokenDailyBudget = atol(value);
    } else if (strcmp(key, "tokenMonthlyBudget") == 0) {
        config->tokenMonthlyBudget = atol(value);
    } else if (strcmp(key, "tokenWarningPercent") == 0) {
        config->tokenWarningPercent = atoi(value);
    } else if (strcmp(key, "tokenCostPerMillionCents") == 0) {
        config->tokenCostPerMillionCents = atoi(value);
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
    char pathCopy[WT_PATH_SIZE];
    copyString(pathCopy, sizeof(pathCopy), path);
    FILE *file = fopen(pathCopy, "r");
    if (!file) {
        return -1;
    }
    copyString(config->configPath, sizeof(config->configPath), pathCopy);
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

int wtConfigWriteFile(const WtConfig *config, const char *path) {
    FILE *file = fopen(path, "w");
    if (!file) {
        return -1;
    }
    int ok = fprintf(file,
        "roomName=%s\n"
        "roomLogPath=%s\n"
        "taskLedgerPath=%s\n"
        "taskProjectionDbPath=%s\n"
        "httpBindAddress=%s\n"
        "httpPort=%d\n"
        "contextMessageCount=%d\n"
        "agentPollMilliseconds=%d\n"
        "adapterTimeoutSeconds=%d\n"
        "adapterMaxOutputBytes=%d\n"
        "maxActiveTasksPerAgent=%d\n"
        "maxSubtasksPerParent=%d\n"
        "maxTasksPerInitiative=%d\n"
        "fsyncEachMessage=%d\n"
        "roleRoutingEnabled=%d\n"
        "enableCodexAdapter=%d\n"
        "enableClaudeAdapter=%d\n"
        "enableGeminiAdapter=%d\n"
        "tokenTelemetryEnabled=%d\n"
        "tokenDailyBudget=%ld\n"
        "tokenMonthlyBudget=%ld\n"
        "tokenWarningPercent=%d\n"
        "tokenCostPerMillionCents=%d\n"
        "runtimeRootPath=%s\n"
        "claudeMode=%s\n"
        "chatgptMode=%s\n"
        "geminiMode=%s\n"
        "claudeCommand=%s\n"
        "gptCommand=%s\n"
        "geminiCommand=%s\n",
        config->roomName,
        config->roomLogPath,
        config->taskLedgerPath,
        config->taskProjectionDbPath,
        config->httpBindAddress,
        config->httpPort,
        config->contextMessageCount,
        config->agentPollMilliseconds,
        config->adapterTimeoutSeconds,
        config->adapterMaxOutputBytes,
        config->maxActiveTasksPerAgent,
        config->maxSubtasksPerParent,
        config->maxTasksPerInitiative,
        config->fsyncEachMessage ? 1 : 0,
        config->roleRoutingEnabled ? 1 : 0,
        config->enableCodexAdapter ? 1 : 0,
        config->enableClaudeAdapter ? 1 : 0,
        config->enableGeminiAdapter ? 1 : 0,
        config->tokenTelemetryEnabled ? 1 : 0,
        config->tokenDailyBudget,
        config->tokenMonthlyBudget,
        config->tokenWarningPercent,
        config->tokenCostPerMillionCents,
        config->runtimeRootPath,
        config->claudeMode,
        config->chatgptMode,
        config->geminiMode,
        config->claudeCommand,
        config->gptCommand,
        config->geminiCommand) > 0;
    return fclose(file) == 0 && ok ? 0 : -1;
}

void wtConfigApplyEnvironment(WtConfig *config) {
    const char *value = NULL;
    if ((value = getenv("WT_ROOM_NAME"))) wtConfigSetValue(config, "roomName", value);
    if ((value = getenv("WT_ROOM_LOG_PATH"))) wtConfigSetValue(config, "roomLogPath", value);
    if ((value = getenv("WT_TASK_LEDGER_PATH"))) wtConfigSetValue(config, "taskLedgerPath", value);
    if ((value = getenv("WT_TASK_PROJECTION_DB_PATH"))) wtConfigSetValue(config, "taskProjectionDbPath", value);
    if ((value = getenv("WT_HTTP_BIND_ADDRESS"))) wtConfigSetValue(config, "httpBindAddress", value);
    if ((value = getenv("WT_HTTP_PORT"))) wtConfigSetValue(config, "httpPort", value);
    if ((value = getenv("WT_CONTEXT_MESSAGE_COUNT"))) wtConfigSetValue(config, "contextMessageCount", value);
    if ((value = getenv("WT_AGENT_POLL_MILLISECONDS"))) wtConfigSetValue(config, "agentPollMilliseconds", value);
    if ((value = getenv("WT_ADAPTER_TIMEOUT_SECONDS"))) wtConfigSetValue(config, "adapterTimeoutSeconds", value);
    if ((value = getenv("WT_ADAPTER_MAX_OUTPUT_BYTES"))) wtConfigSetValue(config, "adapterMaxOutputBytes", value);
    if ((value = getenv("WT_MAX_ACTIVE_TASKS_PER_AGENT"))) wtConfigSetValue(config, "maxActiveTasksPerAgent", value);
    if ((value = getenv("WT_MAX_SUBTASKS_PER_PARENT"))) wtConfigSetValue(config, "maxSubtasksPerParent", value);
    if ((value = getenv("WT_MAX_TASKS_PER_INITIATIVE"))) wtConfigSetValue(config, "maxTasksPerInitiative", value);
    if ((value = getenv("WT_FSYNC_EACH_MESSAGE"))) wtConfigSetValue(config, "fsyncEachMessage", value);
    if ((value = getenv("WT_ROLE_ROUTING_ENABLED"))) wtConfigSetValue(config, "roleRoutingEnabled", value);
    if ((value = getenv("WT_ENABLE_CODEX_ADAPTER"))) wtConfigSetValue(config, "enableCodexAdapter", value);
    if ((value = getenv("WT_ENABLE_CLAUDE_ADAPTER"))) wtConfigSetValue(config, "enableClaudeAdapter", value);
    if ((value = getenv("WT_ENABLE_GEMINI_ADAPTER"))) wtConfigSetValue(config, "enableGeminiAdapter", value);
    if ((value = getenv("WT_TOKEN_TELEMETRY_ENABLED"))) wtConfigSetValue(config, "tokenTelemetryEnabled", value);
    if ((value = getenv("WT_TOKEN_DAILY_BUDGET"))) wtConfigSetValue(config, "tokenDailyBudget", value);
    if ((value = getenv("WT_TOKEN_MONTHLY_BUDGET"))) wtConfigSetValue(config, "tokenMonthlyBudget", value);
    if ((value = getenv("WT_TOKEN_WARNING_PERCENT"))) wtConfigSetValue(config, "tokenWarningPercent", value);
    if ((value = getenv("WT_TOKEN_COST_PER_MILLION_CENTS"))) wtConfigSetValue(config, "tokenCostPerMillionCents", value);
    if ((value = getenv("WT_RUNTIME_ROOT_PATH"))) wtConfigSetValue(config, "runtimeRootPath", value);
    if ((value = getenv("WT_CLAUDE_COMMAND"))) wtConfigSetValue(config, "claudeCommand", value);
    if ((value = getenv("WT_GPT_COMMAND"))) wtConfigSetValue(config, "gptCommand", value);
    if ((value = getenv("WT_GEMINI_COMMAND"))) wtConfigSetValue(config, "geminiCommand", value);
    if ((value = getenv("WT_CLAUDE_MODE"))) wtConfigSetValue(config, "claudeMode", value);
    if ((value = getenv("WT_CHATGPT_MODE"))) wtConfigSetValue(config, "chatgptMode", value);
    if ((value = getenv("WT_GEMINI_MODE"))) wtConfigSetValue(config, "geminiMode", value);
}
