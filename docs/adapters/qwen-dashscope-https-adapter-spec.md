# DashScope HTTPS Adapter Specification (Plan B)
**Document:** `docs/adapters/qwen-dashscope-https-adapter-spec.md`
**Issue:** #8 — Qwen Adapter Implementation
**Workgroup:** CLI/Backend (ChatGPT) • Documentation (Qwen) • Review (Gemini)
**Author:** Qwen Max (Documentation Workgroup)
**Status:** Draft v0.3 — Gemini-approved; ready for implementation PR

> **v0.3 revision notes (Claude, 2026-05-13):** incorporates Gemini's
> Approved-with-Revisions verdict on v0.2. Adds the bubblewrap-jail
> reference to §4A per the updated Issue #8 acceptance criteria;
> resolves the four §8 open questions per Gemini's directives
> (streaming → Phase 2, `bc` → acceptable, token cache → orchestration/
> compliance layer not wrapper, endpoints → env var with safe
> default); adds a new §9 consolidating Phase 2 deferrals so the
> handoff point is unambiguous.
>
> **v0.2 revision notes (Claude, 2026-05-13):** corrected `~/woventeam/...`
> paths to `/woventeam/...`; removed a §7.3 example that named the blocked
> DeepSeek model; reconciled the §2.3 output envelope with the existing
> `docs/api/schema-v0.1.json` by nesting it as the `payload`; trimmed the
> §7.2 `qwen_max.yaml` rewrite to additive fields only (existing schema
> in `models/README.md` is authoritative); replaced "T3Chat Emulation"
> with a clearer phrasing; removed the contradictory `pip3 install
> dashscope` line. The §4A Architecture Note records how this shell
> adapter slots beneath the C dispatcher being tracked in Issue #8.

---

## 1. Overview

This specification defines a POSIX-compliant shell wrapper (`qwen_dashscope_wrapper.sh`) that enables Qwen models (via Alibaba Cloud DashScope) to participate in the WovenTeam framework as a first-class vendor adapter. 

Since no official Qwen agentic CLI binary exists, this adapter implements the **CLI Adapter Contract** using HTTPS calls to the DashScope API, wrapped in a shell interface that is indistinguishable from CLI-based adapters to the orchestration layer.

### 1.1 Design Goals
| Goal | Implementation |
|------|---------------|
| **Contract Compliance** | Mimics `vendor_wrapper.sh` interface: stdin prompt → stdout JSON response |
| **Per-agent credentials** | Dedicated DashScope API key per agent instance; no shared credentials |
| **Cost Transparency** | Token usage and estimated cost logged to audit trail per call |
| **Fallback Ready** | Graceful degradation if DashScope is unavailable; integrates with routing policy |
| **Minimal Dependencies** | POSIX shell + `curl` + `jq` only; no Python SDK required |

### 1.2 Scope
- ✅ Adapter for Qwen-Max, Qwen-Plus, Qwen-Turbo via DashScope API
- ✅ Authentication via environment variable or vault reference
- ✅ Structured JSON I/O matching `docs/api/schema-v0.1.json`
- ✅ Audit logging to `/woventeam/logs/dashscope_*.log`
- ✅ Cost estimation based on DashScope pricing tiers
- ❌ No streaming response support (Phase 2)
- ❌ No tool-use/function-calling beyond basic JSON schema (Phase 2)

---

## 2. Adapter Contract

### 2.1 Invocation Interface
```bash
# Usage
bin/adapters/qwen_dashscope_wrapper.sh \
  --role <role_id> \
  --initiative <initiative_id> \
  --task <task_id> \
  --prompt-file <path_to_prompt.md> \
  --output-file <path_to_output.json> \
  [--model <model_name>] \
  [--max-tokens <int>] \
  [--temperature <float>]

# Environment Variables (required)
DASHSCOPE_API_KEY="sk-..."          # Dedicated key per agent instance
DASHSCOPE_BASE_URL="https://dashscope.aliyuncs.com"  # Optional, defaults to prod
WV_AGENT_ID="backend_dev_03"         # For audit logging
WV_INITIATIVE_ID="esp32_initiative"  # For cost tracking
```

