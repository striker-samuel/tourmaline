//#include <Enginer/Renderer/rendergraph.h>
//
//ToyRenderPass::ToyRenderPass& setColor(
//	ImageKey image,
//	VkAttachmentLoadOp loadOp, // Using Vulkan native types without RHI abstraction for the foreseable future. Only Vulkan backend for now!
//	VkClearColorValue clearValue
//) {
//	this->colorAttachments.resize(_colorAttachmentViewProxies.size());
//	for (size_t index = 0; index < _colorAttachmentViewProxies.size(); index++)
//	{
//		this->colorAttachments[index] = { _colorAttachmentViewProxies[index], _loadOp, _clearValue };
//	}
//	return *this;
//};
