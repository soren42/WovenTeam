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
#define WT_TASK_LEDGER_LINE_SIZE 16384

typedef struct {
    char taskId[WT_TASK_ID_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char assignedRole[WT_TASK_AGENT_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char title[WT_TASK_TITLE_SIZE];
    long long updatedAtUnixMs;
} WtTaskSummary;

int wtTaskAppendRecord(const char *ledgerPath, const char *jsonLine, bool fsyncRecord);
int wtTaskReadAllJsonArray(const char *ledgerPath, char *buffer, size_t bufferSize);
int wtTaskFindQueuedForAgent(const char *ledgerPath, const char *agentName, WtTaskSummary *task);
int wtTaskAppendStatusEvent(const char *ledgerPath, const char *taskId, const char *status,
                            const char *createdBy, const char *message, bool fsyncRecord);

#endif
