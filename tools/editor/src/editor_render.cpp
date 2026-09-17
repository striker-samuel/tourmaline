// EditorApp viewport renderer: Unlit / Lit / Lit+Shadows forward passes.
//
// Reuses App's compiled shaders (default.vert + default_unlit/default_lit/
// default_wire + shadow.vert/masked) and App's ABI-checked FrameUniforms +
// PushConstants. Depth is reversed-Z (clear 0, GreaterEqual) like App; the
// shadow atlas is normal-Z (clear 1, LessEqual). Output goes straight to the
// swapchain (LDR preview): sun/sky intensities default to LDR-friendly
// values, exposure stays 1.0.

#include "editor_app.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Engine/Rendergraph/environment.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/bvh.h>
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

namespace {
constexpr uint32_t kMaxMaterials = 4096;
constexpr uint32_t kMaxLights = 64;
constexpr uint32_t kShadowRes = 2048;
constexpr float kLambda = 0.8f;

std::vector<char> LoadSpv(const char* path) {
	return readFile(path);
}
} // namespace

bool EditorApp::EnsureShaders() {
	if (m_litProgram.shaderCount == 2 && m_unlitProgram.shaderCount == 2 &&
		m_wireProgram.shaderCount == 2 && m_lineProgram.shaderCount == 2 &&
		m_shadowProgram.shaderCount >= 1)
		return true;
	try {
		if (m_vsModule.empty()) {
			m_vsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/default.vert.hlsl.spv"), "editorVS");
			m_lineVsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/debugline.vert.hlsl.spv"), "editorLineVS");
			m_lineFsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/debugline.frag.hlsl.spv"), "editorLineFS");
			m_unlitFsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/default_unlit.frag.hlsl.spv"), "editorUnlitFS");
			m_litFsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/default_lit.frag.hlsl.spv"), "editorLitFS");
			m_wireFsModule = m_context->CreateShaderModule(
				LoadSpv("content/shaders/default_wire.frag.hlsl.spv"), "editorWireFS");
		}
	}
	catch (const std::exception& e) {
		std::cerr << "[Editor] shader load failed: " << e.what() << "\n";
		return false;
	}
	if (m_vsModule.empty() || m_unlitFsModule.empty() || m_litFsModule.empty())
		return false;
	const smvk::Shader vs{
		.module = m_vsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	m_unlitProgram = { .shaders = { vs,
		{ .module = m_unlitFsModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment } },
		.shaderCount = 2 };
	m_litProgram = { .shaders = { vs,
		{ .module = m_litFsModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment } },
		.shaderCount = 2 };
	m_wireProgram = { .shaders = { vs,
		{ .module = m_wireFsModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment } },
		.shaderCount = 2 };
	const smvk::Shader lineVs{
		.module = m_lineVsModule, .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	m_lineProgram = { .shaders = { lineVs,
		{ .module = m_lineFsModule, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment } },
		.shaderCount = 2 };
	// Debug-view fragments (same VS as main; failure is non-fatal and the
	// view falls back to Lit at record time).
	auto tryDebugView = [&](const char* file, const char* name,
		smvk::Holder<smvk::ShaderModuleHandle>& mod, smvk::ShaderProgram& prog) {
		if (prog.shaderCount == 2)
			return;
		try {
			if (mod.empty())
				mod = m_context->CreateShaderModule(LoadSpv(file), name);
		}
		catch (const std::exception& e) {
			std::cerr << "[Editor] debug shader unavailable (" << file << "): "
				<< e.what() << "\n";
			return;
		}
		if (mod.empty())
			return;
		prog = { .shaders = { vs,
			{ .module = mod, .entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment } },
			.shaderCount = 2 };
	};
	tryDebugView("content/shaders/default_uv0_debug.frag.hlsl.spv",
		"editorUv0FS", m_uv0FsModule, m_uv0Program);
	tryDebugView("content/shaders/default_uv1_debug.frag.hlsl.spv",
		"editorUv1FS", m_uv1FsModule, m_uv1Program);
	tryDebugView("content/shaders/default_vertexcolor_debug.frag.hlsl.spv",
		"editorVColorFS", m_vcolorFsModule, m_vcolorProgram);
	// Samplers: mirror App order (0 = linear clamp, 1 = point). The lit
	// shader's comparisonSamplerId = 1 addresses the second slot.
	m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
		.magFilter = smvk::Filter::Linear, .minFilter = smvk::Filter::Linear,
		.mipmapMode = smvk::MipmapMode::Linear,
		.addressU = smvk::AddressMode::ClampToEdge,
		.addressV = smvk::AddressMode::ClampToEdge,
		.addressW = smvk::AddressMode::ClampToEdge, .maxAnisotropy = 1.0f });
	m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
		.magFilter = smvk::Filter::Nearest, .minFilter = smvk::Filter::Nearest,
		.mipmapMode = smvk::MipmapMode::Nearest,
		.addressU = smvk::AddressMode::ClampToEdge,
		.addressV = smvk::AddressMode::ClampToEdge,
		.addressW = smvk::AddressMode::ClampToEdge, .maxAnisotropy = 1.0f });
	return true;
}

