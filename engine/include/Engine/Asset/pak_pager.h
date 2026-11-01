// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE ASYNC PAK PAGER (Phase 5 of the consolidated content
// architecture — docs/pak_vnext.md): page-granular, asynchronous
// reads over a pak FILE with a bounded residency pool. The container
// contract it builds on is SMP8 (PakReader::OpenFile's header/footer/
// TOC-only mount + the per-entry contentCrc); the pager adds the
// scheduling + residency layers:
//
//   - ONE background worker thread over a persistent file handle;
//     every fetch is a positional page read (verify + LZ4-decode)
//     performed OUTSIDE the pool lock.
//   - Demand reads (Read/ReadAsync) jump queued PREFETCH entries;
//     prefetch is the early-work lane, demand owns latency.
//   - In-flight/resident pages are DEDUPLICATED by slot: concurrent
//     requests for the same page attach to one disk read.
//   - A BOUNDED byte-budget pool with LRU eviction — only UNPINNED
//     resident pages evict; a PinnedPage holds its page in the pool
//     for its lifetime (the pool never invalidates a live span).
//   - Cancellation for QUEUED prefetches (never an in-flight read);
//     a demand read on a cancelled page un-cancels it.
//   - Per-page failures are LOUD and terminal: a corrupt page (the
//     contentCrc mismatch) or an IO error fails that page with its
//     path — its neighbors stay servable (SMP8's selective
//   integrity).
//
// LIFETIMES (the CPU page / GPU asset / scene-instance split): the
// pager owns only CPU PAGES. PinnedPage spans are valid until the
// PinnedPage dies AND the pager itself must outlive every PinnedPage
// (the engine's standard owner-outlives-handles rule — a pin is a
// raw view, never a share). GPU uploads from a pinned page copy on
// submit (BulkCopyBuffer stages synchronously); a pager torn down
// after a frame's submits is safe exactly like the image-backed lane.
//
// Main-thread calls (Read/Acquire/State/Cancel/Stats) and the worker
// share one mutex; the worker's DISK IO runs unlocked, so a slow read
// never blocks a poll or a pin release.

#include <Engine/Asset/pak.h>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace smvk {

class PakPager {
public:
	// The page lifecycle. Missing = no record; Queued/Loading = the
	// worker owns the latency; Resident = bytes in the pool (subject
	// to eviction unless pinned); Failed = terminal (the path is in
	// PageRecord::error — a corrupt page or an IO failure).
	enum class PageState : uint8_t {
		Missing = 0,
		Queued,
		Loading,
		Resident,
		Failed,
	};

	struct Stats {
		size_t residentPages = 0;
		uint64_t residentBytes = 0;
		size_t queuedRequests = 0;   // waiting for the worker
		size_t inFlight = 0;         // being read right now
		uint64_t diskReads = 0;      // positional page reads performed
		uint64_t bytesRead = 0;      // bytes pulled from disk
		uint64_t evictions = 0;      // unpinned LRU pages dropped
		size_t failedPages = 0;
		size_t pinnedPages = 0;      // resident pages holding pins
		size_t waitedPages = 0;      // pages with demand waiters
	};

	// A resident page PINNED in the pool: the bytes stay valid until
	// the PinnedPage dies (and the pager outlives it — see the header
	// block). Move-only.
	class PinnedPage {
	public:
		PinnedPage() = default;
		~PinnedPage();
		PinnedPage(PinnedPage&& other) noexcept;
		PinnedPage& operator=(PinnedPage&& other) noexcept;
		PinnedPage(const PinnedPage&) = delete;
		PinnedPage& operator=(const PinnedPage&) = delete;
		[[nodiscard]] const uint8_t* data() const
			{ return m_bytes.data(); }
		[[nodiscard]] size_t size() const { return m_bytes.size(); }
		[[nodiscard]] std::span<const uint8_t> bytes() const
			{ return m_bytes; }
		explicit operator bool() const { return !m_bytes.empty(); }
	private:
		friend class PakPager;
		std::span<const uint8_t> m_bytes;
		PakPager* m_pager = nullptr;
		uint32_t m_slot = 0;
	};

	// A page request: the slot names the page, the id names THIS
	// request instance (Cancel). Copyable — a value, not a handle.
	struct Request {
		uint32_t slot = UINT32_MAX;
		uint64_t id = 0;
	};

