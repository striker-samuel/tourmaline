// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <Engine/Memory/linear_vector.h>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <Engine/Asset/asset_registry.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Asset/touranim.h>
#include <Engine/Memory/arena.h>
#include <Engine/Memory/handle.h>
#include <Engine/Memory/pool.h>
#include <Engine/Memory/string_view.h>
#include <Engine/Memory/uri_map.h>
#include <Engine/Renderer/geometry_upload.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Scene/scene.h>
#include <Engine/Utils/bvh.h>
#include <Engine/smvk.h>

namespace tour {
// (namespace/RHI boundary audit) this block's RHI dependencies are
// FULLY QUALIFIED (smvk::) - no broad using-directive (R6-gated).

struct World;

template <typename HandleT> struct AssetRef;

// Backend-defined record of a static mesh's renderer allocations
// (opaque here; complete type lives in the Vulkan detail header).

using ClusterStaticMeshHandle = smvk::Handle<StaticMesh>;
using ClusterMaterialHandle = smvk::Handle<Material>;
using ClusterTextureHandle = smvk::Handle<struct Texture>;
using ClusterLightHandle = smvk::Handle<SceneLight>;
using ClusterCameraHandle = smvk::Handle<SceneCamera>;
using ClusterDecalHandle = smvk::Handle<SceneDecal>;

// Cooked GI binding (named bake payload): the GI payloads themselves
// (probe volumes, lightmap atlases, manifests, cubemap captures) live
// outside the pak; this names the bake file (gi/<name>.sggi / .sglm /
// .regmanifest / .givolumemanifest, textures/cubemaps/<name>.cubemanifest)
// a cluster's scenes were cooked against. The name is the stable
// bake stem — guid-era paks recorded guid names (see
// Engine/Asset/bake_asset.h for the era history), which resolve the
// same way. See Cluster::giBindings.
//
// NEW (scene-carried giAsset bindings): Kind::GiVolume records carry
// the FULL VFS path of ONE baked GI volume (gi/<scene>_<name>.givolume)
// — the scene's hard per-volume identification (tour giAsset kind
// givolume, stamped by the bake owner), replacing the manifest round-
// trip for GI volumes. Multiple GiVolume bindings are legal (one per
// authored volume, cap SG_GI_VOLUME_MAX); every other kind keeps the
// exactly-one semantics (ResolveGiBakes takes the first).
struct GiBinding {
    enum class Kind : uint32_t {
        Probe = 0,            // gi/<name>.sggi world-field octree
        Lightmap = 1,         // gi/<name>.sglm lightmap atlas
        RegManifest = 2,      // gi/<name>.regmanifest (regular volumes)
        GiVolumeManifest = 3, // gi/<name>.givolumemanifest (legacy manifest channel)
        Cubemap = 4,          // textures/cubemaps/<name>.cubemanifest
        GiVolume = 5,         // gi/<path>.givolume — ONE GI volume, FULL VFS
                              // path (the per-volume scene binding)
    };
    Kind kind = Kind::Probe;
    std::string name;
};

struct Cluster {
    // ctx may be nullptr for GPU-less offline clusters (see SetOfflineMode);
    // any GPU upload with a null context is a programming error.
    explicit Cluster(smvk::ResourceManager* ctx);
    ~Cluster();
    Cluster(const Cluster&) = delete;
    Cluster& operator=(const Cluster&) = delete;

    smvk::ResourceManager* ctx_ = nullptr;
    Scene scene;
    World* world = nullptr;
    Cluster* sibling = nullptr;
    // linked list for import chain if multiple clusters belong to one input file.

    // COOKED STATIC-CULL BVH (the scene blob's static-cull section,
    // Engine/Utils/bvh.h): the frustum-cull tree over the scene's static
    // mesh rows, baked at cook time over LOD-family-union world AABBs.
    // The World ADOPTS it at resolve time instead of rebuilding
    // (World::AdoptCookedStaticBvh) when the adoption-guard hash still
    // matches — the tree is then CONSUMED (moved out), so any later edit
    // resolves through the ordinary rebuild. Empty on runtime-built
    // clusters, editor clusters and pre-section paks (the rebuild path
    // stays the one and only fallback).
    tour::BvhTree cookedStaticBvh;
    // The cook-side StaticCullStateHash fold the adoption compares against,
    // + the dense node count that fold covered (the runtime folds the SAME
    // PREFIX — the world may append runtime-owned nodes post-parse).
    uint64_t cookedStaticBvhStateHash = 0;
    uint32_t cookedStaticBvhNodeCount = 0;
    // SECTION PRESENCE (CookedStaticBvhOut::present): distinguishes a NEW
    // pak whose scene blob carried the static-cull BVH section (possibly a
    // zero-prim tree) from a PRE-SECTION pak that carried none. The load
    // path fails on a pre-section pak unless World::legacyStaticFallback
    // opts into the on-load rebuild.
    bool cookedStaticBvhPresent = false;
    [[nodiscard]] bool hasBvh() const {
        return cookedStaticBvhPresent;
    }

