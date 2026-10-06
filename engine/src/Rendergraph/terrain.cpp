// Tiled GPU terrain heightfield (visual-only) â€” see Rendergraph/terrain.h.
//
// CPU build: deterministic 2-octave value-noise heights sampled analytically
// per vertex (the same field HeightAt serves tests/tooling), packed into the
// 32-byte TerrainVertex stream (world space), one shared index buffer for
// the identical per-tile topology. Draw: 6-plane frustum cull + distance
// LOD, grouped into two pipeline phases (near full / far low).

#include <Engine/Rendergraph/terrain.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace smvk::terrain {

namespace {

// ---- deterministic value noise (hash lattice, 2 octaves) ----------------
// Integer lattice hash -> [0,1) value; bilinear-smoothstep interpolation.
// Same shape as the placement terrain generator (placement_terrain.cpp),
// kept local so the field stays self-contained (no placement dependency).

inline uint32_t TerrainHash2(uint32_t x, uint32_t z, uint64_t seed) {
	uint64_t h = seed;
	h ^= uint64_t(x) * 0x9E3779B97F4A7C15ull;
	h ^= uint64_t(z) * 0xC2B2AE3D27D4EB4Full;
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDull;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ull;
	h ^= h >> 33;
	return uint32_t(h);
}

inline float TerrainLattice(uint32_t x, uint32_t z, uint64_t seed) {
	return float(TerrainHash2(x, z, seed) & 0xFFFFFFu) / float(0x1000000u);
}

inline float TerrainSmooth(float t) {
	return t * t * (3.0f - 2.0f * t);
}

inline float TerrainNoise(float x, float z, uint64_t seed) {
	const float xi = std::floor(x);
	const float zi = std::floor(z);
	const uint32_t ix = uint32_t(int64_t(xi));
	const uint32_t iz = uint32_t(int64_t(zi));
	const float fx = TerrainSmooth(x - xi);
	const float fz = TerrainSmooth(z - zi);
	const float v00 = TerrainLattice(ix, iz, seed);
	const float v10 = TerrainLattice(ix + 1u, iz, seed);
	const float v01 = TerrainLattice(ix, iz + 1u, seed);
	const float v11 = TerrainLattice(ix + 1u, iz + 1u, seed);
	const float a = v00 + (v10 - v00) * fx;
	const float b = v01 + (v11 - v01) * fx;
	return a + (b - a) * fz; // [0,1]
}

} // namespace

float TerrainField::HeightAt(float x, float z, const TerrainDesc& d) {
	// Two octaves: km-scale hills + quarter-scale detail. Ridged second
	// octave (1 - |2n-1|) keeps slopes interesting without high-frequency
	// noise (the 6.25 m grid cannot resolve it anyway).
	const float wx = x * d.noiseFreq;
	const float wz = z * d.noiseFreq;
	const float n1 = TerrainNoise(wx, wz, d.seed);
	const float n2 = TerrainNoise(wx * 3.7f + 11.3f, wz * 3.7f - 7.9f,
		d.seed ^ 0xA5A5A5A5ull);
	const float ridge = 1.0f - std::fabs(n2 * 2.0f - 1.0f);
	const float h = n1 * 0.7f + ridge * 0.3f;
	return h * d.heightScale;
}

bool TerrainField::EnsureShaders(ResourceManager& ctx,
	const char* vsPath, const char* psPath,
	const char* lowVsPath, const char* lowPsPath) {
	bool ok = true;
	try {
		if (vsModule.empty())
			vsModule = ctx.CreateShaderModule(
				readFile(vsPath ? vsPath : kTerrainVsPath), "terrainVert");
		if (psModule.empty())
			psModule = ctx.CreateShaderModule(
				readFile(psPath ? psPath : kTerrainPsPath), "terrainFrag");
		if (lowVsModule.empty())
			lowVsModule = ctx.CreateShaderModule(
				readFile(lowVsPath ? lowVsPath : kTerrainLowVsPath),
				"terrainLowVert");
		if (lowPsModule.empty())
			lowPsModule = ctx.CreateShaderModule(
				readFile(lowPsPath ? lowPsPath : kTerrainLowPsPath),
				"terrainLowFrag");
	}
	catch (...) {
		ok = false;
	}
	if (vsModule.empty() || psModule.empty())
		ok = false;
	if (ok && !ShadersValid()) {
		Shader vs{ .module = vsModule, .entryPoint = "mainVS",
			.stage = ShaderStage::Vertex };
		Shader ps{ .module = psModule, .entryPoint = "mainPS",
			.stage = ShaderStage::Fragment };
		program = { .shaders = { vs, ps }, .shaderCount = 2 };
	}
	if (lowVsModule.empty() || lowPsModule.empty())
		ok = false;
	if (ok && !LowShadersValid()) {
		Shader vs{ .module = lowVsModule, .entryPoint = "mainVS",
			.stage = ShaderStage::Vertex };
		Shader ps{ .module = lowPsModule, .entryPoint = "mainPS",
			.stage = ShaderStage::Fragment };
		lowProgram = { .shaders = { vs, ps }, .shaderCount = 2 };
	}
	return ok;
}

