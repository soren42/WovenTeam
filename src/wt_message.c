/*
 * wt_message.c - JSONL serialization for shared Phase 0 room messages.
 */
#include "wt_message.h"

#include "wt_json.h"
#include "wt_time.h"

#include <stdio.h>
#include <string.h>

static int isAllowedName(const char *value, const char *const *allowed) {
    for (int index = 0; allowed[index]; index++) {
        if (strcmp(value, allowed[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

void wtMessageInit(WtMessage *message) {
    memset(message, 0, sizeof(*message));
    snprintf(message->roomName, sizeof(message->roomName), "%s", "phase0");
    snprintf(message->senderName, sizeof(message->senderName), "%s", "system");
    snprintf(message->targetName, sizeof(message->targetName), "%s", "all");
    snprintf(message->messageType, sizeof(message->messageType), "%s", "chat");
}

int wtMessageValidateNames(const WtMessage *message) {
    static const char *const senders[] = {"ceo", "system", "claude", "chatgpt", "gemini", NULL};
    static const char *const targets[] = {"all", "ceo", "claude", "chatgpt", "gemini", NULL};
    static const char *const types[] = {
        "chat", "directive", "status", "error",
        "task.assign", "task.status", "task.result",
        NULL
    };
    return isAllowedName(message->senderName, senders) &&
           isAllowedName(message->targetName, targets) &&
           isAllowedName(message->messageType, types) ? 0 : -1;
}

int wtMessageToJson(const WtMessage *message, char *buffer, size_t bufferSize) {
    char roomName[WT_MESSAGE_NAME_SIZE * 2];
    char senderName[WT_MESSAGE_NAME_SIZE * 2];
    char targetName[WT_MESSAGE_NAME_SIZE * 2];
    char messageType[WT_MESSAGE_TYPE_SIZE * 2];
    char messageBody[WT_MESSAGE_BODY_SIZE * 2];
    if (wtJsonEscape(message->roomName, roomName, sizeof(roomName)) != 0 ||
        wtJsonEscape(message->senderName, senderName, sizeof(senderName)) != 0 ||
        wtJsonEscape(message->targetName, targetName, sizeof(targetName)) != 0 ||
        wtJsonEscape(message->messageType, messageType, sizeof(messageType)) != 0 ||
        wtJsonEscape(message->messageBody, messageBody, sizeof(messageBody)) != 0) {
        return -1;
    }
    int written = snprintf(buffer, bufferSize,
        "{\"messageId\":%ld,\"roomName\":\"%s\",\"senderName\":\"%s\","
        "\"targetName\":\"%s\",\"messageType\":\"%s\",\"messageBody\":\"%s\","
        "\"createdAtUnixMs\":%lld,\"replyToMessageId\":%ld}",
        message->messageId, roomName, senderName, targetName, messageType,
        messageBody, message->createdAtUnixMs, message->replyToMessageId);
    return written > 0 && (size_t)written < bufferSize ? 0 : -1;
}

int wtMessageFromJsonLine(const char *line, WtMessage *message) {
    wtMessageInit(message);
    if (wtJsonReadLong(line, "messageId", &message->messageId) != 0 ||
        wtJsonReadString(line, "roomName", message->roomName, sizeof(message->roomName)) != 0 ||
        wtJsonReadString(line, "senderName", message->senderName, sizeof(message->senderName)) != 0 ||
        wtJsonReadString(line, "targetName", message->targetName, sizeof(message->targetName)) != 0 ||
        wtJsonReadString(line, "messageType", message->messageType, sizeof(message->messageType)) != 0 ||
        wtJsonReadString(line, "messageBody", message->messageBody, sizeof(message->messageBody)) != 0 ||
        wtJsonReadLongLong(line, "createdAtUnixMs", &message->createdAtUnixMs) != 0 ||
        wtJsonReadLong(line, "replyToMessageId", &message->replyToMessageId) != 0) {
        return -1;
    }
    return 0;
}

void wtMessagePrintHuman(const WtMessage *message) {
    char timestamp[64];
    wtFormatUnixMilliseconds(message->createdAtUnixMs, timestamp, sizeof(timestamp));
    printf("[%ld] %s %s -> %s: %s\n", message->messageId, timestamp,
           message->senderName, message->targetName, message->messageBody);
}
