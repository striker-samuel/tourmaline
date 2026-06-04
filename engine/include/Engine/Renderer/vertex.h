#pragma once

#include <Engine/Shared/common.h>
#include "volk.h"


// Fixed vertex format. This is no good. vertex format specializations is needed in a renderer!

struct VertexPos {
	glm::vec3 pos;
};

struct VertexShading {
	glm::vec3 color;
	glm::vec2 texCoord;
	glm::vec3 normal;
	glm::vec4 tangent;
	// for normal mapping. We have a global vertex format right now so its on by default.
	// We will specialize vertex formats when the need arise.
};

struct Vertex {
	glm::vec3 pos;
	glm::vec3 color;
	glm::vec2 texCoord;
	glm::vec3 normal; 
	glm::vec4 tangent; // xyz + sign in w

	static std::array<VkVertexInputBindingDescription, 2> getBindingDescription();
	static std::array<VkVertexInputAttributeDescription, 5> getAttributeDescriptions();

	bool operator==(const Vertex& other) const {
		return pos == other.pos && color == other.color && texCoord == other.texCoord && other.tangent == tangent;
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
