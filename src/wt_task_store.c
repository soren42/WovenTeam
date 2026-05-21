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

static void populateTaskAutonomy(const char *line, WtTaskSummary *task) {
    readOptionalString(line, "autonomyLevel", task->autonomyLevel,
                       sizeof(task->autonomyLevel), "");
    readOptionalString(line, "scope", task->autonomyScope,
                       sizeof(task->autonomyScope), "");
    readOptionalString(line, "network", task->autonomyNetwork,
                       sizeof(task->autonomyNetwork), "none");
    readOptionalString(line, "credentialClass", task->autonomyCredentialClass,
                       sizeof(task->autonomyCredentialClass), "none");
    long value = 0;
    if (wtJsonReadLong(line, "ttlSeconds", &value) == 0) {
        task->autonomyTtlSeconds = (int)value;
    }
    if (wtJsonReadLong(line, "maxWallClockSeconds", &value) == 0) {
        task->autonomyMaxWallClockSeconds = (int)value;
    }
    if (wtJsonReadLong(line, "requiresCleanWorktree", &value) == 0) {
        task->autonomyRequiresCleanWorktree = value != 0;
    }
    wtJsonReadLongLong(line, "createdAtUnixMs", &task->autonomyCreatedAtUnixMs);
}

static void populateTaskPackageFields(const char *line, WtTaskSummary *task) {
    readOptionalString(line, "assignedAgent", task->assignedAgent, sizeof(task->assignedAgent), "");
    readOptionalString(line, "assignedRole", task->assignedRole, sizeof(task->assignedRole), "");
    readOptionalString(line, "status", task->status, sizeof(task->status), "queued");
    readOptionalString(line, "title", task->title, sizeof(task->title), "Untitled task");
    readOptionalString(line, "body", task->body, sizeof(task->body), "");
    readOptionalString(line, "modelId", task->modelId, sizeof(task->modelId), "");
    readOptionalString(line, "profile", task->toolProfile, sizeof(task->toolProfile), "observe");
    readOptionalString(line, "executionHost", task->executionHost, sizeof(task->executionHost), "");
    populateTaskAutonomy(line, task);
    long timeout = 0;
    long maxOutput = 0;
    if (wtJsonReadLong(line, "timeoutSeconds", &timeout) == 0) {
        task->timeoutSeconds = (int)timeout;
    }
    if (wtJsonReadLong(line, "maxOutputBytes", &maxOutput) == 0) {
        task->maxOutputBytes = (int)maxOutput;
    }
    wtJsonReadLongLong(line, "createdAtUnixMs", &task->updatedAtUnixMs);
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
                populateTaskPackageFields(line, &tasks[index]);
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
        if (tasks[index].executionHost[0] == '\0' &&
            strcmp(tasks[index].assignedAgent, agentName) == 0 &&
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
                populateTaskPackageFields(line, &tasks[index]);
                snprintf(ownerAgent[index], sizeof(ownerAgent[index]), "%s", tasks[index].assignedAgent);
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
        if (stuck && tasks[index].executionHost[0] == '\0' && strcmp(ownerAgent[index], agentName) == 0) {
            /* Restore the package's rightful owner on the returned summary so the
             * caller sees a coherent assignedAgent for downstream lease events. */
            snprintf(tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), "%s", ownerAgent[index]);
            *task = tasks[index];
            return 1;
        }
    }
    return 0;
}

