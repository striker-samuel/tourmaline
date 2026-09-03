#pragma once

#include <memory>
#include <string>
#include <vector>

#include <Engine/Asset/pak.h>

namespace smvk {

struct Cluster;
class ResourceManager;

struct ClusterSerializeStats {
	uint32_t meshes = 0;
	uint32_t materials = 0;
	uint32_t textures = 0;
	uint32_t textureBytes = 0;
	uint32_t nodes = 0;
};

// Serializes one Cluster (scene + meshes + materials + embedded textures)
// into writer. Mesh/material/texture blobs are keyed by their registered
// asset URIs (see Cluster::GetMeshUri); assets without a registered URI get
// a "<cluster-local>#..." fallback so the scene remap stays lossless.
// Texture blobs embed the .ktx2 source bytes from Cluster::GetTextureSource
// — the importer stashes them; a missing payload fails that texture with
// an error (cooker must fail loudly, never silently drop).
// Returns false on error (err filled).
bool SerializeCluster(
	const Cluster& cluster,
	PakWriter& writer,
	ClusterSerializeStats* stats = nullptr,
	std::string* err = nullptr);

// Rebuilds a Cluster from a parsed pak: textures (parallel from embedded
// .ktx2 bytes) -> materials (texture URIs re-resolved) -> meshes (GPU
// upload via Cluster::UploadStaticMesh, streamer-batched, caller issues
// ctx.FlushUploads() afterwards) -> scene graph (pak-local ids remapped to
// fresh handles, global transforms recomputed). Asset URI maps are
// re-registered so AssetRef resolution works on the loaded cluster.
// Returns nullptr on error (err filled).
std::unique_ptr<Cluster> DeserializeCluster(
	ResourceManager& ctx,
	const PakReader& reader,
	std::string* err = nullptr);

} // namespace smvk
