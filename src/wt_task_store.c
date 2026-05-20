/*
 * wt_task_store.c - Append-only JSONL task package ledger.
 */
#include "wt_task_store.h"

#include "wt_json.h"
#include "wt_room_store.h"
#include "wt_time.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int lineHasSchema(const char *line, const char *schema) {
    char value[128];
    return wtJsonReadString(line, "schema", value, sizeof(value)) == 0 &&
           strcmp(value, schema) == 0;
}

static void readOptionalString(const char *json, const char *key, char *buffer, size_t bufferSize, const char *fallback) {
    if (wtJsonReadString(json, key, buffer, bufferSize) != 0) {
        snprintf(buffer, bufferSize, "%s", fallback ? fallback : "");
    }
}

int wtTaskAppendRecord(const char *ledgerPath, const char *jsonLine, bool fsyncRecord) {
    if (wtRoomEnsureParentDirs(ledgerPath) != 0) {
        return -1;
    }
    int fd = open(ledgerPath, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    size_t length = strlen(jsonLine);
    int ok = write(fd, jsonLine, length) == (ssize_t)length &&
             write(fd, "\n", 1) == 1;
    if (ok && fsyncRecord) {
        ok = fsync(fd) == 0;
    }
    flock(fd, LOCK_UN);
    close(fd);
    return ok ? 0 : -1;
}

int wtTaskReadAllJsonArray(const char *ledgerPath, char *buffer, size_t bufferSize) {
    FILE *file = fopen(ledgerPath, "r");
    size_t used = 0;
    if (bufferSize < 3) {
        return -1;
    }
    buffer[used++] = '[';
    if (file) {
        char line[WT_TASK_LEDGER_LINE_SIZE];
        int first = 1;
        while (fgets(line, sizeof(line), file)) {
            size_t length = strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
                line[--length] = '\0';
            }
            if (length == 0) {
                continue;
            }
            if (used + length + 3 >= bufferSize) {
                break;
            }
            if (!first) {
                buffer[used++] = ',';
            }
            first = 0;
            memcpy(buffer + used, line, length);
            used += length;
        }
        fclose(file);
    }
    buffer[used++] = ']';
    buffer[used] = '\0';
    return 0;
}

static int findKnownTask(WtTaskSummary *tasks, int count, const char *taskId) {
    for (int index = 0; index < count; index++) {
        if (strcmp(tasks[index].taskId, taskId) == 0) {
            return index;
        }
    }
    return -1;
}

int wtTaskFindQueuedForAgent(const char *ledgerPath, const char *agentName, WtTaskSummary *task) {
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    WtTaskSummary tasks[256];
    int count = 0;
    char line[WT_TASK_LEDGER_LINE_SIZE];
    while (fgets(line, sizeof(line), file)) {
        char taskId[WT_TASK_ID_SIZE];
        if (wtJsonReadString(line, "taskId", taskId, sizeof(taskId)) != 0) {
            continue;
        }
        int index = findKnownTask(tasks, count, taskId);
        if (lineHasSchema(line, "woventeam.task_package.v0.1")) {
            if (index < 0 && count < 256) {
                index = count++;
                memset(&tasks[index], 0, sizeof(tasks[index]));
                snprintf(tasks[index].taskId, sizeof(tasks[index].taskId), "%s", taskId);
            }
            if (index >= 0) {
                readOptionalString(line, "assignedAgent", tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), "");
                readOptionalString(line, "assignedRole", tasks[index].assignedRole, sizeof(tasks[index].assignedRole), "");
                readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), "queued");
                readOptionalString(line, "title", tasks[index].title, sizeof(tasks[index].title), "Untitled task");
                readOptionalString(line, "body", tasks[index].body, sizeof(tasks[index].body), "");
                readOptionalString(line, "modelId", tasks[index].modelId, sizeof(tasks[index].modelId), "");
                readOptionalString(line, "profile", tasks[index].toolProfile, sizeof(tasks[index].toolProfile), "observe");
                long timeout = 0;
                long maxOutput = 0;
                if (wtJsonReadLong(line, "timeoutSeconds", &timeout) == 0) {
                    tasks[index].timeoutSeconds = (int)timeout;
                }
                if (wtJsonReadLong(line, "maxOutputBytes", &maxOutput) == 0) {
                    tasks[index].maxOutputBytes = (int)maxOutput;
                }
                wtJsonReadLongLong(line, "createdAtUnixMs", &tasks[index].updatedAtUnixMs);
            }
        } else if (lineHasSchema(line, "woventeam.task_event.v0.1")) {
            if (index >= 0) {
                readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), tasks[index].status);
                readOptionalString(line, "assignedAgent", tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), tasks[index].assignedAgent);
                wtJsonReadLongLong(line, "createdAtUnixMs", &tasks[index].updatedAtUnixMs);
            }
        }
    }
    fclose(file);
    for (int index = 0; index < count; index++) {
        if (strcmp(tasks[index].assignedAgent, agentName) == 0 &&
            (strcmp(tasks[index].status, "queued") == 0 || strcmp(tasks[index].status, "assigned") == 0)) {
            *task = tasks[index];
            return 1;
        }
    }
    return 0;
}

