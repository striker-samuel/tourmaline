// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

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
#include <Engine/smvk.h> // smvk::TextureStreamMeta (v4 tail-upload digest form)

#include <mutex>

namespace tour::asset {
// (namespace/RHI boundary audit) fully qualified - no broad
// using-directive (R6-gated).

// ---------------------------------------------------------------------------
// .pak container: a mountable mini-filesystem for cooked clusters.
//
// Layout (all integers little-endian, all blobs 16-byte aligned):
//
//   Header  (32 B): magic "SMP8", u32 reserved, u32 flags, u64 tocOffset,
//                   u64 tocCount, u32 reserved
//   Blobs   : [payload bytes ...] (opaque to this layer; see
//             Engine/Asset/cluster_serializer.h for Scene/Mesh/Material/
//             Texture/Light blob encodings). Each blob is a PAGE: an
//             independently verifiable, independently readable extent -
//             the per-entry contentCrc in the TOC is its integrity, and
//             PakReader::OpenFile reads any single page positionally
//             without the rest of the file.
//   TOC     : tocCount × { u64 pathHash, u32 kind, u32 flags,
//                          u64 offset, u64 size, u32 slot,
//                          u32 contentCrc, u32 pathLen, path bytes }
//   Footer  (24 B): u64 tocOffset, u64 tocCount, u32 tocCrc,
//                   u32 magic "KPM8"
//                   (tocCrc covers ONLY the TOC bytes - the mount
//                   validates the small metadata cheaply; every blob
//                   verifies through its own contentCrc at FETCH time,
//                   the lazy per-page contract. VerifyAll() is the
//                   explicit full-image policy for deployment gates.)
//
// NO FORMAT VERSIONS: the magic IS the version. A breaking format change
// bumps the trailing magic char and the reader fails closed on
// mismatch — content is recooked from zero, never migrated. (The
// reserved header slots stay zero.) ONE CANONICAL PATH: the reader
// accepts exactly ONE magic; there are no legacy-epoch acceptances.
// SMP5 (the decal-entity break): PakAssetKind::Decal blobs + the scene
//   blob's v11 decal-link section.
// SMP6 (the tiled-terrain break): PakAssetKind::Terrain blobs (H-tile
//   height/mask payloads + the terrain/meta blob).
// SMP7 (the scene-tables break): the pak scene blob's mesh/material
//   link sections are sourced from the typed scene tables (and the
//   legacy .scene format's association sections went row-native).
// SMP8 (the paged break): per-entry contentCrc (lazy page verification
//   replaces the whole-image CRC at mount), stable per-entry slots,
//   the PakAssetKind::Plan mesh-plan join blob, and the file-backed
//   OpenFile (positional page reads). Every SMP7-or-older pak fails
//   closed at the magic - content recooks from zero.
//
// TOC pathHash = FNV-1a64 over the normalized VFS path (textures: the
// original file path, e.g. "textures/brick.ktx2"). The full path string is
// kept in the TOC for debugging and collision-safe lookup. `slot` is the
// entry's STABLE ordinal (assigned at cook, preserved across appends) -
// the compiled-plan addressing of the pak-vNext work keys pages by slot,
// never by TOC position. `contentCrc` is the CRC-32 of the entry's STORED
// bytes (the LZ4 wrapper included when compressed).
//
// VFS interop: PakBackend serves *texture*, *sound* and *shader*
// entries through the smvk::IVfsBackend interface (returns the embedded
// .ktx2 / raw .wav / raw compiled-shader bytes for the original VFS
// path), so materials in one pak can reference textures stored in
// another pak via plain VFS paths, emitters the same for sounds, and
// the shader library the same for engine/project shader packages.
// Scene/Mesh/Material blobs are cluster-internal and are consumed
// through PakReader by the cluster deserializer, not via VFS file
// reads.
// ---------------------------------------------------------------------------

// Mesh blobs carry trailing meshlet sections and the Scene blob a
// trailing static-subtree section (see cluster_serializer.cpp); a TOC
// entry with PakEntryFlag_Lz4 stores `u64 uncompressedSizeLE + LZ4 block
// bytes` (see compression.h) and the reader decompresses transparently.
// Mesh blobs carry PACKED SoA streams (PackedVertexPosition[] + PackedVertexShading[] +
// u32[] — the exact GPU heap layout, plus blob-relative subrange offsets)
// stored UNCOMPRESSED, so the payload bytes are direct pak-image subspans
// and the loader stages them with pure offset math (BulkCopyBuffer
// content = the pak image). Texture blobs append a digest header (tail
// mip split) so the loader uploads only the coarse-mip tail; digest-less
// blobs (e.g. bakery-appended textures) keep the full-resident path.
// A breaking change to any of this bumps the magic below (never a
// version field); content recooks from zero.
inline constexpr uint32_t kPakMagic = 0x38504D53; // "SMP8" LE
inline constexpr uint32_t kPakFooterMagic = 0x384D504B; // "KPM8" LE
// The per-pak MESH PLAN blob's reserved TOC path (SMP8): the cooked
// family -> member join (see PakMeshPlanFamily). A fixed reserved
// path, not a cluster-local one - one plan per PAK.
inline constexpr const char* kPakMeshPlanPath = "#meshplan";
// The per-pak IDENTITY TABLE blob's reserved TOC path: the pak's
// content-identity rows (see PakAssetIdRow + WriteAssetIdsBlob) —
// the discovery lane's metadata (a fixed reserved path, one per pak,
// exactly like #meshplan / #schemas).
inline constexpr const char* kPakAssetIdsPath = "#ids";

// A breaking change to any of this bumps the pak magic (SMP8 is the
// current epoch - the paged break: per-page contentCrc, stable slots,
// the mesh plan, the schema registry); content recooks from zero.
enum class PakAssetKind : uint32_t {
	Scene = 1,
	Mesh = 2,
	Material = 3,
	Texture = 4,
	// Cooked punctual light (KHR_lights_punctual). One blob per light,
	// keyed by its asset URI — loaded back into the Cluster lights pool
	// exactly like Mesh blobs (see cluster_serializer.cpp).
	Light = 5,
	// Cooked GI registry: one blob per pak naming the GI bake payloads
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
	// Tiled terrain (SMP6): H-tile height/mask payloads + the single
	// terrain/meta blob. Paths are terrain/h_XX_ZZ, terrain/m_XX_ZZ and
	// terrain/meta (see Rendergraph/terrain_meta.h). Stored UNCOMPRESSED
	// or LZ4 like any other blob (height RGBA8 deltas compress well).
	Terrain = 9,
	// LEVEL (the exploration spike): ONE composite blob per pak packing
	// a playable unit — the scene payload + its baked data blobs
	// (cubemap set manifest + KTX2 rows, GI payloads, terrain meta...)
	// as inline sub-blobs addressed by their canonical keys
	// (gi/cubemaps/<id>.* etc.). The LEVEL owns the data (ID-keyed:
	// levels/<name>.level); the scene-stem bake-file association dies at
	// the pak level. See Asset/level.h for the blob format + accessors.
	Level = 10,
	// SOUND (the audio asset kind): one blob per .wav sound asset,
	// keyed by its original VFS path, payload = the raw .wav bytes
	// (LoadWavFromMemory decodes whatever SDL accepts — any rate/
	// channel count/bit depth, converted at load). The SECOND VFS-
	// addressable kind (with Texture): PakBackend serves Sound blobs
	// by path so cross-pak sound resolution + the runtime façade's
	// ReadAsset(project::sounds::*) flow exactly like textures. The
	// COOKED SCENE references sounds by their stable numeric content
	// id (the scene blob's audio section) — never by path — and the
	// appmanifest's [ids] table carries the id -> (pak, path) join.
	Sound = 11,
	// SHADER (the compiled-shader asset kind): one blob per compiled
	// shader stage/variant, keyed by its original VFS path (the same
	// "content/shaders/<file>.spv" strings every consumer already
	// uses), payload = the raw compiled bytecode (SPIR-V on the
	// Vulkan backend — NEVER wrapped in a texture payload). The THIRD
	// VFS-addressable kind (with Texture + Sound): PakBackend serves
	// Shader blobs by path through the same Exists/TryRead/ListDir/
	// Stat surface, and the engine shader library (Engine/Utils/
	// shader_library.h) reads + VALIDATES them (the SPIR-V magic on
	// the Vulkan backend; DXIL carries its own DXBC magic). The
	// engine-owned package (engine-shaders.pak — the build's
	// declared-output list) is a MANDATORY runtime dependency of
	// every graphics host.
	Shader = 12,
	// PLAN (SMP8, the cooked mesh-plan join): ONE blob per pak at the
	// reserved path kPakMeshPlanPath ("#meshplan") carrying the
	// family -> member table the cook resolves (see
	// WriteMeshPlanBlob/ParseMeshPlanBlob): which Mesh blobs make up
	// each .geometry family, with LOD indices. The runtime mesh join
	// (the AssetStore's family acquire) reads the PLAN - never a TOC
	// string-prefix scan (the consolidated architecture's "resolve
	// ambiguity while cooking; no runtime name search"). Cluster-
	// internal like Scene/Mesh: consumed through PakReader, invisible
	// to the VFS backends.
	Plan = 13,
	// SCHEMA (the project-defined node-schema registry): ONE blob per
	// pak at the reserved path kPakSchemaRegistryPath ("#schemas")
	// carrying the cooked schema LAYOUTS (Engine/Scene/node_schema.h)
	// the project's scenes were authored against — the runtime + the
	// generated project::types PODs decode the scene blob's v19
	// schema-association rows against it. Cluster-internal, invisible
	// to the VFS backends.
	Schema = 14,
	// IDENTITY (the per-pak content-identity table): ONE blob per pak
	// at the reserved path kPakAssetIdsPath ("#ids") carrying the
	// pak's PakAssetIdRow set — the stable ids + kinds + logical paths
	// (and optionally the cooked scene names) the pak OWNS. The
	// DISCOVERY lane's metadata (the .tapp [ids] successor): a
	// session builds its record table from the paks themselves
	// (ContentRegistry::DiscoverPak — the light mount + one page
	// read, no manifest, no whole-image load). Cluster-internal,
	// invisible to the VFS backends.
	Identity = 15,
	// AUDIOGRAPH (the authored audio-graph asset kind): one blob per
	// .audiograph file, keyed by its original VFS path, payload = the
	// AUTHORED TEXT VERBATIM (the strict parser — engine/Engine/Asset/
	// audiograph.h — validates it at pack time, the corrupt-refused
	// discipline; the runtime parses after ReadAsset). The FOURTH
	// VFS-addressable kind (with Texture + Sound + Shader): PakBackend
	// serves AudioGraph blobs by path so graphs resolve cross-pak
	// exactly like sounds. A graph's SOURCE nodes reference sound
	// assets by VFS path (resolved loudly at instance create through
	// the same registry), never by stamped numeric id.
	AudioGraph = 16,
	SpriteSet = 17,
	StateMachine = 18, // immutable authored gameplay transition graph
	NavMesh = 19,      // immutable precomputed navigation triangles
	Sdf = 20,          // versioned mesh-local .geometry.sdf derived volume
	Shape = 21,        // authored .shape spline / point cloud / primitive text
	ShapePlacements = 22, // cooked .tour scene-owned placements (not VFS)
	MapTile = 23, // cooked Map tile placement pages (logical world-cell content, not VFS)
	MapManifest = 24, // cooked world-wide Map ID, tile count and persistent placement index
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
	// SMP8: the entry's STABLE ordinal (assigned at cook, preserved
	// across AppendPakBlobs grows) and the CRC-32 of its STORED bytes
	// (the page integrity - verified at FETCH time, never at mount).
	uint32_t slot = 0;
	uint32_t contentCrc = 0;
	smvk::VfsPath path;
};

uint64_t PakHashPath(const std::string& normalizedPath);
uint32_t PakCrc32(const void* data, size_t size);
// The shared VFS serve shape (PakBackend::TryRead + the PAGER lane of
// ProjectRuntime::ReadAsset): the kind-appropriate bytes for a
// TEXTURE / SOUND / SHADER entry's payload (texture: the envelope's
// embedded .ktx2; sound + shader: the raw payload). Nullopt on a
// malformed texture envelope.
std::optional<std::vector<uint8_t>> PakServeVfsBytes(
	PakAssetKind kind, std::span<const uint8_t> payload,
	const std::string& path);
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

// ---- THE COOKED MESH PLAN (SMP8, the family -> member join) ---------------
// One Plan-kind blob per pak (kPakMeshPlanPath) mapping each .geometry
// FAMILY (the [ids] mesh row's path - the public MeshKey's identity)
// to its cooked MEMBER blobs (base + /lodN siblings) with LOD indices.
// The cook resolves the ambiguity ONCE; the runtime join is a plan
// lookup, never a TOC string-prefix scan (the consolidated content
// architecture's contract).
struct PakMeshPlanMember {
	std::string tocPath; // the Mesh blob's full pak URI
	uint32_t lodIndex = 0; // 0 = the base mesh
};
struct PakMeshPlanFamily {
	std::string familyPath; // the .geometry path (the [ids] row path)
	std::vector<PakMeshPlanMember> members;
};
// Blob encoding (LE): 'MPLN' u32 magic | u32 version(=1) | u32
// familyCount | per family: u32 pathLen | path | u32 memberCount |
// per member: u32 pathLen | tocPath | u32 lodIndex. End-exact.
std::vector<uint8_t> WriteMeshPlanBlob(
	const std::vector<PakMeshPlanFamily>& families);
bool ParseMeshPlanBlob(std::span<const uint8_t> blob,
	std::vector<PakMeshPlanFamily>& out, std::string& err);

// ---- THE PER-PAK IDENTITY TABLE (the "#ids" registry blob) ---------------
// One Identity-kind blob per pak (kPakAssetIdsPath) carrying the
// CONTENT-IDENTITY rows the pak owns — the per-pak successor of the
// .tapp [ids] table (the consolidated content architecture's
// discovery lane): a session can build its whole record table from
// the paks themselves, no manifest. `id` + `kind` use the SHARED ABI
// (smvk::kCatalogKind* — the same bytes the manifest's kind strings
// and the generated project.h's AssetKind enum carry); `path` is the
// LOGICAL path (project-relative, the join key); scene rows MAY
// carry the cooked scene's name (empty = the single-scene inference
// the scene join already performs — multi-scene paks without names
// refuse at load time, recook).
struct PakAssetIdRow {
	uint64_t id = 0;
	uint8_t kind = 0;          // Nil until assigned a real catalog kind
	std::string path;          // the logical path (project-relative)
	std::string sceneName;     // scene rows: the cooked name (may be empty)
};
// Blob encoding (LE): 'IDTR' magic | u32 version(=1) | u32 rowCount |
// per row: u64 id | u8 kind | u32 pathLen | path bytes |
// u32 nameLen | name bytes. End-exact.
[[nodiscard]] std::vector<uint8_t> WriteAssetIdsBlob(
	const std::vector<PakAssetIdRow>& rows);
// False + err on: short/malformed input, a bad magic/version, a row
// whose id is 0, whose kind is Nil/unknown, or whose path is
// empty. End-exact (trailing bytes = malformed).
bool ParseAssetIdsBlob(std::span<const uint8_t> blob,
	std::vector<PakAssetIdRow>& out, std::string& err);

// Append-grow: adds blobs to a pak FILE on disk (created when missing
// or empty), keeping the image valid for PakReader::Open at all times.
// Entries are stored UNCOMPRESSED so registry-tracked (offset, size)
// reads stay direct. Duplicate uris REPLACE their TOC entry (the TOC
// stays unique-by-path; first-match Find keeps resolving) — the
// superseded payload becomes dead space (cache GC compacts later).
//
// Crash ordering (read exactly — the old text overpromised): payloads
// land after every live old byte (nothing live is overwritten), then
// the new TOC, then the header patch, then the footer + the
// truncating resize. PakReader::Open accepts the footer ONLY at the
// physical end of the file, so from the FIRST payload write until the
// final footer+resize the old footer is buried and an interrupted
// append leaves a file that FAILS Open. The previous contents' BYTES
// all survive — direct (offset, size) reads (the artifact registry's
// addressing) keep resolving across any crash — and a later append
// heals the file with a fresh footer. No in-place write ordering can
// do better: the footer must end up at the new end, so mid-append the
// file is inherently footer-less; a crash-safe grow would need
// build-in-temp + atomic replace, which the cache's append frequency
// against multi-GB class paks does not justify. Torn header writes
// remain possible (the header is the one live range rewritten); the
// scratch store treats a pak that fails PakReader::Open as disposable
// and re-cooks from sources.
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

