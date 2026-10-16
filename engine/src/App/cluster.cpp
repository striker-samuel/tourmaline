// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/core.h>
#include <Engine/App/cluster.h>
#include <Engine/Renderer/vertex_pack.h>
#include "../Vulkan/detail/vulkan_impl.h"


smvk::Cluster::~Cluster() {

	staticMeshes.forEachHandle([&](ClusterStaticMeshHandle han) {
		DestroyMesh(han);
	});

	assert(meshAllocations_.empty());

	// Materials are just pods
	materials.clear();

	// textures own holders, when cleared holders go.
	textures.clear();
}

smvk::ClusterStaticMeshHandle smvk::Cluster::UploadStaticMesh(
	const StaticMeshUpload& data)
{
	const uint32_t vertexCount =
		static_cast<uint32_t>(data.vertices.size());

	const uint32_t indexCount =
		static_cast<uint32_t>(data.indices.size());


	if (vertexCount == 0)
		return {};

	// GPU-free offline mode: CPU geometry only (no VMA/streamer). Mirrors the
	// runtime single-surface layout so the serializer sees the same shape.
	if (offlineMode_) {
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = vertexCount;
		mesh.geometry.indexCount = indexCount;
		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;
		mesh.geometry.sphereRadius =
			glm::length(data.aabbMax - data.aabbMin) * 0.5f;
		Surface surface{};
		surface.firstIndex = 0;
		surface.indexCount = indexCount;
		surface.baseVertex = 0;
		mesh.geometry.surfaces.push_back(surface);
		const ClusterStaticMeshHandle handle =
			staticMeshes.create(std::move(mesh));
		if (retainUploads_)
			retainedUploads_[handle.index()] = data;
		return handle;
	}

	StaticMesh result{};
	StaticMeshAllocation alloc{};


	// =========================================================================
	// 1. Allocate vertex range
	//
	// vertexBase is in VERTEX UNITS, not bytes.
	// =========================================================================

	VkDeviceSize vertexBase = 0;

	{
		VmaVirtualAllocationCreateInfo allocInfo{};
		allocInfo.size = vertexCount;
		allocInfo.alignment = 1;

		const VkResult res = vmaVirtualAllocate(
			staticVertexFmtHeap.m_staticVertexBlock,
			&allocInfo,
			&alloc.vertexAllocation,
			&vertexBase
		);

		if (res != VK_SUCCESS) {
			return {};
		}
	}


	// =========================================================================
	// 2. Allocate index range
	// =========================================================================

	const VkDeviceSize indexBytes =
		sizeof(uint32_t) *
		static_cast<VkDeviceSize>(indexCount);

	VkDeviceSize indexByteOffset = 0;

	if (indexCount > 0) {

		VmaVirtualAllocationCreateInfo allocInfo{};
		allocInfo.size = indexBytes;
		allocInfo.alignment = alignof(uint32_t);

		const VkResult res = vmaVirtualAllocate(
			staticVertexFmtHeap.m_staticIndexBlock,
			&allocInfo,
			&alloc.indexAllocation,
			&indexByteOffset
		);

		if (res != VK_SUCCESS) {

			vmaVirtualFree(
				staticVertexFmtHeap.m_staticVertexBlock,
				alloc.vertexAllocation
			);

			return {};
		}
	}


	// =========================================================================
	// 3. Convert AoS -> SoA-ish GPU streams
	// =========================================================================

	std::vector<VertexPos> positions(vertexCount);
	std::vector<VertexShading> shading(vertexCount);

	for (uint32_t i = 0; i < vertexCount; ++i) {

		const StaticVertex& src = data.vertices[i];

		positions[i].pos = src.pos;

		shading[i].color = vertexpack::PackColorUnorm8(src.color);
		shading[i].uv = vertexpack::PackUvHalf(src.texCoord);
		shading[i].chartUv = vertexpack::PackChartUv(src.texCoord2);
		shading[i].chart = src.chartId;
		shading[i].normal = vertexpack::PackNormalOct(src.normal);
		shading[i].tangent = vertexpack::PackTangentOct(
			glm::vec3(src.tangent), src.tangent.w);
	}


	// =========================================================================
	// 4. Upload position stream
	// =========================================================================

	const VkDeviceSize positionOffset =
		vertexBase * sizeof(VertexPos);

	const VkDeviceSize positionBytes =
		vertexCount * sizeof(VertexPos);

	static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque())->UploadDataToBuffer(
		staticVertexFmtHeap.m_staticVertexPosBuffer,
		positions.data(),
		positionBytes,

		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,

		0,
		positionOffset
	);


	// =========================================================================
	// 5. Upload shading stream
	// =========================================================================

	const VkDeviceSize shadingOffset =
		vertexBase * sizeof(VertexShading);

	const VkDeviceSize shadingBytes =
		vertexCount * sizeof(VertexShading);

	static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque())->UploadDataToBuffer(
		staticVertexFmtHeap.m_staticVertexShadingBuffer,
		shading.data(),
		shadingBytes,

		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,

		0,
		shadingOffset
	);


	// =========================================================================
	// 6. Upload indices
	// =========================================================================

	if (indexCount > 0) {

		static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque())->UploadDataToBuffer(
			staticVertexFmtHeap.m_staticIndexBuffer,
			data.indices.data(),
			indexBytes,

			VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
			VK_ACCESS_2_INDEX_READ_BIT,

			0,
			indexByteOffset
		);
	}


	// =========================================================================
	// 7. Construct mesh
	// =========================================================================

	StaticMesh& mesh = result;

	mesh.geometry.topology =
		PrimitiveTopology::TriangleList;

	mesh.geometry.vertexCount = vertexCount;
	mesh.geometry.indexCount = indexCount;

	mesh.geometry.aabbMin = data.aabbMin;
	mesh.geometry.aabbMax = data.aabbMax;


	Surface surface{};

	surface.firstIndex =
		static_cast<uint32_t>(
			indexByteOffset / sizeof(uint32_t)
			);

	surface.indexCount = indexCount;

	surface.baseVertex =
		static_cast<int32_t>(vertexBase);

	mesh.geometry.surfaces.push_back(surface);


	const ClusterStaticMeshHandle handle =
		staticMeshes.create(std::move(result));

	if (handle.empty()) {
		if (alloc.indexAllocation) {
			vmaVirtualFree(
				staticVertexFmtHeap.m_staticIndexBlock,
				alloc.indexAllocation);
		}
		vmaVirtualFree(
			staticVertexFmtHeap.m_staticVertexBlock,
			alloc.vertexAllocation);
		return {};
	}

	auto [it, inserted] = meshAllocations_.emplace(
		handle.index(),
		new StaticMeshAllocation(std::move(alloc)));
	assert(inserted);

	if (retainUploads_)
		retainedUploads_[handle.index()] = data;

	return handle;
}


