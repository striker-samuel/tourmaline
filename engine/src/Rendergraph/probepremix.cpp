// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Rendergraph/probepremix.h>

#include <algorithm>

namespace smvk::probepremix {

namespace {

// The shader payloads (paths ride the App content dir like every other
// module; entry points mainCS by the .cs.hlsl convention).
inline constexpr const char* kWeightsCsPath =
	"content/shaders/probepremix_weights.cs.hlsl.spv";
inline constexpr const char* kMixCsPath =
	"content/shaders/probepremix_mix.cs.hlsl.spv";

std::vector<char> ReadShader(const char* path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return {};
	return std::vector<char>(
		(std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
}

} // namespace

bool ProbePremix::Ensure(ResourceManager& ctx, RenderGraph& rg,
	uint32_t faceSizeIn, uint32_t mipCountIn) {
	if (valid && size == faceSizeIn && mipCount == mipCountIn)
		return true;
	Destroy(rg);

	size = std::max(faceSizeIn, 4u);
	mipCount = std::max(mipCountIn, 1u);

	// The blended cubemap image: cube-compatible, all mips + 6 faces.
	// Storage (the mix pass writes through the bindless storage array,
	// binding 2) + Sampled (the material reads through the cube view,
	// binding 5). RGBA16F matches the HDR half-float radiance of the
	// baked chains.
	{
		ImageCreateInfo ci{};
		ci.type = ImageType::Image2D;
		ci.extent = { size, size, 1 };
		ci.format = Format::RGBA16_Float;
		ci.usage = ImageUsage::Sampled | ImageUsage::Storage;
		ci.mipLevels = mipCount;
		ci.arrayLayers = smvk::kCubemapFaceCount;
		ci.isCubemap = true;
		ci.name = "premix blended cubemap";
		Holder<ImageHandle> image = ctx.CreateImage(ci, nullptr);
		if (image.empty())
			return false;

		// Sampled cube view: the full chain (all faces, all mips) for the
		// material's roughness-LOD fetches.
		{
			ImageViewCreateInfo vci{};
			vci.viewType = ImageViewType::ViewCube;
			vci.format = Format::Undefined; // inherit
			vci.subresourceRange = {
				ImageAspect::Color, 0, mipCount, 0, smvk::kCubemapFaceCount };
			vci.name = "premix blended cube view";
			Holder<ImageViewHandle> view = ctx.CreateImageView(image, vci);
			if (view.empty())
				return false;
			Holder<BindlessImageHandle> bindless =
				ctx.CreateBindlessImage(view, ImageLayout::ShaderReadOnlyOptimal);
			if (bindless.empty())
				return false;
			cubeView = std::move(view);
			cubeBindless = std::move(bindless);
			cubeBindlessIndex = ctx.GetBindlessIndex(cubeBindless);
		}

		// Per-mip storage views: 2D-array over the 6 face layers at ONE
		// mip level each (storage image views must be single-level). These
		// register in the shared bindless storage array's slot space
		// (binding 2, the evsmStorage/spdImages space) - the mix pass
		// imageStores through them.
		mipStorageViews.resize(mipCount);
		mipStorageBindless.resize(mipCount);
		mipStorageIndex.resize(mipCount);
		for (uint32_t m = 0; m < mipCount; ++m) {
			ImageViewCreateInfo vci{};
			vci.viewType = ImageViewType::View2DArray;
			vci.format = Format::Undefined;
			vci.subresourceRange = {
				ImageAspect::Color, m, 1, 0, smvk::kCubemapFaceCount };
			vci.name = "premix blended mip storage view";
			Holder<ImageViewHandle> view = ctx.CreateImageView(image, vci);
			if (view.empty())
				return false;
			Holder<BindlessImageHandle> bindless =
				ctx.CreateBindlessImage(view, ImageLayout::General);
			if (bindless.empty())
				return false;
			mipStorageViews[m] = std::move(view);
			mipStorageBindless[m] = std::move(bindless);
			mipStorageIndex[m] = ctx.GetBindlessIndex(mipStorageBindless[m]);
			// Graph barrier handle: the mix pass writes this mip's layers
			// (General read-write), the material pass reads the CUBE view.
			// The cube view and the per-mip storage views alias the same
			// image - the graph resolves the barriers per logical view, and
			// both are registered so the write->read edge exists.
			mipLogical.push_back(rg.AddExternalImageView(
				mipStorageViews[m], Format::RGBA16_Float,
				("premix mip logical " + std::to_string(m)).c_str()));
		}

		blendedImage = std::move(image);
	}

	// Selection buffer (GPU-owned, rewritten each frame by the weights
	// pass; the mix pass reads it by BDA - no CPU readback anywhere).
	{
		Holder<BufferHandle> sel = ctx.CreateBuffer(
			sizeof(BlendSelectionGpu), BufferType_Storage, "premix selection");
		if (sel.empty())
			return false;
		selectionBuffer = std::move(sel);
		selectionBda = ctx.GetBufferDeviceAddress(selectionBuffer);
	}

	// Programs.
	{
		std::vector<char> code = ReadShader(kWeightsCsPath);
		if (code.empty()) {
			std::cerr << "[ProbePremix] weights shader missing: "
				<< kWeightsCsPath << "\n";
			return false;
		}
		weightsModule = ctx.CreateShaderModule(code, "premixWeightsCS");
		weightsProgram = {
			.shaders = { smvk::Shader{
				.module = weightsModule,
				.entryPoint = "mainCS",
				.stage = smvk::ShaderStage::Compute } },
			.shaderCount = 1
		};
	}
	{
		std::vector<char> code = ReadShader(kMixCsPath);
		if (code.empty()) {
			std::cerr << "[ProbePremix] mix shader missing: "
				<< kMixCsPath << "\n";
			return false;
		}
		mixModule = ctx.CreateShaderModule(code, "premixMixCS");
		mixProgram = {
			.shaders = { smvk::Shader{
				.module = mixModule,
				.entryPoint = "mainCS",
				.stage = smvk::ShaderStage::Compute } },
			.shaderCount = 1
		};
	}

	valid = true;
	return true;
}

void ProbePremix::Destroy(RenderGraph& rg) {
	for (auto& logical : mipLogical)
		if (!logical.empty())
			rg.FreeImageView(logical);
	mipLogical.clear();
	mipStorageIndex.clear();
	mipStorageBindless.clear();
	mipStorageViews.clear();
	cubeBindlessIndex = UINT32_MAX;
	cubeBindless = {};
	cubeView = {};
	blendedImage = {};
	selectionBda = 0;
	selectionBuffer = {};
	weightsProgram = ShaderProgram{};
	weightsModule = {};
	mixProgram = ShaderProgram{};
	mixModule = {};
	size = 0;
	mipCount = 0;
	valid = false;
}

bool ProbePremix::AppendPasses(RenderGraph& rg, uint64_t lightsBda,
	uint32_t lightCount, uint32_t probeCount, const glm::vec3& poi,
	uint32_t samplerIndex) {
	if (!valid || probeCount == 0)
		return false;

	// ---- Pass A: weights (one group; deterministic selection) ----
	{
		// Graph buffer handle for the selection (barriered write; the
		// External buffer exists for the whole module lifetime).
		LogicalBufferHandle selectionLogical =
			rg.AddExternalBuffer(selectionBuffer, "premix selection");
		WeightsPush push{};
		push.lights = lightsBda;
		push.selection = selectionBda;
		push.poiX = poi.x;
		push.poiY = poi.y;
		push.poiZ = poi.z;
		push.lightCount = lightCount;
		push.probeCount = probeCount;
		RenderPass& pass = rg.AddComputePass("ProbePremixWeights");
		BufferSyncRequest selWrite{};
		selWrite.barrierMasks.accessMask =
			AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
		selWrite.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(selectionLogical, selWrite);
		smvk::ShaderProgram* prog = &weightsProgram;
		pass.SetRecordingFunc([prog, push](CommandBuffer& cmd) {
			cmd.BindComputePipeline(*prog);
			cmd.PushConstants(0, sizeof(push), &push,
				smvk::ShaderStage::Compute);
			cmd.Dispatch(1, 1, 1);
		});
	}

	// ---- Pass B: mix (one dispatch per mip; groups cover face*8x8) ----
	{
		// Graph buffer handle for the selection read (the weights pass
		// wrote it; the same-frame edge resolves through the shared
		// logical buffer).
		LogicalBufferHandle selectionLogical =
			rg.AddExternalBuffer(selectionBuffer, "premix selection");
		RenderPass& pass = rg.AddComputePass("ProbePremixMix");
		// Selection read (the weights pass wrote it; same-frame edge).
		BufferSyncRequest selRead{};
		selRead.barrierMasks.accessMask = AccessFlags::ShaderRead;
		selRead.barrierMasks.stageMask = PipelineStageBit_Compute;
		pass.AddBufferHint(selectionLogical, selRead);
		// Every mip's storage view is written (General read-write: the
		// storage write + the next pass's cube-view read resolve through
		// the graph's external-view aliasing).
		for (auto& logical : mipLogical) {
			ImageSyncRequest write{};
			write.barrierMasks.accessMask =
				AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
			write.barrierMasks.stageMask = PipelineStageBit_Compute;
			write.layout = ImageLayout::General;
			pass.AddImageHint(logical, write);
		}
		smvk::ShaderProgram* prog = &mixProgram;
		const uint32_t localSize = 8;
		const std::vector<uint32_t> dstIndices = mipStorageIndex;
		const uint32_t mipCountLocal = mipCount;
		const uint32_t sizeLocal = size;
		const uint64_t selBda = selectionBda;
		const uint32_t samplerLocal = samplerIndex;
		pass.SetRecordingFunc([prog, dstIndices, mipCountLocal, sizeLocal,
			selBda, samplerLocal, localSize](CommandBuffer& cmd) {
			cmd.BindComputePipeline(*prog);
			for (uint32_t m = 0; m < mipCountLocal; ++m) {
				const uint32_t faceSize =
					std::max(sizeLocal >> m, 1u);
				MixPush push{};
				push.selection = selBda;
				push.dstIndex = dstIndices[m];
				push.mip = m;
				push.faceSize = faceSize;
				push.samplerIndex = samplerLocal;
				push.mipCount = mipCountLocal;
				cmd.PushConstants(0, sizeof(push), &push,
					smvk::ShaderStage::Compute);
				const uint32_t groups =
					(faceSize + localSize - 1) / localSize;
				// z = the face layer (6) - one dispatch per mip covers
				// every (face, texel) of the level.
				cmd.Dispatch(groups, groups, smvk::kCubemapFaceCount);
			}
		});
	}
	return true;
}

} // namespace smvk::probepremix
