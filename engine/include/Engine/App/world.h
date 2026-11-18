// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Engine/App/animator.h>
#include <Engine/App/cluster.h>
#include <Engine/Asset/cluster_serializer.h> // smvk::ExternalTextureResolver
#include <Engine/Asset/lut_handle.h>
#include <Engine/Asset/mesh_handle.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Rendergraph/terrain_hfield.h>
#include <Engine/Scene/entity_handle.h> // smvk::AudioSourceHandle (typed)
#include <Engine/Scene/region_registry.h>
#include <Engine/Scene/scene.h>
#include <Engine/Scene/world_scene_instances.h>
#include <Engine/Utils/bvh.h>

namespace app {
class AssetManager;
}

namespace tour {
// (namespace/RHI boundary audit) this block's RHI dependencies are
// FULLY QUALIFIED (smvk::) - no broad using-directive (R6-gated).

class TextureStreamer;

// Tiled-terrain source payload (CPU side): harvested from a pak's
// Terrain blobs by LoadTerrainFromPak. The renderer bakes G-blocks
// from hfield (+ mask R into vertex color) at terrain build; masks
// are NOT kept resident past the bake in v1 (GPU mask textures land
// with the atlas in phase 4). Empty (hasTerrain=false) = no terrain.
struct TerrainSource {
    smvk::terrain::TerrainHField hfield{};
    // 256 S-tiles of 256x256 RGBA8, row-major (tz*16+tx) test masks.
    // Present only between harvest and the terrain bake.
    std::vector<std::vector<uint8_t>> masks{};
    // Optional packed conservative max-Y pyramid from the cook.
    std::vector<uint8_t> occlusionPyramid{};
    bool hasTerrain = false;
};

// Per-frame inputs for error-driven LOD selection (see World::
// LodIndexForNode). frameId is renderer-supplied and MONOTONIC (the
// RHI ring index is NOT — it wraps); it keys the per-node decision
// stamps (and the halfrate parity tick when full rate is off).
struct LodFrameInputs {
    uint64_t frameId = 0;
    glm::vec3 eye{0.0f};
    float viewportHeight = 1080.0f;
    float fovY = 0.7853982f; // 45 deg
    float pixelBudget = 1.0f;
};

// ---- Shader LODs (per-material shading programs) ---------------
// A LOD level carries a geometry sibling; the shading program that
// draws it is resolved per MATERIAL (see ShaderLodPolicy), not
// stamped on the level. The Main pass reads the resolved program,
// geometry consumers — shadows, prepass, motion, culling — ignore
// it. The farthest level of a cooked chain keeps the coarsest
// geometry and shades on the LIT program with the material's quality
// specialization (see ShaderLodQuality). The cheap vertex-lit
// whole-program substitution is KILLED: a far LOD never downgrades
// to per-vertex shading, so far instances keep the full lit pipeline
// (modulo the quality knobs). Every other level (and every uncooked
// chain) stays fully lit.

typedef enum IteratorFlagBits {
    IteratorFlagBit_None = 0,
    IteratorFlagBit_Renderable = 1u << 0,
    IteratorFlagBit_Opaque = 1u << 1,
    IteratorFlagBit_Transparent = 1u << 2,
    IteratorFlagBit_PassedCullTest = 1u << 3,
    IteratorFlagBit_StaticVertexFmt = 1u << 4,
    IteratorFlagBit_CastShadow = 1u << 5,
    IteratorFlagBit_LightDirectional = 1u << 6,
    IteratorFlagBit_LightSpot = 1u << 7,
    IteratorFlagBit_LightPoint = 1u << 8,
    // Set when the node sits inside a static subtree (see TransformHierarchy::
    // staticRoot/staticNode). Static nodes skip per-frame transform
    // updates and dynamic BVH rebuilds; collapsed mega-mesh nodes keep
    // this flag as well.
    IteratorFlagBit_Static = 1u << 9,
    // Cutout material (ShadingModel::Masked): alpha-tested draw path.
    // Set alongside Opaque so the masked/solid phase splits in the
    // shadow, prepass and motion passes test one bit instead of
    // fetching the material per node per pass.
    IteratorFlagBit_Masked = 1u << 10,
    // Volumetric-shadow caster (the coarse fog tile's marked large
    // occluders - see TransformHierarchy::castsVolumetricShadow): set alongside
    // Opaque/CastShadow for flagged nodes so the coarse fog-shadow
    // pass iterates a prefiltered list (forEachRenderItem's exact
    // flag-match walk). Material-independent - the SCENE owns the
    // mark (buildings/roofs/cliffs, not small props).
    IteratorFlagBit_CastVolumetricShadow = 1u << 11,
    // IteratorFlagBit_Skinned,
} IteratorFlagBits;
typedef uint32_t IteratorFlags;

// World is the single runtime scene graph. It is *not* an imported scene.
// It owns (has-a) the merged Scene hierarchy, owns the Pages that hold
// the actual mesh/material/texture resources, and resolves per-node mesh/
// material indices stored in the Scene to the owning WorldPage + handle.
//
// FLAT WORLD CONTRACT:
// - The World is a CONTAINER, not a transform node. There is no synthetic
//   world-root node: top level of the merged hierarchy is every live root
//   (parent == -1) of every merged cluster scene, preserved verbatim.
// - parent == -1 IS the root-node contract; a scene may carry any number
//   of roots, and node 0 is regular content (never "the world").
// - Relationships only change through explicit parenting operations; the
//   resolve never reparents merged content and never shifts levels.
// - Runtime spawns (camera pass-through pose, decals, audio sources,
//   mesh entities) are root-level by default.
// - OWNERSHIP is independent of hierarchy: cluster-owned resources ride
//   the WorldPage pools + the per-cluster location tables; the Region
//   registry exposes every merged ROOT as an ownership boundary. Removing
//   a parent never deletes cluster-owned nodes; unloading a cluster
//   removes exactly its nodes and resources through the resolve.
class ScenePageStreamer; // camera-driven page streaming (scene_page_streamer.h)
struct World {
    explicit World() {};
    // Must be set before loading/registering pages or first resolve.
    // Budget covers the merged CONTENT nodes (no synthetic root; runtime
    // spawn nodes count against it as they merge).
    // 0 = UNBUDGETED (the default host shape): node storage rides the
    // PROCESS linear allocator, the byte budget (--mem-process-mb /
    // TOURMALINE_MEM_PROCESS_MB) owns capacity and its [OOM] fail-fast
    // is the overflow abort. There is NO CLI flag.
    void ConfigureSceneNodeBudget(size_t slots) {
        if (!slots) {
            // 0 = clear to unbudgeted (a no-op when already 0): keep
            // any pre-reserved storage, drop the cap
            // (ReserveNodeSlots treats 0 as invalid).
            scene.nodeSlotBudget = 0;
            return;
        }
        if (!pages.empty() || !scene.hierarchy.empty())
            tour::transformhierarchy::CrashNodeBudget(
                "late World budget", scene.hierarchy.size(), slots);
        scene.ReserveNodeSlots(slots);
    }

    // The World owns nodes. The project catalog names assets; LoadMesh
    // resolves a catalog key through the attached app AssetManager,
    // returning a reusable loaded resource handle. Game code attaches
    // this handle to nodes; no raw world mesh indices escape this API.
    [[nodiscard]] app::MeshHandle LoadMesh(uint64_t catalogKey, std::string& error);
    // Refuses while a World node still references the handle.
    bool UnloadMesh(app::MeshHandle mesh);
    // Global artistic LUT, with the same explicit catalog-key ->
    // loaded resource-handle discipline as meshes. A LUT is a world
    // rendering setting, not a mesh/node component.
    [[nodiscard]] app::LutHandle LoadLut(uint64_t catalogKey, std::string& error);
    [[nodiscard]] app::LutHandle LoadLut(uint64_t catalogKey) {
        lastGameAssetError_.clear();
        return LoadLut(catalogKey, lastGameAssetError_);
    }
    bool SetLut(app::LutHandle lut);
    bool UnloadLut(app::LutHandle lut);
    // Ergonomic overload for callers that do not need an immediate
    // out-error; details remain available through LastAssetError().
    [[nodiscard]] app::MeshHandle LoadMesh(uint64_t catalogKey) {
        lastGameAssetError_.clear();
        return LoadMesh(catalogKey, lastGameAssetError_);
    }
    [[nodiscard]] const std::string& LastAssetError() const {
        return lastGameAssetError_;
    }
    // Gameplay node entry. Root-level by default (the flat-world
    // contract: relationships only form through EXPLICIT parenting).
    // A live `parent` handle links the child under it — `localFrame` is
    // then PARENT-RELATIVE. A stale/dead parent handle is a
    // generation-checked REFUSAL (returns the invalid handle), never a
    // silent root spawn. Game nodes + their links live until the next
    // full legacy resolve (the documented game-node lifetime).
    [[nodiscard]] NodeHandle AddNode(const glm::mat4& localFrame = glm::mat4(1.0f),
                                     NodeHandle parent = kNodeInvalidHandle);
    bool RemoveNode(NodeHandle node);
    // World-space identity, LOCAL-space TRS mutation. These accept any live
    // node in World::scene, including loaded/additive scene nodes; they do
    // not require World::AddNode gameplay ownership. Failed validation or
    // non-TRS inputs leave the node unchanged.
    bool SetNodePosition(NodeHandle node, const glm::vec3& position);
    bool SetNodeRotation(NodeHandle node, const glm::quat& rotation);
    bool SetNodeScale(NodeHandle node, const glm::vec3& scale);

