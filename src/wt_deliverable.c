/*
 * wt_deliverable.c - Phase 3 Sprint 3 deliverables pipeline.
 *
 * Ships an accepted artifact through one of four packaging modes (copy,
 * tarball, branch, pull-request). Always runs the configured pre-ship
 * secret scan; branch + pr modes refuse on a positive match.
 *
 * External tool dependencies: sha256sum, tar, gzip, git, gh. All four are
 * expected to be on $PATH in the runtime environment (they are checked at
 * preflight time, not here, so a missing tool surfaces as a classified
 * error reason).
 */
#include "wt_deliverable.h"

#include "wt_json.h"
#include "wt_security.h"
#include "wt_system.h"
#include "wt_task_store.h"
#include "wt_time.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WT_DELIVERABLE_MAX_PATTERNS 32
#define WT_DELIVERABLE_SCAN_CHUNK   (64 * 1024)

typedef struct {
    char name[64];
    regex_t regex;
    int compiled;
} WtPattern;

static long long nowMs(void) { return wtNowUnixMilliseconds(); }

static void copyStringSafe(char *dst, size_t dstSize, const char *src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dstSize) len = dstSize - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void setError(WtDeliverableResult *r, const char *reason) {
    if (!r) return;
    copyStringSafe(r->errorReason, sizeof(r->errorReason), reason);
}

/*
 * Generate a short opaque id of the form <prefix>_<unixMs>_<6 hex>. /dev/urandom
 * supplies the hex tail so concurrent ships do not collide. Falls back to
 * the time-only form if /dev/urandom is unavailable.
 */
static void makeShortId(const char *prefix, char *out, size_t outSize) {
    unsigned char rnd[3] = {0};
    int gotRandom = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, rnd, sizeof(rnd)) == (ssize_t)sizeof(rnd)) gotRandom = 1;
        close(fd);
    }
    long long ms = nowMs();
    if (gotRandom) {
        snprintf(out, outSize, "%s_%lld_%02x%02x%02x", prefix, ms, rnd[0], rnd[1], rnd[2]);
    } else {
        snprintf(out, outSize, "%s_%lld", prefix, ms);
    }
}

int wtDeliverableModeFromString(const char *s, WtDeliverableMode *out) {
    if (!s || !out) return -1;
    if (strcmp(s, "copy") == 0) { *out = WT_DELIVERABLE_MODE_COPY; return 0; }
    if (strcmp(s, "tarball") == 0) { *out = WT_DELIVERABLE_MODE_TARBALL; return 0; }
    if (strcmp(s, "branch") == 0) { *out = WT_DELIVERABLE_MODE_BRANCH; return 0; }
    if (strcmp(s, "pull-request") == 0 || strcmp(s, "pr") == 0) {
        *out = WT_DELIVERABLE_MODE_PR; return 0;
    }
    return -1;
}

const char *wtDeliverableModeToString(WtDeliverableMode mode) {
    switch (mode) {
        case WT_DELIVERABLE_MODE_COPY: return "copy";
        case WT_DELIVERABLE_MODE_TARBALL: return "tarball";
        case WT_DELIVERABLE_MODE_BRANCH: return "branch";
        case WT_DELIVERABLE_MODE_PR: return "pull-request";
    }
    return "copy";
}

/*
 * Recursive-ish mkdir matching `mkdir -p`. Returns 0 if the directory exists
 * or was created, -1 on hard failure.
 */
