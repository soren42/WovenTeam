/*
 * wt_task_store.h - Append-only task package ledger helpers.
 */
#ifndef WT_TASK_STORE_H
#define WT_TASK_STORE_H

#include <stdbool.h>
#include <stddef.h>

#define WT_TASK_ID_SIZE 128
#define WT_TASK_AGENT_SIZE 64
#define WT_TASK_STATUS_SIZE 32
#define WT_TASK_TITLE_SIZE 192
#define WT_TASK_BODY_SIZE 2048
#define WT_TASK_POLICY_SIZE 64
#define WT_TASK_MODEL_SIZE 128
#define WT_TASK_LEDGER_LINE_SIZE 16384

typedef struct {
    char taskId[WT_TASK_ID_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char assignedRole[WT_TASK_AGENT_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char title[WT_TASK_TITLE_SIZE];
    char body[WT_TASK_BODY_SIZE];
    char modelId[WT_TASK_MODEL_SIZE];
    char toolProfile[WT_TASK_POLICY_SIZE];
    int timeoutSeconds;
    int maxOutputBytes;
    long long updatedAtUnixMs;
} WtTaskSummary;

typedef struct {
    long long dayWindowAllocatedTokens;
    long long monthWindowAllocatedTokens;
    long long allTimeAllocatedTokens;
    long long dayWindowActualTokens;
    long long monthWindowActualTokens;
    long long allTimeActualTokens;
    long long dayWindowActualCostCents;
    long long monthWindowActualCostCents;
    long long allTimeActualCostCents;
    int dayWindowPackages;
    int monthWindowPackages;
    int allTimePackages;
    int dayWindowUsageEvents;
    int monthWindowUsageEvents;
    int allTimeUsageEvents;
} WtTokenSummary;

int wtTaskAppendRecord(const char *ledgerPath, const char *jsonLine, bool fsyncRecord);
int wtTaskReadAllJsonArray(const char *ledgerPath, char *buffer, size_t bufferSize);
int wtTaskFindQueuedForAgent(const char *ledgerPath, const char *agentName, WtTaskSummary *task);
int wtTaskAgentPaused(const char *ledgerPath, const char *agentName);
int wtTaskAppendLeaseEvent(const char *ledgerPath, const char *taskId, const char *agentName,
                           int attempt, long long leaseExpiresAtUnixMs, bool fsyncRecord);
int wtTaskAppendStatusEvent(const char *ledgerPath, const char *taskId, const char *status,
                            const char *createdBy, const char *message, bool fsyncRecord);
int wtTaskAppendBlockedDependents(const char *ledgerPath, const char *blockedTaskId,
                                  const char *createdBy, bool fsyncRecord);
int wtTaskSummarizeTokenBudgets(const char *ledgerPath, long long nowUnixMs, WtTokenSummary *summary);

#endif