    [[nodiscard]] bool IsGameNode(NodeHandle node) const {
        return gameNodes_.contains(node) && NodeHandleValid(scene, node);
    }
    // Called by AssetManager for the duration of its attachment.
    // AssetManager must outlive its loaded mesh handles.
    // The runtime installs its asset service here. Ownership follows the
    // World lifetime, so no app::AssetManager setup is needed in game
    // code. The concrete type lives in the apphost; this erased owner
    // avoids a dependency from engine back onto apphost.
    void OwnGameAssets(std::shared_ptr<void> owner) {
        gameAssetOwner_ = std::move(owner);
    }
    [[nodiscard]] bool HasAssetService() const {
        return gameAssets_ != nullptr;
    }
    void SetGameAssets(app::AssetManager* assets) {
        if (gameAssets_ && gameAssets_ != assets)
            ClearGameMeshBindings();
        gameAssets_ = assets;
    }
    // Called before releasing all assets or the bound manager.
    // Attached nodes become meshless; their transforms and value
    // components remain intact.
    void ClearGameMeshBindings() {
        if (gameMeshBindings_.empty())
            return;
        for (const auto& [handle, resource] : gameMeshBindings_)
            if (IsGameNode(handle))
                (void)tour::transformhierarchy::RemoveMesh(scene, NodeHandleSlot(handle));
        gameMeshBindings_.clear();
        RefreshRuntimeSceneCaches();
    }
    [[nodiscard]] bool HasGameMeshBinding(uint64_t resourceValue) const {
        for (const auto& [node, mesh] : gameMeshBindings_)
            if (mesh == resourceValue && IsGameNode(node))
                return true;
        return false;
    }

    class NodeRef {
      public:
        NodeRef(World& owner, NodeHandle handle) : owner_(owner), handle_(handle) {}
        // Requires a PREVIOUSLY loaded app::MeshHandle. One mesh per
        // node: replacement is atomic on invalid/unmapped handles.
        bool SetMesh(app::MeshHandle mesh) const;
        bool RemoveMesh() const;
        // Per-instance direct-light linking (32 user-defined channels).
        // This is node-local, not shared by mesh instances.
        bool SetLightLayerMask(uint32_t mask) const {
            if (!owner_.IsGameNode(handle_))
                return false;
            tour::transformhierarchy::EnsureNodeMetadata(owner_.scene);
            owner_.scene.metadata[NodeHandleSlot(handle_)].lightLayerMask = mask;
            return true;
        }
        bool SetLocalTransform(const glm::mat4& frame) const {
            return owner_.IsGameNode(handle_) &&
                   tour::transformhierarchy::SetLocalTransform(owner_.scene, handle_, frame);
        }
        bool SetPosition(const glm::vec3& position) const {
            return owner_.SetNodePosition(handle_, position);
        }
        bool SetRotation(const glm::quat& rotation) const {
            return owner_.SetNodeRotation(handle_, rotation);
        }
        bool SetScale(const glm::vec3& scale) const {
            return owner_.SetNodeScale(handle_, scale);
        }
        bool SetPhysicsBody(const tour::transformhierarchy::ScenePhysicsBody& body) const {
            return owner_.IsGameNode(handle_) && tour::transformhierarchy::SetPhysicsBody(
                                                     owner_.scene, NodeHandleSlot(handle_), body);
        }
        bool SetAudioRoom(const tour::transformhierarchy::SceneAudioRoom& room) const {
            return owner_.IsGameNode(handle_) && tour::transformhierarchy::SetAudioRoom(
                                                     owner_.scene, NodeHandleSlot(handle_), room);
        }
        bool SetTriggerVolume(const tour::transformhierarchy::SceneTriggerVolume& trigger) const {
            return owner_.IsGameNode(handle_) &&
                   tour::transformhierarchy::SetTriggerVolume(
                       owner_.scene, NodeHandleSlot(handle_), trigger);
        }

      private:
        World& owner_;
        NodeHandle handle_;
    };
    [[nodiscard]] NodeRef operator[](NodeHandle handle) {
        return NodeRef(*this, handle);
    }

    // In-place draw/cull refresh, never the destructive cluster fold.
    void RefreshRuntimeSceneCaches();
    // Legacy ResolveAndFixup currently invalidates game nodes; this
    // pending 6c migration is an explicit limitation of this slice.
    std::unordered_set<NodeHandle> gameNodes_;
    // Cross-resolve per-slot generation high water mark. The legacy
    // scene reset cannot re-mint a stale game handle on reused slots.
    std::unordered_map<NodeId, uint32_t> gameSlotGeneration_;
    std::unordered_map<NodeHandle, uint64_t> gameMeshBindings_;
    app::AssetManager* gameAssets_ = nullptr;
    std::string lastGameAssetError_;

    // Merged runtime scene graph. Imported WorldPage scenes are folded into
    // this via ResolveAndFixup_pages().
    TransformHierarchy scene;
    // Registered streaming/ownership subtree roots in the authoritative
    // world hierarchy. Registration does not take ownership of assets:
    // cluster-backed resource pools remain the transitional asset layer.
    RegionRegistry regions;
    // Direct World-owned hierarchy instances do not create Pages or
    // duplicate authoring Scenes. Asset leases are instance-independent.
    tour::transformhierarchy::WorldSceneInstances worldSceneInstances;
    // One active authored Map lives in this runtime World.
    bool SetActiveMapIdentity(uint64_t id) {
        return worldSceneInstances.SetActiveMap(id);
    }
    uint64_t ActiveMapIdentity() const {
        return worldSceneInstances.ActiveMap();
    }
    uint64_t InstantiateSceneHierarchy(
        const TransformHierarchy& source,
        uint64_t sceneAssetId,
        const glm::dmat4& placement,
        std::vector<tour::asset::AssetLease> assets,
        std::string& err,
        const tour::transformhierarchy::WorldSceneResourceMap* bindings = nullptr);
    bool RetireSceneHierarchy(uint64_t instanceId);

    // Registration of a region root is separate from node lifetime;
    // the caller must retire the subtree independently at unload.
    uint64_t RegisterRegion(NodeHandle root, RegionKind kind = RegionKind::Scene) {
        return regions.Register(scene, root, kind);
    }
    bool UnregisterRegion(uint64_t id) {
        return regions.Unregister(id);
    }
    const SceneRegion* RegionForNode(NodeId node) const {
        return regions.RegionOf(scene, node);
    }
    // Monotonic per-World owner registration identity; never use a
    // recycled WorldPage* address as the permanent GPU scene key.
    uint64_t gpuSceneTopologyRevision = 0;
    uint64_t nextGpuScenePageId = 1;
    // Monotonic stable fold id source (WorldPage::pageOrdinal): the
    // incremental merge's stable discriminator, independent of the dense
    // `pages` slot index (which shifts on unload).
    uint32_t nextPageOrdinal_ = 0;
    // Gameplay nodes share the same monotonically allocated registration namespace
    // as WorldPage owners, but use World node slots/generations for identity.
    uint64_t gpuSceneGameNodeRegistration = 0;
    std::unordered_map<const WorldPage*, uint64_t> gpuScenePageIds;

    bool pagesDirty_ = false; // pending cluster scene merge
    mutable bool dirtyCull = true;
    std::vector<IteratorFlags> flags; // indexed by node
    std::vector<std::unique_ptr<WorldPage>> pages;
    // INCREMENTAL FOLD STATE (persists across MergePage/UnmergePage
    // deltas; reset by ResolveAndFixup_pages before a full rebuild): the
    // material dedup map (stable WorldPage::pageOrdinal -> cluster-local
    // material -> world material index) and the first-wins flags.
    std::unordered_map<uint64_t, uint32_t> worldMatByClusterMat_;
    bool envTaken_ = false;
    bool renderTaken_ = false;
    bool defaultCamTaken_ = false;
    bool activeEnvTaken_ = false;
    // MATERIAL USERDATA/SHADER-FAMILY re-key capture (stable-keyed, moved to
    // members so the incremental RebuildDerivedCaches can re-emit them into the
    // new index layout after a batch of deltas).
    std::vector<std::tuple<WorldPage*, WorldPageMaterialHandle, uint32_t>> oldMaterialUserData_;
    std::vector<std::tuple<WorldPage*, WorldPageMaterialHandle, uint8_t>> oldMaterialShaderFamily_;
    // The camera-driven page streamer (see scene_page_streamer.h): set by
    // the host loader after the entry scene loads; null = streaming
    // disabled (every page is resident, the legacy load-everything model).
    std::shared_ptr<ScenePageStreamer> pageStreamer;
    // RETAINED pages (the shared-asset bridge, WorldPage::assetRefs):
    // pages PARKED by UnloadCluster while asset-side references
    // were live. A parked cluster is OUT of the merged fold + every
    // location table (its scene instance is gone — removal never
    // resurrects scene content) but its resource rows (meshes...)
    // stay alive for the reference holders (app::AssetStore). The
    // last ReleasePage destroys it. UnregisterPage can pull a
    // parked cluster back out for re-parenting. Main-thread only.
    std::vector<std::unique_ptr<WorldPage>> retainedPages;