    smvk::Pool<StaticMesh, StaticMesh> staticMeshes;
    // smvk::Pool<SkinnedMesh, SkinnedMesh> skinnedMesh;
    smvk::Pool<Material, Material> materials;
    smvk::Pool<Texture, smvk::Holder<smvk::TextureHandle>> textures;
    smvk::Pool<SceneLight, SceneLight> lights;
    smvk::Pool<SceneCamera, SceneCamera> cameras;
    smvk::Pool<SceneDecal, SceneDecal> decals;

    ClusterStaticMeshHandle UploadMesh(const MeshData& data);
    // Batched UploadMesh: identical semantics per entry (heap
    // range allocation, pool entry, uri/lod registration happen at the
    // CALLER), but the AoS->SoA conversion packs ONE blob per heap and
    // the GPU upload is three BulkCopyBuffer calls (table-driven; the
    // RHI stages 512 MB blobs — one memcpy pass + one submit + one
    // barrier set per blob instead of three streamer calls per mesh).
    // Handles return in input order. All-or-nothing: {} when ANY entry
    // fails (degenerate mesh, heap full) — nothing is staged to the GPU
    // in that case. Cook mode / retainUploads mirror the single path.
    std::vector<ClusterStaticMeshHandle> UploadMeshes(std::span<const MeshData> uploads);
    // Direct-addressed batched upload (v4 fast-load path): the sources
    // are packed-SoA stream subranges of ONE blob (`content` — the
    // loaded pak image) addressed by byte offsets, so the upload is
    // pure offset math: per entry heap range allocation + pool entry,
    // then THREE BulkCopyBuffer calls take the {srcOffset, dstOffset,
    // size} tables straight from `content`. No AoS parse, no
    // per-vertex CPU conversion, no intermediate packed vectors. The
    // caller attaches cooked meta (sphere, lod, lightmap chunk,
    // meshlets, uri) per handle afterwards. All-or-nothing like
    // UploadMeshes; cook mode is a programming error (the
    // deserializer only calls this with a GPU context).
    //
    // The carve + copy core lives in the SHARED GEOMETRY UPLOAD
    // SERVICE (Renderer/geometry_upload.h — extracted so non-cluster
    // owners upload through the same seam); this method is the
    // CLUSTER's side of that service: the caller-owned arena tokens
    // land in this cluster's meshAllocations_ map, the mesh values
    // in this cluster's pool. The source shape is the service's
    // smvk::StaticGeometrySource (the old nested StaticMeshDirectSource
    // name is GONE — call sites migrated, no compat alias).
    std::vector<ClusterStaticMeshHandle> UploadStaticMeshesDirect(
        const void* content,
        std::span<const smvk::StaticGeometrySource> sources);
    ClusterMaterialHandle CreateMaterial(smvk::TextureHandle diffuse);
    ClusterMaterialHandle CreateMaterial(const MaterialCreateInfo& info);
    // Pure value assembly (no texture resolution): shared core of
    // CreateMaterial and the GPU-free offline path. textureIds use the
    // same integer domain as MaterialValues (RHI indices at runtime,
    // cook-table ids when cooking).
    ClusterMaterialHandle CreateMaterialFromValues(const MaterialValues& values,
                                                   ShadingModel shadingModel,
                                                   AlphaMode alphaMode,
                                                   float alphaCutoff,
                                                   bool doubleSided,
                                                   bool castsShadows = true,
                                                   bool allowFarVertexLit = true,
                                                   ShaderLodQuality farQuality = {});

    // Cooked punctual light (KHR_lights_punctual): pure value asset,
    // no GPU side. World position/direction resolve from the light
    // node's global transform at use time (see Scene::lightForNode).
    ClusterLightHandle CreateLight(SceneLight def);
    // Authored camera lens (see Scene::cameraForNode): pure value
    // asset, no GPU side. The pose resolves from the camera node's
    // global transform at use time.
    ClusterCameraHandle CreateCamera(SceneCamera def);
    // Authored/runtime decal look (see Scene::decalForNode + SceneDecal):
    // pure value asset, no GPU side (the textures resolve through the
    // cluster pool at use). The shape resolves from the decal node's
    // global transform at use time.
    ClusterDecalHandle CreateDecal(SceneDecal def);

    // Cooked GI bindings (named bake payloads): probe volumes
    // (.sggi) and lightmap atlases (.sglm) live OUTSIDE the pak under
    // content/gi/, and the cluster names the ones its scenes were
    // cooked against. The model is a list (future multi-GI); loaders
    // enforce one binding per kind for now (first wins + warn).
    std::vector<GiBinding> giBindings;
    void RegisterGiBinding(GiBinding::Kind kind, const std::string& name);

