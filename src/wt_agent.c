/*
 * wt_agent.c - Stub-mode Phase 0 room participant.
 *
 * Agents reconstruct context from the shared JSONL room log, respond only to
 * CEO/system messages addressed to them or all, and record the last handled
 * message ID in a tiny state file to avoid duplicate replies and loops.
 */
#include "wt_config.h"
#include "wt_json.h"
#include "wt_message.h"
#include "wt_room_store.h"
#include "wt_task_store.h"

#include <ctype.h>
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
    fprintf(stderr, "Usage: %s --agent claude|chatgpt|gemini [--once|--loop] [--config FILE] [--remote URL --bearer TOKEN --host HOST] [--ssh HOST]\n", programName);
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

static int shellSafeText(const char *value) {
    if (!value || !value[0]) return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '.' || *cursor == ':' || *cursor == '/' ||
              *cursor == '_' || *cursor == '-' || *cursor == '=')) {
            return 0;
        }
    }
    return 1;
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

static int isCliArtifactEligibleTask(const WtConfig *config, const char *agentName, const WtTaskSummary *task) {
    if (strcmp(agentName, "claude") == 0) {
        return config->enableClaudeAdapter &&
               strcmp(config->claudeMode, "adapter") == 0 &&
               (task->modelId[0] == '\0' || strncmp(task->modelId, "anthropic/", 10) == 0);
    }
    if (strcmp(agentName, "gemini") == 0) {
        return config->enableGeminiAdapter &&
               strcmp(config->geminiMode, "adapter") == 0 &&
               (task->modelId[0] == '\0' || strncmp(task->modelId, "google/", 7) == 0);
    }
    return 0;
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

static const char *profileDefaultAutonomy(const char *profile) {
    if (!profile || strcmp(profile, "observe") == 0) return "observe";
    if (strcmp(profile, "repo_branch") == 0 || strcmp(profile, "test_local") == 0) return "ask-batch";
    return "ask-each";
}

static const char *configuredAutonomyForAgent(const WtConfig *config, const char *agentName) {
    if (strcmp(agentName, "claude") == 0 && config->claudeDefaultAutonomyLevel[0]) {
        return config->claudeDefaultAutonomyLevel;
    }
    if (strcmp(agentName, "chatgpt") == 0 && config->chatgptDefaultAutonomyLevel[0]) {
        return config->chatgptDefaultAutonomyLevel;
    }
    if (strcmp(agentName, "gemini") == 0 && config->geminiDefaultAutonomyLevel[0]) {
        return config->geminiDefaultAutonomyLevel;
    }
    return config->defaultAutonomyLevel[0] ? config->defaultAutonomyLevel : NULL;
}

static void effectiveAutonomyLevel(const WtConfig *config, const char *agentName,
                                   const WtTaskSummary *task, char *buffer, size_t bufferSize) {
    if (task->autonomyLevel[0]) {
        snprintf(buffer, bufferSize, "%s", task->autonomyLevel);
        return;
    }
    const char *configured = configuredAutonomyForAgent(config, agentName);
    snprintf(buffer, bufferSize, "%s", configured ? configured : profileDefaultAutonomy(task->toolProfile));
}

static int scopeCoversAdapter(const char *scope) {
    return scope && (strstr(scope, "workspace") || strstr(scope, "adapter") || strstr(scope, "*"));
}

static int cleanWorktreeRequiredOk(const WtTaskSummary *task) {
    if (!task->autonomyRequiresCleanWorktree) {
        return 1;
    }
    int rc = system("git diff --quiet && git diff --cached --quiet");
    return rc == 0;
}

static int autonomyGrantActive(const WtConfig *config, const char *agentName,
                               const WtTaskSummary *task, const char *adapterName,
                               long long nowUnixMs, char *level, size_t levelSize,
                               char *reason, size_t reasonSize) {
    effectiveAutonomyLevel(config, agentName, task, level, levelSize);
    long long revokedAt = 0;
    char revokedBy[WT_TASK_AGENT_SIZE];
    wtTaskReadLatestAutonomyRevocation(config->taskLedgerPath, task->taskId,
                                       &revokedAt, revokedBy, sizeof(revokedBy));
    if (revokedAt > 0) {
        snprintf(reason, reasonSize, "autonomy revoked by %s", revokedBy[0] ? revokedBy : "operator");
        return 0;
    }
    if (strcmp(level, "autonomous") != 0) {
        snprintf(reason, reasonSize, "autonomy level %s does not permit elevated adapter flags", level);
        return 0;
    }
    if (task->autonomyTtlSeconds <= 0 || !scopeCoversAdapter(task->autonomyScope)) {
        snprintf(reason, reasonSize, "autonomy grant missing ttl or adapter/workspace scope");
        return 0;
    }
    long long grantStart = task->autonomyCreatedAtUnixMs > 0 ? task->autonomyCreatedAtUnixMs : nowUnixMs;
    if (nowUnixMs > grantStart + (long long)task->autonomyTtlSeconds * 1000LL) {
        snprintf(reason, reasonSize, "autonomy grant expired");
        return 0;
    }
    if (!cleanWorktreeRequiredOk(task)) {
        snprintf(reason, reasonSize, "autonomy requires a clean worktree");
        return 0;
    }
    snprintf(reason, reasonSize, "autonomy grant permits %s adapter invocation", adapterName);
    return 1;
}

/*
 * Phase 3 Sprint 1: wait for an adapter child process to exit, while
 *   (1) renewing the lease every leaseRenewalIntervalSeconds, and
 *   (2) checking for an operator cancel request via the ledger.
 *
 * Returns 0 on normal exit, 1 on timeout, 2 on cancel. On cancel or timeout
 * the helper kills the child's process group so any helper subprocesses go
 * with it (the adapter is invoked via fork+exec, so child is the leader of
 * its own process group). Caller still observes the child's exit status via
 * the waitpid in the loop; *outTimedOut and *outCancelled are set so the
 * caller can format the manifest + status events accordingly.
 */
static int waitForAdapterChild(const WtConfig *config, const char *agentName,
                               const WtTaskSummary *task, pid_t child,
                               int timeoutSeconds, int *outStatus,
                               int *outTimedOut, int *outCancelled,
                               long long *outFinalLeaseExpiry) {
    *outTimedOut = 0;
    *outCancelled = 0;
    time_t start = time(NULL);
    time_t lastRenewal = start;
    time_t lastCancelCheck = start;
    int renewalInterval = config->leaseRenewalIntervalSeconds > 0 ?
                          config->leaseRenewalIntervalSeconds : 600;
    int cancelInterval = config->cancelPollIntervalSeconds > 0 ?
                         config->cancelPollIntervalSeconds : 5;
    int leaseDuration = config->leaseDurationSeconds > 0 ?
                        config->leaseDurationSeconds : 900;
    long long currentLeaseExpiry = (long long)start * 1000LL + (long long)leaseDuration * 1000LL;
    /*
     * Configure the lease window to fit the chosen lease duration. The initial
     * lease was recorded by handleAssignedTask with the same duration, so the
     * first renewal extends rather than rewriting the original ceiling.
     */
    while (1) {
        pid_t done = waitpid(child, outStatus, WNOHANG);
        if (done == child) {
            break;
        }
        if (done < 0) {
            perror("wait adapter child");
            return -1;
        }
        time_t now = time(NULL);
        if ((int)(now - start) >= timeoutSeconds) {
            kill(-child, SIGKILL);
            waitpid(child, outStatus, 0);
            *outTimedOut = 1;
            break;
        }
        if ((int)(now - lastCancelCheck) >= cancelInterval) {
            lastCancelCheck = now;
            if (wtTaskCancelRequested(config->taskLedgerPath, task->taskId)) {
                /* SIGTERM to the whole group first so the adapter can clean
                 * up its own children; SIGKILL after a brief grace period. */
                kill(-child, SIGTERM);
                for (int waitTicks = 0; waitTicks < 3; waitTicks++) {
                    sleep(1);
                    pid_t reaped = waitpid(child, outStatus, WNOHANG);
                    if (reaped == child) break;
                }
                pid_t reaped = waitpid(child, outStatus, WNOHANG);
                if (reaped != child) {
                    kill(-child, SIGKILL);
                    waitpid(child, outStatus, 0);
                }
                *outCancelled = 1;
                break;
            }
        }
        if ((int)(now - lastRenewal) >= renewalInterval) {
            lastRenewal = now;
            currentLeaseExpiry = (long long)now * 1000LL + (long long)leaseDuration * 1000LL;
            /* Attempt number is recovered from the original lease event by the
             * projection; we pass 1 here because the API consumer (status,
             * audit) reads attemptCount from the projection rather than from
             * the renewal event. The renewal helper records the renewal
             * irrespective of count so the audit shows the chain. */
            wtTaskAppendLeaseRenewal(config->taskLedgerPath, task->taskId, agentName,
                                     1, currentLeaseExpiry, config->fsyncEachMessage);
        }
        sleep(1);
    }
    *outFinalLeaseExpiry = currentLeaseExpiry;
    return 0;
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
    char autonomyLevel[WT_TASK_POLICY_SIZE];
    char autonomyReason[256];
    int elevated = autonomyGrantActive(config, agentName, task, "codex", (long long)time(NULL) * 1000LL,
                                       autonomyLevel, sizeof(autonomyLevel),
                                       autonomyReason, sizeof(autonomyReason));
    wtTaskAppendAutonomyEvent(config->taskLedgerPath, task->taskId, actor, "codex",
                              elevated ? "adapter_invocation_elevated" : "adapter_invocation",
                              "codex-cli", autonomyLevel, autonomyReason, elevated, -1,
                              config->fsyncEachMessage);

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
        /* Make this child the leader of its own process group so the wait
         * helper can SIGTERM/SIGKILL the whole tree on cancel or timeout. */
        setpgid(0, 0);
        int stdoutFd = open(stdoutPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        int stderrFd = open(stderrPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (stdoutFd >= 0) dup2(stdoutFd, STDOUT_FILENO);
        if (stderrFd >= 0) dup2(stderrFd, STDERR_FILENO);
        if (stdoutFd > 2) close(stdoutFd);
        if (stderrFd > 2) close(stderrFd);
        char *args[20];
        int argc = 0;
        args[argc++] = (char *)config->gptCommand;
        args[argc++] = "exec";
        args[argc++] = "--ephemeral";
        args[argc++] = "--skip-git-repo-check";
        args[argc++] = "--cd";
        args[argc++] = workspace;
        args[argc++] = "--sandbox";
        args[argc++] = elevated ? "danger-full-access" : "workspace-write";
        args[argc++] = "--ask-for-approval";
        args[argc++] = elevated ? "never" : "on-request";
        args[argc++] = "--output-last-message";
        args[argc++] = resultPath;
        args[argc++] = prompt;
        args[argc] = NULL;
        execvp(config->gptCommand, args);
        perror("exec codex");
        _exit(127);
    }
    /* Also set the child's pgid from the parent in case the child raced past
     * exec before its own setpgid; both call sites are idempotent. */
    setpgid(child, child);

    int status = 0;
    int timedOut = 0;
    int cancelled = 0;
    long long finalLeaseExpiry = 0;
    if (waitForAdapterChild(config, agentName, task, child, timeoutSeconds,
                            &status, &timedOut, &cancelled, &finalLeaseExpiry) != 0) {
        return 1;
    }

    int exitCode;
    if (cancelled) {
        exitCode = 130; /* SIGTERM convention */
    } else if (timedOut) {
        exitCode = 124;
    } else {
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 125;
    }
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
             "  \"toolProfile\": \"%s\",\n"
             "  \"timeoutSeconds\": %d,\n"
             "  \"maxOutputBytes\": %d,\n"
             "  \"workspace\": \"%s\",\n"
             "  \"stdout\": \"%s\",\n"
             "  \"stderr\": \"%s\",\n"
             "  \"result\": \"%s\",\n"
             "  \"timedOut\": %s,\n"
             "  \"exitCode\": %d\n"
             "}\n",
             task->taskId, config->gptCommand, task->toolProfile, timeoutSeconds, maxOutputBytes,
             workspace, stdoutPath, stderrPath, resultPath,
             timedOut ? "true" : "false", exitCode);
    writeTextFile(manifestPath, manifest);

    char eventMessage[1024];
    snprintf(eventMessage, sizeof(eventMessage),
             "Codex adapter exitCode=%d timedOut=%s cancelled=%s; see task workspace manifest.",
             exitCode, timedOut ? "true" : "false", cancelled ? "true" : "false");
    /* Cancelled runs land as "cancelled" so the artifact promotion path can
     * filter them out (operator policy: cancelled output never gets promoted). */
    const char *finalStatus = cancelled ? "cancelled" : (exitCode == 0 ? "complete" : "failed");
    /* Classify the failure so the operator can see why this attempt died. */
    const char *retryCause = NULL;
    if (cancelled) {
        retryCause = "operator_cancel";
    } else if (exitCode != 0) {
        retryCause = timedOut ? "timeout" : (exitCode == 127 ? "adapter_unavailable" : "exit_nonzero");
    }
    wtTaskAppendStatusEventWithCause(config->taskLedgerPath, task->taskId, finalStatus, actor,
                                     eventMessage, retryCause, config->fsyncEachMessage);
    wtTaskAppendAutonomyEvent(config->taskLedgerPath, task->taskId, actor, "codex",
                              elevated ? "adapter_exit_elevated" : "adapter_exit",
                              "codex-cli", autonomyLevel, autonomyReason, elevated, exitCode,
                              config->fsyncEachMessage);

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

static int runCliArtifactAdapter(const WtConfig *config, const char *agentName,
                                 const WtTaskSummary *task, const char *adapterName,
                                 const char *command) {
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
             "You are WovenTeam agent %s running the %s artifact adapter.\n\n"
             "Task ID: %s\nRole: %s\nTool policy: %s\n\n"
             "Task title: %s\n\nTask body:\n%s\n\n"
             "Produce a concise result artifact. Do not modify the WovenTeam repository.",
             agentName, adapterName, task->taskId, task->assignedRole, task->toolProfile,
             task->title, task->body);
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
    char autonomyLevel[WT_TASK_POLICY_SIZE];
    char autonomyReason[256];
    int elevated = autonomyGrantActive(config, agentName, task, adapterName, (long long)time(NULL) * 1000LL,
                                       autonomyLevel, sizeof(autonomyLevel),
                                       autonomyReason, sizeof(autonomyReason));
    char commandClass[128];
    snprintf(commandClass, sizeof(commandClass), "%s-cli", adapterName);
    wtTaskAppendAutonomyEvent(config->taskLedgerPath, task->taskId, actor, adapterName,
                              elevated ? "adapter_invocation_elevated" : "adapter_invocation",
                              commandClass, autonomyLevel, autonomyReason, elevated, -1,
                              config->fsyncEachMessage);
    char runningMessage[512];
    snprintf(runningMessage, sizeof(runningMessage), "%s adapter started for taskId=%s", adapterName, task->taskId);
    wtTaskAppendStatusEvent(config->taskLedgerPath, task->taskId, "running", actor, runningMessage, config->fsyncEachMessage);
    appendAgentTaskMessage(config, agentName, task, "task.status", "running", runningMessage);

    pid_t child = fork();
    if (child < 0) {
        perror("fork cli artifact adapter");
        return 1;
    }
    if (child == 0) {
        /* Process-group leader so the wait helper can SIGTERM/SIGKILL the tree. */
        setpgid(0, 0);
        int stdoutFd = open(stdoutPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        int stderrFd = open(stderrPath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (stdoutFd >= 0) dup2(stdoutFd, STDOUT_FILENO);
        if (stderrFd >= 0) dup2(stderrFd, STDERR_FILENO);
        if (stdoutFd > 2) close(stdoutFd);
        if (stderrFd > 2) close(stderrFd);
        if (chdir(workspace) != 0) {
            perror("chdir cli artifact workspace");
            _exit(126);
        }
        if (strcmp(adapterName, "claude") == 0) {
            if (elevated) {
                execlp(command, command, "--dangerously-skip-permissions", "--bare", "--print", prompt, (char *)NULL);
            }
            execlp(command, command, "--bare", "--print", prompt, (char *)NULL);
        } else if (strcmp(adapterName, "gemini") == 0) {
            execlp(command, command, "--skip-trust", "--approval-mode", elevated ? "yolo" : "plan",
                   "--output-format", "text", "--prompt", prompt, (char *)NULL);
        } else {
            execlp(command, command, prompt, (char *)NULL);
        }
        perror("exec cli artifact adapter");
        _exit(127);
    }
    setpgid(child, child);

    int status = 0;
    int timedOut = 0;
    int cancelled = 0;
    long long finalLeaseExpiry = 0;
    if (waitForAdapterChild(config, agentName, task, child, timeoutSeconds,
                            &status, &timedOut, &cancelled, &finalLeaseExpiry) != 0) {
        return 1;
    }

    int exitCode;
    if (cancelled) {
        exitCode = 130;
    } else if (timedOut) {
        exitCode = 124;
    } else {
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 125;
    }
    capFileSize(stdoutPath, maxOutputBytes);
    capFileSize(stderrPath, maxOutputBytes);
    char resultSnippet[2048];
    readFileSnippet(stdoutPath, maxOutputBytes < 2000 ? maxOutputBytes : 2000, resultSnippet, sizeof(resultSnippet));
    if (resultSnippet[0] == '\0') {
        snprintf(resultSnippet, sizeof(resultSnippet), "%s adapter produced no stdout result.", adapterName);
    }
    writeTextFile(resultPath, resultSnippet);

    char manifest[4096];
    snprintf(manifest, sizeof(manifest),
             "{\n"
             "  \"schema\": \"woventeam.adapter_manifest.v0.1\",\n"
             "  \"taskId\": \"%s\",\n"
             "  \"adapter\": \"%s\",\n"
             "  \"command\": \"%s\",\n"
             "  \"toolProfile\": \"%s\",\n"
             "  \"timeoutSeconds\": %d,\n"
             "  \"maxOutputBytes\": %d,\n"
             "  \"workspace\": \"%s\",\n"
             "  \"prompt\": \"%s\",\n"
             "  \"stdout\": \"%s\",\n"
             "  \"stderr\": \"%s\",\n"
             "  \"result\": \"%s\",\n"
             "  \"timedOut\": %s,\n"
             "  \"exitCode\": %d\n"
             "}\n",
             task->taskId, adapterName, command, task->toolProfile, timeoutSeconds, maxOutputBytes,
             workspace, promptPath, stdoutPath, stderrPath, resultPath,
             timedOut ? "true" : "false", exitCode);
    writeTextFile(manifestPath, manifest);

    char eventMessage[1024];
    snprintf(eventMessage, sizeof(eventMessage),
             "%s adapter exitCode=%d timedOut=%s cancelled=%s; see task workspace manifest.",
             adapterName, exitCode, timedOut ? "true" : "false", cancelled ? "true" : "false");
    const char *finalStatus = cancelled ? "cancelled" : (exitCode == 0 ? "complete" : "failed");
    const char *retryCause = NULL;
    if (cancelled) {
        retryCause = "operator_cancel";
    } else if (exitCode != 0) {
        retryCause = timedOut ? "timeout" : (exitCode == 127 ? "adapter_unavailable" : "exit_nonzero");
    }
    wtTaskAppendStatusEventWithCause(config->taskLedgerPath, task->taskId, finalStatus, actor,
                                     eventMessage, retryCause, config->fsyncEachMessage);
    wtTaskAppendAutonomyEvent(config->taskLedgerPath, task->taskId, actor, adapterName,
                              elevated ? "adapter_exit_elevated" : "adapter_exit",
                              commandClass, autonomyLevel, autonomyReason, elevated, exitCode,
                              config->fsyncEachMessage);

    char detail[1024];
    snprintf(detail, sizeof(detail), "%s adapter exitCode=%d workspace=%s", adapterName, exitCode, workspace);
    appendAgentTaskMessage(config, agentName, task, "task.result", finalStatus, detail);

    printf("[adapter:%s] %s task=%s exit=%d workspace=%s\n", adapterName, finalStatus, task->taskId, exitCode, workspace);
    return exitCode == 0 ? 0 : 1;
}

static int handleAssignedTask(const WtConfig *config, const char *agentName) {
    if (wtTaskAgentPaused(config->taskLedgerPath, agentName)) {
        printf("[agent] %s paused by operator control\n", agentName);
        return 0;
    }
    WtTaskSummary task;
    long long nowUnixMs = (long long)time(NULL) * 1000LL;
    int found = wtTaskFindClaimableForAgent(config->taskLedgerPath, agentName, nowUnixMs, &task);
    if (found <= 0) {
        return 0;
    }
    /*
     * Sprint 3 closeout: inspect the latest lease on this task before claiming.
     * If a foreign agent still holds a valid lease we must not double-run; if
     * the lease has expired we append a "reclaim" event so the projection knows
     * this is the second attempt and the new lease counter is consistent.
     */
    char leaseHolder[WT_TASK_AGENT_SIZE] = "";
    long long previousLeaseExpiresAt = 0;
    int previousAttempt = 0;
    int hasActiveLease = wtTaskFindActiveLease(config->taskLedgerPath, task.taskId,
                                               leaseHolder, sizeof(leaseHolder),
                                               &previousLeaseExpiresAt, &previousAttempt);
    if (hasActiveLease && previousLeaseExpiresAt > nowUnixMs && strcmp(leaseHolder, agentName) != 0) {
        /* Foreign lease still valid - skip this task. */
        printf("[agent] %s skipping %s held by %s until %lld\n",
               agentName, task.taskId, leaseHolder, previousLeaseExpiresAt);
        return 0;
    }
    if (hasActiveLease && previousLeaseExpiresAt <= nowUnixMs) {
        /* The previous lease is stale - release it before recording our own. */
        char reclaimMessage[256];
        snprintf(reclaimMessage, sizeof(reclaimMessage),
                 "Auto-reclaim by wt-agent@%s after lease expired (previous holder=%s).",
                 agentName, leaseHolder[0] ? leaseHolder : "unknown");
        if (wtTaskAppendReclaimEvent(config->taskLedgerPath, task.taskId, leaseHolder,
                                     agentName, "lease_expired", reclaimMessage,
                                     config->fsyncEachMessage) != 0) {
            perror("record auto-reclaim");
            return 1;
        }
    }
    int leaseAttempt = previousAttempt + 1;
    /* Lease window is configurable in Phase 3 Sprint 1; default 900s matches
     * the prior hard-coded 15-minute value the projection uses for stuck-task
     * detection. */
    int leaseDuration = config->leaseDurationSeconds > 0 ? config->leaseDurationSeconds : 900;
    long long leaseExpiresAtUnixMs = nowUnixMs + (long long)leaseDuration * 1000LL;
    if (wtTaskAppendLeaseEvent(config->taskLedgerPath, task.taskId, agentName, leaseAttempt,
                               leaseExpiresAtUnixMs, config->fsyncEachMessage) != 0) {
        perror("record task lease");
        return 1;
    }
    char actor[128];
    snprintf(actor, sizeof(actor), "wt-agent@%s", agentName);
    char effectiveLevel[WT_TASK_POLICY_SIZE];
    effectiveAutonomyLevel(config, agentName, &task, effectiveLevel, sizeof(effectiveLevel));
    if (strcmp(effectiveLevel, "autonomous") == 0) {
        wtTaskAppendAutonomyEvent(config->taskLedgerPath, task.taskId, actor, agentName,
                                  "grant_observed", "task-claim", effectiveLevel,
                                  "agent claim entered autonomous policy path", 1, -1,
                                  config->fsyncEachMessage);
    }
    if (isCodexEligibleTask(config, agentName, &task)) {
        return runCodexAdapter(config, agentName, &task);
    }
    if (isCliArtifactEligibleTask(config, agentName, &task)) {
        if (strcmp(agentName, "claude") == 0) {
            return runCliArtifactAdapter(config, agentName, &task, "claude", config->claudeCommand);
        }
        return runCliArtifactAdapter(config, agentName, &task, "gemini", config->geminiCommand);
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

static int curlPostJson(const char *baseUrl, const char *token, const char *path,
                        const char *json, char *response, size_t responseSize) {
    if (!shellSafeText(baseUrl) || !shellSafeText(token) || !shellSafeText(path)) {
        fprintf(stderr, "remote URL, token, or path contains unsupported shell characters\n");
        return -1;
    }
    char templatePath[] = "/tmp/wt-agent-remote-XXXXXX";
    int fd = mkstemp(templatePath);
    if (fd < 0) {
        perror("mkstemp remote response");
        return -1;
    }
    close(fd);
    char command[4096];
    snprintf(command, sizeof(command),
             "curl -sS -H 'Content-Type: application/json' -H 'Authorization: Bearer %s' "
             "-d '%s' '%s%s' > '%s'",
             token, json, baseUrl, path, templatePath);
    int rc = system(command);
    readFileSnippet(templatePath, (int)responseSize - 1, response, responseSize);
    unlink(templatePath);
    return rc == 0 ? 0 : -1;
}

static int postRemoteStatus(const char *remoteUrl, const char *bearerToken,
                            const char *agentName, const char *taskId,
                            const char *status, const char *message) {
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"agent\":\"%s\",\"taskId\":\"%s\",\"status\":\"%s\",\"message\":\"%s\"}",
             agentName, taskId, status, message);
    char response[1024];
    if (curlPostJson(remoteUrl, bearerToken, "/api/remote-task-event", body,
                     response, sizeof(response)) != 0) {
        fprintf(stderr, "remote status post failed: %s\n", response);
        return 1;
    }
    if (!strstr(response, "\"ok\":true")) {
        fprintf(stderr, "remote status rejected: %s\n", response);
        return 1;
    }
    return 0;
}

static int handleRemoteOnce(const WtConfig *config, const char *agentName,
                            const char *remoteUrl, const char *bearerToken,
                            const char *hostName) {
    (void)config;
    if (!shellSafeText(remoteUrl) || !shellSafeText(bearerToken) || !shellSafeText(hostName)) {
        fprintf(stderr, "remote mode requires shell-safe URL, token, and host\n");
        return 2;
    }
    char capBody[1024];
    const char *profiles = strcmp(agentName, "chatgpt") == 0 ? "observe,repo_branch,test_local" : "observe,ops_read";
    snprintf(capBody, sizeof(capBody),
             "{\"host\":\"%s\",\"agent\":\"%s\",\"profiles\":\"%s\",\"adapters\":\"%s\"}",
             hostName, agentName, profiles, agentName);
    char response[4096];
    if (curlPostJson(remoteUrl, bearerToken, "/api/host-capabilities", capBody,
                     response, sizeof(response)) != 0 || !strstr(response, "\"ok\":true")) {
        fprintf(stderr, "host capability registration failed: %s\n", response);
        return 1;
    }
    char claimBody[512];
    snprintf(claimBody, sizeof(claimBody), "{\"host\":\"%s\",\"agent\":\"%s\"}", hostName, agentName);
    if (curlPostJson(remoteUrl, bearerToken, "/api/remote-claim", claimBody,
                     response, sizeof(response)) != 0) {
        fprintf(stderr, "remote claim failed: %s\n", response);
        return 1;
    }
    if (strstr(response, "token_revoked")) {
        fprintf(stderr, "remote claim rejected: token_revoked\n");
        return 3;
    }
    if (strstr(response, "capability_unmet")) {
        fprintf(stderr, "remote claim rejected: capability_unmet\n");
        return 4;
    }
    if (!strstr(response, "\"claimed\":true")) {
        printf("[remote] %s@%s no claim: %s\n", agentName, hostName, response);
        return 0;
    }
    char taskId[WT_TASK_ID_SIZE];
    if (wtJsonReadString(response, "taskId", taskId, sizeof(taskId)) != 0) {
        fprintf(stderr, "remote claim response missing taskId: %s\n", response);
        return 1;
    }
    if (postRemoteStatus(remoteUrl, bearerToken, agentName, taskId, "running",
                         "Remote stub agent accepted task.") != 0) {
        return 1;
    }
    if (postRemoteStatus(remoteUrl, bearerToken, agentName, taskId, "complete",
                         "Remote stub agent completed task.") != 0) {
        return 1;
    }
    printf("[remote] %s@%s completed %s\n", agentName, hostName, taskId);
    return 0;
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    const char *agentName = NULL;
    const char *remoteUrl = NULL;
    const char *bearerToken = NULL;
    const char *hostName = NULL;
    const char *sshHost = NULL;
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
        } else if (strcmp(argv[index], "--remote") == 0 && index + 1 < argc) {
            remoteUrl = argv[++index];
        } else if (strcmp(argv[index], "--bearer") == 0 && index + 1 < argc) {
            bearerToken = argv[++index];
        } else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
            hostName = argv[++index];
        } else if (strcmp(argv[index], "--ssh") == 0 && index + 1 < argc) {
            sshHost = argv[++index];
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
    if (sshHost) {
        if (!shellSafeText(sshHost)) {
            fprintf(stderr, "--ssh host contains unsupported characters\n");
            return 2;
        }
        printf("operator launch target: ssh %s wt-agent --agent %s --remote <url> --bearer <token> --host %s\n",
               sshHost, agentName, sshHost);
        return 0;
    }
    if (remoteUrl || bearerToken || hostName) {
        if (!remoteUrl || !bearerToken || !hostName || loop) {
            usage(argv[0]);
            return 2;
        }
        return handleRemoteOnce(&config, agentName, remoteUrl, bearerToken, hostName);
    }
    do {
        int rc = handleOnce(&config, agentName);
        if (rc != 0 || once) {
            return rc;
        }
        sleepMilliseconds(config.agentPollMilliseconds);
    } while (1);
}
