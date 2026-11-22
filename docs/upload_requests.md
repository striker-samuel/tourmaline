# Upload request and timeline ticket — initial integration

Stacked on PR #395. Opt-in API only; existing void UploadDataToBuffer
calls continue unchanged. Device-local staging uploads return a small
shard/sequence ticket; polls use the existing transfer Submitter timeline.
No per-upload fences, promises, heap allocations, or forced flushes.

Lifecycle: Rejected (invalid input, stale buffer, oversized range,
unsupported host-mappable path); Recorded (copied and recorded in staging);
Submitted (batch timeline published); Completed (timeline value reached);
Failed (submission returned no valid point or the poll counter failed).

**Constraints:**
- A request must use a device-local buffer with transfer-dst usage.
  Host-visible direct uploads intentionally remain on the legacy path.
- The admission result does not certify every possible VMA failure; some
  existing streamer paths use VK_CHECK and abort on fatal allocation errors.
- Completion means the transfer timeline signaled, not that a later
  consuming graphics pass completed. The existing cross-queue dependency
  remains responsible for visibility.
- Poll can be conservative: it checks the newest shard timeline point, so
  an older upload may show Submitted longer than necessary.
- The shard sequence has no long-term archival retention. Tickets are
  scoped to the ResourceManager lifetime and should be polled promptly.
- No changes to the renderer's material publication callback in this
  foundational PR; it can adopt checked admission in a follow-up.
- Build, CTest and device testing remain required. GPU device loss and
  submission failure behavior need real-device fault injection before
  adopting this as authoritative recovery.