    // ---- Bake payloads (entity-home for GI/cubemap content) ----------
    // The baked bytes a cluster's scenes were cooked against (sggi
    // octree, sgrv/givolume volumes, reg/cv/cubemap manifests, cubemap
    // KTX2s, sglm atlas), keyed by their VFS path — the payload half
    // of the giBindings registry. Loaded pak clusters carry them
    // zero-copy (spans into the PINNED pak image; owned copies only
    // for LZ4 entries); runtime hosts (the editor's quick-bake)
    // INJECT owned bytes through AddBakePayload — typically on the
    // dynamic cluster, whose bindings/payloads override cooked ones
    // (see World::ReadBakeBytes). Cluster-owned content: unloading
    // the cluster drops it, exactly like meshes/lights/decals.
    struct BakePayload {
        // Borrowed span into the pinned pak image (uncompressed pak
        // entries) — valid for the cluster's lifetime (PinPakImage).
        std::span<const uint8_t> borrowed{};
        // Owned bytes (LZ4 pak entries, editor injections).
        std::vector<uint8_t> owned{};
        [[nodiscard]] const uint8_t* data() const {
            return owned.empty() ? borrowed.data() : owned.data();
        }
        [[nodiscard]] size_t size() const {
            return owned.empty() ? borrowed.size() : owned.size();
        }
        [[nodiscard]] bool empty() const {
            return owned.empty() && borrowed.empty();
        }
    };
    // Runtime injection (editor quick-bake / app-side content).
    void AddBakePayload(const std::string& vfsPath, std::vector<uint8_t> bytes);
    // Pak-load path only (DeserializeCluster): borrowed span into the
    // PINNED pak image — the caller must PinPakImage first/after.
    void AddBakePayloadBorrowed(const std::string& vfsPath, std::span<const uint8_t> bytes);
    // Lookup by VFS path ("gi/<stem>.sggi", ...). Null = not carried.
    [[nodiscard]] const BakePayload* FindBakePayload(const std::string& vfsPath) const;
    // Editor hot-bake: the newly minted GI path replaces the old
    // payload; do not retain the previous bake's CPU bytes forever.
    void RemoveBakePayload(const std::string& vfsPath) {
        bakePayloads_.erase(vfsPath);
    }

    // Cook support: when enabled, UploadMesh keeps a CPU-side copy
    // of every upload keyed by pool index so the pak cooker can embed
    // full vertex/index payloads (runtime Clusters never see this cost:
    // the default is off, and GPU pools hold no CPU vertex data after
    // upload). DestroyMesh drops the retained copy.
    void SetRetainUploads(bool retain);
    const MeshData* GetRetainedUpload(ClusterStaticMeshHandle handle) const;

    // ------------------------------------------------------------------
    // Skeletal skin (the Northlight data path, touranim.h + mesh.h).
    //
    // The cluster carries ONE Skeleton (the first imported glTF skin
    // wins - the knight has exactly one; multi-skin clusters would
    // need a table keyed by mesh, deliberately deferred). The
    // per-mesh PackedSkinInfluences streams (mesh.h, binding 2) index the
    // Skeleton's joint table; the palette build
    // (asset::BuildSkinPalette) evaluates it per frame.
    //
    // The importer records (mesh, glTF skin) pairs during the node
    // walk, then patches the retained uploads' skin streams in a POST
    // pass (the Skeleton needs the gltf node map fully populated -
    // joint nodes may import after the meshes that reference them);
    // see GltfToSceneImporter::BuildSkinData. The serializer persists
    // both (the mesh blob's trailing skin-stream section + the scene
    // blob's trailing skeleton section).
    // ------------------------------------------------------------------
    asset::Skeleton skeleton;
    bool hasSkeleton = false;
    // Patches a retained upload's PackedSkinInfluences stream (the import post
    // pass; also the deserializer on load). No-op when uploads are
    // not retained (the caller logs); the stream rides the upload
    // through serialize/deserialize like any other stream.
    void PatchRetainedSkinStream(ClusterStaticMeshHandle handle,
                                 std::vector<PackedSkinInfluences>&& stream);
    // Marks a mesh pool entry skinned (the deserializer + the import
    // post pass; the runtime draw path gates on the retained stream,
    // this is the pool-level marker for tooling).
    void SetMeshSkinned(ClusterStaticMeshHandle handle, bool skinned);
    // The runtime-side per-mesh skin streams (binding-2 data), keyed
    // by mesh pool index. Filled by the import post pass (retained OR
    // runtime clusters - PatchRetainedSkinStream always stores here)
    // and the deserializer (the mesh blob's trailing skin section).
    // The GPU binding-2 allocation comes with the skinning pipeline
    // (slice 2); until then this is the draw path's source. Null =
    // static mesh.
    [[nodiscard]] const std::vector<PackedSkinInfluences>* GetMeshSkinStream(
        ClusterStaticMeshHandle handle) const;
    // The RETAINED upload's skin stream (or null). The import post pass
    // reads it back: the stream attached before OptimizeMeshData
    // rode the vertex remap, so it is already in final vertex order.
    // Null when uploads are not retained or the mesh carries no skin.
    [[nodiscard]] const std::vector<PackedSkinInfluences>* GetRetainedSkinStream(
        ClusterStaticMeshHandle handle) const;
    void SetMeshSkinStream(ClusterStaticMeshHandle handle,
                           std::vector<PackedSkinInfluences>&& stream);

