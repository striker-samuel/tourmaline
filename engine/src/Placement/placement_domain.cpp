// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_domain.h>

#include <algorithm>
#include <cmath>

namespace tour::placement {
using namespace ::smvk;

DomainSample GridDomain::sample(StableID id, uint32_t candidateIndex) const {
	DomainSample s;
	if (cellsX == 0 || cellsZ == 0) return s;
	const uint32_t cell = candidateIndex % cellCount();
	const uint32_t cx = cell % cellsX;
	const uint32_t cz = cell / cellsX;
	const Random02 j = Random02Of(id, RandomChannel::Position);
	const float fx = (static_cast<float>(cx) + j.x) / static_cast<float>(cellsX);
	const float fz = (static_cast<float>(cz) + j.y) / static_cast<float>(cellsZ);
	s.p = glm::vec3(origin.x + fx * extent.x, origin.y, origin.z + fz * extent.y);
	s.n = glm::vec3(0, 1, 0);
	s.uv = glm::vec2(fx, fz);
	s.primitiveId = cell;
	return s;
}

float TriangleArea(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
	return 0.5f * glm::length(glm::cross(b - a, c - a));
}

void BuildMeshAliasTable(PlacementMeshData& mesh) {
	const size_t n = mesh.triangleCount();
	mesh.aliasProb.assign(n, 1.0f);
	mesh.aliasIndex.assign(n, 0);
	if (n == 0) return;
	std::vector<float> area(n, 0.0f);
	double total = 0.0;
	for (size_t t = 0; t < n; ++t) {
		const glm::vec3& a = mesh.positions[mesh.indices[t * 3 + 0]];
		const glm::vec3& b = mesh.positions[mesh.indices[t * 3 + 1]];
		const glm::vec3& c = mesh.positions[mesh.indices[t * 3 + 2]];
		area[t] = TriangleArea(a, b, c);
		total += area[t];
	}
	if (!(total > 0.0)) { // fully degenerate: uniform fallback
		for (size_t t = 0; t < n; ++t) {
			mesh.aliasProb[t] = 1.0f;
			mesh.aliasIndex[t] = static_cast<uint32_t>(t);
		}
		return;
	}
	std::vector<float> scaled(n);
	for (size_t t = 0; t < n; ++t) scaled[t] = area[t] * (float)n / (float)total;
	std::vector<uint32_t> small, large;
	small.reserve(n); large.reserve(n);
	for (uint32_t t = 0; t < (uint32_t)n; ++t)
		(scaled[t] < 1.0f ? small : large).push_back(t);
	while (!small.empty() && !large.empty()) {
		uint32_t s = small.back(); small.pop_back();
		uint32_t l = large.back(); large.pop_back();
		mesh.aliasProb[s] = scaled[s];
		mesh.aliasIndex[s] = l;
		scaled[l] = scaled[l] + scaled[s] - 1.0f;
		if (scaled[l] < 1.0f) small.push_back(l);
		else large.push_back(l);
	}
	for (uint32_t t : small) { mesh.aliasProb[t] = 1.0f; mesh.aliasIndex[t] = t; }
	for (uint32_t t : large) { mesh.aliasProb[t] = 1.0f; mesh.aliasIndex[t] = t; }
}

DomainSample SampleMeshSurface(const PlacementMeshData& mesh, float r1, float r2, float bx, float by) {
	DomainSample s;
	const size_t n = mesh.triangleCount();
	if (n == 0 || mesh.positions.empty() || mesh.indices.empty()) return s;
	r1 = std::clamp(r1, 0.0f, 0.99999994f);
	r2 = std::clamp(r2, 0.0f, 0.99999994f);
	uint32_t col = static_cast<uint32_t>(r1 * (float)n);
	if (col >= n) col = (uint32_t)n - 1;
	uint32_t tri = col;
	if (mesh.hasAlias() && r2 >= mesh.aliasProb[col]) tri = mesh.aliasIndex[col];
	// Reflect barycentrics into the triangle (standard uniform mapping).
	float u = std::clamp(bx, 0.0f, 0.99999994f);
	float v = std::clamp(by, 0.0f, 0.99999994f);
	if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
	const float w = 1.0f - u - v;
	const uint32_t i0 = mesh.indices[tri * 3 + 0];
	const uint32_t i1 = mesh.indices[tri * 3 + 1];
	const uint32_t i2 = mesh.indices[tri * 3 + 2];
	const glm::vec3& a = mesh.positions[i0];
	const glm::vec3& b = mesh.positions[i1];
	const glm::vec3& c = mesh.positions[i2];
	s.p = a * w + b * u + c * v;
	if (!mesh.normals.empty()) {
		s.n = glm::normalize(mesh.normals[i0] * w + mesh.normals[i1] * u + mesh.normals[i2] * v);
	} else {
		glm::vec3 e1 = b - a, e2 = c - a;
		s.n = glm::normalize(glm::cross(e1, e2));
	}
	if (!mesh.uvs.empty())
		s.uv = mesh.uvs[i0] * w + mesh.uvs[i1] * u + mesh.uvs[i2] * v;
	s.primitiveId = tri;
	return s;
}

DomainSample SampleMeshSurface(const PlacementMeshData& mesh, StableID id) {
	const Random02 a = Random02Of(id, RandomChannel::Position);
	const Random02 b = Random02Of(id, RandomChannel::Rotation);
	return SampleMeshSurface(mesh, a.x, a.y, b.x, b.y);
}

float HeightfieldData::sampleHeight(float x, float z) const {
	if (!valid()) return 0.0f;
	float gx = (x - origin.x) / spacing.x;
	float gz = (z - origin.z) / spacing.y;
	gx = std::clamp(gx, 0.0f, (float)width - 1.001f);
	gz = std::clamp(gz, 0.0f, (float)height - 1.001f);
	const uint32_t x0 = (uint32_t)gx, z0 = (uint32_t)gz;
	const float fx = gx - (float)x0, fz = gz - (float)z0;
	const float h00 = heights[(size_t)z0 * width + x0];
	const float h10 = heights[(size_t)z0 * width + x0 + 1];
	const float h01 = heights[(size_t)(z0 + 1) * width + x0];
	const float h11 = heights[(size_t)(z0 + 1) * width + x0 + 1];
	return h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz;
}

glm::vec3 HeightfieldData::sampleNormal(float x, float z) const {
	const float e = std::min(spacing.x, spacing.y);
	const float hx1 = sampleHeight(x + e, z), hx0 = sampleHeight(x - e, z);
	const float hz1 = sampleHeight(x, z + e), hz0 = sampleHeight(x, z - e);
	glm::vec3 n((hx0 - hx1) / (2.0f * e), 1.0f, (hz0 - hz1) / (2.0f * e));
	return glm::normalize(n);
}

float EcotopeMaskData::Sample(uint32_t channel, float x, float z) const {
	if (!valid() || channel >= channels) return 0.0f;
	// Same texel mapping + clamp + bilinear composition as
	// HeightfieldData::sampleHeight (the documented parity contract).
	float gx = (x - origin.x) / spacing.x;
	float gz = (z - origin.y) / spacing.y;
	gx = std::clamp(gx, 0.0f, (float)width - 1.001f);
	gz = std::clamp(gz, 0.0f, (float)height - 1.001f);
	const uint32_t x0 = (uint32_t)gx, z0 = (uint32_t)gz;
	const float fx = gx - (float)x0, fz = gz - (float)z0;
	const size_t base = ((size_t)channel * height + z0) * width + x0;
	const float w00 = weights[base];
	const float w10 = weights[base + 1];
	const float w01 = weights[base + width];
	const float w11 = weights[base + width + 1];
	return w00 * (1 - fx) * (1 - fz) + w10 * fx * (1 - fz)
		+ w01 * (1 - fx) * fz + w11 * fx * fz;
}

DomainSample HeightfieldDomain::sample(StableID id) const {
	DomainSample s;
	const Random02 j = Random02Of(id, RandomChannel::Position);
	const float x = extentMin.x + j.x * (extentMax.x - extentMin.x);
	const float z = extentMin.y + j.y * (extentMax.y - extentMin.y);
	s.p = glm::vec3(x, field ? field->sampleHeight(x, z) : 0.0f, z);
	s.n = field ? field->sampleNormal(x, z) : glm::vec3(0, 1, 0);
	s.uv = glm::vec2(j.x, j.y);
	s.primitiveId = 0;
	return s;
}

} // namespace tour::placement