bool EditorApp::EnsureBuffers(uint32_t flightCount) {
	if (m_flightCount == flightCount && !m_frameUniforms.empty())
		return true;
	m_flightCount = flightCount;
	m_frameUniforms = m_context->CreateBuffer(
		sizeof(app::FrameUniforms) * flightCount, smvk::BufferType_Uniform, "editor frame");
	m_materialsBuf = m_context->CreateBuffer(
		sizeof(smvk::MaterialValues) * kMaxMaterials * flightCount,
		smvk::BufferType_Storage, "editor materials");
	m_lightsBuf = m_context->CreateBuffer(
		sizeof(smvk::Light) * kMaxLights * flightCount,
		smvk::BufferType_Storage, "editor lights");
	m_cascadeBuf = m_context->CreateBuffer(
		sizeof(smvk::GpuDirectionalLightDescription) * flightCount,
		smvk::BufferType_Storage, "editor cascades");
	return !m_frameUniforms.empty() && !m_materialsBuf.empty() &&
		!m_lightsBuf.empty() && !m_cascadeBuf.empty();
}

bool EditorApp::EnsureDepth(smvk::Extent2D extent) {
	if (m_depth.Valid() && m_depth.extent.width == extent.width &&
		m_depth.extent.height == extent.height)
		return true;
	smvk::DepthDesc desc{};
	desc.format = m_swapchain->GetDepthFormat();
	desc.name = "editorDepth";
	desc.viewName = "editorDepthView";
	desc.logicalName = "editorDepthLogical";
	return m_depth.Ensure(*m_context, *m_rendergraph, extent, desc);
}

bool EditorApp::EnsureShadowAtlas() {
	smvk::CascadedShadowMapDesc desc{};
	desc.cascadeCount = 1; // simple shadows: single cascade
	desc.cascadeResolution = kShadowRes;
	desc.format = smvk::Format::D32_Float;
	desc.lambda = kLambda;
	desc.name = "editorShadowAtlas";
	if (!m_shadowAtlas.Ensure(*m_context, *m_rendergraph, desc))
		return false;
	m_shadowProgram = m_shadowAtlas.program;
	return m_shadowAtlas.ShadersValid();
}