    // ------------------------------------------------------------------
    // SKINNED-MESH GPU data (the Northlight compute-skinning path).
    //
    // The heaps stay the BIND POSE (the skinning pass's input - the
    // v4 fast-load streams untouched). The pass reads: the palette
    // (per frame, the World/animation driver), the SKINNED STREAM
    // buffer (PackedSkinInfluences per vertex - the joint ids + weights), + the
    // bind-pose pos/normal/tangent words from the heaps; and writes
    // the DEFORMED buffer ([pos3, normalW, tangentW] per vertex, 20B
    // - the Northlight "GPU skinning produce animated positions and
    // normals" box). The draw paths fetch the deformed buffer for
    // skinned draws (a per-draw flag + the cluster's heap-base
    // offset - one fetch swap, no per-vertex skin math in the draw
    // shaders).
    //
    // The deformed buffer is a PARITY DOUBLE BUFFER (two halves of
    // span bytes): the driver's per-frame parity flip makes the
    // skinning pass write one half while every render fetch reads
    // the cur half through the SAME BDA+offset arithmetic (the
    // driver points skinDeformedBda at the cur half - no consumer
    // changes), and the MOTION vector leg reads cur + PREV halves
    // (this frame's + last frame's animated pose - skinned motion
    // vectors, the SMAA T2x resolve / DLSS / motion blur history
    // inputs). The prev half is only valid while consecutive frames
    // ran the skinning on the same heap (the driver's validity
    // tracking).
    // ------------------------------------------------------------------
    struct SkinnedMeshRange {
        ClusterStaticMeshHandle mesh;
        uint32_t vertexCount = 0;
        uint32_t heapVertexBase = 0; // the bind-pose heap range base (vertex units)
        uint32_t skinBase = 0;       // offset into the skinned stream buffer (vertex units)
    };
    // Builds + uploads the skinned-stream buffer (concatenated per
    // mesh, static data - once) + allocates the deformed buffer
    // covering the skinned meshes' heap span. Idempotent; false when
    // the cluster carries no skin streams or the context is null.
    bool EnsureSkinnedGpuData(smvk::ResourceManager& ctx);
    [[nodiscard]] const std::vector<SkinnedMeshRange>& GetSkinnedRanges() const {
        return skinnedRanges_;
    }
    [[nodiscard]] smvk::BufferHandle GetSkinnedStreamBuffer() const {
        return skinnedStream_;
    }
    [[nodiscard]] smvk::BufferHandle GetSkinnedDeformedBuffer() const {
        return skinnedDeformed_;
    }
    // The DEFORMED buffer's heap-id base (the skinned span's first
    // heap vertex id; the draw fetch maps vi - base into the
    // deformed buffer). UINT32_MAX when nothing is skinned.
    [[nodiscard]] uint32_t GetSkinnedDeformedVertexBase() const {
        return skinnedDeformedBase_;
    }
    // ONE PARITY HALF of the deformed buffer, in bytes (the buffer
    // holds two halves; the driver's parity flip picks cur, the
    // motion leg reads cur + prev). The driver's BDA arithmetic:
    // cur = buffer BDA + (parity ? span : 0). 0 when nothing is
    // skinned.
    [[nodiscard]] uint64_t GetSkinnedDeformedSpanBytes() const {
        return skinnedDeformedSpanBytes_;
    }
    // The ranges table BDA (the compute pass's per-range
    // (heapBase, count, skinBase) rows - static, uploaded at
    // Ensure). 0 when nothing is skinned.
    [[nodiscard]] smvk::BufferHandle GetSkinnedRangesBuffer() const {
        return skinnedRangesBuf_;
    }

    // ------------------------------------------------------------------
    // The ANIMATION driver (the "GPU bone animation" box's CPU side -
    // the palette build; the joint-local writes + the global walk +
    // BuildSkinPalette run per frame on the CPU (85 joints x 300
    // keys: microseconds - the trivial box), the palette uploads +
    // the skinning pass run on the GPU).
    //
    // SetAnimClip attaches a loaded .tourAnim clip (the App loads it
    // - TOURMALINE_ANIM or a stem probe). SampleAnimation advances
    // the time cursor (looping the clip duration), samples every
    // track (SampleTrack), writes the joint locals into the
    // CLUSTER's own scene (the skeleton's joint ids + the .tourAnim
    // track ids are cluster-local by construction - the 1:1 node
    // map), marks the joint chains changed, recomputes globals,
    // builds the skin palette (BuildSkinPalette).
    // Untracked joints/channels keep the loaded (bind-pose) locals.
    // Returns false (no palette) when no clip or no skeleton.
    // ------------------------------------------------------------------
    void SetAnimClip(asset::Clip clip);
    [[nodiscard]] bool HasAnimClip() const {
        return animClip_.has_value();
    }
    bool SampleAnimation(float dt, std::vector<glm::mat4>& outPalette);
    // Same pose update with frame-backed output storage for renderer use.
    bool SampleAnimation(float dt, tour::mem::FrameVector<glm::mat4>& outPalette);
    bool SampleAnimationPose(float dt);
    // ANIMATOR-DRIVEN pose (World::Update flushes a live animator's
    // evaluated joint locals into this scene and sets this): the
    // renderer's skinning leg must NOT advance its own clip on a
    // driven cluster (the pose is already in the scene — double
    // advancing would run both cursors). Cleared when the driving
    // animator dies/destroys (the cluster resumes its own clip) or
    // never set (the normal path).
    bool poseExternallyDriven = false;

