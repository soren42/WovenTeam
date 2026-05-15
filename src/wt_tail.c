/*
 * wt_tail.c - Human-readable tail for the durable Phase 0 room log.
 */
#include "wt_config.h"
#include "wt_message.h"
#include "wt_room_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(const char *programName) {
    fprintf(stderr, "Usage: %s [--config FILE] [--limit N] [--follow]\n", programName);
}

static void sleepMilliseconds(int milliseconds) {
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) != 0) {
    }
}

static int printRecent(const WtConfig *config, int limit, long *lastPrintedId) {
    WtMessage messages[256];
    if (limit > 256) {
        limit = 256;
    }
    int count = wtRoomReadRecent(config->roomLogPath, limit, messages, 256);
    if (count < 0) {
        perror("read room log");
        return 1;
    }
    for (int index = 0; index < count; index++) {
        if (messages[index].messageId > *lastPrintedId) {
            wtMessagePrintHuman(&messages[index]);
            *lastPrintedId = messages[index].messageId;
        }
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    WtConfig config;
    wtConfigInitDefaults(&config);
    int limit = 50;
    int follow = 0;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--config") == 0 && index + 1 < argc) {
            wtConfigLoadFile(&config, argv[++index]);
        } else if (strcmp(argv[index], "--limit") == 0 && index + 1 < argc) {
            limit = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--follow") == 0) {
            follow = 1;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    wtConfigApplyEnvironment(&config);
    long lastPrintedId = 0;
    do {
        int rc = printRecent(&config, limit, &lastPrintedId);
        if (rc != 0 || !follow) {
            return rc;
        }
        sleepMilliseconds(500);
    } while (1);
}