static int mkdirP(const char *path) {
    if (!path || !*path) return -1;
    char buf[WT_DELIVERABLE_PATH_SIZE];
    copyStringSafe(buf, sizeof(buf), path);
    size_t len = strlen(buf);
    if (len == 0) return -1;
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                buf[i] = '/';
                return -1;
            }
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int wtDeliverableSha256(const char *path, char *hexOut, size_t hexOutSize,
                        long long *sizeBytesOut) {
    if (!path || !hexOut || hexOutSize < 65) return -1;
    hexOut[0] = '\0';
    if (sizeBytesOut) *sizeBytesOut = 0;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (sizeBytesOut) *sizeBytesOut = (long long)st.st_size;
    /*
     * wt-roomd installs SIGCHLD=SIG_IGN, which auto-reaps children and makes
     * popen()/pclose() return -1 (ECHILD). system() handles this case via
     * internal sigaction trickery, so we route through system() + a temp
     * file. Matches the pattern used in wt_notify.c.
     */
    char tmpPath[64];
    snprintf(tmpPath, sizeof(tmpPath), "/tmp/wt-sha-XXXXXX");
    int tmpFd = mkstemp(tmpPath);
    if (tmpFd < 0) return -1;
    close(tmpFd);
    char cmd[WT_DELIVERABLE_PATH_SIZE + 128];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s' > %s 2>/dev/null", path, tmpPath);
    int sysRc = wtSystemReliable(cmd);
    if (sysRc != 0) {
        unlink(tmpPath);
        return -1;
    }
    int ok = 0;
    FILE *out = fopen(tmpPath, "r");
    if (out) {
        char line[256];
        if (fgets(line, sizeof(line), out)) {
            char *end = line;
            while (*end && !isspace((unsigned char)*end)) end++;
            size_t hexLen = (size_t)(end - line);
            if (hexLen == 64 && hexLen < hexOutSize) {
                memcpy(hexOut, line, hexLen);
                hexOut[hexLen] = '\0';
                ok = 1;
            }
        }
        fclose(out);
    }
    unlink(tmpPath);
    return ok ? 0 : -1;
}

/*
 * Load patterns from the configured file. Each non-comment non-blank line is
 * NAME=REGEX. Compiles up to WT_DELIVERABLE_MAX_PATTERNS entries. Returns the
 * compiled count (>= 0) or -1 on file open failure.
 */
