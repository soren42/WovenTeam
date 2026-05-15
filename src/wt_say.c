/*
 * wt_say.c - Append a CEO/system/agent message to the shared Phase 0 room.
 */
#include "wt_config.h"
#include "wt_message.h"
#include "wt_room_store.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *programName) {
    fprintf(stderr,
            "Usage: %s [--config FILE] SENDER TARGET MESSAGE\n"
            "Example: %s ceo all \"Hello team.\"\n",
            programName, programName);
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    int argIndex = 1;
    if (argc > 2 && strcmp(argv[argIndex], "--config") == 0) {
        wtConfigLoadFile(&config, argv[argIndex + 1]);
        argIndex += 2;
    }
    wtConfigApplyEnvironment(&config);
    if (argc - argIndex != 3) {
        usage(argv[0]);
        return 2;
    }

    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config.roomName);
    snprintf(message.senderName, sizeof(message.senderName), "%s", argv[argIndex]);
    snprintf(message.targetName, sizeof(message.targetName), "%s", argv[argIndex + 1]);
    snprintf(message.messageType, sizeof(message.messageType), "%s", "chat");
    snprintf(message.messageBody, sizeof(message.messageBody), "%s", argv[argIndex + 2]);
    if (wtMessageValidateNames(&message) != 0) {
        fprintf(stderr, "Invalid sender, target, or message type.\n");
        return 2;
    }
    if (wtRoomAppendNewMessage(config.roomLogPath, &message, config.fsyncEachMessage) != 0) {
        perror("append room message");
        return 1;
    }
    wtMessagePrintHuman(&message);
    return 0;
}
