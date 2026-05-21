/*
 * wt_task_projection.c - SQLite read model rebuilt from task JSONL.
 */
#include "wt_task_projection.h"

#include "wt_json.h"
#include "wt_room_store.h"
#include "wt_task_store.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int execSql(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

static int lineHasSchema(const char *line, const char *schema) {
    char value[128];
    return wtJsonReadString(line, "schema", value, sizeof(value)) == 0 &&
           strcmp(value, schema) == 0;
}

static void readStringOrDefault(const char *json, const char *key, char *buffer,
                                size_t bufferSize, const char *fallback) {
    if (wtJsonReadString(json, key, buffer, bufferSize) != 0) {
        snprintf(buffer, bufferSize, "%s", fallback ? fallback : "");
    }
}

static void bindText(sqlite3_stmt *stmt, int index, const char *value) {
    sqlite3_bind_text(stmt, index, value ? value : "", -1, SQLITE_TRANSIENT);
}

static int projectPackage(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO tasks "
        "(task_id,initiative_id,parent_task_id,requested_by_role,assigned_role,assigned_agent,"
        "model_id,priority,status,title,body,tool_profile,autonomy_level,autonomy_scope,"
        "autonomy_network,autonomy_credential_class,autonomy_ttl_seconds,autonomy_max_wall_clock_seconds,"
        "autonomy_requires_clean_worktree,max_tokens,created_at_ms,updated_at_ms,event_count) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0) "
        "ON CONFLICT(task_id) DO UPDATE SET "
        "initiative_id=excluded.initiative_id,parent_task_id=excluded.parent_task_id,"
        "requested_by_role=excluded.requested_by_role,assigned_role=excluded.assigned_role,"
        "assigned_agent=excluded.assigned_agent,model_id=excluded.model_id,priority=excluded.priority,"
        "status=excluded.status,title=excluded.title,body=excluded.body,tool_profile=excluded.tool_profile,"
        "autonomy_level=excluded.autonomy_level,autonomy_scope=excluded.autonomy_scope,"
        "autonomy_network=excluded.autonomy_network,autonomy_credential_class=excluded.autonomy_credential_class,"
        "autonomy_ttl_seconds=excluded.autonomy_ttl_seconds,"
        "autonomy_max_wall_clock_seconds=excluded.autonomy_max_wall_clock_seconds,"
        "autonomy_requires_clean_worktree=excluded.autonomy_requires_clean_worktree,"
        "max_tokens=excluded.max_tokens,created_at_ms=excluded.created_at_ms,"
        "updated_at_ms=max(tasks.updated_at_ms,excluded.updated_at_ms)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char taskId[WT_TASK_ID_SIZE];
    char initiativeId[WT_TASK_ID_SIZE];
    char parentTaskId[WT_TASK_ID_SIZE];
    char requestedByRole[WT_TASK_AGENT_SIZE];
    char assignedRole[WT_TASK_AGENT_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char modelId[WT_TASK_MODEL_SIZE];
    char priority[32];
    char status[WT_TASK_STATUS_SIZE];
    char title[WT_TASK_TITLE_SIZE];
    char body[WT_TASK_BODY_SIZE];
    char toolProfile[WT_TASK_POLICY_SIZE];
    char autonomyLevel[WT_TASK_POLICY_SIZE];
    char autonomyScope[WT_TASK_BODY_SIZE];
    char autonomyNetwork[WT_TASK_POLICY_SIZE];
    char autonomyCredentialClass[WT_TASK_POLICY_SIZE];
    long autonomyTtlSeconds = 0;
    long autonomyMaxWallClockSeconds = 0;
    long autonomyRequiresCleanWorktree = 0;
    long maxTokens = 0;
    long long createdAtMs = 0;
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "initiativeId", initiativeId, sizeof(initiativeId), "");
    readStringOrDefault(line, "parentTaskId", parentTaskId, sizeof(parentTaskId), "");
    readStringOrDefault(line, "requestedByRole", requestedByRole, sizeof(requestedByRole), "");
    readStringOrDefault(line, "assignedRole", assignedRole, sizeof(assignedRole), "");
    readStringOrDefault(line, "assignedAgent", assignedAgent, sizeof(assignedAgent), "all");
    readStringOrDefault(line, "modelId", modelId, sizeof(modelId), "");
    readStringOrDefault(line, "priority", priority, sizeof(priority), "normal");
    readStringOrDefault(line, "status", status, sizeof(status), "queued");
    readStringOrDefault(line, "title", title, sizeof(title), "Untitled task");
    readStringOrDefault(line, "body", body, sizeof(body), "");
    readStringOrDefault(line, "profile", toolProfile, sizeof(toolProfile), "observe");
    readStringOrDefault(line, "autonomyLevel", autonomyLevel, sizeof(autonomyLevel), "");
    readStringOrDefault(line, "scope", autonomyScope, sizeof(autonomyScope), "");
    readStringOrDefault(line, "network", autonomyNetwork, sizeof(autonomyNetwork), "none");
    readStringOrDefault(line, "credentialClass", autonomyCredentialClass, sizeof(autonomyCredentialClass), "none");
    wtJsonReadLong(line, "ttlSeconds", &autonomyTtlSeconds);
    wtJsonReadLong(line, "maxWallClockSeconds", &autonomyMaxWallClockSeconds);
    wtJsonReadLong(line, "requiresCleanWorktree", &autonomyRequiresCleanWorktree);
    wtJsonReadLong(line, "maxTokens", &maxTokens);
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, taskId);
    bindText(stmt, 2, initiativeId);
    bindText(stmt, 3, parentTaskId);
    bindText(stmt, 4, requestedByRole);
    bindText(stmt, 5, assignedRole);
    bindText(stmt, 6, assignedAgent);
    bindText(stmt, 7, modelId);
    bindText(stmt, 8, priority);
    bindText(stmt, 9, status);
    bindText(stmt, 10, title);
    bindText(stmt, 11, body);
    bindText(stmt, 12, toolProfile);
    bindText(stmt, 13, autonomyLevel);
    bindText(stmt, 14, autonomyScope);
    bindText(stmt, 15, autonomyNetwork);
    bindText(stmt, 16, autonomyCredentialClass);
    sqlite3_bind_int64(stmt, 17, autonomyTtlSeconds);
    sqlite3_bind_int64(stmt, 18, autonomyMaxWallClockSeconds);
    sqlite3_bind_int64(stmt, 19, autonomyRequiresCleanWorktree);
    sqlite3_bind_int64(stmt, 20, maxTokens);
    sqlite3_bind_int64(stmt, 21, createdAtMs);
    sqlite3_bind_int64(stmt, 22, createdAtMs);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return taskId[0] && rc == 0 ? 0 : -1;
}

