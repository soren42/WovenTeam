/*
 * wt_message.h - Durable room message representation.
 *
 * Each message maps to one JSON Lines record in data/phase0-room.jsonl. Fixed
 * buffers make Phase 0 memory behavior predictable on the target SBC.
 */
#ifndef WT_MESSAGE_H
#define WT_MESSAGE_H

#include <stddef.h>

#define WT_MESSAGE_BODY_SIZE 4096
#define WT_MESSAGE_JSON_SIZE 8192
#define WT_MESSAGE_NAME_SIZE 64
#define WT_MESSAGE_TYPE_SIZE 32

typedef struct {
    long messageId;
    char roomName[WT_MESSAGE_NAME_SIZE];
    char senderName[WT_MESSAGE_NAME_SIZE];
    char targetName[WT_MESSAGE_NAME_SIZE];
    char messageType[WT_MESSAGE_TYPE_SIZE];
    char messageBody[WT_MESSAGE_BODY_SIZE];
    long long createdAtUnixMs;
    long replyToMessageId;
} WtMessage;

void wtMessageInit(WtMessage *message);
int wtMessageValidateNames(const WtMessage *message);
int wtMessageToJson(const WtMessage *message, char *buffer, size_t bufferSize);
int wtMessageFromJsonLine(const char *line, WtMessage *message);
void wtMessagePrintHuman(const WtMessage *message);

#endif
