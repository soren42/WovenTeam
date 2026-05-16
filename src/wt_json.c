/*
 * wt_json.c - Minimal JSON string escaping and field extraction.
 *
 * The room store emits controlled flat JSON objects. These helpers read that
 * format plus the tiny POST body accepted by wt-roomd without adding a parser
 * dependency to the core runtime.
 */
#include "wt_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wtJsonEscape(const char *input, char *output, size_t outputSize) {
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor; cursor++) {
        char escaped[8];
        const char *piece = escaped;
        if (*cursor == '"' || *cursor == '\\') {
            snprintf(escaped, sizeof(escaped), "\\%c", *cursor);
        } else if (*cursor == '\n') {
            snprintf(escaped, sizeof(escaped), "\\n");
        } else if (*cursor == '\r') {
            snprintf(escaped, sizeof(escaped), "\\r");
        } else if (*cursor == '\t') {
            snprintf(escaped, sizeof(escaped), "\\t");
        } else if (*cursor < 32) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
        } else {
            escaped[0] = (char)*cursor;
            escaped[1] = '\0';
        }
        size_t pieceLength = strlen(piece);
        if (used + pieceLength + 1 > outputSize) {
            return -1;
        }
        memcpy(output + used, piece, pieceLength);
        used += pieceLength;
    }
    if (used + 1 > outputSize) {
        return -1;
    }
    output[used] = '\0';
    return 0;
}

static const char *findFieldValue(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *cursor = json;
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        cursor += strlen(pattern);
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == ':') {
            cursor++;
            while (*cursor && isspace((unsigned char)*cursor)) cursor++;
            return cursor;
        }
    }
    return NULL;
}

int wtJsonReadString(const char *json, const char *key, char *output, size_t outputSize) {
    const char *cursor = findFieldValue(json, key);
    if (!cursor || *cursor != '"' || outputSize == 0) {
        return -1;
    }
    cursor++;
    size_t used = 0;
    while (*cursor && *cursor != '"') {
        char value = *cursor++;
        if (value == '\\') {
            char escaped = *cursor++;
            if (escaped == 'n') value = '\n';
            else if (escaped == 'r') value = '\r';
            else if (escaped == 't') value = '\t';
            else if (escaped == '"' || escaped == '\\' || escaped == '/') value = escaped;
            else value = escaped;
        }
        if (used + 1 >= outputSize) {
            return -1;
        }
        output[used++] = value;
    }
    output[used] = '\0';
    return *cursor == '"' ? 0 : -1;
}

int wtJsonReadLong(const char *json, const char *key, long *output) {
    const char *cursor = findFieldValue(json, key);
    if (!cursor) {
        return -1;
    }
    char *end = NULL;
    long value = strtol(cursor, &end, 10);
    if (end == cursor) {
        return -1;
    }
    *output = value;
    return 0;
}

int wtJsonReadLongLong(const char *json, const char *key, long long *output) {
    const char *cursor = findFieldValue(json, key);
    if (!cursor) {
        return -1;
    }
    char *end = NULL;
    long long value = strtoll(cursor, &end, 10);
    if (end == cursor) {
        return -1;
    }
    *output = value;
    return 0;
}
