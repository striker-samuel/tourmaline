// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// THE MESH IMPORTER: glTF -> the engine-native MESH ARTIFACT family
// (Unity-style preprocessed assets). This is the ONLY glTF-aware import
// surface in the codebase — every consumer downstream (editor sessions,
// PakCooker, GiBaker, ApplySceneReferences) builds from the ARTIFACTS:
//
//   cameras, materials, lights, geometry, animations — ALL of it rides
//   the derived asset set; the glTF never parses outside this module.
//
// Importing a glTF writes PREPROCESSED assets next to the source (the
// glTF itself stays as the container/source of truth):
//
//   <stem>.geometry               — the DATA BLOB: the whole scene graph
//                                  (hierarchy, raw TRS as authored, node
//                                  names, static/noLightmap flags, light/
//                                  camera defs) + the geometry blobs (the
//                                  packed SoA GPU heap streams — the same
//                                  layout the pak cooker stores) +
//                                  per-instance MATERIAL ASSET GUID
//                                  bindings. The ".mesh" name is the
//                                  HIGHER asset's now: a scene-placeable
//                                  mesh (geometry + material) the asset
//                                  browser drag-drops into viewports.
//   <stem>.mat<idx>_<name>.material — one per glTF material: the PBR
//                                  values + texture refs (image files
//                                  beside the glTF, as-authored URIs).
//   <stem>.anim<idx>_<name>.anim — one per glTF animation (the .tourAnim
//                                  format; tracks target the .geometry's
//                                  node ids — same deterministic import).
//   <stem>.gltf.meta             — the MANIFEST: the glTF's GUID + every
//                                  derived asset (kind, file, GUID,
//                                  source index). GUIDs are STABLE:
//                                  re-importing keeps them (matched by
//                                  kind+source index), so scenes
//                                  referencing the assets never break;
//                                  new sources get fresh GUIDs, removed
//                                  ones drop (scenes report them).
//
// The editor's scene (.tour) references these ASSETS by GUID (the root
// source points at the .geometry; the .geometry binds its .material
// GUIDs); the glTF never opens as a scene — a .gltf OPEN in the editor
// routes through the importer (derive-if-stale beside the source) and
// the session records the .geometry. The asset browser shows the glTF
// as a FOLDER of the derived assets.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Asset/meshlet.h>
#include <Engine/Asset/touranim.h>

namespace tinygltf { class Model; }

namespace smvk {
class ResourceManager; // GPU-load context (ImportMaterialAssetToCluster)
} // namespace smvk

