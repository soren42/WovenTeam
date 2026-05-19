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
        "model_id,priority,status,title,body,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0) "
        "ON CONFLICT(task_id) DO UPDATE SET "
        "initiative_id=excluded.initiative_id,parent_task_id=excluded.parent_task_id,"
        "requested_by_role=excluded.requested_by_role,assigned_role=excluded.assigned_role,"
        "assigned_agent=excluded.assigned_agent,model_id=excluded.model_id,priority=excluded.priority,"
        "status=excluded.status,title=excluded.title,body=excluded.body,tool_profile=excluded.tool_profile,"
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
    sqlite3_bind_int64(stmt, 13, maxTokens);
    sqlite3_bind_int64(stmt, 14, createdAtMs);
    sqlite3_bind_int64(stmt, 15, createdAtMs);
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
    long long createdAtMs = 0;
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
    wtJsonReadLongLong(line, "createdAtUnixMs", &createdAtMs);
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
    if (status[0] != '\0' || assignedAgent[0] != '\0') {
        sqlite3_stmt *update = NULL;
        const char *updateSql =
            "UPDATE tasks SET "
            "status=CASE WHEN ? != '' THEN ? ELSE status END,"
            "assigned_agent=CASE WHEN ? != '' THEN ? ELSE assigned_agent END,"
            "updated_at_ms=max(updated_at_ms,?),"
            "event_count=event_count+1 "
            "WHERE task_id=?";
        if (sqlite3_prepare_v2(db, updateSql, -1, &update, NULL) != SQLITE_OK) {
            return -1;
        }
        bindText(update, 1, status);
        bindText(update, 2, status);
        bindText(update, 3, assignedAgent);
        bindText(update, 4, assignedAgent);
        sqlite3_bind_int64(update, 5, createdAtMs);
        bindText(update, 6, taskId);
        rc = sqlite3_step(update) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(update);
    }
    return rc;
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
        execSql(db, "DROP TABLE IF EXISTS tasks; DROP TABLE IF EXISTS task_events;") != 0 ||
        execSql(db,
            "CREATE TABLE tasks ("
            "task_id TEXT PRIMARY KEY,"
            "initiative_id TEXT,parent_task_id TEXT,requested_by_role TEXT,"
            "assigned_role TEXT,assigned_agent TEXT,model_id TEXT,priority TEXT,status TEXT,"
            "title TEXT,body TEXT,tool_profile TEXT,max_tokens INTEGER DEFAULT 0,"
            "created_at_ms INTEGER DEFAULT 0,updated_at_ms INTEGER DEFAULT 0,event_count INTEGER DEFAULT 0);"
            "CREATE INDEX idx_tasks_status ON tasks(status);"
            "CREATE INDEX idx_tasks_agent ON tasks(assigned_agent);"
            "CREATE INDEX idx_tasks_initiative ON tasks(initiative_id);"
            "CREATE TABLE task_events ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,task_id TEXT,schema TEXT,event_type TEXT,status TEXT,"
            "assigned_agent TEXT,message TEXT,created_by TEXT,created_at_ms INTEGER DEFAULT 0,raw_json TEXT);"
            "CREATE INDEX idx_task_events_task ON task_events(task_id,id);") != 0) {
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
        "model_id,priority,status,title,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count "
        "FROM tasks ORDER BY updated_at_ms DESC, created_at_ms DESC LIMIT 200";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    size_t used = 0;
    appendRaw(buffer, bufferSize, &used, "[");
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char number[128];
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
                 ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"eventCount\":%d}",
                 sqlite3_column_int64(stmt, 11),
                 sqlite3_column_int64(stmt, 12),
                 sqlite3_column_int64(stmt, 13),
                 sqlite3_column_int(stmt, 14));
        appendRaw(buffer, bufferSize, &used, number);
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
        "model_id,priority,status,title,body,tool_profile,max_tokens,created_at_ms,updated_at_ms,event_count "
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
    snprintf(number, sizeof(number),
             ",\"maxTokens\":%lld,\"createdAtUnixMs\":%lld,\"updatedAtUnixMs\":%lld,\"eventCount\":%d},\"events\":[",
             sqlite3_column_int64(task, 12),
             sqlite3_column_int64(task, 13),
             sqlite3_column_int64(task, 14),
             sqlite3_column_int(task, 15));
    appendRaw(buffer, bufferSize, &used, number);
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
