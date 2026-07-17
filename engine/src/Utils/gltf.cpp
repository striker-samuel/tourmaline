#include <Engine/Utils/gltf.h>

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