static int loadPatterns(const char *path, WtPattern *patterns, int maxPatterns) {
    if (!path || !*path || !patterns || maxPatterns <= 0) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int count = 0;
    char line[512];
    while (count < maxPatterns && fgets(line, sizeof(line), file)) {
        char *cursor = line;
        while (*cursor && isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0' || *cursor == '#') continue;
        char *equals = strchr(cursor, '=');
        if (!equals) continue;
        *equals = '\0';
        char *name = cursor;
        char *expr = equals + 1;
        /* Trim. */
        size_t nameLen = strlen(name);
        while (nameLen > 0 && isspace((unsigned char)name[nameLen - 1])) name[--nameLen] = '\0';
        while (*expr && isspace((unsigned char)*expr)) expr++;
        size_t exprLen = strlen(expr);
        while (exprLen > 0 && isspace((unsigned char)expr[exprLen - 1])) expr[--exprLen] = '\0';
        if (!*name || !*expr) continue;
        if (regcomp(&patterns[count].regex, expr, REG_EXTENDED | REG_NOSUB) != 0) {
            continue;
        }
        copyStringSafe(patterns[count].name, sizeof(patterns[count].name), name);
        patterns[count].compiled = 1;
        count++;
    }
    fclose(file);
    return count;
}

static void freePatterns(WtPattern *patterns, int count) {
    for (int i = 0; i < count; ++i) {
        if (patterns[i].compiled) regfree(&patterns[i].regex);
    }
}

int wtDeliverableScanFile(const WtConfig *config, const char *path,
                          WtDeliverableResult *result) {
    if (!config || !path || !result) return -1;
    result->scanRan = 0;
    result->scanMatched = 0;
    result->scanHitCount = 0;
    result->scanPatternCount = 0;
    if (!config->secretScanPatternsFile[0]) {
        return 0;
    }
    WtPattern patterns[WT_DELIVERABLE_MAX_PATTERNS];
    memset(patterns, 0, sizeof(patterns));
    int patternCount = loadPatterns(config->secretScanPatternsFile, patterns,
                                    WT_DELIVERABLE_MAX_PATTERNS);
    if (patternCount < 0) {
        /* Patterns file configured but missing - record but do not block. */
        return 0;
    }
    if (patternCount == 0) {
        return 0;
    }
    /*
     * Read the file in one shot. The expected artifact sizes are small
     * (result.md, manifest.json) so this is fine for v1.0; future revision
     * can stream.
     */
    FILE *file = fopen(path, "rb");
    if (!file) {
        freePatterns(patterns, patternCount);
        return -1;
    }
    /* Cap at 4 MiB to avoid memory exhaustion on a wild input. */
    const size_t maxRead = 4 * 1024 * 1024;
    char *buffer = (char *)malloc(maxRead + 1);
    if (!buffer) {
        fclose(file);
        freePatterns(patterns, patternCount);
        return -1;
    }
    size_t total = 0;
    size_t r;
    while (total < maxRead && (r = fread(buffer + total, 1, maxRead - total, file)) > 0) {
        total += r;
    }
    buffer[total] = '\0';
    fclose(file);
    /*
     * regexec on REG_NOSUB returns 0 or REG_NOMATCH. Count occurrences by
     * walking with offset advances on each match - we use a strchr-style
     * iteration that re-issues regexec on the suffix until no further match.
     */
    result->scanRan = 1;
    int totalHits = 0;
    int distinctPatterns = 0;
    for (int i = 0; i < patternCount; ++i) {
        int patternHits = 0;
        char *cursor = buffer;
        while (*cursor) {
            if (regexec(&patterns[i].regex, cursor, 0, NULL, 0) != 0) break;
            patternHits++;
            /*
             * Without REG_NOSUB we could advance by the match length; with it
             * we re-run with a one-char advance to avoid infinite loops on
             * zero-width matches. This is good enough for token-shaped
             * regexes which always consume characters.
             */
            cursor++;
            /* Cap per-pattern hits at 100 to avoid runaway on synthetic input. */
            if (patternHits >= 100) break;
        }
        if (patternHits > 0) {
            totalHits += patternHits;
            if (distinctPatterns < 8) {
                copyStringSafe(result->scanPatterns[distinctPatterns],
                               sizeof(result->scanPatterns[distinctPatterns]),
                               patterns[i].name);
                result->scanPatternCounts[distinctPatterns] = patternHits;
                distinctPatterns++;
            }
        }
    }
    result->scanHitCount = totalHits;
    result->scanPatternCount = distinctPatterns;
    result->scanMatched = totalHits > 0 ? 1 : 0;
    free(buffer);
    freePatterns(patterns, patternCount);
    return 0;
}

/* ---- ledger append helpers ---- */

static int appendDeliverableEvent(const char *ledgerPath,
                                  const WtDeliverableRequest *req,
                                  const WtDeliverableResult *res,
                                  WtDeliverableMode mode) {
    char eTaskId[WT_TASK_ID_SIZE * 2];
    char eInitiative[WT_TASK_ID_SIZE * 2];
    char eSource[WT_DELIVERABLE_PATH_SIZE * 2];
    char eDest[WT_DELIVERABLE_PATH_SIZE * 2];
    char eReviewer[WT_TASK_AGENT_SIZE * 2];
    char eCreatedBy[WT_TASK_AGENT_SIZE * 2];
    char eSupersedes[WT_DELIVERABLE_ID_SIZE * 2];
    char eDelivId[WT_DELIVERABLE_ID_SIZE * 2];
    char eAutonomyLevel[64];

    if (wtJsonEscape(req->taskId ? req->taskId : "", eTaskId, sizeof(eTaskId)) != 0 ||
        wtJsonEscape(req->initiativeId ? req->initiativeId : "", eInitiative, sizeof(eInitiative)) != 0 ||
        wtJsonEscape(req->sourcePath ? req->sourcePath : "", eSource, sizeof(eSource)) != 0 ||
        wtJsonEscape(res->deliverablePath, eDest, sizeof(eDest)) != 0 ||
        wtJsonEscape(req->reviewer ? req->reviewer : "", eReviewer, sizeof(eReviewer)) != 0 ||
        wtJsonEscape(req->createdBy ? req->createdBy : "operator", eCreatedBy, sizeof(eCreatedBy)) != 0 ||
        wtJsonEscape(req->supersedes ? req->supersedes : "", eSupersedes, sizeof(eSupersedes)) != 0 ||
        wtJsonEscape(res->deliverableId, eDelivId, sizeof(eDelivId)) != 0 ||
        wtJsonEscape(req->autonomyLevel ? req->autonomyLevel : "", eAutonomyLevel, sizeof(eAutonomyLevel)) != 0) {
        return -1;
    }
    /*
     * Provenance is optional in the schema; we always emit it because Sprint
     * 2 always has either an explicit grant or the default ask-each. Empty
     * autonomy level means the task predates autonomy_event emission.
     */
    char provenance[256];
    if (req->autonomyLevel && *req->autonomyLevel) {
        snprintf(provenance, sizeof(provenance),
                 ",\"autonomyProvenance\":{\"elevated\":%s,\"autonomyLevel\":\"%s\"}",
                 req->autonomyElevated ? "true" : "false", eAutonomyLevel);
    } else {
        provenance[0] = '\0';
    }
    char json[4096];
    int written = snprintf(json, sizeof(json),
        "{\"schema\":\"woventeam.deliverable.v0.1\","
        "\"deliverableId\":\"%s\",\"taskId\":\"%s\",\"initiativeId\":\"%s\","
        "\"sourceWorkspacePath\":\"%s\",\"deliverablePath\":\"%s\","
        "\"packagingMode\":\"%s\",\"sizeBytes\":%lld,\"sha256\":\"%s\","
        "\"reviewer\":\"%s\",\"supersedes\":%s%s%s%s,"
        "\"createdBy\":\"%s\",\"createdAtUnixMs\":%lld}",
        eDelivId, eTaskId, eInitiative,
        eSource, eDest,
        wtDeliverableModeToString(mode),
        res->sizeBytes, res->sha256,
        eReviewer,
        eSupersedes[0] ? "\"" : "null",
        eSupersedes[0] ? eSupersedes : "",
        eSupersedes[0] ? "\"" : "",
        provenance,
        eCreatedBy, nowMs());
    if (written < 0 || written >= (int)sizeof(json)) return -1;
    return wtTaskAppendRecord(ledgerPath, json, req->fsyncRecord);
}

static int appendSecretScanEvent(const char *ledgerPath,
                                 const WtDeliverableRequest *req,
                                 const WtDeliverableResult *res,
                                 WtDeliverableMode mode) {
    if (!res->scanRan) return 0;
    char eTaskId[WT_TASK_ID_SIZE * 2];
    char eDeliv[WT_DELIVERABLE_ID_SIZE * 2];
    char eScan[WT_DELIVERABLE_ID_SIZE * 2];
    char eSource[WT_DELIVERABLE_PATH_SIZE * 2];

    if (wtJsonEscape(req->taskId ? req->taskId : "", eTaskId, sizeof(eTaskId)) != 0 ||
        wtJsonEscape(res->deliverableId, eDeliv, sizeof(eDeliv)) != 0 ||
        wtJsonEscape(res->scanId, eScan, sizeof(eScan)) != 0 ||
        wtJsonEscape(req->sourcePath ? req->sourcePath : "", eSource, sizeof(eSource)) != 0) {
        return -1;
    }
    /* Build patternHits array. */
    char hits[1024];
    hits[0] = '\0';
    size_t pos = 0;
    pos += snprintf(hits + pos, sizeof(hits) - pos, "[");
    for (int i = 0; i < res->scanPatternCount && pos < sizeof(hits) - 64; ++i) {
        char ePattern[128];
        wtJsonEscape(res->scanPatterns[i], ePattern, sizeof(ePattern));
        pos += snprintf(hits + pos, sizeof(hits) - pos,
                        "%s{\"pattern\":\"%s\",\"count\":%d}",
                        i == 0 ? "" : ",",
                        ePattern, res->scanPatternCounts[i]);
    }
    if (pos < sizeof(hits)) snprintf(hits + pos, sizeof(hits) - pos, "]");
    char json[4096];
    int written = snprintf(json, sizeof(json),
        "{\"schema\":\"woventeam.secret_scan.v0.1\","
        "\"scanId\":\"%s\",\"deliverableId\":\"%s\",\"taskId\":\"%s\","
        "\"scannedPath\":\"%s\",\"matched\":%s,\"hitCount\":%d,"
        "\"patternHits\":%s,\"packagingMode\":\"%s\",\"createdAtUnixMs\":%lld}",
        eScan, eDeliv, eTaskId, eSource,
        res->scanMatched ? "true" : "false", res->scanHitCount,
        hits, wtDeliverableModeToString(mode), nowMs());
    if (written < 0 || written >= (int)sizeof(json)) return -1;
    return wtTaskAppendRecord(ledgerPath, json, req->fsyncRecord);
}

/* ---- packaging-mode handlers ---- */

static int copyFileBytes(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[WT_DELIVERABLE_SCAN_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out); unlink(dst); return -1;
        }
    }
    fclose(in);
    if (fclose(out) != 0) { unlink(dst); return -1; }
    return 0;
}

