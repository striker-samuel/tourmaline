#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Engine/Asset/compression.h>
#include <Engine/Vfs/vfs.h>
#include <Engine/smvk.h> // TextureStreamMeta (v4 tail-upload digest form)

namespace smvk {

// ---------------------------------------------------------------------------
// .pak container: a mountable mini-filesystem for cooked clusters.
//
// Layout (all integers little-endian, all blobs 16-byte aligned):
//
//   Header  (32 B): magic "SMP5", u32 reserved, u32 flags, u64 tocOffset,
//                   u64 tocCount, u32 reserved
//   Blobs   : [payload bytes ...] (opaque to this layer; see
//             Engine/Asset/cluster_serializer.h for Scene/Mesh/Material/
//             Texture/Light blob encodings)
//   TOC     : tocCount × { u64 pathHash, u32 kind, u32 flags,
//                          u64 offset, u64 size, u32 pathLen, path bytes }
//   Footer  (24 B): u64 tocOffset, u64 tocCount, u32 crc32, u32 magic "KPM5"
//                   (crc covers header+blobs+TOC)
//
// NO FORMAT VERSIONS: the magic IS the version. A breaking format change
// bumps the trailing magic char (SMP4 -> SMP5, KPM4 -> KPM5) and the
// reader fails closed on mismatch — content is recooked from zero, never
// migrated. (The reserved header slots stay zero.)
// SMP5 (the decal-entity break): PakAssetKind::Decal blobs + the scene
// blob's v11 decal-link section.
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

// Mesh blobs carry trailing meshlet sections and the Scene blob a
// trailing static-subtree section (see cluster_serializer.cpp); a TOC
// entry with PakEntryFlag_Lz4 stores `u64 uncompressedSizeLE + LZ4 block
// bytes` (see compression.h) and the reader decompresses transparently.
// Mesh blobs carry PACKED SoA streams (VertexPos[] + VertexShading[] +
// u32[] — the exact GPU heap layout, plus blob-relative subrange offsets)
// stored UNCOMPRESSED, so the payload bytes are direct pak-image subspans
// and the loader stages them with pure offset math (BulkCopyBuffer
// content = the pak image). Texture blobs append a digest header (tail
// mip split) so the loader uploads only the coarse-mip tail; digest-less
// blobs (e.g. bakery-appended textures) keep the full-resident path.
// A breaking change to any of this bumps the magic below (never a
// version field); content recooks from zero.
inline constexpr uint32_t kPakMagic = 0x35504D53; // "SMP5" LE
inline constexpr uint32_t kPakFooterMagic = 0x354D504B; // "KPM5" LE

enum class PakAssetKind : uint32_t {
	Scene = 1,
	Mesh = 2,
	Material = 3,
	Texture = 4,
	// Cooked punctual light (KHR_lights_punctual). One blob per light,
	// keyed by its asset URI — loaded back into the Cluster lights pool
	// exactly like Mesh blobs (see cluster_serializer.cpp).
	Light = 5,
	// Cooked GI registry: one blob per pak naming the sidecar GI payloads
	// (probe volumes / lightmap atlases under content/gi/) the packed
	// scenes were cooked against. Invisible to the VFS backends (they only
	// serve Texture blobs); read via ParseGiRegistryBlob / ListPakScenes
	// style discovery, never via file reads.
	Gi = 6,
	// Authored camera (editor camera node). One blob per camera, keyed
	// by its asset URI — loaded back into the Cluster cameras pool
	// exactly like Light blobs (see cluster_serializer.cpp).
	Camera = 7,
	// Authored decal (editor/app decal node). One blob per decal, keyed
	// by its asset URI — loaded back into the Cluster decals pool
	// exactly like Light blobs (see cluster_serializer.cpp). The SHAPE
	// rides the node transform (scene-blob decal links), the blob carries
	// the look (texture URIs + material knobs).
	Decal = 8,
};

enum PakEntryFlags : uint32_t {
	PakEntryFlag_None = 0,
	// Blob payload is `u64 uncompressedSizeLE + LZ4 block bytes`
	// (see compression.h). Payload() returns the decompressed bytes.
	PakEntryFlag_Lz4 = 1u << 0,
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
// CRC-32 of A||B from the CRCs of the parts (zlib crc32_combine style:
// GF(2) zeros-operator matrices over the reflected CRC-32 polynomial).
// len2 is the byte length covered by crc2. Public for tests; the reader
// uses it to fold worker-computed chunk CRCs.
uint32_t PakCrc32Combine(uint32_t crc1, uint32_t crc2, uint64_t len2);
// Same result as PakCrc32, but computed chunk-parallel on a short-lived
// worker pool and folded with PakCrc32Combine. Used by PakReader::Open for
// multi-GB images where the serial table walk would dominate startup.
// chunkBytes overrides the split (default 8 MB; tests use small splits).
uint32_t PakCrc32Parallel(std::span<const uint8_t> data,
	size_t chunkBytes = 8ull * 1024ull * 1024ull);

// Per-blob placement of an appended payload (direct-read addressing:
// with PakEntryFlag_None the bytes at [offset, offset+size) ARE the
// payload, no TOC walk needed).
struct PakAppendInfo {
	uint64_t offset = 0;
	uint64_t size = 0;
};

// Append-grow: adds blobs to a pak FILE on disk (created when missing
// or empty), keeping the image valid for PakReader::Open at all times.
// Entries are stored UNCOMPRESSED so registry-tracked (offset, size)
// reads stay direct. Duplicate uris REPLACE their TOC entry (the TOC
// stays unique-by-path; first-match Find keeps resolving) — the
// superseded payload becomes dead space (cache GC compacts later).
//
// Crash ordering: payloads land after the old TOC+footer (nothing live
// is overwritten), the header patch is the final write — an
// interrupted append leaves the PREVIOUS pak contents valid (the
// header still points at the intact old TOC; trailing bytes are
// ignored). Torn header writes remain possible; the scratch store
// treats a pak that fails PakReader::Open as disposable.
// out receives one (offset, size) per blob, in order.
bool AppendPakBlobs(const std::string& pakPath, PakAssetKind kind,
	const std::vector<std::pair<std::string,
		std::span<const uint8_t>>>& blobs,
	std::vector<PakAppendInfo>& out, std::string& err);