static int insertEvent(sqlite3 *db, const char *line, const char *schema) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO task_events "
        "(task_id,schema,event_type,status,assigned_agent,message,created_by,created_at_ms,raw_json) "
        "VALUES (?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char taskId[WT_TASK_ID_SIZE];
    char eventType[64];
    char status[WT_TASK_STATUS_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char message[WT_TASK_BODY_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    /* retryCause and reclaimReason are Sprint 3 closeout fields used to drive the
     * operator's visibility into why a task failed or had its lease released. */
    char retryCause[64];
    char reclaimReason[64];
    long long createdAtMs = 0;
    long long leaseExpiresAtMs = 0;
    long attempt = 0;
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "eventType", eventType, sizeof(eventType),
                        strcmp(schema, "woventeam.task_request.v0.1") == 0 ? "request" : "event");
    readStringOrDefault(line, "status", status, sizeof(status), "");
    readStringOrDefault(line, "assignedAgent", assignedAgent, sizeof(assignedAgent), "");
    readStringOrDefault(line, "message", message, sizeof(message), "");
    if (message[0] == '\0') {
        readStringOrDefault(line, "title", message, sizeof(message), "");
    }
    readStringOrDefault(line, "createdBy", createdBy, sizeof(createdBy), "");
    if (createdBy[0] == '\0') {
        readStringOrDefault(line, "requestedBy", createdBy, sizeof(createdBy), "");
    }
    readStringOrDefault(line, "retryCause", retryCause, sizeof(retryCause), "");
    readStringOrDefault(line, "reclaimReason", reclaimReason, sizeof(reclaimReason), "");
    /* Sprint 4 artifact fields parsed alongside lease/reclaim fields so a single
     * pass over the JSON line populates everything the projection might need. */
    char artifactState[64];
    char artifactReviewer[WT_TASK_AGENT_SIZE];
    char artifactNotes[WT_TASK_BODY_SIZE];
    char artifactPath[512];
    readStringOrDefault(line, "artifactState", artifactState, sizeof(artifactState), "");
    readStringOrDefault(line, "reviewer", artifactReviewer, sizeof(artifactReviewer), "");
    readStringOrDefault(line, "reviewNotes", artifactNotes, sizeof(artifactNotes), "");
    readStringOrDefault(line, "artifactPath", artifactPath, sizeof(artifactPath), "");
    /* Sprint 5: operator priority change carries a "priority" field on a
     * task_event with eventType "priority". */
    char eventPriority[32];
    readStringOrDefault(line, "priority", eventPriority, sizeof(eventPriority), "");
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    wtJsonReadLongLong(line, "leaseExpiresAtUnixMs", &leaseExpiresAtMs);
    wtJsonReadLong(line, "attempt", &attempt);
    bindText(stmt, 1, taskId);
    bindText(stmt, 2, schema);
    bindText(stmt, 3, eventType);
    bindText(stmt, 4, status);
    bindText(stmt, 5, assignedAgent);
    bindText(stmt, 6, message);
    bindText(stmt, 7, createdBy);
    sqlite3_bind_int64(stmt, 8, createdAtMs);
    bindText(stmt, 9, line);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    if (rc != 0 || taskId[0] == '\0') {
        return -1;
    }
    if (status[0] != '\0' || assignedAgent[0] != '\0' ||
        strcmp(eventType, "reclaim") == 0 || strcmp(eventType, "artifact") == 0 ||
        strcmp(eventType, "priority") == 0) {
        /*
         * Apply the event to the tasks projection row. The CASE expressions are
         * narrow: each column only updates when the event_type or status warrants
         * it, leaving prior values intact for unrelated events. The reclaim
         * branch clears the lease columns so subsequent lease events look fresh.
         */
        sqlite3_stmt *update = NULL;
        const char *updateSql =
            "UPDATE tasks SET "
            "status=CASE WHEN ? != '' THEN ? ELSE status END,"
            "assigned_agent=CASE WHEN ? != '' THEN ? ELSE assigned_agent END,"
            "lease_owner=CASE "
            "  WHEN ? = 'lease' THEN ? "
            "  WHEN ? = 'reclaim' THEN '' "
            "  ELSE lease_owner END,"
            "lease_expires_at_ms=CASE "
            "  WHEN ? = 'lease' THEN ? "
            "  WHEN ? = 'reclaim' THEN 0 "
            "  ELSE lease_expires_at_ms END,"
            "leased_at_ms=CASE "
            "  WHEN ? = 'lease' THEN ? "
            "  WHEN ? = 'reclaim' THEN 0 "
            "  ELSE leased_at_ms END,"
            "running_at_ms=CASE "
            "  WHEN ? = 'status' AND ? = 'running' THEN ? "
            "  WHEN ? = 'reclaim' THEN 0 "
            "  ELSE running_at_ms END,"
            "attempt_count=CASE WHEN ? = 'lease' THEN max(attempt_count + 1, ?) ELSE attempt_count END,"
            "failure_cause=CASE "
            "  WHEN ? = 'status' AND ? = 'failed' AND ? != '' THEN ? "
            "  WHEN ? = 'lease' OR ? = 'reclaim' THEN '' "
            "  ELSE failure_cause END,"
            "last_reclaim_reason=CASE WHEN ? = 'reclaim' THEN ? ELSE last_reclaim_reason END,"
            "reclaim_count=CASE WHEN ? = 'reclaim' THEN reclaim_count + 1 ELSE reclaim_count END,"
            /* Artifact lifecycle columns: only update when eventType=artifact.
             * accepted_at_ms and accepted_artifact_path latch on the first
             * 'accepted' state and stay populated even after later supersede
             * events, so the inventory keeps a stable "what was accepted" log. */
            "artifact_state=CASE WHEN ? = 'artifact' AND ? != '' THEN ? ELSE artifact_state END,"
            "last_reviewer=CASE WHEN ? = 'artifact' AND ? != '' THEN ? ELSE last_reviewer END,"
            "last_review_notes=CASE WHEN ? = 'artifact' THEN ? ELSE last_review_notes END,"
            "accepted_at_ms=CASE WHEN ? = 'artifact' AND ? = 'accepted' THEN ? ELSE accepted_at_ms END,"
            "accepted_artifact_path=CASE WHEN ? = 'artifact' AND ? = 'accepted' AND ? != '' THEN ? ELSE accepted_artifact_path END,"
            "priority=CASE WHEN ? = 'priority' AND ? != '' THEN ? ELSE priority END,"
            "updated_at_ms=max(updated_at_ms,?),"
            "event_count=event_count+1 "
            "WHERE task_id=?";
        if (sqlite3_prepare_v2(db, updateSql, -1, &update, NULL) != SQLITE_OK) {
            return -1;
        }
        int parameterIndex = 1;
        /* status -> column status */
        bindText(update, parameterIndex++, status);
        bindText(update, parameterIndex++, status);
        /* assignedAgent -> column assigned_agent */
        bindText(update, parameterIndex++, assignedAgent);
        bindText(update, parameterIndex++, assignedAgent);
        /* lease_owner: lease branch / reclaim branch */
        bindText(update, parameterIndex++, eventType);   /* CASE WHEN lease */
        bindText(update, parameterIndex++, assignedAgent);
        bindText(update, parameterIndex++, eventType);   /* CASE WHEN reclaim */
        /* lease_expires_at_ms */
        bindText(update, parameterIndex++, eventType);
        sqlite3_bind_int64(update, parameterIndex++, leaseExpiresAtMs);
        bindText(update, parameterIndex++, eventType);
        /* leased_at_ms */
        bindText(update, parameterIndex++, eventType);
        sqlite3_bind_int64(update, parameterIndex++, createdAtMs);
        bindText(update, parameterIndex++, eventType);
        /* running_at_ms: status==running OR reclaim clears it */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, status);
        sqlite3_bind_int64(update, parameterIndex++, createdAtMs);
        bindText(update, parameterIndex++, eventType);
        /* attempt_count: lease bumps the counter */
        bindText(update, parameterIndex++, eventType);
        sqlite3_bind_int64(update, parameterIndex++, attempt);
        /* failure_cause: set on failed status with cause, cleared on lease/reclaim */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, status);
        bindText(update, parameterIndex++, retryCause);
        bindText(update, parameterIndex++, retryCause);
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, eventType);
        /* last_reclaim_reason and reclaim_count */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, reclaimReason);
        bindText(update, parameterIndex++, eventType);
        /* artifact_state: only update when eventType=artifact and a state was given */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, artifactState);
        bindText(update, parameterIndex++, artifactState);
        /* last_reviewer: only update when eventType=artifact with a reviewer */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, artifactReviewer);
        bindText(update, parameterIndex++, artifactReviewer);
        /* last_review_notes: every artifact event refreshes notes (even empty) */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, artifactNotes);
        /* accepted_at_ms / accepted_artifact_path latch on artifact + accepted */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, artifactState);
        sqlite3_bind_int64(update, parameterIndex++, createdAtMs);
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, artifactState);
        bindText(update, parameterIndex++, artifactPath);
        bindText(update, parameterIndex++, artifactPath);
        /* priority: operator priority change updates the column */
        bindText(update, parameterIndex++, eventType);
        bindText(update, parameterIndex++, eventPriority);
        bindText(update, parameterIndex++, eventPriority);
        /* updated_at_ms + WHERE task_id */
        sqlite3_bind_int64(update, parameterIndex++, createdAtMs);
        bindText(update, parameterIndex++, taskId);
        rc = sqlite3_step(update) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(update);
    }
    return rc;
}

static int projectAgentControl(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO agent_controls (agent,state,message,updated_at_ms) VALUES (?,?,?,?) "
        "ON CONFLICT(agent) DO UPDATE SET "
        "state=excluded.state,message=excluded.message,updated_at_ms=excluded.updated_at_ms";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char agent[WT_TASK_AGENT_SIZE];
    char action[32];
    char message[WT_TASK_TITLE_SIZE];
    long long createdAtMs = 0;
    readStringOrDefault(line, "agent", agent, sizeof(agent), "");
    readStringOrDefault(line, "action", action, sizeof(action), "");
    readStringOrDefault(line, "message", message, sizeof(message), "");
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, agent);
    bindText(stmt, 2, strcmp(action, "pause") == 0 ? "paused" : "active");
    bindText(stmt, 3, message);
    sqlite3_bind_int64(stmt, 4, createdAtMs);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return agent[0] && rc == 0 ? 0 : -1;
}

/*
 * Phase 3 Sprint 1: project a heartbeat record. One row per agent; the upsert
 * always overwrites with the latest observation so /api/status sees the most
 * recent state without needing to scan the ledger.
 */
static int projectHeartbeat(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO heartbeats (agent,host,current_task_id,lease_expires_at_ms,status_line,last_seen_ms) "
        "VALUES (?,?,?,?,?,?) "
        "ON CONFLICT(agent) DO UPDATE SET "
        "host=excluded.host,current_task_id=excluded.current_task_id,"
        "lease_expires_at_ms=excluded.lease_expires_at_ms,"
        "status_line=excluded.status_line,last_seen_ms=excluded.last_seen_ms";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char agent[WT_TASK_AGENT_SIZE];
    char host[128];
    char currentTaskId[WT_TASK_ID_SIZE];
    char statusLine[WT_TASK_BODY_SIZE];
    long long leaseExpiresAtMs = 0;
    long long createdAtMs = 0;
    readStringOrDefault(line, "agent", agent, sizeof(agent), "");
    readStringOrDefault(line, "host", host, sizeof(host), "");
    readStringOrDefault(line, "currentTaskId", currentTaskId, sizeof(currentTaskId), "");
    readStringOrDefault(line, "statusLine", statusLine, sizeof(statusLine), "");
    wtJsonReadLongLong(line, "leaseExpiresAtUnixMs", &leaseExpiresAtMs);
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, agent);
    bindText(stmt, 2, host);
    bindText(stmt, 3, currentTaskId);
    sqlite3_bind_int64(stmt, 4, leaseExpiresAtMs);
    bindText(stmt, 5, statusLine);
    sqlite3_bind_int64(stmt, 6, createdAtMs);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return agent[0] && rc == 0 ? 0 : -1;
}

/*
 * Phase 3 Sprint 1: project a milestone record. Append-only - the projection
 * stores every milestone so the audit + status board can show the full chain.
 */
static int projectMilestone(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO milestones (task_id,milestone,message,created_by,created_at_ms) "
        "VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char taskId[WT_TASK_ID_SIZE];
    char milestone[64];
    char message[WT_TASK_BODY_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    long long createdAtMs = 0;
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "milestone", milestone, sizeof(milestone), "");
    readStringOrDefault(line, "message", message, sizeof(message), "");
    readStringOrDefault(line, "createdBy", createdBy, sizeof(createdBy), "");
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, taskId);
    bindText(stmt, 2, milestone);
    bindText(stmt, 3, message);
    bindText(stmt, 4, createdBy);
    sqlite3_bind_int64(stmt, 5, createdAtMs);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return taskId[0] && rc == 0 ? 0 : -1;
}