    // Tiled-terrain source (see TerrainSource): empty unless a loaded
    // pak carried Terrain blobs.
    TerrainSource terrainSource;

    // ---- Compact draw list (hot-path iterator) --------------------
    // Built once per ResolveAndFixup_pages() and kept in sync by
    // Cull(). Every renderable node (mesh attached) appears exactly
    // once, sorted by material so per-material pipeline binds run
    // once per material per pass. Node id + world-level mesh/material
    // indices are precomputed here: iterating this list replaces the
    // per-node scene.tables hash-free row lookups and
    // the full hierarchy scan of forEachNode (which every pass used
    // to repeat, ~20-30 times per frame).
    // material == kInvalidIndex marks a mesh node with no material;
    // passes that require one (all but the depth prepass) skip it.
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
    struct RenderItem {
        NodeId node;
        uint32_t mesh;     // world mesh index (meshLocations)
        uint32_t material; // world material index (materialLocations)
        IteratorFlags flags;
        // PSO pipeline class (the raster/cull/masked/family bin, see
        // tour::gpu_scene::RasterPipelineClass). Precomputed at draw-list
        // build so per-bin iteration is a contiguous range, no flag scan.
        uint8_t psoClass = 0;
        // The resolved MATERIAL-INSTANCE linkage: the bound
        // instance's HANDLE VALUE (0 = none — inherit the base
        // material default). Stored as the raw handle so a
        // destroyed instance (or a younger one that reused the
        // slot) fails the generation check in ResolvedItemUserData
        // and cleanly falls back, never aliasing a stranger's
        // value. The LINKAGE is resolved here (drawItems build /
        // entity mirrors); the VALUE stays live in the pool —
        // per-frame value updates (SetMaterialInstanceUserData:
        // the damage flash fade) are plain pool writes, no
        // rebuild, visible on the next frame build.
        uint64_t materialInstance = 0;
    };
    std::vector<RenderItem> drawItems; // sorted by (psoClass, material); see above

    // SoA mirror of drawItems[].flags: a dense flags-only column the
    // SIMD scan in ItemIndicesFor walks 8 items per iteration.
    std::vector<IteratorFlags> itemFlags;

    // Per-PSO-bin contiguous ranges into drawItems: psoBinOffset_[c] is the
    // first drawItems index of pipeline class c, the class runs to
    // psoBinOffset_[c+1] (or drawItems.size()). The +1 sentinel bounds the
    // last class. drawItems is kept sorted by (psoClass, material), so each
    // class is a contiguous range with no flag scan.
    std::array<uint32_t, 8 + 1> psoBinOffset_{};

    // INCREMENTAL DRAW LIST (the streaming path): pages never share
    // hierarchy and materials are PER-PAGE (world material indices are a
    // contiguous range assigned in page-merge order), so the material sort
    // decomposes into per-page chunks concatenated in ordinal order. Each
    // cooked page's chunk is sorted ONCE and cached; the dynamic page's
    // chunk (runtime entities) is rebuilt every batch. Parallel to `pages`
    // (dense ordinal), erased on UnmergePage.
    struct PageDrawChunk {
        std::vector<RenderItem> items;
        bool built = false;
    };
    std::vector<PageDrawChunk> pageDrawChunks_;

    // Lazily cooked index lists into drawItems, keyed by the
    // required-flags combination (call sites use ~3 distinct combos).
    // Built once per combo on first request — vectorized on AVX2
    // (AND + CMPEQ + movemask + bit-scan over itemFlags) — and
    // invalidated whenever flags change (ResolveAndFixup rebuild,
    // Cull). Hot passes then iterate a contiguous, prefiltered
    // uint32 index array: no per-item flag test, no branch
    // mispredicts, and pairs perfectly with the material sort.
    mutable std::unordered_map<IteratorFlags, std::vector<uint32_t>> flagLists;

    // Returns the cooked item-index list for a flag combination
    // (lazily built; see flagLists above).
    const std::vector<uint32_t>& ItemIndicesFor(IteratorFlags requiredFlags) const;

    // Hot-path iterator over drawItems (see above). `func` takes
    // const RenderItem&. The flag test is resolved through the
    // cooked index list, not per-item.
    template <typename Func>
    void forEachRenderItem(IteratorFlags requiredFlags, Func&& func) const {
        if (requiredFlags == static_cast<IteratorFlags>(IteratorFlagBit_None)) {
            for (const RenderItem& item : drawItems)
                func(item);
            return;
        }
        for (uint32_t idx : ItemIndicesFor(requiredFlags))
            func(drawItems[idx]);
    }

    // Contiguous per-pipeline-class range into drawItems: [begin, end) for
    // class `cls`. drawItems is kept sorted by (psoClass, material), so a
    // pass iterates the class range directly with no flag scan.
    std::pair<uint32_t, uint32_t> PsoBinRange(uint32_t cls) const {
        if (cls + 1 >= psoBinOffset_.size())
            return {0u, 0u};
        return {psoBinOffset_[cls], psoBinOffset_[cls + 1]};
    }

    // Hot-path iterator over ONE pipeline class's contiguous range.
    template <typename Func> void forEachPsoBin(uint32_t cls, Func&& func) const {
        const auto [b, e] = PsoBinRange(cls);
        for (uint32_t i = b; i < e; ++i)
            func(drawItems[i]);
    }

