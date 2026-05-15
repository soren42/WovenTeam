/*
 * wt_http.c - Small HTTP response/request helpers for wt-roomd.
 */
#include "wt_http.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int wtHttpSendBytes(int clientFd, int statusCode, const char *statusText,
                    const char *contentType, const char *body, size_t bodySize) {
    char header[512];
    int headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        statusCode, statusText, contentType, bodySize);
    if (headerLength < 0 || (size_t)headerLength >= sizeof(header)) {
        return -1;
    }
    if (send(clientFd, header, (size_t)headerLength, 0) < 0) {
        return -1;
    }
    if (bodySize > 0 && send(clientFd, body, bodySize, 0) < 0) {
        return -1;
    }
    return 0;
}

int wtHttpSendText(int clientFd, int statusCode, const char *statusText,
                   const char *contentType, const char *body) {
    return wtHttpSendBytes(clientFd, statusCode, statusText, contentType,
                           body, strlen(body));
}

int wtHttpReadRequest(int clientFd, char *buffer, size_t bufferSize, size_t *outLength) {
    size_t used = 0;
    while (used + 1 < bufferSize) {
        ssize_t count = recv(clientFd, buffer + used, bufferSize - used - 1, 0);
        if (count <= 0) {
            break;
        }
        used += (size_t)count;
        buffer[used] = '\0';
        char *headersEnd = strstr(buffer, "\r\n\r\n");
        if (headersEnd) {
            char *contentLengthHeader = strstr(buffer, "Content-Length:");
            if (!contentLengthHeader) {
                break;
            }
            int contentLength = 0;
            sscanf(contentLengthHeader, "Content-Length: %d", &contentLength);
            size_t headerBytes = (size_t)(headersEnd + 4 - buffer);
            if (used >= headerBytes + (size_t)contentLength) {
                break;
            }
        }
    }
    buffer[used] = '\0';
    if (outLength) {
        *outLength = used;
    }
    return used > 0 ? 0 : -1;
}