### 2.2 Input Format (Prompt File)
The `--prompt-file` contains a Markdown document with structured sections:
```markdown
# ROLE: backend_dev
# INITIATIVE: esp32_initiative
# TASK: task_042

## System Prompt
You are a backend developer agent. Output valid JSON per schema.

## Context
- Existing artifacts: src/bridge/stub.c, docs/esp32_protocol.md
- Constraints: No external dependencies, C99 compatible

## Task Description
Implement MQTT subscription handler for ESP32 mesh bridge.

## Acceptance Criteria
1. Handler subscribes to esp32/# topic
2. Forwards messages to mesh routing table
3. Includes error handling for disconnects
```

### 2.3 Output Format (JSON Response)

The adapter writes a single JSON object to `--output-file`. The outer
envelope conforms to `docs/api/schema-v0.1.json` (Phase 0 envelope —
`id`, `ts`, `from`, `type`, `initiative`, `payload`); the adapter-
specific fields live inside `payload`:

```json
{
  "id": "msg_qwen_20260507_001",
  "ts": 1762512600,
  "from": "qwen",
  "type": "result",
  "initiative": "esp32_initiative",
  "payload": {
    "schema_version": "0.1",
    "correlation_id": "task_042",
    "vendor": "qwen",
    "model": "qwen-max",
    "status": "complete",
    "response": {
      "content": "Generated code or structured output here",
      "finish_reason": "stop"
    },
    "usage": {
      "prompt_tokens": 342,
      "completion_tokens": 1205,
      "total_tokens": 1547
    },
    "cost_estimate_usd": 0.0077,
    "warnings": [],
    "errors": []
  }
}
```

---

## 3. Implementation Details

### 3.1 Authentication & Credential Management
```bash
# Credential resolution order:
# 1. $DASHSCOPE_API_KEY environment variable
# 2. Vault reference: $VAULT_DIR/qwen/api_key (file containing key)
# 3. Fail with error if neither present

resolve_api_key() {
    if [[ -n "$DASHSCOPE_API_KEY" ]]; then
        echo "$DASHSCOPE_API_KEY"
    elif [[ -f "$VAULT_DIR/qwen/api_key" ]]; then
        cat "$VAULT_DIR/qwen/api_key"
    else
        echo "[ERROR] No DashScope API key found" >&2
        return 1
    fi
}
```

### 3.2 API Request Construction
```bash
# DashScope API endpoint: POST /api/v1/services/aigc/text-generation/generation
# Headers:
#   Authorization: Bearer $API_KEY
#   Content-Type: application/json
#   X-DashScope-WorkSpace: $WV_AGENT_ID  # For usage attribution

build_request_payload() {
    local prompt_file="$1"
    local model="${2:-qwen-max}"
    local max_tokens="${3:-2048}"
    
    # Extract system prompt and user content from markdown
    local system_prompt=$(extract_section "$prompt_file" "System Prompt")
    local user_content=$(extract_section "$prompt_file" "Task Description")
    
    cat <<EOF
{
  "model": "$model",
  "input": {
    "messages": [
      {"role": "system", "content": $(json_escape "$system_prompt")},
      {"role": "user", "content": $(json_escape "$user_content")}
    ]
  },
  "parameters": {
    "result_format": "message",
    "max_tokens": $max_tokens,
    "temperature": ${TEMPERATURE:-0.1}
  }
}
EOF
}
```

### 3.3 Response Handling & Normalization
```bash
parse_dashscope_response() {
    local raw_response="$1"
    
    # Extract usage and content from DashScope response format
    jq -r '
      {
        schema_version: "0.1",
        message_id: ("msg_qwen_" + (.request_id // "unknown")),
        correlation_id: $correlation_id,
        timestamp: (now | strftime("%Y-%m-%dT%H:%M:%SZ")),
        vendor: "qwen",
        model: .model,
        status: (if .output.choices[0].finish_reason == "stop" then "complete" else "error" end),
        response: {
          content: .output.choices[0].message.content,
          finish_reason: .output.choices[0].finish_reason
        },
        usage: .usage,
        cost_estimate_usd: (
          (.usage.output_tokens * 0.000004) +  # qwen-max pricing example
          (.usage.input_tokens * 0.000001)
        ),
        warnings: [],
        errors: (if .code then [.message] else [] end)
      }
    ' --arg correlation_id "$CORRELATION_ID"
}
```

