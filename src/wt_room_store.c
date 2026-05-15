/*
 * wt_room_store.c - Append-only JSONL storage for the native room.
 *
 * This store favors boring reliability over database features. Phase 0 scans
 * the log when it needs recent messages; an index can be added later if the
 * log grows beyond what the SBC can comfortably scan.
 */
#include "wt_room_store.h"

#include "wt_time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int wtRoomEnsureParentDirs(const char *path) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    if (copy[0] == '\0') {
        return 0;
    }
    char partial[512] = "";
    char *cursor = copy;
    if (*cursor == '/') {
        snprintf(partial, sizeof(partial), "/");
        cursor++;
    }
    char *token = strtok(cursor, "/");
    while (token) {
        if (strlen(partial) > 1) {
            strncat(partial, "/", sizeof(partial) - strlen(partial) - 1);
        }
        strncat(partial, token, sizeof(partial) - strlen(partial) - 1);
        if (mkdir(partial, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        token = strtok(NULL, "/");
    }
    return 0;
}

long wtRoomReadLastMessageId(const char *roomLogPath) {
    FILE *file = fopen(roomLogPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_MESSAGE_JSON_SIZE];
    long lastId = 0;
    while (fgets(line, sizeof(line), file)) {
        WtMessage message;
        if (wtMessageFromJsonLine(line, &message) == 0 && message.messageId > lastId) {
            lastId = message.messageId;
        }
    }
    fclose(file);
    return lastId;
}

int wtRoomAppendMessage(const char *roomLogPath, const WtMessage *message, bool fsyncEachMessage) {
    if (wtRoomEnsureParentDirs(roomLogPath) != 0) {
        return -1;
    }
    int fd = open(roomLogPath, O_APPEND | O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }
    char json[WT_MESSAGE_JSON_SIZE];
    if (wtMessageToJson(message, json, sizeof(json)) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    size_t length = strlen(json);
    int ok = write(fd, json, length) == (ssize_t)length &&
             write(fd, "\n", 1) == 1;
    if (ok && fsyncEachMessage) {
        ok = fsync(fd) == 0;
    }
    flock(fd, LOCK_UN);
    close(fd);
    return ok ? 0 : -1;
}

int wtRoomAppendNewMessage(const char *roomLogPath, WtMessage *message, bool fsyncEachMessage) {
    if (wtRoomEnsureParentDirs(roomLogPath) != 0) {
        return -1;
    }
    int fd = open(roomLogPath, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return -1;
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return -1;
    }

    long lastId = 0;
    FILE *reader = fdopen(dup(fd), "r");
    if (!reader) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    char line[WT_MESSAGE_JSON_SIZE];
    while (fgets(line, sizeof(line), reader)) {
        WtMessage existingMessage;
        if (wtMessageFromJsonLine(line, &existingMessage) == 0 && existingMessage.messageId > lastId) {
            lastId = existingMessage.messageId;
        }
    }
    fclose(reader);

    message->messageId = lastId + 1;
    if (message->createdAtUnixMs == 0) {
        message->createdAtUnixMs = wtNowUnixMilliseconds();
    }

    char json[WT_MESSAGE_JSON_SIZE];
    if (wtMessageToJson(message, json, sizeof(json)) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_END) < 0) {
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }
    size_t length = strlen(json);
    int ok = write(fd, json, length) == (ssize_t)length &&
             write(fd, "\n", 1) == 1;
    if (ok && fsyncEachMessage) {
        ok = fsync(fd) == 0;
    }
    flock(fd, LOCK_UN);
    close(fd);
    return ok ? 0 : -1;
}

int wtRoomReadRecent(const char *roomLogPath, int limit, WtMessage *messages, int maxMessages) {
    if (limit <= 0 || limit > maxMessages) {
        limit = maxMessages;
    }
    FILE *file = fopen(roomLogPath, "r");
    if (!file) {
        return 0;
    }
    WtMessage *ring = calloc((size_t)limit, sizeof(WtMessage));
    if (!ring) {
        fclose(file);
        return -1;
    }
    char line[WT_MESSAGE_JSON_SIZE];
    int seen = 0;
    while (fgets(line, sizeof(line), file)) {
        WtMessage message;
        if (wtMessageFromJsonLine(line, &message) == 0) {
            ring[seen % limit] = message;
            seen++;
        }
    }
    int count = seen < limit ? seen : limit;
    int start = seen < limit ? 0 : seen % limit;
    for (int index = 0; index < count; index++) {
        messages[index] = ring[(start + index) % limit];
    }
    free(ring);
    fclose(file);
    return count;
}

int wtRoomReadMessagesAfter(const char *roomLogPath, long afterMessageId, WtMessage *messages, int maxMessages) {
    FILE *file = fopen(roomLogPath, "r");
    if (!file) {
        return 0;
    }
    char line[WT_MESSAGE_JSON_SIZE];
    int count = 0;
    while (count < maxMessages && fgets(line, sizeof(line), file)) {
        WtMessage message;
        if (wtMessageFromJsonLine(line, &message) == 0 && message.messageId > afterMessageId) {
            messages[count++] = message;
        }
    }
    fclose(file);
    return count;
}

static void agentStatePath(const char *roomLogPath, const char *agentName, char *buffer, size_t bufferSize) {
    char directory[256];
    snprintf(directory, sizeof(directory), "%s", roomLogPath);
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        snprintf(buffer, bufferSize, "%s/%s.state", directory, agentName);
    } else {
        snprintf(buffer, bufferSize, "%s.state", agentName);
    }
}

int wtRoomReadAgentState(const char *roomLogPath, const char *agentName, long *messageId) {
    char path[512];
    agentStatePath(roomLogPath, agentName, path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) {
        *messageId = 0;
        return 0;
    }
    if (fscanf(file, "%ld", messageId) != 1) {
        *messageId = 0;
    }
    fclose(file);
    return 0;
}

int wtRoomWriteAgentState(const char *roomLogPath, const char *agentName, long messageId) {
    char path[512];
    agentStatePath(roomLogPath, agentName, path, sizeof(path));
    if (wtRoomEnsureParentDirs(path) != 0) {
        return -1;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        return -1;
    }
    fprintf(file, "%ld\n", messageId);
    fclose(file);
    return 0;
}