std::vector<smvk::ClusterStaticMeshHandle> smvk::Cluster::UploadStaticMeshes(
	std::span<const StaticMeshUpload> uploads)
{
	std::vector<ClusterStaticMeshHandle> handles;
	if (uploads.empty())
		return handles;

	// GPU-free offline mode: CPU geometry only — per-mesh single path (the
	// batch win is GPU-side staging, which offline mode never touches).
	if (offlineMode_) {
		handles.reserve(uploads.size());
		for (const StaticMeshUpload& data : uploads)
			handles.push_back(UploadStaticMesh(data));
		return handles;
	}

	// =========================================================================
	// 1. Allocate heap ranges (per mesh; all-or-nothing — a degenerate mesh
	//    or a full heap fails the batch with NOTHING staged to the GPU)
	// =========================================================================

	struct MeshRange {
		VkDeviceSize vertexBase = 0; // VERTEX UNITS, not bytes
		VkDeviceSize indexByteOffset = 0;
		StaticMeshAllocation alloc{};
	};
	std::vector<MeshRange> ranges(uploads.size());
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		if (vertexCount == 0)
			return {}; // degenerate entry fails the whole batch
		MeshRange& r = ranges[i];
		{
			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = vertexCount;
			allocInfo.alignment = 1;
			if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticVertexBlock,
					&allocInfo, &r.alloc.vertexAllocation,
					&r.vertexBase) != VK_SUCCESS)
				return {};
		}
		if (indexCount > 0) {
			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = sizeof(uint32_t)
				* static_cast<VkDeviceSize>(indexCount);
			allocInfo.alignment = alignof(uint32_t);
			if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticIndexBlock,
					&allocInfo, &r.alloc.indexAllocation,
					&r.indexByteOffset) != VK_SUCCESS)
				return {};
		}
	}

	// =========================================================================
	// 2. Pool entries + bookkeeping (BEFORE any GPU staging: a failure here
	//    leaves nothing queued in the upload streamer)
	// =========================================================================

	handles.resize(uploads.size());
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		const MeshRange& r = ranges[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = vertexCount;
		mesh.geometry.indexCount = indexCount;
		mesh.geometry.aabbMin = data.aabbMin;
		mesh.geometry.aabbMax = data.aabbMax;
		Surface surface{};
		surface.firstIndex = static_cast<uint32_t>(
			r.indexByteOffset / sizeof(uint32_t));
		surface.indexCount = indexCount;
		surface.baseVertex = static_cast<int32_t>(r.vertexBase);
		mesh.geometry.surfaces.push_back(surface);
		handles[i] = staticMeshes.create(std::move(mesh));
		if (handles[i].empty()) {
			// Roll the created entries back (frees their heap ranges via
			// meshAllocations_), then the failed entry's own fresh ranges.
			for (size_t j = 0; j < i; ++j)
				DestroyMesh(handles[j]);
			if (r.alloc.indexAllocation) {
				vmaVirtualFree(staticVertexFmtHeap.m_staticIndexBlock,
					r.alloc.indexAllocation);
			}
			vmaVirtualFree(staticVertexFmtHeap.m_staticVertexBlock,
				r.alloc.vertexAllocation);
			return {};
		}
		auto [it, inserted] = meshAllocations_.emplace(
			handles[i].index(),
			new StaticMeshAllocation(std::move(r.alloc)));
		assert(inserted);
		if (retainUploads_)
			retainedUploads_[handles[i].index()] = data;
	}

	// =========================================================================
	// 3. Convert AoS -> SoA-ish into ONE packed blob per heap stream
	// =========================================================================

	size_t totalVerts = 0;
	size_t totalIdx = 0;
	for (const StaticMeshUpload& data : uploads) {
		totalVerts += data.vertices.size();
		totalIdx += data.indices.size();
	}
	std::vector<VertexPos> positions(totalVerts);
	std::vector<VertexShading> shading(totalVerts);
	std::vector<uint32_t> indices(totalIdx);
	std::vector<BufferBulkCopy> posCopies;
	std::vector<BufferBulkCopy> shadeCopies;
	std::vector<BufferBulkCopy> idxCopies;
	posCopies.reserve(uploads.size());
	shadeCopies.reserve(uploads.size());
	idxCopies.reserve(uploads.size());
	size_t srcVert = 0;
	size_t srcIdx = 0;
	for (size_t i = 0; i < uploads.size(); ++i) {
		const StaticMeshUpload& data = uploads[i];
		const MeshRange& r = ranges[i];
		const uint32_t vertexCount =
			static_cast<uint32_t>(data.vertices.size());
		const uint32_t indexCount =
			static_cast<uint32_t>(data.indices.size());
		VertexPos* posOut = positions.data() + srcVert;
		VertexShading* shadeOut = shading.data() + srcVert;
		for (uint32_t v = 0; v < vertexCount; ++v)
			PackStaticVertex(data.vertices[v], posOut[v], shadeOut[v]);
		if (indexCount > 0) {
			memcpy(indices.data() + srcIdx, data.indices.data(),
				size_t(indexCount) * sizeof(uint32_t));
			idxCopies.push_back(BufferBulkCopy{
				srcIdx * sizeof(uint32_t),
				size_t(r.indexByteOffset),
				size_t(indexCount) * sizeof(uint32_t) });
		}
		posCopies.push_back(BufferBulkCopy{
			srcVert * sizeof(VertexPos),
			size_t(r.vertexBase) * sizeof(VertexPos),
			size_t(vertexCount) * sizeof(VertexPos) });
		shadeCopies.push_back(BufferBulkCopy{
			srcVert * sizeof(VertexShading),
			size_t(r.vertexBase) * sizeof(VertexShading),
			size_t(vertexCount) * sizeof(VertexShading) });
		srcVert += vertexCount;
		srcIdx += indexCount;
	}

	// =========================================================================
	// 4. Three table-driven bulk uploads (one per heap; the RHI stages 512MB
	//    blobs — one memcpy pass + one submit + one barrier set per blob)
	// =========================================================================

	auto* vctx = static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque());
	vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
		positions.data(), posCopies,
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
		shading.data(), shadeCopies,
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	if (!idxCopies.empty()) {
		vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
			indices.data(), idxCopies,
			VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
			VK_ACCESS_2_INDEX_READ_BIT);
	}
	return handles;
}

