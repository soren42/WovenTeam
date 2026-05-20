/*
 * wt_notify.c - Notification dispatcher (Phase 3 Sprint 1).
 */
#include "wt_notify.h"
#include "wt_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/*
 * Look up a webhook URL in the keyed file. The file format is one KEY=URL
 * per line; '#' lines and blank lines are ignored. Whitespace around the key
 * and URL is trimmed. Returns 0 on success, -1 when the file cannot be
 * opened, 1 when the key isn't present.
 */
static int lookupWebhookUrl(const char *path, const char *key, char *url, size_t urlSize) {
    if (!path || !key || !url || urlSize == 0) {
        return -1;
    }
    url[0] = '\0';
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char *cursor = line;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || *cursor == '#') continue;
        char *equals = strchr(cursor, '=');
        if (!equals) continue;
        *equals = '\0';
        char *fileKey = cursor;
        char *fileVal = equals + 1;
        /* Trim whitespace + newline from both halves. */
        size_t keyLen = strlen(fileKey);
        while (keyLen > 0 && isspace((unsigned char)fileKey[keyLen - 1])) fileKey[--keyLen] = '\0';
        while (*fileVal && isspace((unsigned char)*fileVal)) fileVal++;
        size_t valLen = strlen(fileVal);
        while (valLen > 0 && isspace((unsigned char)fileVal[valLen - 1])) fileVal[--valLen] = '\0';
        if (strcmp(fileKey, key) == 0) {
            snprintf(url, urlSize, "%s", fileVal);
            found = 1;
            break;
        }
    }
    fclose(file);
    return found ? 0 : 1;
}

int wtNotifySend(const WtConfig *config, const char *key,
                 const char *title, const char *message) {
    if (!config || !title || !message) {
        return -1;
    }
    char url[1024];
    url[0] = '\0';
    const char *override = getenv("WT_NOTIFICATION_OVERRIDE_URL");
    if (override && *override) {
        /* Test override path. The integration test sets this to a local
         * mock listener so it can assert the daemon actually emits. */
        snprintf(url, sizeof(url), "%s", override);
    } else {
        if (!key || !key[0]) {
            return 1; /* no key configured for this event class - silent skip */
        }
        if (lookupWebhookUrl(config->slackWebhookFile, key, url, sizeof(url)) != 0 ||
            url[0] == '\0') {
            return 1;
        }
    }
    /*
     * Build the JSON body. Slack accepts a minimal {"text": "..."} shape.
     * We bundle the title into the text since that's friendlier than two
     * separate fields when the receiver is just Slack.
     */
    char combined[2048];
    snprintf(combined, sizeof(combined), "%s — %s", title, message);
    char escaped[4096];
    if (wtJsonEscape(combined, escaped, sizeof(escaped)) != 0) {
        return -1;
    }
    /* Write the body to a tmp file so the JSON does not have to survive
     * shell escaping. tempnam / mkstemp avoids the security warning. */
    char tmpPath[64];
    snprintf(tmpPath, sizeof(tmpPath), "/tmp/wt-notify-XXXXXX");
    int tmpFd = mkstemp(tmpPath);
    if (tmpFd < 0) return -1;
    FILE *tmp = fdopen(tmpFd, "w");
    if (!tmp) { close(tmpFd); unlink(tmpPath); return -1; }
    fprintf(tmp, "{\"text\":\"%s\"}", escaped);
    fclose(tmp);
    /*
     * Compose the curl command. -sS keeps stderr but silences progress;
     * --max-time bounds the call so a slow webhook does not stall the
     * daemon. We redirect output to /dev/null because callers do not
     * consume the webhook response.
     */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -sS --max-time 5 -X POST -H 'Content-Type: application/json' "
             "-d @%s '%s' >/dev/null 2>&1",
             tmpPath, url);
    int rc = system(cmd);
    unlink(tmpPath);
    return rc == 0 ? 0 : -1;
}