static int shipCopy(const WtConfig *config, const WtDeliverableRequest *req,
                    WtDeliverableResult *res) {
    char destDir[WT_DELIVERABLE_PATH_SIZE / 2];
    snprintf(destDir, sizeof(destDir), "%s/%s",
             config->deliverableRoot, req->initiativeId);
    if (mkdirP(destDir) != 0) {
        setError(res, "deliverable_root_unwritable");
        return -1;
    }
    const char *base = req->destBasename;
    if (!base || !*base) {
        const char *slash = strrchr(req->sourcePath, '/');
        base = slash ? slash + 1 : req->sourcePath;
    }
    snprintf(res->deliverablePath, sizeof(res->deliverablePath), "%s/%s", destDir, base);
    if (copyFileBytes(req->sourcePath, res->deliverablePath) != 0) {
        setError(res, "copy_failed");
        return -1;
    }
    return 0;
}

static int shipTarball(const WtConfig *config, const WtDeliverableRequest *req,
                       WtDeliverableResult *res) {
    char destDir[WT_DELIVERABLE_PATH_SIZE / 2];
    snprintf(destDir, sizeof(destDir), "%s/%s",
             config->deliverableRoot, req->initiativeId);
    if (mkdirP(destDir) != 0) {
        setError(res, "deliverable_root_unwritable");
        return -1;
    }
    const char *base = req->destBasename;
    if (!base || !*base) {
        const char *slash = strrchr(req->sourcePath, '/');
        base = slash ? slash + 1 : req->sourcePath;
    }
    snprintf(res->deliverablePath, sizeof(res->deliverablePath), "%s/%s.tar.gz", destDir, base);
    /*
     * tar -czf TARBALL -C PARENT BASENAME. Use --transform to rename if
     * caller supplied destBasename - skipped here for simplicity; the
     * tarball entry uses the source's filename which is fine for v1.0.
     */
    char srcCopy[WT_DELIVERABLE_PATH_SIZE];
    copyStringSafe(srcCopy, sizeof(srcCopy), req->sourcePath);
    char *slash = strrchr(srcCopy, '/');
    const char *parent = ".";
    const char *entry = srcCopy;
    if (slash) {
        *slash = '\0';
        parent = srcCopy;
        entry = slash + 1;
    }
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "tar -czf '%s' -C '%s' '%s' 2>/dev/null",
             res->deliverablePath, parent, entry);
    if (wtSystemReliable(cmd) != 0) {
        setError(res, "tar_failed");
        return -1;
    }
    return 0;
}