    // Resource lookup into pages
    struct MeshLocation {
        WorldPage* cluster;
        WorldPageStaticMeshHandle handle;
    };
    struct MaterialLocation {
        WorldPage* cluster;
        WorldPageMaterialHandle handle;
    };
    struct TextureLocation {
        WorldPage* cluster;
        WorldPageTextureHandle handle;
    };
    // Cooked punctual lights (KHR_lights_punctual): pool assets like
    // meshes — scene.lightForNode stores world indices into this table,
    // each entry resolving back to the owning WorldPage + handle.
    struct LightLocation {
        WorldPage* cluster;
        WorldPageLightHandle handle;
    };
    // Authored cameras: pool assets like lights — scene.cameraForNode
    // stores world indices into this table, each entry resolving back
    // to the owning WorldPage + handle.
    struct CameraLocation {
        WorldPage* cluster;
        WorldPageCameraHandle handle;
    };
    // Authored/runtime decals: pool assets like lights — scene.decalForNode
    // stores world indices into this table, each entry resolving back to
    // the owning WorldPage + handle. The shape rides the node transform
    // (see SceneDecal).
    struct DecalLocation {
        WorldPage* cluster;
        WorldPageDecalHandle handle;
    };
    // Indexed by the mesh/material index stored in the scene's
    // tables (tables.meshes / tables.materials). Each entry points back to the owning
    // WorldPage so resources stay owned by pages.
    std::vector<MeshLocation> meshLocations;
    std::vector<MaterialLocation> materialLocations;
    // The fallback material for renderable surfaces that carry no material
    // row (Caldera strips materials). A world index into materialLocations,
    // resolved once by ResolveAndFixup_pages from the cooked
    // "editor://default" material; kInvalidIndex when no default exists.
    uint32_t defaultMaterialIndex = kInvalidIndex;
    std::vector<TextureLocation> textureLocations;
    std::vector<LightLocation> lightLocations;
    std::vector<CameraLocation> cameraLocations;
    std::vector<DecalLocation> decalLocations;
    // WorldPage-slot -> MERGED-scene node id, parallel to `pages`
    // (filled by ResolveAndFixup; UINT32_MAX = hole/retired). The
    // retained mapping is what lets the entity layers key state by
    // the STABLE (cluster, cluster-local node) pair while reading
    // positions from the merged global transforms — merged node
    // ids themselves are rebuilt on every resolve (unstable).
    std::vector<std::vector<NodeId>> pageMergedNodes;
    // Runtime audio-source entity registry (the smvk::AudioSourceHandle
    // slots — see SpawnAudioSource). Keyed by handle index; the
    // generation is the stale-handle guarantee. `owner` +
    // `clusterNode` are the DURABLE identity (the dynamic cluster
    // + its own scene's node); `mergedNode` is the resolved/mirrored
    // merged-scene node (rebuilt by resolve/mirror).
    struct AudioSourceEntity {
        uint32_t generation = 1;
        bool alive = false;
        WorldPage* owner = nullptr;
        NodeId clusterNode = 0;
        NodeId mergedNode = 0;
    };
    std::vector<AudioSourceEntity> audioEntities;
    std::vector<uint32_t> audioEntityFree;
    // Runtime MESH entities: a bare durable transform node (dynamic
    // cluster) + CROSS-CLUSTER mesh/material bindings stored as the
    // stable (owner cluster, pool handle) pair — the merged-scene
    // rows are mirrors the resolve tail re-establishes. The
    // animatorSlot is the ATTACHED animator (an explicitly-owned
    // attachment: DestroyMesh cleans it up).
    struct MeshEntity {
        uint32_t generation = 1;
        bool alive = false;
        WorldPage* transformOwner = nullptr; // the dynamic cluster
        NodeId clusterNode = 0;              // durable transform node
        NodeId mergedNode = 0;               // resolved/mirrored node
        WorldPage* meshOwner = nullptr;      // the mesh's owning cluster
        WorldPageStaticMeshHandle mesh{};
        WorldPageMaterialHandle material{}; // empty = material-less
        uint32_t animatorSlot = UINT32_MAX;
        // The OPTIONAL material instance (the 32-bit userdata
        // override lane — see the material-instance registry
        // below): empty = inherit the base material's default.
        // Binding changes mark the world dirty like any other
        // mirror edit; the INSTANCE VALUE stays live in the pool.
        smvk::MaterialInstanceHandle materialInstance{};
    };
    std::vector<MeshEntity> meshEntities;
    std::vector<uint32_t> meshEntityFree;
    // ---- material instances (the userdata override lane,
    // docs/shader_library_design.md Phase A) ----
    // A material instance references ONE base material (the world
    // material index at creation, re-keyed through resolves like
    // every location) and optionally overrides the material's
    // 32-bit userdata. NO MaterialValues copy: the base material's
    // GPU row, textures, PBR factors, alpha mode and batching stay
    // SHARED — the instance only owns the one runtime word, so
    // instance value changes are never a reupload, a pipeline
    // event or a batch split. The base binding is stored as the
    // stable (owner cluster, pool handle) pair so cluster
    // unloads/re-resolves re-key it like the entity bindings.
    struct MaterialInstanceEntity {
        uint32_t generation = 1;
        bool alive = false;
        WorldPage* baseOwner = nullptr; // the base material's owner
        WorldPageMaterialHandle base{}; // ... and its pool handle
        uint32_t userData = 0;
        bool overridesUserData = false;
    };
    std::vector<MaterialInstanceEntity> materialInstances;
    std::vector<uint32_t> materialInstanceFree;
    // Per-MATERIAL userdata defaults, parallel to materialLocations
    // (the resolved value for every draw of the material that has
    // no live instance override). Re-keyed at every resolve through
    // the (owner, handle) capture so the values survive cluster
    // loads/unloads reordering the index layout. NEVER inside
    // MaterialValues (the GPU 128B row): a value change is a CPU
    // write, nothing more.
    std::vector<uint32_t> materialUserData;
    // Per-MATERIAL shader family (MaterialShaderFamily, packed in
    // uint8_t rows), parallel to materialLocations + re-keyed by
    // the same (owner, handle) capture as materialUserData — the
    // Phase B lane over the userdata lane (the family is CODE
    // identity: which compiled lit FS the material's draws bind;
    // the userdata is the runtime VALUE the family's hooks read).
    // NEVER inside MaterialValues (CPU pipeline-selection state,
    // design §5) and NEVER per-instance (instances inherit the
    // base family; only userdata overrides per instance).
    std::vector<uint8_t> materialShaderFamily;
    // Animator registry (the AnimatorState payloads + generational
    // slots — see animator.h; meshEntitySlot back-references the
    // attached entity for the DestroyMesh cleanup).
    struct AnimatorEntity {
        uint32_t generation = 1;
        bool alive = false;
        uint32_t meshEntitySlot = UINT32_MAX;
        AnimatorState state{};
    };
    std::vector<AnimatorEntity> animators;
    std::vector<uint32_t> animatorFree;
    // Per-mesh LOD markers parallel to meshLocations (cook-time chain:
    // LOD0 + "/lodN" siblings with world-scaled simplifier error used
    // for runtime LOD selection — see SelectLodIndex).
    std::vector<smvk::LodInfo> meshLodInfos;

    WorldPage& LoadPage(smvk::ResourceManager& ctx);
    // Deserializes a cooked pak (PakReader over caller-owned bytes) into
    // a new registered WorldPage. Throws std::runtime_error on failure.
    // The caller issues ctx.FlushUploads() after ResolveAndFixup.
    // streamer (optional): v4 textures carrying a digest stream through
    // CreateStreamingTextureFromMeta (tail-only upload, no KTX parse)
    // and register for demand upgrades — imagePin (the loaded pak
    // image) must then be given to pin the borrowed payloads.
    WorldPage& LoadPageFromPak(smvk::ResourceManager& ctx,
                               const asset::PakReader& reader,
                               uint32_t sceneIndex = 0,
                               TextureStreamer* streamer = nullptr,
                               std::shared_ptr<const std::vector<uint8_t>> imagePin = {},
                               const smvk::ExternalTextureResolver* externalTexture = nullptr);
    // The camera-driven page streamer (null until the host loader installs
    // one; see scene_page_streamer.h). The renderer ticks it once per frame.
    ScenePageStreamer* PageStreamer() {
        return pageStreamer.get();
    }
    void SetPageStreamer(std::shared_ptr<ScenePageStreamer> s) {
        pageStreamer = std::move(s);
    }
    // Forwards the camera position + forward to the page streamer (a no-op
    // when streaming is disabled). Returns true when the page set changed
    // (the merged scene was rebuilt + uploads flushed by the streamer).
    bool StreamPages(glm::vec3 cameraPos, glm::vec3 viewDir = {});
    // Harvests the pak's Terrain blobs (terrain/meta + 16x H + 256x S)
    // into terrainSource (byte copies; the reader may die after). Also
    // called by LoadPageFromPak (non-fatal: scene loads even when
    // terrain is broken). False with err set on malformed terrain.
    bool LoadTerrainFromPak(const asset::PakReader& reader, std::string& err);
    // The world's DYNAMIC cluster (see WorldPage::isDynamic): the
    // dedicated home for runtime-created entities (editor-created
    // lights/cameras, app demo content, later decals/particles).
    // Lazily created on first call — repeat calls return the SAME
    // cluster. ctx is the host context (nullptr only in GPU-less
    // tests: value entities need no GPU). NOT resolved here: callers
    // either edit incrementally (node in the merged scene + location
    // push, the editor's live path) or keep their nodes in the
    // dynamic cluster's own scene so a later resolve folds them in.
    WorldPage& EnsureDynamicPage(smvk::ResourceManager* ctx);
    // Runtime decal creation (app presets, the renderer's J crosshair
    // spawn): appends a node with `frame` as its local transform
    // (translation = projector center, local -Z into the surface, +Y
    // texture V up, scale = half extents x/y/z — see SceneDecal) plus a
    // SceneDecal look def in the DYNAMIC cluster. The durable home is
    // the dynamic cluster's OWN scene, so a later ResolveAndFixup
    // (e.g. a pak-cluster unload) folds the decal back in like cooked
    // content — runtime entities survive pak unloads. A MIRROR node +
    // link land in the merged scene incrementally for immediate
    // visibility (the mirror dies at the next resolve; the original
    // folds in). Returns the merged-scene node id (UINT32_MAX on
    // failure).
    NodeId CreateDecalAt(const glm::mat4& frame, SceneDecal def);

    // ---- runtime audio-source entities (the gameplay-handle
    // surface — see Engine/Scene/entity_handle.h) ----
    // Spawns a runtime audio-source EMITTER: a transform node (the
    // durable home is the dynamic cluster's own scene, so the
    // entity survives pak-cluster unloads + resolves — the
    // CreateDecalAt pattern) + the SceneAudioSource payload row +
    // a registry slot keyed by the returned smvk::AudioSourceHandle. A
    // MIRROR row lands in the merged scene for immediate
    // visibility. Gameplay code holds the handle and destroys it
    // EXPLICITLY (World::DestroyAudioSource); authored .tour
    // sources carry no handle (they die with their scene).
    // `parent` (optional, UINT32_MAX = the world root) parents
    // the durable node — the emitter pose resolves from the node's
    // GLOBAL transform at use, so parenting applies like meshes.
    smvk::AudioSourceHandle SpawnAudioSource(const glm::mat4& localFrame,
                                             NodeId parent,
                                             const SceneAudioSource& def);
    // Destroys the spawned emitter (its rows + nodes drop, its
    // registry generation advances — every stale copy of the
    // handle fails validation forever). False on a stale/unknown
    // handle. The VOICES keyed to it stop: the audio manager
    // validates its emitters against IsAudioSourceAlive each
    // update (nothing here reaches the mixer).
    bool DestroyAudioSource(smvk::AudioSourceHandle handle);
    [[nodiscard]] bool IsAudioSourceAlive(smvk::AudioSourceHandle handle) const;
    // The emitter's MERGED-scene node (UINT32_MAX = stale handle
    // or not-yet-resolved). Unstable across resolves — the durable
    // identity is the handle; use this only for same-frame reads
    // (FindAudioSource on the merged row, gizmo selection...).
    [[nodiscard]] NodeId AudioSourceNode(smvk::AudioSourceHandle handle) const;

