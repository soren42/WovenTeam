# WovenTeam Threat Model

Updated: 2026-05-21

## Scope

WovenTeam is currently modeled as a single-tenant operator workbench on a trusted intranet. `wt-roomd` remains the policy and audit origin. Remote agents may execute work, but they do not become policy authorities; they authenticate, advertise capabilities, claim leased tasks, and append task lifecycle events through the daemon.

## Trust Boundaries

- Local operator origin: loopback requests are treated as operator-origin for the current TCP daemon. A future Unix-domain socket should replace this compatibility shortcut.
- Remote agent origin: bearer token plus IP allow-list is required for `/api/host-capabilities`, `/api/remote-claim`, and `/api/remote-task-event`.
- Operator bearer origin: mutating operator endpoints require either local origin or a bearer token with role `operator`.
- Audit origin: all token, host capability, lease, reclaim, cancel, autonomy, artifact, and deliverable actions are append-only JSONL records.

## Current Controls

- Bearer tokens carry `tokenId`, `role`, `subject`, `issuedAtUnixMs`, and `ttlSeconds`.
- `/api/auth-token/revoke` appends a revoke event; validation rejects the next use with `token_revoked`.
- `remoteAllowedIps` constrains bearer-authenticated remote calls before token validation.
- Host capabilities are recorded as `woventeam.host_event.v0.1` records and exposed through `/api/hosts`.
- `executionHost` pins a task to one advertised host. Capability mismatches reject with `capability_unmet`.
- Lease expiry and reclaim use the existing append-only reclaim path, so network partitions surface as stuck leases and recover without a separate remote state machine.

## Out Of Scope

- Multi-tenant isolation.
- Internet-exposed daemon operation without mTLS or a reverse proxy.
- Secret storage hardening for token material in the JSONL ledger.
- Strong cryptographic token generation. Sprint 4 uses opaque bearer values sufficient for the local integration substrate; production hardening should use OS CSPRNG material.
- Remote filesystem sandboxing beyond the adapter/tool policy already recorded on tasks.

## Required Hardening Before Cross-Network Use

- Put `wt-roomd` behind mTLS or add native TLS client certificate validation.
- Move token secrets out of the audit ledger, storing only hashed token material.
- Replace loopback TCP operator bypass with a Unix-domain socket or explicit local operator session.
- Add per-role token issuance policy so remote agents cannot request operator-class tokens through local bootstrap paths.