	// Opens the pak file (the SMP8 small mount: header + footer +
	// TOC only — no blob bytes). Valid()/OpenError() report the
	// outcome; an invalid pager refuses every read loudly.
	explicit PakPager(std::string pakPath);
	~PakPager();
	PakPager(const PakPager&) = delete;
	PakPager& operator=(const PakPager&) = delete;

	[[nodiscard]] bool Valid() const { return m_reader.has_value(); }
	[[nodiscard]] const std::string& OpenError() const
		{ return m_openError; }
	// The TOC-resident view: entries / Find / FindBySlot / the mesh
	// plan — NO blob bytes (every payload flows through the pager).
	[[nodiscard]] const PakReader& Reader() const { return *m_reader; }

	// ---- the read surface ------------------------------------------------
	// Demand read (BLOCKING): the page resident + pinned. Null page +
	// err on failure (a corrupt page names its path — the SMP8 lazy
	// verification, surfaced by the pager).
	[[nodiscard]] PinnedPage Read(const PakTocEntry& entry,
		std::string& err);
	// Async demand: enqueue (jumping queued prefetches); ATTACH to an
	// in-flight/resident page (dedup by slot). Acquire blocks for the
	// pinned result; State polls; Cancel cancels.
	[[nodiscard]] Request ReadAsync(const PakTocEntry& entry);
	// Early work (the low-priority lane): enqueues unless the page is
	// already queued/loading/resident. PREFETCH IS NEVER A GUARANTEE —
	// demand reads jump the queue and eviction may drop the page
	// before a pin; the later Read is the contract (the design's
	// "prefetch starts work early, only a later synchronization
	// guarantees readiness").
	void Prefetch(const PakTocEntry& entry);
	// Blocks until the request's page is Resident (pinned) or Failed.
	[[nodiscard]] PinnedPage Acquire(const Request& r,
		std::string& err);
	// The request's page state RIGHT NOW (poll).
	[[nodiscard]] PageState State(const Request& r) const;
	// Cancels a QUEUED PREFETCH (never an in-flight read, never a
	// demand request): the page record dies unless a demand arrived.
	[[nodiscard]] bool Cancel(const Request& r);

	// ---- the pool ---------------------------------------------------------
	// The residency budget (unpinned LRU pages evict above it; pinned
	// pages never evict). Default 64 MiB. Changing it trims
	// immediately.
	void SetBudget(uint64_t bytes);
	[[nodiscard]] uint64_t Budget() const;
	[[nodiscard]] Stats PageStats() const;

private:
	struct PageRecord {
		uint32_t slot = 0;
		std::vector<uint8_t> bytes;
		PageState state = PageState::Missing;
		std::string error;
		uint32_t pins = 0;
		// Outstanding DEMAND requests attached to this page. A page
		// with waiters is EVICTION-IMMUNE (with pins): the install ->
		// Acquire-pins window must never evict the page out from
		// under the thread about to pin it (a budget below one page
		// would otherwise install -> evict -> re-enqueue -> install
		// forever - the livelock). ReadAsync-without-Acquire leaks a
		// waiter (one page held resident) - a caller bug, documented.
		uint32_t waiters = 0;
		uint64_t touch = 0;       // LRU order (bump on install + pin)
		bool prefetchCancelled = false;
		bool demandArrived = false;
	};

	void WorkerLoop();
	bool EnqueueLocked(uint32_t slot, bool demand, Request& out);
	PinnedPage PinLocked(PageRecord& rec);
	void Unpin(uint32_t slot);
	void EvictOverBudgetLocked();
	PageRecord* FindRecordLocked(uint32_t slot) const;

	std::string m_pakPath;
	std::string m_openError;
	std::optional<PakReader> m_reader; // the TOC view (file-backed)
	std::fstream m_file;               // the WORKER's private read handle

	mutable std::mutex m_mutex;
	std::condition_variable m_cvWorker;
	std::condition_variable m_cvDone;
	std::deque<uint32_t> m_demandQueue;   // slots, demand-first
	std::deque<uint32_t> m_prefetchQueue; // slots, the early-work lane
	std::map<uint32_t, std::unique_ptr<PageRecord>> m_pages;
	uint64_t m_nextRequestId = 1;
	uint64_t m_touchSeq = 1;
	uint64_t m_budget = 64ull * 1024ull * 1024ull;
	uint64_t m_diskReads = 0;
	uint64_t m_bytesRead = 0;
	uint64_t m_evictions = 0;
	bool m_stop = false;
	std::thread m_worker;
};

} // namespace smvk
