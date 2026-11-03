// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Renderer/GameUiRenderer.h>
#include <Engine/Utils/shader_library.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tour {
namespace {

bool Finite(glm::vec2 p) {
	return std::isfinite(p.x) && std::isfinite(p.y);
}
bool Finite(glm::vec4 p) {
	return std::isfinite(p.x) && std::isfinite(p.y) &&
		std::isfinite(p.z) && std::isfinite(p.w);
}
bool Valid(UiRect2D r) {
	return Finite(r.origin) && Finite(r.size) &&
		r.size.x > 0.0f && r.size.y > 0.0f &&
		std::isfinite(r.Right()) && std::isfinite(r.Bottom());
}

} // namespace

std::vector<GameUiPreparedRect> PrepareGameUiRects(
	std::span<const UiDrawRect2D> rects, smvk::Extent2D viewport) {
	std::vector<GameUiPreparedRect> prepared;
	if (!viewport.width || !viewport.height) return prepared;
	prepared.reserve(rects.size());
	const UiRect2D canvas{{0.0f, 0.0f},
		{float(viewport.width), float(viewport.height)}};
	for (const auto& d : rects) {
		if (!Valid(d.bounds) || !Finite(d.tint) ||
			!Finite(d.uvMin) || !Finite(d.uvMax))
			continue;
		UiRect2D clip = canvas;
		if (d.clip) {
			if (!Valid(*d.clip)) continue;
			clip = IntersectUiRects(clip, *d.clip);
		}
		clip = IntersectUiRects(clip, d.bounds);
		if (clip.size.x <= 0.0f || clip.size.y <= 0.0f)
			continue;
		GameUiPreparedRect p{};
		auto& instance = p.instance;
		instance.origin[0] = d.bounds.origin.x;
		instance.origin[1] = d.bounds.origin.y;
		instance.size[0] = d.bounds.size.x;
		instance.size[1] = d.bounds.size.y;
		instance.uvMin[0] = d.uvMin.x;
		instance.uvMin[1] = d.uvMin.y;
		instance.uvMax[0] = d.uvMax.x;
		instance.uvMax[1] = d.uvMax.y;
		instance.tint[0] = d.tint.r;
		instance.tint[1] = d.tint.g;
		instance.tint[2] = d.tint.b;
		instance.tint[3] = d.tint.a;
		instance.clipMin[0] = clip.origin.x;
		instance.clipMin[1] = clip.origin.y;
		instance.clipMax[0] = clip.Right();
		instance.clipMax[1] = clip.Bottom();
		instance.pad = d.effect; // 72-byte instance ABI: previously unused word
		p.texture = d.texture;
		prepared.push_back(p);
	}
	return prepared;
}

bool GameUiRenderer::EnsureShaders(smvk::ResourceManager& ctx) {
	if (m_program.shaderCount == 2) return true;
	try {
		if (m_vs.empty())
			m_vs = ctx.CreateShaderModule(
				shaders::Read("content/shaders/gameui.vert.hlsl.spv"),
				"gameuiVS");
		if (m_fs.empty())
			m_fs = ctx.CreateShaderModule(
				shaders::Read(m_fragmentShaderPath),
				"gameuiPS");
	} catch (...) {
		return false;
	}
	if (m_vs.empty() || m_fs.empty()) return false;
	const smvk::Shader vs{.module = m_vs, .entryPoint = "mainVS",
		.stage = smvk::ShaderStage::Vertex};
	const smvk::Shader ps{.module = m_fs, .entryPoint = "mainPS",
		.stage = smvk::ShaderStage::Fragment};
	m_program = {.shaders = {vs, ps}, .shaderCount = 2};
	return true;
}

