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
int wtTaskProjectionReadAgentsJson(const char *dbPath, long long nowUnixMs, char *buffer, size_t bufferSize);
int wtTaskProjectionReadCapacityJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionCountActiveForAgent(const char *dbPath, const char *agent);
int wtTaskProjectionCountActiveForParent(const char *dbPath, const char *parentTaskId);
int wtTaskProjectionCountActiveForInitiative(const char *dbPath, const char *initiativeId);

#endif
