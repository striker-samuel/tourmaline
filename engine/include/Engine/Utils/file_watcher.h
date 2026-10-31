// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE PROJECT-SCOPOPED NATIVE FILESYSTEM WATCHER (the watched-resource
// pipeline's OS event source): ReadDirectoryChangesW on Windows,
// inotify on Linux — one background thread over a set of REGISTERED
// watch roots, filtered by an ignore policy, DEBOUNCED to settled
// changes, and reconcilable against a stat snapshot when events were
// missed (startup, overflow, sleep/suspend, a restarted watcher).
//
// The consumer contract (the checklist's watcher responsibilities):
//   - Register the SOURCE inputs to watch (project content dirs, the
//     shaders dir, a recipe, a .tour, a .meta, an importer
//     dependency). Never the whole disk, never ad-hoc polling.
//   - The POLICY excludes generated artifacts/intermediates/published
//     products from the stream WHATEVER directory they physically sit
//     in: derived .geometry/.material/.anim families beside their
//     glTF sources, compiled .spv, staging dirs, caches, generated
//     headers, paks/.tapp manifests, executables. Only authoritative
//     sources produce change events.
//   - Poll() once per frame is CHEAP: it drains an already-debounced
//     queue. The OS queue carries the load; the watcher never
//     scans/hashes the entire project per frame.
//   - A change settles only after the file goes QUIET for the
//     stabilize window AND its bytes are readable (atomic-save
//     renames — write-temp-then-replace — land as one settled
//     Modified on the target; the temp name itself is filtered).
//   - Overflowed() true (the OS buffer dropped events) or a fresh
//     Start() → the consumer calls Reconcile(): a stat-only snapshot
//     diff against the watcher's last known state that reports
//     everything the event stream missed. CONTENT hashes stay with
//     the fingerprinting layer (verified content, never metadata).
//
// Thread model: Start() spawns the thread; Add*() / Stop() / Poll()
// / Reconcile() are safe from any one thread at a time (the editor's
// main loop). All paths are absolute, forward-slash, lexically
// normalized.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace smvk {

class FileWatcher {
public:
	// One settled change (post-debounce, post-filter).
	enum class Event : uint8_t {
		Modified, // content/attributes changed (incl. rename-over)
		Added,    // a new file appeared (or replaced a gap)
		Removed,  // the file vanished
	};

	struct Change {
		std::string path; // absolute, forward slashes
		Event event = Event::Modified;
		uint64_t size = 0;     // post-settle size (0 = removed/unknown)
		int64_t mtimeSec = -1; // post-settle unix seconds
	};

	// The exclusion policy (the generated-vs-source boundary). The
	// DEFAULT carries the pipeline's canonical set; owners can extend.
	struct Policy {
		// Directory NAMES never watched (recursively skipped wherever
		// they appear): derived data, staging, caches, build trees,
		// pak output, editor exports.
		std::vector<std::string> ignoredDirNames;
		// File EXTENSIONS never reported (generated + published
		// artifacts): the derived family, compiled shaders, paks,
		// manifests, generated headers, executables, databases.
		std::vector<std::string> ignoredExtensions;
		// File NAME substrings never reported (atomic-save temps).
		std::vector<std::string> ignoredNameContains;

		// The canonical project policy (the checklist's exclusion
		// set): content sources + .meta + .tour/.tourrecipe + shader
		// sources/includes produce events; everything the pipeline
		// itself writes is invisible (feedback loops die here).
		[[nodiscard]] static Policy ProjectDefault();

		// True when `absPath` (absolute, any separators) is excluded.
		[[nodiscard]] bool Ignored(
			const std::string& absPath) const;
	};

	FileWatcher() = default;
	~FileWatcher(); // Stop()s (joins) — never leaves a thread behind

	FileWatcher(const FileWatcher&) = delete;
	FileWatcher& operator=(const FileWatcher&) = delete;

	// ---- lifecycle -------------------------------------------------------
	// Starts the watcher thread with `policy` (a second Start while
	// running just swaps the policy — cheap and idempotent).
	[[nodiscard]] bool Start(const Policy& policy, std::string& err);
	void Stop(); // joins the thread; registered roots STAY registered
	[[nodiscard]] bool Running() const;

	// ---- registration ----------------------------------------------------
	// Register a DIRECTORY (watched recursively — the policy prunes)
	// or a single FILE. Idempotent; registering under a stopped
	// watcher is fine (the roots arm at the next Start). Returns
	// false only for a nonexistent path.
	[[nodiscard]] bool AddDirectory(const std::string& dir,
		std::string& err);
	[[nodiscard]] bool AddFile(const std::string& file,
		std::string& err);

	// ---- consumption -----------------------------------------------------
	// Drain SETTLED changes (call once per frame). `stabilizeMs` is
	// the quiet window a file must sit through before its change is
	// reported (writes land as one event, not a burst). Cheap: the
	// queue is already filtered; settling only stats the touched
	// files.
	[[nodiscard]] std::vector<Change> Poll(int stabilizeMs = 150);

	// True when the OS event buffer overflowed since the last Poll —
	// events were LOST; the consumer should Reconcile().
	[[nodiscard]] bool Overflowed() const;

	// The missed-event recovery: a stat-only snapshot diff of every
	// registered root against the watcher's last known state. Used at
	// startup (baseline), after Overflowed(), after sleep/suspend, or
	// any time the owner suspects gaps. NOT a content hash — the
	// fingerprinting layer owns verified bytes.
	[[nodiscard]] std::vector<Change> Reconcile(std::string& err);

private:
	struct Impl;
	Impl* m_impl = nullptr; // lazily built (Start); see file_watcher.cpp
};

} // namespace smvk