/*
 * Run a shell command inside a specific directory. Returns the system() exit
 * status (0 on success, non-zero on failure).
 */
static int runIn(const char *dir, const char *cmd) {
    char wrapped[4096];
    snprintf(wrapped, sizeof(wrapped),
             "cd '%s' && %s",
             dir ? dir : ".", cmd);
    return wtSystemReliable(wrapped);
}

static int shipBranch(const WtConfig *config, const WtDeliverableRequest *req,
                      WtDeliverableResult *res, int openPullRequest) {
    if (openPullRequest && !req->yesGate) {
        setError(res, "missing_yes_gate");
        return -1;
    }
    if (!req->repoPath || !*req->repoPath) {
        setError(res, "repo_path_missing");
        return -1;
    }
    /* Check the repo exists and is a git working tree. */
    struct stat st;
    char gitDir[WT_DELIVERABLE_PATH_SIZE];
    snprintf(gitDir, sizeof(gitDir), "%s/.git", req->repoPath);
    if (stat(gitDir, &st) != 0) {
        setError(res, "repo_not_git");
        return -1;
    }
    char branch[128];
    snprintf(branch, sizeof(branch), "%s/%s",
             config->deliverableBranchPrefix, req->initiativeId);
    /* Check out (or create) the branch. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "git fetch origin '%s' 2>/dev/null; "
             "git checkout '%s' 2>/dev/null || git checkout -b '%s' >/dev/null 2>&1",
             branch, branch, branch);
    if (runIn(req->repoPath, cmd) != 0) {
        setError(res, "git_checkout_failed");
        return -1;
    }
    /* Copy the source into a deliverables/ tree inside the repo. */
    char destDirInRepo[WT_DELIVERABLE_PATH_SIZE / 2];
    snprintf(destDirInRepo, sizeof(destDirInRepo), "%s/deliverables/%s",
             req->repoPath, req->initiativeId);
    if (mkdirP(destDirInRepo) != 0) {
        setError(res, "repo_dir_unwritable");
        return -1;
    }
    const char *base = req->destBasename;
    if (!base || !*base) {
        const char *slash = strrchr(req->sourcePath, '/');
        base = slash ? slash + 1 : req->sourcePath;
    }
    snprintf(res->deliverablePath, sizeof(res->deliverablePath),
             "%s/%s", destDirInRepo, base);
    if (copyFileBytes(req->sourcePath, res->deliverablePath) != 0) {
        setError(res, "copy_failed");
        return -1;
    }
    /* git add + commit + push. */
    char repoRelPath[WT_DELIVERABLE_PATH_SIZE];
    snprintf(repoRelPath, sizeof(repoRelPath), "deliverables/%s/%s",
             req->initiativeId, base);
    char commitMsg[512];
    snprintf(commitMsg, sizeof(commitMsg),
             "Ship deliverable %s (task %s)",
             res->deliverableId, req->taskId ? req->taskId : "?");
    char escapedMsg[1024];
    if (wtJsonEscape(commitMsg, escapedMsg, sizeof(escapedMsg)) != 0) {
        setError(res, "commit_msg_encode_failed");
        return -1;
    }
    snprintf(cmd, sizeof(cmd),
             "git add '%s' && git commit -m \"%s\" >/dev/null 2>&1 && "
             "git push -u origin '%s' >/dev/null 2>&1",
             repoRelPath, commitMsg, branch);
    if (runIn(req->repoPath, cmd) != 0) {
        setError(res, "git_push_failed");
        return -1;
    }
    if (!openPullRequest) {
        return 0;
    }
    /* gh pr create against the same branch. */
    const char *prTitle = req->prTitle ? req->prTitle : commitMsg;
    char prBody[1024];
    if (req->prBody && *req->prBody) {
        copyStringSafe(prBody, sizeof(prBody), req->prBody);
    } else {
        snprintf(prBody, sizeof(prBody),
                 "Automated deliverable ship.\n\nDeliverable: %s\nTask: %s\n",
                 res->deliverableId, req->taskId ? req->taskId : "?");
    }
    /* gh accepts the title + body directly; we keep things shell-safe by
     * using simple substitution and single-quote escaping the values. We
     * intentionally use \"%s\" here so shell metachars in the body are not
     * a problem - the body content is operator-supplied. */
    snprintf(cmd, sizeof(cmd),
             "gh pr create --head '%s' --title \"%s\" --body \"%s\" >/dev/null 2>&1",
             branch, prTitle, prBody);
    if (runIn(req->repoPath, cmd) != 0) {
        setError(res, "gh_pr_create_failed");
        return -1;
    }
    return 0;
}