    // GPU-free offline mode (pak cooker without a Vulkan device).
    // UploadMesh records CPU geometry + retained uploads only
    // (no VMA/streamer touch); textures mint fake RHI handles whose
    // indices are stable offline ids (see Handle::Mint). The offline cluster
    // is serializable and nothing else: never register it with a World
    // or render from it. Implies SetRetainUploads(true).
    void SetOfflineMode(bool offline);
    bool IsOfflineMode() const {
        return offlineMode_;
    }
    // DYNAMIC cluster (see World::EnsureDynamicCluster): the World's
    // dedicated home for runtime-created entities (editor-created
    // lights/cameras, app demo content, later decals/particles) —
    // never pak content. Nothing serializes it; World::UnloadCluster
    // refuses it (pak unloads must not destroy runtime state). Purely
    // semantic otherwise: resolve folds it into the merged scene like
    // any other cluster.
    bool isDynamic = false;
    // ---- Shared-asset retention (the app AssetStore bridge) --------
    // EXTERNAL reference count held by ASSET-side owners (app::
    // AssetStore) on a cluster that ALSO carries shared asset rows
    // (the v1 pak boundary: mesh families ship inside their owning
    // scene cluster). World::UnloadCluster PARKS a cluster with live
    // refs (detached from the merged fold — its nodes stop rendering,
    // exactly like an unload; scene removal never resurrects scene
    // content) but keeps it alive in the world's retained list; the
    // last World::ReleaseCluster destroys it. The engine only
    // COUNTS — the retention POLICY belongs to the asset-side holder,
    // never here. Main-thread only (cluster lifetime state).
    uint32_t assetRefs = 0;
    // Allocates the next cook texture id (valid Handle::Mint index for
    // MaterialValues texture slots).
    uint32_t AllocOfflineTextureId();

    void DestroyMesh(ClusterStaticMeshHandle handle);
    void DestroyMaterial(ClusterMaterialHandle handle);

    // ------------------------------------------------------------------
    // URI-mapped resource lookup (AssetRef backing store).
    //
    // Identity = VFS path with optional '#'-fragment (see
    // Engine/Asset/asset_registry.h). The VFS overlay decides which
    // bytes back a path; these maps only resolve an already-loaded
    // asset inside this Cluster. Main-thread only (Cluster pools are
    // externally synchronized, see Memory/pool.h).
    //
    // Storage is string-free: keys are smvk::StringViewT slices owned by the
    // per-Cluster uriArena_ (freed wholesale with the Cluster) and the
    // maps are open-addressing UriMaps — lookups take caller views and
    // never allocate (the previous std::unordered_map<std::string,...>
    // round-tripped a std::string per lookup).
    // ------------------------------------------------------------------
    bool RegisterMeshUri(smvk::StringViewT uri, ClusterStaticMeshHandle handle);
    bool RegisterMaterialUri(smvk::StringViewT uri, ClusterMaterialHandle handle);
    bool RegisterLightUri(smvk::StringViewT uri, ClusterLightHandle handle);
    bool RegisterCameraUri(smvk::StringViewT uri, ClusterCameraHandle handle);
    bool RegisterDecalUri(smvk::StringViewT uri, ClusterDecalHandle handle);
    // Textures live in the global RHI pool for binding (materials store
    // RHI TextureHandles); the Cluster pool only owns lifetime Holders.
    // The uri map therefore stores the RHI handle used at bind time.
    bool RegisterTextureUri(smvk::StringViewT uri, smvk::TextureHandle handle);
    void UnregisterUri(smvk::StringViewT uri);

    std::optional<ClusterStaticMeshHandle> FindMeshByUri(smvk::StringViewT uri) const;
    std::optional<ClusterMaterialHandle> FindMaterialByUri(smvk::StringViewT uri) const;
    std::optional<ClusterLightHandle> FindLightByUri(smvk::StringViewT uri) const;
    std::optional<ClusterCameraHandle> FindCameraByUri(smvk::StringViewT uri) const;
    std::optional<ClusterDecalHandle> FindDecalByUri(smvk::StringViewT uri) const;
    std::optional<smvk::TextureHandle> FindTextureByUri(smvk::StringViewT uri) const;

    // Resolves an AssetRef in place: keeps ref.uri, fills cluster/handle
    // on hit, clears resolution on miss. Returns true on hit.
    template <typename HandleT> bool ResolveAssetRef(AssetRef<HandleT>& ref);

    // Reverse lookup for serialization: asset handle -> stored uri.
    // Returns an empty view when the handle was created without a
    // registered uri (procedural/default assets); the serializer then
    // assigns a "<cluster-local>#..." fallback uri. The view points
    // into the cluster's uri arena and stays valid for the cluster's
    // lifetime.
    smvk::StringViewT GetMeshUri(ClusterStaticMeshHandle handle) const;
    // The mesh's vertexBase into the STATIC pos/shading heaps (vertex
    // units — the global vertex ids the cluster-ordered index streams
    // carry). 0xFFFFFFFF when the handle is empty/unknown. Consumers
    // derive per-draw window deltas from it (the lightmap stream's
    // independently-packed windows — the draw ring's GfxDraw.misc2.w).
    [[nodiscard]] uint32_t GetMeshHeapVertexBase(ClusterStaticMeshHandle handle) const;
    smvk::StringViewT GetMaterialUri(ClusterMaterialHandle handle) const;
    smvk::StringViewT GetLightUri(ClusterLightHandle handle) const;
    smvk::StringViewT GetCameraUri(ClusterCameraHandle handle) const;
    smvk::StringViewT GetDecalUri(ClusterDecalHandle handle) const;