int wtTaskFindClaimableForAgent(const char *ledgerPath, const char *agentName,
                                long long nowUnixMs, WtTaskSummary *task) {
    /*
     * First-pass: try the existing queued/assigned helper. That's the common
     * path and keeps backwards compatibility with all prior tests.
     */
    int queued = wtTaskFindQueuedForAgent(ledgerPath, agentName, task);
    if (queued > 0) {
        return queued;
    }
    /*
     * Second-pass: walk the ledger again looking for tasks that are stuck in
     * leased/running with an expired lease. The package's assignedAgent must
     * match this agent so we only attempt our own backlog (lease ownership is
     * checked separately in handleAssignedTask).
     */
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    WtTaskSummary tasks[256];
    /* ownerAgent tracks who the routing/package says should run this task; it is
     * never overwritten by lease events (lease events name the previous holder). */
    char ownerAgent[256][WT_TASK_AGENT_SIZE];
    long long leaseExpiresAt[256];
    int count = 0;
    char line[WT_TASK_LEDGER_LINE_SIZE];
    while (fgets(line, sizeof(line), file)) {
        char taskId[WT_TASK_ID_SIZE];
        if (wtJsonReadString(line, "taskId", taskId, sizeof(taskId)) != 0) {
            continue;
        }
        int index = findKnownTask(tasks, count, taskId);
        if (lineHasSchema(line, "woventeam.task_package.v0.1")) {
            if (index < 0 && count < 256) {
                index = count++;
                memset(&tasks[index], 0, sizeof(tasks[index]));
                ownerAgent[index][0] = '\0';
                leaseExpiresAt[index] = 0;
                snprintf(tasks[index].taskId, sizeof(tasks[index].taskId), "%s", taskId);
            }
            if (index >= 0) {
                readOptionalString(line, "assignedAgent", tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), "");
                snprintf(ownerAgent[index], sizeof(ownerAgent[index]), "%s", tasks[index].assignedAgent);
                readOptionalString(line, "assignedRole", tasks[index].assignedRole, sizeof(tasks[index].assignedRole), "");
                readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), "queued");
                readOptionalString(line, "title", tasks[index].title, sizeof(tasks[index].title), "Untitled task");
                readOptionalString(line, "body", tasks[index].body, sizeof(tasks[index].body), "");
                readOptionalString(line, "modelId", tasks[index].modelId, sizeof(tasks[index].modelId), "");
                readOptionalString(line, "profile", tasks[index].toolProfile, sizeof(tasks[index].toolProfile), "observe");
                long timeoutSec = 0;
                long maxOutput = 0;
                if (wtJsonReadLong(line, "timeoutSeconds", &timeoutSec) == 0) {
                    tasks[index].timeoutSeconds = (int)timeoutSec;
                }
                if (wtJsonReadLong(line, "maxOutputBytes", &maxOutput) == 0) {
                    tasks[index].maxOutputBytes = (int)maxOutput;
                }
                wtJsonReadLongLong(line, "createdAtUnixMs", &tasks[index].updatedAtUnixMs);
            }
        } else if (lineHasSchema(line, "woventeam.task_event.v0.1") && index >= 0) {
            char eventType[32];
            readOptionalString(line, "eventType", eventType, sizeof(eventType), "");
            readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), tasks[index].status);
            wtJsonReadLongLong(line, "createdAtUnixMs", &tasks[index].updatedAtUnixMs);
            /* Only routing/assignment events change the rightful owner. */
            if (strcmp(eventType, "routing") == 0 || strcmp(eventType, "assignment") == 0) {
                readOptionalString(line, "assignedAgent", ownerAgent[index], sizeof(ownerAgent[index]), ownerAgent[index]);
                readOptionalString(line, "assignedAgent", tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), tasks[index].assignedAgent);
            }
            if (strcmp(eventType, "lease") == 0) {
                wtJsonReadLongLong(line, "leaseExpiresAtUnixMs", &leaseExpiresAt[index]);
            } else if (strcmp(eventType, "reclaim") == 0) {
                leaseExpiresAt[index] = 0;
            }
        }
    }
    fclose(file);
    for (int index = 0; index < count; index++) {
        int stuck = (strcmp(tasks[index].status, "leased") == 0 ||
                     strcmp(tasks[index].status, "running") == 0) &&
                    leaseExpiresAt[index] > 0 && leaseExpiresAt[index] <= nowUnixMs;
        if (stuck && strcmp(ownerAgent[index], agentName) == 0) {
            /* Restore the package's rightful owner on the returned summary so the
             * caller sees a coherent assignedAgent for downstream lease events. */
            snprintf(tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), "%s", ownerAgent[index]);
            *task = tasks[index];
            return 1;
        }
    }
    return 0;
}

