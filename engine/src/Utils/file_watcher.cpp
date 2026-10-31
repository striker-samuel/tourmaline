// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// THE PROJECT-SCOPOPED NATIVE FILESYSTEM WATCHER (see file_watcher.h).
// One background thread; ReadDirectoryChangesW-over-IOCP on Windows,
// inotify on Linux. The OS event stream lands in a PENDING map keyed
// by path (the LAST event wins, timestamped); Poll() settles entries
// whose quiet window elapsed and whose bytes are readable (an
// in-flight writer holding a share lock re-arms the window). The
// KNOWN-STATE map (mtime+size per path) classifies Added vs Modified
// vs Removed and backs the Reconcile() snapshot diff — stat-only,
// never a content hash (the fingerprinting layer owns verified
// bytes).

#include <Engine/Utils/file_watcher.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace smvk {

namespace {

namespace fs = std::filesystem;

// Absolute, forward-slash, lexically-normalized (the watcher's path
// canonical form — every event, root and known-state key uses it).
std::string Norm(const std::string& p) {
	std::error_code ec;
	fs::path abs = fs::absolute(fs::path(p), ec);
	if (ec)
		abs = fs::path(p);
	return abs.lexically_normal().generic_string();
}

bool StatPath(const std::string& path, int64_t& mtimeSec,
	uint64_t& size) {
	std::error_code ec;
	const fs::path p(path);
	if (!fs::is_regular_file(p, ec))
		return false;
	const fs::file_time_type lwt = fs::last_write_time(p, ec);
	if (ec)
		return false;
	const uintmax_t bytes = fs::file_size(p, ec);
	if (ec)
		return false;
	mtimeSec = int64_t(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::clock_cast<std::chrono::system_clock>(lwt)
			.time_since_epoch())
		.count());
	size = uint64_t(bytes);
	return true;
}

// The stability probe: a file still being written typically holds a
// write share lock — an open-for-read failure means NOT settled yet.
bool ReadableNow(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	return in.good();
}

int64_t SteadyMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

struct FileInfo {
	int64_t mtimeSec = -1;
	uint64_t size = 0;
};

} // namespace

// ---- the policy ---------------------------------------------------------

bool FileWatcher::Policy::Ignored(const std::string& absPath) const {
	const fs::path p(Norm(absPath));
	// Directory components: an ignored dir NAME anywhere in the path
	// prunes the whole subtree (regardless of physical location —
	// "exclude generated outputs wherever they sit").
	for (const auto& part : p.parent_path()) {
		const std::string comp = part.generic_string();
		for (const std::string& name : ignoredDirNames)
			if (comp == name)
				return true;
	}
	const std::string file = p.filename().generic_string();
	const std::string ext = p.extension().generic_string();
	for (const std::string& e : ignoredExtensions)
		if (ext == e)
			return true;
	for (const std::string& needle : ignoredNameContains)
		if (file.find(needle) != std::string::npos)
			return true;
	return false;
}

FileWatcher::Policy FileWatcher::Policy::ProjectDefault() {
	Policy p;
	// Derived data / staging / caches / build trees / published
	// output: wherever they appear (a staging dir inside content/
	// is still invisible).
	p.ignoredDirNames = {
		".tourmaline", // the project's derived-data home (cache.db,
		               // assets.db, stage/, pak/)
		".git",
		"out",      // build trees (content/shaders/*.spv live here)
		"build",    // the ids/paks default publish dir
		"paks",     // per-scene editor cook outputs
		"exports",  // editor capture/export output
		"external", // vendored third-party trees (not authored source)
	};
	// Generated + published artifacts (the derive-beside families,
	// compiled shaders, packages, generated headers, executables,
	// databases).
	p.ignoredExtensions = {
		".geometry", // the importer's derived mesh family
		".material", // derived material assets
		".anim",     // derived animation assets
		".spv",      // compiled shaders
		".pak",      // content-addressed packages
		".tapp",     // published manifests
		".exe",
		".dll",
		".pdb",
		".db",
		".obj",
		".lib",
	};
	// NOTE: .meta sidecars (incl. the .gltf.meta manifest) are NOT
	// ignored — they are identity-bearing SOURCE files (a hand-edited
	// guid row is an authored change). The GENERATED header
	// (project.h) carries no stable extension, so the
	// generated-headers rule rides the ignored DIRS (out/, build/)
	// + the name filter below.
	p.ignoredNameContains = {
		"project.h",  // the generated catalog (any dir)
		"~",          // atomic-save/editor temps
		".tmp",       // atomic-save temps
	};
	return p;
}

