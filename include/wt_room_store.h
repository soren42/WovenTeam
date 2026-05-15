/*
 * wt_room_store.h - Append-only JSONL room store.
 *
 * The room log is the Phase 0 system of record. Appends use O_APPEND and an
 * advisory lock so separate tools can safely write one full message per line.
 */
#ifndef WT_ROOM_STORE_H
#define WT_ROOM_STORE_H

#include <stdbool.h>
#include "wt_message.h"

int wtRoomEnsureParentDirs(const char *path);
long wtRoomReadLastMessageId(const char *roomLogPath);
int wtRoomAppendMessage(const char *roomLogPath, const WtMessage *message, bool fsyncEachMessage);
int wtRoomAppendNewMessage(const char *roomLogPath, WtMessage *message, bool fsyncEachMessage);
int wtRoomReadRecent(const char *roomLogPath, int limit, WtMessage *messages, int maxMessages);
int wtRoomReadMessagesAfter(const char *roomLogPath, long afterMessageId, WtMessage *messages, int maxMessages);
int wtRoomReadAgentState(const char *roomLogPath, const char *agentName, long *messageId);
int wtRoomWriteAgentState(const char *roomLogPath, const char *agentName, long messageId);

#endif
