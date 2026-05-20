/*
 * wt_notify.h - Phase 3 Sprint 1 notification routing.
 *
 * Resolves a webhook URL from a KEY=URL file (default config/slack_webhook.txt)
 * by key, then POSTs a small JSON payload. The helper exists primarily to
 * push policy denials, cancel events, and escalate milestones to Slack
 * without forcing every call site to re-implement the lookup.
 *
 * Implementation note: shells out to `curl` rather than embedding an HTTPS
 * client. That keeps the daemon free of OpenSSL / mbedTLS, matches the
 * existing pattern (the codex/claude/gemini adapters all shell out), and lets
 * the operator audit egress with the same tooling they use for everything
 * else.
 */
#ifndef WT_NOTIFY_H
#define WT_NOTIFY_H

#include "wt_config.h"

/*
 * Send a notification. key selects which line in slackWebhookFile to use.
 * title is a short headline (e.g. "policy denial"); message is the body.
 * Returns 0 when the POST was attempted (regardless of webhook response),
 * non-zero when no key resolved or curl failed to invoke.
 *
 * Override behavior: when the WT_NOTIFICATION_OVERRIDE_URL env var is set,
 * the helper POSTs to that URL irrespective of the key. Used by the
 * integration test against a local mock listener.
 */
int wtNotifySend(const WtConfig *config, const char *key,
                 const char *title, const char *message);

#endif