// ---- the implementation --------------------------------------------------

struct FileWatcher::Impl {
	Policy policy;

	std::atomic<bool> running{ false };
	std::atomic<bool> overflowed{ false };
	std::thread thread;

	// Registered roots (normalized). `dir` roots watch RECURSIVELY;
	// `file` roots watch the file's parent dir and report only that
	// file.
	std::vector<std::string> dirRoots;
	std::set<std::string> fileRoots;

	// The pending event stream: path -> last event + timestamp.
	struct Pending {
		FileWatcher::Event event = FileWatcher::Event::Modified;
		int64_t lastEventMs = 0;
	};
	std::mutex mtx;
	std::map<std::string, Pending> pending;
	std::map<std::string, FileInfo> known;
	bool baselineTaken = false; // Reconcile(): silent first snapshot

	// Is `path` inside any registered root (or a registered file)?
	bool Watched(const std::string& path) const {
		if (fileRoots.count(path))
			return true;
		for (const std::string& root : dirRoots) {
			const std::string prefix = root + "/";
			if (path.size() > prefix.size()
				&& path.compare(0, prefix.size(), prefix) == 0)
				return true;
			if (path == root)
				return true;
		}
		return false;
	}

	// Record one raw event (thread side). Filtered by policy +
	// registration; the LAST event for a path wins (an add-then-write
	// burst collapses to the newest classification).
	void Record(const std::string& rawPath, Event ev) {
		const std::string path = Norm(rawPath);
		if (path.empty() || !Watched(path))
			return;
		if (policy.Ignored(path))
			return;
		std::lock_guard<std::mutex> lock(mtx);
		Pending& p = pending[path];
		p.event = ev;
		p.lastEventMs = SteadyMs();
	}

	// Snapshot every dir root (recursive, policy-pruned) + file root.
	std::map<std::string, FileInfo> Snapshot() const {
		std::map<std::string, FileInfo> snap;
		for (const std::string& root : dirRoots) {
			std::error_code ec;
			if (!fs::is_directory(fs::path(root), ec))
				continue;
			for (const fs::directory_entry& e :
					fs::recursive_directory_iterator(fs::path(root),
						ec)) {
				if (ec)
					break;
				std::error_code fe;
				if (!e.is_regular_file(fe))
					continue;
				const std::string path =
					e.path().lexically_normal().generic_string();
				if (policy.Ignored(path))
					continue;
				FileInfo fi;
				if (StatPath(path, fi.mtimeSec, fi.size))
					snap.emplace(path, fi);
			}
		}
		for (const std::string& f : fileRoots) {
			FileInfo fi;
			if (StatPath(f, fi.mtimeSec, fi.size))
				snap.emplace(f, fi);
		}
		return snap;
	}

	// ---- the OS watch arms (platform) --------------------------------
#ifdef _WIN32
	// ReadDirectoryChangesW over an IOCP: one directory handle per
	// dir root + one per file root's PARENT (filtered by Watched()).
	// A registration change (Add*/Stop) posts a control key; the
	// thread rebuilds the handle set on it.
	static constexpr ULONG_PTR kDirKey = 1;
	static constexpr ULONG_PTR kRebuildKey = 2;
	static constexpr ULONG_PTR kQuitKey = 3;
	HANDLE iocp = nullptr;
	struct Watch {
		HANDLE handle = nullptr;
		OVERLAPPED ovl{};
		std::vector<char> buffer;
		std::string dir; // the watched root (names arrive RELATIVE)
	};

