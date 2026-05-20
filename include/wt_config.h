/*
 * wt_config.h - Small Phase 0 configuration surface.
 *
 * The native room spike keeps configuration deliberately plain: built-in
 * defaults, optional key=value file, environment overrides, and a few command
 * line flags used by individual binaries.
 */
#ifndef WT_CONFIG_H
#define WT_CONFIG_H

#include <stdbool.h>

#define WT_PATH_SIZE 256
#define WT_NAME_SIZE 64

typedef struct {
    char configPath[WT_PATH_SIZE];
    char roomName[WT_NAME_SIZE];
    char roomLogPath[WT_PATH_SIZE];
    char taskLedgerPath[WT_PATH_SIZE];
    char taskProjectionDbPath[WT_PATH_SIZE];
    char httpBindAddress[WT_NAME_SIZE];
    int httpPort;
    int contextMessageCount;
    int agentPollMilliseconds;
    int adapterTimeoutSeconds;
    int adapterMaxOutputBytes;
    int maxActiveTasksPerAgent;
    int maxSubtasksPerParent;
    int maxTasksPerInitiative;
    bool fsyncEachMessage;
    bool roleRoutingEnabled;
    bool enableCodexAdapter;
    bool enableClaudeAdapter;
    bool enableGeminiAdapter;
    bool tokenTelemetryEnabled;
    long tokenDailyBudget;
    long tokenMonthlyBudget;
    int tokenWarningPercent;
    int tokenCostPerMillionCents;
    char runtimeRootPath[WT_PATH_SIZE];
    char claudeMode[WT_NAME_SIZE];
    char chatgptMode[WT_NAME_SIZE];
    char geminiMode[WT_NAME_SIZE];
    char claudeCommand[WT_PATH_SIZE];
    char gptCommand[WT_PATH_SIZE];
    char geminiCommand[WT_PATH_SIZE];
    /*
     * Sprint 5 policy + budget settings.
     *   blockedVendors          - comma-separated vendor prefixes that the
     *                             central policy evaluator refuses to accept on
     *                             new task packages (e.g. "deepseek,xai").
     *   tokenBudgetPerInitiative - per-initiative allocation cap (0 disables).
     *                             Compared against the sum of maxTokens across
     *                             active task packages with the same initiativeId.
     *   tokenBudgetPerModelFamily - per-model-family allocation cap (0 disables).
     *                              Family = prefix before the first '/' in modelId.
     */
    char blockedVendors[WT_PATH_SIZE];
    long tokenBudgetPerInitiative;
    long tokenBudgetPerModelFamily;
} WtConfig;

void wtConfigInitDefaults(WtConfig *config);
int wtConfigLoadFile(WtConfig *config, const char *path);
int wtConfigWriteFile(const WtConfig *config, const char *path);
void wtConfigApplyEnvironment(WtConfig *config);
int wtConfigSetValue(WtConfig *config, const char *key, const char *value);

#endif
