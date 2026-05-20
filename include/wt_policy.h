/*
 * wt_policy.h - Central policy evaluator for Phase 2 Sprint 5.
 *
 * Returns a single accept/deny decision for a proposed task package given the
 * current config + projection state. Callers (the /api/task-package and
 * /api/task-request handlers, plus the CLI) get a classified reason code so
 * they can render a useful error message AND append a durable policy_decision
 * record to the ledger for audit history.
 *
 * The evaluator is intentionally a pure function over its inputs: it does NOT
 * read the ledger or rebuild the projection; the caller is responsible for
 * preparing those views. That keeps unit-testing tractable and keeps the HTTP
 * handlers in charge of when projection rebuilds happen.
 */
#ifndef WT_POLICY_H
#define WT_POLICY_H

#include "wt_config.h"
#include "wt_task_store.h"

#include <stddef.h>

/* Reason codes are kept short + stable so the ledger and UI can both depend on
 * them. Keep in sync with docs/api/task-ledger-v0.1.md. */
#define WT_POLICY_REASON_SIZE 48
#define WT_POLICY_MESSAGE_SIZE 256

typedef struct {
    int allowed;                                 /* 1 = accept, 0 = deny */
    char reason[WT_POLICY_REASON_SIZE];          /* e.g. "vendor_blocked" */
    char message[WT_POLICY_MESSAGE_SIZE];        /* human-readable detail */
} WtPolicyDecision;

typedef struct {
    const char *taskId;
    const char *assignedRole;
    const char *assignedAgent;
    const char *modelId;
    const char *toolProfile;
    const char *initiativeId;
    const char *autonomyLevel;
    const char *autonomyScope;
    const char *autonomyNetwork;
    const char *autonomyCredentialClass;
    long autonomyTtlSeconds;
    long autonomyMaxWallClockSeconds;
    int autonomyRequiresCleanWorktree;
    long long autonomyCreatedAtUnixMs;
    long long autonomyRevokedAtUnixMs;
    long maxTokens;
    long long nowUnixMs;
    /* Pre-computed projection totals so the evaluator stays a pure function. */
    long long dayWindowAllocatedTokens;
    long long monthWindowAllocatedTokens;
    long long initiativeAllocatedTokens;
    long long modelFamilyAllocatedTokens;
    int activeTasksForAgent;
    int activeTasksForInitiative;
} WtPolicyInput;

/*
 * Evaluate the proposed task package. Output decision.reason/message are
 * always populated. allowed=1 means accept; allowed=0 means deny.
 */
void wtPolicyEvaluateTaskPackage(const WtConfig *config, const WtPolicyInput *input,
                                 WtPolicyDecision *decision);

/*
 * Build the comma-separated modelId family prefix list expected by
 * blockedVendors. modelFamilyBuffer is filled with the family prefix
 * ("openai", "anthropic", "google", "deepseek", ...) parsed from the supplied
 * modelId. Returns 0 on success, -1 when modelId has no '/' separator.
 */
int wtPolicyExtractModelFamily(const char *modelId, char *modelFamilyBuffer, size_t bufferSize);

/*
 * Return 1 if vendor (prefix before the slash) appears in the blockedVendors
 * CSV string. The match is case-sensitive and exact per token.
 */
int wtPolicyVendorIsBlocked(const char *blockedVendors, const char *modelFamily);

#endif