	bool ArmWatch(Watch& w, const std::string& dir,
		std::string& err) {
		const std::wstring wide = fs::path(dir).wstring();
		w.dir = dir;
		w.handle = CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
			nullptr);
		if (w.handle == INVALID_HANDLE_VALUE) {
			err = "cannot open watch dir '" + dir + "' ("
				+ std::to_string(GetLastError()) + ")";
			return false;
		}
		if (!CreateIoCompletionPort(w.handle, iocp, kDirKey, 0)) {
			err = "cannot bind watch dir '" + dir + "' to the IOCP ("
				+ std::to_string(GetLastError()) + ")";
			CloseHandle(w.handle);
			w.handle = nullptr;
			return false;
		}
		return Reissue(w, err);
	}

	bool Reissue(Watch& w, std::string& err) {
		w.buffer.assign(64 * 1024, 0);
		memset(&w.ovl, 0, sizeof w.ovl);
		const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME
			| FILE_NOTIFY_CHANGE_LAST_WRITE
			| FILE_NOTIFY_CHANGE_SIZE
			| FILE_NOTIFY_CHANGE_CREATION;
		if (!ReadDirectoryChangesW(w.handle, w.buffer.data(),
				DWORD(w.buffer.size()), TRUE /* subtree */, filter,
				nullptr, &w.ovl, nullptr)) {
			const DWORD e = GetLastError();
			if (e == ERROR_NOTIFY_ENUM_DIR) {
				// The buffer overflowed: events were LOST.
				overflowed.store(true);
				return true; // re-issuable later
			}
			err = "ReadDirectoryChangesW failed (" + std::to_string(e)
				+ ")";
			return false;
		}
		return true;
	}

	void ThreadLoop() {
		std::vector<std::unique_ptr<Watch>> local;
		auto rebuild = [&](std::string& err) {
			for (std::unique_ptr<Watch>& w : local) {
				if (w && w->handle) {
					CancelIo(w->handle);
					CloseHandle(w->handle);
				}
			}
			local.clear();
			// Drain the stale completions of the handles just closed
			// (they arrive as ERROR_OPERATION_ABORTED): they must not
			// be mistaken for live events against the new set.
			for (;;) {
				DWORD bytes = 0;
				ULONG_PTR key = 0;
				OVERLAPPED* ovl = nullptr;
				if (!GetQueuedCompletionStatus(iocp, &bytes, &key,
						&ovl, 0))
					break;
			}
			// The watch dirs: every dir root + every file root's
			// parent (deduped).
			std::set<std::string> dirs(dirRoots.begin(),
				dirRoots.end());
			for (const std::string& f : fileRoots)
				dirs.insert(fs::path(f).parent_path()
					.lexically_normal().generic_string());
			for (const std::string& d : dirs) {
				std::unique_ptr<Watch> w = std::make_unique<Watch>();
				if (ArmWatch(*w, d, err)) {
					local.push_back(std::move(w));
				}
				else {
					std::fprintf(stderr,
						"[FileWatcher] %s\n", err.c_str());
					err.clear(); // one bad root never kills the rest
				}
			}
		};
		std::string err;
		rebuild(err);
		while (running.load()) {
			DWORD bytes = 0;
			ULONG_PTR key = 0;
			OVERLAPPED* ovl = nullptr;
			if (!GetQueuedCompletionStatus(iocp, &bytes, &key, &ovl,
					100 /* ms */)) {
				const DWORD e = GetLastError();
				if (e == WAIT_TIMEOUT)
					continue; // the idle tick
				if (e == ERROR_OPERATION_ABORTED)
					continue; // our own CancelIo (a rebuild/stop)
				if (e == ERROR_ABANDONED_WAIT_0) {
					// A watch handle died under us: events were lost.
					overflowed.store(true);
					continue;
				}
				continue; // anything else: retry the loop
			}
			if (key == kQuitKey)
				break;
			if (key == kRebuildKey) {
				rebuild(err);
				continue;
			}
			if (key == kDirKey && ovl) {
				// Find the owning watch (the overlapped pointer
				// identifies it) and parse its buffer.
				Watch* w = nullptr;
				for (std::unique_ptr<Watch>& x : local)
					if (&x->ovl == ovl) {
						w = x.get();
						break;
					}
				if (!w)
					continue;
				if (bytes == 0) {
					// Overflow notification: events were lost.
					overflowed.store(true);
					if (!Reissue(*w, err))
						std::fprintf(stderr, "[FileWatcher] %s\n",
							err.c_str());
					continue;
				}
				const char* base = w->buffer.data();
				DWORD off = 0;
				while (true) {
					const FILE_NOTIFY_INFORMATION* info =
						reinterpret_cast<const
							FILE_NOTIFY_INFORMATION*>(base + off);
					// The name is the struct's flexible array member
					// (right past the fixed header) — FileNameLength
					// is in BYTES.
					const wchar_t* name =
						reinterpret_cast<const wchar_t*>(
							reinterpret_cast<const char*>(info)
								+ offsetof(FILE_NOTIFY_INFORMATION,
									FileName));
					std::wstring rel(name,
						info->FileNameLength / sizeof(wchar_t));
					// The event name is RELATIVE to the watch root —
					// join before any filtering.
					const std::string path =
						(fs::path(w->dir) / fs::path(rel))
							.lexically_normal().generic_string();
					switch (info->Action) {
					case FILE_ACTION_ADDED:
					case FILE_ACTION_MODIFIED:
					case FILE_ACTION_RENAMED_NEW_NAME: {
						bool wasKnown = false;
						{
							std::lock_guard<std::mutex> lock(mtx);
							wasKnown = known.count(path) != 0;
						}
						Record(path, wasKnown
								? Event::Modified : Event::Added);
						break;
					}
					case FILE_ACTION_REMOVED:
					case FILE_ACTION_RENAMED_OLD_NAME:
						Record(path, Event::Removed);
						break;
					default:
						break;
					}
					if (info->NextEntryOffset == 0)
						break;
					off += info->NextEntryOffset;
				}
				if (!Reissue(*w, err))
					std::fprintf(stderr, "[FileWatcher] %s\n",
						err.c_str());
			}
		}
		for (std::unique_ptr<Watch>& w : local) {
			if (w && w->handle) {
				CancelIo(w->handle);
				CloseHandle(w->handle);
			}
		}
	}
