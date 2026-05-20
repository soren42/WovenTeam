/*
 * wt_policy.c - Central policy evaluator (Phase 2 Sprint 5).
 *
 * Pure function over inputs. All projection / ledger reads happen in the
 * caller; this module just classifies the decision.
 */
#include "wt_policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int wtPolicyExtractModelFamily(const char *modelId, char *modelFamilyBuffer, size_t bufferSize) {
    if (!modelId || !modelFamilyBuffer || bufferSize == 0) {
        return -1;
    }
    modelFamilyBuffer[0] = '\0';
    const char *slash = strchr(modelId, '/');
    if (!slash || slash == modelId) {
        return -1;
    }
    size_t length = (size_t)(slash - modelId);
    if (length >= bufferSize) {
        length = bufferSize - 1;
    }
    memcpy(modelFamilyBuffer, modelId, length);
    modelFamilyBuffer[length] = '\0';
    return 0;
}

int wtPolicyVendorIsBlocked(const char *blockedVendors, const char *modelFamily) {
    if (!blockedVendors || blockedVendors[0] == '\0' || !modelFamily || modelFamily[0] == '\0') {
        return 0;
    }
    /* Walk the CSV without copying. Each token is whitespace-trimmed before
     * comparison so "deepseek, xai" is interpreted the same as "deepseek,xai". */
    const char *cursor = blockedVendors;
    size_t familyLength = strlen(modelFamily);
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
        if (!*cursor) break;
        const char *tokenStart = cursor;
        while (*cursor && *cursor != ',') cursor++;
        const char *tokenEnd = cursor;
        while (tokenEnd > tokenStart && (tokenEnd[-1] == ' ' || tokenEnd[-1] == '\t')) tokenEnd--;
        size_t tokenLength = (size_t)(tokenEnd - tokenStart);
        if (tokenLength == familyLength && strncmp(tokenStart, modelFamily, familyLength) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * Map agent name to the modelId prefix it accepts. Mirrors the routing logic
 * in wt_roomd; kept local so the evaluator does not pull in the daemon's
 * static helpers.
 */
static int modelMatchesAgentLocal(const char *agent, const char *modelFamily) {
    if (!agent || !modelFamily || !modelFamily[0]) {
        return 1; /* nothing to check */
    }
    if (strcmp(agent, "claude") == 0) return strcmp(modelFamily, "anthropic") == 0;
    if (strcmp(agent, "gemini") == 0) return strcmp(modelFamily, "google") == 0;
    if (strcmp(agent, "chatgpt") == 0) return strcmp(modelFamily, "openai") == 0;
    return 1;
}

static void setDecision(WtPolicyDecision *decision, int allowed, const char *reason, const char *message) {
    decision->allowed = allowed;
    snprintf(decision->reason, sizeof(decision->reason), "%s", reason ? reason : "");
    snprintf(decision->message, sizeof(decision->message), "%s", message ? message : "");
}

static int autonomyLevelIsValid(const char *level) {
    return level && (
        strcmp(level, "observe") == 0 ||
        strcmp(level, "ask-each") == 0 ||
        strcmp(level, "ask-batch") == 0 ||
        strcmp(level, "autonomous") == 0);
}

static int autonomyScopeCoversAdapter(const char *scope) {
    if (!scope || scope[0] == '\0') {
        return 0;
    }
    return strstr(scope, "workspace") != NULL ||
           strstr(scope, "adapter") != NULL ||
           strstr(scope, "*") != NULL;
}

void wtPolicyEvaluateTaskPackage(const WtConfig *config, const WtPolicyInput *input,
                                 WtPolicyDecision *decision) {
    if (!config || !input || !decision) {
        if (decision) setDecision(decision, 0, "internal", "policy evaluator received null inputs");
        return;
    }
    char modelFamily[64];
    int hasFamily = (wtPolicyExtractModelFamily(input->modelId, modelFamily, sizeof(modelFamily)) == 0);

    /*
     * Phase 3 Sprint 2: autonomy grants fail closed. Only autonomous tasks need
     * a grant; lower levels run without elevated adapter flags.
     */
    if (input->autonomyLevel && input->autonomyLevel[0]) {
        if (!autonomyLevelIsValid(input->autonomyLevel)) {
            setDecision(decision, 0, "autonomy_required", "unknown autonomyLevel");
            return;
        }
        if (strcmp(input->autonomyLevel, "autonomous") == 0) {
            if (input->autonomyRevokedAtUnixMs > 0) {
                setDecision(decision, 0, "autonomy_revoked", "autonomy grant was revoked");
                return;
            }
            if (input->autonomyTtlSeconds <= 0 ||
                !autonomyScopeCoversAdapter(input->autonomyScope)) {
                setDecision(decision, 0, "autonomy_required",
                            "autonomous tasks require a positive ttlSeconds and adapter/workspace scope");
                return;
            }
            long long grantStart = input->autonomyCreatedAtUnixMs > 0 ?
                                   input->autonomyCreatedAtUnixMs : input->nowUnixMs;
            if (input->nowUnixMs > grantStart + input->autonomyTtlSeconds * 1000LL) {
                setDecision(decision, 0, "autonomy_expired", "autonomy grant ttl has expired");
                return;
            }
        }
    }

    /*
     * 1. Blocked vendor check. Runs first so an immediately-rejected request
     *    cannot leak through later checks. The audit ledger captures the
     *    modelId and reason for each denial.
     */
    if (hasFamily && wtPolicyVendorIsBlocked(config->blockedVendors, modelFamily)) {
        char detail[WT_POLICY_MESSAGE_SIZE];
        snprintf(detail, sizeof(detail),
                 "vendor '%s' is on the blockedVendors allowlist (modelId=%s).",
                 modelFamily, input->modelId);
        setDecision(decision, 0, "vendor_blocked", detail);
        return;
    }

    /*
     * 2. Model-agent compatibility. Operators can pin modelId at task creation;
     *    if the routed agent's family does not match the chosen model we deny
     *    before the ledger ever sees the package.
     */
    if (hasFamily && input->assignedAgent && input->assignedAgent[0] &&
        strcmp(input->assignedAgent, "router") != 0 && strcmp(input->assignedAgent, "all") != 0 &&
        !modelMatchesAgentLocal(input->assignedAgent, modelFamily)) {
        char detail[WT_POLICY_MESSAGE_SIZE];
        snprintf(detail, sizeof(detail),
                 "model '%s' does not match assigned agent '%s'.",
                 input->modelId, input->assignedAgent);
        setDecision(decision, 0, "model_agent_mismatch", detail);
        return;
    }

    /*
     * 3. Token budgets. The order is intentional: most-specific wins first so
     *    the denial reason maps to the most actionable knob. Each check is
     *    skipped when its corresponding config knob is 0 (disabled).
     */
    if (input->maxTokens > 0 && config->tokenTelemetryEnabled) {
        if (input->initiativeId && input->initiativeId[0] &&
            config->tokenBudgetPerInitiative > 0 &&
            input->initiativeAllocatedTokens + input->maxTokens > config->tokenBudgetPerInitiative) {
            char detail[WT_POLICY_MESSAGE_SIZE];
            snprintf(detail, sizeof(detail),
                     "initiative '%s' would exceed tokenBudgetPerInitiative "
                     "(allocated=%lld + request=%ld > cap=%ld).",
                     input->initiativeId,
                     input->initiativeAllocatedTokens, input->maxTokens,
                     config->tokenBudgetPerInitiative);
            setDecision(decision, 0, "initiative_budget", detail);
            return;
        }
        if (hasFamily && config->tokenBudgetPerModelFamily > 0 &&
            input->modelFamilyAllocatedTokens + input->maxTokens > config->tokenBudgetPerModelFamily) {
            char detail[WT_POLICY_MESSAGE_SIZE];
            snprintf(detail, sizeof(detail),
                     "model family '%s' would exceed tokenBudgetPerModelFamily "
                     "(allocated=%lld + request=%ld > cap=%ld).",
                     modelFamily, input->modelFamilyAllocatedTokens,
                     input->maxTokens, config->tokenBudgetPerModelFamily);
            setDecision(decision, 0, "model_family_budget", detail);
            return;
        }
        if (config->tokenDailyBudget > 0 &&
            input->dayWindowAllocatedTokens + input->maxTokens > config->tokenDailyBudget) {
            setDecision(decision, 0, "daily_budget", "daily token budget exceeded");
            return;
        }
        if (config->tokenMonthlyBudget > 0 &&
            input->monthWindowAllocatedTokens + input->maxTokens > config->tokenMonthlyBudget) {
            setDecision(decision, 0, "monthly_budget", "monthly token budget exceeded");
            return;
        }
    }

    /*
     * 4. Capacity caps. These mirror the existing enforceCapacity behavior in
     *    wt_roomd; keeping them inside the evaluator means /api/task-request
     *    cannot drift out of sync with /api/task-package.
     */
    if (config->maxActiveTasksPerAgent > 0 &&
        input->activeTasksForAgent >= config->maxActiveTasksPerAgent) {
        char detail[WT_POLICY_MESSAGE_SIZE];
        snprintf(detail, sizeof(detail),
                 "agent '%s' is at capacity (active=%d cap=%d).",
                 input->assignedAgent ? input->assignedAgent : "?",
                 input->activeTasksForAgent, config->maxActiveTasksPerAgent);
        setDecision(decision, 0, "capacity_agent", detail);
        return;
    }
    if (input->initiativeId && input->initiativeId[0] &&
        config->maxTasksPerInitiative > 0 &&
        input->activeTasksForInitiative >= config->maxTasksPerInitiative) {
        char detail[WT_POLICY_MESSAGE_SIZE];
        snprintf(detail, sizeof(detail),
                 "initiative '%s' is at task-count capacity (active=%d cap=%d).",
                 input->initiativeId,
                 input->activeTasksForInitiative, config->maxTasksPerInitiative);
        setDecision(decision, 0, "capacity_initiative", detail);
        return;
    }

    setDecision(decision, 1, "ok", "policy accepted");
}