    // Per-mesh LOD marker (see Asset/mesh_optimizer.h). Stored into
    // MeshBlobs; the renderer does not branch on it. simplifyError is
    // MESH-LOCAL (scale-free) - the renderer applies the instance's
    // world scale at pick time (World::LodIndexForNode), so no
    // cook-time world-scaling pass exists (the removed
    // FinalizeLodWorldErrors compounded the scale per re-cook; see
    // the removal note in mesh_optimizer.cpp).
    void RegisterMeshLod(smvk::StringViewT uri, const smvk::LodInfo& lod);
    std::optional<smvk::LodInfo> GetMeshLod(smvk::StringViewT uri) const;
    // All LOD markers for cook post-passes.
    std::vector<std::pair<smvk::StringViewT, smvk::LodInfo>> ListMeshLods() const;

    // Per-mesh cooked meshlets (see Asset/meshlet.h). Stamped by the
    // importer when meshlet cooking is enabled, persisted in MeshBlobs,
    // and loaded into StaticMesh::meshlets on deserialize. The renderer
    // does not consume them yet (indirect path foundation only).
    void RegisterMeshMeshlets(smvk::StringViewT uri, const smvk::CookedMeshlets& meshlets);
    std::optional<smvk::CookedMeshlets> GetMeshMeshlets(smvk::StringViewT uri) const;

    // Enumerates registered texture URIs for serialization
    // ({uri, RHI handle} pairs, sorted by uri for deterministic paks).
    std::vector<std::pair<smvk::StringViewT, smvk::TextureHandle>> ListTextureUris() const;
    // Allocation-free enumeration for render-frame remapping. Ordering is
    // irrelevant to pool-index lookups; serializer callers retain the
    // sorted, owning ListTextureUris() API.
    template <typename Fn> void ForEachTextureUri(Fn&& fn) const {
        textureByUri_.ForEach(std::forward<Fn>(fn));
    }

    // glTF sampler parameters per texture uri (persisted into paks;
    // the renderer still uses its default sampler until sampler
    // selection is wired up — see Asset/asset_registry.h notes).
    void RegisterTextureSampler(smvk::StringViewT uri, const smvk::TextureSamplerDesc& desc);
    std::optional<smvk::TextureSamplerDesc> GetTextureSampler(smvk::StringViewT uri) const;

    // Embedded source bytes per texture uri (populated by the importer
    // so the cooker can embed .ktx2 payloads without re-reading the
    // host fs; also populated on pak load so re-cook works).
    // Call ReleaseTextureSources() to drop the copies at runtime.
    void SetTextureSource(smvk::StringViewT uri, std::vector<uint8_t> bytes);
    const std::vector<uint8_t>* GetTextureSource(smvk::StringViewT uri) const;
    void ReleaseTextureSources();
    // Pins the loaded pak image for BORROWED texture payloads (the v4
    // fast-load path: the texture streamer registers ktx spans owned
    // by the pak image instead of per-texture owned copies). The pin
    // keeps the payloads alive as long as the cluster — the streamer
    // contract for RegisterExisting (payload must outlive the
    // registration) is satisfied while the cluster lives.
    void PinPakImage(std::shared_ptr<const std::vector<uint8_t>> image);

