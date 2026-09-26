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
	tryDebugView("content/shaders/default_baked_debug.frag.hlsl.spv",
		"editorBakedFS", m_bakedFsModule, m_bakedProgram);
	// Meshlet debug view (F4): App's meshdebug program (position-only VS +
	// flat push color). Failure is non-fatal: the view falls back to Lit.
	if (m_meshDebugProgram.shaderCount != 2) {
		try {
			if (m_meshDebugVsModule.empty())
				m_meshDebugVsModule = m_context->CreateShaderModule(
					LoadSpv("content/shaders/meshdebug.vert.hlsl.spv"),
					"editorMeshDebugVS");
			if (m_meshDebugFsModule.empty())
				m_meshDebugFsModule = m_context->CreateShaderModule(
					LoadSpv("content/shaders/meshdebug.frag.hlsl.spv"),
					"editorMeshDebugFS");
		}
		catch (const std::exception& e) {
			std::cerr << "[Editor] meshdebug shader unavailable: "
				<< e.what() << "\n";
		}
		if (!m_meshDebugVsModule.empty() && !m_meshDebugFsModule.empty())
			m_meshDebugProgram = { .shaders = {
				{ .module = m_meshDebugVsModule, .entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex },
				{ .module = m_meshDebugFsModule, .entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment } },
				.shaderCount = 2 };
	}
	// Flat UV views (per-panel type): mesh projected into UV space with
	// its texture content. Failure is non-fatal: the panel falls back to
	// the 3D UV debug view.
	auto tryFlatUv = [&](const char* vsFile, const char* fsFile,
		const char* vsName, const char* fsName,
		smvk::Holder<smvk::ShaderModuleHandle>& vsMod,
		smvk::Holder<smvk::ShaderModuleHandle>& fsMod,
		smvk::ShaderProgram& prog) {
		if (prog.shaderCount == 2)
			return;
		try {
			if (vsMod.empty())
				vsMod = m_context->CreateShaderModule(LoadSpv(vsFile), vsName);
			if (fsMod.empty())
				fsMod = m_context->CreateShaderModule(LoadSpv(fsFile), fsName);
		}
		catch (const std::exception& e) {
			std::cerr << "[Editor] flat-uv shader unavailable (" << vsFile
				<< "): " << e.what() << "\n";
			return;
		}
		if (!vsMod.empty() && !fsMod.empty())
			prog = { .shaders = {
				{ .module = vsMod, .entryPoint = "mainVS",
					.stage = smvk::ShaderStage::Vertex },
				{ .module = fsMod, .entryPoint = "mainPS",
					.stage = smvk::ShaderStage::Fragment } },
				.shaderCount = 2 };
	};
	tryFlatUv("content/shaders/uvflat0.vert.hlsl.spv",
		"content/shaders/uvflat0.frag.hlsl.spv",
		"editorFlatUv0VS", "editorFlatUv0FS",
		m_flatUv0VsModule, m_flatUv0FsModule, m_flatUv0Program);
	tryFlatUv("content/shaders/uvflat1.vert.hlsl.spv",
		"content/shaders/uvflat1.frag.hlsl.spv",
		"editorFlatUv1VS", "editorFlatUv1FS",
		m_flatUv1VsModule, m_flatUv1FsModule, m_flatUv1Program);
	// UV-coordinate PNG export: same unwrapped VS, fragment outputs the
	// atlas uv itself (uvflat1uv.frag) — no atlas content read.
	tryFlatUv("content/shaders/uvflat1.vert.hlsl.spv",
		"content/shaders/uvflat1uv.frag.hlsl.spv",
		"editorFlatUv1VS", "editorFlatUvCoordFS",
		m_flatUv1VsModule, m_flatUvCoordFsModule, m_flatUvCoordProgram);
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
		sizeof(app::FrameUniforms) * flightCount * kMaxViewports,
		smvk::BufferType_Uniform, "editor frames");
	m_insetFrameUniforms = m_context->CreateBuffer(
		sizeof(app::FrameUniforms) * flightCount, smvk::BufferType_Uniform,
		"editor inset frame");
	m_materialsBuf = m_context->CreateBuffer(
		sizeof(smvk::MaterialValues) * kMaxMaterials * flightCount,
		smvk::BufferType_Storage, "editor materials");
	m_lightsBuf = m_context->CreateBuffer(
		sizeof(smvk::Light) * kMaxLights * flightCount,
		smvk::BufferType_Storage, "editor lights");
	m_cascadeBuf = m_context->CreateBuffer(
		sizeof(smvk::GpuDirectionalLightDescription) * flightCount,
		smvk::BufferType_Storage, "editor cascades");
	return !m_frameUniforms.empty() && !m_insetFrameUniforms.empty() &&
		!m_materialsBuf.empty() && !m_lightsBuf.empty() && !m_cascadeBuf.empty();
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