### 3.4 Audit Logging
Every invocation appends to `/woventeam/logs/dashscope_YYYYMMDD.log`:
```log
[2026-05-07T10:30:00Z] AGENT=backend_dev_03 INITIATIVE=esp32_initiative TASK=task_042 MODEL=qwen-max PROMPT_TOKENS=342 COMPLETION_TOKENS=1205 COST_USD=0.0077 STATUS=complete
```

---

## 4. Error Handling & Fallback

### 4.1 Error Codes Mapping
| DashScope Error | Adapter Behavior |
|----------------|-----------------|
| `InvalidApiKey` | Exit 1, log auth failure, trigger credential renewal alert |
| `QuotaExhausted` | Exit 2, log budget warning, notify orchestrator |
| `RateLimitExceeded` | Retry with exponential backoff (max 3 attempts) |
| `ModelNotFound` | Exit 3, log model config error, fallback to `qwen-plus` if configured |
| `NetworkError` | Retry once, then exit 4 with diagnostic info |

### 4.2 Fallback Integration
The adapter respects the `--fallback-model` flag from the orchestrator:
```bash
# If primary model fails and fallback is specified:
if [[ "$EXIT_CODE" -ne 0 && -n "$FALLBACK_MODEL" ]]; then
    log_event "FALLBACK_TRIGGERED" "primary=$MODEL fallback=$FALLBACK_MODEL"
    MODEL="$FALLBACK_MODEL"
    retry_request
fi
```

---

## 4A. Architecture Note (relation to Issue #8)

Issue #8 frames per-vendor adapters as C functions compiled into
`wt-agent` at `src/worker/adapters/<vendor>.c`. This spec describes a
shell wrapper at `bin/adapters/qwen_dashscope_wrapper.sh`. These are
complementary, not conflicting:

- The C-side adapter in `wt-agent` is a thin **dispatcher**. For
  vendors with an installed agentic CLI (Anthropic `claude`, OpenAI
  `codex`), it `execve()`s that CLI directly with the role's system
  prompt and a scoped working directory.
- For vendors without a native CLI (Qwen via DashScope, Gemini via
  Generative Language API until a real CLI ships), the dispatcher
  `execve()`s a shell wrapper script — this document specifies one
  such script for Qwen.

Issue #8's acceptance criteria record this two-tier arrangement: C
dispatcher + per-vendor adapter that may itself be a CLI or a shell
wrapper.

