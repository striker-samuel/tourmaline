// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE ASYNC PAK PAGER - the implementation (see pak_pager.h for the
// design record + docs/pak_vnext.md for the container contract).

#include <Engine/Asset/pak_pager.h>

#include <Engine/Asset/compression.h>

#include <cstring>
#include <fstream>

namespace smvk {

// ---------------------------------------------------------------------------
// PinnedPage
// ---------------------------------------------------------------------------

PakPager::PinnedPage::~PinnedPage()
{
	if (m_pager)
		m_pager->Unpin(m_slot);
}

PakPager::PinnedPage::PinnedPage(PinnedPage&& other) noexcept
	: m_bytes(other.m_bytes)
	, m_pager(other.m_pager)
	, m_slot(other.m_slot)
{
	other.m_pager = nullptr;
	other.m_slot = 0;
	other.m_bytes = {};
}

PakPager::PinnedPage& PakPager::PinnedPage::operator=(
	PinnedPage&& other) noexcept
{
	if (this != &other) {
		if (m_pager)
			m_pager->Unpin(m_slot);
		m_bytes = other.m_bytes;
		m_pager = other.m_pager;
		m_slot = other.m_slot;
		other.m_pager = nullptr;
		other.m_slot = 0;
		other.m_bytes = {};
	}
	return *this;
}

// ---------------------------------------------------------------------------
// Construction / teardown
// ---------------------------------------------------------------------------

PakPager::PakPager(std::string pakPath)
	: m_pakPath(std::move(pakPath))
{
	std::string err;
	m_reader = PakReader::OpenFile(m_pakPath, err);
	if (!m_reader) {
		m_openError = err;
		return;
	}
	// The WORKER's private read handle (positional page reads; only
	// the worker touches it — no lock needed around the stream).
	m_file.open(m_pakPath,
		std::ios::in | std::ios::out | std::ios::binary);
	if (!m_file)
		m_file.open(m_pakPath, std::ios::in | std::ios::binary);
	if (!m_file) {
		m_openError = "cannot open pak file for paging: " + m_pakPath;
		m_reader.reset();
		return;
	}
	m_worker = std::thread([this] { WorkerLoop(); });
}

PakPager::~PakPager()
{
	if (m_worker.joinable()) {
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_cvWorker.notify_all();
		m_worker.join();
	}
}

// ---------------------------------------------------------------------------
// The read surface
// ---------------------------------------------------------------------------

PakPager::PinnedPage PakPager::Read(const PakTocEntry& entry,
	std::string& err)
{
	err.clear();
	Request r = ReadAsync(entry);
	return Acquire(r, err);
}

PakPager::Request PakPager::ReadAsync(const PakTocEntry& entry)
{
	Request r;
	if (!m_reader) {
		r.slot = UINT32_MAX;
		return r;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!EnqueueLocked(entry.slot, /*demand=*/true, r))
		r.slot = UINT32_MAX;
	// WAKE THE WORKER: an enqueued page with no notify is a deadlock
	// (the worker's predicate wait sleeps until some OTHER notify —
	// Acquire would block forever on a page the worker never saw).
	m_cvWorker.notify_one();
	return r;
}

void PakPager::Prefetch(const PakTocEntry& entry)
{
	if (!m_reader)
		return;
	std::lock_guard<std::mutex> lock(m_mutex);
	Request ignored;
	const bool queued = EnqueueLocked(entry.slot, /*demand=*/false,
		ignored);
	if (queued)
		m_cvWorker.notify_one();
}

PakPager::PinnedPage PakPager::Acquire(const Request& r,
	std::string& err)
{
	err.clear();
	if (!m_reader || r.slot == UINT32_MAX) {
		err = m_reader ? "the page request is invalid (a null request)"
			: "the pager failed to open its pak: " + m_openError;
		return {};
	}
	std::unique_lock<std::mutex> lock(m_mutex);
	for (;;) {
		// Re-look-up EVERY wake: a raw record pointer must never be
		// held across the wait — another thread's install can push the
		// pool over budget and evict this (unpinned) page before this
		// waiter pins it.
		PageRecord* rec = FindRecordLocked(r.slot);
		if (!rec) {
			// Missing: a cancelled prefetch after eviction, or the
			// record died while queued. Re-enqueue ON DEMAND — the
			// later Read is the contract (Acquire of a dead record
			// re-does the work rather than failing silently).
			Request again;
			if (!EnqueueLocked(r.slot, /*demand=*/true, again)) {
				err = "the page request could not be re-enqueued";
				return {};
			}
			m_cvWorker.notify_one();
			rec = FindRecordLocked(r.slot);
			if (!rec) {
				err = "the page record could not be established";
				return {};
			}
		}
		if (rec->state == PageState::Resident) {
			PinnedPage pin = PinLocked(*rec); // pin in the same lock
			                                  // scope: no eviction can
			                                  // intervene
			if (rec->waiters)
				--rec->waiters;
			return pin;
		}
		if (rec->state == PageState::Failed) {
			if (rec->waiters)
				--rec->waiters;
			err = rec->error;
			return {};
		}
		m_cvDone.wait(lock);
	}
}

PakPager::PageState PakPager::State(const Request& r) const
{
	if (!m_reader || r.slot == UINT32_MAX)
		return PageState::Missing;
	std::lock_guard<std::mutex> lock(m_mutex);
	const PageRecord* rec = FindRecordLocked(r.slot);
	return rec ? rec->state : PageState::Missing;
}

bool PakPager::Cancel(const Request& r)
{
	if (!m_reader || r.slot == UINT32_MAX)
		return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	PageRecord* rec = FindRecordLocked(r.slot);
	if (!rec || rec->state != PageState::Queued)
		return false; // in-flight/resident/failed/missing: no cancel
	// A demand request never cancels (the contract: demand owns
	// latency). A QUEUED PREFETCH dies unless a demand arrived.
	if (rec->demandArrived)
		return false;
	rec->prefetchCancelled = true;
	// The record stays until the worker pops it (it checks the flag);
	// nothing else can hold a pin on a queued page.
	return true;
}

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

void PakPager::SetBudget(uint64_t bytes)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_budget = bytes;
	EvictOverBudgetLocked();
}