    // ---- runtime MESH entities (the gameplay-handle surface,
    // Engine/Scene/entity_handle.h — one spawn API, static and
    // skinned alike; skinning is a capability of the MESH ASSET,
    // not a separate entity category) ----
    // Instances a loaded world mesh onto a fresh entity node: the
    // durable home is the dynamic cluster's own scene (a bare
    // transform — the CreateDecalAt pattern), and the MESH +
    // MATERIAL bindings live on the ENTITY (cross-cluster: the
    // source mesh stays owned by its pak cluster; the merged-scene
    // mirror rows resolve through world meshLocations, and the
    // resolve tail RE-MIRRORS them after every rebuild — the
    // binding is stored as the stable (owner cluster, pool handle)
    // pair). `frame` is the entity node's local transform.
    // SpawnMeshFromNode copies the source NODE's mesh + material
    // associations (the classic spawn-point instancing); empty
    // handle on a bad source.
    smvk::MeshEntityHandle SpawnMeshFromNode(NodeId sourceNode,
                                             const glm::mat4& frame,
                                             NodeId parent);
    // The KEY-DRIVEN spawn (the content-as-code lane, the typed-key
    // gameplay surface): instances a WORLD MESH ROW directly — no
    // authored source node. The game loads a typed project mesh key
    // through app::AssetManager (LoadMesh — MeshWorldIndex resolves
    // the handle's CURRENT row fresh) and spawns HERE; the row's
    // owning cluster keeps the mesh (cross-cluster binding, exactly
    // like SpawnMeshFromNode). `worldMaterialIndex` is the optional
    // base-material row (kInvalidIndex = material-less). The parent
    // contract matches SpawnMeshFromNode (the resolved hierarchy
    // owns the merge). Empty handle on a bad row.
    smvk::MeshEntityHandle SpawnMesh(uint32_t worldMeshIndex,
                                     const glm::mat4& frame,
                                     NodeId parent,
                                     uint32_t worldMaterialIndex = kInvalidIndex);
    // The shared spawn core (both entries): the durable transform
    // node (dynamic cluster) + the merged mirror (fresh world
    // indices) + the entity slot. NOT API — the public entries
    // resolve the (mesh, material) bindings from their own sources.
    smvk::MeshEntityHandle SpawnMeshEntityInternal(const MeshLocation& meshLoc,
                                                   const MaterialLocation* matLoc,
                                                   const glm::mat4& frame);
    // Destroys the entity (rows + nodes drop, the generation
    // advances — stale copies fail validation forever). The
    // ATTACHED animator (an explicitly-owned attachment) dies with
    // it: its playback state + its external-pose claim drop; an
    // independently-held smvk::AnimatorHandle fails validation.
    bool DestroyMesh(smvk::MeshEntityHandle handle);
    [[nodiscard]] bool IsMeshEntityAlive(smvk::MeshEntityHandle handle) const;
    // The entity's MERGED-scene node (UINT32_MAX = stale). Like
    // AudioSourceNode: unstable across resolves, same-frame use.
    [[nodiscard]] NodeId MeshEntityNode(smvk::MeshEntityHandle handle) const;
    // The world meshLocations index the entity's mesh resolves
    // through (UINT32_MAX = stale/unresolvable — the owning
    // cluster may have been unloaded).
    [[nodiscard]] uint32_t MeshEntityMeshIndex(smvk::MeshEntityHandle handle) const;

    // ---- material runtime state + MATERIAL INSTANCES (the
    // 32-bit userdata lane, docs/shader_library_design.md) ----
    // userData is ONE runtime word per resolved draw: the material
    // instance override or the base material default, resolved at
    // frame build and forwarded flat to the lit fragment's
    // TM_ModifySurface / TM_ModifyLitColor hooks (VS location 9 /
    // gfx Draw misc3.x / the direct legs' push byte 244). The bit
    // layout belongs to gameplay code — the engine never
    // interprets it. A value change is a plain CPU write: no
    // shader recompile, no pipeline event, no MaterialValues
    // reupload, no batch split (the material stays the sort key).
    // Sets the base material's DEFAULT userdata (every draw of the
    // material without a live instance override). False on a bad
    // index.
    bool SetMaterialUserData(uint32_t materialIndex, uint32_t userData);
    // The material's default (0 = never set / bad index).
    [[nodiscard]] uint32_t MaterialUserData(uint32_t materialIndex) const;
    // Creates a material instance over a base material (the shared
    // base row stays untouched). Empty handle on a bad base index.
    [[nodiscard]] smvk::MaterialInstanceHandle CreateMaterialInstance(uint32_t baseMaterialIndex);
    // Destroys the instance (its overrides die with it; meshes
    // bound to it silently fall back to the base default — the
    // drawItems linkage stays valid, ResolvedItemUserData skips
    // dead slots). NEVER touches the base material.
    bool DestroyMaterialInstance(smvk::MaterialInstanceHandle handle);
    [[nodiscard]] bool IsMaterialInstanceAlive(smvk::MaterialInstanceHandle handle) const;
    // Binds a material instance to a live mesh ENTITY (the
    // per-entity override join — static scene nodes take instances
    // through spawn/editor flows only in v1). Binding marks the
    // world for the next resolve's linkage rebuild; the instance
    // VALUE stays live per frame. False on stale handles.
    bool SetMeshMaterialInstance(smvk::MeshEntityHandle mesh,
                                 smvk::MaterialInstanceHandle instance);
    // The instance's value override (live: visible on the next
    // frame build, no rebuild). False on a stale handle.
    bool SetMaterialInstanceUserData(smvk::MaterialInstanceHandle instance, uint32_t userData);
    // Clears the override — the instance goes back to inheriting
    // the base material's default. False on a stale handle.
    bool ClearMaterialInstanceUserDataOverride(smvk::MaterialInstanceHandle instance);
    // The per-frame RESOLUTION: a draw item's effective userdata —
    // the bound instance's override when live and overriding, else
    // the base material default. This is what the draw producers
    // call per item at frame build (GfxRenderer's filtered walk,
    // the direct legs' per-draw push).
    [[nodiscard]] uint32_t ResolvedItemUserData(const RenderItem& item) const;
    // ---- the SHADER FAMILY lane (Phase B, design §5) -----------
    // Sets the base material's lit shader family (the compiled
    // ProjectLit hook family vs the engine's StandardLit no-ops).
    // Every draw of the material (and every material INSTANCE —
    // families inherit, never override) binds the family's FS
    // twins from the next frame build. False on a bad index.
    bool SetMaterialShaderFamily(uint32_t materialIndex, smvk::MaterialShaderFamily family);
    // The material's family (StandardLit when never set / bad
    // index — the fail-safe stock pick).
    [[nodiscard]] smvk::MaterialShaderFamily ShaderFamilyOf(uint32_t materialIndex) const;

    // ---- the ANIMATOR component (Engine/App/animator.h —
    // per-entity playback state, the explicit Option-B surface) ----
    // Attaches an animator to a live MESH entity. The skeleton
    // binding is the entity mesh's OWNING cluster (must carry one —
    // the v1 single-skeleton-per-cluster contract; a static-mesh
    // entity refuses with an empty facade). The returned facade
    // forwards to World-owned state; clips pass BY VALUE (the
    // animator owns its copy — shared clip caches are the asset
    // manager's business).
    [[nodiscard]] Animator AddAnimator(smvk::MeshEntityHandle mesh, asset::Clip initialClip);
    // Destroys the animator (independent ownership — gameplay code
    // that holds animators NOT attached through DestroyMesh's
    // cleanup destroys them explicitly; see the Kill() pattern).
    bool DestroyAnimator(smvk::AnimatorHandle handle);
    [[nodiscard]] bool IsAnimatorAlive(smvk::AnimatorHandle handle) const;
    [[nodiscard]] AnimatorState* AnimatorStateOf(smvk::AnimatorHandle handle);
    [[nodiscard]] const AnimatorState* AnimatorStateOf(smvk::AnimatorHandle handle) const;

    // Advances every live animator + flushes the FIRST-ALIVE
    // animator's evaluated pose into its target cluster's scene
    // (WorldPage::poseExternallyDriven — the existing skinning path
    // consumes the pose; the renderer's own clip advance skips
    // driven pages). PUBLIC because the renderer's frame build
    // drives it with the frame's dt (bench-deterministic); custom
    // hosts that drive it themselves own the call instead.
    void Update(float dt);