std::vector<smvk::ClusterStaticMeshHandle> smvk::Cluster::UploadStaticMeshesDirect(
	const void* content, std::span<const StaticMeshDirectSource> sources)
{
	std::vector<ClusterStaticMeshHandle> handles;
	if (!content || sources.empty())
		return handles;
	assert(!offlineMode_ && "UploadStaticMeshesDirect is a GPU-context fast path");

	// =========================================================================
	// 1. Allocate heap ranges (all-or-nothing — nothing staged on failure)
	// =========================================================================
	struct MeshRange {
		VkDeviceSize vertexBase = 0; // VERTEX UNITS
		VkDeviceSize indexByteOffset = 0;
		StaticMeshAllocation alloc{};
	};
	std::vector<MeshRange> ranges(sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		if (src.vertexCount == 0)
			return {}; // degenerate entry fails the whole batch
		MeshRange& r = ranges[i];
		{
			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = src.vertexCount;
			allocInfo.alignment = 1;
			if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticVertexBlock,
					&allocInfo, &r.alloc.vertexAllocation,
					&r.vertexBase) != VK_SUCCESS)
				return {};
		}
		if (src.indexCount > 0) {
			VmaVirtualAllocationCreateInfo allocInfo{};
			allocInfo.size = sizeof(uint32_t)
				* static_cast<VkDeviceSize>(src.indexCount);
			allocInfo.alignment = alignof(uint32_t);
			if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticIndexBlock,
					&allocInfo, &r.alloc.indexAllocation,
					&r.indexByteOffset) != VK_SUCCESS)
				return {};
		}
	}

	// =========================================================================
	// 2. Pool entries + bookkeeping (BEFORE any GPU staging)
	// =========================================================================
	handles.resize(sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		const MeshRange& r = ranges[i];
		StaticMesh mesh{};
		mesh.geometry.topology = PrimitiveTopology::TriangleList;
		mesh.geometry.vertexCount = src.vertexCount;
		mesh.geometry.indexCount = src.indexCount;
		mesh.geometry.aabbMin = src.aabbMin;
		mesh.geometry.aabbMax = src.aabbMax;
		Surface surface{};
		surface.firstIndex = static_cast<uint32_t>(
			r.indexByteOffset / sizeof(uint32_t));
		surface.indexCount = src.indexCount;
		surface.baseVertex = static_cast<int32_t>(r.vertexBase);
		mesh.geometry.surfaces.push_back(surface);
		handles[i] = staticMeshes.create(std::move(mesh));
		if (handles[i].empty()) {
			for (size_t j = 0; j < i; ++j)
				DestroyMesh(handles[j]);
			if (r.alloc.indexAllocation) {
				vmaVirtualFree(staticVertexFmtHeap.m_staticIndexBlock,
					r.alloc.indexAllocation);
			}
			vmaVirtualFree(staticVertexFmtHeap.m_staticVertexBlock,
				r.alloc.vertexAllocation);
			return {};
		}
		auto [it, inserted] = meshAllocations_.emplace(
			handles[i].index(),
			new StaticMeshAllocation(std::move(r.alloc)));
		assert(inserted);
	}

	// =========================================================================
	// 3. THREE table-driven bulk uploads straight from `content` — the
	//    sources are packed-SoA subranges of one blob (the loaded pak
	//    image), so this is pure offset math: no AoS parse, no conversion,
	//    no intermediate vectors.
	// =========================================================================
	std::vector<BufferBulkCopy> posCopies;
	std::vector<BufferBulkCopy> shadeCopies;
	std::vector<BufferBulkCopy> idxCopies;
	posCopies.reserve(sources.size());
	shadeCopies.reserve(sources.size());
	idxCopies.reserve(sources.size());
	for (size_t i = 0; i < sources.size(); ++i) {
		const StaticMeshDirectSource& src = sources[i];
		const MeshRange& r = ranges[i];
		posCopies.push_back(BufferBulkCopy{
			size_t(src.posSrc),
			size_t(r.vertexBase) * sizeof(VertexPos),
			size_t(src.vertexCount) * sizeof(VertexPos) });
		shadeCopies.push_back(BufferBulkCopy{
			size_t(src.shadeSrc),
			size_t(r.vertexBase) * sizeof(VertexShading),
			size_t(src.vertexCount) * sizeof(VertexShading) });
		if (src.indexCount > 0) {
			idxCopies.push_back(BufferBulkCopy{
				size_t(src.idxSrc),
				size_t(r.indexByteOffset),
				size_t(src.indexCount) * sizeof(uint32_t) });
		}
	}
	auto* vctx = static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque());
	vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
		content, posCopies,
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
		content, shadeCopies,
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	if (!idxCopies.empty()) {
		vctx->BulkCopyBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
			content, idxCopies,
			VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
			VK_ACCESS_2_INDEX_READ_BIT);
	}
	return handles;
}