void EditorApp::UpdateFrameData(uint32_t flight, smvk::Extent2D extent) {
	const bool wantShadows = (m_viewMode == ViewMode::LitShadows);
	if (wantShadows)
		EnsureShadowAtlas();

	auto view = m_camera.getViewMatrix();
	auto proj = m_camera.getProjMatrix();
	if (proj == glm::mat4(0.0f)) {
		m_camera.setPerspective(glm::radians(45.0f),
			extent.width > 0 && extent.height > 0
			? float(extent.width) / float(extent.height) : 16.0f / 9.0f,
			kEditorNearClip, kEditorFarClip);
		proj = m_camera.getProjMatrix();
	}
	proj[1][1] *= -1.0f; // Vulkan Y-flip
	const glm::mat4 viewProj = proj * view;

	app::FrameUniforms frame{};
	frame.view = view;
	frame.proj = proj;
	frame.viewProj = viewProj;
	frame.invView = glm::inverse(view);
	frame.invProj = glm::inverse(proj);
	frame.invViewProj = glm::inverse(viewProj);
	frame.prevViewProj = viewProj;
	frame.cameraPos = m_camera.getPosition();
	frame.time = time_since_start_s;
	frame.viewportSize = glm::vec2(float(extent.width), float(extent.height));
	frame.invViewportSize = glm::vec2(
		extent.width ? 1.0f / float(extent.width) : 0.0f,
		extent.height ? 1.0f / float(extent.height) : 0.0f);
	frame.nearPlane = kEditorNearClip;
	frame.farPlane = kEditorFarClip;
	frame.exposure = m_exposure;
	frame.aoTextureIndex = UINT32_MAX;
	frame.aoIntensity = 0.0f;
	frame.evsmMode = 0;
	frame.decalCount = 0;
	frame.lmEnabled = 0;
	frame.regEnabled = 0;
	frame.regCount = 0;
	frame.dsEnabled = 0;
	frame.giEnabled = 0;
	frame.giDebug = 0;

	smvk::EnvironmentSettings env{};
	env.skyEnabled = true;
	env.iblEnabled = true;
	env.skyZenithColor = m_skyColor;
	env.skyHorizonColor = m_skyColor;
	env.skyIntensity = m_skyIntensity;
	env.groundColor = m_skyColor * 0.5f;
	env.iblIntensity = m_skyIntensity;
	env.sunAzimuthDeg = m_sunAzimuth;
	env.sunElevationDeg = m_sunElevation;
	env.sunColor = m_sunColor;
	env.sunIntensity = (m_viewMode == ViewMode::Unlit) ? 0.0f : m_sunIntensity;
	env.fogMode = 0;
	frame.env = env.ToFrameData();

	m_lastFrame = frame;
	m_lastViewProj = viewProj;

	m_context->UploadDataToBuffer(m_frameUniforms, &frame, sizeof(frame),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
		smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(frame));

	// Sun light (lights[0], same convention as App).
	smvk::Light lights[kMaxLights]{};
	{
		const glm::vec3 toSun = env.SunDirectionToSun();
		smvk::Light& l = lights[0];
		l.color = glm::vec4(m_sunColor, 1.0f);
		l.direction[0] = -toSun.x;
		l.direction[1] = -toSun.y;
		l.direction[2] = -toSun.z;
		l.intensity = (m_viewMode == ViewMode::Unlit) ? 0.0f : m_sunIntensity;
		l.shadow_map_index = -1;
	}
	int32_t shadowBindless = -1;
	if (wantShadows && m_shadowAtlas.Valid())
		shadowBindless = int32_t(m_context->GetBindlessIndex(m_shadowAtlas.bindless));
	lights[0].shadow_map_index = shadowBindless;

	// Cascade descriptor (single cascade over the main view range).
	smvk::GpuDirectionalLightDescription dir{};
	if (wantShadows && m_shadowAtlas.Valid()) {
		auto camProj = m_camera.getProjMatrix();
		camProj[1][1] *= -1.0f;
		const auto splits = smvk::ComputeCascadeSplits(
			kEditorNearClip, std::min(kEditorFarClip, 500.0f), kLambda, 1);
		dir = m_shadowAtlas.BuildDirectionalLight(
			glm::vec3(lights[0].direction[0], lights[0].direction[1],
				lights[0].direction[2]),
			lights[0].color, lights[0].intensity,
			shadowBindless, frame.invView, frame.invProj, splits, 10.0f);
		// Keep the legacy single-VP slot in sync (shadow.vert fallback).
		std::memcpy(lights[0].lightVP, dir.lightVP, sizeof(lights[0].lightVP));
	}
	m_context->UploadDataToBuffer(m_lightsBuf, lights, sizeof(lights),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
		smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(lights));

	m_context->UploadDataToBuffer(m_cascadeBuf, &dir, sizeof(dir),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
		smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(dir));

	UploadMaterials(flight);
}

