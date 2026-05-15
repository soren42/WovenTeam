/*
 * wt_config.h - Small Phase 0 configuration surface.
 *
 * The native room spike keeps configuration deliberately plain: built-in
 * defaults, optional key=value file, environment overrides, and a few command
 * line flags used by individual binaries.
 */
#ifndef WT_CONFIG_H
#define WT_CONFIG_H

#include <stdbool.h>

#define WT_PATH_SIZE 256
#define WT_NAME_SIZE 64

typedef struct {
    char roomName[WT_NAME_SIZE];
    char roomLogPath[WT_PATH_SIZE];
    char httpBindAddress[WT_NAME_SIZE];
    int httpPort;
    int contextMessageCount;
    int agentPollMilliseconds;
    bool fsyncEachMessage;
    char claudeMode[WT_NAME_SIZE];
    char chatgptMode[WT_NAME_SIZE];
    char geminiMode[WT_NAME_SIZE];
    char claudeCommand[WT_PATH_SIZE];
    char gptCommand[WT_PATH_SIZE];
    char geminiCommand[WT_PATH_SIZE];
} WtConfig;

void wtConfigInitDefaults(WtConfig *config);
int wtConfigLoadFile(WtConfig *config, const char *path);
void wtConfigApplyEnvironment(WtConfig *config);
int wtConfigSetValue(WtConfig *config, const char *key, const char *value);

#endif