// -----------------------------------------------------------------------------
// Mesh destruction
// -----------------------------------------------------------------------------

void smvk::Cluster::DestroyMesh(
	smvk::ClusterStaticMeshHandle handle)
{
	if (handle.empty())
		return;

	retainedUploads_.erase(handle.index());

	auto it =
		meshAllocations_.find(handle.index());

	if (it != meshAllocations_.end()) {

		StaticMeshAllocation* allocation = it->second;

		if (allocation->vertexAllocation) {

			vmaVirtualFree(
				staticVertexFmtHeap.m_staticVertexBlock,
				allocation->vertexAllocation
			);
		}

		if (allocation->indexAllocation) {

			vmaVirtualFree(
				staticVertexFmtHeap.m_staticIndexBlock,
				allocation->indexAllocation
			);
		}

		delete allocation;
		meshAllocations_.erase(it);
	}

	staticMeshes.destroy(handle);
}



// -----------------------------------------------------------------------------
// Material
// -----------------------------------------------------------------------------

smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterial(
	smvk::TextureHandle diffuse)
{
	MaterialCreateInfo ci{};
	ci.diffuse = diffuse;
	ci.baseColorTexture = diffuse;
	return CreateMaterial(ci);
}

smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterial(
	const MaterialCreateInfo& info)
{
	// Resolve baseColor texture â€“ prefer baseColorTexture, fall back to legacy diffuse
	TextureHandle baseTex = !info.baseColorTexture.empty() ? info.baseColorTexture : info.diffuse;

	MaterialValues values{};
	values.diffuse = info.baseColorFactor;
	values.textures[0] = baseTex.empty() ? -1 : static_cast<int>(baseTex.index());
	values.textures[1] = info.metallicRoughnessTexture.empty() ? -1 : static_cast<int>(info.metallicRoughnessTexture.index());
	values.textures[2] = info.normalTexture.empty() ? -1 : static_cast<int>(info.normalTexture.index());
	values.textures[3] = info.occlusionTexture.empty() ? -1 : static_cast<int>(info.occlusionTexture.index());
	values.textures2[0] = info.emissiveTexture.empty() ? -1 : static_cast<int>(info.emissiveTexture.index());
	values.textures2[1] = -1;
	values.textures2[2] = -1;
	values.textures2[3] = -1;

	values.params.x = info.alphaCutoff;
	values.params.y = info.metallicFactor;
	values.params.z = info.roughnessFactor;
	values.params.w = static_cast<float>(static_cast<uint32_t>(info.shadingModel));

	values.params2.x = info.occlusionStrength;
	values.params2.y = info.normalScale;
	values.params2.z = info.emissiveStrength;
	values.params2.w = 0.0f;

	values.emissiveFactor = glm::vec4(info.emissiveFactor, 0.0f);
	// values.ambient keeps the MaterialValues default (0,0,0,0): the legacy
	// sky-ambient floor is off by default; ambient comes from the env/IBL
	// block instead. See comment in mesh.h.
	return CreateMaterialFromValues(
		values, info.shadingModel, info.alphaMode, info.alphaCutoff,
		info.doubleSided, info.castsShadows, info.allowFarVertexLit,
		info.farQuality);
}

	smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterialFromValues(
		const MaterialValues& values,
		ShadingModel shadingModel,
		AlphaMode alphaMode,
		float alphaCutoff,
		bool doubleSided,
		bool castsShadows,
		bool allowFarVertexLit,
		ShaderLodQuality farQuality)
	{
		smvk::Material material{};
		material.values = values;
	// Authoritative alphaMode mirror for the shader (params2.w): Blend
	// materials need screen-door dither in the fragment shader, but
	// shadingModel alone cannot distinguish them from Opaque (both are
	// DefaultLit). Enforced here so cooked paks (whose blob predates the
	// encoding) backfill correctly on deserialize.
	material.values.params2.w = static_cast<float>(static_cast<uint32_t>(alphaMode));
	material.shadingModel = shadingModel;
	material.alphaMode = alphaMode;
	material.alphaCutoff = alphaCutoff;
	material.doubleSided = doubleSided;
	material.castsShadows = castsShadows;
	material.shaderLod.allowFarVertexLit = allowFarVertexLit;
	material.shaderLod.farQuality = farQuality;
	material.pipeline = MakePipelineStateForMaterial(alphaMode, doubleSided, shadingModel);
	return materials.create(std::move(material));
}

void smvk::Cluster::DestroyMaterial(smvk::ClusterMaterialHandle handle)
{
	materials.destroy(handle);
}

