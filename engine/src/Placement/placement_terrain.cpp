#include <Engine/Placement/placement_terrain.h>
#include <Engine/Placement/placement_eval.h>

#include <algorithm>
#include <cmath>

namespace smvk::placement {

HeightfieldData BuildTerrainHeights(const TerrainParams& params) {
	HeightfieldData hf;
	const uint32_t res = std::clamp(params.res, 2u, 512u);
	const float size = std::max(params.size, 1.0f);
	const float height = std::max(params.height, 0.0f);
	const float freq = std::max(params.freq, 1e-4f);
	hf.width = res;
	hf.height = res;
	hf.origin = glm::vec3(params.origin.x, 0.0f, params.origin.y);
	const float sp = size / float(res - 1);
	hf.spacing = glm::vec2(sp, sp);
	hf.heights.resize((size_t)res * res);
	// Rolling hills: two value-noise octaves. Output range [0, height].
	for (uint32_t z = 0; z < res; ++z) {
		for (uint32_t x = 0; x < res; ++x) {
			const float wx = params.origin.x + float(x) * sp;
			const float wz = params.origin.y + float(z) * sp;
			const float n1 = ValueNoise2D(wx * freq, wz * freq, params.seed);
			const float n2 = ValueNoise2D(wx * freq * 3.7f + 11.3f,
				wz * freq * 3.7f + 7.1f, params.seed ^ 0x9e3779b9ULL);
			hf.heights[(size_t)z * res + x] = height * (0.7f * n1 + 0.3f * n2);
		}
	}
	return hf;
}

bool BuildTerrainUpload(const TerrainParams& params, StaticMeshUpload& out) {
	out = StaticMeshUpload{};
	const HeightfieldData hf = BuildTerrainHeights(params);
	const uint32_t res = hf.width;
	if (res < 2)
		return false;
	const float size = std::max(params.size, 1.0f);
	const float sp = size / float(res - 1);
	out.vertices.reserve((size_t)res * res);
	out.indices.reserve((size_t)(res - 1) * (res - 1) * 6);
	glm::vec3 mn(1e30f), mx(-1e30f);
	for (uint32_t z = 0; z < res; ++z) {
		for (uint32_t x = 0; x < res; ++x) {
			const float lx = float(x) * sp;
			const float lz = float(z) * sp;
			const float wx = params.origin.x + lx;
			const float wz = params.origin.y + lz;
			const float h = hf.heights[(size_t)z * res + x];
			const glm::vec3 n = hf.sampleNormal(wx, wz);
			// Stable tangent basis (terrain is never vertical).
			glm::vec3 t = glm::cross(glm::vec3(0, 1, 0), n);
			if (glm::dot(t, t) < 1e-8f)
				t = glm::vec3(1, 0, 0);
			t = glm::normalize(t);
			StaticVertex v{};
			v.pos = glm::vec3(lx, h, lz);
			v.color = glm::vec4(1.0f);
			v.texCoord = glm::vec2(float(x) / float(res - 1),
				float(z) / float(res - 1));
			v.texCoord2 = glm::vec2(0.0f);
			v.normal = n;
			v.tangent = glm::vec4(t, 1.0f);
			out.vertices.push_back(v);
			mn.x = std::min(mn.x, v.pos.x);
			mn.y = std::min(mn.y, v.pos.y);
			mn.z = std::min(mn.z, v.pos.z);
			mx.x = std::max(mx.x, v.pos.x);
			mx.y = std::max(mx.y, v.pos.y);
			mx.z = std::max(mx.z, v.pos.z);
		}
	}
	for (uint32_t z = 0; z + 1 < res; ++z) {
		for (uint32_t x = 0; x + 1 < res; ++x) {
			const uint32_t a = z * res + x;
			const uint32_t b = a + 1;
			const uint32_t c = a + res;
			const uint32_t d = c + 1;
			// Up-facing winding for +Y-up counter-clockwise front faces.
			out.indices.insert(out.indices.end(), { a, c, b, b, c, d });
		}
	}
	Surface s{};
	s.firstIndex = 0;
	s.indexCount = (uint32_t)out.indices.size();
	s.baseVertex = 0;
	out.surfaces.push_back(s);
	out.aabbMin = mn;
	out.aabbMax = mx;
	return true;
}

} // namespace smvk::placement
