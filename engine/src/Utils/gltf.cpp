#include <Engine/Utils/gltf.h>



static bool starts_with(const std::string& s, const char* prefix) {
	return s.rfind(prefix, 0) == 0;
}
size_t gltfElementSize(const tinygltf::Accessor& a) {
	using namespace tinygltf;
	int numComps =
		(a.type == TINYGLTF_TYPE_SCALAR) ? 1 :
		(a.type == TINYGLTF_TYPE_VEC2) ? 2 :
		(a.type == TINYGLTF_TYPE_VEC3) ? 3 :
		(a.type == TINYGLTF_TYPE_VEC4) ? 4 :
		(a.type == TINYGLTF_TYPE_MAT2) ? 4 :
		(a.type == TINYGLTF_TYPE_MAT3) ? 9 :
		(a.type == TINYGLTF_TYPE_MAT4) ? 16 : 0;

	size_t compSize =
		(a.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) ? 1 :
		(a.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) ? 2 :
		(a.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT ||
			a.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) ? 4 : 0;

	return numComps * compSize;
}
static const uint8_t* gltfPtr(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t index)
{
	const auto& bv = m.bufferViews[a.bufferView];
	const auto& buf = m.buffers[bv.buffer];
	size_t elemSize = gltfElementSize(a);
	size_t stride = bv.byteStride ? bv.byteStride : elemSize;

	size_t base = (bv.byteOffset ? bv.byteOffset : 0) +
		(a.byteOffset ? a.byteOffset : 0);
	return buf.data.data() + base + index * stride;
}
static glm::vec3 readVec3ForientationFix(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0] * -1, p[1], p[2] * -1);
}
static glm::vec4 readVec4F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC4
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec4(p[0], p[1], p[2], p[3]);
}
static glm::vec3 readVec3F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC3
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec3(p[0], p[1], p[2]);
}
static glm::vec2 readVec2F(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	// Expect FLOAT VEC2
	const float* p = reinterpret_cast<const float*>(gltfPtr(m, a, i));
	return glm::vec2(p[0], p[1]);
}
uint32_t readIndex(const tinygltf::Model& m, const tinygltf::Accessor& a, size_t i) {
	using namespace tinygltf;
	const uint8_t* p = gltfPtr(m, a, i);
	switch (a.componentType) {
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  return *reinterpret_cast<const uint8_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return *reinterpret_cast<const uint16_t*>(p);
	case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   return *reinterpret_cast<const uint32_t*>(p);
	default: throw std::runtime_error("Unsupported index componentType");
	}
}

//
std::filesystem::path UriFromTexture(const tinygltf::Model& model, GltfTextureRef& texture_ref) {
	const tinygltf::Image& img = model.images[texture_ref.imageIndex];

	// Case 1: External image via URI
	if (!img.uri.empty() && !starts_with(img.uri, "data:")) {

		// this duty falls on the caller as of now .
		//std::filesystem::path baseDir = gltfPath.parent_path();
		//std::filesystem::path imagePath = baseDir / img.uri;
		std::filesystem::path imagePath = img.uri;

		return imagePath.lexically_normal();
	}
	else {
		// No internal texures!
		assert(false && "Internal textures not supported for gld loading.");
		return "";
	}
}

