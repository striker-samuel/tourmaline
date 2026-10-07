#pragma once
// Tiled GPU terrain heightfield — VISUAL ONLY (no collision, no placement
// integration; the placement Terrain domain is a separate authored system).
//
// One deterministic 4 km x 4 km heightfield rendered as 100 m square tiles
// (40 x 40 by default), each tile a regular (res+1)^2 vertex grid in the
// terrain's OWN vertex format (Renderer/mesh.h TerrainVertex — 32 B
// interleaved POSITION/NORMAL/TANGENT_BFLIP/COLOR). Per-tile topology is
// IDENTICAL, so one shared index buffer serves every tile through
// DrawIndexed(vertexOffset = tileBase); heights live in the vertex stream
// ("vertex form"), never in a heightmap texture.
//
// LOD = the two terrain vertex formats over the SAME buffer: near tiles
// draw with TerrainVertexFormat (4 attributes), far tiles with
// TerrainLowVertexFormat (POSITION + COLOR at the same offsets — the far
// pipeline simply fetches 2 of the 4 attributes out of the same 32-byte
// records; shading degrades to flat-up sun + sky, no normal/tangent).
// The LOD threshold is a distance from the tile AABB.
//
// Draw recording (RecordDraws): CPU frustum-cull per tile (AABB vs the 6
// viewProj planes) + distance LOD, then two grouped bind phases (near
// full, far low) with one DrawIndexed per visible tile. The terrain rides
// the SAME scene push constants (common.hlsli PushConstants) as the Main
// pass — the shaders read FrameUniforms (env: sun/sky/fog) and the
// RenderView system's viewProj, so no terrain-specific push exists.
//
// The field is engine-owned default world dressing: WorldRenderer builds
// it once (the App and the editor live view both render it) and never
// exports it into any scene/cluster/pak — visual-only by design.

#include <Engine/Memory/pool.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/platform.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace smvk::terrain {

// ---- configuration ------------------------------------------------------
struct TerrainDesc {
	// World extent (meters, square, centered at the origin).
	float worldSize = 4000.0f;
	// One tile's extent (meters). tiles = worldSize / tileSize per axis.
	float tileSize = 100.0f;
	// Quads per tile edge (verts per edge = tileRes + 1). All tiles share
	// the topology, so this fixes the shared index buffer too.
	uint32_t tileRes = 16;
	// Height amplitude (meters) of the deterministic field.
	float heightScale = 80.0f;
	// Base noise frequency (1/m). 0.0009 = ~1.1 km hills at 4 km extent.
	float noiseFreq = 0.0009f;
	// Deterministic seed (same desc -> identical field).
	uint64_t seed = 0x7E3D19A4ull;
	// LOD: tiles whose AABB is farther than this from the eye draw with
	// the low pipeline (POSITION + COLOR only).
	float lodDistance = 700.0f;
	// Frustum cull distance cap (tiles beyond the far plane cull anyway;
	// this only bounds degenerate infinite-projection cases).
	float maxDrawDistance = 10000.0f;
};

// Per-tile precomputed constants (built once with the vertex stream).
struct TerrainTileInfo {
	// World-space AABB (heights baked in — exact for the culled grid).
	glm::vec3 min{ 0.0f };
	glm::vec3 max{ 0.0f };
	// This tile's first vertex index in the vertex buffer (the shared
	// index buffer's indices are tile-local; vertexOffset = this).
	int32_t vertexBase = 0;
};

inline constexpr const char* kTerrainVsPath =
	"content/shaders/terrain.vert.hlsl.spv";
inline constexpr const char* kTerrainPsPath =
	"content/shaders/terrain.frag.hlsl.spv";
inline constexpr const char* kTerrainLowVsPath =
	"content/shaders/terrainlow.vert.hlsl.spv";
inline constexpr const char* kTerrainLowPsPath =
	"content/shaders/terrainlow.frag.hlsl.spv";

struct TerrainField {
	TerrainDesc desc{};

	// GPU state (valid == built).
	Holder<BufferHandle> vertexBuffer; // TerrainVertex stream (world space)
	Holder<BufferHandle> indexBuffer;   // ONE tile's topology, shared
	bool valid = false;

	// Tile table (tilesX * tilesZ, row-major z-major: idx = tz * tilesX + tx).
	std::vector<TerrainTileInfo> tiles;
	uint32_t tilesX = 0;
	uint32_t tilesZ = 0;
	uint32_t vertsPerTile = 0;  // (tileRes + 1)^2
	uint32_t indicesPerTile = 0; // tileRes^2 * 6
	uint32_t vertexCount = 0;   // tiles * vertsPerTile
	glm::vec3 boundsMin{ 0.0f };
	glm::vec3 boundsMax{ 0.0f };

	// Full-detail program (POSITION/NORMAL/TANGENT_BFLIP/COLOR).
	Holder<ShaderModuleHandle> vsModule;
	Holder<ShaderModuleHandle> psModule;
	ShaderProgram program{};
	// Lower-detail program (POSITION + COLOR out of the same buffer).
	Holder<ShaderModuleHandle> lowVsModule;
	Holder<ShaderModuleHandle> lowPsModule;
	ShaderProgram lowProgram{};

	[[nodiscard]] bool ShadersValid() const {
		return program.shaderCount == 2 && !vsModule.empty()
			&& !psModule.empty();
	}
	[[nodiscard]] bool LowShadersValid() const {
		return lowProgram.shaderCount == 2 && !lowVsModule.empty()
			&& !lowPsModule.empty();
	}

	bool EnsureShaders(ResourceManager& ctx,
		const char* vsPath = kTerrainVsPath,
		const char* psPath = kTerrainPsPath,
		const char* lowVsPath = kTerrainLowVsPath,
		const char* lowPsPath = kTerrainLowPsPath);
	void DestroyShaders();

	// Deterministic height at world XZ (the exact vertex heights; also the
	// gradient source for normals/tangents). Public for tooling/tests.
	static float HeightAt(float x, float z, const TerrainDesc& d);

	// Builds the vertex stream + shared index buffer + tile table + the
	// shader programs. Idempotent per desc (rebuilds on change).
	bool Build(ResourceManager& ctx, const TerrainDesc& d);
	void Destroy();

	// Frustum-culls + LOD-splits the tiles and records the draws on the
	// command buffer. `push` = the SCENE push constants (engine::
	// IRenderer::PushConstants — the terrain rides the scene pc: frame uniforms env +
	// the RenderView viewProj); pushed after each pipeline bind so the
	// draws are self-contained (no ordering dependency on a prior push).
	// Non-const only because CommandBuffer::BindRenderPipeline takes
	// ShaderProgram& (the same contract as placement's PushAndDispatch).
	// Returns the drawn tile count.
	uint32_t RecordDraws(CommandBuffer& cmd, const glm::mat4& viewProj,
		const glm::vec3& eye, const engine::IRenderer::PushConstants& push,
		const DepthSettings& depth, const StencilSettings& stencil,
		const BlendSettings& blend, const MSAASettings& msaa);
};

} // namespace smvk::terrain
