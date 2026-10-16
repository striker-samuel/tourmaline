// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Vfs/vfs.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace smvk {
namespace {

// Host filesystem helpers ------------------------------------------------

bool ReadHostFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(p, ec);
	if (ec)
		return false;
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	out.resize(static_cast<size_t>(size));
	if (!out.empty())
		f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
	return static_cast<bool>(f) || out.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// Path utilities
// ---------------------------------------------------------------------------

namespace {

// Core posix-relative normalization shared by regular and scheme paths.
// Collapses separators, skips '.', resolves '..'; returns false when the
// input escapes its root via '..' (out stays empty).
bool NormalizeRelative(const std::string& path, VfsPath& out) {
	std::vector<std::string> parts;
	std::string cur;
	auto flush = [&]() {
		if (cur.empty() || cur == ".") {
			cur.clear();
			return;
		}
		if (cur == "..") {
			if (parts.empty())
				parts.push_back(".."); // marker: escapes root -> invalid below
			else if (parts.back() == "..")
				parts.push_back("..");
			else
				parts.pop_back();
			cur.clear();
			return;
		}
		parts.push_back(cur);
		cur.clear();
	};
	for (char c : path) {
		if (c == '/' || c == '\\') {
			flush();
		}
		else {
			cur.push_back(c);
		}
	}
	flush();
	if (parts.empty()) {
		out.clear();
		return true; // empty is valid: "no remainder"
	}
	for (const auto& p : parts) {
		if (p == "..") {
			out.clear();
			return false; // escaped the root
		}
	}
	out.clear();
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i)
			out.push_back('/');
		out += parts[i];
	}
	return true;
}

} // namespace

VfsPath vfs::Normalize(const std::string& path) {
	if (path.empty())
		return {};
	// Drive-letter absolute (C:/..., C:\...) or posix absolute.
	if (path.size() >= 2 && path[1] == ':')
		return {};
	if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
		return {};
	VfsPath out;
	if (!NormalizeRelative(path, out))
		return {}; // escaped the virtual root
	return out;
}

VfsPath vfs::Join(const std::string& a, const std::string& b) {
	if (a.empty())
		return Normalize(b);
	if (b.empty())
		return Normalize(a);
	return Normalize(a + "/" + b);
}

VfsPath vfs::BaseDir(const VfsPath& path) {
	const VfsPath n = Normalize(path);
	const size_t slash = n.rfind('/');
	if (slash == std::string::npos)
		return {};
	return n.substr(0, slash);
}

VfsPath vfs::FileName(const VfsPath& path) {
	const VfsPath n = Normalize(path);
	const size_t slash = n.rfind('/');
	if (slash == std::string::npos)
		return n;
	return n.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// LooseBackend
// ---------------------------------------------------------------------------

LooseBackend::LooseBackend(std::string rootDir)
	: rootDir_(std::move(rootDir)) {
}

bool LooseBackend::Exists(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return false;
	std::error_code ec;
	return std::filesystem::is_regular_file(std::filesystem::path(rootDir_) / n, ec);
}

std::optional<std::vector<uint8_t>> LooseBackend::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::vector<uint8_t> bytes;
	if (!ReadHostFile(std::filesystem::path(rootDir_) / n, bytes))
		return std::nullopt;
	return bytes;
}

std::vector<VfsPath> LooseBackend::ListDir(const VfsPath& dir) const {
	std::vector<VfsPath> out;
	const VfsPath n = vfs::Normalize(dir);
	std::filesystem::path hostDir = std::filesystem::path(rootDir_);
	if (!n.empty())
		hostDir /= n;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(hostDir, ec)) {
		if (ec)
			break;
		const std::string name = e.path().filename().generic_string();
		if (name.empty() || name == "." || name == "..")
			continue;
		out.push_back(n.empty() ? VfsPath(name) : n + "/" + name);
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::optional<VfsStat> LooseBackend::Stat(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::error_code ec;
	const std::filesystem::path host = std::filesystem::path(rootDir_) / n;
	if (!std::filesystem::is_regular_file(host, ec))
		return std::nullopt;
	VfsStat s;
	s.size = static_cast<uint64_t>(std::filesystem::file_size(host, ec));
	s.backend = Name();
	s.source = host.generic_string();
	return s;
}

// ---------------------------------------------------------------------------
// MemBackend
// ---------------------------------------------------------------------------

MemBackend::MemBackend(std::string name)
	: name_(std::move(name)) {
}

bool MemBackend::Exists(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	return files_.find(n) != files_.end();
}

std::optional<std::vector<uint8_t>> MemBackend::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = files_.find(n);
	if (it == files_.end())
		return std::nullopt;
	return it->second;
}

