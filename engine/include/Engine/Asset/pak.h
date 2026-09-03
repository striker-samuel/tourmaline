#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Engine/Vfs/vfs.h>

namespace smvk {

// ---------------------------------------------------------------------------
// .pak container: a mountable mini-filesystem for cooked clusters.
//
// Layout (all integers little-endian, all blobs 16-byte aligned):
//
//   Header  (32 B): magic "SMPK", u32 version, u32 flags, u64 tocOffset,
//                   u64 tocCount
//   Blobs   : [payload bytes ...] (opaque to this layer; see
//             Engine/Asset/cluster_serializer.h for Scene/Mesh/Material/
//             Texture blob encodings)
//   TOC     : tocCount × { u64 pathHash, u32 kind, u32 flags,
//                          u64 offset, u64 size, u32 pathLen, path bytes }
//   Footer  (24 B): u64 tocOffset, u64 tocCount, u32 crc32, u32 magic "KPM S"?
//                   (footer magic "KPMS", crc covers header+blobs+TOC)
//
// TOC pathHash = FNV-1a64 over the normalized VFS path (textures: the
// original file path, e.g. "textures/brick.ktx2"). The full path string is
// kept in the TOC for debugging and collision-safe lookup.
//
// VFS interop: PakBackend serves *texture* entries through the IVfsBackend
// interface (returns the embedded .ktx2 bytes for the original VFS path),
// so materials in one pak can reference textures stored in another pak via
// plain VFS paths. Scene/Mesh/Material blobs are cluster-internal and are
// consumed through PakReader by the cluster deserializer, not via VFS
// file reads.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kPakVersion = 1;
inline constexpr uint32_t kPakMagic = 0x4B504D53; // "SMPK" LE
inline constexpr uint32_t kPakFooterMagic = 0x534D504B; // "KPMS" LE

enum class PakAssetKind : uint32_t {
	Scene = 1,
	Mesh = 2,
	Material = 3,
	Texture = 4,
};

enum PakEntryFlags : uint32_t {
	PakEntryFlag_None = 0,
};

struct PakTocEntry {
	uint64_t pathHash = 0;
	PakAssetKind kind = PakAssetKind::Scene;
	uint32_t flags = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	VfsPath path;
};

uint64_t PakHashPath(const std::string& normalizedPath);
uint32_t PakCrc32(const void* data, size_t size);

// TextureBlob payload encoding (shared by the serializer and PakBackend):
//   u32 uriLen | uri bytes | i32 magFilter | i32 minFilter | i32 wrapS |
//   i32 wrapT | u64 ktxSize | ktx bytes
// Returns false when the payload is malformed.
struct PakTexturePayload {
	std::string uri;
	int32_t magFilter = -1;
	int32_t minFilter = -1;
	int32_t wrapS = -1;
	int32_t wrapT = -1;
	std::span<const uint8_t> ktxBytes;
};
bool ParseTexturePayload(std::span<const uint8_t> payload, PakTexturePayload& out);
std::vector<uint8_t> BuildTexturePayload(
	const std::string& uri, int32_t magFilter, int32_t minFilter,
	int32_t wrapS, int32_t wrapT, std::span<const uint8_t> ktxBytes);

// Incremental pak builder (cooker side).
class PakWriter {
public:
	PakWriter() = default;
	// Payloads are copied. Path is normalized; empty/invalid paths fail.
	bool AddBlob(PakAssetKind kind, const std::string& path, std::vector<uint8_t> payload);
	size_t EntryCount() const { return entries_.size(); }
	const std::vector<PakTocEntry>& Entries() const { return metas_; }
	// Serializes header+blobs+TOC+footer. Throws on failure.
	std::vector<uint8_t> Finalize() const;

private:
	struct Pending {
		PakTocEntry meta;
		std::vector<uint8_t> payload;
	};
	std::vector<Pending> entries_;
	std::vector<PakTocEntry> metas_; // mirrors entries_[].meta (stable order)
};

// Parsed pak view over borrowed bytes (the caller keeps the image alive;
// PakBackend keeps a shared image for this reason).
class PakReader {
public:
	// Validates magic/version/sizes/CRC. Nullopt on failure (reason logged).
	static std::optional<PakReader> Open(std::span<const uint8_t> image, std::string& outError);
	size_t EntryCount() const { return entries_.size(); }
	const std::vector<PakTocEntry>& Entries() const { return entries_; }
	std::vector<const PakTocEntry*> BlobsOfKind(PakAssetKind kind) const;
	const PakTocEntry* Find(PakAssetKind kind, const std::string& normalizedPath) const;
	std::span<const uint8_t> Payload(const PakTocEntry& entry) const;

private:
	std::span<const uint8_t> image_;
	std::vector<PakTocEntry> entries_;
};

// IVfsBackend over a loaded pak image. Serves texture entries by their
// original VFS path (embedded .ktx2 bytes). All other kinds are invisible
// to VFS file reads (Exists() == false) — load them via PakReader.
class PakBackend final : public IVfsBackend {
public:
	// Takes shared ownership of the pak image bytes.
	static std::shared_ptr<PakBackend> Open(
		std::shared_ptr<const std::vector<uint8_t>> image,
		std::string name,
		std::string& outError);

	std::string Name() const override { return name_; }
	bool Exists(const VfsPath& path) const override;
	std::optional<std::vector<uint8_t>> TryRead(const VfsPath& path) const override;
	std::vector<VfsPath> ListDir(const VfsPath& dir) const override;
	std::optional<VfsStat> Stat(const VfsPath& path) const override;
	const PakReader& Reader() const { return *reader_; }

private:
	std::shared_ptr<const std::vector<uint8_t>> image_;
	std::optional<PakReader> reader_;
	std::string name_;
};

} // namespace smvk