    // ---- Geometry LOD streaming (Renderer/geometry_streamer.h) ----------
    // A DEFERRED LOD sibling: cooked, metadata-registered (GetMeshLod
    // resolves it - RebuildLodChains sees the full chain), but WITHOUT a
    // pool mesh or heap payload - not in VRAM. The record retains
    // everything the deserializer knew so PromoteDeferredMeshLod can
    // replay the exact upload + registration sequence later (the
    // metadata-survives-eviction requirement - only the PAYLOAD is
    // absent, never the identity).
    // Records PERSIST through promote (promotedHandle set) so EVICTION
    // can restore them: evict moves the mesh's runtime fields back into
    // the record, destroys the pool mesh, and clears promotedHandle -
    // the sibling is deferred again, re-promotable byte-identically.
    struct DeferredMeshLod {
        std::string uri;
        smvk::StaticGeometrySource src; // pak-image offsets/counts/aabb
        // The owning TOC entry's file-absolute offset (Stage 6: the
        // page-based promote rebases src by this — the entry's OWN
        // bytes are a complete contiguous payload, exactly like the
        // whole image is for the pin path).
        uint64_t entryOffset = 0;
        float sphere = 0.0f;
        uint32_t lmChunk = 0;
        std::vector<glm::vec4> lmXforms;
        smvk::LodInfo lod;
        smvk::CookedMeshlets meshlets;
        uint64_t payloadBytes = 0; // vertex+index bytes (budget/throttle)
        // Valid while the sibling is PROMOTED (resident): the pool
        // handle EvictMeshLod destroys. The record's payload fields are
        // MOVED OUT on promote and MOVED BACK on evict.
        ClusterStaticMeshHandle promotedHandle{};
    };
    // Park a sibling as deferred (the deserializer's streaming path).
    // Returns the accounted payload bytes (the load census).
    uint64_t DeferMeshLod(DeferredMeshLod&& d);
    [[nodiscard]] const DeferredMeshLod* FindDeferredMeshLod(smvk::StringViewT uri) const;
    // Promote: re-run the direct upload (one source, from the pinned pak
    // image) + the full registration the deserializer performed for
    // load-time meshes; the record STAYS (promotedHandle set) for the
    // evict path. Returns the fresh handle + the payload bytes (the
    // streamer's budget accounting) or nullopt (not deferred / no
    // pinned image / upload failed). Render thread, between frames.
    struct PromotedLod {
        ClusterStaticMeshHandle handle{};
        uint64_t payloadBytes = 0;
    };
    [[nodiscard]] std::optional<PromotedLod> PromoteDeferredMeshLod(smvk::StringViewT uri);
    // STAGE 6 (the pager lane): the same promote with the payload
    // supplied as the ENTRY'S OWN BYTES (a pager page — contiguous,
    // the whole blob) instead of the pinned whole image. The record's
    // file-absolute stream offsets rebase by entryOffset; the upload
    // stages from `entryPayload`. The caller owns the payload's
    // lifetime through the call (BulkCopyBuffer stages
    // synchronously — release the page after the return).
    [[nodiscard]] std::optional<PromotedLod> PromoteDeferredMeshLodFromPage(
        smvk::StringViewT uri,
        const uint8_t* entryPayload,
        size_t entryBytes);
    // The shared promote tail (both payload lanes — the pinned-image
    // path and the Stage-6 page path): meta re-attach + registration
    // + the record's promotedHandle.
    std::optional<PromotedLod> FinishPromoteDeferred(DeferredMeshLod* rec,
                                                     ClusterStaticMeshHandle h);
    // Evict: the inverse of promote - the promoted sibling's runtime
    // fields move back into its record, the pool mesh + heap ranges
    // retire (DestroyMesh - the range frees ride DeferDestroyTask, so
    // in-flight frames drawing the ranges stay safe), the mesh URI
    // unregisters (FindMeshByUri misses again - the chain sees the
    // sibling as deferred) while the smvk::LodInfo metadata STAYS. Returns
    // the freed payload bytes, 0 when the uri is not currently
    // promoted. Render thread, between frames.
    [[nodiscard]] uint64_t EvictMeshLod(smvk::StringViewT uri);
    [[nodiscard]] size_t DeferredLodCount() const;
    [[nodiscard]] uint64_t DeferredLodBytes() const;
    // ANY streaming record (deferred OR promoted) - the streamer tick's
    // active gate: with eviction on, the tick must keep running (touch
    // tracking + budget enforcement) even after everything promoted.
    [[nodiscard]] bool HasLodRecords() const {
        return !deferredLods_.empty();
    }

