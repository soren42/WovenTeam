/*
 * wt_task_projection.h - Rebuildable SQLite projection for the task ledger.
 */
#ifndef WT_TASK_PROJECTION_H
#define WT_TASK_PROJECTION_H

#include <stddef.h>

int wtTaskProjectionRebuild(const char *dbPath, const char *ledgerPath);
int wtTaskProjectionReadSummariesJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionReadDetailJson(const char *dbPath, const char *taskId, char *buffer, size_t bufferSize);
int wtTaskProjectionReadCapacityJson(const char *dbPath, char *buffer, size_t bufferSize);
int wtTaskProjectionCountActiveForAgent(const char *dbPath, const char *agent);
int wtTaskProjectionCountActiveForParent(const char *dbPath, const char *parentTaskId);
int wtTaskProjectionCountActiveForInitiative(const char *dbPath, const char *initiativeId);

#endif
