/*
 * wt_agent.c - Stub-mode Phase 0 room participant.
 *
 * Agents reconstruct context from the shared JSONL room log, respond only to
 * CEO/system messages addressed to them or all, and record the last handled
 * message ID in a tiny state file to avoid duplicate replies and loops.
 */
#include "wt_config.h"
#include "wt_message.h"
#include "wt_room_store.h"
#include "wt_task_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *programName) {
    fprintf(stderr, "Usage: %s --agent claude|chatgpt|gemini [--once|--loop] [--config FILE]\n", programName);
}

static void sleepMilliseconds(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0) {
    }
}

static int isKnownAgent(const char *agentName) {
    return strcmp(agentName, "claude") == 0 ||
           strcmp(agentName, "chatgpt") == 0 ||
           strcmp(agentName, "gemini") == 0;
}

static int shouldHandle(const char *agentName, const WtMessage *message, long lastHandledId) {
    if (message->messageId <= lastHandledId) {
        return 0;
    }
    if (!(strcmp(message->senderName, "ceo") == 0 || strcmp(message->senderName, "system") == 0)) {
        return 0;
    }
    if (!(strcmp(message->targetName, "all") == 0 || strcmp(message->targetName, agentName) == 0)) {
        return 0;
    }
    return 1;
}

static void buildStubReply(const char *agentName, const WtMessage *trigger, char *buffer, size_t bufferSize) {
    if (strcmp(agentName, "claude") == 0) {
        snprintf(buffer, bufferSize,
                 "[stub claude] I received messageId %ld and can respond from the shared room transcript.",
                 trigger->messageId);
    } else if (strcmp(agentName, "chatgpt") == 0) {
        snprintf(buffer, bufferSize,
                 "[stub chatgpt] I can see messageId %ld in the same durable room state and will treat it as authoritative.",
                 trigger->messageId);
    } else {
        snprintf(buffer, bufferSize,
                 "[stub gemini] I confirm the message bus is working from shared transcript messageId %ld.",
                 trigger->messageId);
    }
}

static int appendAgentTaskMessage(const WtConfig *config, const char *agentName, const WtTaskSummary *task,
                                  const char *messageType, const char *status, const char *detail) {
    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config->roomName);
    snprintf(message.senderName, sizeof(message.senderName), "%s", agentName);
    snprintf(message.targetName, sizeof(message.targetName), "%s", "ceo");
    snprintf(message.messageType, sizeof(message.messageType), "%s", messageType);
    snprintf(message.messageBody, sizeof(message.messageBody), "taskId=%s status=%s title=%s%s%s",
             task->taskId, status, task->title,
             detail && detail[0] ? " detail=" : "",
             detail && detail[0] ? detail : "");
    return wtRoomAppendNewMessage(config->roomLogPath, &message, config->fsyncEachMessage);
}

static int handleAssignedTask(const WtConfig *config, const char *agentName) {
    WtTaskSummary task;
    int found = wtTaskFindQueuedForAgent(config->taskLedgerPath, agentName, &task);
    if (found <= 0) {
        return 0;
    }
    char actor[128];
    snprintf(actor, sizeof(actor), "wt-agent@%s", agentName);
    if (wtTaskAppendStatusEvent(config->taskLedgerPath, task.taskId, "running", actor,
                                "Stub agent accepted assigned task.", config->fsyncEachMessage) != 0 ||
        appendAgentTaskMessage(config, agentName, &task, "task.status", "running",
                               "stub agent accepted task") != 0) {
        perror("record task running");
        return 1;
    }
    if (wtTaskAppendStatusEvent(config->taskLedgerPath, task.taskId, "complete", actor,
                                "Stub agent completed the assignment path without harness execution.", config->fsyncEachMessage) != 0 ||
        appendAgentTaskMessage(config, agentName, &task, "task.result", "complete",
                               "assignment path verified; harness execution coming in Sprint 3") != 0) {
        perror("record task complete");
        return 1;
    }
    printf("[task] %s completed %s (%s)\n", agentName, task.taskId, task.title);
    return 0;
}

static int handleOnce(const WtConfig *config, const char *agentName) {
    int taskRc = handleAssignedTask(config, agentName);
    if (taskRc != 0) {
        return taskRc;
    }

    WtMessage messages[256];
    int limit = config->contextMessageCount > 0 ? config->contextMessageCount : 20;
    if (limit > 256) {
        limit = 256;
    }
    int count = wtRoomReadRecent(config->roomLogPath, limit, messages, 256);
    if (count < 0) {
        perror("read room log");
        return 1;
    }
    long lastHandledId = 0;
    wtRoomReadAgentState(config->roomLogPath, agentName, &lastHandledId);

    for (int index = count - 1; index >= 0; index--) {
        if (!shouldHandle(agentName, &messages[index], lastHandledId)) {
            continue;
        }
        WtMessage reply;
        wtMessageInit(&reply);
        snprintf(reply.roomName, sizeof(reply.roomName), "%s", config->roomName);
        snprintf(reply.senderName, sizeof(reply.senderName), "%s", agentName);
        snprintf(reply.targetName, sizeof(reply.targetName), "%s", "ceo");
        snprintf(reply.messageType, sizeof(reply.messageType), "%s", "chat");
        reply.replyToMessageId = messages[index].messageId;
        buildStubReply(agentName, &messages[index], reply.messageBody, sizeof(reply.messageBody));
        if (wtRoomAppendNewMessage(config->roomLogPath, &reply, config->fsyncEachMessage) != 0) {
            perror("append agent reply");
            return 1;
        }
        if (wtRoomWriteAgentState(config->roomLogPath, agentName, messages[index].messageId) != 0) {
            perror("write agent state");
            return 1;
        }
        wtMessagePrintHuman(&reply);
        return 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    const char *agentName = NULL;
    int loop = 0;
    int once = 0;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--agent") == 0 && index + 1 < argc) {
            agentName = argv[++index];
        } else if (strcmp(argv[index], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[index], "--loop") == 0) {
            loop = 1;
        } else if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            wtConfigLoadFile(&config, argv[++index]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    wtConfigApplyEnvironment(&config);
    if (!agentName || !isKnownAgent(agentName) || (once && loop)) {
        usage(argv[0]);
        return 2;
    }
    if (!once && !loop) {
        once = 1;
    }
    do {
        int rc = handleOnce(&config, agentName);
        if (rc != 0 || once) {
            return rc;
        }
        sleepMilliseconds(config.agentPollMilliseconds);
    } while (1);
}
