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

	// GPU-free cook mode: CPU geometry only (no VMA/streamer). Mirrors the
	// runtime single-surface layout so the serializer sees the same shape.
	if (cookMode_) {
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
		shading[i].uv2 = vertexpack::PackUvHalf(src.texCoord2);
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
// Mesh replacement (lightmap expanded runs)
// -----------------------------------------------------------------------------

bool smvk::Cluster::ReplaceStaticMesh(
	smvk::ClusterStaticMeshHandle handle, const StaticMeshUpload& data)
{
	if (handle.empty() || cookMode_)
		return false;
	const uint32_t vertexCount = uint32_t(data.vertices.size());
	const uint32_t indexCount = uint32_t(data.indices.size());
	if (vertexCount == 0 || (indexCount % 3) != 0)
		return false;
	StaticMesh* mesh = staticMeshes.get(handle);
	if (!mesh)
		return false;

	// Free the old ranges (mirrors DestroyMesh) before re-allocating, so
	// the freed space can serve the new (usually larger) request.
	auto it = meshAllocations_.find(handle.index());
	if (it != meshAllocations_.end()) {
		StaticMeshAllocation* allocation = it->second;
		if (allocation->vertexAllocation)
			vmaVirtualFree(staticVertexFmtHeap.m_staticVertexBlock,
				allocation->vertexAllocation);
		if (allocation->indexAllocation)
			vmaVirtualFree(staticVertexFmtHeap.m_staticIndexBlock,
				allocation->indexAllocation);
		delete allocation;
		meshAllocations_.erase(it);
	}

	VkDeviceSize vertexBase = 0;
	VkDeviceSize indexByteOffset = 0;
	StaticMeshAllocation alloc{};
	{
		VmaVirtualAllocationCreateInfo allocInfo{};
		allocInfo.size = vertexCount;
		allocInfo.alignment = 1;
		if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticVertexBlock,
				&allocInfo, &alloc.vertexAllocation, &vertexBase) != VK_SUCCESS)
			return false;
	}
	const VkDeviceSize indexBytes =
		sizeof(uint32_t) * static_cast<VkDeviceSize>(indexCount);
	{
		VmaVirtualAllocationCreateInfo allocInfo{};
		allocInfo.size = indexBytes;
		allocInfo.alignment = alignof(uint32_t);
		if (vmaVirtualAllocate(staticVertexFmtHeap.m_staticIndexBlock,
				&allocInfo, &alloc.indexAllocation, &indexByteOffset) != VK_SUCCESS) {
			vmaVirtualFree(staticVertexFmtHeap.m_staticVertexBlock,
				alloc.vertexAllocation);
			return false;
		}
	}

	// Pack + upload the streams (mirrors UploadStaticMesh steps 3-5).
	std::vector<VertexPos> positions(vertexCount);
	std::vector<VertexShading> shading(vertexCount);
	for (uint32_t i = 0; i < vertexCount; ++i) {
		const StaticVertex& src = data.vertices[i];
		positions[i].pos = src.pos;
		shading[i].color = vertexpack::PackColorUnorm8(src.color);
		shading[i].uv = vertexpack::PackUvHalf(src.texCoord);
		shading[i].uv2 = vertexpack::PackUvHalf(src.texCoord2);
		shading[i].normal = vertexpack::PackNormalOct(src.normal);
		shading[i].tangent = vertexpack::PackTangentOct(
			glm::vec3(src.tangent), src.tangent.w);
	}
	auto* vctx = static_cast<VulkanResourceManager*>(ctx_->GetVulkanOpaque());
	vctx->UploadDataToBuffer(staticVertexFmtHeap.m_staticVertexPosBuffer,
		positions.data(), vertexCount * sizeof(VertexPos),
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, 0,
		vertexBase * sizeof(VertexPos));
	vctx->UploadDataToBuffer(staticVertexFmtHeap.m_staticVertexShadingBuffer,
		shading.data(), vertexCount * sizeof(VertexShading),
		VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
		VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, 0,
		vertexBase * sizeof(VertexShading));
	if (indexCount > 0)
		vctx->UploadDataToBuffer(staticVertexFmtHeap.m_staticIndexBuffer,
			data.indices.data(), indexBytes,
			VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
			VK_ACCESS_2_INDEX_READ_BIT, 0, indexByteOffset);

	// Rebuild the pool entry's geometry in place (handle stays alive so
	// material/node references keep resolving). Stale cooked meshlets are
	// DROPPED: they reference the OLD vertex layout (local ids into the
	// pre-swap ranges), so the gfx filter would bake garbage into its
	// index heap. Meshes without meshlets fall back to the direct-draw
	// path (correct, just not HZB-culled) — re-cook the pak with the
	// lightmap-expanded geometry to restore the filtered path.
	mesh->geometry.vertexCount = vertexCount;
	mesh->geometry.indexCount = indexCount;
	mesh->geometry.aabbMin = data.aabbMin;
	mesh->geometry.aabbMax = data.aabbMax;
	mesh->geometry.surfaces.clear();
	Surface surface{};
	surface.firstIndex = uint32_t(indexByteOffset / sizeof(uint32_t));
	surface.indexCount = indexCount;
	surface.baseVertex = int32_t(vertexBase);
	mesh->geometry.surfaces.push_back(surface);
	mesh->meshlets.reset();

	auto [it2, inserted] = meshAllocations_.emplace(
		handle.index(), new StaticMeshAllocation(std::move(alloc)));
	assert(inserted);
	(void)it2;
	if (retainUploads_)
		retainedUploads_[handle.index()] = data;
	return true;
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
	// Resolve baseColor texture – prefer baseColorTexture, fall back to legacy diffuse
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
		info.doubleSided, info.castsShadows);
}

smvk::ClusterMaterialHandle smvk::Cluster::CreateMaterialFromValues(
	const MaterialValues& values,
	ShadingModel shadingModel,
	AlphaMode alphaMode,
	float alphaCutoff,
	bool doubleSided,
	bool castsShadows)
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
	material.pipeline = MakePipelineStateForMaterial(alphaMode, doubleSided, shadingModel);
	return materials.create(std::move(material));
}

void smvk::Cluster::DestroyMaterial(smvk::ClusterMaterialHandle handle)
{
	materials.destroy(handle);
}