	// TextureBlob payload encoding (shared by the serializer and PakBackend):
	// v3 shape:  u32 uriLen | uri bytes | i32 magFilter | i32 minFilter |
	//            i32 wrapS | i32 wrapT | u64 ktxSize | ktx bytes
	// v4 shape:  ... | i32 wrapT | u32 digestLen | digest bytes |
	//            u64 ktxSize | ktx bytes
	// digestLen == 0 (or no digest field at all) marks a legacy/digest-less
	// payload — the texture loads through the ktx-parse full-resident path.
	// digestLen > 0 carries the v4 tail-upload digest (see PakTextureDigest):
	// the loader creates the tail-resident image objects straight from it
	// (no KTX parse) and uploads only the coarse-mip tail; the embedded
	// ktx is always GPU-ready (transcoded at cook) and carries the tops
	// for the streamer's demand upgrades.
	// ParseTexturePayload accepts the v3 shape AND the digest-less v4 shape
	// (digest empty); ParseTexturePayloadV4 additionally requires + parses
	// the digest. Both shapes validate end-exact; false on any malformation.
	struct PakTexturePayload {
		std::string uri;
		int32_t magFilter = -1;
		int32_t minFilter = -1;
		int32_t wrapS = -1;
		int32_t wrapT = -1;
		std::span<const uint8_t> digest; // empty when digest-less
		std::span<const uint8_t> ktxBytes;
	};
	bool ParseTexturePayload(std::span<const uint8_t> payload, PakTexturePayload& out);
	bool ParseTexturePayloadV4(std::span<const uint8_t> payload, PakTexturePayload& out);
	std::vector<uint8_t> BuildTexturePayload(
		const std::string& uri, int32_t magFilter, int32_t minFilter,
		int32_t wrapS, int32_t wrapT, std::span<const uint8_t> ktxBytes);
	// v4 form (always writes digestLen, empty or not; the serializer always
	// passes a digest for transcodeable sources).
	std::vector<uint8_t> BuildTexturePayloadV4(
		const std::string& uri, int32_t magFilter, int32_t minFilter,
		int32_t wrapS, int32_t wrapT, std::span<const uint8_t> ktxBytes,
		std::span<const uint8_t> digest);