#else
	// inotify: one watch descriptor per dir root (recursive at arm
	// time; new subdirs discovered via events re-arm) + file roots'
	// parents (filtered by Watched()). A self-pipe wakes the poll on
	// registration changes/stop.
	int inotifyFd = -1;
	int wakePipe[2] = { -1, -1 }; // [0] read, [1] write
	std::map<int, std::string> pathByWd;

	bool ArmWatch(const std::string& dir, std::string& err) {
		const uint32_t mask = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE
			| IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
		const int wd = inotify_add_watch(inotifyFd, dir.c_str(), mask);
		if (wd < 0) {
			err = std::string("inotify_add_watch failed on '") + dir
				+ "': " + std::strerror(errno);
			return false;
		}
		pathByWd[wd] = dir;
		return true;
	}

	// Recursive arm (the subtree walk, policy-pruned).
	void ArmRecursive(const std::string& dir, std::string& err) {
		if (!ArmWatch(dir, err)) {
			std::fprintf(stderr, "[FileWatcher] %s\n", err.c_str());
			err.clear();
			return;
		}
		std::error_code ec;
		for (const fs::directory_entry& e :
				fs::directory_iterator(fs::path(dir), ec)) {
			if (ec)
				break;
			std::error_code de;
			if (!e.is_directory(de))
				continue;
			if (policy.Ignored(
					e.path().lexically_normal().generic_string()))
				continue;
			ArmRecursive(e.path().lexically_normal()
				.generic_string(), err);
		}
	}

	void ThreadLoop() {
		std::string err;
		auto rebuild = [&]() {
			// Drop the old descriptors, re-arm the current roots.
			for (const auto& [wd, path] : pathByWd)
				inotify_rm_watch(inotifyFd, wd);
			pathByWd.clear();
			for (const std::string& d : dirRoots)
				ArmRecursive(d, err);
			std::set<std::string> parents;
			for (const std::string& f : fileRoots)
				parents.insert(fs::path(f).parent_path()
					.lexically_normal().generic_string());
			for (const std::string& p : parents)
				ArmWatch(p, err);
		};
		rebuild();
		char buf[64 * 1024] __attribute__((aligned(8)));
		while (running.load()) {
			pollfd fds[2] = {
				{ inotifyFd, POLLIN, 0 },
				{ wakePipe[0], POLLIN, 0 },
			};
			const int ready = poll(fds, 2, 100 /* ms */);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (fds[1].revents & POLLIN) {
				char drain[64];
				while (read(wakePipe[0], drain, sizeof drain) > 0) {}
				if (!running.load())
					break;
				rebuild();
			}
			if (!(fds[0].revents & POLLIN))
				continue;
			const ssize_t n = read(inotifyFd, buf, sizeof buf);
			if (n <= 0)
				continue;
			ssize_t off = 0;
			while (off < n) {
				const inotify_event* ev =
					reinterpret_cast<const inotify_event*>(
						buf + off);
				off += sizeof *ev + ev->len;
				const auto wit = pathByWd.find(ev->wd);
				if (wit == pathByWd.end())
					continue;
				if (ev->mask & IN_Q_OVERFLOW) {
					overflowed.store(true);
					continue;
				}
				const std::string name = ev->len ? ev->name : "";
				const std::string path =
					fs::path(wit->second) / fs::path(name)
						.lexically_normal().generic_string();
				if (name.empty())
					continue;
				if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
					Record(path, Event::Removed);
					continue;
				}
				if (ev->mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE
						| IN_CREATE | IN_MOVED_TO)) {
					bool wasKnown = false;
					{
						std::lock_guard<std::mutex> lock(mtx);
						wasKnown = known.count(Norm(path)) != 0;
					}
					Record(path, wasKnown
							? Event::Modified : Event::Added);
				}
			}
		}
	}