bool GameUiRenderer::AppendPass(smvk::ResourceManager& ctx,
	smvk::RenderGraph& rg, smvk::LogicalImageViewHandle colorTarget,
	smvk::Extent2D viewport, uint32_t frameSlot, bool clear) {
	if (colorTarget.empty() || !viewport.width || !viewport.height ||
		frameSlot >= m_flights.size())
		return false;

	auto prepared = PrepareGameUiRects(m_rects, viewport);
	if (prepared.empty()) {
		if (clear) {
			rg.AddPass("GameUI clear").SetExtent(viewport).SetColor(
				{colorTarget}, smvk::AttachmentLoadOp::Clear,
				smvk::AttachmentStoreOp::Store, smvk::ClearColor(0, 0, 0, 0))
				.SetRecordingFunc([](smvk::CommandBuffer&) {});
		}
		return true;
	}
	if (prepared.size() > UINT32_MAX || !EnsureShaders(ctx))
		return false;

	// Resolve texture handles every frame: a destroyed/reloaded texture
	// cannot accidentally keep an old bindless descriptor index.
	std::vector<GameUiRectInstance> packed;
	packed.reserve(prepared.size());
	for (auto& p : prepared) {
		if (!p.texture.empty()) {
			p.instance.textureIndex = ctx.GetTextureBindlessIndex(p.texture);
			if (p.instance.textureIndex == UINT32_MAX)
				return false; // never silently render a missing image as a panel
		}
		packed.push_back(p.instance);
	}

	if (m_sampler == UINT32_MAX) {
		smvk::SamplerCreateInfo sampler{};
		sampler.addressU = smvk::AddressMode::ClampToEdge;
		sampler.addressV = smvk::AddressMode::ClampToEdge;

		sampler.addressW = smvk::AddressMode::ClampToEdge;
        if (m_pixelArtSampling) {
            sampler.minFilter = smvk::Filter::Nearest;
            sampler.magFilter = smvk::Filter::Nearest;
            sampler.mipmapMode = smvk::MipmapMode::Nearest;
            sampler.maxLod = 0.0f;
        }
		m_sampler = ctx.GetOrCreateSampler(sampler);
		if (m_sampler == UINT32_MAX) return false;
	}

	Flight& flight = m_flights[frameSlot];
	const size_t bytes = packed.size() * sizeof(GameUiRectInstance);
	if (flight.instances.empty() || bytes > flight.capacity) {
		size_t capacity = std::max<size_t>(flight.capacity, 4096);
		while (capacity < bytes) {
			if (capacity > std::numeric_limits<size_t>::max() / 2)
				return false;
			capacity *= 2;
		}
		auto buffer = ctx.CreateBuffer(
			capacity, smvk::BufferType_Storage, "gameuiInstances");
		if (buffer.empty()) return false;
		const auto logical = rg.AddExternalBuffer(buffer, "gameuiInstances");
		if (logical.empty()) return false;
		if (!flight.logical.empty()) rg.FreeBuffer(flight.logical);
		flight.instances = std::move(buffer);
		flight.logical = logical;
		flight.capacity = capacity;
	}
	ctx.UploadDataToBuffer(flight.instances, packed.data(), bytes,
		smvk::PipelineStageBit_Vertex, smvk::AccessFlags::ShaderRead);
	ctx.FlushUploads();
	Push push{};
	push.instances = ctx.GetBufferDeviceAddress(flight.instances);
	if (!push.instances) return false;
	push.viewport[0] = float(viewport.width);
	push.viewport[1] = float(viewport.height);
	push.samplerIndex = m_sampler;

	auto program = m_program;
	auto& pass = rg.AddPass("GameUI");
	pass.SetExtent(viewport).SetColor(
		{colorTarget},
		clear ? smvk::AttachmentLoadOp::Clear : smvk::AttachmentLoadOp::Load,
		smvk::AttachmentStoreOp::Store, smvk::ClearColor(0, 0, 0, 0));
	smvk::BufferSyncRequest read{};
	read.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
	read.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
	pass.AddBufferHint(flight.logical, read);
	const auto instanceCount = static_cast<uint32_t>(packed.size());
	pass.SetRecordingFunc([program, push, instanceCount](
		smvk::CommandBuffer& cmd) mutable {
		cmd.BindRenderPipeline(program, smvk::VertexLayoutId::Empty,
			smvk::DepthSettings::Disabled(),
			smvk::StencilSettings::DefaultDisabled(),
			smvk::BlendSettings::AlphaBlend(),
			smvk::MSAASettings::Count1(),
			smvk::RasterSettings{.cullMode = smvk::CullMode::None});
		cmd.PushConstants(0, sizeof(Push), &push,
			smvk::ShaderStage::AllGraphics);
		cmd.Draw(6, instanceCount, 0, 0);
	});
	return true;
}

void GameUiRenderer::Destroy(smvk::RenderGraph& rg) {
	for (auto& f : m_flights) {
		if (!f.logical.empty()) rg.FreeBuffer(f.logical);
		f = {};
	}
	// NOTE: the paren form (never `= {}` / `= ShaderProgram{}`) - GCC
	// gimplify rejects brace-init of this aggregate (smvk.h ShaderProgram).
	m_program = smvk::ShaderProgram();
	m_vs = {};
	m_fs = {};
	m_sampler = UINT32_MAX;
}

} // namespace tour
