/*
 * wt_task_projection.h - Rebuildable SQLite projection for the task ledger.
 */
#ifndef WT_TASK_PROJECTION_H
#define WT_TASK_PROJECTION_H

#include <stddef.h>

int wtTaskProjectionRebuild(const char *dbPath, const char *ledgerPath);
int wtTaskProjectionReadSummariesJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionReadDetailJson(const char *dbPath, const char *taskId, char *buffer, size_t bufferSize);
int wtTaskProjectionReadInitiativesJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionReadInitiativeDetailJson(const char *dbPath, const char *initiativeId, char *buffer, size_t bufferSize);
/*
 * Sprint 4: accepted-asset inventory for an initiative. Returns
 *   { ok: true, initiativeId, artifacts: [ { taskId, title, assignedAgent,
 *     artifactState, lastReviewer, lastReviewNotes, acceptedAtUnixMs,
 *     acceptedArtifactPath } ... ] }
 * Includes only tasks where artifact_state != ''. Sorted by accepted_at_ms DESC
 * with pending decisions (state != 'accepted') trailing.
 */
int wtTaskProjectionReadInitiativeArtifactsJson(const char *dbPath, const char *initiativeId, char *buffer, size_t bufferSize);
/*
 * Sprint 5 combined audit report. One JSON document covering:
 *   - initiative summary (counts, budget, timestamps)
 *   - every task under the initiative with full detail row
 *   - every task event (status, lease, reclaim, artifact, gate, etc.)
 *   - policy decisions (denials recorded as woventeam.policy_decision.v0.1)
 *   - usage events (token consumption)
 * The caller controls the buffer size; auditBuffer is typically 256 KiB or
 * larger because initiatives can accumulate hundreds of events.
 */
int wtTaskProjectionReadInitiativeAuditJson(const char *dbPath, const char *ledgerPath,
                                            const char *initiativeId,
                                            char *buffer, size_t bufferSize);
int wtTaskProjectionReadAgentsJson(const char *dbPath, long long nowUnixMs, char *buffer, size_t bufferSize);
int wtTaskProjectionReadCapacityJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionCountActiveForAgent(const char *dbPath, const char *agent);
int wtTaskProjectionCountActiveForParent(const char *dbPath, const char *parentTaskId);
int wtTaskProjectionCountActiveForInitiative(const char *dbPath, const char *initiativeId);
/*
 * Sprint 5 policy budgets. Sum max_tokens for active (non-terminal) tasks under
 * an initiative or whose model_id starts with the given family prefix
 * (followed by '/'). Returns -1 on error.
 */
long long wtTaskProjectionAllocatedTokensForInitiative(const char *dbPath, const char *initiativeId);
long long wtTaskProjectionAllocatedTokensForModelFamily(const char *dbPath, const char *modelFamily);

#endif