	// v4 tail-upload digest record (smvk::TextureStreamMeta serialization; fixed
	// u32/u64 fields, 16 B per region row):
	//   u32 kind(=1) | u32 tailFirstMip | u32 residentMips |
	//   u32 baseWidth | u32 baseHeight | u32 baseDepth | u32 numLevels |
	//   u32 ktxLayers | u32 numFaces | u32 ktxVkFormat | u32 transferSrgb |
	//   u32 dims | u32 cubemap | u32 regionCount |
	//   rows: u64 bufferOffset | u32 mip | u32 arrayLayer
	bool BuildTextureDigest(const smvk::TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, smvk::TextureStreamMeta& out);

	// v4 tail-upload digest record (smvk::TextureStreamMeta serialization):
	//   u32 kind(=1) | u32 tailFirstMip
	bool BuildTextureDigest(const smvk::TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, smvk::TextureStreamMeta& out);

	// v4 tail-upload digest record (smvk::TextureStreamMeta serialization; fixed
	// u32/u64 fields, 16 B per region row):
	//   u32 kind(=1) | u32 tailFirstMip | u32 residentMips |
	//   u32 baseWidth | u32 baseHeight | u32 baseDepth | u32 numLevels |
	//   u32 ktxLayers | u32 numFaces | u32 ktxVkFormat | u32 transferSrgb |
	//   u32 dims | u32 cubemap | u32 regionCount |
	//   rows: u64 bufferOffset | u32 mip | u32 arrayLayer
	bool BuildTextureDigest(const smvk::TextureStreamMeta& meta, std::vector<uint8_t>& out);
	bool ParseTextureDigest(std::span<const uint8_t> digest, smvk::TextureStreamMeta& out);

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
		void SetCompression(smvk::PakCompression mode, int lz4Acceleration = 1) {
			compression_ = mode;
			lz4Acceleration_ = lz4Acceleration < 1 ? 1 : lz4Acceleration;
		}
		smvk::PakCompression Compression() const { return compression_; }
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
		smvk::PakCompression compression_ = smvk::PakCompression::None;
		int lz4Acceleration_ = 1;
	};

