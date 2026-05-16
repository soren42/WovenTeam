/*
 * wt_roomd.c - Native HTTP/SSE daemon for the Phase 0 shared room.
 *
 * The daemon serves the static browser UI, accepts CEO messages, exposes recent
 * messages as JSON, and streams room updates with Server-Sent Events. It forks
 * one lightweight child per client so an SSE stream cannot block other local
 * requests in the parent accept loop.
 */
#include "wt_config.h"
#include "wt_http.h"
#include "wt_json.h"
#include "wt_message.h"
#include "wt_room_store.h"
#include "wt_task_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *programName) {
    fprintf(stderr, "Usage: %s [--config FILE] [--port N] [--bind ADDRESS]\n", programName);
}

static const char *contentTypeForPath(const char *path) {
    if (strstr(path, ".css")) return "text/css; charset=utf-8";
    if (strstr(path, ".js")) return "application/javascript; charset=utf-8";
    if (strstr(path, ".html")) return "text/html; charset=utf-8";
    return "text/plain; charset=utf-8";
}

static void sleepMilliseconds(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int serveFile(int clientFd, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return wtHttpSendText(clientFd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0 || size > 1024 * 1024) {
        fclose(file);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "file error\n");
    }
    char *body = malloc((size_t)size + 1);
    if (!body) {
        fclose(file);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "memory error\n");
    }
    size_t bytesRead = fread(body, 1, (size_t)size, file);
    fclose(file);
    if (bytesRead != (size_t)size) {
        free(body);
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "text/plain; charset=utf-8", "file read error\n");
    }
    int rc = wtHttpSendBytes(clientFd, 200, "OK", contentTypeForPath(path), body, (size_t)size);
    free(body);
    return rc;
}

static int sendMessagesJson(int clientFd, const WtConfig *config, int limit) {
    WtMessage messages[256];
    int count = wtRoomReadRecent(config->roomLogPath, limit, messages, 256);
    char body[WT_MESSAGE_JSON_SIZE * 4];
    size_t used = 0;
    body[used++] = '[';
    for (int index = 0; index < count; index++) {
        char json[WT_MESSAGE_JSON_SIZE];
        wtMessageToJson(&messages[index], json, sizeof(json));
        size_t jsonLength = strlen(json);
        if (used + jsonLength + 3 >= sizeof(body)) {
            break;
        }
        if (index > 0) body[used++] = ',';
        memcpy(body + used, json, jsonLength);
        used += jsonLength;
    }
    body[used++] = ']';
    body[used] = '\0';
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int handlePostMessage(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config->roomName);
    if (wtJsonReadString(body, "senderName", message.senderName, sizeof(message.senderName)) != 0 ||
        wtJsonReadString(body, "targetName", message.targetName, sizeof(message.targetName)) != 0 ||
        wtJsonReadString(body, "messageType", message.messageType, sizeof(message.messageType)) != 0 ||
        wtJsonReadString(body, "messageBody", message.messageBody, sizeof(message.messageBody)) != 0 ||
        wtMessageValidateNames(&message) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid message\"}\n");
    }
    if (wtRoomAppendNewMessage(config->roomLogPath, &message, config->fsyncEachMessage) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char json[WT_MESSAGE_JSON_SIZE + 32];
    char messageJson[WT_MESSAGE_JSON_SIZE];
    wtMessageToJson(&message, messageJson, sizeof(messageJson));
    snprintf(json, sizeof(json), "{\"ok\":true,\"message\":%s}\n", messageJson);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", json);
}

static void compactJsonLine(const char *input, char *output, size_t outputSize) {
    size_t used = 0;
    int inString = 0;
    int escaped = 0;
    for (const char *cursor = input; *cursor && used + 1 < outputSize; cursor++) {
        char value = *cursor;
        if (escaped) {
            output[used++] = value;
            escaped = 0;
            continue;
        }
        if (value == '\\' && inString) {
            output[used++] = value;
            escaped = 1;
            continue;
        }
        if (value == '"') {
            inString = !inString;
            output[used++] = value;
            continue;
        }
        if (!inString && (value == '\n' || value == '\r' || value == '\t')) {
            continue;
        }
        output[used++] = value;
    }
    output[used] = '\0';
}