    // Erases the cluster and REBUILDS the world: after the erase the
    // merged scene + every location table still point into the
    // destroyed cluster, so a full ResolveAndFixup_pages() runs
    // over the survivors — the unloaded cluster's entities vanish.
    // Refuses the dynamic cluster (runtime state, never pak content).
    // Load-time operation, not a hot path.
    //
    // SHARED-ASSET PARKING: a cluster with live external references
    // (WorldPage::assetRefs — an asset holder retained its resource
    // rows) is NOT destroyed: it detaches from the merged fold
    // exactly like an unload (nodes stop rendering, location tables
    // rebuild) but moves to retainedPages until the last
    // ReleasePage. Scene removal never destroys an independently
    // referenced asset — and never resurrects scene content.
    void UnloadCluster(smvk::WorldPage&);

    // ---- Shared-asset retention (WorldPage::assetRefs) ---------------
    // Bumps the external ref count of a REGISTERED or RETAINED
    // cluster (false when this world does not hold it; the dynamic
    // cluster is refused — runtime state is never an asset carrier).
    // Main-thread only.
    bool RetainPage(WorldPage&);
    // Drops one external ref. When the count reaches zero on a
    // RETAINED (parked) cluster the cluster is DESTROYED there (its
    // mesh range frees ride the same deferred-retirement contract
    // as UnloadCluster; no resolve is needed — a parked cluster is
    // outside the fold). A still-REGISTERED cluster is left to its
    // scene owner (UnloadCluster re-checks the count when its scene
    // goes). False when the cluster is unknown to this world.
    bool ReleasePage(WorldPage&);

    void RegisterPage(std::unique_ptr<WorldPage> cluster);
    // Detaches (does not destroy) + rebuilds like UnloadCluster; the
    // returned cluster is re-parentable into another World (also
    // pulls a RETAINED/parked cluster out — the re-parenting use
    // case). Refuses the dynamic cluster.
    std::unique_ptr<WorldPage> UnregisterPage(WorldPage&);
    void ResolveAndFixup_pages();
    // INCREMENTAL fold (the streaming path): append ONE freshly registered
    // page into the merged scene + location tables WITHOUT re-folding the
    // already-resident pages. The O(total) derived caches (flags /
    // drawItems / LOD chains / static BVH) are NOT rebuilt here — call
    // RebuildDerivedCaches once after a settled batch of MergePage calls.
    // The page must be registered and must not already be folded.
    // `clusterOrdinal` is the page's dense slot index in `pages` (only
    // the pageMergedNodes parallel-index discriminator); the material
    // dedup uses the STABLE WorldPage::pageOrdinal. `isIdentityPrimary`
    // is true only for the single-content-page bulk-copy fast path.
    void MergePage(smvk::WorldPage& page, uint32_t clusterOrdinal, bool isIdentityPrimary);
    // Retire ONE page's folded nodes + resource rows (mark its slots dead,
    // drop its location-table entries) WITHOUT compacting the survivors.
    // The derived caches are rebuilt by the next RebuildDerivedCaches.
    void UnmergePage(smvk::WorldPage& page);
    // Rebuilds the O(total) derived caches (iterator flags, material-sorted
    // draw list, LOD chains, lightmap charts, static-cull BVH) after a batch
    // of MergePage/UnmergePage deltas. This is the tail of a full resolve.
    void RebuildDerivedCaches();
    // App/scene code marks a mutated cluster; the renderer consumes this
    // at the next frame boundary, before recording any scene readers.
    // The cluster must be registered in this World (including dynamic).
    bool MarkPageDirty(const smvk::WorldPage& cluster);
    bool HasDirtyPages() const {
        return pagesDirty_;
    }
    bool ResolveDirtyPages();

    smvk::StaticMesh* GetMesh(uint32_t meshIndex);
    const smvk::StaticMesh* GetMesh(uint32_t meshIndex) const;
    smvk::Material* GetMaterial(uint32_t materialIndex);
    const smvk::Material* GetMaterial(uint32_t materialIndex) const;
    // LOD marker for tooling (LOD0 + cooked "/lodN" siblings with
    // world-scaled simplifier error). Null when the index is out of range.
    const smvk::LodInfo* GetLodInfo(uint32_t meshIndex) const;

    // Runtime LOD chain over cooked "/lodN" siblings (see mesh_optimizer.h).
    // The scene always references LOD0; siblings share the base URI and
    // are resolved here per instance. Length is >= 1 (LOD0 alone when
    // nothing was cooked). ResolveLodMesh() misses (nullopt) past the end.
    // Chains are resolved once in ResolveAndFixup_pages() into
    // lodChains (hot paths below are O(1): no URI strings, no cluster
    // scans — the previous per-draw probing showed up as a major CPU
    // regression once every Bistro mesh had a cooked chain).
    //
    // One LEVEL = one geometry sibling; the shading program is
    // resolved per (level, material) off the same level index
    // (FarShadingAt) — there is no separate shader LOD level. The
    // farthest level of a cooked chain (length > 1) shades on the
    // lit program with the material's quality specialization (see
    // ShaderLodPolicy); vertex-lit substitution is killed, and level
    // 0 and uncooked chains stay lit.
    struct LodChain {
        // Resolved levels, LOD0..N (levels[0].mesh == meshLocations[i]).
        struct Level {
            MeshLocation mesh; // geometry sibling for this level
            float error;       // world-space simplifier error (0 for LOD0)
            // Geometry streaming (geometry_streamer.h): false = the
            // sibling is KNOWN (smvk::LodInfo registered - the metadata
            // chain includes it) but its payload is NOT resident
            // (no pool mesh). mesh is invalid for such levels; the
            // picker never selects them (World::SelectLodIndex
            // compacts the resident subset), the demand pass reports
            // them, GetLodMesh would fall back to LOD0 if anything
            // ever asked (defense in depth - nothing should).
            bool resident = true;
            // Metadata bounds survive deferred residency. Required for a
            // conservative sphere across the FULL cooked LOD chain.
            glm::vec3 boundsMin{0.0f};
            glm::vec3 boundsMax{0.0f};
            bool boundsKnown = false;
        };
        std::vector<Level> levels;
        // True when any level is deferred: gates the desired-LOD
        // demand computation (chains without deferred siblings skip
        // it - their hot path is unchanged).
        bool anyDeferred = false;
    };
    uint32_t LodChainLength(uint32_t meshIndex) const;
    std::optional<MeshLocation> ResolveLodMesh(uint32_t meshIndex, uint32_t lodIndex) const;
    smvk::StaticMesh* GetLodMesh(uint32_t meshIndex, uint32_t lodIndex);
    void RebuildLodChains();
    // Simplifier error for a chain entry (0 for LOD0, world units).
    // Falls back to 0 when the marker is missing.
    float LodErrorAt(uint32_t meshIndex, uint32_t lodIndex) const;
    // Resolved far-level shading for a (mesh level, material) pair
    // (shader LODs; see ShaderLodPolicy): the farthest level of a
    // cooked chain (length > 1) stays on the lit program with the
    // material's quality specialization. The cheap vertex-lit
    // whole-program substitution is KILLED (vertexLit is always
    // false; the flag is retained on the wire + struct for future
    // use). Every other level, every uncooked chain, out-of-range
    // probes and invalid materials resolve default (full-lit, no spec).
    struct FarShading {
        bool vertexLit = false;     // killed: never set (retained for ABI)
        ShaderLodQuality quality{}; // quality spec when staying on lit
        // Default full-lit shading: neither substitution nor
        // specialization (the level draws exactly like LOD0).
        bool isDefault() const {
            return !vertexLit && quality == ShaderLodQuality{};
        }
    };
    FarShading FarShadingAt(uint32_t meshIndex, uint32_t lodIndex, uint32_t materialIndex) const;
    // Error-driven selection: coarsest LOD whose projected screen error
    // (LodProjectedPixelError over each entry's simplifyError scaled by
    // the INSTANCE's world scale) stays within maxPixelError at
    // `distance`. The serialized simplifyError is MESH-LOCAL (scale-free
    // by design - never bake a scene transform into the artifact: the
    // removed cook-time world-scaling pass compounded per re-cook), so
    // the pick multiplies by the node's own world scale here -
    // per-instance exact, including multi-instance meshes. Returns 0
    // (full mesh) when no chain was cooked or nothing coarser is in
    // budget. currentLod anchors the hysteresis band
    // (kLodSelectHysteresis): levels within the band of the current one
    // are sticky, so switch distances don't flip siblings frame to
    // frame.
    uint32_t SelectLodIndex(uint32_t meshIndex,
                            float distance,
                            float viewportHeightPx,
                            float fovYRadians,
                            float maxPixelError,
                            uint32_t currentLod = 0,
                            float instanceWorldScale = 1.0f) const;