// Parsed pak view over borrowed bytes (the caller keeps the image alive;
// PakBackend keeps a shared image for this reason). LZ4 blobs are
// decompressed LAZILY on first Payload() access into reader-owned
// buffers (guarded by an internal mutex; the decompressed bytes stay
// cached for the reader's lifetime — a returned span is never
// invalidated). Selective by construction: a pak whose consumer only
// touches one scene never pays for the other entries' decompression
// (the old eager Open() decompressed EVERY compressed entry up front,
// dominating preload on multi-GB paks). Decompression failure at
// Payload() time throws (the CRC already verified the image at Open, so
// a bad LZ4 block means a torn write that passed CRC — loud, and the
// host's load path turns it into a failed launch, not a corrupt scene).
//
// SMP8 PAGE VERIFICATION: Open validates the small metadata only
// (magic/header/footer agreement + the TOC CRC); every entry's
// contentCrc verifies at FIRST Payload() fetch (cached - once per
// reader), so a corrupt page fails loudly AT THE PAGE, and a consumer
// that never touches the page never pays its hash. VerifyAll() is the
// explicit whole-image policy (deployment gates). A reader opened over
// a FILE (OpenFile) has no resident image: every fetch is a positional
// page read + verify + cache - the pager's container primitive.
class PakReader {
public:
	// Validates magic/header/footer/TOC (small + cheap). Nullopt on
	// failure (reason logged). Page bytes verify lazily at Payload.
	static std::optional<PakReader> Open(std::span<const uint8_t> image, std::string& outError);
	// FILE-BACKED open: reads ONLY the header + footer + TOC from the
	// file; every Payload() fetch is a positional page read (verify +
	// cache, lifetime-stable spans like the image-backed form). The
	// page-level container primitive the async pager builds on. A
	// file-backed reader has NO image: ImageData() is null and
	// image-relative consumers (the direct-addressed mesh fast path)
	// must use the image-backed Open.
	static std::optional<PakReader> OpenFile(const std::string& path, std::string& outError);
	~PakReader();
	PakReader(PakReader&&) noexcept;
	PakReader& operator=(PakReader&&) noexcept;
	PakReader(const PakReader&) = delete;
	PakReader& operator=(const PakReader&) = delete;
	size_t EntryCount() const { return entries_.size(); }
	size_t CompressedEntryCount() const;
	const std::vector<PakTocEntry>& Entries() const { return entries_; }
	std::vector<const PakTocEntry*> BlobsOfKind(PakAssetKind kind) const;
	const PakTocEntry* Find(PakAssetKind kind, const std::string& normalizedPath) const;
	// The entry by its STABLE slot (the compiled-plan addressing):
	// O(1) through the slot index built at open - never a TOC walk.
	// Null when no entry carries the slot.
	const PakTocEntry* FindBySlot(uint32_t slot) const;
	std::span<const uint8_t> Payload(const PakTocEntry& entry) const;
	// The explicit full-verification policy: hashes every entry's
	// stored bytes against its contentCrc (image-backed: in-memory;
	// file-backed: positional reads). False + the first failing path
	// on any corrupt page. NOT the default - the lazy per-page checks
	// own runtime integrity; this is the deployment-gate form.
	bool VerifyAll(std::string& outError) const;
	// The cooked mesh-plan join (the Plan blob at kPakMeshPlanPath):
	// the family's members (base + lodN), or false + err when the pak
	// carries no plan / no such family (a version skew between the
	// catalog and the cooked content - recook).
	bool MeshPlanFamily(const std::string& familyPath,
		std::vector<PakMeshPlanMember>& outMembers,
		std::string& outError) const;
	// Base of the loaded image (null for file-backed readers).
	// UNCOMPRESSED entry payloads are subspans of this base at
	// entry.offset — the v4 fast-load path stages geometry straight
	// from here with pure offset math (BulkCopyBuffer content).
	const uint8_t* ImageData() const { return image_.data(); }
	// True when the reader was opened image-backed (an image exists
	// to borrow spans from).
	bool HasImage() const { return image_.data() != nullptr; }

private:
	PakReader() = default;
	struct FileHandle; // the fstream wrapper (defined in the impl)
	// The slot index builder (both open paths share it).
	static void BuildSlotIndex(PakReader& r);
	std::span<const uint8_t> image_;
	std::vector<PakTocEntry> entries_;
	// Slot -> entries_ index (built at open; -1 = the slot is
	// unclaimed). The compiled-plan lane's O(1) addressing - never a
	// TOC walk.
	std::vector<int32_t> entriesBySlot_;
	// Lazy per-entry payload materialization (parallel to entries_;
	// sized at Open, filled under lazyMutex_ on first Payload()
	// access and kept for the reader's lifetime — the spans Payload()
	// returns must stay valid, so the cache never evicts). For an
	// image-backed reader only LZ4 entries land here; a FILE-backed
	// reader caches EVERY fetched page (there is no image to borrow
	// from). `done_` marks "materialized" separately from "empty
	// result". Mutable: Payload() is const (callers are const).
	mutable std::vector<std::vector<uint8_t>> decompressed_;
	mutable std::vector<uint8_t> decompressDone_;
	// Per-entry page verification (SMP8): contentCrc checked once per
	// reader at first fetch.
	mutable std::vector<uint8_t> verified_;
	mutable std::unique_ptr<std::mutex> lazyMutex_;
	// File-backed mode: the open file (null for image-backed).
	std::unique_ptr<FileHandle> file_;
};