std::optional<GltfTextureRef> GltfResolveTexture(const tinygltf::Model& model, const tinygltf::TextureInfo& texInfo)
{
	if (texInfo.index < 0 || texInfo.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[texInfo.index];
	GltfTextureRef out{};
	out.textureIndex = texInfo.index;
	out.texCoord = texInfo.texCoord;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt; // some files might omit source if using extensions
	return out;
}

std::optional<GltfTextureRef> GtlfResolveNormalTexture(const tinygltf::Model& model, const tinygltf::NormalTextureInfo& nrm)
{
	if (nrm.index < 0 || nrm.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[nrm.index];
	GltfTextureRef out{};
	out.textureIndex = nrm.index;
	out.texCoord = nrm.texCoord;
	out.scaleOrStrength = (float)nrm.scale;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt;
	return out;
}

std::optional<GltfTextureRef> GLtfResolveOcclusionTexture(const tinygltf::Model& model, const tinygltf::OcclusionTextureInfo& occ)
{
	if (occ.index < 0 || occ.index >= (int)model.textures.size()) return std::nullopt;
	const tinygltf::Texture& tex = model.textures[occ.index];
	GltfTextureRef out{};
	out.textureIndex = occ.index;
	out.texCoord = occ.texCoord;
	out.scaleOrStrength = (float)occ.strength;

	if (tex.source >= 0 && tex.source < (int)model.images.size())
		out.imageIndex = tex.source;
	if (tex.sampler >= 0 && tex.sampler < (int)model.samplers.size())
		out.samplerIndex = tex.sampler;

	if (out.imageIndex < 0) return std::nullopt;
	return out;
}

std::vector<GltfMaterial> ExtractMaterials(const tinygltf::Model& model)
{
	std::vector<GltfMaterial> out;
	out.reserve(model.materials.size());

	for (int i = 0; i < (int)model.materials.size(); ++i) {
		const tinygltf::Material& m = model.materials[i];
		GltfMaterial mat{};
		mat.name = m.name;

		// Core PBR factors
		if (m.pbrMetallicRoughness.baseColorFactor.size() == 4) {
			for (int k = 0; k < 4; ++k) mat.baseColorFactor[k] = m.pbrMetallicRoughness.baseColorFactor[k];
		}
		mat.metallicFactor = m.pbrMetallicRoughness.metallicFactor;
		mat.roughnessFactor = m.pbrMetallicRoughness.roughnessFactor;

		// Textures
		if (auto t = GltfResolveTexture(model, m.pbrMetallicRoughness.baseColorTexture))
			mat.baseColorTex = *t;

		if (auto t = GltfResolveTexture(model, m.pbrMetallicRoughness.metallicRoughnessTexture))
			mat.metallicRoughnessTex = *t;

		if (auto t = GtlfResolveNormalTexture(model, m.normalTexture))
			mat.normalTex = *t;

		if (auto t = GLtfResolveOcclusionTexture(model, m.occlusionTexture))
			mat.occlusionTex = *t;

		if (auto t = GltfResolveTexture(model, m.emissiveTexture))
			mat.emissiveTex = *t;

		if (m.emissiveFactor.size() == 3) {
			for (int k = 0; k < 3; ++k) mat.emissiveFactor[k] = m.emissiveFactor[k];
		}

		// Alpha + sidedness
		mat.alphaMode = ParseAlphaMode(m.alphaMode);
		mat.alphaCutoff = m.alphaCutoff;     // only meaningful for MASK
		mat.doubleSided = m.doubleSided;

		// (Optional) Example: read KHR_materials_unlit flag
		// if (m.additionalValues.count("unlit")) { ... } // or check m.extensions

		// (Optional) Example: KHR_texture_transform on baseColorTexture
		// auto it = m.extensions.find("KHR_texture_transform");
		// If present, read offset/scale/rotation and apply in your UV pipeline.

		out.emplace_back(std::move(mat));
	}

	return out;
}

glm::mat4 GetNodeLocalTransform(const tinygltf::Node& n)
{
	if (!n.matrix.empty()) {
		glm::mat4 m(1.0f);
		memcpy(glm::value_ptr(m), n.matrix.data(), sizeof(float) * 16);
		return m;
	}

	glm::vec3 t(0.0f);
	glm::quat r(1, 0, 0, 0);
	glm::vec3 s(1.0f);

	if (!n.translation.empty())
		t = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);

	if (!n.rotation.empty())
		r = glm::quat(
			n.rotation[3],
			n.rotation[0],
			n.rotation[1],
			n.rotation[2]
		);

	if (!n.scale.empty())
		s = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);

	return glm::translate(glm::mat4(1.0f), t)
		* glm::mat4_cast(r)
		* glm::scale(glm::mat4(1.0f), s);
}
