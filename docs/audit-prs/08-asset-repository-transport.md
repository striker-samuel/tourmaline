# Secure shared asset repository access and authorization

> **STATUS: IN PROGRESS.** Integrated onto per-user ACL-enabled master; real TLS gateway validation still required.

**Finding:** L-11  
**Priority:** P0

## Problem and intended change
Harden non-loopback transport and token handling, verify endpoint permissions, reject traversal and symlink escapes, and document safe TLS deployment.

## Source files to inspect / potentially modify
- `tools/shared_asset_repository.py`
- `tools/shared_asset_server.py`
- `tools/configure_asset_client.py`
- `tools/tests/test_shared_asset_repository.py`
- `docs/asset_repository_manual.md`

## Acceptance criteria
Remote credentials and assets are not sent in cleartext under supported secure deployment; negative auth and path tests cover server endpoints.

## Implementation checklist
- [ ] Trace all current call sites and confirm design invariants against current `master`.
- [ ] Implement the smallest compatible correction in source code.
- [ ] Add positive, negative, and integration regression tests.
- [ ] Verify existing fallback and caller-owned resource lifetime behavior.
- [ ] Run relevant native tests, and GPU/manual tests where applicable.
- [ ] Document compatibility, benchmarking, and before/after evidence.

## Scope boundary
No generic GPU memory or automatic lifetime manager; the caller remains responsible for memory. Do not enable experimental rendering paths as part of this fix.
