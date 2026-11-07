// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <Engine/App/cluster.h>
#include <Engine/Asset/pak.h>
#include <Engine/Scene/node_schema.h>
#include <Engine/Scene/scene.h>

namespace tour {
struct Cluster;
struct GiBinding;
class TextureStreamer;
} // namespace tour
namespace smvk {
using ::tour::Cluster;

using ::tour::GiBinding;
using ::tour::TextureStreamer; // (moved: engine-owned streamer, tour::)
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
    // Authored audio sources persisted in the Scene blob's v18 section
    // (inline emitter payloads — no separate blobs; the referenced
    // sounds ship as PakAssetKind::Sound blobs keyed by VFS path).
    uint32_t audioSources = 0;
    // Authored audio rooms persisted in the Scene blob's v20 section
    // (the environmental-reverb volume rows — inline payloads, the
    // shape riding the node transform itself; no .givolume, no bake
    // artifact).
    uint32_t audioRooms = 0;
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
// excludeTextureVfsPaths/excludeTextureVfsCount (optional, the recipe
// build's multi-pak ownership surface): VFS texture paths this pak must
// NOT embed because a DEPENDENCY pak owns them — the materials keep
// their texture refs and resolve them cross-pak through the mounted pak
// backends at runtime (pak.h's interop contract: PakBackend serves
// Texture-kind blobs by VFS path). Null/0 = embed everything (the
// default, byte-identical to the historical output).
// Returns false on error (err filled).
bool SerializeClusters(const std::vector<const Cluster*>& clusters,
                       const std::vector<std::string>& sceneNames, // parallel, may be short
                       PakWriter& writer,
                       ClusterSerializeStats* stats = nullptr,
                       std::string* err = nullptr,
                       const std::string* excludeTextureVfsPaths = nullptr,
                       size_t excludeTextureVfsCount = 0,
                       // The sound-id map (the cooker's --sound-id list): authored sound
                       // assetPath -> stable 0x4xxx content id, stamped into the scene
                       // blob's audio section. Null/0 = a bare, non-project cook (sounds
                       // serialize as 0 = unresolved, the cooker warns); a non-empty map
                       // with an unresolved referenced path FAILS the serialize (the
                       // project contract: never silently omit a referenced sound).
                       const std::string* soundIdPaths = nullptr,
                       const uint64_t* soundIds = nullptr,
                       size_t soundIdCount = 0,
                       // The project's node schemas (Engine/Scene/node_schema.h): emitted
                       // as the pak-level "#schemas" registry blob (the cooked LAYOUTS
                       // the runtime + the generated project::types decode against).
                       // Null/0 = a schema-less pak (association-free content).
                       const NodeSchema* schemas = nullptr,
                       size_t schemaCount = 0);

// Single-scene form (identical layout: index-0 scene blob). The
// schema params forward to SerializeClusters (the pak-level
// "#schemas" registry).
bool SerializeCluster(const Cluster& cluster,
                      PakWriter& writer,
                      ClusterSerializeStats* stats = nullptr,
                      std::string* err = nullptr,
                      const NodeSchema* schemas = nullptr,
                      size_t schemaCount = 0);

// Parses a PakAssetKind::Gi registry blob (u32 count | u32 kind |
// str name) into bindings. Used by DeserializeCluster and by tools
// that only inspect (see PakCooker --dump-pak).
bool ParseGiRegistryBlob(std::span<const uint8_t> payload,
                         std::vector<GiBinding>& out,
                         std::string& err);

// Lists the scenes of a parsed pak in index order (names may be empty;
// old single-scene paks report one unnamed entry). No cluster is built.
std::vector<PakSceneInfo> ListPakScenes(const PakReader& reader, std::string* err = nullptr);

// Cross-pak texture resolution hook (the multi-pak recipe build): a
// material's texture URI with NO blob in this pak names a texture a
// DEPENDENCY pak owns (the serializer's exclude kept it out on
// purpose — the plan's ownership table). The resolver decodes +
// uploads through the caller's own channel (typically the mounted pak
// backends through the content VFS), registers the texture on the
// cluster and returns the handle; an empty handle leaves the slot
// empty (the historical behavior). This is what makes pak.h's interop
// contract — materials in one pak referencing textures stored in
// another via plain VFS paths — true at load time.
using ExternalTextureResolver = std::function<TextureHandle(Cluster&, const std::string& uri)>;

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
    std::shared_ptr<const std::vector<uint8_t>> imagePin = {},
    const ExternalTextureResolver* externalTexture = nullptr,
    size_t sceneNodeBudget = 0);

// ---- the GPU-less scene-blob reader (the structure-only lane) ----
// The pak-id remap tables for one scene-blob parse (null = a
// structure-only walk: links are skipped but the cursor still
// walks + validates — the ListPakScenes + test lane).
struct SceneLinkMaps {
    const std::vector<ClusterStaticMeshHandle>* meshes = nullptr;
    const std::vector<ClusterMaterialHandle>* materials = nullptr;
    const std::vector<ClusterLightHandle>* lights = nullptr;
    const std::vector<ClusterCameraHandle>* cameras = nullptr;
    const std::vector<ClusterDecalHandle>* decals = nullptr;
};
// The shared Scene-blob reader (the writer's v1..v19 sections):
// hierarchy + transforms + node links + names + the trailing
// sections. With maps, links resolve to fresh pool handles;
// without (null), only structure + names come out. skeletonOut/
// hasSkeletonOut (v14): null = skip (structure-only callers); an
// absent section leaves *hasSkeletonOut false.
// wantNodeNames (default true): NODE names are AUTHORING-ONLY data -
// no renderer/audio/app runtime consumer exists (the cooked skeleton
// binds joints by node id + inverse binds, no names) - so a runtime
// world-load lane passes false and the blob's node-name pair table +
// string table are cursor-skipped with ZERO allocations (a flat
// stress grid carries ~1M of each; materializing them was 1M heap
// strings nobody read). Material names always load (diagnostics).
bool ParseSceneBlob(std::span<const uint8_t> payload,
                    Scene& scene,
                    std::string& nameOut,
                    const SceneLinkMaps* maps,
                    std::string& err,
                    smvk::touranim::Skeleton* skeletonOut = nullptr,
                    bool* hasSkeletonOut = nullptr,
                    bool wantNodeNames = true);

} // namespace smvk

// The process-wide default for ParseSceneBlob's wantNodeNames, read by
// DeserializeCluster's world-load lane. ON by default (editors, tools
// and tests author scenes - names are their data); RunPakApp flips it
// OFF before any load because the runtime render lanes consume zero
// node names. A host that wants them back (a game addressing nodes by
// name) calls the setter before loading. Set once at startup, before
// the first load - no locking by contract (same shape as
// smvk::StrictResources).
namespace tour::asset {
void SetSceneNodeNamesWanted(bool wanted);
bool SceneNodeNamesWanted();
} // namespace tour::asset
