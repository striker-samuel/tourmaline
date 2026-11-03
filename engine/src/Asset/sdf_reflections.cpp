// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/sdf_reflections.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

namespace tour::asset {
namespace {

bool Finite(const glm::vec3& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite(const glm::mat4& m) {
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			if (!std::isfinite(m[col][row])) return false;
	return true;
}

bool Valid(const StaticSdfTraceOptions& o) {
	return std::isfinite(o.maxDistance) && o.maxDistance > 0.0f &&
		std::isfinite(o.hitEpsilon) && o.hitEpsilon > 0.0f &&
		std::isfinite(o.originBias) && o.originBias >= 0.0f &&
		o.maxSteps > 0 && o.maxSteps <= 4096;
}

bool Valid(const SdfVolume& v) {
	const size_t r = v.resolution;
	return r >= 2 && r <= 128 && v.samples.size() == r * r * r &&
		Finite(v.origin) && std::isfinite(v.voxelSize) && v.voxelSize > 0.0f &&
		std::isfinite(v.maxDistance) && v.maxDistance > 0.0f;
}

bool RayBox(const glm::vec3& origin, const glm::vec3& direction,
	const glm::vec3& bmin, const glm::vec3& bmax,
	float& tmin, float& tmax) {
	for (int axis = 0; axis < 3; ++axis) {
		const float o = origin[axis], d = direction[axis];
		if (std::abs(d) < 1e-12f) {
			if (o < bmin[axis] || o > bmax[axis]) return false;
			continue;
		}
		float a = (bmin[axis] - o) / d;
		float b = (bmax[axis] - o) / d;
		if (a > b) std::swap(a, b);
		tmin = std::max(tmin, a);
		tmax = std::min(tmax, b);
		if (tmax < tmin) return false;
	}
	return true;
}

float Sample(const SdfVolume& v, const glm::vec3& p) {
	const float maxGrid = float(v.resolution - 1);
	const glm::vec3 grid = glm::clamp((p - v.origin) / v.voxelSize,
		glm::vec3(0.0f), glm::vec3(maxGrid));
	const uint32_t x = uint32_t(grid.x), y = uint32_t(grid.y), z = uint32_t(grid.z);
	const uint32_t x1 = std::min(x + 1, v.resolution - 1);
	const uint32_t y1 = std::min(y + 1, v.resolution - 1);
	const uint32_t z1 = std::min(z + 1, v.resolution - 1);
	const glm::vec3 f = grid - glm::vec3(float(x), float(y), float(z));
	const size_t r = v.resolution;
	const auto tap = [&](uint32_t ix, uint32_t iy, uint32_t iz) {
		return float(v.samples[size_t(ix) + r * (size_t(iy) + r * size_t(iz))]);
	};
	const float a = glm::mix(tap(x, y, z), tap(x1, y, z), f.x);
	const float b = glm::mix(tap(x, y1, z), tap(x1, y1, z), f.x);
	const float c = glm::mix(tap(x, y, z1), tap(x1, y, z1), f.x);
	const float d = glm::mix(tap(x, y1, z1), tap(x1, y1, z1), f.x);
	return glm::mix(glm::mix(a, b, f.y), glm::mix(c, d, f.y), f.z)
		* (v.maxDistance / 32767.0f);
}

glm::vec3 Gradient(const SdfVolume& v, const glm::vec3& p) {
	const float h = 0.5f * v.voxelSize;
	const glm::vec3 dx(h, 0.0f, 0.0f), dy(0.0f, h, 0.0f), dz(0.0f, 0.0f, h);
	return glm::vec3(
		Sample(v, p + dx) - Sample(v, p - dx),
		Sample(v, p + dy) - Sample(v, p - dy),
		Sample(v, p + dz) - Sample(v, p - dz));
}

bool TraceOne(const StaticSdfInstance& instance, const glm::vec3& origin,
	const glm::vec3& direction, const StaticSdfTraceOptions& options,
	float maxDistance, float& hitDistance, glm::vec3& hitNormal) {
	if (!instance.volume || !Valid(*instance.volume) ||
		!Finite(instance.meshToWorld) ||
		std::abs(instance.meshToWorld[0][3]) > 1e-6f ||
		std::abs(instance.meshToWorld[1][3]) > 1e-6f ||
		std::abs(instance.meshToWorld[2][3]) > 1e-6f ||
		std::abs(instance.meshToWorld[3][3] - 1.0f) > 1e-6f)
		return false;

	const glm::mat3 localToWorld(instance.meshToWorld);
	const float determinant = glm::determinant(localToWorld);
	if (!std::isfinite(determinant) || std::abs(determinant) < 1e-10f)
		return false;
	const glm::mat3 worldToLocal = glm::inverse(localToWorld);
	const glm::vec3 translation(instance.meshToWorld[3]);
	const glm::vec3 lo = worldToLocal * (origin - translation);
	const glm::vec3 ld = worldToLocal * direction;
	const float localSpeed = glm::length(ld); // local units per world metre
	if (!Finite(lo) || !Finite(ld) || !(localSpeed > 1e-10f) ||
		!std::isfinite(localSpeed)) return false;

	const SdfVolume& v = *instance.volume;
	float enter = 0.0f, exit = maxDistance;
	const glm::vec3 minCorner = v.origin;
	const glm::vec3 maxCorner = v.origin +
		glm::vec3(float(v.resolution - 1) * v.voxelSize);
	if (!RayBox(lo, ld, minCorner, maxCorner, enter, exit))
		return false;

	// Sphere trace in mesh space; t, enter, exit and the winning hit
	// stay in WORLD metres. Dividing by |worldToLocal * rayDirection|
	// is the safe change-of-units for any affine scale, including
	// nonuniform transforms. Short steps hedge tri-linear interpolation
	// error and the finite resolution of the narrow-band samples.
	// The int16-quantized, trilinearly interpolated field resolves a
	// surface no finer than a conservative fraction of ONE VOXEL. The
	// true surface can fall between voxel planes (half-voxel floor),
	// and near-diagonal geometry mixes off-surface corner taps into
	// the interpolated value - observed ~0.6*voxelSize along an
	// open-triangle edge trace. A tighter epsilon floor makes
	// convergence unreachable: the tracer steps through the flat zone
	// and reports a miss (the open-triangle trace in test_sdf). The
	// 0.75*voxelSize floor keeps every hit within three quarters of a
	// voxel of the true surface.
	const float localEpsilon = std::max(options.hitEpsilon * localSpeed,
		0.75f * v.voxelSize);
	float t = enter;
	for (uint32_t step = 0; step < options.maxSteps && t <= exit; ++step) {
		const glm::vec3 p = lo + t * ld;
		const float distance = std::abs(Sample(v, p));
		if (distance <= localEpsilon) {
			glm::vec3 g = glm::transpose(worldToLocal) * Gradient(v, p);
			const float len = glm::length(g);
			if (!Finite(g) || !(len > 1e-10f))
				g = -direction; // plateau / degenerate interpolated normal
			else
				g /= len;
			if (glm::dot(g, direction) > 0.0f) g = -g;
			hitDistance = t;
			hitNormal = g;
			return true;
		}
		// Never jump by the full decoded SDF distance: the source is
		// quantized, tri-linearly interpolated, and narrow-band clamped.
		const float safeLocalStep = std::max(
			(distance - 0.25f * v.voxelSize) * 0.75f,
			0.25f * localEpsilon);
		t += safeLocalStep / localSpeed;
	}
	return false;
}

} // namespace

bool TraceStaticSdfRay(std::span<const StaticSdfInstance> instances,
	const glm::vec3& worldOrigin, const glm::vec3& worldDirection,
	const StaticSdfTraceOptions& options, StaticSdfHit& hit) {
	if (!Valid(options) || !Finite(worldOrigin) || !Finite(worldDirection))
		return false;
	const float directionLength = glm::length(worldDirection);
	if (!(directionLength > 1e-10f) || !std::isfinite(directionLength))
		return false;
	const glm::vec3 direction = worldDirection / directionLength;
	float best = options.maxDistance;
	StaticSdfHit closest{};
	bool found = false;
	for (size_t i = 0; i < instances.size(); ++i) {
		float distance = 0.0f;
		glm::vec3 normal(0.0f);
		if (!TraceOne(instances[i], worldOrigin, direction,
			options, best, distance, normal)) continue;
		if (!found || distance < best) {
			best = distance;
			closest.instanceIndex = i;
			closest.distance = distance;
			closest.position = worldOrigin + direction * distance;
			closest.normal = normal;
			found = true;
		}
	}
	if (found) hit = closest;
	return found;
}

bool TraceStaticSdfReflection(std::span<const StaticSdfInstance> instances,
	const glm::vec3& receiverWorldPosition,
	const glm::vec3& incidentWorldDirection,
	const glm::vec3& receiverWorldNormal,
	const StaticSdfTraceOptions& options, StaticSdfHit& hit) {
	if (!Valid(options) || !Finite(receiverWorldPosition) ||
		!Finite(receiverWorldNormal) || !Finite(incidentWorldDirection))
		return false;
	const float nl = glm::length(receiverWorldNormal);
	const float il = glm::length(incidentWorldDirection);
	if (!(nl > 1e-10f) || !(il > 1e-10f) ||
		!std::isfinite(nl) || !std::isfinite(il)) return false;
	const glm::vec3 reflected = glm::reflect(incidentWorldDirection / il,
		receiverWorldNormal / nl);
	const float bias = std::max(options.originBias, options.hitEpsilon * 2.0f);
	if (bias >= options.maxDistance) return false;
	StaticSdfTraceOptions remaining = options;
	remaining.maxDistance -= bias;
	StaticSdfHit candidate{};
	if (!TraceStaticSdfRay(instances, receiverWorldPosition + reflected * bias,
		reflected, remaining, candidate)) return false;
	candidate.distance += bias;
	// Report distance from the RECEIVER, not from the biased ray origin.
	candidate.position = receiverWorldPosition + reflected * candidate.distance;
	hit = candidate;
	return true;
}

} // namespace tour::asset