uint64_t PakPager::Budget() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_budget;
}

PakPager::Stats PakPager::PageStats() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	Stats s;
	s.queuedRequests = m_demandQueue.size() + m_prefetchQueue.size();
	s.diskReads = m_diskReads;
	s.bytesRead = m_bytesRead;
	s.evictions = m_evictions;
	uint64_t residentBytes = 0;
	for (const auto& [slot, rec] : m_pages) {
		(void)slot;
		if (rec->state == PageState::Resident) {
			++s.residentPages;
			residentBytes += rec->bytes.size();
			if (rec->pins)
				++s.pinnedPages;
		}
		else if (rec->state == PageState::Loading)
			++s.inFlight;
		else if (rec->state == PageState::Failed)
			++s.failedPages;
		if (rec->waiters)
			++s.waitedPages;
	}
	s.residentBytes = residentBytes;
	return s;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

bool PakPager::EnqueueLocked(uint32_t slot, bool demand, Request& out)
{
	PageRecord* rec = FindRecordLocked(slot);
	if (!rec) {
		auto owned = std::make_unique<PageRecord>();
		owned->slot = slot;
		owned->state = PageState::Queued;
		rec = owned.get();
		m_pages.emplace(slot, std::move(owned));
		if (demand)
			m_demandQueue.push_back(slot);
		else
			m_prefetchQueue.push_back(slot);
	}
	else if (rec->state == PageState::Missing) {
		rec->state = PageState::Queued;
		if (demand)
			m_demandQueue.push_back(slot);
		else
			m_prefetchQueue.push_back(slot);
	}
	// Resident/Loading/Queued: ATTACH (the dedup — one disk read per
	// page no matter how many callers arrive). NOTE: the common block
	// below counts the waiter for BOTH the created and the attached
	// record — creation must NOT pre-set waiters (the double-count
	// leaked one waiter per demand read and made every page
	// permanently eviction-immune).
	if (demand) {
		rec->demandArrived = true;
		rec->prefetchCancelled = false;
		// The waiter claim: eviction-immune until the Acquire pins
		// (or fails) - see PageRecord::waiters.
		++rec->waiters;
	}
	out.slot = slot;
	out.id = m_nextRequestId++;
	return true;
}

PakPager::PinnedPage PakPager::PinLocked(PageRecord& rec)
{
	++rec.pins;
	rec.touch = m_touchSeq++;
	PinnedPage pin;
	pin.m_bytes = std::span<const uint8_t>(rec.bytes);
	pin.m_pager = this;
	pin.m_slot = rec.slot;
	return pin;
}

void PakPager::Unpin(uint32_t slot)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (PageRecord* rec = FindRecordLocked(slot); rec && rec->pins)
		--rec->pins;
}

