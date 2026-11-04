# Project build publication: integrity and recovery

This describes the release-commit contract for `ExecuteBuildPlan`.

## Ownership and concurrency

- A per-project `.tourmaline/.build.lock/` guards import freshness,
  derived registry updates, cooking and packing. A second builder of the
  **same project** fails early instead of interleaving catalog writes.
- Every invocation stages pak bytes inside its own
  `<publish>/.staging/build-<nonce>/` directory.
- A per-publish-directory `.publish.lock/` serializes immutable package
  landings, receipt publication and the `.tapp` commit. It also protects
  two different projects publishing into the same output directory.
- Resource and GPU memory lifetimes remain caller-owned. This protocol
  concerns files and catalogs only.

## Commit, validation and provenance

1. Cook and pack all output packages into the unique staging directory.
2. Validate/police the packages' identities and VFS ownership.
3. Deduplicate an existing content-addressed `<stem>-<fnv64>.pak` only
   after **byte-for-byte** comparison with the staged package.
4. Publish every required immutable pak by rename.
5. Write and publish immutable `<manifest>-<fnv64>.receipt` containing
   the manifest content hash and each package's name/hash.
6. Atomically replace the `.tapp` appmanifest. **This is the only
   live-release pointer.** The receipt is an independently verifiable
   audit artifact, not another release selector.
7. Reclaim orphaned paks only if all visible manifests parse and no other
   staging directory is present; retain paks referenced by ANY manifest.

A failed run before step 6 leaves the previous `.tapp` unchanged.
Orphaned immutable files and receipts are harmless until a verified
cleanup. A power failure cannot be modeled as a full durability guarantee
without platform-specific directory `fsync`/flush: the API promises an
atomic namespace change, not an unconditional power-loss durability SLA.

## Recovering from a killed/crashed builder

**Never automatically break a lock or wipe `.staging`.** A stalled
builder may be alive on another host or the PID may have been reused.

1. Stop all builds using the project or publication directory, including
   other machines sharing the same storage.
2. Inspect `<project>/.tourmaline/.build.lock/`,
   `<publish>/.publish.lock/`, and `<publish>/.staging/`. Confirm that
   no process still owns them, and make a backup/snapshot if uncertain.
3. Read all live `*.tapp` manifests and ensure every referenced pak is
   present. For addressed paks, verify their contents against the
   filename's FNV-1a64 digest. Check the corresponding immutable
   `*.receipt` for the manifest digest when available.
4. Only after those checks, remove orphaned `build-*` directories from
   `.staging`, then the stale `.publish.lock` and `.build.lock`
   directories. Do **not** remove referenced paks.
5. Restart the build. A successful build will deduplicate identical
   packages, commit a fresh manifest, and perform a conservative sweep.

If any manifest is malformed or any referenced package is missing or
corrupt, stop recovery and redeploy/rebuild from known-good artifacts.
Do not hand-edit `.tapp` or synthesize a receipt to bypass verification.

## Regression test coverage

`ProjectPipelineTests` covers project/publish lock rejection, interrupted
mid-set packing, previous-generation retention, byte mismatch rejection
on reuse, multi-manifest package retention, and immutable receipt output.
