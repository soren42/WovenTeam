/*
 * wt_json.h - Tiny JSON helpers for Phase 0 flat objects.
 *
 * This is not a general JSON library. It only escapes strings and extracts the
 * simple fields used by the room protocol, avoiding external dependencies.
 */
#ifndef WT_JSON_H
#define WT_JSON_H

#include <stddef.h>

int wtJsonEscape(const char *input, char *output, size_t outputSize);
int wtJsonReadString(const char *json, const char *key, char *output, size_t outputSize);
int wtJsonReadLong(const char *json, const char *key, long *output);
int wtJsonReadLongLong(const char *json, const char *key, long long *output);

#endif
