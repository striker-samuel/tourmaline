// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/static_sdf_gpu.h>

#include <Engine/App/world.h>
#include <Engine/Asset/sdf.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace smvk {
namespace {

bool ValidTransform(const glm::mat4& m) {
	for (uint32_t column = 0; column < 4; ++column)
		for (uint32_t row = 0; row < 4; ++row)
			if (!std::isfinite(m[column][row])) return false;
	if (std::abs(m[0][3]) > 1e-5f ||
		std::abs(m[1][3]) > 1e-5f ||
		std::abs(m[2][3]) > 1e-5f ||
		std::abs(m[3][3] - 1.0f) > 1e-5f) return false;
	const float det = glm::determinant(glm::mat3(m));
	return std::isfinite(det) && std::abs(det) > 1e-10f;
}

bool ValidVolume(const tour::asset::SdfVolume& v) {
	const size_t r = v.resolution;
	return r >= 8 && r <= 128 && v.samples.size() == r * r * r &&
		std::isfinite(v.voxelSize) && v.voxelSize > 0.0f &&
		std::isfinite(v.maxDistance) && v.maxDistance > 0.0f &&
		std::isfinite(v.origin.x) &&
		std::isfinite(v.origin.y) && std::isfinite(v.origin.z);
}

} // namespace

std::vector<uint32_t> PackStaticSdfWords(std::span<const int16_t> samples) {
	std::vector<uint32_t> packed;
	packed.reserve((samples.size() + 1) / 2);
	for (size_t i = 0; i < samples.size(); i += 2) {
		const uint32_t lo = uint16_t(samples[i]);
		const uint32_t hi = (i + 1 < samples.size())
			? uint32_t(uint16_t(samples[i + 1])) << 16 : 0u;
		packed.push_back(lo | hi);
	}
	return packed;
}

void StaticSdfGpuScene::Clear() {
	m_header = {};
	m_instances = {};
	m_samples = {};
	m_instanceCount = 0;
	m_world = nullptr;
	m_worldRevision = std::numeric_limits<uint64_t>::max();
	m_cachedEyeValid = false;
}

uint64_t StaticSdfGpuScene::SceneAddress(ResourceManager& ctx) const {
	return Ready() ? ctx.GetBufferDeviceAddress(m_header) : 0;
}

