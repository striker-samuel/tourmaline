#pragma once

#include <Engine/Shared/common.h>
#include <Engine/Vulkan/vulkan_setup.h>


// Structuring vertices like thsi is not a good idea. Mehses are collections of vertices.
// The GLTF stream has the different properties of verices in different streams. We can use those streams directly
// in GPU. this is bad adn unnecesary if theres no need to address the vertices individually and if that need arised
// there could be helpers to do that without needing to layout the data like this. 
// 
//32 aligned
struct Vertex {
	glm::vec3 pos;
	glm::vec3 color;
	glm::vec2 texCoord;
	glm::vec3 normal;

	// we store only 2 normals, and rebuild the orthogonal thirs missing one in shader if needed.
	// Not implemented yet
	//glm::vec2 normals;

	static VkVertexInputBindingDescription getBindingDescription(); 
	static std::array<VkVertexInputAttributeDescription, 4> getAttributeDescriptions();

	bool operator==(const Vertex& other) const {
		return pos == other.pos && color == other.color && texCoord == other.texCoord;
	}

};

namespace std {
	template<> struct hash<Vertex> {
		size_t operator()(Vertex const& vertex) const {
			return ((hash<glm::vec3>()(vertex.pos) ^
					 (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^
				(hash<glm::vec2>()(vertex.texCoord) << 1);
		}
	};
}
