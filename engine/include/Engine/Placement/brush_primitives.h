// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Reusable, deterministic brush geometry for terrain, instance and particle
// authoring. A brush produces samples; the consumer owns the edit command,
// undo/redo, material/particle payload and persistence. No GPU/UI dependency.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace tour::placement::brush {

enum class Shape : uint8_t { Disk, Ball, SphereShell, Box };
enum class Falloff : uint8_t { Constant, Linear, Smooth };
struct Settings {
	Shape shape = Shape::Ball;
	Falloff falloff = Falloff::Smooth;
	float radius = 1.0f;
	float strength = 1.0f;
	float spacing = 0.25f;  // fraction of radius between drag dabs
	float density = 8.0f;   // samples per cubic/square-meter-equivalent dab
	float jitter = 0.0f;    // 0..1, displacement relative to radius
	uint32_t seed = 1;
};
struct Sample {
	glm::vec3 position{};
	glm::vec3 normal{0, 1, 0};
	float weight = 1.0f;
};

// Stable hash; avoids mutable global RNG state and per-platform distributions.
inline uint32_t Hash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	return x ^ (x >> 16);
}
inline float Unit(uint32_t seed, uint32_t index, uint32_t channel) {
	return float(Hash(seed ^ Hash(index * 0x9e3779b9u + channel))
		>> 8) * (1.0f / 16777216.0f);
}
inline float Weight(float normalizedDistance, Falloff f) {
	const float t = std::clamp(1.0f - normalizedDistance, 0.0f, 1.0f);
	switch (f) {
	case Falloff::Constant: return t > 0.0f ? 1.0f : 0.0f;
	case Falloff::Linear: return t;
	case Falloff::Smooth: return t * t * (3.0f - 2.0f * t);
	}
	return 0.0f;
}
inline float DabSpacing(const Settings& s) {
	return std::max(0.001f, std::max(0.001f, s.radius)
		* std::clamp(s.spacing, 0.01f, 4.0f));
}
inline glm::vec3 Direction(uint32_t seed, uint32_t i) {
	const float z = 2.0f * Unit(seed, i, 0) - 1.0f;
	const float a = 6.28318530718f * Unit(seed, i, 1);
	const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
	return {r * std::cos(a), z, r * std::sin(a)};
}
// Generate an independent dab. A Ball scatters throughout its volume;
// SphereShell scatters on its surface; Disk uses the XZ plane. Consumers
// may project samples to hit surfaces or use them as particle emitters.
inline std::vector<Sample> Scatter(const Settings& settings,
	const glm::vec3& center, uint32_t strokeId, uint32_t dabIndex,
	uint32_t maxSamples = 65536) {
	const float radius = std::max(0.001f, settings.radius);
	const float measure = settings.shape == Shape::Disk
		? 3.14159265359f * radius * radius
		: settings.shape == Shape::SphereShell
			? 4.0f * 3.14159265359f * radius * radius
			: (4.0f / 3.0f) * 3.14159265359f * radius * radius * radius;
	const float requested = std::clamp(measure * std::max(0.0f, settings.density),
		0.0f, float(maxSamples));
	const uint32_t count = uint32_t(requested);
	const uint32_t seed = Hash(settings.seed ^ Hash(strokeId)
		^ Hash(dabIndex + 0x6d2b79f5u));
	std::vector<Sample> out;
	out.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		const glm::vec3 direction = Direction(seed, i);
		glm::vec3 offset{};
		glm::vec3 normal = direction;
		switch (settings.shape) {
		case Shape::Disk: {
			const float angle = 6.28318530718f * Unit(seed, i, 2);
			const float r = std::sqrt(Unit(seed, i, 3));
			offset = radius * r * glm::vec3(std::cos(angle), 0, std::sin(angle));
			normal = {0, 1, 0};
			break;
		}
		case Shape::Ball:
			offset = radius * std::cbrt(Unit(seed, i, 2)) * direction;
			break;
		case Shape::SphereShell:
			offset = radius * direction;
			break;
		case Shape::Box:
			offset = radius * glm::vec3(
			2 * Unit(seed, i, 2) - 1, 2 * Unit(seed, i, 3) - 1,
			2 * Unit(seed, i, 4) - 1);
			break;
		}
		const float j = std::clamp(settings.jitter, 0.0f, 1.0f);
		if (j > 0.0f)
			offset += direction * (radius * j * (Unit(seed, i, 5) - 0.5f));
		const float d = glm::length(offset) / radius;
		const float weight = settings.shape == Shape::SphereShell
			? std::clamp(settings.strength, 0.0f, 1.0f)
			: Weight(d, settings.falloff) * std::max(0.0f, settings.strength);
		out.push_back({center + offset, normal, weight});
	}
	return out;
}
// Regular, arc-length-spaced dabs along a stroke, including its end.
// The dab number can be used as the deterministic Scatter dabIndex.
template<class Callback>
inline void Stroke(const glm::vec3& from, const glm::vec3& to,
	const Settings& settings, Callback&& emit) {
	const float distance = glm::length(to - from);
	const uint32_t steps = std::max(1u, uint32_t(std::ceil(
		distance / DabSpacing(settings))));
	for (uint32_t i = 0; i <= steps; ++i)
		emit(glm::mix(from, to, float(i) / float(steps)), i);
}
} // namespace tour::placement::brush