void EditorApp::UpdateFrameData(uint32_t flight) {
	SetupDefaultCameras();
	const bool wantShadows = (ActiveViewMode() == ViewMode::LitShadows);
	if (wantShadows)
		EnsureShadowAtlas();

	// One frame slice per (flight, viewport): each viewport renders
	// with ITS camera (aspect from its rect). The ACTIVE viewport also
	// drives the light buffer + shadow cascade.
	const uint64_t baseAddr =
		m_context->GetBufferDeviceAddress(m_frameUniforms);
	const size_t sliceStride = sizeof(app::FrameUniforms);
	for (int vp = 0; vp < ViewportCount(); ++vp) {
		const EditorCam& cam = m_editorCams[m_viewports[vp].camIdx];
		const ViewportRect& rect = m_viewports[vp].rect;
		const float aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
		const glm::mat4 view = EditorCamView(cam);
		const glm::mat4 proj = EditorCamProj(cam, aspect);
		const glm::mat4 viewProj = proj * view;

		app::FrameUniforms frame{};
		frame.view = view;
		frame.proj = proj;
		frame.viewProj = viewProj;
		frame.invView = glm::inverse(view);
		frame.invProj = glm::inverse(proj);
		frame.invViewProj = glm::inverse(viewProj);
		frame.prevViewProj = viewProj;
		frame.cameraPos = EditorCamEye(cam);
		frame.time = time_since_start_s;
		frame.viewportSize = glm::vec2(rect.w, rect.h);
		frame.invViewportSize = glm::vec2(
			rect.w > 0.0f ? 1.0f / rect.w : 0.0f,
			rect.h > 0.0f ? 1.0f / rect.h : 0.0f);
		frame.nearPlane = cam.nearClip;
		frame.farPlane = cam.farClip;
		frame.exposure = CurrentEnv().exposure;
		frame.aoTextureIndex = UINT32_MAX;
		frame.aoIntensity = 0.0f;
		frame.evsmMode = 0;
		frame.decalCount = 0;
		frame.lmEnabled = 0;
		// Baked-irradiance atlas (F8 view): bindless layer slots ride the
		// frame; the direct-draw VS never resolves per-draw chunks, so the
		// lit path ignores this while the baked debug FS samples layer 0
		// (texture 0 = lobe 0 in both GPU layouts).
		if (m_lmGpu) {
			for (uint32_t t = 0; t < 12; ++t)
				frame.lmLayers[t] = m_lmLayerSlots[t];
			frame.lmLayerKind = m_lmLayerKind;
			frame.lmEnabled = 1;
		}
		frame.regEnabled = 0;
		frame.regCount = 0;
		frame.dsEnabled = 0;
		frame.giEnabled = 0;
		frame.giDebug = 0;

		smvk::EnvironmentSettings env{};
		ApplyEnvToFrame(frame, env);
		frame.env = env.ToFrameData();

		m_viewports[vp].frame = frame;
		m_viewports[vp].viewProj = viewProj;
		m_viewports[vp].frameAddr = baseAddr +
			(size_t(flight) * kMaxViewports + size_t(vp)) * sliceStride;
		m_context->UploadDataToBuffer(m_frameUniforms, &frame, sizeof(frame),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
			smvk::AccessFlags::ShaderRead, 0,
			(size_t(flight) * kMaxViewports + size_t(vp)) * sliceStride);
	}
	// Legacy single-frame mirrors (shadow pass, insets): the ACTIVE vp.
	m_lastFrame = m_viewports[m_activeVp].frame;
	m_lastViewProj = m_viewports[m_activeVp].viewProj;
	m_lastFrameAddrs = FrameAddrs{
		.frame = m_viewports[m_activeVp].frameAddr,
		.lights = m_context->GetBufferDeviceAddress(m_lightsBuf) +
			flight * sizeof(smvk::Light) * kMaxLights,
		.materials = m_context->GetBufferDeviceAddress(m_materialsBuf) +
			flight * sizeof(smvk::MaterialValues) * kMaxMaterials,
		.cascades = m_context->GetBufferDeviceAddress(m_cascadeBuf) +
			flight * sizeof(smvk::GpuDirectionalLightDescription) };

	// Sun light (lights[0], same convention as App) from the CURRENT env.
	const EditorEnv& envSet = CurrentEnv();
	smvk::EnvironmentSettings env{};
	env.sunAzimuthDeg = envSet.sunAzimuth;
	env.sunElevationDeg = envSet.sunElevation;
	const glm::vec3 toSun = env.SunDirectionToSun();
	smvk::Light lights[kMaxLights]{};
	{
		smvk::Light& l = lights[0];
		l.color = glm::vec4(envSet.sunColor, 1.0f);
		l.direction[0] = -toSun.x;
		l.direction[1] = -toSun.y;
		l.direction[2] = -toSun.z;
		l.intensity = (ActiveViewMode() == ViewMode::Unlit) ? 0.0f : envSet.sunIntensity;
		l.shadow_map_index = -1;
	}
	int32_t shadowBindless = -1;
	if (wantShadows && m_shadowAtlas.Valid())
		shadowBindless = int32_t(m_context->GetBindlessIndex(m_shadowAtlas.bindless));
	lights[0].shadow_map_index = shadowBindless;

	// Cascade descriptor (single cascade over the ACTIVE view range).
	smvk::GpuDirectionalLightDescription dir{};
	if (wantShadows && m_shadowAtlas.Valid()) {
		const EditorCam& cam = ActiveCam();
		const auto splits = smvk::ComputeCascadeSplits(
			cam.nearClip, std::min(cam.farClip, 500.0f), kLambda, 1);
		dir = m_shadowAtlas.BuildDirectionalLight(
			glm::vec3(lights[0].direction[0], lights[0].direction[1],
				lights[0].direction[2]),
			lights[0].color, lights[0].intensity,
			shadowBindless, m_lastFrame.invView, m_lastFrame.invProj,
			splits, 10.0f);
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

void EditorApp::ApplyEnvToFrame(app::FrameUniforms& frame,
	smvk::EnvironmentSettings& env) const {
	const EditorEnv& e = CurrentEnv();
	env.skyEnabled = true;
	env.iblEnabled = true;
	env.skyZenithColor = e.skyColor;
	env.skyHorizonColor = e.skyColor;
	env.skyIntensity = e.skyIntensity;
	env.groundColor = e.skyColor * 0.5f;
	env.iblIntensity = e.skyIntensity;
	env.sunAzimuthDeg = e.sunAzimuth;
	env.sunElevationDeg = e.sunElevation;
	env.sunColor = e.sunColor;
	env.sunIntensity = (ActiveViewMode() == ViewMode::Unlit) ? 0.0f : e.sunIntensity;
	env.fogMode = 0; // volumetrics authored, not yet rendered here
}

EditorApp::EditorEnv& EditorApp::CurrentEnv() {
	if (m_envCurrent == UINT32_MAX)
		return m_envDefault;
	auto it = m_envByNode.find(m_envCurrent);
	if (it == m_envByNode.end()) {
		m_envCurrent = UINT32_MAX;
		return m_envDefault;
	}
	return it->second;
}

const EditorApp::EditorEnv& EditorApp::CurrentEnv() const {
	if (m_envCurrent == UINT32_MAX)
		return m_envDefault;
	auto it = m_envByNode.find(m_envCurrent);
	return (it != m_envByNode.end()) ? it->second : m_envDefault;
}

void EditorApp::UploadMaterials(uint32_t flight) {
	if (!m_world)
		return;
	// Pool index -> bindless slot for the shader's textures[] array
	// (materials store pool indices; sampling needs descriptor slots).
	auto remap = [&](int poolIdx) -> int {
		if (poolIdx < 0)
			return -1;
		auto hit = m_bindlessBySlot.find(static_cast<uint32_t>(poolIdx));
		return hit != m_bindlessBySlot.end()
			? static_cast<int>(hit->second) : poolIdx;
	};
	const uint32_t slotCount =
		std::min<uint32_t>(uint32_t(m_world->materialLocations.size()), kMaxMaterials);
	std::vector<smvk::MaterialValues> mats(slotCount);
	for (uint32_t i = 0; i < slotCount; ++i) {
		if (auto* mat = m_world->GetMaterial(i)) {
			mats[i] = mat->values;
			mats[i].textures.x = remap(mat->values.textures.x);
			mats[i].textures.y = remap(mat->values.textures.y);
			mats[i].textures.z = remap(mat->values.textures.z);
			mats[i].textures.w = remap(mat->values.textures.w);
			mats[i].textures2.x = remap(mat->values.textures2.x);
		}
	}
	if (!mats.empty())
		m_context->UploadDataToBuffer(m_materialsBuf, mats.data(),
			sizeof(smvk::MaterialValues) * mats.size(),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
			smvk::AccessFlags::ShaderRead, 0,
			size_t(flight) * sizeof(smvk::MaterialValues) * kMaxMaterials);
}

// ---- editor cameras (orbit frame; perspective or orthographic) ---------
glm::vec3 EditorCamEye(const EditorCam& c) {
	return c.target + c.dist * glm::vec3(
		std::cos(c.pitch) * std::sin(c.yaw),
		-std::sin(c.pitch),
		std::cos(c.pitch) * std::cos(c.yaw));
}

glm::mat4 EditorCamView(const EditorCam& c) {
	const glm::vec3 eye = EditorCamEye(c);
	return glm::lookAt(eye, c.target, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamProj(const EditorCam& c, float aspect) {
	glm::mat4 proj(1.0f);
	if (c.type == EditorCam::Type::Orthographic) {
		const float h = std::max(c.orthoHalfH, 0.01f);
		// Swapped near/far args = reversed-Z (near -> 1, far -> 0) for the
		// GreaterEqual depth pass, matching the perspective path.
		proj = glm::orthoRH_ZO(-h * aspect, h * aspect, -h, h,
			c.farClip, c.nearClip);
	}
	else {
		proj = MakeReversedZProjRH(glm::radians(c.fovYDeg), aspect,
			c.nearClip, c.farClip);
	}
	proj[1][1] *= -1.0f; // Vulkan Y-flip
	return proj;
}

void EditorApp::SetupDefaultCameras() {
	if (!m_editorCams.empty())
		return;
	m_editorCams.resize(3);
	m_editorCams[0].name = "Perspective";
	m_editorCams[0].type = EditorCam::Type::Perspective;
	m_editorCams[0].target = glm::vec3(0.0f, 1.0f, 0.0f);
	m_editorCams[0].dist = 6.0f;
	m_editorCams[1].name = "Top";
	m_editorCams[1].type = EditorCam::Type::Orthographic;
	m_editorCams[1].target = glm::vec3(0.0f);
	m_editorCams[1].yaw = 0.0f;
	m_editorCams[1].pitch = -1.5707963f; // straight down
	m_editorCams[1].dist = 25.0f;
	m_editorCams[1].orthoHalfH = 10.0f;
	m_editorCams[2].name = "Side";
	m_editorCams[2].type = EditorCam::Type::Orthographic;
	m_editorCams[2].target = glm::vec3(0.0f);
	m_editorCams[2].yaw = 1.5707963f; // looking along -X
	m_editorCams[2].pitch = 0.0f;
	m_editorCams[2].dist = 25.0f;
	m_editorCams[2].orthoHalfH = 10.0f;
	// Viewport 0 = perspective; quad adds Top (1) + Side (2) + Persp (0).
	for (int i = 0; i < kMaxViewports; ++i)
		m_viewports[i].camIdx = (i == 1) ? 1 : (i == 2) ? 2 : 0;
}

void EditorApp::ComputeViewportRects(smvk::Extent2D extent) {
	// Central area between the panels (m_vp in single layout = vp[0]).
	ViewportRect area;
	area.y = m_menuH;
	area.h = float(extent.height) - area.y;
	area.x = 0.0f;
	area.w = float(extent.width);
	if (m_uiMode) {
		area.x = m_outlinerW;
		area.w = float(extent.width) - m_outlinerW - m_inspectorW;
		area.h -= m_bottomDockH;
	}
	area.w = std::max(area.w, 1.0f);
	area.h = std::max(area.h, 1.0f);

	const float mx = float(m_input.getMouseX());
	const float my = float(m_input.getMouseY());
	const int count = ViewportCount();
	for (int i = 0; i < kMaxViewports; ++i)
		m_viewports[i].hovered = false;
	for (int i = 0; i < count; ++i) {
		ViewportRect& r = m_viewports[i].rect;
		if (m_vpLayout == 1) {
			// Quad: 2x2 grid.
			const float halfW = area.w * 0.5f;
			const float halfH = area.h * 0.5f;
			r.x = (i & 1) ? area.x + halfW : area.x;
			r.w = halfW;
			r.y = (i >= 2) ? area.y + halfH : area.y;
			r.h = halfH;
		}
		else {
			r = area;
		}
		m_viewports[i].hovered =
			mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
		if (m_viewports[i].hovered)
			m_activeVp = i;
	}
	// Central-rect member (legacy consumers: inset strip, overlay, chrome).
	m_vp = m_viewports[m_activeVp].rect;
}

bool EditorApp::SecondViewActive() const {
	if (m_secondView == SecondView::Off)
		return false;
	// Fullscreen already shows UVs: no inset needed.
	if (m_viewports[0].viewMode == ViewMode::Uv0 || m_viewports[0].viewMode == ViewMode::Uv1)
		return false;
	return true;
}

smvk::Extent2D EditorApp::InsetStrip(const ViewportRect& vp, uint32_t& outX) {
	uint32_t w = std::min<uint32_t>(400u, uint32_t(vp.w) / 3u);
	w = std::max(w, 160u);
	if (w >= uint32_t(vp.w) || vp.h <= 0.0f) {
		outX = uint32_t(vp.x) + uint32_t(vp.w);
		return smvk::Extent2D{ 0, 0 };
	}
	outX = uint32_t(vp.x + vp.w) - w; // window-absolute strip left edge
	return smvk::Extent2D{ w, uint32_t(vp.h) };
}

void EditorApp::UpdateInsetFrameData(uint32_t flight, smvk::Extent2D stripExtent) {
	// Same camera as viewport 0, strip aspect: re-project m_lastFrame
	// (viewport 0's frame) so the inset is undistorted.
	SetupDefaultCameras();
	const EditorCam& cam = m_editorCams[m_viewports[0].camIdx];
	app::FrameUniforms frame = m_viewports[0].frame;
	const glm::mat4 view = EditorCamView(cam);
	glm::mat4 proj = EditorCamProj(cam,
		float(stripExtent.width) / float(stripExtent.height));
	const glm::mat4 viewProj = proj * view;
	frame.view = view;
	frame.proj = proj;
	frame.viewProj = viewProj;
	frame.invView = glm::inverse(view);
	frame.invProj = glm::inverse(proj);
	frame.invViewProj = glm::inverse(viewProj);
	frame.prevViewProj = viewProj;
	frame.viewportSize = glm::vec2(float(stripExtent.width),
		float(stripExtent.height));
	frame.invViewportSize = glm::vec2(
		stripExtent.width ? 1.0f / float(stripExtent.width) : 0.0f,
		stripExtent.height ? 1.0f / float(stripExtent.height) : 0.0f);
	m_context->UploadDataToBuffer(m_insetFrameUniforms, &frame, sizeof(frame),
		smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
		smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(frame));
	m_lastInsetFrameAddr =
		m_context->GetBufferDeviceAddress(m_insetFrameUniforms) +
		flight * sizeof(app::FrameUniforms);
}

void EditorApp::DrawWorldNodes(smvk::CommandBuffer& cmd,
	const app::PushConstants& basePc, bool maskedPhase, bool selectedOnly,
	bool shadowPass, bool bakedOnly, bool chartTail) {
	(void)basePc;
	if (!m_world)
		return;
	const smvk::IteratorFlags req =
		smvk::IteratorFlags(smvk::IteratorFlagBit_Renderable |
			smvk::IteratorFlagBit_Opaque | smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (item.material == smvk::World::kInvalidIndex)
			return;
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return; // live visibility overrides apply without a Resolve
		if (selectedOnly && !IsSelected(item.node))
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
		// Baked-irradiance view: only baked draws (the direct path has no
		// per-draw chunk id, so unbaked surfaces would sample a foreign
		// texel instead of reading black).
		if (bakedOnly && mesh->lmChunk == 0xFFFFFFFFu)
			return;
		const glm::mat4& model = m_world->scene.globalTransform[item.node];
		app::PushConstants objPc{};
		std::memcpy(objPc.model, glm::value_ptr(model), sizeof(objPc.model));
		objPc.materialId = item.material;
		cmd.PushConstants(offsetof(app::PushConstants, materialId),
			sizeof(objPc.materialId), &objPc.materialId, smvk::ShaderStage::All);
		cmd.PushConstants(offsetof(app::PushConstants, model),
			sizeof(objPc.model), objPc.model, smvk::ShaderStage::All);
		if (chartTail) {
			// Per-mesh chart tail: the direct VS unpacks chart uvs to
			// atlas space for views that sample lightmaps (F8/Uv1).
			smvk::gfx::UvFlatVsPushTail tail{};
			tail.hasXforms = 1;
			tail.chartOffset = m_world->ChartOffsetFor(item.mesh, 0);
			tail.lmXforms = EnsureChartXforms();
			cmd.PushConstants(smvk::gfx::kUvFlatVsPushOffset,
				sizeof(tail), &tail, smvk::ShaderStage::All);
		}
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

smvk::ShaderProgram& EditorApp::MainProgram(ViewMode m) {
	switch (m) {
	case ViewMode::Unlit:
		return m_unlitProgram;
	case ViewMode::Uv0:
		return m_uv0Program.shaderCount == 2 ? m_uv0Program : m_litProgram;
	case ViewMode::Uv1:
		return m_uv1Program.shaderCount == 2 ? m_uv1Program : m_litProgram;
	case ViewMode::VertexColors:
		return m_vcolorProgram.shaderCount == 2 ? m_vcolorProgram : m_litProgram;
	case ViewMode::Baked:
		return m_bakedProgram.shaderCount == 2 ? m_bakedProgram : m_litProgram;
	default:
		return m_litProgram;
	}
}

smvk::ShaderProgram& EditorApp::FlatUvProgram(ViewMode m) {
	return (m == ViewMode::FlatUv1) ? m_flatUv1Program : m_flatUv0Program;
}

smvk::ShaderProgram& EditorApp::FlatUvCoordProgram() {
	return m_flatUvCoordProgram;
}

uint64_t EditorApp::EnsureChartXforms() {
	if (!m_world || !m_context)
		return 0;
	if (m_world->lmChartVersion == m_lmXformsVersion && !m_lmXformsBuf.empty())
		return m_lmXformsAddr;
	m_lmXformsVersion = m_world->lmChartVersion;
	const size_t count = std::max<size_t>(m_world->lmChartTable.size(), 1);
	const size_t bytes = count * sizeof(glm::vec4);
	if (m_lmXformsBuf.empty() || m_lmXformsCap < bytes) {
		m_lmXformsBuf = m_context->CreateBuffer(bytes,
			smvk::BufferType_Storage, "lm chart xforms");
		m_lmXformsCap = bytes;
	}
	if (m_lmXformsBuf.empty()) {
		m_lmXformsAddr = 0;
		return 0;
	}
	if (!m_world->lmChartTable.empty()) {
		m_context->UploadDataToBuffer(m_lmXformsBuf,
			m_world->lmChartTable.data(), bytes,
			smvk::PipelineStage(smvk::PipelineStageBit_Vertex),
			smvk::AccessFlags::ShaderRead);
	}
	m_lmXformsAddr = m_context->GetBufferDeviceAddress(m_lmXformsBuf);
	return m_lmXformsAddr;
}

void EditorApp::DrawFlatUvNodes(smvk::CommandBuffer& cmd, ViewMode mode,
	const app::PushConstants& pc) {
	// Flat UV view: the PRIMARY SELECTION's subtree meshes, unwrapped by
	// the chosen UV channel with the texture content behind them. No
	// selection = empty panel (classic UV editor: one asset at a time).
	if (!m_world || m_selected == UINT32_MAX ||
		m_selected >= m_world->scene.hierarchy.size())
		return;
	smvk::ShaderProgram& prog = FlatUvProgram(mode);
	if (prog.shaderCount != 2)
		return;
	// Subtree node set of the primary selection.
	std::vector<uint32_t> stack{ m_selected };
	std::unordered_set<uint32_t> subtree;
	while (!stack.empty()) {
		const uint32_t n = stack.back();
		stack.pop_back();
		if (n >= m_world->scene.hierarchy.size() ||
			!subtree.insert(n).second)
			continue;
		for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
			c = m_world->scene.hierarchy[c].nextSibling)
			stack.push_back(uint32_t(c));
	}
	const smvk::VertexFormat fmt =
		HasDrawItems() ? smvk::VertexFormat::StaticVertexFormat
		: smvk::VertexFormat::Empty;
	if (fmt == smvk::VertexFormat::StaticVertexFormat)
		cmd.BindStaticVertexFmt();
	cmd.BindRenderPipeline(prog, fmt,
		smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = true, .depthTestEnable = true },
		smvk::StencilSettings{}, smvk::BlendSettings{},
		smvk::MSAASettings{},
		smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
	cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable |
		smvk::IteratorFlagBit_StaticVertexFmt);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (item.material == smvk::World::kInvalidIndex)
			return;
		if (!subtree.contains(item.node))
			return;
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return;
		const smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh)
			return;
		// Lightmap layout: baked meshes only (like the F8 view).
		if (mode == ViewMode::FlatUv1 && mesh->lmChunk == 0xFFFFFFFFu)
			return;
		uint32_t matId = item.material;
		cmd.PushConstants(offsetof(app::PushConstants, materialId),
			sizeof(matId), &matId, smvk::ShaderStage::All);
		if (mode == ViewMode::FlatUv1) {
			// Per-mesh chart tail: the flat VS unpacks chart uvs to
			// atlas space itself (no draw ring on the direct path).
			smvk::gfx::UvFlatVsPushTail tail{};
			tail.hasXforms = 1;
			tail.chartOffset = m_world->ChartOffsetFor(item.mesh, 0);
			tail.lmXforms = EnsureChartXforms();
			cmd.PushConstants(smvk::gfx::kUvFlatVsPushOffset,
				sizeof(tail), &tail, smvk::ShaderStage::All);
		}
		for (const auto& s : mesh->geometry.surfaces)
			cmd.DrawIndexed(s.indexCount, 1, s.firstIndex, s.baseVertex);
	});
}

void EditorApp::BuildFlatUvWires(
	std::vector<smvk::DebugLineVertex>& outLines, ViewMode mode,
	uint32_t subtreeRoot, int vpIdx) const {
	// UV wires from the kept glTF model (pak scenes have no CPU copies:
	// the flat panel still shows the texture fill, wires are skipped).
	outLines.clear();
	if (!m_hasGltfModel || !m_world)
		return;
	const bool wholeScene = (subtreeRoot == UINT32_MAX);
	if (!wholeScene && subtreeRoot >= m_world->scene.hierarchy.size())
		return;
	std::unordered_set<uint32_t> subtree;
	if (!wholeScene) {
		std::vector<uint32_t> stack{ subtreeRoot };
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			if (n >= m_world->scene.hierarchy.size() ||
				!subtree.insert(n).second)
				continue;
			for (int c = m_world->scene.hierarchy[n].firstChild; c != -1;
				c = m_world->scene.hierarchy[c].nextSibling)
				stack.push_back(uint32_t(c));
		}
	}
	const tinygltf::Model& model = m_gltfModel;
	// UV accessor read (float vec2 only; skip exotic encodings).
	auto readUv = [&](int accessorIdx, size_t i, glm::vec2& out) -> bool {
		if (accessorIdx < 0 || accessorIdx >= int(model.accessors.size()))
			return false;
		const tinygltf::Accessor& acc = model.accessors[size_t(accessorIdx)];
		if (i >= size_t(acc.count) || acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT)
			return false;
		const tinygltf::BufferView& bv = model.bufferViews[size_t(acc.bufferView)];
		const tinygltf::Buffer& buf = model.buffers[size_t(bv.buffer)];
		const size_t stride = acc.ByteStride(bv);
		const size_t off = bv.byteOffset + acc.byteOffset + i * stride;
		if (off + 8 > buf.data.size())
			return false;
		const float* f = reinterpret_cast<const float*>(buf.data.data() + off);
		out = glm::vec2(f[0], f[1]);
		return true;
	};
	const bool lightmap = (mode == ViewMode::FlatUv1);
	// Interactive panels keep wires readable; exports wire everything.
	const size_t wireTriCap = wholeScene ? 400000 : 60000;
	size_t tris = 0;
	for (const auto& [node, ref] : m_primRef) {
		if (!wholeScene && !subtree.contains(node))
			continue;
		if (ref.mesh < 0 || ref.prim < 0)
			continue;
		const tinygltf::Mesh& gm = model.meshes[size_t(ref.mesh)];
		if (ref.prim >= int(gm.primitives.size()))
			continue;
		const tinygltf::Primitive& prim =
			gm.primitives[size_t(ref.prim)];
		const std::string attr = lightmap ? "TEXCOORD_1" : "TEXCOORD_0";
		auto pa = prim.attributes.find(attr);
		if (pa == prim.attributes.end())
			continue; // prim without this UV channel: no wires
		const int uvAcc = pa->second;
		const size_t vcount = size_t(model.accessors[size_t(uvAcc)].count);
		glm::vec2 uv[3];
		// Whole-scene export uses identity (pane nav never affects PNGs).
		const bool applyNav = !wholeScene;
		auto emit = [&](size_t a, size_t b, size_t c) {
			if (!readUv(uvAcc, a, uv[0]) || !readUv(uvAcc, b, uv[1])
				|| !readUv(uvAcc, c, uv[2]))
				return;
			for (int k = 0; k < 3; ++k) {
				glm::vec2 na = applyNav ? UvToFlatNdc(uv[k], vpIdx)
					: (uv[k] * 2.0f - glm::vec2(1.0f));
				glm::vec2 nb = applyNav ? UvToFlatNdc(uv[(k + 1) % 3], vpIdx)
					: (uv[(k + 1) % 3] * 2.0f - glm::vec2(1.0f));
				outLines.push_back({ glm::vec3(na.x, na.y, 1.0f) });
				outLines.push_back({ glm::vec3(nb.x, nb.y, 1.0f) });
			}
		};
		if (prim.indices >= 0
			&& prim.indices < int(model.accessors.size())) {
			// Indexed: faces address vertices through the index buffer
			// (u8/u16/u32); reading t*3+k raw names unrelated vertices.
			const tinygltf::Accessor& iacc =
				model.accessors[size_t(prim.indices)];
			if (iacc.bufferView < 0
				|| iacc.bufferView >= int(model.bufferViews.size()))
				continue;
			const tinygltf::BufferView& ibv =
				model.bufferViews[size_t(iacc.bufferView)];
			if (ibv.buffer < 0 || ibv.buffer >= int(model.buffers.size()))
				continue;
			const tinygltf::Buffer& ibuf = model.buffers[size_t(ibv.buffer)];
			const size_t istride = iacc.ByteStride(ibv);
			auto readIdx = [&](size_t i, uint32_t& out) -> bool {
				const size_t off = ibv.byteOffset + iacc.byteOffset
					+ i * istride;
				switch (iacc.componentType) {
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
					if (off + 1 > ibuf.data.size())
						return false;
					out = ibuf.data[off];
					return true;
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
					if (off + 2 > ibuf.data.size())
						return false;
					uint16_t v;
					std::memcpy(&v, ibuf.data.data() + off, 2);
					out = v;
					return true;
				}
				case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
					if (off + 4 > ibuf.data.size())
						return false;
					uint32_t v;
					std::memcpy(&v, ibuf.data.data() + off, 4);
					out = v;
					return true;
				}
				default:
					return false; // exotic index encoding: no wires
				}
			};
			const size_t tcount = size_t(iacc.count) / 3;
			for (size_t t = 0; t < tcount; ++t) {
				uint32_t i0, i1, i2;
				if (!readIdx(t * 3, i0) || !readIdx(t * 3 + 1, i1)
					|| !readIdx(t * 3 + 2, i2))
					break; // truncated index stream: stop this prim
				if (i0 >= vcount || i1 >= vcount || i2 >= vcount)
					continue; // corrupt face: skip, keep the rest
				emit(i0, i1, i2);
				if (++tris > wireTriCap)
					return;
			}
		}
		else {
			for (size_t v = 0; v + 2 < vcount; v += 3) {
				emit(v, v + 1, v + 2);
				if (++tris > wireTriCap)
					return;
			}
		}
	}
}