void EditorApp::UploadMaterials(uint32_t flight) {
	if (!m_world)
		return;
	const uint32_t slotCount =
		std::min<uint32_t>(uint32_t(m_world->materialLocations.size()), kMaxMaterials);
	std::vector<smvk::MaterialValues> mats(slotCount);
	for (uint32_t i = 0; i < slotCount; ++i)
		if (auto* mat = m_world->GetMaterial(i))
			mats[i] = mat->values;
	if (!mats.empty())
		m_context->UploadDataToBuffer(m_materialsBuf, mats.data(),
			sizeof(smvk::MaterialValues) * mats.size(),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
			smvk::AccessFlags::ShaderRead, 0,
			size_t(flight) * sizeof(smvk::MaterialValues) * kMaxMaterials);
}

void EditorApp::DrawWorldNodes(smvk::CommandBuffer& cmd,
	const app::PushConstants& basePc, bool maskedPhase, bool selectedOnly,
	bool shadowPass) {
	(void)basePc;
	if (!m_world)
		return;
	const smvk::IteratorFlags req =
		smvk::IteratorFlags(smvk::IteratorFlagBit_Renderable |
			smvk::IteratorFlagBit_Opaque | smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (item.material == smvk::World::kInvalidIndex)
			return;
		const bool isMasked = (item.flags & smvk::IteratorFlagBit_Masked) != 0;
		if (isMasked != maskedPhase)
			return;
		if (selectedOnly && item.node != m_selected)
			return;
		if (!shadowPass) {
			const smvk::Material* mat = m_world->GetMaterial(item.material);
			if (mat && mat->isTransparent())
				return; // transparent leg draws separately
		}
		else {
			// Shadow pass: opaque casters only (transparent never casts).
			if ((item.flags & smvk::IteratorFlagBit_CastShadow) == 0)
				return;
		}
		smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		app::PushConstants objPc{};
		std::memcpy(objPc.model, glm::value_ptr(model), sizeof(objPc.model));
		objPc.materialId = item.material;
		cmd.PushConstants(offsetof(app::PushConstants, materialId),
			sizeof(objPc.materialId), &objPc.materialId, smvk::ShaderStage::All);
		cmd.PushConstants(offsetof(app::PushConstants, model),
			sizeof(objPc.model), objPc.model, smvk::ShaderStage::All);
		for (const auto& s : mesh->geometry.surfaces)
			cmd.DrawIndexed(s.indexCount, 1, s.firstIndex, s.baseVertex);
		});
}

bool EditorApp::HasDrawItems() const {
	return m_world && !m_world->drawItems.empty();
}

void EditorApp::RecordShadowPass(uint32_t cascadeIndex) {
	if (!m_shadowAtlas.Valid() || m_shadowProgram.shaderCount == 0)
		return;
	// shadow.vert resolves its VP from pc.cascadeBuffer/cascadeIndex (or
	// the lights[0].lightVP fallback), so the pass push carries the real
	// buffer addresses — never null BDAs.
	smvk::RenderPass& pass = m_shadowAtlas.AddCascadePass(
		*m_rendergraph, cascadeIndex, "EditorShadow");
	pass.SetRecordingFunc([this, cascadeIndex](smvk::CommandBuffer& cmd) {
		// BindRenderPipeline re-binds the static monoblock internally for
		// StaticVertexFormat (which does not exist on an empty world), so
		// empty passes bind Empty (no draws either way).
		const bool hasDraws = HasDrawItems();
		if (!hasDraws)
			return;
		cmd.BindStaticVertexFmt();
		cmd.BindRenderPipeline(m_shadowProgram,
			smvk::VertexFormat::StaticVertexFormat,
			smvk::DepthSettings{ .depthFunc = smvk::CompareOp::LessEqual,
				.writeEnable = true, .depthTestEnable = true },
			smvk::StencilSettings{}, smvk::BlendSettings{},
			smvk::MSAASettings{}, smvk::RasterSettings{ .cullMode = smvk::CullMode::Back });
		app::PushConstants pc{};
		pc.frameUniforms = m_lastFrameAddrs.frame;
		pc.lightsBuffer = m_lastFrameAddrs.lights;
		pc.materialsBuffer = m_lastFrameAddrs.materials;
		pc.cascadeBuffer = m_lastFrameAddrs.cascades;
		pc.numCascades = 1;
		pc.cascadeIndex = cascadeIndex;
		pc.renderViewIndex = UINT32_MAX;
		pc.spotIndex = UINT32_MAX;
		cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
		DrawWorldNodes(cmd, pc, false /*opaque*/, false, true);
		});
}

