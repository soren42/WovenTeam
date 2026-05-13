# WovenTeam Model Profiles

## Overview

This directory contains **model profile YAML files** — operational and capability descriptions for every AI model available to the WovenTeam orchestrator. Each profile describes what the model *is*: its identity, cost structure, token capacity, operational status, and a **suitability matrix** rating its fitness for each of the 18 agent roles on a 0–10 scale.

Model profiles are the counterpart to ROLE.md files. Together they answer:

- **ROLE.md:** “What does this job require?” (behavioral contract, injected as system prompt)
- **Model YAML:** “How well can this model do that job?” (capabilities, cost, suitability scores)

The orchestrator uses both to make routing decisions.

## Directory Structure

```
models/
├── README.md                      ← You are here
├── anthropic/
│   ├── claude_opus.yaml           # Anthropic Claude Opus (flagship reasoning)
│   └── claude_sonnet.yaml         # Anthropic Claude Sonnet (balanced cost/capability)
├── openai/
│   ├── gpt_4o.yaml               # OpenAI GPT-4o (multimodal, broad knowledge)
│   └── gpt_o3.yaml               # OpenAI o3 (deep reasoning, chain-of-thought)
├── google/
│   ├── gemini_pro.yaml            # Google Gemini Pro (large context, quantitative)
│   └── gemini_flash.yaml          # Google Gemini Flash (fast, cost-effective)
├── xai/
│   └── grok.yaml                  # xAI Grok (real-time data, creative)
├── qwen/
│   └── qwen_max.yaml             # Alibaba Qwen Max (cost-effective, multilingual)
└── deepseek/
    └── deepseek_coder.yaml        # DeepSeek Coder (BLOCKED — see trust policy)
```

## Profile Schema

Every model profile YAML follows this schema:

```yaml
# ─── Identity ───
model_id: vendor/model_name           # Unique identifier used in routing
vendor: vendor_name                    # Vendor organization
model_name: Human-readable name       # Display name
version: model_version                # Model version string
cli_tool: command_name                 # Vendor CLI binary name
cli_auth: auth_command                 # Authentication command

# ─── Operational Status ───
status: active | standby | blocked | deprecated
status_reason: ""                      # Explanation if not active
last_verified: ISO 8601                # Last time status was confirmed

# ─── Cost & Capacity ───
cost:
  input_per_1k_tokens: 0.00           # USD per 1K input tokens
  output_per_1k_tokens: 0.00          # USD per 1K output tokens
  max_context_tokens: 0               # Maximum context window
  max_output_tokens: 0                # Maximum output per response
  rate_limit_rpm: 0                   # Requests per minute (0 = unknown/unlimited)
  billing_model: per_token | subscription | hybrid

budget:
  daily_ceiling_usd: 0.00             # Max spend per day across all roles
  monthly_ceiling_usd: 0.00           # Max spend per month
  current_month_usage_usd: 0.00       # Tracked by orchestrator at runtime
  token_allotment_monthly: 0          # Total tokens budgeted per month (0 = unlimited)
  tokens_used_this_month: 0           # Tracked by orchestrator at runtime

# ─── Capabilities ───
capabilities:
  multimodal_input: true | false      # Can process images/audio/video
  multimodal_output: true | false     # Can generate images/audio
  code_execution: true | false        # Can execute code in sandbox
  web_search: true | false            # Has real-time web access
  file_handling: true | false         # Can read/write files
  long_context: true | false          # >100K token context window
  structured_output: true | false     # Reliable JSON/schema output

# ─── Role Suitability Matrix ───
# 0 = completely unsuitable
# 1-3 = poor fit (use only if nothing else available)
# 4-5 = acceptable (can do the job, not ideal)
# 6-7 = good fit (solid performance expected)
# 8-9 = strong fit (among the best choices)
# 10 = exceptional fit (best-in-class for this role)
suitability:
  program_manager: 0
  project_manager: 0
  software_architect: 0
  systems_architect: 0
  mockup_artist: 0
  graphic_artist: 0
  frontend_dev: 0
  backend_dev: 0
  database_engineer: 0
  code_reviewer: 0
  tester: 0
  performance_engineer: 0
  systems_administrator: 0
  network_administrator: 0
  database_administrator: 0
  integration_specialist: 0
  deployment_engineer: 0
  technical_writer: 0

# ─── Notes ───
notes: |
  Free-text notes about the model's strengths, weaknesses,
  quirks, and observed behavior in WovenTeam contexts.
```

## Suitability Score Scale

|Score|Meaning              |Routing Implication                                        |
|-----|---------------------|-----------------------------------------------------------|
|0    |Completely unsuitable|Never assign this role                                     |
|1–3  |Poor fit             |Assign only as last resort; expect quality issues          |
|4–5  |Acceptable           |Can handle routine tasks in this role; not for complex work|
|6–7  |Good fit             |Solid choice for most tasks in this role                   |
|8–9  |Strong fit           |Among the best options; prefer for complex tasks           |
|10   |Exceptional          |Best-in-class; default choice when budget allows           |

### How Scores Are Determined

Initial scores are based on:

1. **Observed performance** during the WovenTeam strategy/development passes (Passes 1–4)
1. **Known model characteristics** (reasoning depth, code quality, context window, multimodal capability)
1. **CEO assessment** from direct experience with each model

Scores should be **updated empirically** as agents execute real tasks. The orchestrator logs task success/failure, output quality ratings, and token efficiency per model-role combination. Over time, suitability scores converge on measured reality rather than initial estimates.

## How the Orchestrator Uses Model Profiles

### Routing Decision Flow

```
1. Task arrives needing role X
2. Read role YAML: get preferred_model, fallback_model, cost_limit
3. For each available model:
   a. Read model YAML: check status (must be 'active')
   b. Check suitability[role_X] score
   c. Check budget (current_month_usage vs monthly_ceiling)
   d. Check cost (estimated task cost vs role cost_limit)
4. Select model with highest suitability score that:
   - Has status: active
   - Is within budget
   - Meets minimum suitability threshold (configurable, default: 4)
5. If preferred_model meets all criteria, prefer it (tiebreaker)
6. If no model meets threshold, escalate to PM/CEO
7. Inject ROLE.md as system prompt, spawn agent
```

### Runtime Telemetry

The orchestrator updates these fields in real-time (not stored in the YAML file on disk, but tracked in the SQLite `model_telemetry` table):

- `current_month_usage_usd` — Running cost total
- `tokens_used_this_month` — Running token total
- `uptime_status` — Last health check result (ping vendor CLI)
- `avg_response_time_ms` — Rolling average response latency
- `task_success_rate` — Percentage of tasks completed without error or revision
- `suitability_observed` — Empirically adjusted suitability scores (separate from the baseline in YAML)

## Updating Model Profiles

### When to Update

- **New model released** → Create a new profile YAML. Set initial suitability scores conservatively (5s across the board) and refine after testing.
- **Model deprecated** → Set `status: deprecated`. Do not delete the file — historical routing decisions reference it.
- **Pricing changes** → Update cost fields. Recalculate budget ceilings if necessary.
- **Trust policy change** → Update `status` field (e.g., `blocked` for DeepSeek per MPD reveal decision).
- **Observed performance shift** → Adjust suitability scores with a note in the changelog.

### Versioning

Model profiles are version-controlled via Git. Each profile includes a `profile_version` field and internal changelog. Bump the version on every substantive change.