smvk::ShaderProgram& EditorApp::InsetProgram() {
	switch (m_secondView) {
	case SecondView::Uv0:
		return m_uv0Program.shaderCount == 2 ? m_uv0Program : m_litProgram;
	case SecondView::Uv1:
		return m_uv1Program.shaderCount == 2 ? m_uv1Program : m_litProgram;
	default:
		return m_litProgram;
	}
}

void EditorApp::RecordMainPass(smvk::LogicalImageViewHandle swapView, int vpIdx,
	uint32_t flight, bool selectedWireOverlay) {
	const ViewportRect& vp = m_viewports[vpIdx].rect;
	// View mode is PER VIEWPORT: UV views are panel types too. The
	// single shadow cascade is shared: only the ACTIVE viewport's
	// LitShadows mode drives it.
	const ViewMode mode = m_viewports[vpIdx].viewMode;
	const bool shadows =
		(m_viewports[m_activeVp].viewMode == ViewMode::LitShadows) &&
		m_shadowAtlas.Valid();
	smvk::ShaderProgram& prog = MainProgram(mode);
	// Meshlet view has its own program + draw path (no material legs).
	const bool meshletDbg =
		(mode == ViewMode::Meshlets) && m_meshDebugProgram.shaderCount == 2;
	// Flat UV panels: unwrapped shell + texture content, own draw path.
	const bool flatUv =
		(mode == ViewMode::FlatUv0 || mode == ViewMode::FlatUv1) &&
		FlatUvProgram(mode).shaderCount == 2;
	if (!meshletDbg && !flatUv && prog.shaderCount != 2)
		return;
	// Baked view draws baked meshes only (see DrawWorldNodes).
	const bool bakedOnly = (mode == ViewMode::Baked);
	// UV debug views show UNWRAPPED texture space: faces must never be
	// backface-culled (the UV shell flips winding arbitrarily).
	const bool uvDbg = (mode == ViewMode::Uv0 || mode == ViewMode::Uv1);

	const uint64_t frameAddr = m_viewports[vpIdx].frameAddr;
	const uint64_t lightsAddr =
		m_context->GetBufferDeviceAddress(m_lightsBuf) + flight * sizeof(smvk::Light) * kMaxLights;
	const uint64_t matsAddr =
		m_context->GetBufferDeviceAddress(m_materialsBuf) +
		flight * sizeof(smvk::MaterialValues) * kMaxMaterials;
	const uint64_t cascadeAddr =
		m_context->GetBufferDeviceAddress(m_cascadeBuf) +
		flight * sizeof(smvk::GpuDirectionalLightDescription);

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
	if (flatUv) {
		// Flat-UV pane transform (must match uvflat.vert.hlsli +
		// UvToFlatNdc): model[0][0] = zoom, model[3].xy = pan.
		const int vi = std::clamp(vpIdx, 0, kMaxViewports - 1);
		const float z = (m_uvZoom[vi] > 1e-6f) ? m_uvZoom[vi] : 1.0f;
		std::memset(pc.model, 0, sizeof(pc.model));
		pc.model[0] = z;
		pc.model[5] = z;
		pc.model[10] = 1.0f;
		pc.model[15] = 1.0f;
		pc.model[12] = m_uvPan[vi].x;
		pc.model[13] = m_uvPan[vi].y;
	}

	// Viewport rect only (the EditorBg pass clears the rest of the
	// window behind the panels). Color loads the bg clear; depth
	// clears in the rect (reversed-Z).
	auto& pass = m_rendergraph->AddPass("EditorMain")
		.SetOffset(uint32_t(vp.x), uint32_t(vp.y))
		.SetExtent(smvk::Extent2D{ uint32_t(vp.w), uint32_t(vp.h) })
		.SetColor({ swapView },
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0.07f, 0.07f, 0.09f, 1.0f))
		.SetDepth(m_depth.logicalView,
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
	pass.SetRecordingFunc([this, pc, selectedWireOverlay, bakedOnly, meshletDbg, flatUv, uvDbg, mode](smvk::CommandBuffer& cmd) {
		// Flat UV panel: the selection's unwrapped shell over its texture
		// content; the UV wires come from the line pass. Nothing else
		// draws in this panel.
		if (flatUv) {
			DrawFlatUvNodes(cmd, mode, pc);
			return;
		}
		// Meshlet debug leg (F4): flat per-meshlet colors, own program +
		// expanded index buffer; skips the shaded legs entirely.
		if (meshletDbg) {
			DrawMeshletNodes(cmd);
			// Still show the selection highlight on top.
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
			return;
		}
		smvk::ShaderProgram& prog = MainProgram(mode);
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
		// Wireframe toggle (Z): the whole scene draws with the flat wire
		// program in line-polygon mode (falls back to shaded when the
		// module is missing — a bad cook never blanks the frame).
		const bool wireAll = m_wireframeAll && m_wireProgram.shaderCount == 2;
		smvk::ShaderProgram& scene = wireAll ? m_wireProgram : prog;
		// Opaque leg (opaque + masked; back-cull unless UV/wire view:
		// the unwrapped UV shells flip winding arbitrarily — culled UVs
		// look like missing lightmap data).
		for (int masked = 0; masked <= 1; ++masked) {
			const bool maskedPhase = (masked == 1);
			cmd.BindRenderPipeline(scene, fmt,
				depth, smvk::StencilSettings{}, smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{ .polygonMode = wireAll
					? smvk::PolygonMode::Line : smvk::PolygonMode::Fill,
					.cullMode = (maskedPhase || uvDbg || wireAll)
					? smvk::CullMode::None : smvk::CullMode::Back });
		cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
		DrawWorldNodes(cmd, pc, maskedPhase, false, false, bakedOnly,
			bakedOnly || mode == ViewMode::Uv1);
	}
		// Transparent leg (alpha blend, no depth write).
		{
			cmd.BindRenderPipeline(scene, fmt,
				smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false, .depthTestEnable = true },
				smvk::StencilSettings{},
				smvk::BlendSettings::AlphaBlend(),
				smvk::MSAASettings{},
				smvk::RasterSettings{ .polygonMode = wireAll
					? smvk::PolygonMode::Line : smvk::PolygonMode::Fill,
					.cullMode = smvk::CullMode::None });
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
					if (!IsVisibleNode(m_world->scene, int(item.node)))
						return;
					smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
					if (!mesh)
						return;
					if (bakedOnly && mesh->lmChunk == 0xFFFFFFFFu)
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

void EditorApp::RecordInsetPass(smvk::LogicalImageViewHandle swapView,
	uint32_t flight) {
	// Second viewport: UV debug side-strip INSIDE viewport 0 (single
	// layout only; same camera, strip aspect via the inset frame slice).
	// Color loads (empty areas keep the main view = PiP); depth clears
	// in the strip because the aspect differs from the main pass.
	if (!SecondViewActive() || m_vpLayout == 1)
		return;
	smvk::ShaderProgram& prog = InsetProgram();
	if (prog.shaderCount != 2)
		return;
	const ViewportRect vp = m_viewports[0].rect;
	uint32_t stripX = 0;
	const smvk::Extent2D strip = InsetStrip(vp, stripX);
	if (strip.width == 0 || strip.height == 0)
		return;

	const uint64_t lightsAddr =
		m_context->GetBufferDeviceAddress(m_lightsBuf) + flight * sizeof(smvk::Light) * kMaxLights;
	const uint64_t matsAddr =
		m_context->GetBufferDeviceAddress(m_materialsBuf) +
		flight * sizeof(smvk::MaterialValues) * kMaxMaterials;
	const uint64_t cascadeAddr =
		m_context->GetBufferDeviceAddress(m_cascadeBuf) +
		flight * sizeof(smvk::GpuDirectionalLightDescription);

	app::PushConstants pc{};
	pc.frameUniforms = m_lastInsetFrameAddr;
	pc.lightsBuffer = lightsAddr;
	pc.materialsBuffer = matsAddr;
	pc.cascadeBuffer = cascadeAddr;
	pc.materialId = 0;
	pc.comparisonSamplerId = 1;
	pc.numCascades = 0;
	pc.cascadeIndex = 0;
	pc.fpLightCount = 0;
	pc.spotShadowMapIndex = UINT32_MAX;
	pc.spotShadowCount = 0;
	pc.spotIndex = UINT32_MAX;
	pc.pad_pc = 0;
	pc.renderViews = 0;
	pc.mainRenderView = UINT32_MAX;
	pc.renderViewIndex = UINT32_MAX;

	auto& pass = m_rendergraph->AddPass("EditorInset")
		.SetOffset(stripX, uint32_t(vp.y))
		.SetExtent(strip)
		.SetColor({ swapView },
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0, 0, 0, 1))
		.SetDepth(m_depth.logicalView,
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
	pass.SetRecordingFunc([this, pc](smvk::CommandBuffer& cmd) {
		smvk::ShaderProgram& prog = InsetProgram();
		const smvk::VertexFormat fmt =
			HasDrawItems() ? smvk::VertexFormat::StaticVertexFormat
			: smvk::VertexFormat::Empty;
		if (fmt == smvk::VertexFormat::StaticVertexFormat)
			cmd.BindStaticVertexFmt();
		const smvk::DepthSettings depth{
			.depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = true, .depthTestEnable = true };
		for (int masked = 0; masked <= 1; ++masked) {
			const bool maskedPhase = (masked == 1);
			// UV inset: NO backface cull (the second viewport shows
			// unwrapped UV shells whose winding flips arbitrarily).
			cmd.BindRenderPipeline(prog, fmt,
				depth, smvk::StencilSettings{}, smvk::BlendSettings{},
				smvk::MSAASettings{},
				smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
			cmd.PushConstants(0, sizeof(pc), &pc, smvk::ShaderStage::All);
			DrawWorldNodes(cmd, pc, maskedPhase, false, false);
		}
		{
			cmd.BindRenderPipeline(prog, fmt,
				smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
					.writeEnable = false, .depthTestEnable = true },
				smvk::StencilSettings{},
				smvk::BlendSettings::AlphaBlend(),
				smvk::MSAASettings{}, smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
			app::PushConstants tpc = pc;
			tpc.pad_pc = 1;
			cmd.PushConstants(0, sizeof(tpc), &tpc, smvk::ShaderStage::All);
			if (m_world) {
				const smvk::IteratorFlags req = smvk::IteratorFlags(
					smvk::IteratorFlagBit_Renderable |
					smvk::IteratorFlagBit_Transparent |
					smvk::IteratorFlagBit_StaticVertexFmt);
				m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
					if (item.material == smvk::World::kInvalidIndex)
						return;
					if (!IsVisibleNode(m_world->scene, int(item.node)))
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
		});
}

// ---- meshlet debug view (F4) -------------------------------------------
// Expansion of every cooked meshlet into one shared UINT32 index buffer
// of monoblock-absolute vertex ids (mesh-local ids + the mesh's
// baseVertex, same rebase gfx::RegisterMeshFromCooked performs), with a
// per-meshlet draw range so each meshlet gets a flat stable color from
// the meshdebug program (App's F3 view, editor port without the GPU
// filter). Meshes without cooked meshlets draw their normal surfaces in
// a dim fallback color so nothing vanishes.

namespace {
// Editor meshlet palette: 0 = random-but-stable hue per (mesh, meshlet),
// 1 = cone-cutoff heat (-1 flat/backfacing-safe -> blue, 1 needle -> red).
void MeshletDebugColor(uint32_t mesh, uint32_t meshlet, float coneCutoff,
	int mode, float out[4]) {
	if (mode == 1) {
		const float t = std::clamp((coneCutoff + 1.0f) * 0.5f, 0.0f, 1.0f);
		// Blue -> cyan -> green -> yellow -> red piecewise ramp.
		if (t < 0.25f) {
			const float k = t / 0.25f;
			out[0] = 0.15f;
			out[1] = 0.35f * k;
			out[2] = 0.95f;
		}
		else if (t < 0.5f) {
			const float k = (t - 0.25f) / 0.25f;
			out[0] = 0.15f;
			out[1] = 0.35f + 0.6f * k;
			out[2] = 0.95f - 0.8f * k;
		}
		else if (t < 0.75f) {
			const float k = (t - 0.5f) / 0.25f;
			out[0] = 0.15f + 0.8f * k;
			out[1] = 0.95f;
			out[2] = 0.15f + 0.4f * (1.0f - k) * 0.0f;
		}
		else {
			const float k = (t - 0.75f) / 0.25f;
			out[0] = 0.95f;
			out[1] = 0.95f - 0.75f * k;
			out[2] = 0.15f;
		}
	}
	else {
		uint32_t h = (mesh * 0x9E3779B1u) ^ (meshlet * 0x85EBCA6Bu);
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 11;
		const float hue = float(h % 1000u) / 1000.0f;
		// HSV -> RGB (S 0.62, V 1.0): compact sector interpolation.
		const float s = 0.62f, v = 1.0f;
		const int sector = int(hue * 6.0f);
		const float f = hue * 6.0f - float(sector);
		const float p = v * (1.0f - s);
		const float q = v * (1.0f - s * f);
		const float t = v * (1.0f - s * (1.0f - f));
		switch (sector % 6) {
		case 0: out[0] = v; out[1] = t; out[2] = p; break;
		case 1: out[0] = q; out[1] = v; out[2] = p; break;
		case 2: out[0] = p; out[1] = v; out[2] = t; break;
		case 3: out[0] = p; out[1] = q; out[2] = v; break;
		case 4: out[0] = t; out[1] = p; out[2] = v; break;
		default: out[0] = v; out[1] = p; out[2] = q; break;
		}
	}
	out[3] = 1.0f;
}
} // namespace

void EditorApp::ResetMeshletDebug() {
	m_meshletRanges.clear();
	m_meshletRangesByMesh.clear();
	m_meshletIndices = {};
	m_meshletIndexCap = 0;
	m_meshletDebugBuilt = false;
	m_meshletDebugHasData = false;
}

bool EditorApp::EnsureMeshletDebug() {
	if (!m_world || !m_context)
		return false;
	if (m_meshletDebugBuilt)
		return true;
	m_meshletDebugBuilt = true;
	std::vector<uint32_t> indices;
	uint32_t meshletCount = 0;
	for (uint32_t mi = 0; mi < uint32_t(m_world->meshLocations.size()); ++mi) {
		const smvk::StaticMesh* mesh = m_world->GetMesh(mi);
		if (!mesh || !mesh->meshlets || mesh->meshlets->empty())
			continue;
		const smvk::CookedMeshlets& cooked = *mesh->meshlets;
		const uint32_t baseVertex = mesh->geometry.surfaces.empty()
			? 0u
			: uint32_t(std::max<int32_t>(
				mesh->geometry.surfaces[0].baseVertex, 0));
		std::vector<uint32_t> meshIndices;
		std::vector<smvk::MeshletDrawCommand> cmds;
		smvk::BuildMeshletIndirectCommands(cooked, meshIndices, cmds);
		for (const auto& c : cmds) {
			MeshletRange r{};
			r.firstIndex = uint32_t(indices.size()) + c.firstIndex;
			r.indexCount = c.indexCount;
			r.meshlet = c.meshletIndex;
			r.coneCutoff = c.meshletIndex < cooked.meshlets.size()
				? cooked.meshlets[c.meshletIndex].coneCutoff : 1.0f;
			m_meshletRangesByMesh[mi].push_back(
				uint32_t(m_meshletRanges.size()));
			m_meshletRanges.push_back(r);
		}
		for (uint32_t v : meshIndices)
			indices.push_back(v + baseVertex);
		meshletCount += uint32_t(cooked.meshlets.size());
	}
	m_meshletDebugHasData = !m_meshletRanges.empty();
	if (indices.empty())
		return true; // fallback-only view (no cooked meshlets)
	const size_t bytes = indices.size() * sizeof(uint32_t);
	if (m_meshletIndices.empty() || m_meshletIndexCap < uint32_t(indices.size())) {
		m_meshletIndices = m_context->CreateBuffer(bytes,
			smvk::BufferType_Index, "editor meshlet dbg indices");
		m_meshletIndexCap = uint32_t(indices.size());
	}
	if (m_meshletIndices.empty()) {
		m_meshletDebugBuilt = false;
		return false;
	}
	m_context->UploadDataToBuffer(m_meshletIndices, indices.data(), bytes,
		smvk::PipelineStageBit_Vertex, smvk::AccessFlags::IndexRead);
	Log("Meshlet debug: " + std::to_string(meshletCount) +
		" meshlets across " + std::to_string(m_meshletRangesByMesh.size()) +
		" meshes (" + std::to_string(indices.size() / 3) + " triangles).");
	return true;
}

void EditorApp::DrawMeshletNodes(smvk::CommandBuffer& cmd) {
	if (!m_world || !EnsureMeshletDebug())
		return;
	const smvk::VertexFormat fmt =
		HasDrawItems() ? smvk::VertexFormat::StaticVertexFormat
		: smvk::VertexFormat::Empty;
	if (fmt == smvk::VertexFormat::StaticVertexFormat)
		cmd.BindStaticVertexFmt();
	cmd.BindRenderPipeline(m_meshDebugProgram, fmt,
		smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
			.writeEnable = true, .depthTestEnable = true },
		smvk::StencilSettings{}, smvk::BlendSettings{},
		smvk::MSAASettings{},
		smvk::RasterSettings{ .polygonMode = m_wireframeAll
			? smvk::PolygonMode::Line : smvk::PolygonMode::Fill,
			.cullMode = smvk::CullMode::None });
	app::MeshDebugPush dp{};
	std::memcpy(dp.viewProj, glm::value_ptr(m_lastViewProj),
		sizeof(dp.viewProj));
	const smvk::IteratorFlags req = smvk::IteratorFlags(
		smvk::IteratorFlagBit_Renderable |
		smvk::IteratorFlagBit_Opaque |
		smvk::IteratorFlagBit_StaticVertexFmt);
	// Fallback leg (static monoblock indices): meshes without cooked
	// meshlets draw dim gray — nothing vanishes in the debug view.
	dp.color[0] = 0.45f;
	dp.color[1] = 0.45f;
	dp.color[2] = 0.5f;
	dp.color[3] = 1.0f;
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (item.material == smvk::World::kInvalidIndex)
			return;
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return;
		const smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
		if (!mesh || (mesh->meshlets && !mesh->meshlets->empty()))
			return; // meshlet leg below
		std::memcpy(dp.model,
			glm::value_ptr(m_world->scene.globalTransform[item.node]),
			sizeof(dp.model));
		cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
		for (const auto& s : mesh->geometry.surfaces)
			cmd.DrawIndexed(s.indexCount, 1, s.firstIndex, s.baseVertex);
	});
	if (m_meshletRanges.empty())
		return;
	// Meshlet leg: switch the index binding to the expanded buffer
	// (monoblock-absolute ids, vertexOffset 0) and flat-color each range.
	cmd.BindIndexBuffer(m_meshletIndices);
	m_world->forEachRenderItem(req, [&](const smvk::World::RenderItem& item) {
		if (item.material == smvk::World::kInvalidIndex)
			return;
		if (!IsVisibleNode(m_world->scene, int(item.node)))
			return;
		auto bit = m_meshletRangesByMesh.find(item.mesh);
		if (bit == m_meshletRangesByMesh.end())
			return;
		std::memcpy(dp.model,
			glm::value_ptr(m_world->scene.globalTransform[item.node]),
			sizeof(dp.model));
		for (uint32_t ri : bit->second) {
			const MeshletRange& r = m_meshletRanges[ri];
			MeshletDebugColor(item.mesh, r.meshlet, r.coneCutoff,
				m_debugMeshletMode, dp.color);
			cmd.PushConstants(0, sizeof(dp), &dp, smvk::ShaderStage::All);
			cmd.DrawIndexed(r.indexCount, 1, r.firstIndex, 0);
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

	// Viewport rects (single central or 2x2 quad) between the panels;
	// the hovered viewport is ACTIVE (input, gizmo, shadows, framing).
	ComputeViewportRects(extent);

	UpdateUvPaneInput(); // flat-UV panes: MMB pan + wheel zoom (own transform)
	UpdateCamera(delta_time_s);
	// vi-style modal keys (editor_modal.cpp): the ACTIVE mode's keymap
	// (LAYOUT/LIGHTING/TEST/PROFILE; 1-4 switch from any mode) interprets
	// keys, with a global fallback table (mode switches, shading F1-F3,
	// undo/redo/save, window toggles). Skipped while ImGui wants the
	// keyboard (typing) or the Q radial popup is open.
	UpdateModalInput();

	const uint32_t flight = uint32_t(fb.imageIndex) % m_flightCount;
	UpdateTextureStreaming(extent);
	UpdateFrameData(flight);
	// Second-viewport frame slice (strip aspect) for the inset pass.
	smvk::Extent2D insetStrip{ 0, 0 };
	if (SecondViewActive() && m_vpLayout == 0) {
		uint32_t insetX = 0;
		insetStrip = InsetStrip(m_viewports[0].rect, insetX);
		if (insetStrip.width != 0)
			UpdateInsetFrameData(flight, insetStrip);
	}

	// Active-viewport mouse (viewport-local). Alt+LMB is the Maya orbit
	// gesture; clicks inside the UV inset strip use a different projection.
	const ViewportRect& vp = m_viewports[m_activeVp].rect;
	const float lmx = float(m_input.getMouseX()) - vp.x;
	const float lmy = float(m_input.getMouseY()) - vp.y;
	const bool inVp = lmx >= 0.0f && lmy >= 0.0f && lmx < vp.w && lmy < vp.h;
	const bool altNav = m_input.isKeyHeld(GLFW_KEY_LEFT_ALT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_ALT);
	const bool uiEats = m_imgui && m_imgui->WantsCaptureMouse();
	const bool shiftHeld = m_input.isKeyHeld(GLFW_KEY_LEFT_SHIFT) ||
		m_input.isKeyHeld(GLFW_KEY_RIGHT_SHIFT);
	uint32_t insetXAbs = 0;
	const bool insetActive = SecondViewActive() && m_vpLayout == 0 &&
		InsetStrip(m_viewports[0].rect, insetXAbs).width != 0;
	const bool inInset = insetActive && m_activeVp == 0 &&
		lmx >= float(insetXAbs) - vp.x;

	// Flat-UV panes pick in UV space (click = face/vertex highlight);
	// 3D viewports keep the mesh/face/vertex click + rubber-band box.
	const bool activeFlat = IsFlatUvViewMode(
		m_viewports[m_activeVp].viewMode);
	if (activeFlat && m_world && inVp && !uiEats &&
		m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) && !altNav) {
		// MMB is pane pan (see UpdateUvPaneInput); LMB clicks pick.
		if (auto sub = PickFlatUv(m_activeVp, lmx, lmy)) {
			ApplySubSelection(*sub);
		}
		else if (!shiftHeld) {
			ClearSelection();
		}
	}
	// ---- terrain height painting --------------------------------------
	// Paint mode replaces the LMB press path entirely: no box select, no
	// gizmo grab, no picking. One stamp per click; drags stamp at
	// radius-spaced intervals along the surface.
	const bool painting = !activeFlat && m_world && m_paintTerrain
		&& !altNav && !inInset && inVp && !uiEats;
	if (painting) {
		// Fresh press always stamps (drag spacing applies within one
		// stroke only).
		if (m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT))
			m_paintLast = glm::vec2(-1e9f, -1e9f);
		if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT)) {
			const smvk::Extent2D vpExt{ uint32_t(vp.w), uint32_t(vp.h) };
			glm::vec3 rayOrigin;
			const glm::vec3 rayDir = MouseRay(lmx, lmy, vpExt,
				m_viewports[m_activeVp].viewProj, rayOrigin);
			const uint32_t group = TerrainGroupOf(m_selected);
			auto it = m_terrainGroups.find(group);
			float tHit = 0.0f;
			if (group != UINT32_MAX && it != m_terrainGroups.end()
				&& smvk::placement::TerrainRaycast(it->second.data.heights,
					rayOrigin, rayDir, 5000.0f, tHit)) {
				const glm::vec3 hit = rayOrigin + rayDir * tHit;
				const glm::vec2 hitXZ(hit.x, hit.z);
				const float spacing = std::max(m_paintRadius * 0.5f, 0.25f);
				if (m_paintLast.x < -1e8f
					|| glm::length(hitXZ - m_paintLast) >= spacing) {
					PaintTerrainAt(rayOrigin, rayDir);
					m_paintLast = hitXZ;
				}
			}
		}
		else {
			m_paintLast = glm::vec2(-1e9f, -1e9f);
		}
	}
	// Rubber-band multiselect: press on empty space (no gizmo grab),
	// drag past a few pixels -> box; release selects all nodes whose
	// projected AABB intersects. Shift adds to the current selection.
	// Box select stays Mesh-mode (face/vertex use click picking).
	else if (!activeFlat && m_world && !m_gizmoDragging && !altNav &&
		!inInset && inVp &&
		m_input.isMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) && !uiEats) {
		if (m_selMode == SelMode::Mesh) {
			if (!GizmoHandleInput()) {
				m_vpBoxDragging = true;
				m_vpBoxAdditive = shiftHeld;
				m_vpBoxVp = m_activeVp;
				m_vpBoxX0 = m_vpBoxX1 = lmx;
				m_vpBoxY0 = m_vpBoxY1 = lmy;
			}
		}
		else {
			// Face/Vertex click-pick (no box, no gizmo grab).
			const smvk::Extent2D vpExt{ uint32_t(vp.w), uint32_t(vp.h) };
			if (auto sub = PickSubObject(m_activeVp, lmx, lmy, vpExt)) {
				ApplySubSelection(*sub);
			}
			else if (!shiftHeld) {
				ClearSelection();
			}
			// Swallow the press so the box path below never starts.
			m_vpBoxDragging = false;
		}
	}
	if (m_vpBoxDragging && !activeFlat) {
		if (m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT)) {
			m_vpBoxX1 = std::clamp(float(m_input.getMouseX()) - vp.x,
				0.0f, vp.w);
			m_vpBoxY1 = std::clamp(float(m_input.getMouseY()) - vp.y,
				0.0f, vp.h);
			// Moved past the dead-zone: it is a box, not a click.
			if (std::fabs(m_vpBoxX1 - m_vpBoxX0) > 5.0f ||
				std::fabs(m_vpBoxY1 - m_vpBoxY0) > 5.0f) {
				// Keep the drag alive; the click-pick path is skipped.
			}
		}
		else {
			// Release: box (if dragged) or plain click-pick (below).
			const bool isBox = std::fabs(m_vpBoxX1 - m_vpBoxX0) > 5.0f ||
				std::fabs(m_vpBoxY1 - m_vpBoxY0) > 5.0f;
			if (isBox) {
				FinishBoxSelect();
			}
			else if (m_world && m_vpBoxVp == m_activeVp) {
				const smvk::Extent2D vpExt{ uint32_t(vp.w), uint32_t(vp.h) };
				if (m_selMode == SelMode::Mesh) {
					if (auto hit = PickNode(m_activeVp, lmx, lmy, vpExt)) {
						if (shiftHeld)
							AddToSelection(hit->node);
						else
							SetSelection(hit->node);
						m_revealNode = hit->node;
					}
					else if (!shiftHeld)
						ClearSelection();
				}
				else {
					if (auto sub = PickSubObject(m_activeVp, lmx, lmy, vpExt)) {
						ApplySubSelection(*sub);
					}
					else if (!shiftHeld)
						ClearSelection();
				}
			}
			m_vpBoxDragging = false;
		}
	}
	else if (!activeFlat && (m_gizmoDragging || (inVp && !altNav &&
		m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) && !uiEats))) {
		GizmoHandleInput();
	}

	m_rendergraph->NewFrame();
	if (ActiveViewMode() == ViewMode::LitShadows)
		RecordShadowPass(0);
	// Window background: the viewport passes cover only their rects, so
	// clear the whole window first (the dark chrome color shows through
	// the gaps between the ImGui panels).
	{
		auto& bg = m_rendergraph->AddPass("EditorBg")
			.SetOffset(0, 0)
			.SetExtent(extent)
			.SetColor({ m_swapViews[fb.imageIndex] },
				smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0.07f, 0.07f, 0.09f, 1.0f))
			.SetDepth(m_depth.logicalView,
				smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
				smvk::ClearDepthStencil(0.0f, 0));
		bg.SetRecordingFunc([](smvk::CommandBuffer& cmd) { (void)cmd; });
	}
	for (int vpIdx = 0; vpIdx < ViewportCount(); ++vpIdx)
		RecordMainPass(m_swapViews[fb.imageIndex], vpIdx, flight, m_wireOverlay);
	RecordInsetPass(m_swapViews[fb.imageIndex], flight);
	RecordGizmoPass(m_swapViews[fb.imageIndex]);
	// One-shot lightmap PNG export: renders the offscreen target this
	// frame, then reads it back after the submit (button in Bake panel).
	const bool lmExportThisFrame = m_lmExportQueued && m_hasSglm &&
		m_lmGpu && m_sglm.width > 0 && m_sglm.height > 0;
	if (lmExportThisFrame) {
		EnsureLightmapExportTarget(
			smvk::Extent2D{ m_sglm.width, m_sglm.height });
		if (!m_lmExportLogical.empty())
			RecordLightmapExportPass(flight);
		else
			Log("Lightmap export: offscreen target creation failed.");
		m_lmExportQueued = false;
	}

	if (m_imgui && m_imgui->IsInitialized()) {
		m_imgui->NewFrame(extent);
		DrawUI(extent);
		m_imgui->AddPass(*m_rendergraph, m_swapViews[fb.imageIndex], extent,
			smvk::AttachmentLoadOp::Load, smvk::ClearColor(0, 0, 0, 1));
	}
	// Flush bindless descriptor writes (scene texture views, samplers)
	// BEFORE Execute records — App does the same (see SceneRenderer).
	// Without this the textures[] array stays uninitialized and the
	// viewport samples garbage (gray) no matter the residency or slots.
	m_context->UpdateDescriptors();
	m_rendergraph->Execute(fb);
	m_context->ProcessDeferredTasks();
	if (lmExportThisFrame) {
		// The export pass ran in this submit: read it back + write PNG.
		WaitForDeviceIdle();
		ExportLightmapPng();
	}
	++m_frameCounter;
}

} // namespace editor
