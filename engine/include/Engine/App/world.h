// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/mesh_optimizer.h>
#include <Engine/Rendergraph/terrain_hfield.h>
#include <Engine/Utils/bvh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

	class PakReader;
	class TextureStreamer;

	// Tiled-terrain source payload (CPU side): harvested from a pak's
	// Terrain blobs by LoadTerrainFromPak. The renderer bakes G-blocks
	// from hfield (+ mask R into vertex color) at terrain build; masks
	// are NOT kept resident past the bake in v1 (GPU mask textures land
	// with the atlas in phase 4). Empty (hasTerrain=false) = no terrain.
	struct TerrainSource {
		terrain::TerrainHField hfield{};
		// 256 S-tiles of 256x256 RGBA8, row-major (tz*16+tx) test masks.
		// Present only between harvest and the terrain bake.
		std::vector<std::vector<uint8_t>> masks{};
		bool hasTerrain = false;
	};

	// Per-frame inputs for error-driven LOD selection (see World::
	// LodIndexForNode). frameId is renderer-supplied and MONOTONIC (the
	// RHI ring index is NOT — it wraps); it keys the per-node decision
	// stamps (and the halfrate parity tick when full rate is off).
	struct LodFrameInputs {
		uint64_t frameId = 0;
		glm::vec3 eye{ 0.0f };
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
	// geometry and — when the material allows it — downgrades shading
	// to the cheap vertex-lit program: lighting is computed per vertex
	// (directional sun + ambient, no shadow-map sampling, no forward+
	// lights, no specular, no textures), so far instances trade shading
	// quality for fragment cost. Every other level (and every uncooked
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
		// Set when the node sits inside a static subtree (see Scene::
		// staticRoot/staticNode). Static nodes skip per-frame transform
		// updates and dynamic BVH rebuilds; collapsed mega-mesh nodes keep
		// this flag as well.
		IteratorFlagBit_Static = 1u << 9,
		// Cutout material (ShadingModel::Masked): alpha-tested draw path.
		// Set alongside Opaque so the masked/solid phase splits in the
		// shadow, prepass and motion passes test one bit instead of
		// fetching the material per node per pass.
		IteratorFlagBit_Masked = 1u << 10,
		//IteratorFlagBit_Skinned,
	} IteratorFlagBits;
	typedef uint32_t IteratorFlags;

	// World is the single runtime scene graph. It is *not* an imported scene.
	// It owns (has-a) the merged Scene hierarchy, owns the Clusters that hold
	// the actual mesh/material/texture resources, and resolves per-node mesh/
	// material indices stored in the Scene to the owning Cluster + handle.
	struct World {
		explicit World() {};

		// Merged runtime scene graph. Imported Cluster scenes are folded into
		// this via ResolveAndFixup_clusters().
		Scene scene;

		mutable bool dirtyCull = true;
		std::vector<IteratorFlags> flags; // indexed by node
		std::vector<std::unique_ptr<Cluster>> clusters;

		// Tiled-terrain source (see TerrainSource): empty unless a loaded
		// pak carried Terrain blobs.
		TerrainSource terrainSource;

		// ---- Compact draw list (hot-path iterator) --------------------
		// Built once per ResolveAndFixup_clusters() and kept in sync by
		// Cull(). Every renderable node (mesh attached) appears exactly
		// once, sorted by material so per-material pipeline binds run
		// once per material per pass. Node id + world-level mesh/material
		// indices are precomputed here: iterating this list replaces the
		// per-node scene.meshForNode / materialForNode hash lookups and
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
		};
		std::vector<RenderItem> drawItems; // material-sorted; see above

		// SoA mirror of drawItems[].flags: a dense flags-only column the
		// SIMD scan in ItemIndicesFor walks 8 items per iteration.
		std::vector<IteratorFlags> itemFlags;

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
		template<typename Func>
		void forEachRenderItem(IteratorFlags requiredFlags, Func&& func) const
		{
			if (requiredFlags == static_cast<IteratorFlags>(IteratorFlagBit_None)) {
				for (const RenderItem& item : drawItems)
					func(item);
				return;
			}
			for (uint32_t idx : ItemIndicesFor(requiredFlags))
				func(drawItems[idx]);
		}

		// Resource lookup into clusters
		struct MeshLocation {
			Cluster* cluster;
			ClusterStaticMeshHandle handle;
		};
		struct MaterialLocation {
			Cluster* cluster;
			ClusterMaterialHandle handle;
		};
		struct TextureLocation {
			Cluster* cluster;
			ClusterTextureHandle handle;
		};
		// Cooked punctual lights (KHR_lights_punctual): pool assets like
		// meshes — scene.lightForNode stores world indices into this table,
		// each entry resolving back to the owning Cluster + handle.
		struct LightLocation {
			Cluster* cluster;
			ClusterLightHandle handle;
		};
		// Authored cameras: pool assets like lights — scene.cameraForNode
		// stores world indices into this table, each entry resolving back
		// to the owning Cluster + handle.
		struct CameraLocation {
			Cluster* cluster;
			ClusterCameraHandle handle;
		};
		// Authored/runtime decals: pool assets like lights — scene.decalForNode
		// stores world indices into this table, each entry resolving back to
		// the owning Cluster + handle. The shape rides the node transform
		// (see SceneDecal).
		struct DecalLocation {
			Cluster* cluster;
			ClusterDecalHandle handle;
		};
		// Indexed by the mesh/material index stored in scene.meshForNode /
		// scene.materialForNode. Each entry points back to the owning
		// Cluster so resources stay owned by clusters.
		std::vector<MeshLocation> meshLocations;
		std::vector<MaterialLocation> materialLocations;
		std::vector<TextureLocation> textureLocations;
		std::vector<LightLocation> lightLocations;
		std::vector<CameraLocation> cameraLocations;
		std::vector<DecalLocation> decalLocations;
		// Per-mesh LOD markers parallel to meshLocations (cook-time chain:
		// LOD0 + "/lodN" siblings with world-scaled simplifier error used
		// for runtime LOD selection — see SelectLodIndex).
		std::vector<LodInfo> meshLodInfos;

		Cluster& LoadCluster(ResourceManager& ctx);
		// Deserializes a cooked pak (PakReader over caller-owned bytes) into
		// a new registered Cluster. Throws std::runtime_error on failure.
		// The caller issues ctx.FlushUploads() after ResolveAndFixup.
		// streamer (optional): v4 textures carrying a digest stream through
		// CreateStreamingTextureFromMeta (tail-only upload, no KTX parse)
		// and register for demand upgrades — imagePin (the loaded pak
		// image) must then be given to pin the borrowed payloads.
		Cluster& LoadClusterFromPak(ResourceManager& ctx, const class PakReader& reader,
			uint32_t sceneIndex = 0,
			struct TextureStreamer* streamer = nullptr,
			std::shared_ptr<const std::vector<uint8_t>> imagePin = {});
		// Harvests the pak's Terrain blobs (terrain/meta + 16x H + 256x S)
		// into terrainSource (byte copies; the reader may die after). Also
		// called by LoadClusterFromPak (non-fatal: scene loads even when
		// terrain is broken). False with err set on malformed terrain.
		bool LoadTerrainFromPak(const class PakReader& reader, std::string& err);
		// The world's DYNAMIC cluster (see Cluster::isDynamic): the
		// dedicated home for runtime-created entities (editor-created
		// lights/cameras, app demo content, later decals/particles).
		// Lazily created on first call — repeat calls return the SAME
		// cluster. ctx is the host context (nullptr only in GPU-less
		// tests: value entities need no GPU). NOT resolved here: callers
		// either edit incrementally (node in the merged scene + location
		// push, the editor's live path) or keep their nodes in the
		// dynamic cluster's own scene so a later resolve folds them in.
		Cluster& EnsureDynamicCluster(ResourceManager* ctx);
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
		// Erases the cluster and REBUILDS the world: after the erase the
		// merged scene + every location table still point into the
		// destroyed cluster, so a full ResolveAndFixup_clusters() runs
		// over the survivors — the unloaded cluster's entities vanish.
		// Refuses the dynamic cluster (runtime state, never pak content).
		// Load-time operation, not a hot path.
		void UnloadCluster(smvk::Cluster&);

		void RegisterCluster(std::unique_ptr<Cluster> cluster);
		// Detaches (does not destroy) + rebuilds like UnloadCluster; the
		// returned cluster is re-parentable into another World. Refuses
		// the dynamic cluster.
		std::unique_ptr<Cluster> UnregisterCluster(Cluster&);
		void ResolveAndFixup_clusters();

		smvk::StaticMesh* GetMesh(uint32_t meshIndex);
		const smvk::StaticMesh* GetMesh(uint32_t meshIndex) const;
		smvk::Material* GetMaterial(uint32_t materialIndex);
		const smvk::Material* GetMaterial(uint32_t materialIndex) const;
		// LOD marker for tooling (LOD0 + cooked "/lodN" siblings with
		// world-scaled simplifier error). Null when the index is out of range.
		const LodInfo* GetLodInfo(uint32_t meshIndex) const;

		// Runtime LOD chain over cooked "/lodN" siblings (see mesh_optimizer.h).
		// The scene always references LOD0; siblings share the base URI and
		// are resolved here per instance. Length is >= 1 (LOD0 alone when
		// nothing was cooked). ResolveLodMesh() misses (nullopt) past the end.
		// Chains are resolved once in ResolveAndFixup_clusters() into
		// lodChains (hot paths below are O(1): no URI strings, no cluster
		// scans — the previous per-draw probing showed up as a major CPU
		// regression once every Bistro mesh had a cooked chain).
		//
		// One LEVEL = one geometry sibling; the shading program is
		// resolved per (level, material) off the same level index
		// (UseVertexLitShaderAt) — there is no separate shader LOD
		// level. The farthest level of a cooked chain (length > 1)
		// shades vertex-lit when the material allows it (see
		// ShaderLodPolicy); level 0 and uncooked chains stay lit.
		struct LodChain {
			// Resolved levels, LOD0..N (levels[0].mesh == meshLocations[i]).
			struct Level {
				MeshLocation mesh;  // geometry sibling for this level
				float error;        // world-space simplifier error (0 for LOD0)
			};
			std::vector<Level> levels;
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
		// cooked chain (length > 1) either substitutes the cheap
		// vertex-lit program (the material allows it) or stays on the
		// lit program with the material's quality specialization.
		// Every other level, every uncooked chain, out-of-range probes
		// and invalid materials resolve default (full-lit, no spec).
		struct FarShading {
			bool vertexLit = false;     // use the cheap vertex-lit program
			ShaderLodQuality quality{}; // quality spec when staying on lit
			// Default full-lit shading: neither substitution nor
			// specialization (the level draws exactly like LOD0).
			bool isDefault() const {
				return !vertexLit && quality == ShaderLodQuality{};
			}
		};
		FarShading FarShadingAt(uint32_t meshIndex, uint32_t lodIndex,
			uint32_t materialIndex) const;
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
		uint32_t SelectLodIndex(uint32_t meshIndex, float distance,
			float viewportHeightPx, float fovYRadians, float maxPixelError,
			uint32_t currentLod = 0, float instanceWorldScale = 1.0f) const;

		// Cached LOD chains parallel to meshLocations (see RebuildLodChains).
		// Mutable so const hot paths (LodChainLength/SelectLodIndex) can
		// lazily rebuild after direct cluster edits that bypassed
		// ResolveAndFixup_clusters().
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
		// at the end of ResolveAndFixup_clusters(); lmChartVersion bumps
		// on every rebuild so renderers re-upload only on change.
		std::vector<glm::vec4> lmChartTable;
		uint64_t lmChartVersion = 0;
		struct LmChartKey {
			const Cluster* cluster = nullptr;
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
		uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh,
			const LodFrameInputs& inputs) const;
		// View distance used by LodIndexForNode: closest approach from the
		// eye to the instance's LOD0 world sphere (0 when inside/touching).
		// Node origins can sit meters from the visible surface (Sponza
		// cooks the whole building onto ONE node), so an origin distance
		// would select coarser LODs for nearby surfaces. Conservative:
		// siblings only shrink the surface. Falls back to the origin
		// distance when bounds are unavailable.
		float LodDistanceForNode(NodeId node, uint32_t worldMesh,
			const glm::vec3& eye) const;
		// Drops every cached decision (parameter tweak / scene edit).
		void InvalidateLodCache();
		// Full-rate LOD refresh (default): every node recomputes every
		// frame. false = the halfrate parity split above (perf A/B).
		// Config, not cache: InvalidateLodCache never touches it.
		void SetLodFullRate(bool on) { lodFullRate = on; }
		bool LodFullRate() const { return lodFullRate; }
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
		[[nodiscard]] uint32_t LodStatDraws() const { return lodStatDraws_; }
		[[nodiscard]] uint32_t LodStatSwitched() const { return lodStatSwitched_; }
		// Cache storage (lazily sized to the node count; reset by
		// ResolveAndFixup_clusters).
		mutable std::vector<uint8_t> nodeLod_;
		mutable std::vector<uint64_t> nodeLodStamp_; // frameId of computation
		mutable std::vector<uint64_t> nodeLodCountFrame_; // frameId of stat count
		mutable uint32_t lodStatDraws_ = 0;
		mutable uint32_t lodStatSwitched_ = 0;
		mutable uint64_t lodTick_ = 0;               // parity source
		mutable uint64_t lodLastFrameId_ = ~uint64_t(0);
		mutable LodFrameInputs lodInputs_{};

		// First-hit URI resolution across clusters in registration order
		// (mirrors the VFS overlay rule). Used for cross-pak references.
		std::optional<MeshLocation> ResolveMeshAcrossClusters(StringViewT uri) const;
		std::optional<MaterialLocation> ResolveMaterialAcrossClusters(StringViewT uri) const;
		// Bake-payload channel (see Cluster::bakePayloads): the bytes at a
		// GI/cubemap VFS path ("gi/<stem>.sggi", "textures/cubemaps/...").
		// DYNAMIC cluster first — runtime injections (editor quick-bake)
		// override cooked pak payloads — then pak clusters in registration
		// order. Empty span = nothing carries it.
		[[nodiscard]] std::span<const uint8_t> ReadBakeBytes(
			const std::string& vfsPath) const;
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
			return ZoneAtPosition(scene,
				glm::vec3(scene.globalTransform[node][3]));
		}

		// ---- Static BVH for CPU frustum culling ---------------------
		// Built once from frozen static-subtree nodes (see Scene::
		// staticNode) at the end of ResolveAndFixup_clusters(); static
		// globals never move per frame so the tree stays valid until the
		// next resolve. Bounds are the LOD-chain union per instance so any
		// LodIndexForNode selection stays inside the cached AABB.
		// Dynamic nodes are NOT in this tree: callers brute-force a sphere
		// test per dynamic item (few, moving) while static items do one
		// O(1) mask lookup after a single frustumCullBVH walk.
		mutable ::BvhTree staticBvh_;
		mutable bool staticBvhValid_ = false;
		// (Re)builds staticBvh_ from the current scene. Cheap no-op when
		// there are no static mesh nodes (leaves the tree empty).
		void RebuildStaticBvh();
		void InvalidateStaticBvh();
		// Lazily rebuilds when invalid; returns the cached static tree.
		const ::BvhTree& EnsureStaticBvh() const;
		// World-space AABB over every mesh instance (union of the
		// per-instance mesh AABBs under their global transforms). Used to
		// fit procedural content (e.g. the Forward+ demo light grid) to the
		// loaded scene. Empty (infinities) when the world holds no geometry.
		::Aabb ComputeSceneBounds() const;
		[[nodiscard]] bool HasStaticBvh() const {
			return staticBvhValid_ && !staticBvh_.nodes.empty()
				&& !staticBvh_.primIndices.empty();
		}
		// Frustum-culls the static tree against caller planes (e.g. the
		// main RenderView's 6 guarded planes) and returns a per-node mask
		// sized to scene.hierarchy.size(): 1 = static node visible OR any
		// non-static node (pass-through; caller sphere-tests dynamic),
		// 0 = static node culled. Empty vector = nothing cached / no
		// culling (caller must draw everything).
		std::vector<uint8_t> StaticVisibilityMask(
			const glm::vec4* planes, int planeCount) const;

		template<typename Func>
		void forEachNode(IteratorFlags requiredFlags, Func&& func)
		{
			for (uint32_t i = 0; i < scene.hierarchy.size(); ++i) {
				const IteratorFlags nf =
					(i < flags.size()) ? flags[i]
					: static_cast<IteratorFlags>(IteratorFlagBit_None);

				if ((nf & requiredFlags) != requiredFlags)
					continue;

				func(static_cast<NodeId>(i));
			}
		}

	};

};