	// v4 tail-upload digest record (TextureStreamMeta serialization; fixed
	// u32/u64 fields, 16 B per region row):
	//   u32 kind(=1) | u32 tailFirstMip | u32 residentMips |
	//   u32 baseWidth | u32 baseHeight | u32 baseDepth | u32 numLevels |
	//   u32 ktxLayers | u32 numFaces | u32 ktxVkFormat | u32 transferSrgb |
	//   u32 dims | u32 cubemap | u32 regionCount |
	//   rows: u64 bufferOffset | u32 mip | u32 arrayLayer
	bool BuildTextureDigest(const TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, TextureStreamMeta& out);

	// v4 tail-upload digest record (TextureStreamMeta serialization):
	//   u32 kind(=1) | u32 tailFirstMip
	bool BuildTextureDigest(const TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, TextureStreamMeta& out);

	// v4 tail-upload digest record (TextureStreamMeta serialization; fixed
	// u32/u64 fields, 16 B per region row):
	//   u32 kind(=1) | u32 tailFirstMip | u32 residentMips |
	//   u32 baseWidth | u32 baseHeight | u32 baseDepth | u32 numLevels |
	//   u32 ktxLayers | u32 numFaces | u32 ktxVkFormat | u32 transferSrgb |
	//   u32 dims | u32 cubemap | u32 regionCount |
	//   rows: u64 bufferOffset | u32 mip | u32 arrayLayer
	bool BuildTextureDigest(const TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, TextureStreamMeta& out);

	// Incremental pak builder (cooker side).
	class PakWriter {
	public:
		PakWriter() = default;
		// Payloads are copied. Path is normalized; empty/invalid paths fail.
		// storeUncompressed forces flags == None regardless of SetCompression:
		// v4 direct-addressed blobs (packed-SoA mesh streams) must stay
		// byte-identical subspans of the pak image — compressing them would
		// break the offset-table load path.
		bool AddBlob(PakAssetKind kind, const std::string& path, std::vector<uint8_t> payload,
			bool storeUncompressed = false);
		// LZ4 compression for subsequently added blobs (default: None).
		// Blobs below kCompressMinBytes or that do not shrink stay stored
		// verbatim (flags == None), so tiny blobs never grow.
		void SetCompression(PakCompression mode, int lz4Acceleration = 1) {
			compression_ = mode;
			lz4Acceleration_ = lz4Acceleration < 1 ? 1 : lz4Acceleration;
		}
		PakCompression Compression() const { return compression_; }
		size_t EntryCount() const { return entries_.size(); }
		size_t CompressedEntryCount() const;
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
		PakCompression compression_ = PakCompression::None;
		int lz4Acceleration_ = 1;
	};

// Parsed pak view over borrowed bytes (the caller keeps the image alive;
// PakBackend keeps a shared image for this reason). LZ4 blobs are
// decompressed once at Open() into reader-owned buffers; Payload()
// transparently returns decompressed bytes for flagged entries.
class PakReader {
public:
	// Validates magic/version/sizes/CRC. Nullopt on failure (reason logged).
	static std::optional<PakReader> Open(std::span<const uint8_t> image, std::string& outError);
	size_t EntryCount() const { return entries_.size(); }
	size_t CompressedEntryCount() const;
	const std::vector<PakTocEntry>& Entries() const { return entries_; }
	std::vector<const PakTocEntry*> BlobsOfKind(PakAssetKind kind) const;
	const PakTocEntry* Find(PakAssetKind kind, const std::string& normalizedPath) const;
	std::span<const uint8_t> Payload(const PakTocEntry& entry) const;
	// Base of the loaded image. UNCOMPRESSED entry payloads are subspans of
	// this base at entry.offset — the v4 fast-load path stages geometry
	// straight from here with pure offset math (BulkCopyBuffer content).
	const uint8_t* ImageData() const { return image_.data(); }

private:
	std::span<const uint8_t> image_;
	std::vector<PakTocEntry> entries_;
	// Decompressed bytes for entries with PakEntryFlag_Lz4, parallel to
	// entries_ (empty for uncompressed entries).
	std::vector<std::vector<uint8_t>> decompressed_;
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
