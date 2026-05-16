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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static int isCodexEligibleTask(const WtConfig *config, const char *agentName, const WtTaskSummary *task) {
    if (!config->enableCodexAdapter || strcmp(agentName, "chatgpt") != 0) {
        return 0;
    }
    if (!(strcmp(task->toolProfile, "repo_branch") == 0 || strcmp(task->toolProfile, "test_local") == 0)) {
        return 0;
    }
    if (task->modelId[0] != '\0' && strncmp(task->modelId, "openai/", 7) != 0) {
        return 0;
    }
    return 1;
}

static int writeTextFile(const char *path, const char *content) {
    if (wtRoomEnsureParentDirs(path) != 0) {
        return -1;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        return -1;
    }
    fputs(content ? content : "", file);
    fclose(file);
    return 0;
}

static void readFileSnippet(const char *path, int maxBytes, char *buffer, size_t bufferSize) {
    buffer[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file || bufferSize == 0) {
        if (file) fclose(file);
        return;
    }
    int limit = maxBytes > 0 && maxBytes < (int)bufferSize - 1 ? maxBytes : (int)bufferSize - 1;
    size_t bytes = fread(buffer, 1, (size_t)limit, file);
    buffer[bytes] = '\0';
    fclose(file);
}

static void capFileSize(const char *path, int maxBytes) {
    if (maxBytes > 0) {
        int rc = truncate(path, (off_t)maxBytes);
        (void)rc;
    }
}