static int appendTaskRoomEvent(const WtConfig *config, const char *taskId, const char *targetName,
                               const char *messageType, const char *status, const char *title) {
    WtMessage message;
    wtMessageInit(&message);
    snprintf(message.roomName, sizeof(message.roomName), "%s", config->roomName);
    snprintf(message.senderName, sizeof(message.senderName), "%s", "system");
    if (targetName && (strcmp(targetName, "claude") == 0 || strcmp(targetName, "chatgpt") == 0 ||
                       strcmp(targetName, "gemini") == 0 || strcmp(targetName, "ceo") == 0 ||
                       strcmp(targetName, "all") == 0)) {
        snprintf(message.targetName, sizeof(message.targetName), "%s", targetName);
    } else {
        snprintf(message.targetName, sizeof(message.targetName), "%s", "all");
    }
    snprintf(message.messageType, sizeof(message.messageType), "%s", messageType);
    snprintf(message.messageBody, sizeof(message.messageBody), "taskId=%s status=%s title=%s",
             taskId, status, title && title[0] ? title : "Untitled task");
    return wtRoomAppendNewMessage(config->roomLogPath, &message, config->fsyncEachMessage);
}

static int handlePostTaskPackage(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char schema[128];
    char taskId[WT_TASK_ID_SIZE];
    char assignedRole[WT_TASK_AGENT_SIZE];
    char assignedAgent[WT_TASK_AGENT_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char title[WT_TASK_TITLE_SIZE];
    if (wtJsonReadString(body, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "woventeam.task_package.v0.1") != 0 ||
        wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0 ||
        wtJsonReadString(body, "assignedRole", assignedRole, sizeof(assignedRole)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid task package\"}\n");
    }
    if (wtJsonReadString(body, "assignedAgent", assignedAgent, sizeof(assignedAgent)) != 0) {
        snprintf(assignedAgent, sizeof(assignedAgent), "%s", "all");
    }
    if (wtJsonReadString(body, "status", status, sizeof(status)) != 0) {
        snprintf(status, sizeof(status), "%s", "queued");
    }
    if (wtJsonReadString(body, "title", title, sizeof(title)) != 0) {
        snprintf(title, sizeof(title), "%s", "Untitled task");
    }
    char compact[WT_TASK_LEDGER_LINE_SIZE];
    compactJsonLine(body, compact, sizeof(compact));
    if (wtTaskAppendRecord(config->taskLedgerPath, compact, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, taskId, assignedAgent, "task.assign", status, title) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"assignedAgent\":\"%s\",\"status\":\"%s\"}\n",
             taskId, assignedAgent, status);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static int sendTasksJson(int clientFd, const WtConfig *config) {
    char body[WT_TASK_LEDGER_LINE_SIZE * 8];
    if (wtTaskReadAllJsonArray(config->taskLedgerPath, body, sizeof(body)) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false}\n");
    }
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
}

static int handlePostTaskEvent(int clientFd, const WtConfig *config, const char *request) {
    const char *body = strstr(request, "\r\n\r\n");
    if (!body) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false}\n");
    }
    body += 4;
    char schema[128];
    char taskId[WT_TASK_ID_SIZE];
    char status[WT_TASK_STATUS_SIZE];
    char message[WT_TASK_TITLE_SIZE];
    char createdBy[WT_TASK_AGENT_SIZE];
    if (wtJsonReadString(body, "schema", schema, sizeof(schema)) != 0 ||
        strcmp(schema, "woventeam.task_event.v0.1") != 0 ||
        wtJsonReadString(body, "taskId", taskId, sizeof(taskId)) != 0 ||
        wtJsonReadString(body, "status", status, sizeof(status)) != 0) {
        return wtHttpSendText(clientFd, 400, "Bad Request", "application/json", "{\"ok\":false,\"error\":\"invalid task event\"}\n");
    }
    if (wtJsonReadString(body, "message", message, sizeof(message)) != 0) {
        snprintf(message, sizeof(message), "%s", "Task status updated.");
    }
    if (wtJsonReadString(body, "createdBy", createdBy, sizeof(createdBy)) != 0) {
        snprintf(createdBy, sizeof(createdBy), "%s", "system");
    }
    char compact[WT_TASK_LEDGER_LINE_SIZE];
    compactJsonLine(body, compact, sizeof(compact));
    const char *messageType = strcmp(status, "complete") == 0 || strcmp(status, "failed") == 0 ?
                              "task.result" : "task.status";
    if (wtTaskAppendRecord(config->taskLedgerPath, compact, config->fsyncEachMessage) != 0 ||
        appendTaskRoomEvent(config, taskId, "all", messageType, status, message) != 0) {
        return wtHttpSendText(clientFd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"append failed\"}\n");
    }
    char response[512];
    snprintf(response, sizeof(response), "{\"ok\":true,\"taskId\":\"%s\",\"status\":\"%s\",\"createdBy\":\"%s\"}\n",
             taskId, status, createdBy);
    return wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", response);
}