int wtTaskAgentPaused(const char *ledgerPath, const char *agentName) {
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_TASK_LEDGER_LINE_SIZE];
    int paused = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!lineHasSchema(line, "woventeam.agent_control.v0.1")) {
            continue;
        }
        char agent[WT_TASK_AGENT_SIZE];
        char action[32];
        if (wtJsonReadString(line, "agent", agent, sizeof(agent)) != 0 ||
            strcmp(agent, agentName) != 0 ||
            wtJsonReadString(line, "action", action, sizeof(action)) != 0) {
            continue;
        }
        if (strcmp(action, "pause") == 0) {
            paused = 1;
        } else if (strcmp(action, "resume") == 0) {
            paused = 0;
        }
    }
    fclose(file);
    return paused;
}

int wtTaskAppendLeaseEvent(const char *ledgerPath, const char *taskId, const char *agentName,
                           int attempt, long long leaseExpiresAtUnixMs, bool fsyncRecord) {
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedAgent[WT_TASK_AGENT_SIZE * 2];
    char message[256];
    char escapedMessage[512];
    char json[2048];
    snprintf(message, sizeof(message), "Task leased by wt-agent@%s attempt %d.", agentName, attempt);
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(agentName, escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(message, escapedMessage, sizeof(escapedMessage)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
             "\"eventType\":\"lease\",\"status\":\"leased\",\"assignedAgent\":\"%s\","
             "\"message\":\"%s\",\"createdBy\":\"wt-agent@%s\",\"attempt\":%d,"
             "\"leaseExpiresAtUnixMs\":%lld,\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedAgent, escapedMessage, escapedAgent, attempt,
             leaseExpiresAtUnixMs, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendStatusEvent(const char *ledgerPath, const char *taskId, const char *status,
                            const char *createdBy, const char *message, bool fsyncRecord) {
    /*
     * Backwards-compatible status-event entry point. New callers should use
     * wtTaskAppendStatusEventWithCause when they have classified retry context.
     */
    return wtTaskAppendStatusEventWithCause(ledgerPath, taskId, status, createdBy, message,
                                            NULL, fsyncRecord);
}

int wtTaskAppendStatusEventWithCause(const char *ledgerPath, const char *taskId, const char *status,
                                     const char *createdBy, const char *message,
                                     const char *retryCause, bool fsyncRecord) {
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedStatus[WT_TASK_STATUS_SIZE * 2];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    char escapedMessage[1024];
    char escapedRetryCause[64];
    char json[2400];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(status, escapedStatus, sizeof(escapedStatus)) != 0 ||
        wtJsonEscape(createdBy, escapedBy, sizeof(escapedBy)) != 0 ||
        wtJsonEscape(message ? message : "", escapedMessage, sizeof(escapedMessage)) != 0) {
        return -1;
    }
    /* retryCause is optional - emit the field only when the caller supplied one. */
    int hasRetryCause = retryCause && retryCause[0] != '\0';
    if (hasRetryCause && wtJsonEscape(retryCause, escapedRetryCause, sizeof(escapedRetryCause)) != 0) {
        return -1;
    }
    if (hasRetryCause) {
        snprintf(json, sizeof(json),
                 "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
                 "\"eventType\":\"status\",\"status\":\"%s\",\"message\":\"%s\","
                 "\"retryCause\":\"%s\",\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
                 escapedTaskId, escapedStatus, escapedMessage, escapedRetryCause,
                 escapedBy, wtNowUnixMilliseconds());
    } else {
        snprintf(json, sizeof(json),
                 "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
                 "\"eventType\":\"status\",\"status\":\"%s\",\"message\":\"%s\","
                 "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
                 escapedTaskId, escapedStatus, escapedMessage, escapedBy, wtNowUnixMilliseconds());
    }
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendReclaimEvent(const char *ledgerPath, const char *taskId, const char *previousAgent,
                             const char *reclaimedBy, const char *reason, const char *message,
                             bool fsyncRecord) {
    /*
     * A reclaim event releases the latest lease so the task returns to the queued
     * pool. Reclaims are not status transitions on their own - they always set
     * status back to "queued" so wtTaskFindQueuedForAgent will pick the task up
     * on the next agent cycle.
     */
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedPreviousAgent[WT_TASK_AGENT_SIZE * 2];
    char escapedReclaimedBy[WT_TASK_AGENT_SIZE * 2];
    char escapedReason[64];
    char escapedMessage[1024];
    char json[2400];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(previousAgent ? previousAgent : "", escapedPreviousAgent, sizeof(escapedPreviousAgent)) != 0 ||
        wtJsonEscape(reclaimedBy ? reclaimedBy : "", escapedReclaimedBy, sizeof(escapedReclaimedBy)) != 0 ||
        wtJsonEscape(reason ? reason : "operator", escapedReason, sizeof(escapedReason)) != 0 ||
        wtJsonEscape(message ? message : "", escapedMessage, sizeof(escapedMessage)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
             "\"eventType\":\"reclaim\",\"status\":\"queued\",\"assignedAgent\":\"%s\","
             "\"reclaimReason\":\"%s\",\"message\":\"%s\",\"createdBy\":\"%s\","
             "\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedPreviousAgent, escapedReason, escapedMessage,
             escapedReclaimedBy, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskFindActiveLease(const char *ledgerPath, const char *taskId,
                          char *leaseHolder, size_t leaseHolderSize,
                          long long *leaseExpiresAtUnixMs, int *attempt) {
    /*
     * Walk the ledger in order. The last "lease" event followed by no later
     * "reclaim" event for the same task is the active lease.
     */
    if (leaseHolder && leaseHolderSize > 0) leaseHolder[0] = '\0';
    if (leaseExpiresAtUnixMs) *leaseExpiresAtUnixMs = 0;
    if (attempt) *attempt = 0;
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_TASK_LEDGER_LINE_SIZE];
    int hasActiveLease = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!lineHasSchema(line, "woventeam.task_event.v0.1")) {
            continue;
        }
        char eventTaskId[WT_TASK_ID_SIZE];
        char eventType[32];
        if (wtJsonReadString(line, "taskId", eventTaskId, sizeof(eventTaskId)) != 0 ||
            strcmp(eventTaskId, taskId) != 0 ||
            wtJsonReadString(line, "eventType", eventType, sizeof(eventType)) != 0) {
            continue;
        }
        if (strcmp(eventType, "lease") == 0) {
            if (leaseHolder && leaseHolderSize > 0) {
                readOptionalString(line, "assignedAgent", leaseHolder, leaseHolderSize, "");
            }
            long long expires = 0;
            long attemptValue = 0;
            wtJsonReadLongLong(line, "leaseExpiresAtUnixMs", &expires);
            wtJsonReadLong(line, "attempt", &attemptValue);
            if (leaseExpiresAtUnixMs) *leaseExpiresAtUnixMs = expires;
            if (attempt) *attempt = (int)attemptValue;
            hasActiveLease = 1;
        } else if (strcmp(eventType, "reclaim") == 0 ||
                   strcmp(eventType, "status") == 0) {
            /* Reclaim releases the lease. Terminal status transitions (complete,
             * failed, cancelled, closed) also release it implicitly. */
            char eventStatus[WT_TASK_STATUS_SIZE];
            if (strcmp(eventType, "reclaim") == 0) {
                hasActiveLease = 0;
            } else if (wtJsonReadString(line, "status", eventStatus, sizeof(eventStatus)) == 0) {
                if (strcmp(eventStatus, "complete") == 0 || strcmp(eventStatus, "failed") == 0 ||
                    strcmp(eventStatus, "cancelled") == 0 || strcmp(eventStatus, "closed") == 0) {
                    hasActiveLease = 0;
                }
            }
        }
    }
    fclose(file);
    return hasActiveLease;
}

static int lineDependsOnTask(const char *line, const char *blockedTaskId) {
    const char *dependencies = strstr(line, "\"dependencies\"");
    if (!dependencies) {
        return 0;
    }
    const char *end = strchr(dependencies, ']');
    if (!end) {
        return 0;
    }
    char quoted[WT_TASK_ID_SIZE + 4];
    snprintf(quoted, sizeof(quoted), "\"%s\"", blockedTaskId);
    const char *match = strstr(dependencies, quoted);
    return match && match < end;
}

int wtTaskAppendBlockedDependents(const char *ledgerPath, const char *blockedTaskId,
                                  const char *createdBy, bool fsyncRecord) {
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    WtTaskSummary tasks[256];
    int count = 0;
    char dependsOnBlocked[256][WT_TASK_ID_SIZE];
    int dependentCount = 0;
    char line[WT_TASK_LEDGER_LINE_SIZE];
    while (fgets(line, sizeof(line), file)) {
        char taskId[WT_TASK_ID_SIZE];
        if (wtJsonReadString(line, "taskId", taskId, sizeof(taskId)) != 0) {
            continue;
        }
        int index = findKnownTask(tasks, count, taskId);
        if (lineHasSchema(line, "woventeam.task_package.v0.1")) {
            if (index < 0 && count < 256) {
                index = count++;
                memset(&tasks[index], 0, sizeof(tasks[index]));
                snprintf(tasks[index].taskId, sizeof(tasks[index].taskId), "%s", taskId);
            }
            if (index >= 0) {
                readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), "queued");
                if (lineDependsOnTask(line, blockedTaskId) && dependentCount < 256) {
                    snprintf(dependsOnBlocked[dependentCount++], sizeof(dependsOnBlocked[0]), "%s", taskId);
                }
            }
        } else if (lineHasSchema(line, "woventeam.task_event.v0.1") && index >= 0) {
            readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), tasks[index].status);
        }
    }
    fclose(file);

    int appended = 0;
    for (int depIndex = 0; depIndex < dependentCount; depIndex++) {
        int taskIndex = findKnownTask(tasks, count, dependsOnBlocked[depIndex]);
        if (taskIndex < 0 || strcmp(tasks[taskIndex].status, "complete") == 0 ||
            strcmp(tasks[taskIndex].status, "failed") == 0 ||
            strcmp(tasks[taskIndex].status, "cancelled") == 0 ||
            strcmp(tasks[taskIndex].status, "closed") == 0 ||
            strcmp(tasks[taskIndex].status, "blocked") == 0) {
            continue;
        }
        char message[256];
        snprintf(message, sizeof(message), "Blocked because dependency %s is blocked.", blockedTaskId);
        if (wtTaskAppendStatusEvent(ledgerPath, dependsOnBlocked[depIndex], "blocked",
                                    createdBy, message, fsyncRecord) != 0) {
            return -1;
        }
        appended++;
    }
    return appended;
}

