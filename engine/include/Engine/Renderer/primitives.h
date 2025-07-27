#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>


#include <Engine/common.h>
#include <Engine/Vulkan/vulkan_setup.h>
#include <Engine/Renderer/vertex.h>

struct MeshData {
	std::vector<glm::vec3> vertices;
	std::vector<glm::vec3> normals;
	std::vector<uint32_t> indices;
};

struct InstanceData {
	float transform[16];
	uint32_t meshIndex;
	uint32_t materialIndex;
	uint32_t LOD;
	uint32_t indexOffset;
};

struct UniformBufferObject {
	alignas(16) glm::mat4 model;
	alignas(16) glm::mat4 view;
	alignas(16) glm::mat4 proj;
};

//Down like to use char for this.
typedef struct Color4 {
	unsigned char r;  // Color red value
	unsigned char g;  // Color green value
	unsigned char b;  // Color blue value
	unsigned char a;  // Color alpha value
};

// ---------------------------------


// Lifted from LightweightVk as is. Just makes too much sense
// Non-ref counted handles; based on:
// https://enginearchitecture.realtimerendering.com/downloads/reac2023_modern_mobile_rendering_at_hypehype.pdf
template<typename ObjectType>
class Handle final {
public:
	Handle() = default;

	bool empty() const {
		return gen_ == 0;
	}
	bool valid() const {
		return gen_ != 0;
	}
	uint32_t index() const {
		return index_;
	}
	uint32_t gen() const {
		return gen_;
	}
	void* indexAsVoid() const {
		return reinterpret_cast<void*>(static_cast<ptrdiff_t>(index_));
	}
	bool operator==(const Handle<ObjectType>& other) const {
		return index_ == other.index_ && gen_ == other.gen_;
	}
	bool operator!=(const Handle<ObjectType>& other) const {
		return index_ != other.index_ || gen_ != other.gen_;
	}
	// allow conditions 'if (handle)'
	explicit operator bool() const {
		return gen_ != 0;
	}

private:
	Handle(uint32_t index, uint32_t gen) : index_(index), gen_(gen) {};

	template<typename ObjectType_, typename ImplObjectType>
	friend class Pool;

	uint32_t index_ = 0;
	uint32_t gen_ = 0;
};

static_assert(sizeof(Handle<class Foo>) == sizeof(uint64_t));



/*
// specialized with dummy structs for type safety
using ComputePipelineHandle = lvk::Handle<struct ComputePipeline>;
using RenderPipelineHandle = lvk::Handle<struct RenderPipeline>;
using RayTracingPipelineHandle = lvk::Handle<struct RayTracingPipeline>;
using ShaderModuleHandle = lvk::Handle<struct ShaderModule>;
using SamplerHandle = lvk::Handle<struct Sampler>;
using BufferHandle = lvk::Handle<struct Buffer>;
using TextureHandle = lvk::Handle<struct Texture>;
using QueryPoolHandle = lvk::Handle<struct QueryPool>;
using AccelStructHandle = lvk::Handle<struct AccelerationStructure>;

// forward declarations to access incomplete type IContext
void destroy(lvk::IContext* ctx, lvk::ComputePipelineHandle handle);
void destroy(lvk::IContext* ctx, lvk::RenderPipelineHandle handle);
void destroy(lvk::IContext* ctx, lvk::RayTracingPipelineHandle handle);
void destroy(lvk::IContext* ctx, lvk::ShaderModuleHandle handle);
void destroy(lvk::IContext* ctx, lvk::SamplerHandle handle);
void destroy(lvk::IContext* ctx, lvk::BufferHandle handle);
void destroy(lvk::IContext* ctx, lvk::TextureHandle handle);
void destroy(lvk::IContext* ctx, lvk::QueryPoolHandle handle);
void destroy(lvk::IContext* ctx, lvk::AccelStructHandle handle);
*/