int wtTaskFindClaimableForAgentOnHost(const char *ledgerPath, const char *agentName,
                                      const char *executionHost, long long nowUnixMs,
                                      WtTaskSummary *task, int *capabilityBlocked) {
    if (capabilityBlocked) *capabilityBlocked = 0;
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    WtTaskSummary tasks[256];
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
                populateTaskPackageFields(line, &tasks[index]);
                snprintf(ownerAgent[index], sizeof(ownerAgent[index]), "%s", tasks[index].assignedAgent);
            }
        } else if (lineHasSchema(line, "woventeam.task_event.v0.1") && index >= 0) {
            char eventType[32];
            readOptionalString(line, "eventType", eventType, sizeof(eventType), "");
            readOptionalString(line, "status", tasks[index].status, sizeof(tasks[index].status), tasks[index].status);
            wtJsonReadLongLong(line, "createdAtUnixMs", &tasks[index].updatedAtUnixMs);
            if (strcmp(eventType, "routing") == 0 || strcmp(eventType, "assignment") == 0) {
                readOptionalString(line, "assignedAgent", ownerAgent[index], sizeof(ownerAgent[index]), ownerAgent[index]);
                readOptionalString(line, "assignedAgent", tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), tasks[index].assignedAgent);
            }
            if (strcmp(eventType, "lease") == 0 || strcmp(eventType, "lease_renewal") == 0) {
                wtJsonReadLongLong(line, "leaseExpiresAtUnixMs", &leaseExpiresAt[index]);
            } else if (strcmp(eventType, "reclaim") == 0) {
                leaseExpiresAt[index] = 0;
            }
        }
    }
    fclose(file);
    for (int index = 0; index < count; index++) {
        int queued = strcmp(tasks[index].status, "queued") == 0 || strcmp(tasks[index].status, "assigned") == 0;
        int stuck = (strcmp(tasks[index].status, "leased") == 0 ||
                     strcmp(tasks[index].status, "running") == 0) &&
                    leaseExpiresAt[index] > 0 && leaseExpiresAt[index] <= nowUnixMs;
        if ((queued || stuck) && strcmp(ownerAgent[index], agentName) == 0) {
            if (tasks[index].executionHost[0] != '\0' && strcmp(tasks[index].executionHost, executionHost) != 0) {
                continue;
            }
            if (tasks[index].executionHost[0] == '\0' || strcmp(tasks[index].executionHost, executionHost) == 0) {
                snprintf(tasks[index].assignedAgent, sizeof(tasks[index].assignedAgent), "%s", ownerAgent[index]);
                *task = tasks[index];
                return 1;
            }
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

int wtTaskAppendHeartbeat(const char *ledgerPath, const char *agentName, const char *host,
                          const char *currentTaskId, long long leaseExpiresAtUnixMs,
                          const char *statusLine, bool fsyncRecord) {
    char escapedAgent[WT_TASK_AGENT_SIZE * 2];
    char escapedHost[128];
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedStatus[WT_TASK_BODY_SIZE];
    /* Worst case: every byte of escapedStatus expands and every fixed field
     * is at max; size the buffer for ~6x escaped status plus the rest. */
    char json[WT_TASK_BODY_SIZE * 2 + 1024];
    if (wtJsonEscape(agentName ? agentName : "", escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(host ? host : "", escapedHost, sizeof(escapedHost)) != 0 ||
        wtJsonEscape(currentTaskId ? currentTaskId : "", escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(statusLine ? statusLine : "", escapedStatus, sizeof(escapedStatus)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.heartbeat.v0.1\",\"agent\":\"%s\","
             "\"host\":\"%s\",\"currentTaskId\":\"%s\","
             "\"leaseExpiresAtUnixMs\":%lld,\"statusLine\":\"%s\","
             "\"createdAtUnixMs\":%lld}",
             escapedAgent, escapedHost, escapedTaskId,
             leaseExpiresAtUnixMs, escapedStatus, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendMilestone(const char *ledgerPath, const char *taskId, const char *milestone,
                          const char *message, const char *createdBy, bool fsyncRecord) {
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedMilestone[64];
    char escapedMessage[WT_TASK_BODY_SIZE];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    /* Same sizing argument as the heartbeat helper above. */
    char json[WT_TASK_BODY_SIZE * 2 + 1024];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(milestone ? milestone : "", escapedMilestone, sizeof(escapedMilestone)) != 0 ||
        wtJsonEscape(message ? message : "", escapedMessage, sizeof(escapedMessage)) != 0 ||
        wtJsonEscape(createdBy ? createdBy : "", escapedBy, sizeof(escapedBy)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.milestone.v0.1\",\"taskId\":\"%s\","
             "\"milestone\":\"%s\",\"message\":\"%s\","
             "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedMilestone, escapedMessage, escapedBy,
             wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendLeaseRenewal(const char *ledgerPath, const char *taskId, const char *agentName,
                             int attempt, long long leaseExpiresAtUnixMs, bool fsyncRecord) {
    /*
     * Renewal carries eventType="lease_renewal" so the projection can recognize
     * it as an extension rather than a fresh claim. The schema reuses
     * woventeam.task_event.v0.1 to keep downstream consumers simple.
     */
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedAgent[WT_TASK_AGENT_SIZE * 2];
    char message[256];
    char escapedMessage[512];
    char json[2048];
    snprintf(message, sizeof(message),
             "Lease renewed by wt-agent@%s attempt %d; extended to %lld.",
             agentName, attempt, leaseExpiresAtUnixMs);
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(agentName, escapedAgent, sizeof(escapedAgent)) != 0 ||
        wtJsonEscape(message, escapedMessage, sizeof(escapedMessage)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
             "\"eventType\":\"lease_renewal\",\"status\":\"running\","
             "\"assignedAgent\":\"%s\",\"message\":\"%s\","
             "\"createdBy\":\"wt-agent@%s\",\"attempt\":%d,"
             "\"leaseExpiresAtUnixMs\":%lld,\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedAgent, escapedMessage, escapedAgent, attempt,
             leaseExpiresAtUnixMs, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendKillEvent(const char *ledgerPath, const char *taskId, const char *reason,
                          const char *createdBy, bool fsyncRecord) {
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedReason[128];
    char escapedBy[WT_TASK_AGENT_SIZE * 2];
    char json[1024];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(reason ? reason : "operator", escapedReason, sizeof(escapedReason)) != 0 ||
        wtJsonEscape(createdBy ? createdBy : "ceo", escapedBy, sizeof(escapedBy)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.kill_event.v0.1\",\"taskId\":\"%s\","
             "\"reason\":\"%s\",\"createdBy\":\"%s\","
             "\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedReason, escapedBy, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskAppendAutonomyEvent(const char *ledgerPath, const char *taskId, const char *actor,
                              const char *target, const char *action, const char *commandClass,
                              const char *autonomyLevel, const char *reason, int allowed,
                              int exitCode, bool fsyncRecord) {
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedActor[WT_TASK_AGENT_SIZE * 2];
    char escapedTarget[WT_TASK_AGENT_SIZE * 2];
    char escapedAction[64];
    char escapedCommandClass[128];
    char escapedAutonomyLevel[WT_TASK_POLICY_SIZE * 2];
    char escapedReason[256];
    char json[2048];
    if (wtJsonEscape(taskId ? taskId : "", escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(actor ? actor : "system", escapedActor, sizeof(escapedActor)) != 0 ||
        wtJsonEscape(target ? target : "", escapedTarget, sizeof(escapedTarget)) != 0 ||
        wtJsonEscape(action ? action : "", escapedAction, sizeof(escapedAction)) != 0 ||
        wtJsonEscape(commandClass ? commandClass : "", escapedCommandClass, sizeof(escapedCommandClass)) != 0 ||
        wtJsonEscape(autonomyLevel ? autonomyLevel : "", escapedAutonomyLevel, sizeof(escapedAutonomyLevel)) != 0 ||
        wtJsonEscape(reason ? reason : "", escapedReason, sizeof(escapedReason)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.autonomy_event.v0.1\",\"taskId\":\"%s\","
             "\"actor\":\"%s\",\"target\":\"%s\",\"action\":\"%s\","
             "\"commandClass\":\"%s\",\"autonomyLevel\":\"%s\","
             "\"reason\":\"%s\",\"allowed\":%s,\"exitCode\":%d,"
             "\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedActor, escapedTarget, escapedAction,
             escapedCommandClass, escapedAutonomyLevel, escapedReason,
             allowed ? "true" : "false", exitCode, wtNowUnixMilliseconds());
    return wtTaskAppendRecord(ledgerPath, json, fsyncRecord);
}

int wtTaskReadLatestAutonomyRevocation(const char *ledgerPath, const char *taskId,
                                       long long *revokedAtUnixMs, char *revokedBy,
                                       size_t revokedBySize) {
    if (revokedAtUnixMs) *revokedAtUnixMs = 0;
    if (revokedBy && revokedBySize > 0) revokedBy[0] = '\0';
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_TASK_LEDGER_LINE_SIZE];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char schema[128];
        char eventTaskId[WT_TASK_ID_SIZE];
        char action[64];
        if (wtJsonReadString(line, "schema", schema, sizeof(schema)) != 0 ||
            strcmp(schema, "woventeam.autonomy_event.v0.1") != 0 ||
            wtJsonReadString(line, "taskId", eventTaskId, sizeof(eventTaskId)) != 0 ||
            strcmp(eventTaskId, taskId) != 0 ||
            wtJsonReadString(line, "action", action, sizeof(action)) != 0 ||
            strcmp(action, "revoked") != 0) {
            continue;
        }
        found = 1;
        if (revokedAtUnixMs) {
            wtJsonReadLongLong(line, "createdAtUnixMs", revokedAtUnixMs);
        }
        if (revokedBy && revokedBySize > 0) {
            readOptionalString(line, "actor", revokedBy, revokedBySize, "");
        }
    }
    fclose(file);
    return found;
}

int wtTaskCancelRequested(const char *ledgerPath, const char *taskId) {
    /*
     * Walk the ledger; return 1 if any kill_event matches the taskId AFTER the
     * most recent lease event. Resetting on lease means a reclaimed + re-leased
     * task starts a fresh cancel window: the operator's earlier cancel doesn't
     * silently kill the new attempt.
     */
    FILE *file = fopen(ledgerPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_TASK_LEDGER_LINE_SIZE];
    int cancelSeen = 0;
    while (fgets(line, sizeof(line), file)) {
        char schema[128];
        char eventTaskId[WT_TASK_ID_SIZE];
        if (wtJsonReadString(line, "schema", schema, sizeof(schema)) != 0 ||
            wtJsonReadString(line, "taskId", eventTaskId, sizeof(eventTaskId)) != 0 ||
            strcmp(eventTaskId, taskId) != 0) {
            continue;
        }
        if (strcmp(schema, "woventeam.kill_event.v0.1") == 0) {
            cancelSeen = 1;
        } else if (strcmp(schema, "woventeam.task_event.v0.1") == 0) {
            char eventType[32];
            if (wtJsonReadString(line, "eventType", eventType, sizeof(eventType)) == 0 &&
                strcmp(eventType, "lease") == 0) {
                /* A new lease resets the cancel state - the operator must
                 * cancel again to kill the next attempt. */
                cancelSeen = 0;
            }
        }
    }
    fclose(file);
    return cancelSeen;
}

int wtTaskAppendArtifactEvent(const char *ledgerPath, const char *taskId, const char *state,
                              const char *reviewer, const char *notes, const char *artifactPath,
                              bool fsyncRecord) {
    /*
     * Artifact decisions are append-only task events that promote, demote, or
     * annotate a workspace artifact. The event preserves the prior task status
     * by NOT writing a "status" field - status transitions remain the job of
     * lifecycle/gate events. Projection logic only updates the artifact_*
     * columns when it sees an artifact eventType.
     */
    char escapedTaskId[WT_TASK_ID_SIZE * 2];
    char escapedState[64];
    char escapedReviewer[WT_TASK_AGENT_SIZE * 2];
    char escapedNotes[WT_TASK_BODY_SIZE * 2];
    /* Artifact paths are workspace-relative and bounded; 1024 covers prompt.md,
     * stdout.log, stderr.log, manifest.json, result.md and any future promoted
     * subpath without dragging wt_config.h into this translation unit. */
    char escapedPath[1024];
    /* Notes appear twice in the format string (reviewNotes + message), so the
     * upper bound is 2 * escapedNotes + every other escaped field + JSON keys. */
    char json[WT_TASK_BODY_SIZE * 5 + 2048];
    if (wtJsonEscape(taskId, escapedTaskId, sizeof(escapedTaskId)) != 0 ||
        wtJsonEscape(state ? state : "", escapedState, sizeof(escapedState)) != 0 ||
        wtJsonEscape(reviewer ? reviewer : "", escapedReviewer, sizeof(escapedReviewer)) != 0 ||
        wtJsonEscape(notes ? notes : "", escapedNotes, sizeof(escapedNotes)) != 0 ||
        wtJsonEscape(artifactPath ? artifactPath : "", escapedPath, sizeof(escapedPath)) != 0) {
        return -1;
    }
    snprintf(json, sizeof(json),
             "{\"schema\":\"woventeam.task_event.v0.1\",\"taskId\":\"%s\","
             "\"eventType\":\"artifact\",\"artifactState\":\"%s\","
             "\"reviewer\":\"%s\",\"reviewNotes\":\"%s\","
             "\"artifactPath\":\"%s\",\"message\":\"%s\","
             "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
             escapedTaskId, escapedState, escapedReviewer, escapedNotes,
             escapedPath, escapedNotes, escapedReviewer,
             wtNowUnixMilliseconds());
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