/*
 * Phase 3 Sprint 1: project a kill_event record. Append-only; the wt-agent
 * cancel-check uses the JSONL directly, so this table exists primarily for
 * audit export + status surfaces.
 */
static int projectKillEvent(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO kill_events (task_id,reason,created_by,created_at_ms) VALUES (?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char taskId[WT_TASK_ID_SIZE];
    char reason[128];
    char createdBy[WT_TASK_AGENT_SIZE];
    long long createdAtMs = 0;
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "reason", reason, sizeof(reason), "");
    readStringOrDefault(line, "createdBy", createdBy, sizeof(createdBy), "");
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, taskId);
    bindText(stmt, 2, reason);
    bindText(stmt, 3, createdBy);
    sqlite3_bind_int64(stmt, 4, createdAtMs);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return taskId[0] && rc == 0 ? 0 : -1;
}

/*
 * Phase 3 Sprint 3: deliverable + secret_scan projection rows.
 *
 * Both are append-only - each ship produces one deliverable row and zero or
 * one secret_scan row. The deliverable row preserves enough fields to render
 * the audit panel and the per-initiative Deliverables sub-panel without
 * re-parsing the ledger; the raw JSON is also stored for downstream tools
 * that need fields we did not pre-project.
 */
static int projectDeliverable(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO deliverables ("
        " deliverable_id,task_id,initiative_id,source_workspace_path,"
        " deliverable_path,packaging_mode,size_bytes,sha256,reviewer,"
        " supersedes,created_by,created_at_ms,raw_json) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char deliverableId[128];
    char taskId[WT_TASK_ID_SIZE];
    char initiativeId[WT_TASK_ID_SIZE];
    char sourcePath[512];
    char deliverablePath[512];
    char packagingMode[32];
    char sha256[80];
    char reviewer[WT_TASK_AGENT_SIZE];
    char supersedes[128];
    char createdBy[WT_TASK_AGENT_SIZE];
    long long sizeBytes = 0;
    long long createdAtMs = 0;
    readStringOrDefault(line, "deliverableId", deliverableId, sizeof(deliverableId), "");
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "initiativeId", initiativeId, sizeof(initiativeId), "");
    readStringOrDefault(line, "sourceWorkspacePath", sourcePath, sizeof(sourcePath), "");
    readStringOrDefault(line, "deliverablePath", deliverablePath, sizeof(deliverablePath), "");
    readStringOrDefault(line, "packagingMode", packagingMode, sizeof(packagingMode), "copy");
    readStringOrDefault(line, "sha256", sha256, sizeof(sha256), "");
    readStringOrDefault(line, "reviewer", reviewer, sizeof(reviewer), "");
    /* `supersedes` is JSON null when not used - readStringOrDefault returns ""
     * either way. That's fine, the column is empty string in both cases. */
    readStringOrDefault(line, "supersedes", supersedes, sizeof(supersedes), "");
    readStringOrDefault(line, "createdBy", createdBy, sizeof(createdBy), "");
    wtJsonReadLongLong(line, "sizeBytes", &sizeBytes);
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    bindText(stmt, 1, deliverableId);
    bindText(stmt, 2, taskId);
    bindText(stmt, 3, initiativeId);
    bindText(stmt, 4, sourcePath);
    bindText(stmt, 5, deliverablePath);
    bindText(stmt, 6, packagingMode);
    sqlite3_bind_int64(stmt, 7, sizeBytes);
    bindText(stmt, 8, sha256);
    bindText(stmt, 9, reviewer);
    bindText(stmt, 10, supersedes);
    bindText(stmt, 11, createdBy);
    sqlite3_bind_int64(stmt, 12, createdAtMs);
    bindText(stmt, 13, line);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return deliverableId[0] && rc == 0 ? 0 : -1;
}

static int projectSecretScan(sqlite3 *db, const char *line) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO secret_scans ("
        " scan_id,deliverable_id,task_id,scanned_path,matched,hit_count,"
        " packaging_mode,created_at_ms,raw_json) "
        "VALUES (?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    char scanId[128];
    char deliverableId[128];
    char taskId[WT_TASK_ID_SIZE];
    char scannedPath[512];
    char packagingMode[32];
    long hitCount = 0;
    long long createdAtMs = 0;
    int matched = 0;
    readStringOrDefault(line, "scanId", scanId, sizeof(scanId), "");
    readStringOrDefault(line, "deliverableId", deliverableId, sizeof(deliverableId), "");
    readStringOrDefault(line, "taskId", taskId, sizeof(taskId), "");
    readStringOrDefault(line, "scannedPath", scannedPath, sizeof(scannedPath), "");
    readStringOrDefault(line, "packagingMode", packagingMode, sizeof(packagingMode), "");
    wtJsonReadLong(line, "hitCount", &hitCount);
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
    /* Boolean parse via substring scan - tolerant of spaces. */
    {
        const char *m = strstr(line, "\"matched\"");
        if (m) {
            const char *colon = strchr(m, ':');
            if (colon) {
                const char *v = colon + 1;
                while (*v == ' ' || *v == '\t') v++;
                if (strncmp(v, "true", 4) == 0) matched = 1;
            }
        }
    }
    bindText(stmt, 1, scanId);
    bindText(stmt, 2, deliverableId);
    bindText(stmt, 3, taskId);
    bindText(stmt, 4, scannedPath);
    sqlite3_bind_int(stmt, 5, matched);
    sqlite3_bind_int64(stmt, 6, hitCount);
    bindText(stmt, 7, packagingMode);
    sqlite3_bind_int64(stmt, 8, createdAtMs);
    bindText(stmt, 9, line);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return scanId[0] && rc == 0 ? 0 : -1;
}

int wtTaskProjectionRebuild(const char *dbPath, const char *ledgerPath) {
    if (wtRoomEnsureParentDirs(dbPath) != 0) {
        return -1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    if (execSql(db, "PRAGMA journal_mode=WAL;") != 0 ||
        execSql(db, "BEGIN IMMEDIATE;") != 0 ||
        execSql(db, "DROP TABLE IF EXISTS tasks; DROP TABLE IF EXISTS task_events; DROP TABLE IF EXISTS agent_controls; DROP TABLE IF EXISTS heartbeats; DROP TABLE IF EXISTS milestones; DROP TABLE IF EXISTS kill_events; DROP TABLE IF EXISTS deliverables; DROP TABLE IF EXISTS secret_scans;") != 0 ||
        execSql(db,
            "CREATE TABLE tasks ("
            "task_id TEXT PRIMARY KEY,"
            "initiative_id TEXT,parent_task_id TEXT,requested_by_role TEXT,"
            "assigned_role TEXT,assigned_agent TEXT,model_id TEXT,priority TEXT,status TEXT,"
            "title TEXT,body TEXT,tool_profile TEXT,max_tokens INTEGER DEFAULT 0,"
            "autonomy_level TEXT DEFAULT '',autonomy_scope TEXT DEFAULT '',"
            "autonomy_network TEXT DEFAULT 'none',autonomy_credential_class TEXT DEFAULT 'none',"
            "autonomy_ttl_seconds INTEGER DEFAULT 0,autonomy_max_wall_clock_seconds INTEGER DEFAULT 0,"
            "autonomy_requires_clean_worktree INTEGER DEFAULT 0,"
            "created_at_ms INTEGER DEFAULT 0,updated_at_ms INTEGER DEFAULT 0,event_count INTEGER DEFAULT 0,"
            "lease_owner TEXT DEFAULT '',lease_expires_at_ms INTEGER DEFAULT 0,"
            "leased_at_ms INTEGER DEFAULT 0,running_at_ms INTEGER DEFAULT 0,attempt_count INTEGER DEFAULT 0,"
            /* Sprint 3 closeout columns:
             *   failure_cause     - last classified retryCause for a failed attempt
             *   last_reclaim_reason - last reason a lease was released (operator vs lease_expired)
             *   reclaim_count     - cumulative reclaim events recorded for this task
             */
            "failure_cause TEXT DEFAULT '',last_reclaim_reason TEXT DEFAULT '',"
            "reclaim_count INTEGER DEFAULT 0,"
            /* Sprint 4 artifact promotion columns:
             *   artifact_state          - latest artifact lifecycle state
             *                             (draft|reviewed|accepted|rejected|superseded)
             *   last_reviewer           - who recorded the latest artifact decision
             *   last_review_notes       - free-text justification from latest decision
             *   accepted_at_ms          - timestamp of the most recent accepted event
             *   accepted_artifact_path  - workspace-relative path of accepted artifact
             */
            "artifact_state TEXT DEFAULT '',last_reviewer TEXT DEFAULT '',"
            "last_review_notes TEXT DEFAULT '',accepted_at_ms INTEGER DEFAULT 0,"
            "accepted_artifact_path TEXT DEFAULT '');"
            "CREATE INDEX idx_tasks_status ON tasks(status);"
            "CREATE INDEX idx_tasks_artifact ON tasks(artifact_state);"
            "CREATE INDEX idx_tasks_agent ON tasks(assigned_agent);"
            "CREATE INDEX idx_tasks_initiative ON tasks(initiative_id);"
            "CREATE TABLE task_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT,schema TEXT,event_type TEXT,status TEXT,"
            "assigned_agent TEXT,message TEXT,created_by TEXT,created_at_ms INTEGER DEFAULT 0,raw_json TEXT);"
            "CREATE INDEX idx_task_events_task ON task_events(task_id,id);"
            "CREATE TABLE agent_controls (agent TEXT PRIMARY KEY,state TEXT,message TEXT,updated_at_ms INTEGER DEFAULT 0);"
            /*
             * Phase 3 Sprint 1 projection tables:
             *   heartbeats - one row per agent, last seen heartbeat
             *   milestones - one row per milestone event (chronological)
             *   kill_events - one row per cancel request (chronological)
             */
            "CREATE TABLE heartbeats ("
            "agent TEXT PRIMARY KEY,host TEXT,current_task_id TEXT,"
            "lease_expires_at_ms INTEGER DEFAULT 0,status_line TEXT,"
            "last_seen_ms INTEGER DEFAULT 0);"
            "CREATE TABLE milestones ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT,milestone TEXT,"
            "message TEXT,created_by TEXT,created_at_ms INTEGER DEFAULT 0);"
            "CREATE INDEX idx_milestones_task ON milestones(task_id,id);"
            "CREATE TABLE kill_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT,reason TEXT,"
            "created_by TEXT,created_at_ms INTEGER DEFAULT 0);"
            "CREATE INDEX idx_kill_events_task ON kill_events(task_id,id);"
            /*
             * Phase 3 Sprint 3 projection tables:
             *   deliverables - one row per ship event
             *   secret_scans - zero-or-one row per ship event (only when
             *                  patterns file is configured)
             */
            "CREATE TABLE deliverables ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,deliverable_id TEXT,"
            "task_id TEXT,initiative_id TEXT,source_workspace_path TEXT,"
            "deliverable_path TEXT,packaging_mode TEXT,size_bytes INTEGER DEFAULT 0,"
            "sha256 TEXT,reviewer TEXT,supersedes TEXT DEFAULT '',"
            "created_by TEXT,created_at_ms INTEGER DEFAULT 0,raw_json TEXT);"
            "CREATE INDEX idx_deliverables_initiative ON deliverables(initiative_id,id);"
            "CREATE INDEX idx_deliverables_task ON deliverables(task_id,id);"
            "CREATE TABLE secret_scans ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,scan_id TEXT,"
            "deliverable_id TEXT,task_id TEXT,scanned_path TEXT,"
            "matched INTEGER DEFAULT 0,hit_count INTEGER DEFAULT 0,"
            "packaging_mode TEXT,created_at_ms INTEGER DEFAULT 0,raw_json TEXT);"
            "CREATE INDEX idx_secret_scans_deliverable ON secret_scans(deliverable_id,id);") != 0) {
        execSql(db, "ROLLBACK;");
        sqlite3_close(db);
        return -1;
    }
    FILE *file = fopen(ledgerPath, "r");
    if (file) {
        char line[WT_TASK_LEDGER_LINE_SIZE];
        while (fgets(line, sizeof(line), file)) {
            size_t length = strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
                line[--length] = '\0';
            }
            if (lineHasSchema(line, "woventeam.task_package.v0.1")) {
                projectPackage(db, line);
            } else if (lineHasSchema(line, "woventeam.task_event.v0.1")) {
                insertEvent(db, line, "woventeam.task_event.v0.1");
            } else if (lineHasSchema(line, "woventeam.task_request.v0.1")) {
                insertEvent(db, line, "woventeam.task_request.v0.1");
            } else if (lineHasSchema(line, "woventeam.agent_control.v0.1")) {
                projectAgentControl(db, line);
            } else if (lineHasSchema(line, "woventeam.heartbeat.v0.1")) {
                projectHeartbeat(db, line);
            } else if (lineHasSchema(line, "woventeam.milestone.v0.1")) {
                projectMilestone(db, line);
            } else if (lineHasSchema(line, "woventeam.kill_event.v0.1")) {
                projectKillEvent(db, line);
            } else if (lineHasSchema(line, "woventeam.deliverable.v0.1")) {
                projectDeliverable(db, line);
            } else if (lineHasSchema(line, "woventeam.secret_scan.v0.1")) {
                projectSecretScan(db, line);
            }
        }
        fclose(file);
    }
    int rc = execSql(db, "COMMIT;");
    sqlite3_close(db);
    return rc;
}

