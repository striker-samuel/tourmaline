#include <Engine/Renderer/resource_manager.h>

MeshManager::MeshManager(smvk::Context& ctx) :ctx_(ctx) {}
void MeshManager::InitalizeGeoBuffers() {

	{
		uint32_t totalVertexBytes = 512u * 1024u * 1024u;
		uint32_t totalIndexBytes = 512u * 1024u * 1024u;

		// Create the actual GPU buffers (these are the page's buffers)
		smvk::BufferHandle vb_pos = ctx_.CreateBuffer(
			{
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		smvk::BufferHandle vb_shd = ctx_.CreateBuffer(
			{
			totalVertexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		smvk::BufferHandle ib = ctx_.CreateBuffer(
			{
				totalIndexBytes,
			VMA_MEMORY_USAGE_GPU_ONLY,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
			}
		);

		auto max_vertices = std::min(
			totalVertexBytes / uint32_t(sizeof(VertexPos)),
			totalVertexBytes / uint32_t(sizeof(VertexShading))
		);

		// Index allocator (bytes)
		{
			VmaVirtualBlockCreateInfo ci{};
			ci.size = totalIndexBytes;
			VK_CHECK(vmaCreateVirtualBlock(&ci, &index_blk));
		}

		// Vertex allocator (VERTEX UNITS!)
		{
			VmaVirtualBlockCreateInfo vu{
				.size = max_vertices,
				.flags = 0
			};
			VK_CHECK(vmaCreateVirtualBlock(&vu, &vertex_units_blk));
		}

	}

}
MeshHandle MeshManager::UploadVertexData(Mesh::DataUpload& mesh_data)
{
	const uint32_t vcount = static_cast<uint32_t>(mesh_data.vertices.size());
	Mesh mesh_desc{};
	// This is a really hacky way to implement separate buffer pos and shading virtual alloc,
	// we alloc vertex count on a virtual allocator with vertex size and use that as bookkeeping, 
	// but then we build our offsets into the real buffers. 

	// ------------------------------------------
	// 0) Allocate vertices from ONE vertex-units block
	// ------------------------------------------
	VkDeviceSize baseVertex = 0; // <-- returned "offset" in VERTEX UNITS (not bytes)

	{
		VmaVirtualAllocationCreateInfo aV{};
		aV.size = vcount;     // vertex units
		aV.alignment = 1;     // vertex units

		if (vmaVirtualAllocate(vertex_units_blk, &aV, &mesh_desc.vma_alloc.vertex, &baseVertex) != VK_SUCCESS) {
			// Not enough contiguous vertex space in this page (or fragmented)
			return {}; // TODO :: cath this result!
		}
	}

	// Derived byte offsets for each stream (for copy destinations)
	const VkDeviceSize pos_byte_offset = baseVertex * sizeof(VertexPos);
	const VkDeviceSize shd_byte_offset = baseVertex * sizeof(VertexShading);

	// ------------------------------------------
	// 1) INDICES
	// ------------------------------------------
	using IndexT = uint32_t;
	const VkDeviceSize indexBytes =
		sizeof(IndexT) * static_cast<VkDeviceSize>(mesh_data.indices.size());

	VkDeviceSize index_offset_bytes = 0;

	{
		VmaVirtualAllocationCreateInfo aIdx{};
		aIdx.size = indexBytes;
		aIdx.alignment = sizeof(IndexT);

		if (vmaVirtualAllocate(index_blk /*or index_blk*/, &aIdx, &mesh_desc.vma_alloc.index, &index_offset_bytes) != VK_SUCCESS) {
			vmaVirtualFree(vertex_units_blk, mesh_desc.vma_alloc.vertex); // Roll back vertex allocation
			return {};
		}

		ctx_.UploadDataToBuffer(indexBuffer, mesh_data.indices.data(), indexBytes, 0, index_offset_bytes);
	}

	// ------------------------------------------
	// 2) Split vertices into two CPU arrays
	// ------------------------------------------
	std::vector<VertexPos> pos(vcount);
	std::vector<VertexShading> shd(vcount);

	for (uint32_t i = 0; i < vcount; ++i) {
		const Vertex& v = mesh_data.vertices[i];

		pos[i].pos = v.pos;

		shd[i].color = v.color;
		shd[i].texCoord = v.texCoord;
		shd[i].normal = v.normal;
		shd[i].tangent = v.tangent;
	}

	// ------------------------------------------
	// 3) Upload positions to binding 0 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize posBytes = sizeof(VertexPos) * static_cast<VkDeviceSize>(vcount);
		ctx_.UploadDataToBuffer(vertexPosBuffer, pos.data(), posBytes, 0, 0, pos_byte_offset);
	}

	// ------------------------------------------
	// 4) Upload shading to binding 1 buffer at derived offset
	// ------------------------------------------
	{
		const VkDeviceSize shdBytes = sizeof(VertexShading) * static_cast<VkDeviceSize>(vcount);
		ctx_.UploadDataToBuffer(vertexShadingBuffer, pos.data(), shdBytes, 0, 0, shd_byte_offset);
	}

	// ------------------------------------------
	// 5) Surface (bind-once style)
	// ------------------------------------------
	Mesh::Surface ds{};
	ds.index_count = static_cast<uint32_t>(mesh_data.indices.size());
	ds.first_index = static_cast<uint32_t>(index_offset_bytes / sizeof(IndexT));
	ds.base_vertex = static_cast<int32_t>(baseVertex);

	// IMPORTANT: store allocs somewhere if you want to free later.
	// e.g. ds.vertex_alloc = vtx_alloc; ds.index_alloc = index_alloc; (requires fields)

	//mesh_desc.lods = 1;
	//mesh_desc.lod_offsets = { 0 };
	mesh_desc.surfaces = { ds };
	mesh_desc.aabbMin = mesh_data.aabb_min;
	mesh_desc.aabbMax = mesh_data.aabb_max;
	//mesh_desc.alloc.page = current_geometry_page;

	return meshes.create(std::move(mesh_desc));
}
[[nodiscard]] MaterialHandle MeshManager::CreateMaterial(Material::Textures textures) {
	assert(textures.diffuse.valid() && "Invalid handle for diffuse");
	Material mat{};
	mat.textures_.diffuse = textures.diffuse;
	mat.values.tex[0] = textures.diffuse.index();
	return  materials.create(std::move(mat));
}
smvk::TextureHandle MeshManager::GetOrCreateTexture(std::string uri, ktxTexture2& tex) {

	auto it = textureCache.find(uri);
	if (it == textureCache.end()) {
		//auto han = ctx_.CreateTexture(tex);
		//textureCache[uri] = han;
		//return han;
	}
	return it->second;
}
std::vector<smvk::BufferHandle> MeshManager::GetVertexBuffers() {
	return std::vector<smvk::BufferHandle>{
		indexBuffer,
			vertexPosBuffer,
			vertexShadingBuffer,
	};
}
void destroy(MeshManager* ctx, MeshHandle handle) {};
Mesh::DataUpload SurfaceDataFromGltfPrimitive(
	const tinygltf::Model& model,
	const tinygltf::Primitive& primitive
) {
	Mesh::DataUpload data;
	// AABB. for culling	
	{
		auto it = primitive.attributes.find("POSITION");
		if (it == primitive.attributes.end())
			return data;

		int accessorIndex = it->second;
		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];

		if (accessor.minValues.size() >= 3 && accessor.maxValues.size() >= 3) {

			// glTF stores AABB in the accessor
			const std::vector<double>& min = accessor.minValues;
			const std::vector<double>& max = accessor.maxValues;

			glm::vec3 aabbMin(min[0], min[1], min[2]);
			glm::vec3 aabbMax(max[0], max[1], max[2]);

			// Apply same conversion as positions
			glm::vec3 c0(-aabbMin.x, aabbMin.y, -aabbMin.z);
			glm::vec3 c1(-aabbMax.x, aabbMax.y, -aabbMax.z);

			glm::vec3 mn = glm::min(c0, c1);
			glm::vec3 mx = glm::max(c0, c1);

			data.aabb_min = mn;
			data.aabb_max = mx;


		}
		else {

			// I wont handle this. just assert and fail. 
			assert(false && "failed to get bb ");

		}

	}

	// Extract vertices
	auto& posAcc = model.accessors[primitive.attributes.at("POSITION")];
	data.vertices.reserve(posAcc.count);
	for (size_t i = 0; i < posAcc.count; ++i) {
		Vertex v{};
		v.pos = readVec3ForientationFix(model, posAcc, i);
		v.color = { 1.0f, 1.0f, 1.0f };
		v.texCoord = { 0.0f, 0.0f }; // will fill if UVs exist
		v.normal = { 0.0f, 1.0f, 0.0f }; // will replace if normals provided
		v.tangent = { 1.0f, 0.0f,0.0f, 1.0f }; // zero out tangent by default too ;)
		data.vertices.push_back(v);
	}

	if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end()) {
		auto& uvAcc = model.accessors[it->second];
		for (size_t i = 0; i < uvAcc.count && i < data.vertices.size(); ++i) {
			glm::vec2 uv = readVec2F(model, uvAcc, i);
			data.vertices[i].texCoord = uv; // flip V if your shader expects it: uv.y = 1.0f - uv.y;
		}
	}

	if (auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end()) {
		auto& tangentAcc = model.accessors[it->second];
		for (size_t i = 0; i < tangentAcc.count && i < data.vertices.size(); ++i) {
			glm::vec4 tangent = readVec4F(model, tangentAcc, i);

			// Space conversion to my engine. Same as pos
			tangent.x *= -1.0f;
			tangent.z *= -1.0f;

			data.vertices[i].tangent = tangent;
		}
	}


	if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end()) {
		auto& nAcc = model.accessors[it->second];
		for (size_t i = 0; i < nAcc.count && i < data.vertices.size(); ++i) {
			glm::vec3 n = readVec3ForientationFix(model, nAcc, i);
			data.vertices[i].normal = glm::normalize(n);
		}
	}

	if (primitive.indices >= 0) {
		auto& idxAcc = model.accessors[primitive.indices];
		for (size_t i = 0; i < idxAcc.count; ++i) {
			data.indices.push_back(readIndex(model, idxAcc, i));
		}
	}

	return data;

}
uint32_t GltfToSceneImporter::getOrCreateMaterial(int gltfMaterialIndex) {
	if (gltfMaterialIndex < 0) {
		return DEFAULT_MATERIAL_INDEX;
	}

	// glTF guarantees material indices are within model.materials
	// (assuming the file is valid). If you want belt-and-suspenders:
	if ((size_t)gltfMaterialIndex >= materialCache.size()) {
		return DEFAULT_MATERIAL_INDEX;
	}

	uint32_t& slot = materialCache[(size_t)gltfMaterialIndex];
	if (slot != kInvalidMat) {
		return slot;
	}

	// NOTE: Avoid storing baseDir as a dangling c_str(). Build it here.
	const std::string baseDir = gltfModelPath.parent_path().string();

	// TODO : This should be done onlt once instead of here!
	auto mat = ExtractMaterials(model)[gltfMaterialIndex];

	smvk::TextureHandle colorTexHandle{};

	// TODO : Upload all textures
	{

		//auto color_uri = UriFromTexture(model, *mat.baseColorTex);
		//ktxTexture2* ktxTex = nullptr;
		//KTX_error_code res = ktxTexture2_CreateFromNamedFile(
		//	(baseDir.c_str() / color_uri).string().c_str(),
		//	KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, // <<–– correct flags
		//	&ktxTex
		//);

		//if (res != KTX_SUCCESS) {
		//	throw std::runtime_error("KTX load failed");
		//}

		////colorTexHandle = manager_.ctx_.CreateTexture(color_uri.string(), *ktxTex);
		//colorTexHandle = manager_.ctx_.CreateTexture(*ktxTex);
		//ktxTexture2_Destroy(ktxTex);
	}

	return manager_.CreateMaterial(
		{
			.diffuse = colorTexHandle
		}
	).index();

}
uint32_t GltfToSceneImporter::getOrCreateMesh(tinygltf::Primitive primitive) {
	auto mdata = SurfaceDataFromGltfPrimitive(model, primitive);
	//return createMeshCallback_(&mdata).index();
	return manager_.UploadVertexData(mdata).index();
}
void GltfToSceneImporter::importNodeRecursive(int gltfNodeIndex, int parentSceneNode, int level) {
	const tinygltf::Node& gltfNode = model.nodes[gltfNodeIndex];

	// Always create a "group" node that owns name + transform and preserves hierarchy.
	const int groupNode = addNode(scene, parentSceneNode, level);

	{
		const int nameIndex = (int)scene.nodeNames.size();
		scene.nodeNames.push_back(gltfNode.name.empty() ? "Node" : gltfNode.name);
		scene.nameForNode[groupNode] = nameIndex;
	}

	scene.localTransform[groupNode] = GetNodeLocalTransform(gltfNode);
	scene.globalTransform[groupNode] = glm::mat4(1.0f); // resolved later

	// Unroll primitives into separate meshes (your current approach).
	if (gltfNode.mesh >= 0) {
		const tinygltf::Mesh& gltfMesh = model.meshes[gltfNode.mesh];

		for (int primIndex = 0; primIndex < (int)gltfMesh.primitives.size(); ++primIndex) {
			const tinygltf::Primitive& primitive = gltfMesh.primitives[primIndex];

			const int primNode = addNode(scene, groupNode, level + 1);

			scene.localTransform[primNode] = glm::mat4(1.0f);
			scene.globalTransform[primNode] = glm::mat4(1.0f);

			scene.meshForNode[primNode] = getOrCreateMesh(primitive);
			scene.materialForNode[primNode] = getOrCreateMaterial(primitive.material);

			markAsChanged(scene, primNode);
		}
	}

	// Recurse children under the group node to preserve hierarchy.
	for (int child : gltfNode.children) {
		importNodeRecursive(child, groupNode, level + 1);
	}
}