static int runCodexAdapter(const WtConfig *config, const char *agentName, const WtTaskSummary *task) {
    char workspace[512];
    char promptPath[768];
    char stdoutPath[768];
    char stderrPath[768];
    char resultPath[768];
    char manifestPath[768];
    snprintf(workspace, sizeof(workspace), "%s/%s", config->runtimeRootPath, task->taskId);
    snprintf(promptPath, sizeof(promptPath), "%s/prompt.md", workspace);
    snprintf(stdoutPath, sizeof(stdoutPath), "%s/stdout.log", workspace);
    snprintf(stderrPath, sizeof(stderrPath), "%s/stderr.log", workspace);
    snprintf(resultPath, sizeof(resultPath), "%s/result.md", workspace);
    snprintf(manifestPath, sizeof(manifestPath), "%s/manifest.json", workspace);

    char keepPath[768];
    snprintf(keepPath, sizeof(keepPath), "%s/.keep", workspace);
    if (wtRoomEnsureParentDirs(keepPath) != 0 || mkdir(workspace, 0755) != 0) {
        if (errno != EEXIST) {
            perror("create adapter workspace");
            return 1;
        }
    }

    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
             "You are a WovenTeam Codex adapter worker running in an isolated per-task workspace.\n\n"
             "Task ID: %s\nRole: %s\nTool policy: %s\n\n"
             "Task title: %s\n\nTask body:\n%s\n\n"
             "Write your final result as a concise implementation/report note. "
             "Do not modify the WovenTeam repository from this adapter workspace.",
             task->taskId, task->assignedRole, task->toolProfile, task->title, task->body);
    if (writeTextFile(promptPath, prompt) != 0) {
        perror("write adapter prompt");
        return 1;
    }

    int timeoutSeconds = task->timeoutSeconds > 0 ? task->timeoutSeconds : config->adapterTimeoutSeconds;
    if (timeoutSeconds <= 0) timeoutSeconds = 1800;
    int maxOutputBytes = task->maxOutputBytes > 0 ? task->maxOutputBytes : config->adapterMaxOutputBytes;
    if (maxOutputBytes <= 0) maxOutputBytes = 1048576;

    char actor[128];
    snprintf(actor, sizeof(actor), "wt-agent@%s", agentName);
    char runningMessage[512];
    snprintf(runningMessage, sizeof(runningMessage), "Codex adapter started for taskId=%s", task->taskId);
    wtTaskAppendStatusEvent(config->taskLedgerPath, task->taskId, "running", actor, runningMessage, config->fsyncEachMessage);
    appendAgentTaskMessage(config, agentName, task, "task.status", "running", "codex adapter started");

    pid_t child = fork();
    if (child < 0) {
        perror("fork codex adapter");
        return 1;
    }
    if (child == 0) {
        int stdoutFd = open(stdoutPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        int stderrFd = open(stderrPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (stdoutFd >= 0) dup2(stdoutFd, STDOUT_FILENO);
        if (stderrFd >= 0) dup2(stderrFd, STDERR_FILENO);
        if (stdoutFd > 2) close(stdoutFd);
        if (stderrFd > 2) close(stderrFd);
        execlp(config->gptCommand, config->gptCommand,
               "exec",
               "--ephemeral",
               "--skip-git-repo-check",
               "--cd", workspace,
               "--sandbox", "workspace-write",
               "--ask-for-approval", "never",
               "--output-last-message", resultPath,
               prompt,
               (char *)NULL);
        perror("exec codex");
        _exit(127);
    }

    int status = 0;
    int timedOut = 0;
    time_t start = time(NULL);
    while (1) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            break;
        }
        if (done < 0) {
            perror("wait codex");
            return 1;
        }
        if ((int)(time(NULL) - start) >= timeoutSeconds) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            timedOut = 1;
            break;
        }
        sleep(1);
    }

    int exitCode = timedOut ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : 125);
    capFileSize(stdoutPath, maxOutputBytes);
    capFileSize(stderrPath, maxOutputBytes);
    capFileSize(resultPath, maxOutputBytes);
    char resultSnippet[1024];
    char stderrSnippet[1024];
    readFileSnippet(resultPath, maxOutputBytes < 1000 ? maxOutputBytes : 1000, resultSnippet, sizeof(resultSnippet));
    readFileSnippet(stderrPath, maxOutputBytes < 1000 ? maxOutputBytes : 1000, stderrSnippet, sizeof(stderrSnippet));

    char manifest[4096];
    snprintf(manifest, sizeof(manifest),
             "{\n"
             "  \"schema\": \"woventeam.adapter_manifest.v0.1\",\n"
             "  \"taskId\": \"%s\",\n"
             "  \"adapter\": \"codex\",\n"
             "  \"command\": \"%s\",\n"
             "  \"workspace\": \"%s\",\n"
             "  \"stdout\": \"%s\",\n"
             "  \"stderr\": \"%s\",\n"
             "  \"result\": \"%s\",\n"
             "  \"timedOut\": %s,\n"
             "  \"exitCode\": %d\n"
             "}\n",
             task->taskId, config->gptCommand, workspace, stdoutPath, stderrPath, resultPath,
             timedOut ? "true" : "false", exitCode);
    writeTextFile(manifestPath, manifest);

    char eventMessage[1024];
    snprintf(eventMessage, sizeof(eventMessage), "Codex adapter exitCode=%d timedOut=%s; see task workspace manifest.",
             exitCode, timedOut ? "true" : "false");
    const char *finalStatus = exitCode == 0 ? "complete" : "failed";
    wtTaskAppendStatusEvent(config->taskLedgerPath, task->taskId, finalStatus, actor, eventMessage, config->fsyncEachMessage);

    char detail[1024];
    snprintf(detail, sizeof(detail), "codex adapter exitCode=%d workspace=%s", exitCode, workspace);
    appendAgentTaskMessage(config, agentName, task, "task.result", finalStatus, detail);

    printf("[adapter:%s] %s task=%s exit=%d workspace=%s\n", "codex", finalStatus, task->taskId, exitCode, workspace);
    if (resultSnippet[0]) {
        printf("%s\n", resultSnippet);
    } else if (stderrSnippet[0]) {
        fprintf(stderr, "%s\n", stderrSnippet);
    }
    return exitCode == 0 ? 0 : 1;
}

static int handleAssignedTask(const WtConfig *config, const char *agentName) {
    WtTaskSummary task;
    int found = wtTaskFindQueuedForAgent(config->taskLedgerPath, agentName, &task);
    if (found <= 0) {
        return 0;
    }
    char actor[128];
    snprintf(actor, sizeof(actor), "wt-agent@%s", agentName);
    if (isCodexEligibleTask(config, agentName, &task)) {
        return runCodexAdapter(config, agentName, &task);
    }
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
