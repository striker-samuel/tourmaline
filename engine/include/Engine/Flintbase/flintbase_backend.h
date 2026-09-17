#pragma once
// Flintbase VFS backend: serves the reserved //flintbase/ scheme.
//
// Resolution caching: product versions are immutable once finished, so
// resolved manifests are cached forever; "latest" pins to whatever it
// first resolved (per-process pinning - explicit versions are required
// for deterministic replays like .tour files).
//
// File bytes are read straight from storage (root_folder) and verified
// against the manifest SHA-256 on every read; a mismatch fails the read.
//
// Listing: paths above the product level (project/item/task/version)
// do not list - browsing happens through the API. A product path (or a
// directory inside one) lists its manifest entries.

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Engine/Flintbase/flintbase_client.h>
#include <Engine/Vfs/vfs.h>

namespace smvk {

class FlintbaseBackend final : public IVfsBackend {
public:
	// api: injectable for tests; nullptr -> FlintbaseClient().
	explicit FlintbaseBackend(std::shared_ptr<IFlintbaseApi> api = nullptr);

	std::string Name() const override { return "flintbase"; }
	bool ServesSchemePaths() const override { return true; }
	bool Exists(const VfsPath& path) const override;
	std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const override;
	std::vector<VfsPath> ListDir(const VfsPath& dir) const override;
	std::optional<VfsStat> Stat(const VfsPath& path) const override;

	// Resolves a NORMALIZED scheme path (with the //flintbase/ prefix)
	// using the cache. Returns nullptr on failure (error filled).
	[[nodiscard]] const FlintbaseResolved* ResolveCached(const std::string& normalizedSchemePath,
		FlintbaseError& error) const;

private:
	[[nodiscard]] bool ResolveRemainder(const std::string& remainder,
		const FlintbaseResolved*& out, FlintbaseError& error) const;

	std::shared_ptr<IFlintbaseApi> api_;
	mutable std::mutex mutex_;
	// Keyed by scheme-free remainder; immutable entries never evicted.
	mutable std::unordered_map<std::string, FlintbaseResolved> cache_;
};

// Host-path helper for consumers that bypass the VFS (pak open, glTF
// load): "//flintbase/..." paths resolve to their single-file storage
// path; any other input returns unchanged. Empty string on resolution
// failure.
[[nodiscard]] std::string FlintbaseHostPath(const std::string& path);

} // namespace smvk
