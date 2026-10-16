#pragma once
// Tiled terrain heightfield — baked from authored H-tiles (no procedural
// fallback; see Rendergraph/terrain_pack.h + terrain_hfield.h).
//
// World: 4096 x 4096 m from terrain.json. 16 x 16 G-blocks of 256 m:
//  - near LODs L0/L1/L2 (64/32/16 quads, TerrainVertex 32 B — the article's
//    Terrain I..III full-float layout) with skirts, one vertex buffer +
//    one shared index buffer per LOD;
//  - far L3 (16 quads, TerrainFarVertex 8 B half4 — the article's Terrain IV
//    R16G16B16A16 layout): grid coords only, world XZ reconstructs in the
//    VS from the per-block push tail, height samples the R16 atlas.
// Heights/normals come from the H-field (computed normals; the import stub
// stays reserved), vertex color carries the S-tile test-mask checker +
// slope rock (near) / mask brightness (far).
//
// Height atlas: the 16 H-tiles merged into ONE 4096x4096 R16_UNORM texture
// (RG16 bits copied verbatim — linear filtering stays CORRECT, unlike
// packed-RGBA filtering). One bindless slot serves the far VS, the shadow
// raymarch, and future collision/GI. Masks bake into vertex color and are
// dropped after the build (GPU mask textures land with the atlas in phase 4).
//
// Shadows (T3): shadowmap RECEIVE (lights[0] sun convention, PCF/EVSM like
// default_pom) times... minned with the heightfield RAYMARCHED distant
// self-shadow (terrain_shadow.hlsli, HZD-style). Mode/steps ride the frag
// push tail from the RunConfig snapshot (TOURMALINE_TERRAIN_HFSHADOW[_STEPS]).
//
// Draw recording (RecordBlocks): CPU frustum-cull per block (AABB vs the 6
// viewProj planes) + distance LOD, then grouped binds (3x near full-format
// + 1x far) with one DrawIndexed per visible block. The terrain rides the
// SAME scene push constants as the Main pass; per-draw tails (shade/far)
// overlay bytes 192..224 like PlacementVsPush.

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/App/world.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace smvk::terrain {

// ---- configuration ------------------------------------------------------
struct TerrainDesc {
	// LOD splits (AABB closest-approach distance, meters): < lodNearFull
	// draws L0, < lodMid draws L1, < lodFar draws L2, else far L3.
	float lodNearFull = 400.0f;
	float lodMid = 900.0f;
	float lodFar = 2000.0f;
	float maxDrawDistance = 10000.0f;
};

// Per-block baked constants (16 x 16 grid, row-major: idx = gz * 16 + gx).
struct TerrainBlockInfo {
	// World-space AABB over the L0 surface, skirt bottom included in min.
	// Shared by all 4 LODs (L1/L2/far stay inside the L0 hull: same
	// heights, coarser grids + identical skirt drop).
	glm::vec3 min{ 0.0f };
	glm::vec3 max{ 0.0f };
	// This block's first vertex index per near LOD (shared per-LOD
	// indices are block-local; vertexOffset = vertexBase[lod]).
	std::array<int32_t, 3> vertexBase{ 0, 0, 0 };
	// This block's first vertex index in the far buffer.
	int32_t farVertexBase = 0;
};

inline constexpr uint32_t kTerrainBlocksPerAxis = 16;
inline constexpr uint32_t kTerrainBlockCount =
	kTerrainBlocksPerAxis * kTerrainBlocksPerAxis;

// Per-draw tail @192 for ALL terrain draws (32 B; HLSL mirrors:
// terrain.frag.hlsl TerrainFragTail reads every word, terrainfar.vert.hlsl
// TerrainFarPushHlsl reads the world half). Words 0..5 are layout-identical
// for both readers. The far VS takes its block id from firstInstance
// (gl_InstanceIndex) and its skirt drop from the kFarSkirtDepth literal
// (== kSkirtDepth in terrain_blocks.h); far draws always push hfShadow=2
// (receive + raymarch) with the caller's steps.
struct TerrainBlockPush {
	uint32_t atlasSlot = 0xFFFFFFFFu; // R16 height-atlas bindless slot
	float originX = 0.0f;             // world origin (atlas uv00)
	float originZ = 0.0f;
	float invWorldSize = 0.0f;        // 1/worldSize (uv = (xz-origin)*inv)
	float baseY = 0.0f;               // worldY = baseY + h01*range
	float range = 1.0f;
	int32_t hfShadow = 1;             // 0 off | 1 receive | 2 receive+raymarch
	int32_t hfSteps = 16;             // raymarch steps (4..64)
};
static_assert(sizeof(TerrainBlockPush) == 32,
	"TerrainBlockPush moved - update terrain.frag.hlsl + terrainfar.vert.hlsl");

inline constexpr uint32_t kTerrainPushOffset = 192;

inline constexpr const char* kTerrainVsPath =
	"content/shaders/terrain.vert.hlsl.spv";
inline constexpr const char* kTerrainPsPath =
	"content/shaders/terrain.frag.hlsl.spv";
inline constexpr const char* kTerrainFarVsPath =
	"content/shaders/terrainfar.vert.hlsl.spv";

struct TerrainField {
	TerrainDesc desc{};

	// Baked state (valid == built from a TerrainSource).
	TerrainHField hfield{}; // kept for queries (collision/T3 CPU)
	Holder<BufferHandle> vertexBuffer; // ALL blocks x 3 near LODs
	Holder<BufferHandle> indexBuffers[3]{}; // one shared topology per LOD
	Holder<BufferHandle> farVertexBuffer; // ALL blocks, TerrainFarVertex
	Holder<BufferHandle> farIndexBuffer;  // one shared far topology
	Holder<TextureHandle> heightAtlas{};  // merged 4096 R16 (one slot)
	uint32_t atlasSlot = 0xFFFFFFFFu;
	bool valid = false;

	std::vector<TerrainBlockInfo> blocks; // 256, row-major
	std::array<uint32_t, 3> indicesPerLod{ 0, 0, 0 };
	uint32_t farIndexCount = 0;
	uint32_t vertexCount = 0;
	uint32_t farVertexCount = 0;
	glm::vec3 boundsMin{ 0.0f };
	glm::vec3 boundsMax{ 0.0f };

	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> psModule;
	ShaderProgram program{};
	Holder<ShaderModuleHandle> farVsModule;
	ShaderProgram farProgram{}; // far VS + the SAME terrain frag

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount == 2 && !vsModule.empty()
			&& !psModule.empty();
	}
	[[nodiscard]] bool FarShadersValid() const {
		return farProgram.shaderCount == 2 && !farVsModule.empty()
			&& !psModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kTerrainVsPath,
		const char* psPath = kTerrainPsPath,
		const char* farVsPath = kTerrainFarVsPath);
	void DestroyShaders();

	// Bakes G-blocks from the harvested source (H + S masks). Masks bake
	// into vertex color and are NOT retained. Idempotent (rebuilds).
	bool BuildFromSource(ResourceManager& ctx, const TerrainSource& src,
		const TerrainDesc& d = TerrainDesc{});
	void Destroy();

	// Frustum-culls + LOD-selects the blocks and records the draws (see
	// the header contract). hfShadow/hfSteps ride the frag tail from the
	// RunConfig snapshot (WorldRenderer::Configure). Returns the drawn
	// block count.
	uint32_t RecordBlocks(CommandBuffer& cmd, const glm::mat4& viewProj,
		const glm::vec3& eye, const engine::IRenderer::PushConstants& push,
		const DepthSettings& depth, const StencilSettings& stencil,
		const BlendSettings& blend, const MSAASettings& msaa,
		int hfShadow = 1, int hfSteps = 16);
};

} // namespace smvk::terrain