static int appendRaw(char *buffer, size_t bufferSize, size_t *used, const char *text) {
    size_t length = strlen(text);
    if (*used + length + 1 >= bufferSize) {
        return -1;
    }
    memcpy(buffer + *used, text, length);
    *used += length;
    buffer[*used] = '\0';
    return 0;
}

static int appendJsonString(char *buffer, size_t bufferSize, size_t *used, const char *text) {
    char escaped[WT_TASK_BODY_SIZE * 2];
    if (wtJsonEscape(text ? text : "", escaped, sizeof(escaped)) != 0) {
        return -1;
    }
    if (appendRaw(buffer, bufferSize, used, "\"") != 0 ||
        appendRaw(buffer, bufferSize, used, escaped) != 0 ||
        appendRaw(buffer, bufferSize, used, "\"") != 0) {
        return -1;
    }
    return 0;
}

static const char *columnText(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? (const char *)value : "";
}

int wtTaskProjectionReadSummariesJson(const char *dbPath, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT task_id,initiative_id,parent_task_id,requested_by_role,assigned_role,assigned_agent,"
        "model_id,priority,status,title,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count,"
        "lease_owner,lease_expires_at_ms,leased_at_ms,running_at_ms,attempt_count,"
        "failure_cause,last_reclaim_reason,reclaim_count,"
        "artifact_state,last_reviewer,last_review_notes,accepted_at_ms,accepted_artifact_path "
        "FROM tasks ORDER BY updated_at_ms DESC, created_at_ms DESC LIMIT 200";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        /* 320 bytes is large enough for the combined numeric/key block emitted
         * below; the 128-byte buffer used previously could truncate once the
         * lease columns and Sprint 3 closeout columns combined exceeded ~140
         * bytes of static text plus four 64-bit integers. */
        char number[320];
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{");
        const char *keys[] = {"taskId","initiativeId","parentTaskId","requestedByRole","assignedRole",
                              "assignedAgent","modelId","priority","status","title","toolProfile"};
        for (int index = 0; index < 11; index++) {
            if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
            appendRaw(buffer, bufferSize, &used, "\"");
            appendRaw(buffer, bufferSize, &used, keys[index]);
            appendRaw(buffer, bufferSize, &used, "\":");
            appendJsonString(buffer, bufferSize, &used, columnText(stmt, index));
        }
        snprintf(number, sizeof(number),
                 ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"eventCount\":%d,"
                 "\"leaseOwner\":",
                 sqlite3_column_int64(stmt, 11),
                 sqlite3_column_int64(stmt, 12),
                 sqlite3_column_int64(stmt, 13),
                 sqlite3_column_int(stmt, 14));
        appendRaw(buffer, bufferSize, &used, number);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 15));
        snprintf(number, sizeof(number),
                 ",\"leaseExpiresAtUnixMs\":%lld,\"leasedAtUnixMs\":%lld,\"runningAtUnixMs\":%lld,\"attemptCount\":%d,"
                 "\"failureCause\":",
                 sqlite3_column_int64(stmt, 16),
                 sqlite3_column_int64(stmt, 17),
                 sqlite3_column_int64(stmt, 18),
                 sqlite3_column_int(stmt, 19));
        appendRaw(buffer, bufferSize, &used, number);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 20));
        appendRaw(buffer, bufferSize, &used, ",\"lastReclaimReason\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 21));
        snprintf(number, sizeof(number), ",\"reclaimCount\":%d,\"artifactState\":", sqlite3_column_int(stmt, 22));
        appendRaw(buffer, bufferSize, &used, number);
        /* Sprint 4 artifact fields - keep snapshot-friendly order so callers can
         * inspect promotion status without a second fetch. */
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 23));
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewer\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 24));
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewNotes\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 25));
        snprintf(number, sizeof(number), ",\"acceptedAtUnixMs\":%lld,\"acceptedArtifactPath\":",
                 sqlite3_column_int64(stmt, 26));
        appendRaw(buffer, bufferSize, &used, number);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 27));
        appendRaw(buffer, bufferSize, &used, "}");
    }
    appendRaw(buffer, bufferSize, &used, "]");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

