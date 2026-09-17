#include <Engine/Flintbase/flintbase_backend.h>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace smvk {
namespace {

bool ReadHostFileBytes(const std::string& hostPath, std::vector<uint8_t>& out) {
	std::ifstream f(hostPath, std::ios::binary);
	if (!f)
		return false;
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	if (size < 0)
		return false;
	f.seekg(0, std::ios::beg);
	out.resize((size_t)size);
	if (!out.empty())
		f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
	return static_cast<bool>(f) || out.empty();
}

bool Sha256(const uint8_t* data, size_t n, uint8_t out[32]) {
	BCRYPT_ALG_HANDLE alg = nullptr;
	NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (st != 0)
		return false;
	BCRYPT_HASH_HANDLE hash = nullptr;
	st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
	if (st != 0) {
		BCryptCloseAlgorithmProvider(alg, 0);
		return false;
	}
	bool ok = BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0) == 0
		&& BCryptFinishHash(hash, out, 32, 0) == 0;
	BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

bool HexEquals(const uint8_t digest[32], const std::string& hex) {
	static const char* kHex = "0123456789abcdef";
	if (hex.size() != 64)
		return false;
	for (size_t i = 0; i < 32; ++i) {
		const char hi = kHex[digest[i] >> 4];
		const char lo = kHex[digest[i] & 0x0F];
		char a = hex[i * 2], b = hex[i * 2 + 1];
		if (a >= 'A' && a <= 'F') a = char(a - 'A' + 'a');
		if (b >= 'A' && b <= 'F') b = char(b - 'A' + 'a');
		if (a != hi || b != lo)
			return false;
	}
	return true;
}

} // namespace

FlintbaseBackend::FlintbaseBackend(std::shared_ptr<IFlintbaseApi> api)
	: api_(api ? std::move(api) : std::make_shared<FlintbaseClient>()) {
}

bool FlintbaseBackend::ResolveRemainder(const std::string& remainder,
	const FlintbaseResolved*& out, FlintbaseError& error) const {
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = cache_.find(remainder);
	if (it != cache_.end()) {
		out = &it->second;
		return true;
	}
	FlintbaseResolved resolved;
	if (!api_->Resolve(remainder, resolved, error))
		return false;
	out = &cache_.emplace(remainder, std::move(resolved)).first->second;
	return true;
}

const FlintbaseResolved* FlintbaseBackend::ResolveCached(
	const std::string& normalizedSchemePath, FlintbaseError& error) const {
	const VfsPath n = vfs::Normalize(normalizedSchemePath);
	if (n.empty() || !vfs::IsFlintbasePath(n))
		return nullptr;
	const FlintbaseResolved* r = nullptr;
	if (!ResolveRemainder(std::string(vfs::FlintbaseRemainder(n)), r, error))
		return nullptr;
	return r;
}

bool FlintbaseBackend::Exists(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (!vfs::IsFlintbasePath(n))
		return false;
	FlintbaseError error;
	const FlintbaseResolved* r = nullptr;
	return ResolveRemainder(std::string(vfs::FlintbaseRemainder(n)), r, error);
}

std::optional<std::vector<uint8_t>> FlintbaseBackend::TryRead(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (!vfs::IsFlintbasePath(n))
		return std::nullopt;
	FlintbaseError error;
	const FlintbaseResolved* r = nullptr;
	if (!ResolveRemainder(std::string(vfs::FlintbaseRemainder(n)), r, error))
		return std::nullopt;

	const FlintbaseFile* file = r->File();
	if (!file && r->files.size() == 1)
		file = &r->files[0];
	if (!file || file->absPath.empty())
		return std::nullopt;

	std::vector<uint8_t> bytes;
	if (!ReadHostFileBytes(file->absPath, bytes))
		return std::nullopt;

	if (!file->sha256.empty()) {
		uint8_t digest[32];
		if (!Sha256(bytes.data(), bytes.size(), digest)
			|| !HexEquals(digest, file->sha256)) {
			std::fprintf(stderr,
				"Flintbase: SHA-256 mismatch for '%s' (manifest %s)\n",
				file->absPath.c_str(), file->sha256.c_str());
			return std::nullopt;
		}
	}
	return bytes;
}

std::vector<VfsPath> FlintbaseBackend::ListDir(const VfsPath& dir) const {
	std::vector<VfsPath> out;
	const VfsPath n = vfs::Normalize(dir);
	if (!vfs::IsFlintbasePath(n))
		return out;

	// Walk up from `dir` to the nearest resolvable product path, keeping
	// the stripped segments as the listing prefix. Browsing above the
	// product level (project/item/task/version) happens through the API.
	std::string candidate(vfs::FlintbaseRemainder(n));
	std::string prefix;
	for (int attempt = 0; attempt < 8 && !candidate.empty(); ++attempt) {
		FlintbaseError error;
		const FlintbaseResolved* r = nullptr;
		if (ResolveRemainder(candidate, r, error)) {
			for (const auto& f : r->files) {
				if (f.path.compare(0, prefix.size(), prefix) != 0)
					continue;
				const std::string rest = f.path.substr(prefix.size());
				if (rest.empty())
					continue;
				const size_t slash = rest.find('/');
				out.push_back(slash == std::string::npos
					? n + "/" + rest
					: n + "/" + rest.substr(0, slash));
			}
			std::sort(out.begin(), out.end());
			out.erase(std::unique(out.begin(), out.end()), out.end());
			return out;
		}
		const size_t slash = candidate.rfind('/');
		if (slash == std::string::npos)
			break;
		prefix = candidate.substr(slash + 1) + "/" + prefix;
		candidate = candidate.substr(0, slash);
	}
	return out;
}

std::optional<VfsStat> FlintbaseBackend::Stat(const VfsPath& path) const {
	const VfsPath n = vfs::Normalize(path);
	if (!vfs::IsFlintbasePath(n))
		return std::nullopt;
	FlintbaseError error;
	const FlintbaseResolved* r = nullptr;
	if (!ResolveRemainder(std::string(vfs::FlintbaseRemainder(n)), r, error))
		return std::nullopt;

	const FlintbaseFile* file = r->File();
	if (!file && r->files.size() == 1)
		file = &r->files[0];
	if (!file)
		return std::nullopt;

	VfsStat s;
	s.size = file->size;
	s.backend = Name();
	s.source = file->absPath;
	return s;
}

std::string FlintbaseHostPath(const std::string& path) {
	if (!vfs::HasFlintbasePrefix(path))
		return path;
	static FlintbaseBackend backend;
	FlintbaseError error;
	const FlintbaseResolved* r = backend.ResolveCached(path, error);
	if (!r) {
		std::fprintf(stderr, "Flintbase: resolve '%s' failed: %s\n",
			path.c_str(), error.message.c_str());
		return {};
	}
	const FlintbaseFile* file = r->File();
	if (!file && r->files.size() == 1)
		file = &r->files[0];
	if (!file) {
		std::fprintf(stderr,
			"Flintbase: '%s' does not address a single file (%zu in manifest)\n",
			path.c_str(), r->files.size());
		return {};
	}
	return file->absPath;
}

} // namespace smvk