namespace smvk::assets {

enum class AssetKind : uint8_t { Geometry = 0, Material = 1, Anim = 2 };

inline const char* AssetKindName(AssetKind k) {
	switch (k) {
	case AssetKind::Geometry: return "geometry";
	case AssetKind::Material: return "material";
	case AssetKind::Anim: return "anim";
	}
	return "?";
}

// One manifest entry (a derived asset of the glTF).
struct AssetEntry {
	AssetKind kind = AssetKind::Geometry;
	std::string file;       // file NAME next to the glTF (no dirs)
	std::string guid;       // 32-hex asset GUID (stable across reimports)
	std::string name;       // display name (the material/anim name)
	int sourceIndex = -1;   // glTF material/animation index (stability key)
};

// The glTF's manifest (the <stem>.gltf.meta).
struct AssetManifest {
	std::string guid;       // the glTF asset's GUID (the .meta identity)
	std::string source;     // the glTF file NAME (display + reimport)
	// The ORIGINAL SOURCE FILE's content identity (32-hex, derived from the
	// glTF bytes — see SourceAssetGuid). Registered in the .meta so the
	// artifact family always knows the exact source content it came from:
	// a from-zero re-import reproduces it anywhere, and staleness/matching
	// can key on CONTENT (the mtime rule alone misfires on copies that
	// preserve or reorders mtimes). Empty on legacy metas (pre-digest).
	std::string sourceDigest;
	std::string stem;       // the asset file stem ("<stem>.geometry" etc.)
	std::vector<AssetEntry> entries;
};

// ---- .material (the material asset) --------------------------------------
// The PBR values (no pool handles — a scene binds them) + texture refs
// (image files beside the glTF, as-authored glTF URIs; srgb flags by the
// glTF texture slot convention: baseColor/emissive are sRGB, the rest
// linear). Empty path = unbound slot.
struct MaterialAsset {
	std::string guid;
	std::string name;
	smvk::MaterialCreateInfo info{};
	struct TexRef {
		std::string path;
		bool srgb = false;
	};
	TexRef baseColor;
	TexRef metallicRoughness;
	TexRef normalTex;
	TexRef occlusion;
	TexRef emissiveTex;
};

// ---- .texture (the texture asset) -----------------------------------------
// A user-authored TEXTURE ASSET: an import-settings record + a source
// image reference (the file format mirrors .material: key=value text).
// The import settings drive the LDR -> KTX2 conversion (colorspace picks
// the transfer, format the payload: RGBA8 or a BCn target, mipLevels
// clamps the generated chain); the sampler rows (filter/wrap) ride the
// cluster's per-texture sampler registration into the cooked pak. A
// KTX2 SOURCE pre-decided all of that in its header: those fields read
// READ-ONLY in the inspector (ProbeTextureSource reports the header
// facts) and the import passes the payload through verbatim.
// Identity: the guid rides the file (inline, the .material convention)
// AND the <file>.meta sidecar (the assetdb's adoption path; the editor
// writes both at creation, see EditorApp::CreateTextureAsset). Material
// tex refs may point at a .texture file: the import routes through its
// settings (its colorspace wins over the ref's srgb flag).
struct TextureAsset {
	std::string guid;
	std::string name;
	std::string source;     // image file beside the .texture (png/jpg/
	                        // jpeg/tga/bmp/ktx2; POSIX-relative path)
	int colorspace = 0;     // 0 = sRGB (color), 1 = linear (data): the
	                        // FileNode convention; KTX2 sources: read-only
	int format = 0;         // KtxEncodeFormat value (0 = rgba8); KTX2
	                        // sources: read-only (the file's payload)
	int mipLevels = 0;      // 0 = auto (the full chain), N = clamp to N;
	                        // KTX2 sources: read-only (the file's chain)
	int wrap = 0;           // 0 = repeat, 1 = clamp-edge, 2 = mirrored
	                        // repeat (S+T; the FileNode convention)
	int filter = 0;         // 0 = linear, 1 = nearest (mag+min)
};
bool LoadTextureAsset(const std::filesystem::path& path,
	TextureAsset& out, std::string& err);
bool LoadTextureAssetText(const std::string& text, TextureAsset& out,
	std::string& err);
bool SaveTextureAsset(const std::filesystem::path& path,
	const TextureAsset& t, std::string& err);

// ---- .mesh (the scene-graph + geometry asset) ----------------------------
// Everything the world build needs: the hierarchy (RAW locals as the glTF
// authored them — the coordFix basis rides the scene root at load, same
// contract as the live import), the packed SoA geometry blobs (the GPU
// heap layout: VertexPos[] + VertexShading[] + u32[]), and the per-node
// bindings: geometry instances (each with its MATERIAL ASSET GUID) +
// light/camera definitions.
struct MeshAsset {
	struct Node {
		int32_t parent = -1;
		int32_t level = 0;
	};
	std::vector<Node> nodes;
	std::vector<std::string> names;        // per node ("" = unnamed)
	std::vector<glm::mat4> locals;         // raw TRS as authored
	std::vector<uint8_t> staticRoot;       // per node (0/1)
	std::vector<uint8_t> noLightmap;       // per node (0/1)
	struct Instance {
		uint32_t node = 0;
		uint32_t geometry = 0;
		std::string materialGuid;          // "" = unbound
	};
	std::vector<Instance> instances;
	struct LightDef {
		uint32_t node = 0;
		SceneLight def{};
	};
	std::vector<LightDef> lights;
	struct CameraDef {
		uint32_t node = 0;
		SceneCamera def{};
	};
	std::vector<CameraDef> cameras;
	struct Geometry {
		std::vector<smvk::VertexPos> positions;
		std::vector<smvk::VertexShading> shading;
		std::vector<uint32_t> indices;
		std::vector<smvk::Surface> surfaces;
		glm::vec3 aabbMin{ 0.0f };
		glm::vec3 aabbMax{ 0.0f };
		// Registry (the derivation's cluster registration): the mesh's
		// URI FRAGMENT ("mesh.N/prim.M") — the loader registers
		// "<mesh-path>#<fragment>" so cooks address it deterministically.
		std::string uriFragment;
		// Cooked meshlets (the derivation's meshlet pass; F3 + cooked
		// parity). Empty when none.
		smvk::CookedMeshlets meshlets;
		// Cooked lightmap binding (the COOK's lightmap bake writes these
		// back; import.exe leaves them empty). lmChunk = the atlas chunk
		// index (0xFFFFFFFF = uncharted); lmChartXforms parallels the
		// cook's chart indices (one float4 per chart: xy = scale,
		// zw = offset). The cluster restore stamps them onto the
		// StaticMesh so pak assembly needs no bake.
		uint32_t lmChunk = 0xFFFFFFFFu;
		std::vector<glm::vec4> lmChartXforms;
		// glTF skin stream (the Northlight data path), parallel to
		// positions/shading: the packed VertexSkin per vertex (mesh.h
		// binding 2). The BASE blobs carry it (the import post pass
		// filled the retained upload); LOD siblings stay static for now
		// (the runtime skinning slice owns per-LOD skin propagation).
		// Empty = a static mesh.
		std::vector<smvk::VertexSkin> skin;
	};
	std::vector<Geometry> geometry;
	// The cluster's skeletal skin (the FIRST glTF skin won; the import
	// post pass filled it). The joint table is in the ASSET node space
	// (the node list below is a 1:1 copy of the import cluster's scene,
	// so the joint ids ARE asset node indices); BuildFromMeshAsset
	// remaps them through its node mapping on restore. hasSkeleton
	// false = no skinned blobs.
	smvk::touranim::Skeleton skeleton;
	bool hasSkeleton = false;
	// LOD chains, PARALLEL to geometry (per base blob): the derived
	// sibling blobs ("<fragment>/lodK"). Loaded as cluster meshes (not
	// node-attached) + RegisterMeshLod entries — F5 + distance LODs
	// survive the preprocessed path.
	struct LodSibling {
		Geometry geo{};         // the sibling blob (own uri fragment)
		uint32_t lodIndex = 1; // 1-based (0 = the base)
		float simplifyError = 0.0f;
		float screenThreshold = -1.0f;
		uint32_t triCount = 0;
		float acmr = 0.0f;
	};
	std::vector<std::vector<LodSibling>> lodChains;
};

// Derivation CONFIG (mirrors the editor import gates; defaults = the
// editor's on-by-default cook acceleration). shaderLods is the EXCEPTION:
// the per-material far-shading derivation (ShaderLodPolicy — the importer
// stamping allowFarVertexLit on opaque materials) is DEFAULT OFF; opt in to
// have fresh derivations carry far vertex-lit policies again. Explicit
// tour/material authoring (farVertexLit/farSpecular/farNormalMap) is
// unaffected — this gate only controls what the importer DERIVES.
struct DeriveConfig {
	bool meshlets = true;   // TOURMALINE_EDITOR_MESHLETS parity
	bool lods = true;       // TOURMALINE_EDITOR_LODS parity
	bool shaderLods = false; // TOURMALINE_EDITOR_SHADERLODS parity (opt-in)
};

struct DeriveStats {
	uint32_t materials = 0;
	uint32_t anims = 0;
	uint32_t geometryBlobs = 0;
	uint32_t instances = 0;
	uint32_t lights = 0;
	uint32_t cameras = 0;
	// (keptGuids/dropped counted the old random-mint/keep-the-old-manifest
	// scheme; the deterministic derivation always derives — newGuids is the
	// live counter, the other two stay 0.)
	uint32_t keptGuids = 0;
	uint32_t newGuids = 0;
	uint32_t dropped = 0;
};

// Runs the (deterministic, GPU-free offline-mode) import of the glTF + writes
// every derived asset + the manifest next to it (its directory). Identity is
// DETERMINISTIC (see SourceAssetGuid/DerivedEntryGuid): a from-zero
// re-import anywhere reproduces the same GUIDs (and byte-identical derived
// files — they embed the GUIDs). Returns false + err on a failed import or
// a failed write (a partial derivation leaves older files — the next
// successful derive overwrites them).
bool DeriveGltfAssets(
	const tinygltf::Model& model,
	const std::filesystem::path& gltfPath,
	const std::string& gltfGuid,
	const DeriveConfig& config,
	AssetManifest& outManifest,
	DeriveStats* outStats = nullptr,
	std::string* err = nullptr);

// THE MESH IMPORTER entry: parse + derive in one call. The parse is
// host-side (glTF is an import-time SOURCE format, never runtime/VFS
// content). `gltfGuid` is the source's project identity — callers WITHOUT
// their own (assetdb-less, from-zero) pass "" and the derivation derives it
// from the SOURCE FILE CONTENT (SourceAssetGuid: the same uuid anywhere;
// the editor passes "" by design so project sessions agree with from-zero
// re-imports). On success the .geometry + .material/.anim set + the
// manifest sit next to the source, and outManifest carries the table. The
// natural driver is
// MeshArtifactStale below (derive only when the artifacts are missing or
// the source changed).
bool ImportMeshSource(
	const std::filesystem::path& gltfPath,
	const std::string& gltfGuid,
	const DeriveConfig& config,
	AssetManifest& outManifest,
	DeriveStats* outStats = nullptr,
	std::string* err = nullptr);

// ---- from-zero identity (register the original source, same uuid anywhere) --
// SourceAssetGuid: the glTF asset GUID derived DETERMINISTICALLY from the
// SOURCE FILE CONTENT (two-lane FNV-1a over the glTF bytes, 32-hex — the
// .meta GUID format). Identical bytes derive the identical GUID on any
// machine, in any directory, with no project DB and no .meta — a from-zero
// re-import reproduces the identity. DeriveGltfAssets falls back to this
// when the caller passes no guid (the editor passes none: project sessions
// and from-zero imports now agree), and records it as the manifest's
// sourceDigest either way. NOTE: this is the STABLE identity (the .gltf
// file alone) — staleness uses SourceContentDigest below (the full source
// set), so a texture/.bin edit re-derives without re-GUIDing the family.
[[nodiscard]] std::string SourceAssetGuid(
	const std::filesystem::path& gltfPath);
// SourceContentDigest: the STALENESS identity — the same two-lane hash
// over the .gltf bytes PLUS every external source it pulls in (.bin
// buffers + referenced images, sorted by authored URI for determinism;
// missing/unreadable files contribute a ":missing:<uri>" marker, remote
// http(s) URIs contribute the URI alone). Same bytes anywhere = same
// digest; any edited dependency flips it. Recorded as sourcedigest= and
// compared by MeshArtifactStale: a texture or .bin edit re-derives (the
// old single-file digest missed those — the source .gltf never changes
// when a texture is repainted). .glb (all-embedded) digests to the .glb
// bytes alone = byte-identical to SourceAssetGuid (no churn).
[[nodiscard]] std::string SourceContentDigest(
	const std::filesystem::path& gltfPath,
	const tinygltf::Model& model);
// Single-image import (import.exe + loose-image drops): writes
// <stem>.texture beside the source (editor CreateTextureAsset parity:
// sRGB/rgba8/full-chain/repeat/linear defaults; the guid is the source
// CONTENT digest, deterministic). Never overwrites an existing .texture
// (import settings live there — delete it to re-create); force=true
// recreates (settings reset). Returns false + err on IO failure.
bool ImportTextureSource(const std::filesystem::path& imagePath,
	bool force, std::string& outTexturePath, std::string* err = nullptr);
// DerivedEntryGuid: the deterministic GUID of one derived entry (a .material
// / .anim / the .geometry), keyed on (source guid, kind, source index) —
// the same two-lane hash over the seed. With the source guid content-
// derived, a from-zero re-import reproduces the WHOLE manifest (and with
// it the .geometry/.material bytes: entries embed these GUIDs), so the
// artifact family is byte-stable across re-imports.
[[nodiscard]] std::string DerivedEntryGuid(
	const std::string& sourceGuid, AssetKind kind, int sourceIndex);

// The re-import gate: TRUE when the derived .geometry beside `gltfPath`
// is missing/empty, the manifest is missing/unparseable, or the SOURCE
// SET changed (SourceContentDigest: the .gltf + .bin + referenced images
// — a repainted texture re-derives). FALSE = the artifact set is current;
// open the .geometry directly without re-deriving. Digest-less (legacy)
// metas fall back to the mtime rule.
bool MeshArtifactStale(const std::filesystem::path& gltfPath);

// Readers (round-trip; the world build + the read-only inspector use them).
bool LoadMeshAsset(
	const std::filesystem::path& path, MeshAsset& out, std::string& err);
bool LoadMaterialAsset(
	const std::filesystem::path& path, MaterialAsset& out, std::string& err);
// Byte/text forms (VFS reads cook through the overlay; the host forms
// wrap these).
bool LoadMeshAssetBytes(const uint8_t* data, size_t size, MeshAsset& out,
	std::string& err);
bool LoadMaterialAssetText(const std::string& text, MaterialAsset& out,
	std::string& err);
// Writer (round-trip; the COOK writes cooked artifacts back: stamped
// chart UVs ride the shading blobs verbatim, meshlets/LODs persist,
// and the v2 trailing registry carries the per-blob lightmap binding).
// Returns false with err set on IO failure.
bool SaveMeshAsset(const std::filesystem::path& path, const MeshAsset& m,
	std::string& err);
// Reads <gltfPath>.meta (the manifest of an already-imported glTF).
bool LoadAssetManifest(
	const std::filesystem::path& gltfPath, AssetManifest& out,
	std::string& err);
// Text form (VFS reads cook through the overlay; the host form wraps
// this). Tolerant: unknown keys skip, a missing guid fails.
bool LoadAssetManifestText(const std::string& text, AssetManifest& out,
	std::string& err);
// Writes the manifest (DeriveGltfAssets' tail; exposed for tests).
bool SaveAssetManifest(
	const std::filesystem::path& gltfPath, const AssetManifest& manifest,
	std::string& err);

// ---- shared material-asset import (editor + cook) -------------------------
// The SINGLE import path for asset-browser materials (a .material + its
// texture files beside it — see EditorApp::AssignMaterialAsset, which this
// mirrors): parses the asset, loads its texture sources (KTX2 passthrough,
// other sources transcoded to KTX2 honoring the TexRef srgb flag), creates
// the cluster material + registers "asset://<guid>" and the texture URIs
// (URI = the POSIX file path). The ORIGINAL texture source bytes ride the
// cluster pool entries (cooks re-embed from source).
//
// ctx != nullptr (editor): the textures also GPU-load through it so the
// live view shows them this frame (the caller refreshes its pool->RHI map
// afterwards — editor-side state). ctx == nullptr (cook): pool + URI +
// source registration only; no GPU work.
// Returns the cluster material handle (empty + err on failure).
smvk::ClusterMaterialHandle ImportMaterialAssetToCluster(
	smvk::Cluster& cluster, const std::filesystem::path& assetPath,
	smvk::ResourceManager* ctx, std::string& err);
// The editor's deterministic default-material seed ("editor://default"):
// reuses the registered handle when one exists, else creates it (metallic
// 0, roughness 0.9). The editor wraps this with its world materialLocations
// bookkeeping; cooks call it directly when a tour carries the seed record.
smvk::ClusterMaterialHandle EnsureClusterDefaultMaterial(smvk::Cluster& cluster);
// The BUILT-IN solid-pink error texture asset ("editor://error_texture"):
// reuses the registered handle when one exists, else mints it from the
// standalone pink KTX2 (64x64 sRGB RGBA8 — see ktx_error_texture.h).
// Texture slots that FAIL to load (missing/empty/undecodable source,
// failed conversion) bind this instead of empty, so a broken texture
// reads as an unmistakable pink surface — reported, never invisible and
// never fatal. Cook mode mints the pool id + stash only (the cook
// embeds the pink payload; the runtime streams it like any texture).
smvk::TextureHandle EnsureClusterErrorTexture(smvk::Cluster& cluster,
	smvk::ResourceManager* ctx);

// ---- shared texture-asset import (editor + cook) --------------------------
// The SINGLE import path for .texture assets (a .texture + its source image
// beside it, see TextureAsset): parses the asset, reads the source (KTX2
// passthrough — the header pre-decided colorspace/format/mips; LDR sources
// converted honoring the asset's import settings), registers the cluster
// texture + its sampler (filter/wrap) and stashes the GPU-READY payload
// bytes (the conversion is baked at import; the cook passthrough-embeds them
// — RGBA8/BCn payloads never need a cook-time transcode). URI = the POSIX
// .texture file path (re-imports dedupe through it). Materials reference
// .texture files through their tex refs — ImportMaterialAssetToCluster
// routes those here.
//
// ctx != nullptr (editor): the texture also GPU-loads through it (the caller
// refreshes its pool->RHI map afterwards). ctx == nullptr (cook): pool +
// URI + source registration only; no GPU work.
// Returns the cluster texture handle (empty + err on failure).
smvk::TextureHandle ImportTextureAssetToCluster(
	smvk::Cluster& cluster, const std::filesystem::path& assetPath,
	smvk::ResourceManager* ctx, std::string& err);

} // namespace smvk::assets