smvk::ShaderProgram& EditorApp::MainProgram() {
	switch (m_viewMode) {
	case ViewMode::Unlit:
		return m_unlitProgram;
	case ViewMode::Uv0:
		return m_uv0Program.shaderCount == 2 ? m_uv0Program : m_litProgram;
	case ViewMode::Uv1:
		return m_uv1Program.shaderCount == 2 ? m_uv1Program : m_litProgram;
	case ViewMode::VertexColors:
		return m_vcolorProgram.shaderCount == 2 ? m_vcolorProgram : m_litProgram;
	default:
		return m_litProgram;
	}
}

void EditorApp::RecordMainPass(smvk::LogicalImageViewHandle swapView,
	smvk::Extent2D extent, uint32_t flight, bool selectedWireOverlay) {
	(void)extent;
	const bool shadows = (m_viewMode == ViewMode::LitShadows) && m_shadowAtlas.Valid();
	smvk::ShaderProgram& prog = MainProgram();
	if (prog.shaderCount != 2)
		return;

	const uint64_t frameAddr =
		m_context->GetBufferDeviceAddress(m_frameUniforms) + flight * sizeof(app::FrameUniforms);
	const uint64_t lightsAddr =
		m_context->GetBufferDeviceAddress(m_lightsBuf) + flight * sizeof(smvk::Light) * kMaxLights;
	const uint64_t matsAddr =
		m_context->GetBufferDeviceAddress(m_materialsBuf) +
		flight * sizeof(smvk::MaterialValues) * kMaxMaterials;
	const uint64_t cascadeAddr =
		m_context->GetBufferDeviceAddress(m_cascadeBuf) +
		flight * sizeof(smvk::GpuDirectionalLightDescription);
	m_lastFrameAddrs = FrameAddrs{ frameAddr, lightsAddr, matsAddr, cascadeAddr };

	app::PushConstants pc{};
	pc.frameUniforms = frameAddr;
	pc.lightsBuffer = lightsAddr;
	pc.materialsBuffer = matsAddr;
	pc.cascadeBuffer = cascadeAddr;
	pc.materialId = 0;
	pc.comparisonSamplerId = 1;
	pc.numCascades = shadows ? 1 : 0;
	pc.cascadeIndex = 0;
	pc.fpLightCount = 0;
	pc.spotShadowMapIndex = UINT32_MAX;
	pc.spotShadowCount = 0;
	pc.spotIndex = UINT32_MAX;
	pc.pad_pc = 0; // dithered transparency
	pc.renderViews = 0;
	pc.mainRenderView = UINT32_MAX;
	pc.renderViewIndex = UINT32_MAX;

	auto& pass = m_rendergraph->AddPass("EditorMain")
		.SetOffset(0, 0)
		.SetExtent(extent)
		.SetColor({ swapView },
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0.07f, 0.07f, 0.09f, 1.0f))
		.SetDepth(m_depth.logicalView,
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
	pass.SetRecordingFunc([this, pc, selectedWireOverlay](smvk::CommandBuffer& cmd) {
		smvk::ShaderProgram& prog = MainProgram();
		// BindRenderPipeline re-binds the static monoblock internally for
		// StaticVertexFormat (which does not exist on an empty world), so
		// empty passes bind Empty (no draws either way).
		const smvk::VertexFormat fmt =
			HasDrawItems() ? smvk::VertexFormat::StaticVertexFormat
			: smvk::VertexFormat::Empty;
		if (fmt == smvk::VertexFormat::StaticVertexFormat)
			cmd.BindStaticVertexFmt();
		const smvk::DepthSettings depth{
			.depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = true, .depthTestEnable = true };
		// Opaque leg (opaque + masked, back-cull; double-sided/masked -> none).
		for (int masked = 0; masked <= 1; ++masked) {
			const bool maskedPhase = (masked == 1);
			cmd.BindRenderPipeline(prog, fmt,
				depth, smvk::StencilSettings{}, smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{ .cullMode = maskedPhase
					? smvk::CullMode::None : smvk::CullMode::Back });
			cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
			DrawWorldNodes(cmd, pc, maskedPhase, false, false);
		}
		// Transparent leg (alpha blend, no depth write).
		{
			cmd.BindRenderPipeline(prog, fmt,
				smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false, .depthTestEnable = true },
				smvk::StencilSettings{},
				smvk::BlendSettings::AlphaBlend(),
				smvk::MSAASettings{}, smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
			app::PushConstants tpc = pc;
			tpc.pad_pc = 1; // classic alpha blend
			cmd.PushConstants(0, sizeof(tpc), &tpc, smvk::ShaderStage::All);
			if (m_world) {
				const smvk::IteratorFlags req = smvk::IteratorFlags(
					smvk::IteratorFlagBit_Renderable |
					smvk::IteratorFlagBit_Transparent |
					smvk::IteratorFlagBit_StaticVertexFmt);
				m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
					if (item.material == smvk::World::kInvalidIndex)
						return;
					smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
					if (!mesh)
						return;
					const glm::mat4& model = m_world->scene.globalTransform[item.node];
					uint32_t matId = item.material;
					cmd.PushConstants(offsetof(app::PushConstants, materialId),
						sizeof(matId), &matId, smvk::ShaderStage::All);
					cmd.PushConstants(offsetof(app::PushConstants, model),
						sizeof(model), glm::value_ptr(model), smvk::ShaderStage::All);
					for (const auto& s : mesh->geometry.surfaces)
						cmd.DrawIndexed(s.indexCount, 1, s.firstIndex, s.baseVertex);
					});
			}
		}
		// Selection highlight: wireframe overlay over the selected node.
		if (selectedWireOverlay && m_selected != UINT32_MAX &&
			m_wireProgram.shaderCount == 2) {
			cmd.BindRenderPipeline(m_wireProgram,
				smvk::VertexFormat::StaticVertexFormat,
				smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false, .depthTestEnable = true },
				smvk::StencilSettings{}, smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{ .polygonMode = smvk::PolygonMode::Line,
					.cullMode = smvk::CullMode::None });
			cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
			DrawWorldNodes(cmd, pc, false, true, false);
			DrawWorldNodes(cmd, pc, true, true, false);
		}
		});
}