#endif
};

// ---- lifecycle ------------------------------------------------------------

FileWatcher::~FileWatcher() {
	Stop();
	delete m_impl;
}

bool FileWatcher::Start(const Policy& policy, std::string& err) {
	err.clear();
	if (!m_impl)
		m_impl = new Impl{};
	m_impl->policy = policy;
	if (m_impl->running.load())
		return true; // policy swap only — the thread picks it up
#ifdef _WIN32
	m_impl->iocp = CreateIoCompletionPort(
		INVALID_HANDLE_VALUE, nullptr, 0, 1);
	if (!m_impl->iocp) {
		err = "cannot create the IOCP ("
			+ std::to_string(GetLastError()) + ")";
		return false;
	}
#else
	m_impl->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (m_impl->inotifyFd < 0) {
		err = std::string("inotify_init1 failed: ")
			+ std::strerror(errno);
		return false;
	}
	if (pipe(m_impl->wakePipe) != 0) {
		err = std::string("pipe failed: ") + std::strerror(errno);
		close(m_impl->inotifyFd);
		m_impl->inotifyFd = -1;
		return false;
	}
	for (int& fd : m_impl->wakePipe) {
		const int flags = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
#endif
	m_impl->running.store(true);
	try {
		m_impl->thread = std::thread([](Impl* impl) {
			impl->ThreadLoop();
		}, m_impl);
	}
	catch (const std::system_error& e) {
		err = std::string("cannot start the watcher thread: ")
			+ e.what();
		m_impl->running.store(false);
#ifdef _WIN32
		CloseHandle(m_impl->iocp);
		m_impl->iocp = nullptr;
#else
		close(m_impl->inotifyFd);
		m_impl->inotifyFd = -1;
#endif
		return false;
	}
	return true;
}

void FileWatcher::Stop() {
	if (!m_impl || !m_impl->running.load())
		return;
	m_impl->running.store(false);
#ifdef _WIN32
	PostQueuedCompletionStatus(m_impl->iocp, 0, Impl::kQuitKey,
		nullptr);
	if (m_impl->thread.joinable())
		m_impl->thread.join();
	CloseHandle(m_impl->iocp);
	m_impl->iocp = nullptr;
#else
	const char c = 'q';
	(void)!write(m_impl->wakePipe[1], &c, 1);
	if (m_impl->thread.joinable())
		m_impl->thread.join();
	close(m_impl->inotifyFd);
	m_impl->inotifyFd = -1;
	for (int& fd : m_impl->wakePipe)
		if (fd >= 0)
			close(fd);
	m_impl->wakePipe[0] = m_impl->wakePipe[1] = -1;
#endif
	// The pending stream + known state SURVIVE the stop: a restart
	// (Start again) re-arms the same roots against the same state,
	// and Reconcile() reports whatever happened in between.
}

bool FileWatcher::Running() const {
	return m_impl && m_impl->running.load();
}

// ---- registration ---------------------------------------------------------

bool FileWatcher::AddDirectory(const std::string& dir, std::string& err) {
	err.clear();
	const std::string norm = Norm(dir);
	std::error_code ec;
	if (!fs::is_directory(fs::path(norm), ec)) {
		err = "not a directory: " + norm;
		return false;
	}
	if (!m_impl)
		m_impl = new Impl{};
	if (std::find(m_impl->dirRoots.begin(), m_impl->dirRoots.end(),
			norm)
		!= m_impl->dirRoots.end())
		return true; // idempotent
	m_impl->dirRoots.push_back(norm);
	if (m_impl->running.load()) {
#ifdef _WIN32
		PostQueuedCompletionStatus(m_impl->iocp, 0, Impl::kRebuildKey,
			nullptr);
#else
		const char c = 'r';
		(void)!write(m_impl->wakePipe[1], &c, 1);
#endif
	}
	return true;
}

bool FileWatcher::AddFile(const std::string& file, std::string& err) {
	err.clear();
	const std::string norm = Norm(file);
	std::error_code ec;
	if (!fs::is_regular_file(fs::path(norm), ec)) {
		err = "not a file: " + norm;
		return false;
	}
	if (!m_impl)
		m_impl = new Impl{};
	if (m_impl->fileRoots.count(norm))
		return true; // idempotent
	m_impl->fileRoots.insert(norm);
	if (m_impl->running.load()) {
#ifdef _WIN32
		PostQueuedCompletionStatus(m_impl->iocp, 0, Impl::kRebuildKey,
			nullptr);
#else
		const char c = 'r';
		(void)!write(m_impl->wakePipe[1], &c, 1);
#endif
	}
	return true;
}

// ---- consumption ----------------------------------------------------------

std::vector<FileWatcher::Change> FileWatcher::Poll(int stabilizeMs) {
	std::vector<Change> out;
	if (!m_impl)
		return out;
	const int64_t now = SteadyMs();
	std::lock_guard<std::mutex> lock(m_impl->mtx);
	for (auto it = m_impl->pending.begin();
			it != m_impl->pending.end();) {
		if (now - it->second.lastEventMs < stabilizeMs) {
			++it;
			continue;
		}
		const std::string& path = it->first;
		Event ev = it->second.event;
		FileInfo fi;
		const bool present = StatPath(path, fi.mtimeSec, fi.size);
		if (!present) {
			// Vanished (or never landed): a Removed only when it was
			// known; otherwise the temp-file noise drops silently.
			if (!m_impl->known.count(path)) {
				it = m_impl->pending.erase(it);
				continue;
			}
			ev = Event::Removed;
			m_impl->known.erase(path);
		}
		else {
			// The stability probe: a writer still holding the file
			// re-arms the window (the settle waits for the bytes).
			if (!ReadableNow(path)) {
				it->second.lastEventMs = now;
				++it;
				continue;
			}
			ev = m_impl->known.count(path) ? Event::Modified
				: Event::Added;
			m_impl->known[path] = fi;
		}
		Change c;
		c.path = path;
		c.event = ev;
		c.size = fi.size;
		c.mtimeSec = fi.mtimeSec;
		out.push_back(std::move(c));
		it = m_impl->pending.erase(it);
	}
	return out;
}

bool FileWatcher::Overflowed() const {
	return m_impl && m_impl->overflowed.load();
}

std::vector<FileWatcher::Change> FileWatcher::Reconcile(
	std::string& err) {
	err.clear();
	std::vector<Change> out;
	if (!m_impl)
		return out;
	// The FIRST reconcile establishes the baseline silently (the
	// startup contract: "what does the world look like" — the owner's
	// own records decide what that implies); every later one diffs.
	const std::map<std::string, FileInfo> snap = m_impl->Snapshot();
	std::lock_guard<std::mutex> lock(m_impl->mtx);
	if (!m_impl->baselineTaken) {
		m_impl->known = snap;
		m_impl->baselineTaken = true;
		m_impl->overflowed.store(false);
		return out;
	}
	for (const auto& [path, fi] : snap) {
		const auto kit = m_impl->known.find(path);
		if (kit == m_impl->known.end()) {
			Change c{ path, Event::Added, fi.size, fi.mtimeSec };
			out.push_back(std::move(c));
			m_impl->known[path] = fi;
		}
		else if (kit->second.mtimeSec != fi.mtimeSec
				|| kit->second.size != fi.size) {
			Change c{ path, Event::Modified, fi.size, fi.mtimeSec };
			out.push_back(std::move(c));
			kit->second = fi;
		}
	}
	for (auto it = m_impl->known.begin(); it != m_impl->known.end();) {
		if (!snap.count(it->first)) {
			Change c{ it->first, Event::Removed, 0, -1 };
			out.push_back(std::move(c));
			it = m_impl->known.erase(it);
		}
		else {
			++it;
		}
	}
	m_impl->overflowed.store(false);
	return out;
}

} // namespace smvk