void TerrainField::DestroyShaders() {
	program = ShaderProgram{};
	lowProgram = ShaderProgram{};
	vsModule = {};
	psModule = {};
	lowVsModule = {};
	lowPsModule = {};
}

bool TerrainField::Build(ResourceManager& ctx, const TerrainDesc& d) {
	// Shape guards: positive sizes, res in [2..64] (64 -> 65^2 = 4225 verts
	// per tile, plenty for a 100 m tile), world divisible into whole tiles.
	if (!(d.worldSize > 0.0f) || !(d.tileSize > 0.0f))
		return false;
	if (d.tileRes < 2 || d.tileRes > 64)
		return false;
	const float tilesF = d.worldSize / d.tileSize;
	if (tilesF < 1.0f)
		return false;
	const uint32_t tx = uint32_t(tilesF + 0.5f);
	if (std::fabs(float(tx) - tilesF) > 1e-3f || tx == 0 || tx > 128)
		return false;

	Destroy();
	desc = d;
	tilesX = tx;
	tilesZ = tx;
	vertsPerTile = (d.tileRes + 1u) * (d.tileRes + 1u);
	indicesPerTile = d.tileRes * d.tileRes * 6u;
	vertexCount = tilesX * tilesZ * vertsPerTile;
	tiles.resize(size_t(tilesX) * size_t(tilesZ));

	// World-space grid: the field is centered at the origin, tile (0,0) at
	// (-worldSize/2, -worldSize/2). The effective world extent snaps to
	// whole tiles (worldSize = tileSize * tiles).
	const float half = 0.5f * d.tileSize * float(tilesX);
	const float step = d.tileSize / float(d.tileRes);

	// ---- vertex stream (world space, TerrainVertex 32 B) ----------------
	// (Plain-size locals: `std::vector<T> v(size_t(n))` parses as a
	// function declaration — the most vexing parse.)
	const size_t vertCountSz = size_t(vertexCount);
	std::vector<TerrainVertex> verts(vertCountSz);
	// Per-tile min/max heights (AABB) + the tile-local authoring loop.
	float globalMinY = 1e30f;
	float globalMaxY = -1e30f;
	{
		TerrainVertex* out = verts.data();
		const float eps = step; // gradient sample distance (one grid cell)
		for (uint32_t tz = 0; tz < tilesZ; ++tz) {
			for (uint32_t txi = 0; txi < tilesX; ++txi) {
				TerrainTileInfo& info =
					tiles[size_t(tz) * size_t(tilesX) + size_t(txi)];
				info.vertexBase = int32_t((tz * tilesX + txi) * vertsPerTile);
				float minY = 1e30f;
				float maxY = -1e30f;
				const float x0 = -half + float(txi) * d.tileSize;
				const float z0 = -half + float(tz) * d.tileSize;
				for (uint32_t j = 0; j <= d.tileRes; ++j) {
					const float z = z0 + float(j) * step;
					for (uint32_t i = 0; i <= d.tileRes; ++i) {
						const float x = x0 + float(i) * step;
						const float h = HeightAt(x, z, d);
						// Analytic-plane normal via central differences of
						// the SAME height function (matches neighbors by
						// construction â€” no per-tile seams).
						const float hx = HeightAt(x + eps, z, d)
							- HeightAt(x - eps, z, d);
						const float hz = HeightAt(x, z + eps, d)
							- HeightAt(x, z - eps, d);
						const glm::vec3 n = glm::normalize(
							glm::vec3(-hx, 2.0f * eps, -hz));
						// Tangent along +X; bitangent flip -1 so
						// B = cross(N, T) * flip points +Z (flat case:
						// cross(+Y, +X) = -Z).
						const glm::vec3 t = glm::normalize(
							glm::vec3(2.0f * eps, hx, 0.0f));
						// Height/slope albedo: valley green -> rock gray ->
						// snow on high flats. Steep slopes always rock.
						const float slope = 1.0f - n.y;
						glm::vec3 albedo(0.24f, 0.34f, 0.16f);
						const glm::vec3 rock(0.36f, 0.33f, 0.30f);
						const glm::vec3 snow(0.86f, 0.88f, 0.92f);
						const float rockT = std::clamp(slope * 3.2f, 0.0f, 1.0f);
						albedo = albedo * (1.0f - rockT) + rock * rockT;
						const float snowT = std::clamp(
							(h - d.heightScale * 0.62f) / (d.heightScale * 0.25f),
							0.0f, 1.0f) * (1.0f - std::clamp(slope * 4.0f, 0.0f, 1.0f));
						albedo = albedo * (1.0f - snowT) + snow * snowT;

						TerrainVertexSrc src{};
						src.pos = glm::vec3(x, h, z);
						src.normal = n;
						src.tangent = t;
						src.tangentFlip = -1.0f;
						src.color = glm::vec4(albedo, 1.0f);
						PackTerrainVertex(src, *out);
						++out;
						minY = std::min(minY, h);
						maxY = std::max(maxY, h);
					}
				}
				info.min = glm::vec3(x0, minY, z0);
				info.max = glm::vec3(x0 + d.tileSize, maxY,
					z0 + d.tileSize);
				globalMinY = std::min(globalMinY, minY);
				globalMaxY = std::max(globalMaxY, maxY);
			}
		}
	}
	boundsMin = glm::vec3(-half, globalMinY, -half);
	boundsMax = glm::vec3(half, globalMaxY, half);

	// ---- shared index buffer (ONE tile's topology; vertexOffset per tile)
	const size_t idxCountSz = size_t(indicesPerTile);
	std::vector<uint32_t> indices(idxCountSz);
	{
		uint32_t k = 0;
		const uint32_t row = d.tileRes + 1u;
		for (uint32_t j = 0; j < d.tileRes; ++j) {
			for (uint32_t i = 0; i < d.tileRes; ++i) {
				const uint32_t a = j * row + i;
				const uint32_t b = a + 1u;
				const uint32_t c = a + row;
				const uint32_t e = c + 1u;
				// Up-facing winding for +Y-up counter-clockwise front
				// faces (the placement terrain's proven convention).
				indices[k++] = a;
				indices[k++] = c;
				indices[k++] = b;
				indices[k++] = b;
				indices[k++] = c;
				indices[k++] = e;
			}
		}
	}

	vertexBuffer = ctx.CreateBuffer(
		sizeof(TerrainVertex) * size_t(vertexCount),
		BufferType_Vertex, "terrain vertices");
	indexBuffer = ctx.CreateBuffer(
		sizeof(uint32_t) * size_t(indicesPerTile),
		BufferType_Index, "terrain indices");
	if (vertexBuffer.empty() || indexBuffer.empty()) {
		Destroy();
		return false;
	}
	ctx.UploadDataToBuffer(vertexBuffer, verts.data(),
		sizeof(TerrainVertex) * size_t(vertexCount),
		PipelineStageBit_AllGraphics, AccessFlags::VertexAttributeRead, 0, 0);
	ctx.UploadDataToBuffer(indexBuffer, indices.data(),
		sizeof(uint32_t) * size_t(indicesPerTile),
		PipelineStageBit_AllGraphics, AccessFlags::IndexRead, 0, 0);
	valid = true;

	if (!EnsureShaders(ctx)) {
		std::fprintf(stderr,
			"[terrain] shader compile failed - the field stays built but "
			"draws are skipped until the .spv files exist\n");
	}
	std::fprintf(stderr, "[terrain] built: %ux%u tiles of %.0fm "
		"(res %u, %u verts, %u indices/tile) heights [%.1f, %.1f]\n",
		tilesX, tilesZ, d.tileSize, d.tileRes, vertexCount,
		indicesPerTile, globalMinY, globalMaxY);
	return true;
}