    // ---- Geometry LOD streaming (Renderer/geometry_streamer.h) ------
    // Consume the accumulated per-frame DEMAND: (world mesh, level)
    // pairs the camera wanted but that are not resident (the full-
    // chain selection vs the resident selection). Sharpest (lowest)
    // level per mesh wins; the map clears on consume. Empty when no
    // chain has deferred siblings.
    void ConsumeLodDemand(std::vector<std::pair<uint32_t, uint32_t>>& out);
    // Frame-scratch overload, avoiding a temporary heap vector in the
    // geometry-streaming tick.
    template <typename Alloc>
    void ConsumeLodDemand(std::vector<std::pair<uint32_t, uint32_t>, Alloc>& out) {
        out.clear();
        out.reserve(lodDemand_.size());
        for (const auto& [mesh, level] : lodDemand_)
            out.emplace_back(mesh, level);
        lodDemand_.clear();
    }
    // Drawn-level touches (the eviction idle clock's other half):
    // while touch tracking is on, LodIndexForNode records every
    // SERVED (mesh, level) - the streamer's promote registry decays
    // entries nothing has drawn or demanded within
    // idleFramesBeforeEvict. Drain between frames like the demand.
    void SetLodTouchTracking(bool on) {
        lodTouchTracking_ = on;
    }
    void ConsumeLodDrawn(std::vector<std::pair<uint32_t, uint32_t>>& out);
    template <typename Alloc>
    void ConsumeLodDrawn(std::vector<std::pair<uint32_t, uint32_t>, Alloc>& out) {
        out.clear();
        out.reserve(lodDrawnSet_.size());
        for (const uint64_t k : lodDrawnSet_)
            out.emplace_back(uint32_t(k >> 32), uint32_t(k & 0xFFFFFFFFu));
        lodDrawnSet_.clear();
    }
    // GPU-scene LOD demand feedback (the work selector's readback): record
    // the GPU-selected drawn level as a touch and, when a finer non-resident
    // level is desired, that level as a demand. Mirrors the CPU LodIndexForNode
    // bookkeeping (the GPU selector draws only resident levels, so this is its
    // demand source). No-op when touch tracking is unarmed.
    void RecordGpuLodFeedback(uint32_t worldMesh, uint32_t drawnLod, uint32_t desiredLod);

    // Promote one deferred sibling to resident: locate its deferred
    // record on the owning cluster and replay the upload +
    // registration (WorldPage::PromoteDeferredMeshLod). Returns the
    // fresh handle + payload bytes for the streamer's accounting, or
    // nullopt when the level is not deferred/already promoted. The
    // caller rebuilds the chains + the gfx tables afterwards.
    [[nodiscard]] std::optional<WorldPage::PromotedLod> PromoteLodLevel(uint32_t meshIndex,
                                                                        uint32_t lodIndex);
    // Evict a PROMOTED sibling back to deferred (the budget
    // enforcement path): WorldPage::EvictMeshLod destroys the pool
    // mesh (heap ranges retire deferred), the record returns to the
    // deferred state. Returns the freed payload bytes, 0 when the
    // level is not currently promoted (or is the never-evictable
    // LOD0 base).
    [[nodiscard]] uint64_t EvictLodLevel(uint32_t meshIndex, uint32_t lodIndex);
    // Any cluster still holding deferred siblings (the streamer's
    // census).
    [[nodiscard]] bool HasDeferredLods() const;
    // Any cluster holding LOD streaming records at all - deferred
    // OR promoted (the tick's active gate: with eviction on, the
    // tick keeps running for touch tracking + budget enforcement
    // even after everything promoted).
    [[nodiscard]] bool HasLodRecords() const;

    // Cached LOD chains parallel to meshLocations (see RebuildLodChains).
    // Mutable so const hot paths (LodChainLength/SelectLodIndex) can
    // lazily rebuild after direct cluster edits that bypassed
    // ResolveAndFixup_pages().
    mutable std::vector<LodChain> lodChains;

    // ---- Lightmap chart transform table -----------------------
    // Scene-wide concatenation of every StaticMesh::lmChartXforms,
    // sorted by world mesh index, then LOD level, then chart index
    // (one float4 per chart: xy = scale, zw = offset, so atlasUv =
    // chartUv * xy + zw). The runtime uploads this once per scene
    // into a GPU buffer; each draw's chart offset (see
    // gfx::Draw::lmChartOffset, GfxDraw.misc2.x) is its mesh's base
    // into the table, and the vertex shader fetches
    // xforms[base + chartId]. Rebuilt by RebuildLightmapChartTable()
    // at the end of ResolveAndFixup_pages(); lmChartVersion bumps
    // on every rebuild so renderers re-upload only on change.
    std::vector<glm::vec4> lmChartTable;
    // Process-unique rebuild revision, not a counter local to this World:
    // the GPU chart-transform cache survives scene switches.
    uint64_t lmChartVersion = 0;
    struct LmChartKey {
        const WorldPage* cluster = nullptr;
        uint32_t poolIndex = 0;
        bool operator==(const LmChartKey&) const = default;
    };
    struct LmChartKeyHash {
        size_t operator()(const LmChartKey& k) const noexcept {
            return std::hash<const void*>{}(k.cluster) * 31u + k.poolIndex;
        }
    };
    std::unordered_map<LmChartKey, uint32_t, LmChartKeyHash> lmChartBase_;
    void RebuildLightmapChartTable();
    // Base offset for a mesh location (0 when the mesh carries no
    // lightmap — the shader gates on the draw's lobe count anyway).
    uint32_t ChartOffsetFor(MeshLocation loc) const;
    // Convenience over meshLocations[i] / lodChains (LOD sibling).
    uint32_t ChartOffsetFor(uint32_t meshIndex, uint32_t lodIndex) const;

    // ---- Per-node LOD decision cache -------------------------------
    // Decisions are cached per node and refreshed EVERY FRAME (full
    // rate, the default) or on a parity split (halfrate opt-out via
    // SetLodFullRate(false): on tick T only nodes with (node & 1) ==
    // (tick & 1) recompute; every other node serves its last decision,
    // 1 frame stale at worst). All passes inside one frame observe the
    // SAME value for a node (the first computation of a refresh frame
    // wins and is stamped), so filter/direct splits stay consistent
    // even when queried with different eyes (debug cull override).
    // Selection is hysteretic around the pixel budget (see
    // kLodSelectHysteresis): the node keeps its level while the
    // projected error stays inside the band, anchored on the cached
    // current level.
    uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh, const LodFrameInputs& inputs) const;
    // View distance used by LodIndexForNode: closest approach from the
    // eye to the instance's LOD0 world sphere (0 when inside/touching).
    // Node origins can sit meters from the visible surface (Sponza
    // cooks the whole building onto ONE node), so an origin distance
    // would select coarser LODs for nearby surfaces. Conservative:
    // siblings only shrink the surface. Falls back to the origin
    // distance when bounds are unavailable.
    float LodDistanceForNode(NodeId node, uint32_t worldMesh, const glm::vec3& eye) const;
    // Drops every cached decision (parameter tweak / scene edit).
    void InvalidateLodCache();
    // Full-rate LOD refresh (default): every node recomputes every
    // frame. false = the halfrate parity split above (perf A/B).
    // Config, not cache: InvalidateLodCache never touches it.
    void SetLodFullRate(bool on) {
        lodFullRate = on;
    }
    bool LodFullRate() const {
        return lodFullRate;
    }
    bool lodFullRate = true;
    // ---- Per-frame LOD decision stats (the always-0 bug fix) ------
    // The renderer-side counters (LodMeshForNode) only fired on the
    // DIRECT draw legs; the filtered legs (Main via the gfx filter,
    // the filtered shadow registration) decide through LodIndexForNode
    // directly, so the panel/HUD read 0/0/0 with the filter on (the
    // default). Counting HERE - once per (node, frame) at the FIRST
    // query, whichever leg asks first - counts EVERY leg's decision
    // exactly once: a chain instance that draws always queries at
    // least once, all legs share the value from then on.
    // draws = chain instances decided this frame; switched = decided
    // onto a non-LOD0 level (halfrate parity-skips still count the
    // stale level they actually serve). Reset at frame rollover.
    [[nodiscard]] uint32_t LodStatDraws() const {
        return lodStatDraws_;
    }
    [[nodiscard]] uint32_t LodStatSwitched() const {
        return lodStatSwitched_;
    }
    // Cache storage (lazily sized to the node count; reset by
    // ResolveAndFixup_pages).
    mutable std::vector<uint8_t> nodeLod_;
    mutable std::vector<uint64_t> nodeLodStamp_;      // frameId of computation
    mutable std::vector<uint64_t> nodeLodCountFrame_; // frameId of stat count
    mutable uint32_t lodStatDraws_ = 0;
    mutable uint32_t lodStatSwitched_ = 0;
    mutable uint64_t lodTick_ = 0; // parity source
    mutable uint64_t lodLastFrameId_ = ~uint64_t(0);
    mutable LodFrameInputs lodInputs_{};
    // Geometry streaming: accumulated (world mesh -> sharpest
    // non-resident level) demand since the last ConsumeLodDemand -
    // filled by LodIndexForNode's full-chain selection, drained by
    // the streamer tick between frames.
    mutable std::unordered_map<uint32_t, uint32_t> lodDemand_;
    // Geometry streaming: every SERVED (mesh, level) since the last
    // ConsumeLodDrawn (packed mesh<<32|level) - the eviction idle
    // clock. Only accumulated while lodTouchTracking_ (the streamer
    // arms it when streaming is live).
    mutable std::unordered_set<uint64_t> lodDrawnSet_;
    mutable bool lodTouchTracking_ = false;