// smvk::IVfsBackend over a loaded pak image. Serves texture entries by their
// original VFS path (embedded .ktx2 bytes). All other kinds are invisible
// to VFS file reads (Exists() == false) — load them via PakReader.
class PakBackend final : public smvk::IVfsBackend {
public:
	// Takes shared ownership of the pak image bytes.
	static std::shared_ptr<PakBackend> Open(
		std::shared_ptr<const std::vector<uint8_t>> image,
		std::string name,
		std::string& outError);

	std::string Name() const override { return name_; }
	bool Exists(const smvk::VfsPath& path) const override;
	std::optional<std::vector<uint8_t>> TryRead(const smvk::VfsPath& path) const override;
	std::vector<smvk::VfsPath> ListDir(const smvk::VfsPath& dir) const override;
	std::optional<smvk::VfsStat> Stat(const smvk::VfsPath& path) const override;
	const PakReader& Reader() const { return *reader_; }
	// The shared pak image (the reader borrows it; cluster loads pin it
	// so tail-borrowed payloads outlive the backend). Never null on a
	// successfully opened backend.
	[[nodiscard]] std::shared_ptr<const std::vector<uint8_t>> Image()
		const {
		return image_;
	}

private:
	std::shared_ptr<const std::vector<uint8_t>> image_;
	std::optional<PakReader> reader_;
	std::string name_;
};

} // namespace tour::asset


// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the pak
// format is ENGINE-OWNED and lives in tour::asset; the smvk aliases
// keep the existing call sites compiling through the consumer
// migration.
namespace smvk {
	using ::tour::asset::PakAssetKind;
	using ::tour::asset::PakEntryFlags;
	using ::tour::asset::PakTocEntry;
	using ::tour::asset::PakHashPath;
	using ::tour::asset::PakCrc32;
	using ::tour::asset::PakServeVfsBytes;
	using ::tour::asset::PakCrc32Combine;
	using ::tour::asset::PakCrc32Parallel;
	using ::tour::asset::PakAppendInfo;
	using ::tour::asset::PakMeshPlanMember;
	using ::tour::asset::PakMeshPlanFamily;
	using ::tour::asset::WriteMeshPlanBlob;
	using ::tour::asset::ParseMeshPlanBlob;
	using ::tour::asset::PakTexturePayload;
	using ::tour::asset::PakWriter;
	using ::tour::asset::PakReader;
	using ::tour::asset::PakBackend;
	using ::tour::asset::kPakMagic;
	using ::tour::asset::kPakFooterMagic;
	using ::tour::asset::kPakMeshPlanPath;
	using ::tour::asset::PakEntryFlag_None;
	using ::tour::asset::PakEntryFlag_Lz4;
	using ::tour::asset::BuildTexturePayload;
	using ::tour::asset::ParseTexturePayload;
	using ::tour::asset::BuildTexturePayloadV4;
	using ::tour::asset::ParseTexturePayloadV4;
	using ::tour::asset::BuildTextureDigest;
	using ::tour::asset::ParseTextureDigest;
	using ::tour::asset::AppendPakBlobs;
}
