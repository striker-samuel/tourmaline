#include <array>

#include <Engine/Renderer/vertex.h>

std::array<VkVertexInputBindingDescription, 2> Vertex::getBindingDescription() {

	std::array<VkVertexInputBindingDescription, 2> b{};
	b[0].binding = 0;
	b[0].stride = sizeof(VertexPos);
	b[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	// Binding 1: shading
	b[1].binding = 1;
	b[1].stride = sizeof(VertexShading);
	b[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	return b;
};


std::array<VkVertexInputAttributeDescription, 5> Vertex::getAttributeDescriptions() {

	std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions{};

	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].vk_format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[0].offset = offsetof(VertexPos, pos);

	attributeDescriptions[1].binding = 1;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].vk_format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[1].offset = offsetof(VertexShading, color);

	attributeDescriptions[2].binding = 1;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].vk_format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[2].offset = offsetof(VertexShading, texCoord);

	attributeDescriptions[3].binding = 1;
	attributeDescriptions[3].location = 3;
	attributeDescriptions[3].vk_format = VK_FORMAT_R32G32B32_SFLOAT;
	attributeDescriptions[3].offset = offsetof(VertexShading, normal);

	attributeDescriptions[4].binding = 1;
	attributeDescriptions[4].location = 4;
	attributeDescriptions[4].vk_format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributeDescriptions[4].offset = offsetof(VertexShading, tangent);

	return attributeDescriptions;
};


