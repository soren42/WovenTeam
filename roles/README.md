# WovenTeam Agent Role Definitions

## Overview

This directory contains the **fundamental instruction files** (ROLE.md) for every agent role in the WovenTeam framework. Each ROLE.md is a model-agnostic system prompt — the behavioral contract injected into whichever AI model is assigned to that role at runtime.

**Any model can fill any role.** Role assignment is governed by a three-layer architecture:

|Layer          |Location                            |Consumed By                    |Purpose                                                                                             |
|---------------|------------------------------------|-------------------------------|----------------------------------------------------------------------------------------------------|
|**Behavioral** |`roles/<category>/ROLE_<role_id>.md`|The AI model (as system prompt)|Identity, responsibilities, constraints, output format, collaboration rules                         |
|**Operational**|`roles/<category>/<role_id>.yaml`   |The orchestrator/spawner       |Model routing (preferred_model, fallback), cost limits, tool permissions, meeting rules, stage gates|
|**Capability** |`models/<vendor>/<model>.yaml`      |The orchestrator/spawner       |Model identity, cost/token metrics, operational status, **0–10 suitability scores for every role**  |

The orchestrator uses all three to make routing decisions: the model YAML says how well a model *can* do the job, the role YAML says which model is *preferred*, and the ROLE.md tells the selected model *how to do the job*.

See `models/README.md` for the model profile schema and suitability scoring system.

## Architecture

```
roles/
├── README.md                          ← You are here
├── leadership/
│   ├── program_manager.yaml           ← Orchestrator config (model prefs, cost, tools)
│   ├── ROLE_program_manager.md        ← System prompt (identity, behavior, constraints)
│   ├── project_manager.yaml
│   └── ROLE_project_manager.md
├── architecture/
│   ├── software_architect.yaml
│   ├── ROLE_software_architect.md
│   ├── systems_architect.yaml
│   ├── ROLE_systems_architect.md
│   ├── mockup_artist.yaml
│   ├── ROLE_mockup_artist.md
│   ├── graphic_artist.yaml
│   └── ROLE_graphic_artist.md
├── implementation/
│   ├── frontend_dev.yaml
│   ├── ROLE_frontend_dev.md
│   ├── backend_dev.yaml
│   ├── ROLE_backend_dev.md
│   ├── database_engineer.yaml
│   └── ROLE_database_engineer.md
├── quality/
│   ├── code_reviewer.yaml
│   ├── ROLE_code_reviewer.md
│   ├── tester.yaml
│   ├── ROLE_tester.md
│   ├── performance_engineer.yaml
│   └── ROLE_performance_engineer.md
├── operations/
│   ├── systems_administrator.yaml
│   ├── ROLE_systems_administrator.md
│   ├── network_administrator.yaml
│   ├── ROLE_network_administrator.md
│   ├── database_administrator.yaml
│   └── ROLE_database_administrator.md
└── delivery/
    ├── integration_specialist.yaml
    ├── ROLE_integration_specialist.md
    ├── deployment_engineer.yaml
    ├── ROLE_deployment_engineer.md
    ├── technical_writer.yaml
    └── ROLE_technical_writer.md
```

## How ROLE.md Files Are Used

### At Agent Spawn Time

The orchestrator performs the following sequence:

1. **Role selection** — The task or initiative phase determines which role is needed.
1. **Read role YAML** — Get `preferred_model`, `fallback_model`, `minimum_suitability`, and `cost_limit`.
1. **Model evaluation** — For each candidate model, read the model YAML:
   a. Check `status` (must be `active`)
   b. Check `suitability[role_id]` score (must meet `minimum_suitability` threshold)
   c. Check budget (`current_month_usage_usd` vs `monthly_ceiling_usd`)
   d. Estimate task cost against role `cost_limit`
1. **Model selection** — Select the highest-suitability model that passes all checks. If `preferred_model` meets all criteria, prefer it as tiebreaker.
1. **Prompt assembly** — The ROLE.md content is injected as the system prompt. Task-specific context (initiative brief, existing artifacts, constraints) is appended per the instantiation contract.
1. **Agent execution** — The model operates under the ROLE.md behavioral contract for the duration of the task.

### Separation of Concerns

|File             |Purpose                                                                                                |Consumed By                    |
|-----------------|-------------------------------------------------------------------------------------------------------|-------------------------------|
|`ROLE_*.md`      |Identity, behavior, constraints, output format, collaboration rules                                    |The AI model (as system prompt)|
|`*.yaml` (role)  |Model routing (preferred/fallback), cost limits, tool permissions, meeting rules, stage gates          |The orchestrator/spawner       |
|`models/*/*.yaml`|Model identity, cost/token rates, operational status, budget tracking, 0–10 suitability scores per role|The orchestrator/spawner       |

This separation means you can:

- Change *which model* fills a role → edit the role YAML’s `preferred_model` or the model YAML’s suitability scores
- Change *what the role does* → edit the ROLE.md
- Change *model cost/capacity/status* → edit the model YAML
- All three independently, without touching the other layers

### Model Agnosticism

ROLE.md files are written to be effective regardless of which vendor model executes them. They:

- Define behavior in plain, imperative language — no model-specific syntax
- Specify output formats as JSON schemas the orchestrator can parse
- Include explicit constraints and boundaries, not implicit assumptions about model capabilities
- Reference tools by abstract capability name, not vendor-specific function calls

## Role Categories

### Leadership & Coordination

Roles that manage scope, schedule, resources, risk, and cross-initiative alignment. These roles consume status reports and produce directives, plans, and escalations.

### Architecture & Design

Roles that define *what* gets built and *how it looks*. These roles produce specifications, diagrams, wireframes, and visual assets that downstream roles consume.

### Implementation

Roles that write code. These roles consume specifications and produce source code, tests, and deployment configurations.

### Quality

Roles that verify correctness, performance, and standards compliance. These roles consume artifacts from implementation and produce reviews, test results, and optimization recommendations.

### Operations

Roles that manage running infrastructure. These roles have elevated permissions on live systems and produce configuration changes, monitoring setups, and incident responses.

### Delivery

Roles that move artifacts from development to production and document the result. These roles own CI/CD pipelines, release processes, and all written documentation.

## Versioning

Each ROLE.md includes a `version` field in its header. When modifying a role definition, increment the version and note the change. The orchestrator logs which ROLE.md version was active for each agent instance, enabling audit and rollback.

## Adding a New Role

1. Determine the appropriate category subdirectory.
1. Create `ROLE_<role_id>.md` following the template structure used by existing roles.
1. Create `<role_id>.yaml` with orchestrator configuration (model prefs, cost, tools, permissions, meetings, gates).
1. Register the role_id in `conf/routing.yaml` so the orchestrator can assign it.
1. Update this README if the new role introduces a new category.