int wtTaskSummarizeTokenBudgets(const char *ledgerPath, long long nowUnixMs, WtTokenSummary *summary) {
    memset(summary, 0, sizeof(*summary));
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    const long long dayWindowMs = 24LL * 60LL * 60LL * 1000LL;
    const long long monthWindowMs = 30LL * dayWindowMs;
    char line[WT_TASK_LEDGER_LINE_SIZE];
    while (fgets(line, sizeof(line), file)) {
        long long createdAtUnixMs = 0;
        wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtUnixMs);
        if (lineHasSchema(line, "woventeam.task_package.v0.1")) {
            long maxTokens = 0;
            if (wtJsonReadLong(line, "maxTokens", &maxTokens) != 0 || maxTokens < 0) {
                maxTokens = 0;
            }
            summary->allTimeAllocatedTokens += maxTokens;
            summary->allTimePackages++;
            if (createdAtUnixMs > 0 && createdAtUnixMs >= nowUnixMs - monthWindowMs) {
                summary->monthWindowAllocatedTokens += maxTokens;
                summary->monthWindowPackages++;
            }
            if (createdAtUnixMs > 0 && createdAtUnixMs >= nowUnixMs - dayWindowMs) {
                summary->dayWindowAllocatedTokens += maxTokens;
                summary->dayWindowPackages++;
            }
        } else if (lineHasSchema(line, "woventeam.task_usage.v0.1")) {
            long totalTokens = 0;
            long estimatedCostCents = 0;
            if (wtJsonReadLong(line, "totalTokens", &totalTokens) != 0 || totalTokens < 0) {
                totalTokens = 0;
            }
            if (wtJsonReadLong(line, "estimatedCostCents", &estimatedCostCents) != 0 || estimatedCostCents < 0) {
                estimatedCostCents = 0;
            }
            summary->allTimeActualTokens += totalTokens;
            summary->allTimeActualCostCents += estimatedCostCents;
            summary->allTimeUsageEvents++;
            if (createdAtUnixMs > 0 && createdAtUnixMs >= nowUnixMs - monthWindowMs) {
                summary->monthWindowActualTokens += totalTokens;
                summary->monthWindowActualCostCents += estimatedCostCents;
                summary->monthWindowUsageEvents++;
            }
            if (createdAtUnixMs > 0 && createdAtUnixMs >= nowUnixMs - dayWindowMs) {
                summary->dayWindowActualTokens += totalTokens;
                summary->dayWindowActualCostCents += estimatedCostCents;
                summary->dayWindowUsageEvents++;
            }
        }
    }
    fclose(file);
    return 0;
}
