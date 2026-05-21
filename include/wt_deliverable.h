#ifndef WT_DELIVERABLE_H
#define WT_DELIVERABLE_H

#include <stdbool.h>
#include <stddef.h>

#include "wt_config.h"

/*
 * Phase 3 Sprint 3 - deliverables pipeline.
 *
 * Ships an accepted artifact out of the per-task workspace through one of
 * four packaging modes. Each ship appends two records to the task ledger:
 *
 *   - woventeam.deliverable.v0.1  (always)
 *   - woventeam.secret_scan.v0.1  (when secretScanPatternsFile is configured)
 *
 * Branch + PR modes refuse to ship on a positive secret-scan match; copy and
 * tarball modes scan and record but do not block, so the audit trail captures
 * the hit either way.
 */

typedef enum {
    WT_DELIVERABLE_MODE_COPY = 0,
    WT_DELIVERABLE_MODE_TARBALL = 1,
    WT_DELIVERABLE_MODE_BRANCH = 2,
    WT_DELIVERABLE_MODE_PR = 3
} WtDeliverableMode;

#define WT_DELIVERABLE_ID_SIZE 64
#define WT_DELIVERABLE_SHA_SIZE 80   /* hex sha256 + spare */
#define WT_DELIVERABLE_PATH_SIZE 1024
#define WT_DELIVERABLE_REASON_SIZE 192

typedef struct {
    char deliverableId[WT_DELIVERABLE_ID_SIZE];
    char deliverablePath[WT_DELIVERABLE_PATH_SIZE];
    char sha256[WT_DELIVERABLE_SHA_SIZE];
    long long sizeBytes;
    char scanId[WT_DELIVERABLE_ID_SIZE];
    int scanRan;
    int scanMatched;
    int scanHitCount;
    /* Up to 8 distinct pattern hits with their match counts. */
    int scanPatternCount;
    char scanPatterns[8][64];
    int scanPatternCounts[8];
    char errorReason[WT_DELIVERABLE_REASON_SIZE];
} WtDeliverableResult;

typedef struct {
    /* taskId, initiativeId, sourcePath are required. */
    const char *taskId;
    const char *initiativeId;
    const char *sourcePath;
    /* destination basename (e.g. "result.md"); when NULL we derive from sourcePath. */
    const char *destBasename;
    WtDeliverableMode mode;
    const char *reviewer;
    const char *createdBy;
    /* may be NULL or empty when not superseding. */
    const char *supersedes;
    /* Branch/PR modes operate inside this git repo. NULL means cwd. */
    const char *repoPath;
    /* Required true for WT_DELIVERABLE_MODE_PR; ignored for the rest. */
    bool yesGate;
    /* PR title/body for WT_DELIVERABLE_MODE_PR. May be NULL for defaults. */
    const char *prTitle;
    const char *prBody;
    /* When non-NULL records the autonomy provenance in the deliverable manifest. */
    int autonomyElevated;
    const char *autonomyLevel;
    bool fsyncRecord;
} WtDeliverableRequest;

int wtDeliverableModeFromString(const char *s, WtDeliverableMode *out);
const char *wtDeliverableModeToString(WtDeliverableMode mode);

/*
 * Run the configured pre-ship secret scan against a single file. Populates
 * result->scanRan, result->scanMatched, result->scanHitCount, and the
 * scanPatterns[] / scanPatternCounts[] arrays. Returns 0 on a successful
 * scan (regardless of whether matches were found) and -1 only on hard
 * errors (e.g. file unreadable, patterns file malformed).
 */
int wtDeliverableScanFile(const WtConfig *config, const char *path,
                          WtDeliverableResult *result);

/*
 * Compute the SHA-256 of a file. Returns 0 on success, -1 on failure.
 * Writes the lowercase hex digest into hexOut[hexOutSize].
 */
int wtDeliverableSha256(const char *path, char *hexOut, size_t hexOutSize,
                        long long *sizeBytesOut);

/*
 * Ship an accepted artifact. On success appends the deliverable (and
 * optionally secret_scan) ledger records and fills out `result` with the
 * deliverable id, path, sha256, size, and scan summary.
 *
 * Returns 0 on success, -1 on failure (result->errorReason carries the
 * classified reason, e.g. "secret_scan_block", "missing_yes_gate",
 * "git_push_failed", "source_missing").
 */
int wtDeliverableShip(const WtConfig *config,
                      const WtDeliverableRequest *request,
                      WtDeliverableResult *result);

#endif
