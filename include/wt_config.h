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
    /*
     * Phase 3 Sprint 1 (runtime observability + control plane).
     *   heartbeatIntervalSeconds   - wt-agent emits a woventeam.heartbeat.v0.1
     *                                record this often. Default 7200 (2 h).
     *   leaseRenewalIntervalSeconds - while an adapter is running, the agent
     *                                appends a renewal lease event this often.
     *                                Default 600 (10 min). The renewal advances
     *                                leaseExpiresAtUnixMs but keeps lease owner
     *                                and attempt number stable.
     *   leaseDurationSeconds       - new lease + each renewal extends the
     *                                expiry by this many seconds. Default 900
     *                                (15 min, matching the prior hard-coded
     *                                value).
     *   cancelPollIntervalSeconds  - wt-agent polls the ledger for cancel
     *                                events this often inside the adapter
     *                                wait loop. Default 5.
     *   slackWebhookFile           - path to the keyed Slack webhook file
     *                                (KEY=URL, one per line). Defaults to
     *                                config/slack_webhook.txt.
     *   notificationEscalateKey    - webhook key used for escalate / kill /
     *                                stuck events. Empty disables.
     *   notificationStuckKey       - webhook key used for stuck-lease events.
     *                                Falls back to notificationEscalateKey
     *                                when empty.
     */
    int heartbeatIntervalSeconds;
    int leaseRenewalIntervalSeconds;
    int leaseDurationSeconds;
    int cancelPollIntervalSeconds;
    char slackWebhookFile[WT_PATH_SIZE];
    char notificationEscalateKey[WT_NAME_SIZE];
    char notificationStuckKey[WT_NAME_SIZE];
    /*
     * Phase 3 Sprint 2 autonomy defaults. Empty per-agent values mean derive
     * from toolPolicy.profile; explicit values are observe, ask-each,
     * ask-batch, or autonomous.
     */
    char defaultAutonomyLevel[WT_NAME_SIZE];
    char claudeDefaultAutonomyLevel[WT_NAME_SIZE];
    char chatgptDefaultAutonomyLevel[WT_NAME_SIZE];
    char geminiDefaultAutonomyLevel[WT_NAME_SIZE];
    /*
     * Phase 3 Sprint 3 (deliverables pipeline).
     *   deliverableRoot            - directory root for shipped deliverables.
     *                                Each ship lands under
     *                                <deliverableRoot>/<initiativeId>/. The
     *                                daemon mkdir -p's the per-initiative
     *                                subdirectory on first use.
     *                                Default: data/deliverables.
     *   deliverableDefaultMode     - packaging mode when the operator does
     *                                not specify one. One of copy, tarball,
     *                                branch, pull-request. Default: copy.
     *   deliverableBranchPrefix    - branch + pull-request modes commit to
     *                                <prefix>/<initiativeId>. Default:
     *                                deliverables.
     *   secretScanPatternsFile     - path to a patterns file (one
     *                                NAME=REGEX line per pattern, # comments).
     *                                Empty disables scan-on-ship. When set,
     *                                copy/tarball scan and record (do not
     *                                block); branch/pull-request refuse to
     *                                ship on a positive match. Defaults to
     *                                config/secret-scan-patterns.txt which
     *                                ships with conservative coverage for
     *                                GitHub PATs, AWS keys, OpenSSH private
     *                                keys, and a few API-token shapes.
     */
    char deliverableRoot[WT_PATH_SIZE];
    char deliverableDefaultMode[WT_NAME_SIZE];
    char deliverableBranchPrefix[WT_NAME_SIZE];
    char secretScanPatternsFile[WT_PATH_SIZE];
} WtConfig;

void wtConfigInitDefaults(WtConfig *config);
int wtConfigLoadFile(WtConfig *config, const char *path);
int wtConfigWriteFile(const WtConfig *config, const char *path);
void wtConfigApplyEnvironment(WtConfig *config);
int wtConfigSetValue(WtConfig *config, const char *key, const char *value);

#endif