  private:
    // Owned URI key storage for all maps below; freed wholesale with
    // the Cluster (see asset_registry.cpp registration flow).
    smvk::Arena uriArena_;
    smvk::UriMap<ClusterStaticMeshHandle> meshByUri_;
    smvk::UriMap<ClusterMaterialHandle> materialByUri_;
    smvk::UriMap<ClusterLightHandle> lightByUri_;
    smvk::UriMap<ClusterCameraHandle> cameraByUri_;
    smvk::UriMap<ClusterDecalHandle> decalByUri_;
    smvk::UriMap<smvk::TextureHandle> textureByUri_;
    smvk::UriMap<smvk::TextureSamplerDesc> textureSamplers_;
    smvk::UriMap<std::vector<uint8_t>> textureSources_;
    smvk::UriMap<smvk::LodInfo> meshLods_;
    smvk::UriMap<smvk::CookedMeshlets> meshMeshlets_;
    // REVERSE URI LEDGERS (append-only, one entry per REGISTER call):
    // the forward maps above are latest-wins, so a URI re-registered
    // by a LATER occurrence of the SAME artifact (the documented
    // multi-reference pattern: one scene referencing crate.tour as
    // crate_a AND crate_b — every occurrence re-registers the same
    // .geometry URIs, see docs/prefabs_as_tour_scenes.md) silently
    // orphaned every EARLIER handle in the reverse lookup: Get*Uri
    // returned empty, the cook's write-back hard-errored ("cooked
    // mesh N carries no URI") and the serializer fell back to
    // duplicated per-occurrence cluster-local blobs. Every
    // registration appends (handle, uri); the reverse lookup walks
    // newest-first, so a handle re-registered under a new uri
    // reports its latest one. The ledgers are never pruned
    // (UnregisterUri only drops the FORWARD entry — same liveness
    // contract the forward map always had: a destroyed handle's
    // reverse row is unreachable garbage until the Cluster dies
    // with its arena).
    std::vector<std::pair<ClusterStaticMeshHandle, smvk::StringViewT>> meshUriLedger_;
    std::vector<std::pair<ClusterMaterialHandle, smvk::StringViewT>> materialUriLedger_;
    std::vector<std::pair<ClusterLightHandle, smvk::StringViewT>> lightUriLedger_;
    std::vector<std::pair<ClusterCameraHandle, smvk::StringViewT>> cameraUriLedger_;
    std::vector<std::pair<ClusterDecalHandle, smvk::StringViewT>> decalUriLedger_;
    bool retainUploads_ = false;
    std::unordered_map<uint32_t, MeshData> retainedUploads_;
    // Pak-image pin for borrowed texture payloads (see PinPakImage).
    std::shared_ptr<const std::vector<uint8_t>> pakImagePin_;
    // Bake payloads by VFS path (see the public block): borrowed spans
    // point into pakImagePin_, owned vectors cover LZ4 entries +
    // runtime injections.
    std::unordered_map<std::string, BakePayload> bakePayloads_;
    // Per-cluster static mesh allocations keyed by the local staticMeshes
    // pool index (pool indices are only unique within one Cluster).
    std::unordered_map<uint32_t, smvk::StaticMeshAllocation*> meshAllocations_;
    // Geometry-LOD streaming: deferred siblings by uri (see the
    // public block). Linear scan - the promote path touches one per
    // demand miss, the census counts them once.
    std::vector<DeferredMeshLod> deferredLods_;
    // Per-mesh skin streams (binding-2 data), keyed by mesh pool
    // index: the import post pass + the deserializer fill these (see
    // GetMeshSkinStream); the GPU allocation comes with the skinning
    // pipeline.
    std::unordered_map<uint32_t, std::vector<PackedSkinInfluences>> skinStreams_;
    // The Northlight compute-skinning GPU state (EnsureSkinnedGpuData):
    // the per-mesh ranges + the skinned-stream buffer (static) + the
    // deformed buffer (the per-frame output the draw paths fetch -
    // TWO parity halves of span bytes) + the deformed span's heap-id
    // base + the span bytes + the ranges table buffer.
    std::vector<SkinnedMeshRange> skinnedRanges_;
    smvk::Holder<smvk::BufferHandle> skinnedStream_;
    smvk::Holder<smvk::BufferHandle> skinnedDeformed_;
    smvk::Holder<smvk::BufferHandle> skinnedRangesBuf_;
    uint32_t skinnedDeformedBase_ = UINT32_MAX;
    uint64_t skinnedDeformedSpanBytes_ = 0;
    bool skinnedGpuValid_ = false;
    // The animation driver state: the attached clip + the time
    // cursor (seconds, looping the clip duration).
    std::optional<asset::Clip> animClip_;
    double animTime_ = 0.0;
    bool offlineMode_ = false;
    uint32_t offlineNextTextureId_ = 0;
};

template <typename HandleT> struct AssetRef {
    smvk::StringViewT uri;

    Cluster* cluster = nullptr;
    HandleT handle{};

    bool resolved() const {
        return cluster != nullptr && handle.valid();
    }

    void clearResolution() {
        cluster = nullptr;
        handle = {};
    }
};

// ResolveAssetRef specializations (defined here; the generic template
// is intentionally undefined for unsupported handle types).
template <typename HandleT> bool Cluster::ResolveAssetRef(AssetRef<HandleT>& ref) {
    static_assert(!sizeof(HandleT*), "ResolveAssetRef: unsupported handle type");
    return false;
}

template <>
inline bool Cluster::ResolveAssetRef<ClusterStaticMeshHandle>(
    AssetRef<ClusterStaticMeshHandle>& ref) {
    auto hit = FindMeshByUri(ref.uri);
    if (!hit) {
        ref.clearResolution();
        return false;
    }
    ref.cluster = this;
    ref.handle = *hit;
    return true;
}

template <>
inline bool Cluster::ResolveAssetRef<ClusterMaterialHandle>(AssetRef<ClusterMaterialHandle>& ref) {
    auto hit = FindMaterialByUri(ref.uri);
    if (!hit) {
        ref.clearResolution();
        return false;
    }
    ref.cluster = this;
    ref.handle = *hit;
    return true;
}

template <>
inline bool Cluster::ResolveAssetRef<ClusterTextureHandle>(AssetRef<ClusterTextureHandle>& ref) {
    // Cluster pool holders are lifetime-only; resolution by RHI handle
    // is served through FindTextureByUri (see asset_registry.cpp).
    (void)ref;
    return false;
}
} // namespace tour
// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the Cluster is
// ENGINE-OWNED and lives in tour; the smvk aliases keep the existing
// call sites compiling through the consumer migration.
namespace smvk {
using ::tour::AssetRef;
using ::tour::Cluster;
using ::tour::ClusterCameraHandle;
using ::tour::ClusterDecalHandle;
using ::tour::ClusterLightHandle;
using ::tour::ClusterMaterialHandle;
using ::tour::ClusterStaticMeshHandle;
using ::tour::ClusterTextureHandle;
using ::tour::GiBinding;
} // namespace smvk
