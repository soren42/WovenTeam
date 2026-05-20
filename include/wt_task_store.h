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
/*
 * Like wtTaskFindQueuedForAgent but also surfaces tasks whose status is "leased"
 * or "running" if the latest lease for that task has expired (lease expiry is
 * compared against nowUnixMs). This lets wt-agent recover stuck work without
 * requiring a manual operator reclaim first.
 */
int wtTaskFindClaimableForAgent(const char *ledgerPath, const char *agentName,
                                long long nowUnixMs, WtTaskSummary *task);
int wtTaskAgentPaused(const char *ledgerPath, const char *agentName);
int wtTaskAppendLeaseEvent(const char *ledgerPath, const char *taskId, const char *agentName,
                           int attempt, long long leaseExpiresAtUnixMs, bool fsyncRecord);
int wtTaskAppendStatusEvent(const char *ledgerPath, const char *taskId, const char *status,
                            const char *createdBy, const char *message, bool fsyncRecord);
/*
 * Append a "status" task event that also carries a classified retry/failure cause.
 * Phase 2 Sprint 3 closeout: failure causes are projected so the operator can see
 * why a task ended in `failed` without reading raw event JSON. retryCause may be
 * NULL/empty for non-failure transitions (the helper then degrades to the plain
 * status-event shape).
 */
int wtTaskAppendStatusEventWithCause(const char *ledgerPath, const char *taskId, const char *status,
                                     const char *createdBy, const char *message,
                                     const char *retryCause, bool fsyncRecord);
/*
 * Append a reclaim event: the latest lease is released so another agent (or the
 * same agent after restart) can re-claim. reason is a short classifier like
 * "lease_expired" or "operator"; reclaimedBy is the actor releasing the lease.
 */
int wtTaskAppendReclaimEvent(const char *ledgerPath, const char *taskId, const char *previousAgent,
                             const char *reclaimedBy, const char *reason, const char *message,
                             bool fsyncRecord);
/*
 * Append an artifact decision event. Sprint 4 turns raw adapter output into a
 * reviewed asset by recording a state transition for an artifact path.
 *   state: draft | reviewed | accepted | rejected | superseded
 *   reviewer: human or system identifier crediting the decision
 *   notes: free-text justification (recorded verbatim)
 *   artifactPath: path inside the runtime workspace (e.g. result.md)
 * The event is a normal task_event with eventType="artifact" so projection,
 * recovery, and ledger semantics carry over unchanged.
 */
int wtTaskAppendArtifactEvent(const char *ledgerPath, const char *taskId, const char *state,
                              const char *reviewer, const char *notes, const char *artifactPath,
                              bool fsyncRecord);
/*
 * Inspect the most recent lease for a task. Returns 1 when a lease line is found,
 * 0 when no lease exists or the latest lease was already released by a reclaim
 * event. Out-params receive the lease holder, expiry, and attempt count. Used by
 * wt-agent before claiming work to decide whether the task is truly available.
 */
int wtTaskFindActiveLease(const char *ledgerPath, const char *taskId,
                          char *leaseHolder, size_t leaseHolderSize,
                          long long *leaseExpiresAtUnixMs, int *attempt);
int wtTaskAppendBlockedDependents(const char *ledgerPath, const char *blockedTaskId,
                                  const char *createdBy, bool fsyncRecord);
int wtTaskSummarizeTokenBudgets(const char *ledgerPath, long long nowUnixMs, WtTokenSummary *summary);

#endif