void TerrainField::Destroy() {
	vertexBuffer = {};
	indexBuffer = {};
	valid = false;
	tiles.clear();
	tilesX = tilesZ = 0;
	vertsPerTile = indicesPerTile = vertexCount = 0;
	boundsMin = glm::vec3(0.0f);
	boundsMax = glm::vec3(0.0f);
}

namespace {

// Frustum planes from a (post Y-flip, reversed-Z ok) viewProj: the 6
// row-pair planes, Gribb/Hartmann extraction. Planes face INWARD (a point
// inside satisfies dot(p, n) + d >= 0 for all 6).
inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 out[6]) {
	// Rows of m (column-major glm: m[i] is column i; row j = (m[0][j],
	// m[1][j], m[2][j], m[3][j])).
	const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
	const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
	const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
	const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
	// left, right, bottom, top (w + the respective row); reversed-Z:
	// near/far flip to r3-r2 / r3+r2, which still bound the same volume.
	out[0] = r3 + r0;
	out[1] = r3 - r0;
	out[2] = r3 + r1;
	out[3] = r3 - r1;
	out[4] = r3 - r2;
	out[5] = r3 + r2;
	for (int i = 0; i < 6; ++i) {
		const float l = glm::length(glm::vec3(out[i]));
		if (l > 1e-12f)
			out[i] /= l;
	}
}

