// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Engine/Asset/pak.h>

namespace smvk {

struct Cluster;
struct GiBinding;
class TextureStreamer;
class ResourceManager;

struct ClusterSerializeStats {
	uint32_t meshes = 0;
	uint32_t materials = 0;
	uint32_t textures = 0;
	uint32_t textureBytes = 0;
	uint32_t nodes = 0;
	// v4 tail-upload digests stamped (sidecar or ktx-direct flavor): these
	// textures load objects + tail without a KTX parse.
	uint32_t digestedTextures = 0;
	// Named GI bake files registered on the cooked clusters (see
	// Cluster::giBindings), written as one PakAssetKind::Gi blob.
	uint32_t giBindings = 0;
	// Meshlet cook output (indirect-path foundation): meshes carrying a
	// meshlet section, and the total meshlet count across them.
	uint32_t meshesWithMeshlets = 0;
	uint32_t meshletCount = 0;
	// Static-subtree roots persisted in the Scene blob.
	uint32_t staticRoots = 0;
	// Cooked punctual lights (KHR_lights_punctual) persisted as
	// PakAssetKind::Light blobs (one per light, URI-keyed); the Scene
	// blob carries the node links as (node, pakId) pairs.
	uint32_t lights = 0;
	// Authored cameras persisted as PakAssetKind::Camera blobs (one per
	// camera, URI-keyed); the Scene blob carries the node links as
	// (node, pakId) pairs.
	uint32_t cameras = 0;
};

// One scene entry of a pak (TOC order = scene index; index 0 is the
// default scene and the only one old readers accept).
struct PakSceneInfo {
	std::string name;    // glTF scene name (may be empty)
	std::string tocPath; // "<cluster-local>#scene" or "#scene.<index>"
	uint32_t nodes = 0;
};

// Serializes clusters (scene + meshes + materials + embedded textures)
// into writer. Mesh/material/texture/light blobs are keyed by their
// registered asset URIs (see Cluster::GetMeshUri); assets without a
// registered URI get a "<cluster-local>#..." fallback so the scene remap
// stays lossless. Asset blobs are shared by URI: cooking several scenes
// of one model writes each unique blob once, and every scene blob
// references the shared ids. Scene blobs land at "<cluster-local>#scene"
// (index 0) and "<cluster-local>#scene.<i>" with a trailing name section
// (v5; always written, possibly empty).
// Texture blobs embed the .ktx2 source bytes from Cluster::GetTextureSource
// — the importer stashes them; a missing payload fails that texture with
// an error (cooker must fail loudly, never silently drop).
// Returns false on error (err filled).
bool SerializeClusters(
	const std::vector<const Cluster*>& clusters,
	const std::vector<std::string>& sceneNames, // parallel, may be short
	PakWriter& writer,
	ClusterSerializeStats* stats = nullptr,
	std::string* err = nullptr);

// Single-scene form (identical layout: index-0 scene blob).
bool SerializeCluster(
	const Cluster& cluster,
	PakWriter& writer,
	ClusterSerializeStats* stats = nullptr,
	std::string* err = nullptr);

// Parses a PakAssetKind::Gi registry blob (u32 count | u32 kind |
// str name) into bindings. Used by DeserializeCluster and by tools
// that only inspect (see PakCooker --dump-pak).
bool ParseGiRegistryBlob(
	std::span<const uint8_t> payload,
	std::vector<GiBinding>& out,
	std::string& err);

// Lists the scenes of a parsed pak in index order (names may be empty;
// old single-scene paks report one unnamed entry). No cluster is built.
std::vector<PakSceneInfo> ListPakScenes(
	const PakReader& reader,
	std::string* err = nullptr);

// Rebuilds a Cluster from a parsed pak: textures (parallel from embedded
// .ktx2 bytes) -> materials (texture URIs re-resolved) -> meshes (GPU
// upload via Cluster::UploadStaticMesh, streamer-batched, caller issues
// ctx.FlushUploads() afterwards) -> scene graph (pak-local ids remapped to
// fresh handles, global transforms recomputed). Asset URI maps are
// re-registered so AssetRef resolution works on the loaded cluster.
// sceneIndex selects which Scene blob to load (0 = default; see
// ListPakScenes). Returns nullptr on error (err filled).
// v4 fast-load textures: when `streamer` is non-null AND a texture blob
// carries a digest, its objects + coarse-mip tail upload through
// CreateStreamingTextureFromMeta (no KTX parse) and the handle registers
// with the streamer for demand upgrades — `imagePin` (the loaded pak
// image) pins the borrowed payloads on the cluster. Digest-less blobs and
// the streamer==nullptr case keep today's full-resident ktx path. When
// streamer != nullptr the per-texture SetTextureSource copies are skipped
	// (payloads are borrowed, not owned).
	std::unique_ptr<Cluster> DeserializeCluster(
		ResourceManager& ctx,
		const PakReader& reader,
		std::string* err = nullptr,
		uint32_t sceneIndex = 0,
		TextureStreamer* streamer = nullptr,
		std::shared_ptr<const std::vector<uint8_t>> imagePin = {});

} // namespace smvk