bool StaticSdfGpuScene::Update(ResourceManager& ctx, const tour::World& world,
	const glm::vec3& cameraWorldPos) {
	// Keep a bounded set near the camera instead of the first 128
	// material-sorted scene items. A frozen world only needs republishing
	// after a topology edit or an 8 m camera-cell move.
	const bool eyeFinite = std::isfinite(cameraWorldPos.x) &&
		std::isfinite(cameraWorldPos.y) && std::isfinite(cameraWorldPos.z);
	if (m_world == &world && m_worldRevision == world.gpuSceneTopologyRevision &&
		m_cachedEyeValid && eyeFinite &&
		glm::distance(m_cachedEye, cameraWorldPos) < 8.0f)
		return Ready();

	// De-duplicate SDF grids across instanced static meshes. Each sidecar
	// groups all base primitives in one .geometry family; fragment names
	// match the suffix of the cooked mesh URI (before optional /lodN).
	std::unordered_map<std::string, tour::asset::SdfAsset> families;
	std::unordered_map<std::string, uint32_t> wordOffsets;
	std::vector<StaticSdfGpuInstance> instances;
	std::vector<uint32_t> words;
	instances.reserve(std::min(world.drawItems.size(),
		size_t(kStaticSdfMaxInstances)));

	// Deterministic closest-first upload order. Selecting from a scene's
	// material-sorted draw list would often fill the cap with invisible
	// distant geometry before any nearby reflective object is reached.
	std::vector<std::pair<float, const tour::World::RenderItem*>> candidates;
	for (const auto& item : world.drawItems) {
		if ((item.flags & (tour::IteratorFlagBit_Static |
			tour::IteratorFlagBit_Opaque | tour::IteratorFlagBit_StaticVertexFmt))
			!= (tour::IteratorFlagBit_Static |
				tour::IteratorFlagBit_Opaque | tour::IteratorFlagBit_StaticVertexFmt) ||
			(item.flags & tour::IteratorFlagBit_Masked) != 0)
			continue;
		if (item.node >= world.scene.globalTransform.size()) continue;
		const StaticMesh* mesh = world.GetMesh(item.mesh);
		if (!mesh || mesh->skinned) continue;
		const glm::vec3 localCenter = (mesh->geometry.aabbMin +
			mesh->geometry.aabbMax) * 0.5f;
		const glm::vec3 worldCenter = glm::vec3(
			world.scene.globalTransform[item.node] * glm::vec4(localCenter, 1.0f));
		const float distance2 = glm::dot(worldCenter - cameraWorldPos,
			worldCenter - cameraWorldPos);
		candidates.emplace_back(std::isfinite(distance2) ?
			distance2 : std::numeric_limits<float>::max(), &item);
	}
	std::sort(candidates.begin(), candidates.end(),
		[](const auto& a, const auto& b) {
			if (a.first != b.first) return a.first < b.first;
			return a.second->node < b.second->node;
		});
	for (const auto& [distance2, itemPtr] : candidates) {
		if (instances.size() >= kStaticSdfMaxInstances)
			break;
		const auto& item = *itemPtr;
		if (item.node >= world.scene.globalTransform.size() ||
			item.mesh >= world.meshLocations.size()) continue;
		const StaticMesh* mesh = world.GetMesh(item.mesh);
		if (!mesh || mesh->skinned) continue;

		const auto& loc = world.meshLocations[item.mesh];
		if (!loc.cluster) continue;
		const std::string uri = loc.cluster->GetMeshUri(loc.handle).Str();
		const size_t hash = uri.find('#');
		if (hash == std::string::npos || hash + 1 >= uri.size())
			continue;
		const std::string family = uri.substr(0, hash);
		std::string fragment = uri.substr(hash + 1);
		// LOD siblings may use a /lodN URI suffix, but the v1 TSDF
		// sidecar is keyed by the base primitive fragment only.
		const size_t lodSuffix = fragment.rfind("/lod");
		if (lodSuffix != std::string::npos &&
			lodSuffix + 4 < fragment.size() &&
			std::all_of(fragment.begin() + lodSuffix + 4, fragment.end(),
				[](char c) { return c >= '0' && c <= '9'; }))
			fragment.resize(lodSuffix);
		auto familyIt = families.find(family);
		if (familyIt == families.end()) {
			tour::asset::SdfAsset decoded;
			std::string error;
			const auto bytes = world.ReadBakeBytes(family + ".sdf");
			if (!bytes.empty() && !tour::asset::ParseSdfAsset(
				bytes.data(), bytes.size(), decoded, error))
				std::fprintf(stderr,
					"[StaticSdf] invalid sidecar '%s.sdf': %s\n",
					family.c_str(), error.c_str());
			familyIt = families.emplace(family, std::move(decoded)).first;
		}
		const auto it = std::find_if(familyIt->second.meshes.begin(),
			familyIt->second.meshes.end(), [&](const tour::asset::SdfMesh& m) {
				return m.fragment == fragment;
			});
		if (it == familyIt->second.meshes.end() || !ValidVolume(it->volume))
			continue;
		const glm::mat4& model = world.scene.globalTransform[item.node];
		if (!ValidTransform(model)) continue;

		uint32_t wordOffset = 0;
		if (const auto found = wordOffsets.find(uri);
			found != wordOffsets.end()) {
			wordOffset = found->second;
		} else {
			const auto& v = it->volume;
			const size_t newWords = (v.samples.size() + 1u) / 2u;
			if (newWords > kStaticSdfMaxSampleWords - words.size())
				continue;
			wordOffset = uint32_t(words.size());
			const auto packed = PackStaticSdfWords(v.samples);
			words.insert(words.end(), packed.begin(), packed.end());
			wordOffsets.emplace(uri, wordOffset);
		}

		const glm::mat4 inverse = glm::inverse(model);
		StaticSdfGpuInstance gpu{};
		gpu.inverseRow0 = {inverse[0][0], inverse[1][0],
			inverse[2][0], inverse[3][0]};
		gpu.inverseRow1 = {inverse[0][1], inverse[1][1],
			inverse[2][1], inverse[3][1]};
		gpu.inverseRow2 = {inverse[0][2], inverse[1][2],
			inverse[2][2], inverse[3][2]};
		const auto& v = it->volume;
		gpu.originVoxel = glm::vec4(v.origin, v.voxelSize);
		gpu.surface = glm::vec4(0.7f, 0.7f, 0.7f, 0.5f);
		if (const Material* material = world.GetMaterial(item.material)) {
			gpu.surface = material->values.diffuse;
			gpu.surface.w = std::clamp(material->values.params.z, 0.0f, 1.0f);
		}
		gpu.meta = {v.resolution, wordOffset, v.signedDistance ? 1u : 0u,
			std::bit_cast<uint32_t>(v.maxDistance)};
		instances.push_back(gpu);
	}

	// A failed build must not leave stale buffers from a previous world.
	Holder<BufferHandle> headerBuffer, instanceBuffer, sampleBuffer;
	if (!instances.empty()) {
		instanceBuffer = ctx.CreateBuffer(
			instances.size() * sizeof(StaticSdfGpuInstance),
			BufferType_Storage, "Static SDF reflection instances");
		sampleBuffer = ctx.CreateBuffer(
			words.size() * sizeof(uint32_t),
			BufferType_Storage, "Static SDF reflection samples");
		headerBuffer = ctx.CreateBuffer(sizeof(StaticSdfSceneHeader),
			BufferType_Storage, "Static SDF reflection descriptor");
		if (instanceBuffer.empty() || sampleBuffer.empty() ||
			headerBuffer.empty()) {
			Clear();
			m_world = &world;
			m_worldRevision = world.gpuSceneTopologyRevision;
			m_cachedEye = cameraWorldPos;
			m_cachedEyeValid = eyeFinite;
			return false;
		}
		ctx.UploadDataToBuffer(instanceBuffer,
			instances.data(), instances.size() * sizeof(StaticSdfGpuInstance),
			PipelineStageBit_Compute, AccessFlags::ShaderRead);
		ctx.UploadDataToBuffer(sampleBuffer,
			words.data(), words.size() * sizeof(uint32_t),
			PipelineStageBit_Compute, AccessFlags::ShaderRead);
		StaticSdfSceneHeader gpuHeader{};
		gpuHeader.instancesAddress = ctx.GetBufferDeviceAddress(instanceBuffer);
		gpuHeader.samplesAddress = ctx.GetBufferDeviceAddress(sampleBuffer);
		gpuHeader.instanceCount = uint32_t(instances.size());
		ctx.UploadDataToBuffer(headerBuffer, &gpuHeader, sizeof(gpuHeader),
			PipelineStageBit_Compute, AccessFlags::ShaderRead);
	}
	m_header = std::move(headerBuffer);
	m_instances = std::move(instanceBuffer);
	m_samples = std::move(sampleBuffer);
	m_instanceCount = uint32_t(instances.size());
	m_world = &world;
	m_worldRevision = world.gpuSceneTopologyRevision;
	m_cachedEye = cameraWorldPos;
	m_cachedEyeValid = eyeFinite;
	if (m_instanceCount)
		std::fprintf(stderr,
			"[StaticSdf] GPU resident: %u frozen opaque instances, %zu packed words (%.2f MiB)\n",
			m_instanceCount, words.size(),
			double(words.size() * sizeof(uint32_t)) / (1024.0 * 1024.0));
	return Ready();
}

} // namespace smvk