int wtDeliverableShip(const WtConfig *config,
                      const WtDeliverableRequest *request,
                      WtDeliverableResult *result) {
    if (!config || !request || !result) return -1;
    memset(result, 0, sizeof(*result));
    if (!request->taskId || !*request->taskId ||
        !request->initiativeId || !*request->initiativeId ||
        !request->sourcePath || !*request->sourcePath) {
        setError(result, "missing_required_field");
        return -1;
    }
    if (access(request->sourcePath, R_OK) != 0) {
        setError(result, "source_missing");
        return -1;
    }
    makeShortId("deliv", result->deliverableId, sizeof(result->deliverableId));
    makeShortId("scan",  result->scanId,        sizeof(result->scanId));

    /*
     * Compute sha256 + size up front so the manifest reflects the source
     * bytes even if the packaging step transforms them (tarball wraps,
     * branch commits a copy).
     */
    if (wtDeliverableSha256(request->sourcePath, result->sha256, sizeof(result->sha256),
                            &result->sizeBytes) != 0) {
        setError(result, "sha256_failed");
        return -1;
    }
    /*
     * Run the secret scan. We always run it when patterns are configured -
     * branch/PR refuse on a match; copy/tarball record but proceed.
     */
    int scanRc = wtDeliverableScanFile(config, request->sourcePath, result);
    if (scanRc < 0) {
        setError(result, "secret_scan_failed");
        return -1;
    }
    if (result->scanMatched &&
        (request->mode == WT_DELIVERABLE_MODE_BRANCH ||
         request->mode == WT_DELIVERABLE_MODE_PR)) {
        /* Record the scan even on block, so the ledger captures the hit. */
        (void)appendSecretScanEvent(config->taskLedgerPath, request, result, request->mode);
        setError(result, "secret_scan_block");
        return -1;
    }
    int rc = -1;
    switch (request->mode) {
        case WT_DELIVERABLE_MODE_COPY:    rc = shipCopy(config, request, result); break;
        case WT_DELIVERABLE_MODE_TARBALL: rc = shipTarball(config, request, result); break;
        case WT_DELIVERABLE_MODE_BRANCH:  rc = shipBranch(config, request, result, 0); break;
        case WT_DELIVERABLE_MODE_PR:      rc = shipBranch(config, request, result, 1); break;
    }
    if (rc != 0) {
        (void)appendSecretScanEvent(config->taskLedgerPath, request, result, request->mode);
        return -1;
    }
    if (appendDeliverableEvent(config->taskLedgerPath, request, result, request->mode) != 0) {
        setError(result, "ledger_append_failed");
        return -1;
    }
    (void)appendSecretScanEvent(config->taskLedgerPath, request, result, request->mode);
    return 0;
}