int wtTaskProjectionReadDetailJson(const char *dbPath, const char *taskId, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *task = NULL;
    const char *taskSql =
        "SELECT task_id,initiative_id,parent_task_id,requested_by_role,assigned_role,assigned_agent,"
        "model_id,priority,status,title,body,tool_profile,"
        "autonomy_level,autonomy_scope,autonomy_network,autonomy_credential_class,"
        "autonomy_ttl_seconds,autonomy_max_wall_clock_seconds,autonomy_requires_clean_worktree,"
        "max_tokens,created_at_ms,updated_at_ms,event_count,"
        "lease_owner,lease_expires_at_ms,leased_at_ms,running_at_ms,attempt_count,"
        "failure_cause,last_reclaim_reason,reclaim_count,"
        "artifact_state,last_reviewer,last_review_notes,accepted_at_ms,accepted_artifact_path "
        "FROM tasks WHERE task_id=?";
    if (sqlite3_prepare_v2(db, taskSql, -1, &task, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(task, 1, taskId);
    if (sqlite3_step(task) != SQLITE_ROW) {
        sqlite3_finalize(task);
        sqlite3_close(db);
        snprintf(buffer, bufferSize, "{\"ok\":false,\"error\":\"task not found\"}");
        return 1;
    }
    size_t used = 0;
    char number[256];
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"task\":{");
    const char *keys[] = {"taskId","initiativeId","parentTaskId","requestedByRole","assignedRole",
                          "assignedAgent","modelId","priority","status","title","body","toolProfile"};
    for (int index = 0; index < 12; index++) {
        if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
        appendRaw(buffer, bufferSize, &used, "\"");
        appendRaw(buffer, bufferSize, &used, keys[index]);
        appendRaw(buffer, bufferSize, &used, "\":");
        appendJsonString(buffer, bufferSize, &used, columnText(task, index));
    }
    appendRaw(buffer, bufferSize, &used, ",\"autonomyLevel\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 12));
    appendRaw(buffer, bufferSize, &used, ",\"autonomyGrant\":{\"scope\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 13));
    appendRaw(buffer, bufferSize, &used, ",\"network\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 14));
    appendRaw(buffer, bufferSize, &used, ",\"credentialClass\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 15));
    snprintf(number, sizeof(number),
             ",\"ttlSeconds\":%lld,\"maxWallClockSeconds\":%lld,\"requiresCleanWorktree\":%s}",
             sqlite3_column_int64(task, 16),
             sqlite3_column_int64(task, 17),
             sqlite3_column_int(task, 18) ? "true" : "false");
    appendRaw(buffer, bufferSize, &used, number);
    snprintf(number, sizeof(number),
             ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"eventCount\":%d,\"leaseOwner\":",
             sqlite3_column_int64(task, 19),
             sqlite3_column_int64(task, 20),
             sqlite3_column_int64(task, 21),
             sqlite3_column_int(task, 22));
    appendRaw(buffer, bufferSize, &used, number);
    appendJsonString(buffer, bufferSize, &used, columnText(task, 23));
    snprintf(number, sizeof(number),
             ",\"leaseExpiresAtUnixMs\":%lld,\"leasedAtUnixMs\":%lld,\"runningAtUnixMs\":%lld,\"attemptCount\":%d,"
             "\"failureCause\":",
             sqlite3_column_int64(task, 24),
             sqlite3_column_int64(task, 25),
             sqlite3_column_int64(task, 26),
             sqlite3_column_int(task, 27));
    appendRaw(buffer, bufferSize, &used, number);
    appendJsonString(buffer, bufferSize, &used, columnText(task, 28));
    appendRaw(buffer, bufferSize, &used, ",\"lastReclaimReason\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 29));
    snprintf(number, sizeof(number), ",\"reclaimCount\":%d,\"artifactState\":", sqlite3_column_int(task, 30));
    appendRaw(buffer, bufferSize, &used, number);
    appendJsonString(buffer, bufferSize, &used, columnText(task, 31));
    appendRaw(buffer, bufferSize, &used, ",\"lastReviewer\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 32));
    appendRaw(buffer, bufferSize, &used, ",\"lastReviewNotes\":");
    appendJsonString(buffer, bufferSize, &used, columnText(task, 33));
    snprintf(number, sizeof(number), ",\"acceptedAtUnixMs\":%lld,\"acceptedArtifactPath\":",
             sqlite3_column_int64(task, 34));
    appendRaw(buffer, bufferSize, &used, number);
    appendJsonString(buffer, bufferSize, &used, columnText(task, 35));
    appendRaw(buffer, bufferSize, &used, "},\"events\":[");
    sqlite3_finalize(task);

    sqlite3_stmt *event = NULL;
    const char *eventSql =
        "SELECT id,schema,event_type,status,assigned_agent,message,created_by,created_at_ms "
        "FROM task_events WHERE task_id=? ORDER BY id ASC";
    if (sqlite3_prepare_v2(db, eventSql, -1, &event, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(event, 1, taskId);
    int first = 1;
    while (sqlite3_step(event) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        snprintf(number, sizeof(number), "{\"id\":%lld,", sqlite3_column_int64(event, 0));
        appendRaw(buffer, bufferSize, &used, number);
        const char *eventKeys[] = {"schema","eventType","status","assignedAgent","message","createdBy"};
        for (int index = 0; index < 6; index++) {
            if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
            appendRaw(buffer, bufferSize, &used, "\"");
            appendRaw(buffer, bufferSize, &used, eventKeys[index]);
            appendRaw(buffer, bufferSize, &used, "\":");
            appendJsonString(buffer, bufferSize, &used, columnText(event, index + 1));
        }
        snprintf(number, sizeof(number), ",\"createdAtUnixMs\":%lld}", sqlite3_column_int64(event, 7));
        appendRaw(buffer, bufferSize, &used, number);
    }
    appendRaw(buffer, bufferSize, &used, "]}");
    sqlite3_finalize(event);
    sqlite3_close(db);
    return 0;
}

int wtTaskProjectionReadInitiativesJson(const char *dbPath, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT initiative_id,"
        "COUNT(*),"
        "SUM(CASE WHEN status NOT IN ('complete','failed','cancelled','closed') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='complete' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='failed' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='blocked' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status IN ('approved','rejected','revision_requested') THEN 1 ELSE 0 END),"
        "SUM(max_tokens),MIN(created_at_ms),MAX(updated_at_ms),"
        "(SELECT title FROM tasks t2 WHERE t2.initiative_id=t.initiative_id "
        " ORDER BY created_at_ms ASC, task_id ASC LIMIT 1) "
        "FROM tasks t WHERE initiative_id != '' "
        "GROUP BY initiative_id ORDER BY MAX(updated_at_ms) DESC, initiative_id LIMIT 100";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"initiatives\":[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char number[512];
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{\"initiativeId\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 0));
        appendRaw(buffer, bufferSize, &used, ",\"title\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 10));
        snprintf(number, sizeof(number),
                 ",\"taskCount\":%d,\"activeTasks\":%d,\"completeTasks\":%d,"
                 "\"failedTasks\":%d,\"blockedTasks\":%d,\"openGateTasks\":%d,"
                 "\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld}",
                 sqlite3_column_int(stmt, 1),
                 sqlite3_column_int(stmt, 2),
                 sqlite3_column_int(stmt, 3),
                 sqlite3_column_int(stmt, 4),
                 sqlite3_column_int(stmt, 5),
                 sqlite3_column_int(stmt, 6),
                 sqlite3_column_int64(stmt, 7),
                 sqlite3_column_int64(stmt, 8),
                 sqlite3_column_int64(stmt, 9));
        appendRaw(buffer, bufferSize, &used, number);
    }
    appendRaw(buffer, bufferSize, &used, "]}\n");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

int wtTaskProjectionReadInitiativeDetailJson(const char *dbPath, const char *initiativeId,
                                             char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *summary = NULL;
    const char *summarySql =
        "SELECT COUNT(*),"
        "SUM(CASE WHEN status NOT IN ('complete','failed','cancelled','closed') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='complete' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='failed' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='blocked' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status IN ('approved','rejected','revision_requested') THEN 1 ELSE 0 END),"
        "SUM(max_tokens),MIN(created_at_ms),MAX(updated_at_ms),"
        "(SELECT title FROM tasks WHERE initiative_id=? ORDER BY created_at_ms ASC, task_id ASC LIMIT 1) "
        "FROM tasks WHERE initiative_id=?";
    if (sqlite3_prepare_v2(db, summarySql, -1, &summary, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(summary, 1, initiativeId);
    bindText(summary, 2, initiativeId);
    if (sqlite3_step(summary) != SQLITE_ROW || sqlite3_column_int(summary, 0) == 0) {
        sqlite3_finalize(summary);
        sqlite3_close(db);
        snprintf(buffer, bufferSize, "{\"ok\":false,\"error\":\"initiative not found\"}");
        return 1;
    }
    size_t used = 0;
    char number[512];
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"initiative\":{\"initiativeId\":");
    appendJsonString(buffer, bufferSize, &used, initiativeId);
    appendRaw(buffer, bufferSize, &used, ",\"title\":");
    appendJsonString(buffer, bufferSize, &used, columnText(summary, 9));
    snprintf(number, sizeof(number),
             ",\"taskCount\":%d,\"activeTasks\":%d,\"completeTasks\":%d,"
             "\"failedTasks\":%d,\"blockedTasks\":%d,\"openGateTasks\":%d,"
             "\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld},\"tasks\":[",
             sqlite3_column_int(summary, 0),
             sqlite3_column_int(summary, 1),
             sqlite3_column_int(summary, 2),
             sqlite3_column_int(summary, 3),
             sqlite3_column_int(summary, 4),
             sqlite3_column_int(summary, 5),
             sqlite3_column_int64(summary, 6),
             sqlite3_column_int64(summary, 7),
             sqlite3_column_int64(summary, 8));
    appendRaw(buffer, bufferSize, &used, number);
    sqlite3_finalize(summary);

    sqlite3_stmt *task = NULL;
    const char *taskSql =
        "SELECT task_id,parent_task_id,requested_by_role,assigned_role,assigned_agent,"
        "model_id,priority,status,title,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count "
        "FROM tasks WHERE initiative_id=? ORDER BY updated_at_ms DESC, created_at_ms DESC, task_id LIMIT 200";
    if (sqlite3_prepare_v2(db, taskSql, -1, &task, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(task, 1, initiativeId);
    int first = 1;
    while (sqlite3_step(task) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{");
        const char *keys[] = {"taskId","parentTaskId","requestedByRole","assignedRole",
                              "assignedAgent","modelId","priority","status","title","toolProfile"};
        for (int index = 0; index < 10; index++) {
            if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
            appendRaw(buffer, bufferSize, &used, "\"");
            appendRaw(buffer, bufferSize, &used, keys[index]);
            appendRaw(buffer, bufferSize, &used, "\":");
            appendJsonString(buffer, bufferSize, &used, columnText(task, index));
        }
        snprintf(number, sizeof(number),
                 ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"eventCount\":%d}",
                 sqlite3_column_int64(task, 10),
                 sqlite3_column_int64(task, 11),
                 sqlite3_column_int64(task, 12),
                 sqlite3_column_int(task, 13));
        appendRaw(buffer, bufferSize, &used, number);
    }
    appendRaw(buffer, bufferSize, &used, "]}\n");
    sqlite3_finalize(task);
    sqlite3_close(db);
    return 0;
}

static void appendAgentWorkload(sqlite3 *db, char *buffer, size_t bufferSize, size_t *used,
                                const char *agent, long long nowUnixMs) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "SUM(CASE WHEN status NOT IN ('complete','failed','cancelled','closed') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='leased' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='running' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='leased' AND leased_at_ms > 0 AND leased_at_ms < ? THEN 1 "
        "WHEN status='running' AND running_at_ms > 0 AND running_at_ms < ? THEN 1 ELSE 0 END),"
        "SUM(attempt_count),MAX(updated_at_ms) "
        "FROM tasks WHERE assigned_agent=?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return;
    }
    sqlite3_bind_int64(stmt, 1, nowUnixMs - 15LL * 60LL * 1000LL);
    sqlite3_bind_int64(stmt, 2, nowUnixMs - 15LL * 60LL * 1000LL);
    bindText(stmt, 3, agent);
    int active = 0, leased = 0, running = 0, stuck = 0, attempts = 0;
    long long updatedAtMs = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        active = sqlite3_column_int(stmt, 0);
        leased = sqlite3_column_int(stmt, 1);
        running = sqlite3_column_int(stmt, 2);
        stuck = sqlite3_column_int(stmt, 3);
        attempts = sqlite3_column_int(stmt, 4);
        updatedAtMs = sqlite3_column_int64(stmt, 5);
    }
    sqlite3_finalize(stmt);

    char state[32] = "active";
    char message[WT_TASK_TITLE_SIZE] = "";
    long long controlAtMs = 0;
    const char *controlSql = "SELECT state,message,updated_at_ms FROM agent_controls WHERE agent=?";
    if (sqlite3_prepare_v2(db, controlSql, -1, &stmt, NULL) == SQLITE_OK) {
        bindText(stmt, 1, agent);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            snprintf(state, sizeof(state), "%s", columnText(stmt, 0));
            snprintf(message, sizeof(message), "%s", columnText(stmt, 1));
            controlAtMs = sqlite3_column_int64(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }

    char number[512];
    appendRaw(buffer, bufferSize, used, "{\"agent\":");
    appendJsonString(buffer, bufferSize, used, agent);
    appendRaw(buffer, bufferSize, used, ",\"state\":");
    appendJsonString(buffer, bufferSize, used, state);
    appendRaw(buffer, bufferSize, used, ",\"message\":");
    appendJsonString(buffer, bufferSize, used, message);
    snprintf(number, sizeof(number),
             ",\"activeTasks\":%d,\"leasedTasks\":%d,\"runningTasks\":%d,"
             "\"stuckTasks\":%d,\"attempts\":%d,\"updatedAtUnixMs\":%lld,"
             "\"controlAtUnixMs\":%lld}",
             active, leased, running, stuck, attempts, updatedAtMs, controlAtMs);
    appendRaw(buffer, bufferSize, used, number);
}

int wtTaskProjectionReadInitiativeArtifactsJson(const char *dbPath, const char *initiativeId,
                                                char *buffer, size_t bufferSize) {
    /*
     * Accepted-asset inventory for one initiative. The result includes any task
     * with a non-empty artifact_state so the operator can see both pending and
     * accepted decisions. Sort: accepted rows first (by accepted_at_ms DESC),
     * then non-accepted rows by updated_at_ms DESC.
     */
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT task_id,title,assigned_agent,artifact_state,last_reviewer,"
        "last_review_notes,accepted_at_ms,accepted_artifact_path,updated_at_ms "
        "FROM tasks "
        "WHERE initiative_id = ? AND artifact_state != '' "
        "ORDER BY (artifact_state = 'accepted') DESC, accepted_at_ms DESC, updated_at_ms DESC "
        "LIMIT 200";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(stmt, 1, initiativeId);
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"initiativeId\":");
    appendJsonString(buffer, bufferSize, &used, initiativeId);
    appendRaw(buffer, bufferSize, &used, ",\"artifacts\":[");
    int first = 1;
    int accepted = 0;
    int pending = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        const char *state = columnText(stmt, 3);
        if (strcmp(state, "accepted") == 0) accepted++;
        else pending++;
        appendRaw(buffer, bufferSize, &used, "{\"taskId\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 0));
        appendRaw(buffer, bufferSize, &used, ",\"title\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 1));
        appendRaw(buffer, bufferSize, &used, ",\"assignedAgent\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 2));
        appendRaw(buffer, bufferSize, &used, ",\"artifactState\":");
        appendJsonString(buffer, bufferSize, &used, state);
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewer\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 4));
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewNotes\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 5));
        char number[160];
        snprintf(number, sizeof(number),
                 ",\"acceptedAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"acceptedArtifactPath\":",
                 sqlite3_column_int64(stmt, 6),
                 sqlite3_column_int64(stmt, 8));
        appendRaw(buffer, bufferSize, &used, number);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 7));
        appendRaw(buffer, bufferSize, &used, "}");
    }
    sqlite3_finalize(stmt);
    char tail[128];
    snprintf(tail, sizeof(tail), "],\"acceptedCount\":%d,\"pendingCount\":%d}\n", accepted, pending);
    appendRaw(buffer, bufferSize, &used, tail);
    sqlite3_close(db);
    return 0;
}

/*
 * Sprint 5 audit export. Combines:
 *   - initiative summary (counts, budget, timestamps, accepted assets count)
 *   - tasks (full detail rows including artifact_* columns)
 *   - events (task_events rows in chronological order)
 *   - policy decisions (scanned from the JSONL ledger - not projected)
 *   - usage events (scanned from the JSONL ledger - not projected)
 *
 * Returns 1 when the initiative has no tasks (response carries
 * ok:false,error:"initiative not found"). Returns 0 on success, -1 on
 * internal errors.
 */
int wtTaskProjectionReadInitiativeAuditJson(const char *dbPath, const char *ledgerPath,
                                            const char *initiativeId,
                                            long long sinceUnixMs, int limit,
                                            char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"initiativeId\":");
    appendJsonString(buffer, bufferSize, &used, initiativeId);

    /* Initiative summary. */
    sqlite3_stmt *stmt = NULL;
    const char *summarySql =
        "SELECT COUNT(*),"
        "SUM(CASE WHEN status NOT IN ('complete','failed','cancelled','closed') THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='complete' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='failed' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN status='blocked' THEN 1 ELSE 0 END),"
        "SUM(CASE WHEN artifact_state='accepted' THEN 1 ELSE 0 END),"
        "SUM(max_tokens),MIN(created_at_ms),MAX(updated_at_ms),"
        "(SELECT title FROM tasks WHERE initiative_id=? ORDER BY created_at_ms ASC, task_id ASC LIMIT 1) "
        "FROM tasks WHERE initiative_id=?";
    if (sqlite3_prepare_v2(db, summarySql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(stmt, 1, initiativeId);
    bindText(stmt, 2, initiativeId);
    if (sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int(stmt, 0) == 0) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        snprintf(buffer, bufferSize, "{\"ok\":false,\"error\":\"initiative not found\"}\n");
        return 1;
    }
    char number[512];
    snprintf(number, sizeof(number),
             ",\"summary\":{\"taskCount\":%d,\"activeTasks\":%d,\"completeTasks\":%d,"
             "\"failedTasks\":%d,\"blockedTasks\":%d,\"acceptedArtifacts\":%d,"
             "\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"title\":",
             sqlite3_column_int(stmt, 0),
             sqlite3_column_int(stmt, 1),
             sqlite3_column_int(stmt, 2),
             sqlite3_column_int(stmt, 3),
             sqlite3_column_int(stmt, 4),
             sqlite3_column_int(stmt, 5),
             sqlite3_column_int64(stmt, 6),
             sqlite3_column_int64(stmt, 7),
             sqlite3_column_int64(stmt, 8));
    appendRaw(buffer, bufferSize, &used, number);
    appendJsonString(buffer, bufferSize, &used, columnText(stmt, 9));
    appendRaw(buffer, bufferSize, &used, "}");
    sqlite3_finalize(stmt);

    /* Tasks block - one row per task with the columns audit consumers need. */
    appendRaw(buffer, bufferSize, &used, ",\"tasks\":[");
    sqlite3_stmt *taskStmt = NULL;
    const char *taskSql =
        "SELECT task_id,parent_task_id,requested_by_role,assigned_role,assigned_agent,model_id,"
        "priority,status,title,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count,"
        "lease_owner,attempt_count,failure_cause,last_reclaim_reason,reclaim_count,"
        "artifact_state,last_reviewer,last_review_notes,accepted_at_ms,accepted_artifact_path "
        "FROM tasks WHERE initiative_id=? ORDER BY created_at_ms ASC, task_id ASC";
    if (sqlite3_prepare_v2(db, taskSql, -1, &taskStmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(taskStmt, 1, initiativeId);
    int first = 1;
    while (sqlite3_step(taskStmt) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{");
        const char *keys[] = {"taskId","parentTaskId","requestedByRole","assignedRole","assignedAgent",
                              "modelId","priority","status","title","toolProfile"};
        for (int index = 0; index < 10; index++) {
            if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
            appendRaw(buffer, bufferSize, &used, "\"");
            appendRaw(buffer, bufferSize, &used, keys[index]);
            appendRaw(buffer, bufferSize, &used, "\":");
            appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, index));
        }
        char tail[768];
        snprintf(tail, sizeof(tail),
                 ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,"
                 "\"eventCount\":%d,\"attemptCount\":%d,\"reclaimCount\":%d,"
                 "\"acceptedAtUnixMs\":%lld,\"leaseOwner\":",
                 sqlite3_column_int64(taskStmt, 10),
                 sqlite3_column_int64(taskStmt, 11),
                 sqlite3_column_int64(taskStmt, 12),
                 sqlite3_column_int(taskStmt, 13),
                 sqlite3_column_int(taskStmt, 15),
                 sqlite3_column_int(taskStmt, 18),
                 sqlite3_column_int64(taskStmt, 22));
        appendRaw(buffer, bufferSize, &used, tail);
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 14));
        appendRaw(buffer, bufferSize, &used, ",\"failureCause\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 16));
        appendRaw(buffer, bufferSize, &used, ",\"lastReclaimReason\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 17));
        appendRaw(buffer, bufferSize, &used, ",\"artifactState\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 19));
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewer\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 20));
        appendRaw(buffer, bufferSize, &used, ",\"lastReviewNotes\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 21));
        appendRaw(buffer, bufferSize, &used, ",\"acceptedArtifactPath\":");
        appendJsonString(buffer, bufferSize, &used, columnText(taskStmt, 23));
        appendRaw(buffer, bufferSize, &used, "}");
    }
    sqlite3_finalize(taskStmt);

    /* Events block - chronological task_events for any task in this initiative. */
    appendRaw(buffer, bufferSize, &used, "],\"events\":[");
    sqlite3_stmt *eventStmt = NULL;
    /*
     * Phase 3 Sprint 1: optional since/limit pagination. The SQL filters by
     * created_at_ms >= sinceUnixMs when sinceUnixMs > 0; otherwise it returns
     * every event. limit > 0 acts as a soft cap across events + policy +
     * usage combined; the helper tracks emittedCount and stops once limit is
     * hit, recording nextSinceUnixMs as the cursor for the next page.
     */
    const char *eventSql =
        "SELECT id,task_id,schema,event_type,status,assigned_agent,message,created_by,created_at_ms "
        "FROM task_events WHERE task_id IN (SELECT task_id FROM tasks WHERE initiative_id=?) "
        "AND (? = 0 OR created_at_ms >= ?) "
        "ORDER BY created_at_ms ASC, id ASC";
    if (sqlite3_prepare_v2(db, eventSql, -1, &eventStmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(eventStmt, 1, initiativeId);
    sqlite3_bind_int64(eventStmt, 2, sinceUnixMs);
    sqlite3_bind_int64(eventStmt, 3, sinceUnixMs);
    first = 1;
    int emittedCount = 0;
    long long maxEmittedAtMs = 0;
    int truncated = 0;
    while (sqlite3_step(eventStmt) == SQLITE_ROW) {
        if (limit > 0 && emittedCount >= limit) { truncated = 1; break; }
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        char head[64];
        snprintf(head, sizeof(head), "{\"id\":%lld,\"taskId\":", sqlite3_column_int64(eventStmt, 0));
        appendRaw(buffer, bufferSize, &used, head);
        appendJsonString(buffer, bufferSize, &used, columnText(eventStmt, 1));
        const char *eventKeys[] = {"schema","eventType","status","assignedAgent","message","createdBy"};
        for (int index = 0; index < 6; index++) {
            appendRaw(buffer, bufferSize, &used, ",\"");
            appendRaw(buffer, bufferSize, &used, eventKeys[index]);
            appendRaw(buffer, bufferSize, &used, "\":");
            appendJsonString(buffer, bufferSize, &used, columnText(eventStmt, index + 2));
        }
        long long ms = sqlite3_column_int64(eventStmt, 8);
        if (ms > maxEmittedAtMs) maxEmittedAtMs = ms;
        char tail[64];
        snprintf(tail, sizeof(tail), ",\"createdAtUnixMs\":%lld}", ms);
        appendRaw(buffer, bufferSize, &used, tail);
        emittedCount++;
    }
    sqlite3_finalize(eventStmt);

    /*
     * Policy decisions + usage events are not projected into SQLite (they live
     * only in the JSONL). Scan the ledger once and emit any records whose
     * taskId matches a task in this initiative. We pull the task-id set into a
     * small in-memory list first so the scan stays linear.
     */
    appendRaw(buffer, bufferSize, &used, "],\"policyDecisions\":[");
    /* Build a quick membership set of taskIds for this initiative. 1024 is a
     * generous cap matching the existing 200-task UI limit. */
    char taskIds[1024][WT_TASK_ID_SIZE];
    int taskIdCount = 0;
    sqlite3_stmt *idStmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT task_id FROM tasks WHERE initiative_id=?", -1, &idStmt, NULL) == SQLITE_OK) {
        bindText(idStmt, 1, initiativeId);
        while (sqlite3_step(idStmt) == SQLITE_ROW && taskIdCount < 1024) {
            snprintf(taskIds[taskIdCount], sizeof(taskIds[taskIdCount]), "%s", columnText(idStmt, 0));
            taskIdCount++;
        }
        sqlite3_finalize(idStmt);
    }
    sqlite3_close(db);

    FILE *file = fopen(ledgerPath, "r");
    int policyFirst = 1;
    int usageFirst = 1;
    /* Two-buffer approach: hold usage emissions until we close the policy array. */
    char usageBuffer[WT_TASK_LEDGER_LINE_SIZE * 4];
    size_t usageUsed = 0;
    usageBuffer[0] = '\0';
    if (file) {
        char line[WT_TASK_LEDGER_LINE_SIZE];
        while (fgets(line, sizeof(line), file)) {
            if (limit > 0 && emittedCount >= limit) { truncated = 1; break; }
            char schema[128];
            if (wtJsonReadString(line, "schema", schema, sizeof(schema)) != 0) continue;
            int isPolicy = strcmp(schema, "woventeam.policy_decision.v0.1") == 0;
            int isUsage = strcmp(schema, "woventeam.task_usage.v0.1") == 0;
            if (!isPolicy && !isUsage) continue;
            char taskId[WT_TASK_ID_SIZE];
            if (wtJsonReadString(line, "taskId", taskId, sizeof(taskId)) != 0) continue;
            int member = 0;
            for (int index = 0; index < taskIdCount; index++) {
                if (strcmp(taskIds[index], taskId) == 0) { member = 1; break; }
            }
            /* Policy denials carry their own initiativeId so audits can find
             * decisions for tasks that were rejected before they were ever
             * projected. */
            if (!member && isPolicy) {
                char policyInitiative[WT_TASK_ID_SIZE];
                if (wtJsonReadString(line, "initiativeId", policyInitiative,
                                     sizeof(policyInitiative)) == 0 &&
                    strcmp(policyInitiative, initiativeId) == 0) {
                    member = 1;
                }
            }
            if (!member) continue;
            /* since-filter on createdAtUnixMs */
            long long createdAtMs = 0;
            wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
            if (sinceUnixMs > 0 && createdAtMs < sinceUnixMs) continue;
            if (createdAtMs > maxEmittedAtMs) maxEmittedAtMs = createdAtMs;
            /* Strip the trailing newline so the embedded JSON is valid. */
            size_t length = strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
                line[--length] = '\0';
            }
            if (isPolicy) {
                if (!policyFirst) appendRaw(buffer, bufferSize, &used, ",");
                policyFirst = 0;
                appendRaw(buffer, bufferSize, &used, line);
            } else {
                if (!usageFirst) {
                    if (usageUsed + 2 < sizeof(usageBuffer)) {
                        usageBuffer[usageUsed++] = ',';
                        usageBuffer[usageUsed] = '\0';
                    }
                }
                usageFirst = 0;
                size_t copyLength = length;
                if (usageUsed + copyLength + 1 < sizeof(usageBuffer)) {
                    memcpy(usageBuffer + usageUsed, line, copyLength);
                    usageUsed += copyLength;
                    usageBuffer[usageUsed] = '\0';
                }
            }
            emittedCount++;
        }
        fclose(file);
    }
    appendRaw(buffer, bufferSize, &used, "],\"usage\":[");
    if (usageUsed > 0) {
        appendRaw(buffer, bufferSize, &used, usageBuffer);
    }
    /*
     * Phase 3 Sprint 3: emit deliverables[] and secretScans[] from the
     * projection tables. These are already filtered by initiative_id (via
     * the projected column) so the loop is cheap. since/limit pagination
     * also applies here.
     */
    appendRaw(buffer, bufferSize, &used, "],\"deliverables\":[");
    {
        sqlite3 *db2 = NULL;
        if (sqlite3_open(dbPath, &db2) == SQLITE_OK) {
            sqlite3_stmt *stmt = NULL;
            const char *sql =
                "SELECT raw_json,created_at_ms FROM deliverables "
                "WHERE initiative_id=? "
                "AND (? = 0 OR created_at_ms >= ?) "
                "ORDER BY id ASC";
            if (sqlite3_prepare_v2(db2, sql, -1, &stmt, NULL) == SQLITE_OK) {
                bindText(stmt, 1, initiativeId);
                sqlite3_bind_int64(stmt, 2, sinceUnixMs);
                sqlite3_bind_int64(stmt, 3, sinceUnixMs);
                int first = 1;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (limit > 0 && emittedCount >= limit) { truncated = 1; break; }
                    const char *raw = (const char *)sqlite3_column_text(stmt, 0);
                    long long ms = sqlite3_column_int64(stmt, 1);
                    if (!raw) continue;
                    if (!first) appendRaw(buffer, bufferSize, &used, ",");
                    first = 0;
                    appendRaw(buffer, bufferSize, &used, raw);
                    if (ms > maxEmittedAtMs) maxEmittedAtMs = ms;
                    emittedCount++;
                }
                sqlite3_finalize(stmt);
            }
            appendRaw(buffer, bufferSize, &used, "],\"secretScans\":[");
            stmt = NULL;
            /* Secret scans are keyed by deliverable_id; we filter by the
             * deliverable_id set we just emitted by joining via the
             * deliverables table inline. */
            const char *scanSql =
                "SELECT ss.raw_json, ss.created_at_ms "
                "FROM secret_scans ss "
                "JOIN deliverables d ON d.deliverable_id = ss.deliverable_id "
                "WHERE d.initiative_id = ? "
                "AND (? = 0 OR ss.created_at_ms >= ?) "
                "ORDER BY ss.id ASC";
            if (sqlite3_prepare_v2(db2, scanSql, -1, &stmt, NULL) == SQLITE_OK) {
                bindText(stmt, 1, initiativeId);
                sqlite3_bind_int64(stmt, 2, sinceUnixMs);
                sqlite3_bind_int64(stmt, 3, sinceUnixMs);
                int first = 1;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (limit > 0 && emittedCount >= limit) { truncated = 1; break; }
                    const char *raw = (const char *)sqlite3_column_text(stmt, 0);
                    long long ms = sqlite3_column_int64(stmt, 1);
                    if (!raw) continue;
                    if (!first) appendRaw(buffer, bufferSize, &used, ",");
                    first = 0;
                    appendRaw(buffer, bufferSize, &used, raw);
                    if (ms > maxEmittedAtMs) maxEmittedAtMs = ms;
                    emittedCount++;
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db2);
        }
    }
    /*
     * Tail: emit pagination metadata so the caller knows whether more pages
     * follow. nextSinceUnixMs is the highest createdAtUnixMs we emitted plus 1
     * (so the next page does not re-emit the boundary record); 0 when the
     * response was not truncated.
     */
    long long nextSince = truncated ? (maxEmittedAtMs + 1) : 0;
    char tail[160];
    snprintf(tail, sizeof(tail),
             "],\"truncated\":%s,\"nextSinceUnixMs\":%lld,\"emittedCount\":%d}\n",
             truncated ? "true" : "false", nextSince, emittedCount);
    appendRaw(buffer, bufferSize, &used, tail);
    return 0;
}

/*
 * Phase 3 Sprint 1 helpers for /api/status. Each one writes a JSON array
 * (NOT a complete document) into the supplied buffer.
 */
int wtTaskProjectionReadHeartbeatsJson(const char *dbPath, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT agent,host,current_task_id,lease_expires_at_ms,status_line,last_seen_ms "
        "FROM heartbeats ORDER BY agent";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{\"agent\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 0));
        appendRaw(buffer, bufferSize, &used, ",\"host\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 1));
        appendRaw(buffer, bufferSize, &used, ",\"currentTaskId\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 2));
        char number[128];
        snprintf(number, sizeof(number), ",\"leaseExpiresAtUnixMs\":%lld,\"statusLine\":",
                 sqlite3_column_int64(stmt, 3));
        appendRaw(buffer, bufferSize, &used, number);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 4));
        snprintf(number, sizeof(number), ",\"lastSeenUnixMs\":%lld}",
                 sqlite3_column_int64(stmt, 5));
        appendRaw(buffer, bufferSize, &used, number);
    }
    appendRaw(buffer, bufferSize, &used, "]");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

int wtTaskProjectionReadRecentMilestonesJson(const char *dbPath, int limit, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int effectiveLimit = limit > 0 ? limit : 50;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT id,task_id,milestone,message,created_by,created_at_ms "
        "FROM milestones ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, effectiveLimit);
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        char head[64];
        snprintf(head, sizeof(head), "{\"id\":%lld,\"taskId\":", sqlite3_column_int64(stmt, 0));
        appendRaw(buffer, bufferSize, &used, head);
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 1));
        appendRaw(buffer, bufferSize, &used, ",\"milestone\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 2));
        appendRaw(buffer, bufferSize, &used, ",\"message\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 3));
        appendRaw(buffer, bufferSize, &used, ",\"createdBy\":");
        appendJsonString(buffer, bufferSize, &used, columnText(stmt, 4));
        char tail[64];
        snprintf(tail, sizeof(tail), ",\"createdAtUnixMs\":%lld}",
                 sqlite3_column_int64(stmt, 5));
        appendRaw(buffer, bufferSize, &used, tail);
    }
    appendRaw(buffer, bufferSize, &used, "]");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

int wtTaskProjectionReadAgentsJson(const char *dbPath, long long nowUnixMs, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"agents\":[");
    const char *agents[] = {"claude", "chatgpt", "gemini"};
    for (size_t index = 0; index < sizeof(agents) / sizeof(agents[0]); index++) {
        if (index > 0) appendRaw(buffer, bufferSize, &used, ",");
        appendAgentWorkload(db, buffer, bufferSize, &used, agents[index], nowUnixMs);
    }
    appendRaw(buffer, bufferSize, &used, "]}\n");
    sqlite3_close(db);
    return 0;
}

static int countActiveWhere(const char *dbPath, const char *column, const char *value) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM tasks WHERE %s=? "
             "AND status NOT IN ('complete','failed','cancelled','closed')",
             column);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(stmt, 1, value);
    int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

int wtTaskProjectionCountActiveForAgent(const char *dbPath, const char *agent) {
    return countActiveWhere(dbPath, "assigned_agent", agent);
}

int wtTaskProjectionCountActiveForParent(const char *dbPath, const char *parentTaskId) {
    return countActiveWhere(dbPath, "parent_task_id", parentTaskId);
}

int wtTaskProjectionCountActiveForInitiative(const char *dbPath, const char *initiativeId) {
    return countActiveWhere(dbPath, "initiative_id", initiativeId);
}

int wtTaskProjectionResolveAcceptedArtifact(const char *dbPath, const char *taskId,
                                            char *initiativeId, size_t initiativeIdSize,
                                            char *artifactPath, size_t artifactPathSize) {
    if (!dbPath || !taskId || !initiativeId || !artifactPath) return -1;
    if (initiativeIdSize > 0) initiativeId[0] = '\0';
    if (artifactPathSize > 0) artifactPath[0] = '\0';
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT initiative_id, artifact_state, accepted_artifact_path "
        "FROM tasks WHERE task_id = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(stmt, 1, taskId);
    int rc = sqlite3_step(stmt);
    int outcome;
    if (rc != SQLITE_ROW) {
        outcome = -1;
    } else {
        const char *init = (const char *)sqlite3_column_text(stmt, 0);
        const char *state = (const char *)sqlite3_column_text(stmt, 1);
        const char *path = (const char *)sqlite3_column_text(stmt, 2);
        if (init && initiativeIdSize > 0) {
            snprintf(initiativeId, initiativeIdSize, "%s", init);
        }
        int accepted = state && strcmp(state, "accepted") == 0;
        int hasPath = path && path[0] != '\0';
        if (accepted && hasPath) {
            snprintf(artifactPath, artifactPathSize, "%s", path);
            outcome = 0;
        } else {
            outcome = 1;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return outcome;
}

/*
 * Sprint 5 policy budgets. Both helpers sum max_tokens over active (non-
 * terminal) task packages. The model-family query uses a prefix match against
 * "<family>/" so "openai" doesn't accidentally match "openai-experimental".
 */
long long wtTaskProjectionAllocatedTokensForInitiative(const char *dbPath, const char *initiativeId) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COALESCE(SUM(max_tokens), 0) FROM tasks WHERE initiative_id = ? "
        "AND status NOT IN ('complete','failed','cancelled','closed')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    bindText(stmt, 1, initiativeId);
    long long total = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return total;
}

long long wtTaskProjectionAllocatedTokensForModelFamily(const char *dbPath, const char *modelFamily) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COALESCE(SUM(max_tokens), 0) FROM tasks WHERE model_id LIKE ? "
        "AND status NOT IN ('complete','failed','cancelled','closed')";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "%s/%%", modelFamily ? modelFamily : "");
    bindText(stmt, 1, prefix);
    long long total = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return total;
}

int wtTaskProjectionReadCapacityJson(const char *dbPath, char *buffer, size_t bufferSize) {
    sqlite3 *db = NULL;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    char number[256];
    if (appendRaw(buffer, bufferSize, &used, "{\"ok\":true,\"agents\":[") != 0) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *agents = NULL;
    const char *agentSql =
        "SELECT assigned_agent, COUNT(*) FROM tasks "
        "WHERE assigned_agent != '' AND status NOT IN ('complete','failed','cancelled','closed') "
        "GROUP BY assigned_agent ORDER BY assigned_agent";
    if (sqlite3_prepare_v2(db, agentSql, -1, &agents, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int first = 1;
    while (sqlite3_step(agents) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{\"agent\":");
        appendJsonString(buffer, bufferSize, &used, columnText(agents, 0));
        snprintf(number, sizeof(number), ",\"activeTasks\":%d}", sqlite3_column_int(agents, 1));
        appendRaw(buffer, bufferSize, &used, number);
    }
    sqlite3_finalize(agents);
    appendRaw(buffer, bufferSize, &used, "],\"initiatives\":[");

    sqlite3_stmt *initiatives = NULL;
    const char *initiativeSql =
        "SELECT initiative_id, COUNT(*) FROM tasks "
        "WHERE initiative_id != '' AND status NOT IN ('complete','failed','cancelled','closed') "
        "GROUP BY initiative_id ORDER BY COUNT(*) DESC, initiative_id LIMIT 50";
    if (sqlite3_prepare_v2(db, initiativeSql, -1, &initiatives, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    first = 1;
    while (sqlite3_step(initiatives) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{\"initiativeId\":");
        appendJsonString(buffer, bufferSize, &used, columnText(initiatives, 0));
        snprintf(number, sizeof(number), ",\"activeTasks\":%d}", sqlite3_column_int(initiatives, 1));
        appendRaw(buffer, bufferSize, &used, number);
    }
    sqlite3_finalize(initiatives);
    appendRaw(buffer, bufferSize, &used, "],\"parents\":[");

    sqlite3_stmt *parents = NULL;
    const char *parentSql =
        "SELECT parent_task_id, COUNT(*) FROM tasks "
        "WHERE parent_task_id != '' AND status NOT IN ('complete','failed','cancelled','closed') "
        "GROUP BY parent_task_id ORDER BY COUNT(*) DESC, parent_task_id LIMIT 50";
    if (sqlite3_prepare_v2(db, parentSql, -1, &parents, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    first = 1;
    while (sqlite3_step(parents) == SQLITE_ROW) {
        if (!first) appendRaw(buffer, bufferSize, &used, ",");
        first = 0;
        appendRaw(buffer, bufferSize, &used, "{\"parentTaskId\":");
        appendJsonString(buffer, bufferSize, &used, columnText(parents, 0));
        snprintf(number, sizeof(number), ",\"activeTasks\":%d}", sqlite3_column_int(parents, 1));
        appendRaw(buffer, bufferSize, &used, number);
    }
    sqlite3_finalize(parents);
    appendRaw(buffer, bufferSize, &used, "]}");
    sqlite3_close(db);
    return 0;
}