    // First-hit URI resolution across pages in registration order
    // (mirrors the VFS overlay rule). Used for cross-pak references.
    std::optional<MeshLocation> ResolveMeshAcrossClusters(smvk::StringViewT uri) const;
    std::optional<MaterialLocation> ResolveMaterialAcrossClusters(smvk::StringViewT uri) const;
    // Bake-payload channel (see WorldPage::bakePayloads): the bytes at a
    // GI/cubemap VFS path ("gi/<stem>.sggi", "textures/cubemaps/...").
    // DYNAMIC cluster first — runtime injections (editor quick-bake)
    // override cooked pak payloads — then pak pages in registration
    // order. Empty span = nothing carries it.
    [[nodiscard]] std::span<const uint8_t> ReadBakeBytes(const std::string& vfsPath) const;
    // ---- Env zones (see ZoneAtPosition in Engine/Scene/scene.h) --------
    // The zone covering a world-space point / a node's world ORIGIN:
    // priority descending, then innermost (smallest bounded volume),
    // INFINITE scale axes = unbounded slabs. UINT32_MAX = no zone — the
    // resolved default environment stands. The zone's look block:
    // scene.envForNode[zone].
    [[nodiscard]] NodeId zoneAtPosition(const glm::vec3& p) const {
        return ZoneAtPosition(scene, p);
    }
    [[nodiscard]] NodeId zoneForNode(NodeId node) const {
        if (node >= scene.globalTransform.size())
            return UINT32_MAX;
        return ZoneAtPosition(scene, glm::vec3(scene.globalTransform[node][3]));
    }

    // ---- Static BVH for CPU frustum culling ---------------------
    // Built once from frozen static-subtree nodes (see TransformHierarchy::
    // staticNode) at the end of ResolveAndFixup_pages(); static
    // globals never move per frame so the tree stays valid until the
    // next resolve. Bounds are the LOD-chain union per instance so any
    // LodIndexForNode selection stays inside the cached AABB.
    // Dynamic nodes are NOT in this tree: callers brute-force a sphere
    // test per dynamic item (few, moving) while static items do one
    // O(1) mask lookup after a single ::tour::frustumCullBVH walk.
    mutable ::tour::BvhTree staticBvh_;
    mutable bool staticBvhValid_ = false;
    // Set when the single-page COOKED tree was adopted (AdoptCookedStaticBvh)
    // instead of an incrementally built per-page forest. The cooked tree is
    // terminal for a single-page world: the first MergePage that makes the
    // world multi-page must discard it and rebuild every page's subtree.
    mutable bool staticBvhAdopted_ = false;
    // INCREMENTAL STATIC-CULL BVH (the streaming path): pages never share
    // hierarchy, so each page's static meshes fold into an independent
    // subtree. staticBvhPrims_ is the append-only primitive list (indexed
    // by primitive id == the tree's primIndices slot); staticBvhPageRoots_
    // is parallel to `pages` (dense ordinal -> subtree root node index,
    // UINT32_MAX = not yet built). Page add/remove appends/orphans a
    // subtree instead of rebuilding the whole tree.
    mutable std::vector<::tour::Partition> staticBvhPrims_;
    mutable std::vector<uint32_t> staticBvhPageRoots_;
    // Builds (appends) one page's static-cull subtree and returns its root
    // node index. Requires the LOD chains + global transforms to be fresh.
    uint32_t BuildPageStaticBvhSubtree(WorldPage& page);
    // Builds ONE page's material-sorted draw chunk (its RenderItem rows),
    // using the freshly computed per-node `flags` + the entity instance
    // linkage. Cooked pages cache the result; the dynamic page is rebuilt
    // every batch.
    std::vector<RenderItem> BuildPageDrawChunk(
        WorldPage& page,
        uint32_t ordinal,
        const std::unordered_map<NodeId, uint64_t>& entityInstanceByNode);
    // Pipeline-class bin (bit 0 = double-sided, bit 1 = masked, bit 2 =
    // ProjectLit) for one item — mirrors tour::gpu_scene::RasterPipelineClass.
    uint8_t PsoClassFor(const RenderItem& item) const;
    // Recomputes psoBinOffset_ from the current drawItems (which must be
    // sorted by (psoClass, material)). O(drawItems).
    void RebuildPsoBinOffsets();
    // (Re)builds staticBvh_ from the current scene. Cheap no-op when
    // there are no static mesh nodes (leaves the tree empty).
    void RebuildStaticBvh();
    void InvalidateStaticBvh();
    // The resolve-time entry (ResolveAndFixup_pages calls this in
    // place of RebuildStaticBvh): tries the COOKED tree first — a pak
    // whose scene blob carried the static-cull section (WorldPage::
    // cookedStaticBvh) installs it instead of rebuilding, guarded by the
    // adoption hash (tour::StaticCullStateHash over the loaded scene —
    // any post-load edit mismatches and rebuilds). Single-contributing-
    // cluster worlds only; the seed is CONSUMED on any attempt so later
    // resolves always take the ordinary rebuild.
    void RefreshStaticBvh();
    [[nodiscard]] bool AdoptCookedStaticBvh();
    // Lazily rebuilds when invalid; returns the cached static tree.
    const ::tour::BvhTree& EnsureStaticBvh() const;
    // World-space AABB over every mesh instance (union of the
    // per-instance mesh AABBs under their global transforms). Used to
    // fit procedural content (e.g. the Forward+ demo light grid) to the
    // loaded scene. Empty (infinities) when the world holds no geometry.
    ::tour::Aabb ComputeSceneBounds() const;
    [[nodiscard]] bool HasStaticBvh() const {
        return staticBvhValid_ && !staticBvh_.nodes.empty() && !staticBvh_.primIndices.empty();
    }
    // Legacy-static fallback toggle (the app load path owns it): a
    // PRE-SECTION pak (WorldPage::hasBvh() == false — no cooked static-cull
    // section) is a deployment error because the static marking that gates
    // GPU-scene admission never exists, so the whole GPU-driven culling/
    // seed-history chain silently degrades to the legacy path. Default
    // FALSE = FAIL ON OPEN (the load path aborts with a recook message).
    // Set TRUE (TOURMALINE_LEGACY_STATIC_FALLBACK=1) to opt into the
    // viewer-side freeze + on-load BVH rebuild for genuinely legacy paks.
    bool legacyStaticFallback = false;
    // Frustum-culls the static tree against caller planes (e.g. the
    // main RenderView's 6 guarded planes) and returns a per-node mask
    // sized to scene.hierarchy.size(): 1 = static node visible OR any
    // non-static node (pass-through; caller sphere-tests dynamic),
    // 0 = static node culled. Empty vector = nothing cached / no
    // culling (caller must draw everything).
    std::vector<uint8_t> StaticVisibilityMask(const glm::vec4* planes, int planeCount) const;

    template <typename Func> void forEachNode(IteratorFlags requiredFlags, Func&& func) {
        for (uint32_t i = 0; i < scene.hierarchy.size(); ++i) {
            // Epoch 6b: retired slots are holes — the flags vector
            // may still carry a pre-retirement entry for them.
            if (!IsLiveSlot(scene, i))
                continue;
            const IteratorFlags nf =
                (i < flags.size()) ? flags[i] : static_cast<IteratorFlags>(IteratorFlagBit_None);

            if ((nf & requiredFlags) != requiredFlags)
                continue;

            func(static_cast<NodeId>(i));
        }
    }

    // Keep this LAST: C++ destroys fields in reverse order. The
    // AssetManager releases its held assets while World::scene,
    // cluster pools, and location tables are still alive.
    std::shared_ptr<void> gameAssetOwner_;
};

} // namespace tour
// COMPAT (the namespace/RHI boundary audit, todo item 8.4): the World is
// ENGINE-OWNED and lives in tour; the smvk aliases keep the existing
// call sites compiling through the consumer migration.
namespace smvk {
using ::tour::IteratorFlagBit_CastShadow;
using ::tour::IteratorFlagBit_CastVolumetricShadow;
using ::tour::IteratorFlagBit_LightDirectional;
using ::tour::IteratorFlagBit_LightPoint;
using ::tour::IteratorFlagBit_LightSpot;
using ::tour::IteratorFlagBit_Masked;
using ::tour::IteratorFlagBit_None;
using ::tour::IteratorFlagBit_Opaque;
using ::tour::IteratorFlagBit_PassedCullTest;
using ::tour::IteratorFlagBit_Renderable;
using ::tour::IteratorFlagBit_Static;
using ::tour::IteratorFlagBit_StaticVertexFmt;
using ::tour::IteratorFlagBit_Transparent;
using ::tour::IteratorFlagBits;
using ::tour::IteratorFlags;
using ::tour::LodFrameInputs;
using ::tour::TerrainSource;
using ::tour::World;
} // namespace smvk
