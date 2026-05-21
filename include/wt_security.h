#ifndef WT_SECURITY_H
#define WT_SECURITY_H

#include <stddef.h>

void wtTokenHash(const char *token, char *out, size_t outSize);
void wtTokenPreview(const char *token, char *out, size_t outSize);
int wtRedactSecrets(const char *input, char *output, size_t outputSize);

#endif