void PakPager::EvictOverBudgetLocked()
{
	// Only UNPINNED, WAITER-FREE RESIDENT pages evict (LRU by the
	// touch seq); queued/loading/pinned/waited/failed records stay.
	// The pool never invalidates a live span — a pinned page is
	// immune, and a page with demand waiters is immune until they pin
	// (the install -> pin window; a budget below one page would
	// otherwise install -> evict -> re-enqueue forever).
	uint64_t total = 0;
	for (const auto& [slot, rec] : m_pages)
		if (rec->state == PageState::Resident)
			total += rec->bytes.size();
	while (total > m_budget) {
		PageRecord* lru = nullptr;
		for (const auto& [slot, rec] : m_pages) {
			(void)slot;
			if (rec->state != PageState::Resident || rec->pins
					|| rec->waiters)
				continue;
			if (!lru || rec->touch < lru->touch)
				lru = rec.get();
		}
		if (!lru)
			break; // everything resident is pinned/waited: over
			       // budget is the holders' doing, not the pool's
		total -= lru->bytes.size();
		m_pages.erase(lru->slot);
		++m_evictions;
	}
}

PakPager::PageRecord* PakPager::FindRecordLocked(uint32_t slot) const
{
	// Mutable lookup over the map (the mutex is held by the caller;
	// the const-ness of the method is the poll surface's, not the
	// record's).
	const auto it = m_pages.find(slot);
	return it == m_pages.end() ? nullptr : it->second.get();
}

void PakPager::WorkerLoop()
{
	// The demand lane first (prefetch is early work, demand owns
	// latency); a QUEUED PREFETCH that was cancelled (and never saw a
	// demand) dies here.
	for (;;) {
		uint32_t slot = UINT32_MAX;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cvWorker.wait(lock, [this] {
				return m_stop || !m_demandQueue.empty()
					|| !m_prefetchQueue.empty();
			});
			if (m_stop && m_demandQueue.empty() && m_prefetchQueue.empty())
				return;
			if (!m_demandQueue.empty()) {
				slot = m_demandQueue.front();
				m_demandQueue.pop_front();
			}
			else {
				slot = m_prefetchQueue.front();
				m_prefetchQueue.pop_front();
			}
			PageRecord* rec = FindRecordLocked(slot);
			if (!rec)
				continue;
			if (rec->prefetchCancelled && !rec->demandArrived) {
				// The cancelled prefetch: the record dies (no pin can
				// exist on a queued page).
				m_pages.erase(slot);
				continue;
			}
			if (rec->state != PageState::Queued)
				continue; // a previous pass installed it
			rec->state = PageState::Loading;
		}

		// ---- the disk IO, OUTSIDE the lock --------------------------------
		std::vector<uint8_t> decoded;
		std::string failure;
		const PakTocEntry* entry = m_reader->FindBySlot(slot);
		if (!entry) {
			failure = "the pager's TOC has no slot "
				+ std::to_string(slot);
		}
		else {
			std::vector<uint8_t> stored(
				static_cast<size_t>(entry->size));
			m_file.clear();
			m_file.seekg(std::streamoff(entry->offset),
				std::ios::beg);
			m_file.read(reinterpret_cast<char*>(stored.data()),
				std::streamsize(stored.size()));
			if (m_file.gcount() != std::streamsize(stored.size())) {
				failure = "pak page read failed: " + entry->path;
			}
			else if (PakCrc32(stored.data(), stored.size())
					!= entry->contentCrc) {
				// The SMP8 lazy page verification, surfaced by the
				// pager: the corrupt page fails AT the page, loudly.
				failure = "pak page CRC mismatch (corrupt page): "
					+ entry->path;
			}
			else if (entry->flags & PakEntryFlag_Lz4) {
				if (!DecompressPakBlob(stored, true, decoded))
					failure = "pak LZ4 decompress failed: "
						+ entry->path;
			}
			else {
				decoded = std::move(stored);
			}
		}

		// ---- install under the lock --------------------------------------
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_diskReads;
			m_bytesRead += entry ? entry->size : 0;
			PageRecord* rec = FindRecordLocked(slot);
			if (!rec)
				continue; // the record died while we read (nothing
				          // can wait on a dead record)
			if (failure.empty()) {
				rec->bytes = std::move(decoded);
				rec->state = PageState::Resident;
				rec->touch = m_touchSeq++;
			}
			else {
				rec->state = PageState::Failed;
				rec->error = failure;
			}
			EvictOverBudgetLocked();
		}
		m_cvDone.notify_all();
	}
}

} // namespace smvk