**Sandboxing constraint (per Gemini's Issue #8 review):** every Tier 2
invocation — including this shell wrapper — MUST run inside the
`bubblewrap` (or `firejail`) jail constructed by the C dispatcher. The
jail provides:

- No network access except the specific DashScope API endpoint
  configured via `DASHSCOPE_BASE_URL` (egress whitelisted per-vendor at
  jail construction time)
- Read-only access to the system filesystem
- Read-write access only to the task workspace declared in
  `payload.output_paths`

The adapter does not know it is jailed; it just `exec`s `curl`/`jq`/
`bc` and writes to the file paths it has been given. The dispatcher is
the trust boundary, not the adapter. See Issue #8 acceptance criteria
#1 for the canonical specification.

---

## 5. Cost Estimation Logic

DashScope pricing (example; update with current rates):
| Model | Input ($/1K tokens) | Output ($/1K tokens) |
|-------|---------------------|----------------------|
| qwen-turbo | 0.0005 | 0.001 |
| qwen-plus | 0.001 | 0.002 |
| qwen-max | 0.001 | 0.004 |

Calculation in adapter:
```bash
calculate_cost() {
    local model="$1" input_tokens="$2" output_tokens="$3"
    case "$model" in
        qwen-turbo) echo "scale=6; ($input_tokens * 0.0000005) + ($output_tokens * 0.000001)" | bc ;;
        qwen-plus)  echo "scale=6; ($input_tokens * 0.000001) + ($output_tokens * 0.000002)" | bc ;;
        qwen-max)   echo "scale=6; ($input_tokens * 0.000001) + ($output_tokens * 0.000004)" | bc ;;
        *) echo "0.00" ;;
    esac
}
```

---

## 6. Testing & Validation

### 6.1 Unit Test Cases
```bash
# Test 1: Basic invocation
./qwen_dashscope_wrapper.sh --role tester --initiative test --task t001 \
  --prompt-file tests/prompt_basic.md --output-file /tmp/out.json
# Expected: valid JSON output, exit 0

# Test 2: Missing API key
unset DASHSCOPE_API_KEY
./qwen_dashscope_wrapper.sh ... 2>&1 | grep "No DashScope API key"
# Expected: exit 1, error message

# Test 3: Cost calculation accuracy
# Mock response with known token counts, verify cost_estimate_usd matches formula
```

### 6.2 Integration Test with Orchestrator
```bash
# Simulate task assignment via Redis
redis-cli PUBLISH agent.tester.task '{
  "task_id":"test_001",
  "role":"tester",
  "vendor":"qwen",
  "model":"qwen-plus",
  "prompt_file":"/tmp/test_prompt.md"
}'

# Verify adapter consumes task, writes result, updates SQLite
sqlite3 /woventeam/woventeam.db "SELECT status, cost_usd FROM tasks WHERE task_id='test_001';"
# Expected: status='complete', cost_usd > 0
```

---

## 7. Deployment & Configuration

### 7.1 Prerequisites on xenon.akoria.net
```bash
# Required packages
sudo apt install curl jq bc

# Directory structure (most already exist on xenon)
mkdir -p /woventeam/{logs,vault/qwen}
mkdir -p /woventeam/repos/WovenTeam/bin/adapters
```

The Python `dashscope` SDK is intentionally NOT a dependency: §1.1
specifies POSIX shell + `curl` + `jq` only. Phase 2 may revisit if
streaming or function-calling become must-haves.

### 7.2 Model Profile Update (additive only)

The existing schema in `models/README.md` is authoritative. Do NOT
rename or duplicate fields (e.g. don't add `display_name` when
`model_name` already exists; don't add `cost_per_1k_input` when
`cost.input_per_1k_tokens` already exists). Only add fields that are
genuinely new and adapter-specific. Suggested additions for
`models/qwen/qwen_max.yaml`:

```yaml
# already present and authoritative — do NOT change:
#   model_id, vendor, model_name, version, cli_tool, cli_auth,
#   api_model_string, status, cost.*, capabilities.*, suitability.*

# additive — new fields for the DashScope adapter:
adapter:
  type: "https_shell_wrapper"          # vs. "vendor_cli" for claude/codex
  script: "bin/adapters/qwen_dashscope_wrapper.sh"
  api_endpoint: "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation"
  auth_env_var: "DASHSCOPE_API_KEY"
  auth_vault_path: "vault/qwen/api_key"
```

`cli_tool: qwen` in the current profile should be set to `null` since
no official Qwen CLI is shipped; the adapter script takes its place.
Fallback model preferences belong in the per-role YAML
(`roles/<category>/<role_id>.yaml`), not the model profile, so keep
those out of `qwen_max.yaml`.

### 7.3 Routing Policy Integration

Routing decisions live in the per-role YAMLs under
`roles/<category>/<role_id>.yaml` (see `roles/role_config_template.yaml`),
not in a separate `conf/routing.yaml`. The first concrete example is
`roles/delivery/technical_writer.yaml` (PR #7, commit `39b819f`):

```yaml
routing:
  preferred_model: "anthropic/claude-opus"
  fallback_model: "qwen/qwen-max"
  minimum_suitability: 6
  model_override: null
```

When this adapter ships, no routing edit is required — the existing
fallback already names `qwen/qwen-max`. The `preferred_model` for
`technical_writer` can be flipped back to `qwen/qwen-max` in that one
file once Gemini reviews the adapter and CI passes.

**DeepSeek is `status: blocked` in the live registry** (see
`/woventeam/docs/audit/model-manage-remove-deepseek-20260513-055932.md`)
and must not appear in any routing example.

---

## 8. Resolutions (from Gemini's v0.2 review, 2026-05-13)

The four open questions raised in the v0.1 draft were resolved by
Gemini's review of v0.2:

1. **Streaming (`incremental_output`):** **Deferred to Phase 2.**
   Phase 1 focuses on the vertical slice of single-shot task
   completion. Streaming requires additional state machinery in the
   wrapper and on the consuming side; not worth the complexity until
   the basic loop is proven.

2. **`bc` as a dependency for cost math:** **Accepted.** The adapter
   already requires `curl` and `jq`; `bc` is a similarly POSIX-stable
   addition and provides the precision needed for cost telemetry.
   Integer math with scaling was considered and rejected as
   unnecessarily error-prone for the small numbers involved.

3. **Local prompt token cache:** **Deferred, and explicitly NOT a
   wrapper concern.** Per-prompt token caching belongs at the
   Orchestration / Compliance layer (above the wrapper), not in any
   single vendor adapter. The wrapper reports tokens consumed per
   call; the orchestrator decides whether to cache. See
   [[project-runtime-vs-inference-separation]] for why caching at
   the adapter is the wrong layer.

4. **Region-specific endpoints (`dashscope-intl.aliyuncs.com` etc.):**
   **Use the `DASHSCOPE_BASE_URL` environment variable** (already
   defined in §2.1) with the production endpoint as the safe default.
   Operators in regions requiring an alternate endpoint set the env
   var per-agent-instance; no code change.

---

## 9. Phase 2 Deferrals (consolidated)

Items intentionally NOT in scope for Phase 1, but explicitly
acknowledged so Phase 2 has a clean pickup list:

| Item                              | Layer where it should land                  | Trigger to revisit                         |
|-----------------------------------|---------------------------------------------|--------------------------------------------|
| DashScope `incremental_output` streaming | Adapter (this wrapper) + consumer in wt-agent | When long-running roles need progress feedback |
| Local prompt token cache          | Orchestration / Compliance layer            | When prompt repetition justifies the lookup cost |
| Tool-use / function-calling beyond basic JSON | Adapter + dispatcher                | When a role needs DashScope function-calling specifically |
| DashScope Python SDK              | Adapter (replace `curl`+`jq` path)          | Only if Phase 2 features above force the issue |

The Phase 2 trigger for each is documented separately from the
implementation — each row above is a follow-up that can be filed as
its own issue when the trigger condition is met.

---

## 10. Next Steps

1. **ChatGPT (CLI/Backend)**: Implement the C dispatcher per Issue #8
   acceptance criteria, then implement `bin/adapters/qwen_dashscope_wrapper.sh`
   per this spec under the bubblewrap jail it constructs.
2. **Gemini (Review)**: Final Green Light review of the implementation
   PRs against the adapter contract and Issue #8 acceptance criteria
   (sandboxing, envelope, path pinning).
3. **Qwen (Docs)**: Author `docs/dev-guide/03-extending-vendor-support.md`
   with this adapter as the worked example for future vendor wrappers.
4. **CEO (Jason)**: Approve DashScope API key provisioning workflow for
   dedicated per-agent credentials (`/woventeam/vault/qwen/api_key` or
   `DASHSCOPE_API_KEY` env).

---

*WovenTeam • Solarian • xenon.akoria.net*
*Document Version: 0.3 • Last Updated: 2026-05-13*
*Authorship: Qwen Max (v0.1 draft) • Claude Opus (v0.2 review corrections + v0.3 Gemini-resolution incorporation)*
*Review status: Gemini-approved 🟡 → Green Light pending implementation PRs*
