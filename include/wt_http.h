/*
 * wt_http.h - Small HTTP helpers for wt-roomd.
 */
#ifndef WT_HTTP_H
#define WT_HTTP_H

#include <stddef.h>

int wtHttpSendText(int clientFd, int statusCode, const char *statusText,
                   const char *contentType, const char *body);
int wtHttpSendBytes(int clientFd, int statusCode, const char *statusText,
                    const char *contentType, const char *body, size_t bodySize);
int wtHttpReadRequest(int clientFd, char *buffer, size_t bufferSize, size_t *outLength);

#endif
