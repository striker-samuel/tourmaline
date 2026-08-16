#include <Engine/Renderer/mesh.h>

namespace smvk {

    std::vector<VkVertexInputBindingDescription>
        getVkVertexInputBindingDescriptions(const VertexFormat& format)
    {
        std::vector<VkVertexInputBindingDescription> descriptions;
        descriptions.reserve(format.bindings.size());

        for (const VertexBinding& binding : format.bindings) {
            descriptions.push_back({
                .binding = binding.binding,
                .stride = binding.stride,
                .inputRate = binding.inputRate,
                });
        }

        return descriptions;
    }

    std::vector<VkVertexInputAttributeDescription>
        getVkVertexAttributeDescriptions(const VertexFormat& format)
    {
        std::vector<VkVertexInputAttributeDescription> descriptions;

        size_t attributeCount = 0;

        for (const VertexBinding& binding : format.bindings) {
            attributeCount += binding.attributes.size();
        }

        descriptions.reserve(attributeCount);

        uint32_t location = 0;

        for (const VertexBinding& binding : format.bindings) {
            for (const VertexAttribute& attribute : binding.attributes) {
                descriptions.push_back({
                    .location = location++,
                    .binding = binding.binding,
                    .format = attribute.format,
                    .offset = attribute.offset,
                    });
            }
        }

        return descriptions;
    }

} // namespace smvk