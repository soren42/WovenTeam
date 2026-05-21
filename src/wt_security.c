#include "wt_security.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fnv1a64(const char *value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
        hash ^= (uint64_t)(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void wtTokenHash(const char *token, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    snprintf(out, outSize, "fnv1a64:%016llx", (unsigned long long)fnv1a64(token));
}

void wtTokenPreview(const char *token, char *out, size_t outSize) {
    if (!out || outSize == 0) return;
    size_t len = strlen(token ? token : "");
    if (len < 8) {
        snprintf(out, outSize, "%s", "[redacted]");
        return;
    }
    snprintf(out, outSize, "%.4s...[redacted]", token);
}

static int looksSecretAt(const char *s) {
    return strncmp(s, "wt_", 3) == 0 ||
           strncmp(s, "ghp_", 4) == 0 ||
           strncmp(s, "github_pat_", 11) == 0 ||
           strncmp(s, "AKIA", 4) == 0 ||
           strncmp(s, "-----BEGIN ", 11) == 0;
}

int wtRedactSecrets(const char *input, char *output, size_t outputSize) {
    if (!output || outputSize == 0) return -1;
    output[0] = '\0';
    const char *redacted = "[REDACTED]";
    size_t used = 0;
    for (const char *p = input ? input : ""; *p; ) {
        if (looksSecretAt(p)) {
            size_t n = strlen(redacted);
            if (used + n + 1 >= outputSize) return -1;
            memcpy(output + used, redacted, n);
            used += n;
            while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'' && *p != ',' && *p != '}') p++;
            continue;
        }
        if (used + 2 >= outputSize) return -1;
        output[used++] = *p++;
    }
    output[used] = '\0';
    return 0;
}