std::vector<VfsPath> MemBackend::ListDir(const VfsPath& dir) const {
	const VfsPath n = vfs::Normalize(dir);
	const std::string prefix = n.empty() ? std::string{} : n + "/";
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<VfsPath> out;
	for (const auto& [p, _] : files_) {
		if (n.empty()) {
			const size_t slash = p.find('/');
			out.push_back(slash == std::string::npos ? p : p.substr(0, slash));
		}
		else if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0) {
			const std::string rest = p.substr(prefix.size());
			const size_t slash = rest.find('/');
			out.push_back(slash == std::string::npos ? p : prefix + rest.substr(0, slash));
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::optional<VfsStat> MemBackend::Stat(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = files_.find(n);
	if (it == files_.end())
		return std::nullopt;
	VfsStat s;
	s.size = static_cast<uint64_t>(it->second.size());
	s.backend = Name();
	s.source = "mem:" + n;
	return s;
}

void MemBackend::Write(const VfsPath& path, std::vector<uint8_t> bytes) {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	files_[n] = std::move(bytes);
}

bool MemBackend::Erase(const VfsPath& path) {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	return files_.erase(n) > 0;
}

// ---------------------------------------------------------------------------
// OverlayVfs
// ---------------------------------------------------------------------------

void OverlayVfs::Mount(std::shared_ptr<IVfsBackend> backend, int priority, std::string name) {
	if (!backend)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	mounts_.push_back(VfsMount{ priority, std::move(backend), std::move(name) });
	std::stable_sort(mounts_.begin(), mounts_.end(), [](const VfsMount& a, const VfsMount& b) {
		return a.priority < b.priority;
	});
}

bool OverlayVfs::Unmount(const std::string& name) {
	std::lock_guard<std::mutex> lock(mutex_);
	const size_t before = mounts_.size();
	mounts_.erase(
		std::remove_if(mounts_.begin(), mounts_.end(),
			[&](const VfsMount& m) { return m.name == name; }),
		mounts_.end());
	return mounts_.size() != before;
}

void OverlayVfs::Clear() {
	std::lock_guard<std::mutex> lock(mutex_);
	mounts_.clear();
}

bool OverlayVfs::Exists(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& m : mounts_) {
		if (!m.backend)
			continue;
		if (m.backend->Exists(n))
			return true;
	}
	return false;
}

std::optional<std::vector<uint8_t>> OverlayVfs::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& m : mounts_) {
		if (!m.backend)
			continue;
		if (auto bytes = m.backend->TryRead(n))
			return bytes;
	}
	return std::nullopt;
}

std::vector<uint8_t> OverlayVfs::ReadBinary(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (auto bytes = TryRead(n))
		return *bytes;
	std::lock_guard<std::mutex> lock(mutex_);
	std::ostringstream msg;
	msg << "VFS: file not found: '" << path << "' (mounts:";
	for (const auto& m : mounts_)
		msg << " [" << m.name << "]";
	msg << ")";
	throw std::runtime_error(msg.str());
}

std::vector<VfsPath> OverlayVfs::ListDir(const VfsPath& dir) const {
	const VfsPath n = vfs::Normalize(dir);
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<VfsPath> out;
	for (const auto& m : mounts_) {
		if (!m.backend)
			continue;
		for (auto& e : m.backend->ListDir(n))
			out.push_back(std::move(e));
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::optional<VfsStat> OverlayVfs::Stat(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (n.empty())
		return std::nullopt;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& m : mounts_) {
		if (!m.backend)
			continue;
		if (auto s = m.backend->Stat(n))
			return s;
	}
	return std::nullopt;
}

std::vector<std::string> OverlayVfs::FindProviders(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	std::vector<std::string> out;
	if (n.empty())
		return out;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto& m : mounts_) {
		if (m.backend && m.backend->Exists(n))
			out.push_back(m.name);
	}
	return out;
}

std::vector<VfsMount> OverlayVfs::Mounts() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return mounts_;
}

} // namespace smvk