void EditorApp::RenderFrame(smvk::Framebuffer& fb) {
	const auto vkExt = m_swapchain->GetExtent();
	const smvk::Extent2D extent{ vkExt.width, vkExt.height };
	if (extent.width == 0 || extent.height == 0)
		return;
	if (!EnsureShaders())
		return;
	// External swap views persist in the rendergraph (like App/triangle):
	// register once, not per frame.
	if (m_swapViews.size() != m_swapchain->GetImageCount()) {
		m_swapViews.clear();
		for (uint32_t i = 0; i < m_swapchain->GetImageCount(); ++i)
			m_swapViews.push_back(m_rendergraph->AddExternalImageView(
				m_swapchain->GetImageViewHandle(i), m_swapchain->GetSurfaceFormat()));
	}
	if (!EnsureBuffers(m_swapchain->framesInFlight()))
		return;
	if (!EnsureDepth(extent))
		return;

	// Camera aspect follows the swap extent (1:1 viewport).
	m_camera.setPerspective(glm::radians(kEditorFovYDeg),
		float(extent.width) / float(extent.height), kEditorNearClip, kEditorFarClip);

	UpdateCamera(delta_time_s);
	if (!m_uiMode || !(m_imgui && m_imgui->WantsCaptureKeyboard())) {
		if (m_input.isKeyPressed(GLFW_KEY_F1))
			m_viewMode = ViewMode::Unlit;
		if (m_input.isKeyPressed(GLFW_KEY_F2))
			m_viewMode = ViewMode::Lit;
		if (m_input.isKeyPressed(GLFW_KEY_F3))
			m_viewMode = ViewMode::LitShadows;
		if (m_input.isKeyPressed(GLFW_KEY_F4))
			m_viewMode = ViewMode::Uv0;
		if (m_input.isKeyPressed(GLFW_KEY_F5))
			m_viewMode = ViewMode::Uv1;
		if (m_input.isKeyPressed(GLFW_KEY_F6))
			m_viewMode = ViewMode::VertexColors;
		if (m_input.isKeyPressed(GLFW_KEY_I))
			m_uiMode = !m_uiMode;
		if (m_input.isKeyPressed(GLFW_KEY_G))
			m_gizmoMode = GizmoMode((int(m_gizmoMode) + 1) % 3);
		if (m_input.isKeyPressed(GLFW_KEY_1))
			m_gizmoMode = GizmoMode::Translate;
		if (m_input.isKeyPressed(GLFW_KEY_2))
			m_gizmoMode = GizmoMode::Rotate;
		if (m_input.isKeyPressed(GLFW_KEY_3))
			m_gizmoMode = GizmoMode::Scale;
		if (m_input.isKeyPressed(GLFW_KEY_F)) {
			const bool shift = m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
				m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);
			if (shift)
				FrameAll();
			else
				FrameSelection();
		}
		const bool ctrl = m_input.isKeyHeld(GLFW_KEY_LEFT_CONTROL) ||
			m_input.isKeyHeld(GLFW_KEY_RIGHT_CONTROL);
		const bool shift = m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
			m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);
		if (ctrl && m_input.isKeyPressed(GLFW_KEY_Z) && !shift)
			Undo();
		if (ctrl && ((m_input.isKeyPressed(GLFW_KEY_Z) && shift) ||
			m_input.isKeyPressed(GLFW_KEY_Y)))
			Redo();
	}

	const uint32_t flight = uint32_t(fb.imageIndex) % m_flightCount;
	UpdateTextureStreaming(extent);
	UpdateFrameData(flight, extent);

	// Viewport click-select (left button, UI not capturing, gizmo idle,
	// Alt not held: Alt+LMB is the Maya orbit gesture, not a pick).
	const bool altNav = m_input.isKeyHeld(GLFW_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_ALT);
	if (m_world && !m_gizmoDragging && !altNav && m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) &&
		!(m_imgui && m_imgui->WantsCaptureMouse())) {
		if (!GizmoHandleInput(extent)) {
			if (auto hit = PickNode(float(m_input.getMouseX()),
				float(m_input.getMouseY()), extent)) {
				m_selected = hit->node;
				m_revealNode = hit->node; // outline it in the outliner
			}
			else
				m_selected = UINT32_MAX;
		}
	}
	else if (m_gizmoDragging || (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) &&
		!altNav && !(m_imgui && m_imgui->WantsCaptureMouse()))) {
		GizmoHandleInput(extent);
	}

	m_rendergraph->NewFrame();
	if (m_viewMode == ViewMode::LitShadows)
		RecordShadowPass(0);
	RecordMainPass(m_swapViews[fb.imageIndex], extent, flight, m_wireOverlay);
	RecordGizmoPass(m_swapViews[fb.imageIndex], extent);

	if (m_imgui && m_imgui->IsInitialized()) {
		m_imgui->NewFrame(extent);
		DrawUI(extent);
		m_imgui->AddPass(*m_rendergraph, m_swapViews[fb.imageIndex], extent,
			smvk::AttachmentLoadOp::Load, smvk::ClearColor(0, 0, 0, 1));
	}
	m_rendergraph->Execute(fb);
	m_context->ProcessDeferredTasks();
	++m_frameCounter;
}

} // namespace editor