static void sendSseEvent(int clientFd, const WtMessage *message) {
    char json[WT_MESSAGE_JSON_SIZE];
    char frame[WT_MESSAGE_JSON_SIZE + 64];
    wtMessageToJson(message, json, sizeof(json));
    int length = snprintf(frame, sizeof(frame), "event: message\ndata: %s\n\n", json);
    if (length > 0) {
        send(clientFd, frame, (size_t)length, 0);
    }
}

static int handleEvents(int clientFd, const WtConfig *config) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    if (send(clientFd, header, strlen(header), 0) < 0) {
        return -1;
    }
    long lastSentId = 0;
    for (int tick = 0; tick < 7200; tick++) {
        WtMessage messages[64];
        int count = wtRoomReadMessagesAfter(config->roomLogPath, lastSentId, messages, 64);
        for (int index = 0; index < count; index++) {
            sendSseEvent(clientFd, &messages[index]);
            lastSentId = messages[index].messageId;
        }
        if (count == 0) {
            send(clientFd, ": heartbeat\n\n", 13, 0);
        }
        sleepMilliseconds(500);
    }
    return 0;
}

static void handleClient(int clientFd, const WtConfig *config) {
    char request[16384];
    size_t requestLength = 0;
    if (wtHttpReadRequest(clientFd, request, sizeof(request), &requestLength) != 0) {
        close(clientFd);
        return;
    }
    (void)requestLength;
    char method[16] = "";
    char path[256] = "";
    sscanf(request, "%15s %255s", method, path);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serveFile(clientFd, "web/index.html");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/app.js") == 0) {
        serveFile(clientFd, "web/app.js");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/style.css") == 0) {
        serveFile(clientFd, "web/style.css");
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/events") == 0) {
        handleEvents(clientFd, config);
    } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/messages", 13) == 0) {
        int limit = 50;
        char *limitText = strstr(path, "limit=");
        if (limitText) limit = atoi(limitText + 6);
        sendMessagesJson(clientFd, config, limit);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tasks") == 0) {
        sendTasksJson(clientFd, config);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/message") == 0) {
        handlePostMessage(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-package") == 0) {
        handlePostTaskPackage(clientFd, config, request);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/task-event") == 0) {
        handlePostTaskEvent(clientFd, config, request);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/health") == 0) {
        char body[256];
        snprintf(body, sizeof(body), "{\"ok\":true,\"roomName\":\"%s\"}\n", config->roomName);
        wtHttpSendText(clientFd, 200, "OK", "application/json; charset=utf-8", body);
    } else {
        wtHttpSendText(clientFd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n");
    }
    close(clientFd);
}

static int createServerSocket(const WtConfig *config) {
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config->httpPort);
    if (inet_pton(AF_INET, config->httpBindAddress, &address.sin_addr) != 1) {
        close(serverFd);
        return -1;
    }
    if (bind(serverFd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(serverFd, 32) != 0) {
        close(serverFd);
        return -1;
    }
    return serverFd;
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            wtConfigLoadFile(&config, argv[++index]);
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            config.httpPort = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            snprintf(config.httpBindAddress, sizeof(config.httpBindAddress), "%s", argv[++index]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    wtConfigApplyEnvironment(&config);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    int serverFd = createServerSocket(&config);
    if (serverFd < 0) {
        perror("start wt-roomd");
        return 1;
    }
    printf("wt-roomd listening on http://%s:%d/ using %s\n",
           config.httpBindAddress, config.httpPort, config.roomLogPath);
    fflush(stdout);
    while (1) {
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pid_t child = fork();
        if (child == 0) {
            close(serverFd);
            handleClient(clientFd, &config);
            _exit(0);
        }
        close(clientFd);
    }
    close(serverFd);
    return 1;
}