// AABB vs 6 inward planes: outside when fully behind any one plane.
inline bool AabbInFrustum(const glm::vec3& mn, const glm::vec3& mx,
	const glm::vec4 planes[6]) {
	for (int i = 0; i < 6; ++i) {
		const glm::vec3 p(
			planes[i].x > 0.0f ? mx.x : mn.x,
			planes[i].y > 0.0f ? mx.y : mn.y,
			planes[i].z > 0.0f ? mx.z : mn.z);
		if (glm::dot(p, glm::vec3(planes[i])) + planes[i].w < 0.0f)
			return false;
	}
	return true;
}

} // namespace

uint32_t TerrainField::RecordDraws(CommandBuffer& cmd,
	const glm::mat4& viewProj, const glm::vec3& eye,
	const engine::PushConstants& push, const DepthSettings& depth,
	const StencilSettings& stencil, const BlendSettings& blend,
	const MSAASettings& msaa) {
	if (!valid || tiles.empty() || vertexBuffer.empty()
		|| indexBuffer.empty())
		return 0;
	if (!ShadersValid())
		return 0; // built but shaders missing: nothing to draw

	glm::vec4 planes[6];
	ExtractFrustumPlanes(viewProj, planes);

	// Cull + LOD split. Near = full pipeline (4 attributes), far = low
	// pipeline (POSITION + COLOR). Distance = closest approach from the
	// eye to the tile AABB (squared, vs lodDistance^2).
	std::vector<uint32_t> nearTiles;
	std::vector<uint32_t> farTiles;
	const float lod2 = desc.lodDistance * desc.lodDistance;
	const float maxD2 = desc.maxDrawDistance * desc.maxDrawDistance;
	for (uint32_t ti = 0; ti < tiles.size(); ++ti) {
		const TerrainTileInfo& t = tiles[ti];
		if (!AabbInFrustum(t.min, t.max, planes))
			continue;
		const glm::vec3 d = glm::clamp(eye, t.min, t.max) - eye;
		const float d2 = glm::dot(d, d);
		if (d2 > maxD2)
			continue;
		(d2 > lod2 ? farTiles : nearTiles).push_back(ti);
	}
	if (nearTiles.empty() && farTiles.empty())
		return 0;

	cmd.BindVertexBuffer(vertexBuffer, 0, 0);
	cmd.BindIndexBuffer(indexBuffer, 0);

	const uint32_t idxCount = indicesPerTile;
	uint32_t drawn = 0;
	// The scene pc (frame uniforms env + RenderView viewProj) rides every
	// bind: pushed AFTER each pipeline bind so the draws never depend on a
	// prior push in the pass.
	const auto pushPc = [&cmd, &push]() {
		cmd.PushConstants(0, sizeof(engine::PushConstants), &push,
			ShaderStage::All);
	};
	// Near phase: full format (positions + normals + tangents).
	if (!nearTiles.empty()) {
		cmd.BindRenderPipeline(program,
			VertexFormat::TerrainVertexFormat, depth, stencil, blend, msaa,
			RasterSettings{} /* back-face cull, CCW front (tile winding) */);
		pushPc();
		for (uint32_t ti : nearTiles) {
			cmd.DrawIndexed(idxCount, 1, 0, tiles[ti].vertexBase, 0);
			++drawn;
		}
	}
	// Far phase: low format (POSITION + COLOR out of the same records).
	if (!farTiles.empty()) {
		if (LowShadersValid()) {
			cmd.BindRenderPipeline(
				lowProgram,
				VertexFormat::TerrainLowVertexFormat, depth, stencil,
				blend, msaa, RasterSettings{});
			pushPc();
			for (uint32_t ti : farTiles) {
				cmd.DrawIndexed(idxCount, 1, 0, tiles[ti].vertexBase, 0);
				++drawn;
			}
		}
		else {
			// Low shaders missing: degrade to the full pipeline rather
			// than popping tiles out (a missing .spv must never drop
			// terrain coverage).
			cmd.BindRenderPipeline(program,
				VertexFormat::TerrainVertexFormat, depth, stencil, blend,
				msaa, RasterSettings{});
			pushPc();
			for (uint32_t ti : farTiles) {
				cmd.DrawIndexed(idxCount, 1, 0, tiles[ti].vertexBase, 0);
				++drawn;
			}
		}
	}
	return drawn;
}

} // namespace smvk::terrain
