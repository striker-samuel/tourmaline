#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace smvk {

// Virtual filesystem: the runtime's abstraction over cooked resources.
// Backends serve loose content trees (dev iteration) and mounted paks
// (shipped content) behind one path space; import-time file access lives
// with the importer in tools/gltf_import, never behind this interface.
//
// VFS path convention: posix-style, relative to the virtual root, e.g.
//   "models/Sponza/sponza.gltf"
//   "textures/brick.ktx2"
//   "shaders/default.vert.hlsl.spv"
// Normalization (backslashes -> '/', '.' / duplicate-separator collapsing,
// '..' resolution with root-escape rejection) lives in vfs::Normalize().
// An empty normalized path means "invalid".
//
// Reserved scheme: paths under "//flintbase/" (the Flintbase asset
// system) keep the scheme prefix verbatim through normalization:
//   "//flintbase/{project}/{item}/{task}[/{step}]/{vNNN|latest}/{product}[/{file}]"
// Scheme paths are routed exclusively to backends that opt in via
// ServesSchemePaths(); regular backends never see them and scheme
// backends never see regular paths. The normalized form is stable, so
// asset URIs round-trip through paks unchanged.
using VfsPath = std::string;

namespace vfs {
// The reserved Flintbase scheme prefix in its canonical (lowercase,
// forward-slash) normalized form.
inline constexpr std::string_view kFlintbaseScheme = "//flintbase/";

// True when a RAW (possibly unnormalized, any-case, any-separator)
// input begins with the scheme authority ("//flintbase/" or
// "\\flintbase\", case-insensitive).
[[nodiscard]] bool HasFlintbasePrefix(std::string_view path);

// True when a NORMALIZED path is a scheme path.
[[nodiscard]] inline bool IsFlintbasePath(std::string_view normalized) {
	return normalized.size() >= kFlintbaseScheme.size()
		&& normalized.compare(0, kFlintbaseScheme.size(), kFlintbaseScheme) == 0;
}

// The scheme-free remainder of a normalized scheme path
// ("project/item/..."). Empty when not a scheme path.
[[nodiscard]] inline std::string_view FlintbaseRemainder(std::string_view normalized) {
	if (!IsFlintbasePath(normalized))
		return {};
	return normalized.substr(kFlintbaseScheme.size());
}
} // namespace vfs

struct VfsStat {
	uint64_t size = 0;
	std::string backend; // backend Name() that served the entry
	std::string source;  // backend-local source id (host path, pak path, "mem")
};

// Storage behind one OverlayVfs mount. Implementations must be thread-safe
// for concurrent reads (cooker texture jobs read from many threads).
class IVfsBackend {
public:
	virtual ~IVfsBackend() = default;
	virtual std::string Name() const = 0;
	// Backends that serve the reserved //flintbase/ scheme exclusively.
	// OverlayVfs routes scheme paths only to such backends, and regular
	// paths only to the rest.
	virtual bool ServesSchemePaths() const { return false; }
	virtual bool Exists(const VfsPath& path) const = 0;
	virtual std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const = 0;
	virtual std::vector<VfsPath> ListDir(const VfsPath& dir) const = 0;
	virtual std::optional<VfsStat> Stat(const VfsPath& path) const = 0;
};

// Host-directory backend. rootDir is a host path (absolute or relative to
// the process working directory). Serves vfsPath -> rootDir/vfsPath.
class LooseBackend final : public IVfsBackend {
public:
	explicit LooseBackend(std::string rootDir);
	std::string Name() const override { return "loose:" + rootDir_; }
	bool Exists(const VfsPath& path) const override;
	std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const override;
	std::vector<VfsPath> ListDir(const VfsPath& dir) const override;
	std::optional<VfsStat> Stat(const VfsPath& path) const override;
	const std::string& rootDir() const { return rootDir_; }

private:
	std::string rootDir_;
};

// In-memory backend: tests, cooker staging, embedded fallbacks.
class MemBackend final : public IVfsBackend {
public:
	explicit MemBackend(std::string name = "mem");
	std::string Name() const override { return name_; }
	bool Exists(const VfsPath& path) const override;
	std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const override;
	std::vector<VfsPath> ListDir(const VfsPath& dir) const override;
	std::optional<VfsStat> Stat(const VfsPath& path) const override;
	void Write(const VfsPath& path, std::vector<uint8_t> bytes);
	bool Erase(const VfsPath& path);

private:
	std::string name_;
	mutable std::mutex mutex_;
	std::unordered_map<std::string, std::vector<uint8_t>> files_;
};

struct VfsMount {
	int priority = 0; // lower value = higher precedence (checked first)
	std::shared_ptr<IVfsBackend> backend;
	std::string name;
};

// Overlay filesystem. First-match-wins across mounts ordered by priority.
// Overlapping VFS paths in several mounts are legal (loose dev overrides on
// top of paks); use FindProviders() to diagnose collisions.
class OverlayVfs {
public:
	OverlayVfs() = default;

	void Mount(std::shared_ptr<IVfsBackend> backend, int priority, std::string name);
	bool Unmount(const std::string& name);
	void Clear();

	bool Exists(const VfsPath& path) const;
	std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const;
	// Throws std::runtime_error naming the path and mounted backends.
	std::vector<uint8_t> ReadBinary(const VfsPath& path) const;
	// Union of ListDir across mounts (deduplicated, sorted).
	std::vector<VfsPath> ListDir(const VfsPath& dir) const;
	std::optional<VfsStat> Stat(const VfsPath& path) const;
	// Names of every mount that contains path (for collision diagnostics).
	std::vector<std::string> FindProviders(const VfsPath& path) const;
	std::vector<VfsMount> Mounts() const;

private:
	mutable std::mutex mutex_;
	std::vector<VfsMount> mounts_;
};

namespace vfs {
// Normalizes to posix-relative form. Returns "" when the input is empty,
// absolute (leading '/' or drive letter), or escapes the root via '..'.
// Inputs with the reserved //flintbase/ scheme keep the (lowercased)
// prefix verbatim and normalize the remainder; bare unknown authorities
// ("//other/...") remain invalid.
VfsPath Normalize(const std::string& path);
inline bool IsValid(const std::string& path) { return !Normalize(path).empty(); }
VfsPath Join(const std::string& a, const std::string& b);
VfsPath BaseDir(const VfsPath& path);
VfsPath FileName(const VfsPath& path);
} // namespace vfs

} // namespace smvk
