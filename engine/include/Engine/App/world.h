#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Asset/mesh_optimizer.h>
//#include <Engine/Utils/Bvh.h>
#include <Engine/Scene/scene.h>

namespace smvk {

	class PakReader;

	enum struct CullType {
		CPU
		//, GPU
	};

	struct CullConfiguration {
		CullType cullType = CullType::CPU;
		glm::mat4 viewProj{};
	};

	// Per-frame inputs for error-driven LOD selection (see World::
	// LodIndexForNode). frameId is renderer-supplied and MONOTONIC (the
	// RHI ring index is NOT — it wraps); it keys the halfrate parity
	// split and the per-node decision stamps.
	struct LodFrameInputs {
		uint64_t frameId = 0;
		glm::vec3 eye{ 0.0f };
		float viewportHeight = 1080.0f;
		float fovY = 0.7853982f; // 45 deg
		float pixelBudget = 1.0f;
	};

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
		//IteratorFlagBit_Static
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
		// Indexed by the mesh/material index stored in scene.meshForNode /
		// scene.materialForNode. Each entry points back to the owning
		// Cluster so resources stay owned by clusters.
		std::vector<MeshLocation> meshLocations;
		std::vector<MaterialLocation> materialLocations;
		std::vector<TextureLocation> textureLocations;
		// Per-mesh LOD markers parallel to meshLocations (cook-time chain:
		// LOD0 + "/lodN" siblings with world-scaled simplifier error used
		// for runtime LOD selection — see SelectLodIndex).
		std::vector<LodInfo> meshLodInfos;

		Cluster& LoadCluster(ResourceManager& ctx);
		// Deserializes a cooked pak (PakReader over caller-owned bytes) into
		// a new registered Cluster. Throws std::runtime_error on failure.
		// The caller issues ctx.FlushUploads() after ResolveAndFixup.
		Cluster& LoadClusterFromPak(ResourceManager& ctx, const class PakReader& reader);
		void UnloadCluster(smvk::Cluster&);

		void RegisterCluster(std::unique_ptr<Cluster> cluster);
		std::unique_ptr<Cluster> UnregisterCluster(Cluster&);
		void ResolveAndFixup_clusters();

		smvk::StaticMesh* GetMesh(uint32_t meshIndex);
		smvk::Material* GetMaterial(uint32_t materialIndex);
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
		struct LodChain {
			// Resolved sibling locations, LOD0..N (lods[0] == meshLocations[i]).
			std::vector<MeshLocation> lods;
			// World-space simplifier error per entry (errors[0] == 0).
			std::vector<float> errors;
		};
		uint32_t LodChainLength(uint32_t meshIndex) const;
		std::optional<MeshLocation> ResolveLodMesh(uint32_t meshIndex, uint32_t lodIndex) const;
		smvk::StaticMesh* GetLodMesh(uint32_t meshIndex, uint32_t lodIndex);
		void RebuildLodChains();
		// Simplifier error for a chain entry (0 for LOD0, world units).
		// Falls back to 0 when the marker is missing.
		float LodErrorAt(uint32_t meshIndex, uint32_t lodIndex) const;
		// Error-driven selection: coarsest LOD whose projected screen error
		// (LodProjectedPixelError over each entry's simplifyError) stays
		// within maxPixelError at `distance`. Returns 0 (full mesh) when no
		// chain was cooked or nothing coarser is in budget.
		uint32_t SelectLodIndex(uint32_t meshIndex, float distance,
			float viewportHeightPx, float fovYRadians, float maxPixelError) const;

		// Cached LOD chains parallel to meshLocations (see RebuildLodChains).
		// Mutable so const hot paths (LodChainLength/SelectLodIndex) can
		// lazily rebuild after direct cluster edits that bypassed
		// ResolveAndFixup_clusters().
		mutable std::vector<LodChain> lodChains;

		// ---- Halfrate LOD updates (staggered halves) ------------------
		// Per-node LOD decisions are cached and refreshed on a parity
		// split: on tick T, only nodes with (node & 1) == (tick & 1)
		// recompute; every other node serves its last decision (1 frame
		// stale at worst). All passes inside one frame observe the SAME
		// value for a node (the first computation of a refresh frame wins
		// and is stamped), so filter/direct splits stay consistent even
		// when queried with different eyes (debug cull override).
		// Cached per-node LOD index for this frame's inputs. Same frameId
		// → same value for a node; refresh cadence is the parity split
		// above (bootstrap computes nodes the first time they're seen).
		uint32_t LodIndexForNode(NodeId node, uint32_t worldMesh,
			const LodFrameInputs& inputs) const;
		// Drops every cached decision (parameter tweak / scene edit).
		void InvalidateLodCache();
		// Cache storage (lazily sized to the node count; reset by
		// ResolveAndFixup_clusters).
		mutable std::vector<uint8_t> nodeLod_;
		mutable std::vector<uint64_t> nodeLodStamp_; // frameId of computation
		mutable uint64_t lodTick_ = 0;               // parity source
		mutable uint64_t lodLastFrameId_ = ~uint64_t(0);
		mutable LodFrameInputs lodInputs_{};

		// First-hit URI resolution across clusters in registration order
		// (mirrors the VFS overlay rule). Used for cross-pak references.
		std::optional<MeshLocation> ResolveMeshAcrossClusters(StringViewT uri) const;
		std::optional<MaterialLocation> ResolveMaterialAcrossClusters(StringViewT uri) const;

		//void NewFrame();
			//if (dirtyCull && flags.size() == scene.hierarchy.size())
			//	Cull(CullConfiguration{});
		//void EndFrame();

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

	private:
		void Cull(CullConfiguration config);
	};

};
