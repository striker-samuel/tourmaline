// EditorApp viewport renderer: Unlit / Lit forward passes.
//
// Reuses App's compiled shaders (default.vert + default_unlit/default_lit/
// default_wire + shadow.vert/masked) and App's ABI-checked FrameUniforms +
// PushConstants. Depth is reversed-Z (clear 0, GreaterEqual) like App.
//
// Display chain (the "washed out colors" fix): the tonemap-chain view
// modes (Lit + the 1/exposure-pre-scaled debug views: Unlit/Uv0/Uv1/
// VertexColors/Meshlets + the wire overlays - see their shader headers)
// render RAW-LUX HDR into the shared preview HDR (m_sceneHdr) and one
// engine-tonemapper layer per viewport rect resolves it to the LDR
// swapchain (authored operator + manual exposure - the App's chain).
// Baked + the UV editor window canvas keep their own Reinhard + gamma
// display mapping and write the swap directly. (The legacy Lit+Shadows
// editor mode + its single-cascade shadow atlas are gone — the editor
// passes render per-view-mode previews.)

#include "editor_app.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Engine/Camera/camera.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Utils/bvh.h>
#include <Engine/core.h>

#include <imgui.h>

namespace editor {

	namespace {
		constexpr uint32_t kMaxMaterials = 4096;
		constexpr uint32_t kMaxLights = 64;

		std::vector<char> LoadSpv(const char* path) {
			return readFile(path);
		}
	} // namespace

	bool EditorApp::EnsureShaders() {
		if (m_litProgram.shaderCount == 2 && m_unlitProgram.shaderCount == 2 &&
			m_wireProgram.shaderCount == 2 && m_lineProgram.shaderCount == 2)
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
		// Commented out by user.
		//// Flat UV views (per-panel type): mesh projected into UV space with
		//// its texture content. Failure is non-fatal: the panel falls back to
		//// the 3D UV debug view.
		//auto tryFlatUv = [&](const char* vsFile, const char* fsFile,
		//	const char* vsName, const char* fsName,
		//	smvk::Holder<smvk::ShaderModuleHandle>& vsMod,
		//	smvk::Holder<smvk::ShaderModuleHandle>& fsMod,
		//	smvk::ShaderProgram& prog) {
		//	if (prog.shaderCount == 2)
		//		return;
		//	try {
		//		if (vsMod.empty())
		//			vsMod = m_context->CreateShaderModule(LoadSpv(vsFile), vsName);
		//		if (fsMod.empty())
		//			fsMod = m_context->CreateShaderModule(LoadSpv(fsFile), fsName);
		//	}
		//	catch (const std::exception& e) {
		//		std::cerr << "[Editor] flat-uv shader unavailable (" << vsFile
		//			<< "): " << e.what() << "\n";
		//		return;
		//	}
		//	if (!vsMod.empty() && !fsMod.empty())
		//		prog = { .shaders = {
		//			{ .module = vsMod, .entryPoint = "mainVS",
		//				.stage = smvk::ShaderStage::Vertex },
		//			{ .module = fsMod, .entryPoint = "mainPS",
		//				.stage = smvk::ShaderStage::Fragment } },
		//			.shaderCount = 2 };
		//};
		//tryFlatUv("content/shaders/uvflat0.vert.hlsl.spv",
		//	"content/shaders/uvflat0.frag.hlsl.spv",
		//	"editorFlatUv0VS", "editorFlatUv0FS",
		//	m_flatUv0VsModule, m_flatUv0FsModule, m_flatUv0Program);
		//tryFlatUv("content/shaders/uvflat1.vert.hlsl.spv",
		//	"content/shaders/uvflat1.frag.hlsl.spv",
		//	"editorFlatUv1VS", "editorFlatUv1FS",
		//	m_flatUv1VsModule, m_flatUv1FsModule, m_flatUv1Program);
		//// UV-coordinate PNG export: same unwrapped VS, fragment outputs the
		//// atlas uv itself (uvflat1uv.frag) — no atlas content read.
		//tryFlatUv("content/shaders/uvflat1.vert.hlsl.spv",
		//	"content/shaders/uvflat1uv.frag.hlsl.spv",
		//	"editorFlatUv1VS", "editorFlatUvCoordFS",
		//	m_flatUv1VsModule, m_flatUvCoordFsModule, m_flatUvCoordProgram);
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
		// Preview tonemapper (the m_sceneHdr display chain, see the file
		// header): best-effort - a missing SPV never kills the editor, the
		// routing in RecordMainPass falls back to the direct LDR write.
		(void)m_previewTonemap.EnsureShaders(*m_context);
		return true;
	}

	bool EditorApp::EnsureBuffers(uint32_t flightCount) {
		if (m_flightCount == flightCount && !m_frameUniforms.empty())
			return true;
		m_flightCount = flightCount;
		m_frameUniforms = m_context->CreateBuffer(
			sizeof(engine::FrameUniforms) * flightCount * kMaxViewports,
			smvk::BufferType_Uniform, "editor frames");
		m_frameHotBuf = m_context->CreateBuffer(
			sizeof(engine::FramePSHot) * flightCount * kMaxViewports,
			smvk::BufferType_Uniform, "editor frame hot");
		m_insetFrameUniforms = m_context->CreateBuffer(
			sizeof(engine::FrameUniforms) * flightCount, smvk::BufferType_Uniform,
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
		return !m_frameUniforms.empty() && !m_frameHotBuf.empty() &&
			!m_insetFrameUniforms.empty() &&
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

	void EditorApp::UpdateFrameData(uint32_t flight) {
		SetupDefaultCameras();

		// One frame slice per (flight, viewport): each viewport renders
		// with ITS camera (aspect from its rect). The ACTIVE viewport also
		// drives the light buffer.
		const uint64_t baseAddr =
			m_context->GetBufferDeviceAddress(m_frameUniforms);
		const size_t sliceStride = sizeof(engine::FrameUniforms);
		for (int vp = 0; vp < ViewportCount(); ++vp) {
			const ViewportRect& rect = m_viewports[vp].rect;
			const float aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
			// Locked look-through wins over the orbit camera; unbound (or
			// lost-node) viewports keep the existing orbit behavior.
			glm::mat4 view, proj;
			glm::vec3 eye;
			float nearClip, farClip;
			if (!BoundCameraView(vp, view, proj, eye, nearClip, farClip)) {
				const EditorCam& cam = m_editorCams[m_viewports[vp].camIdx];
				view = EditorCamView(cam);
				proj = EditorCamProj(cam, aspect);
				eye = EditorCamEye(cam);
				nearClip = cam.nearClip;
				farClip = cam.farClip;
			}
			const glm::mat4 viewProj = proj * view;

			engine::FrameUniforms frame{};
			frame.view = view;
			frame.proj = proj;
			frame.viewProj = viewProj;
			frame.invView = glm::inverse(view);
			frame.invProj = glm::inverse(proj);
			frame.invViewProj = glm::inverse(viewProj);
			frame.prevViewProj = viewProj;
			frame.cameraPos = eye;
			frame.time = time_since_start_s;
			frame.viewportSize = glm::vec2(rect.w, rect.h);
			frame.invViewportSize = glm::vec2(
				rect.w > 0.0f ? 1.0f / rect.w : 0.0f,
				rect.h > 0.0f ? 1.0f / rect.h : 0.0f);
		frame.nearPlane = nearClip;
		frame.farPlane = farClip;
		// The App feeds the SAME number to its frame uniforms and the
		// tonemap push (see WorldRenderer: data.exposure = FrameExposure()
		// == tonemapDesc.exposure) - the preview mirrors that with its
		// standing exposure so the 1/exposure-pre-scaled debug views and
		// the tonemap layer agree exactly (see PreviewExposure).
		frame.exposure = PreviewExposure();
		frame.aoTextureIndex = UINT32_MAX;
			frame.aoIntensity = 0.0f;
			frame.evsmMode = 0;
			frame.decalCount = 0;
			// NOTE (lightmaps deprecated): FrameUniforms no longer carries the
			// baked-irradiance atlas slots (see the #if 0 block in
			// Engine/Renderer/WorldRenderer.h) — the editor's m_lmGpu upload
			// path is retained, but nothing consumes it per-frame anymore.
			frame.regEnabled = 0;
			frame.regCount = 0;
			frame.dsEnabled = 0;
			frame.giEnabled = 0;
			frame.giDebug = 0;

			smvk::EnvironmentSettings env{};
			ApplyEnvToFrame(frame, env);
			frame.env = env.ToFrameData();

			// FramePSHot hot slab (THE device-death fix): the lit FS reads
			// FrameUniforms::frameHot UN-GATED at the very top of mainPS
			// (default_lit_body.hlsli:354 "FramePSHot hot = frame.frameHot
			// .Get()"), so a zero-init frame made every world-drawing frame
			// dereference a null BDA — an MMU read at address 0x0 (Aftermath
			// confirmed) that took the whole device down (surfacing as the
			// UINT64_MAX timeline abort at the live-view launch). Mirror the
			// App renderer's UpdateFrameUniforms: upload this view's hot
			// struct + carry its slab address in the frame uniform BEFORE the
			// frame upload (the frame carries the BDA). The inset (viewport
			// strip) copies viewport 0's frame, so it inherits this slab.
			{
				engine::FramePSHot hot{};
				hot.cameraPos = frame.cameraPos;
				hot.time = frame.time;
				hot.viewportSize = frame.viewportSize;
				hot.invViewportSize = frame.invViewportSize;
				hot.nearPlane = frame.nearPlane;
				hot.farPlane = frame.farPlane;
				hot.exposure = frame.exposure;
				hot.aoTextureIndex = frame.aoTextureIndex;
				hot.aoIntensity = frame.aoIntensity;
				hot.evsmMode = frame.evsmMode;
				hot.dsEnabled = frame.dsEnabled;
				hot.dsTileCountX = frame.dsTileCountX;
				hot.fogEndAndFlags_w = frame.env.fogEndAndFlags.w;
				hot.giIntensity = frame.giIntensity;
				hot.giDebug = frame.giDebug;
				hot.cvolEnabled = frame.cvolEnabled;
				const size_t hotSlice = (size_t(flight) * kMaxViewports
					+ size_t(vp)) * sizeof(engine::FramePSHot);
				m_context->UploadDataToBuffer(m_frameHotBuf, &hot, sizeof(hot),
					smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
					smvk::AccessFlags::ShaderRead, 0, hotSlice);
				frame.frameHot =
					m_context->GetBufferDeviceAddress(m_frameHotBuf) + hotSlice;
			}

			m_viewports[vp].frame = frame;
			m_viewports[vp].viewProj = viewProj;
			// Basis trace (VULKANTOY_BASIS_TRACE=1): one line per viewport
			// every 60 frames — eye + the screen basis (view-matrix rows:
			// row 0 = right, row 1 = screen-up) + the rect. Compare against
			//\ wr\ twin\ gone;\ kept\ for\ the\ editor's\ own
			// basis disagreement (e.g. a 90-degree roll of boxes/clicks vs
			// pixels) shows up as different right/up rows for the same eye.
			if ((m_frameCounter % 60) == 0) {
				static const bool basisTrace =
					std::getenv("VULKANTOY_BASIS_TRACE") != nullptr;
				if (basisTrace) {
					const glm::vec3 bRight(view[0][0], view[1][0], view[2][0]);
					const glm::vec3 bUp(view[0][1], view[1][1], view[2][1]);
					std::fprintf(stderr,
						"[BASIS] ed vp=%d rect=%.0fx%.0f@%.0f,%.0f "
						"eye=(%.2f,%.2f,%.2f) "
						"right=(%.3f,%.3f,%.3f) up=(%.3f,%.3f,%.3f)\n",
						vp, rect.w, rect.h, rect.x, rect.y,
						eye.x, eye.y, eye.z,
						bRight.x, bRight.y, bRight.z,
						bUp.x, bUp.y, bUp.z);
				}
			}
			m_viewports[vp].frameAddr = baseAddr +
				(size_t(flight) * kMaxViewports + size_t(vp)) * sliceStride;
			m_context->UploadDataToBuffer(m_frameUniforms, &frame, sizeof(frame),
				smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
				smvk::AccessFlags::ShaderRead, 0,
				(size_t(flight) * kMaxViewports + size_t(vp)) * sliceStride);
		}
		// Legacy frame mirrors (gizmo/pick math): the ACTIVE vp.
		m_lastFrame = m_viewports[m_activeVp].frame;
		m_lastViewProj = m_viewports[m_activeVp].viewProj;

	// Sun light (lights[0], same convention as App) from the CURRENT env.
	// The env block is LUX-NATIVE (see SceneEnv) and the lit view now runs
	// the App's RAW-LUX HDR display chain (see the file header + m_sceneHdr)
	// - the lux go in unscaled and the per-viewport tonemap layer brings
	// them down with the authored operator + exposure, exactly like the App.
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
		l.intensity = (ActiveViewMode() == ViewMode::Unlit) ? 0.0f
			: envSet.sunIntensity;
		l.shadow_map_index = -1;
	}

		// Cascade descriptor: no cascades in the editor's own pass (the
		// editor passes render shadows per view mode).
		// Zeroed, uploaded anyway so the shaders never read a null BDA.
		smvk::GpuDirectionalLightDescription dir{};
		m_context->UploadDataToBuffer(m_lightsBuf, lights, sizeof(lights),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
			smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(lights));

		m_context->UploadDataToBuffer(m_cascadeBuf, &dir, sizeof(dir),
			smvk::PipelineStageBit_Vertex | smvk::PipelineStageBit_Fragment,
			smvk::AccessFlags::ShaderRead, 0, size_t(flight) * sizeof(dir));

		UploadMaterials(flight);
	}

	void EditorApp::ApplyEnvToFrame(engine::FrameUniforms& frame,
		smvk::EnvironmentSettings& env) const {
		const EditorEnv& e = CurrentEnv();
		// RAW lux in (see the file header + m_sceneHdr): the lit view runs
		// the App's HDR + tonemap display chain, so the env values are the
		// same numbers the App renders with (sky 600, sun 10000...) and the
		// per-viewport tonemap layer (authored operator + manual
		// exposure) brings them to LDR exactly like the App's Tonemap pass.
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
		env.sunIntensity = (ActiveViewMode() == ViewMode::Unlit) ? 0.0f
			: e.sunIntensity;
		env.fogMode = 0; // volumetrics authored, not yet rendered here
	}

	EditorApp::EditorEnv& EditorApp::CurrentEnv() {
		// Scene-backed (see SceneEnv): the resolved source that renders —
		// the scene default, or the selected node block. UI writes land
		// directly in the scene (no publish step; ResolveSceneEnv refreshes
		// the rendered copy afterwards).
		static SceneEnv fallback{};
		if (!m_world)
			return fallback;
		if (m_envCurrent != UINT32_MAX) {
			auto it = m_world->scene.envForNode.find(m_envCurrent);
			if (it != m_world->scene.envForNode.end())
				return it->second;
			m_envCurrent = UINT32_MAX;
		}
		return m_world->scene.defaultEnvironment;
	}

	const EditorApp::EditorEnv& EditorApp::CurrentEnv() const {
		static const SceneEnv fallback{};
		if (!m_world)
			return fallback;
		if (m_envCurrent != UINT32_MAX) {
			auto it = m_world->scene.envForNode.find(m_envCurrent);
			if (it != m_world->scene.envForNode.end())
				return it->second;
		}
		return m_world->scene.defaultEnvironment;
	}

	const SceneRenderSettings& EditorApp::RenderLook() const {
		// The scene's render look (the split postfx settings — see
		// SceneRenderSettings): the resolved block when a scene stands,
		// defaults otherwise (sceneless-safe like CurrentEnv's
		// fallbacks). No source selection — the look is scene-global.
		static const SceneRenderSettings fallback{};
		if (!m_world)
			return fallback;
		return m_world->scene.hasRenderSettings
			? m_world->scene.renderSettings
			: m_world->scene.defaultRenderSettings;
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
			if (hit == m_bindlessBySlot.end()) {
				// Crash diagnostic: a map miss uploads the RAW POOL
				// INDEX as the bindless slot — a bogus descriptor sample on
				// the first world-drawing frame (the observed device-death).
				// Log the miss so the offender material/slot is named.
				std::fprintf(stderr,
					"[MATSLOT] remap MISS: poolIdx=%d not in m_bindlessBySlot (map=%zu) — raw pool index becomes the bindless slot!\n",
					poolIdx, m_bindlessBySlot.size());
				std::fflush(stderr);
				return poolIdx;
			}
			return static_cast<int>(hit->second);
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
		// SafeLookAt: the SHARED degenerate-lookAt resolution (the same one
		// the camera drive paths use). The old direct lookAt picked
		// its own near-degenerate basis for the straight-down Top camera,
		// which rotated the editor's wire boxes 90 degrees against the scene
		// scene in the top view (the two agreed everywhere else).
		return SafeLookAt(EditorCamEye(c), c.target);
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

	// Locked look-through: viewport follows the bound scene camera node's
	// global pose + lens (forward = node -Z in world). False when unbound
	// or the node lost its camera (deleted/detached) — callers fall back
	// to the orbit camera, and the binding survives so undo restores it.
	bool EditorApp::BoundCameraPoseFor(int vp, BoundCameraPose& out) const {
		if (!m_world || vp < 0 || vp >= kMaxViewports)
			return false;
		const int bound = m_viewports[vp].boundNode;
		if (bound < 0 || bound >= int(m_world->scene.hierarchy.size()))
			return false;
		const ::SceneCamera* C = FindNodeCamera(uint32_t(bound));
		if (!C)
			return false;
		const glm::mat4& G = m_world->scene.globalTransform[uint32_t(bound)];
		out.eye = glm::vec3(G[3]);
		out.fwd = glm::vec3(G * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
		if (!(glm::length(out.fwd) > 1e-6f))
			return false;
		out.fwd = glm::normalize(out.fwd);
		glm::vec3 up = glm::vec3(G * glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
		if (!(glm::length(up) > 1e-6f)
			|| std::abs(glm::dot(up, out.fwd)) > 0.999f)
			// Degenerate (or parallel to the view direction): the SHARED
			// resolution (same convention as every other camera framing —
			// see ResolveLookAtUp) instead of an ad-hoc +X pick, which
			// disagreed with the drive paths.
			up = ResolveLookAtUp(out.eye, out.eye + out.fwd);
		else
			up = glm::normalize(up);
		out.up = up;
		out.nearClip = std::max(C->nearClip, 1e-4f);
		out.farClip = std::max(C->farClip, out.nearClip * 1.1f + 1e-3f);
		out.fovYDeg = std::clamp(C->fovYDeg, 1.0f, 179.0f);
		out.orthoHalfH = (C->projection == ::SceneCamera::Projection::Orthographic)
			? std::max(C->orthoHalfH, 0.01f) : 0.0f;
		return true;
	}

	bool EditorApp::BoundCameraView(int vp, glm::mat4& view, glm::mat4& proj,
		glm::vec3& eye, float& nearClip, float& farClip) const {
		BoundCameraPose pose;
		if (!BoundCameraPoseFor(vp, pose))
			return false;
		eye = pose.eye;
		nearClip = pose.nearClip;
		farClip = pose.farClip;
		view = glm::lookAt(eye, eye + pose.fwd, pose.up);
		const ViewportRect& rect = m_viewports[vp].rect;
		const float aspect = rect.h > 0.0f ? rect.w / rect.h : 1.0f;
		glm::mat4 p(1.0f);
		if (pose.orthoHalfH > 0.0f) {
			p = glm::orthoRH_ZO(-pose.orthoHalfH * aspect,
				pose.orthoHalfH * aspect, -pose.orthoHalfH, pose.orthoHalfH,
				farClip, nearClip);
		}
		else {
			p = MakeReversedZProjRH(glm::radians(pose.fovYDeg), aspect,
				nearClip, farClip);
		}
		p[1][1] *= -1.0f; // Vulkan Y-flip
		proj = p;
		return true;
	}

	void EditorApp::SetupDefaultCameras() {
		if (!m_editorCams.empty())
			return;
		m_editorCams.resize(4);
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
		m_editorCams[3].name = "Front";
		m_editorCams[3].type = EditorCam::Type::Orthographic;
		m_editorCams[3].target = glm::vec3(0.0f);
		// yaw 0 = eye at +Z looking along -Z: the engine's -Z-forward
		// convention seen head-on (Side is the same construction at yaw
		// pi/2 looking along -X).
		m_editorCams[3].yaw = 0.0f;
		m_editorCams[3].pitch = 0.0f;
		m_editorCams[3].dist = 25.0f;
		m_editorCams[3].orthoHalfH = 10.0f;
		// Viewport 0 = perspective; quad adds Top (1) + Side (2) +
		// Front (3) — the fourth slot was a second Perspective before.
		for (int i = 0; i < kMaxViewports; ++i)
			m_viewports[i].camIdx =
				(i == 1) ? 1 : (i == 2) ? 2 : (i == 3) ? 3 : 0;
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
		// Layout shrank (quad -> single via the View menu; the modal toggle
		// resets the active index, the menu path does not): a stale active
		// index pointed at an INVISIBLE viewport — orbit input, the F-key
		// modes and the World panel's camera section edited a camera no
		// panel shows (chip/content desync), and m_vp picked its stale
		// rect. Clamp to the visible range.
		if (m_activeVp >= count)
			m_activeVp = 0;
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
		engine::FrameUniforms frame = m_viewports[0].frame;
		// frame.view already follows the look-through binding (see
		// UpdateFrameData); only the projection is re-derived for the strip
		// aspect — from the bound lens when bound, else the orbit camera.
		const float aspect = stripExtent.height
			? float(stripExtent.width) / float(stripExtent.height) : 1.0f;
		glm::mat4 proj = EditorCamProj(m_editorCams[m_viewports[0].camIdx],
			aspect);
		glm::mat4 view = frame.view;
		if (m_viewports[0].boundNode >= 0) {
			if (const ::SceneCamera* C = FindNodeCamera(
				uint32_t(m_viewports[0].boundNode))) {
				glm::mat4 p(1.0f);
				if (C->projection
					== ::SceneCamera::Projection::Orthographic) {
					const float h = std::max(C->orthoHalfH, 0.01f);
					p = glm::orthoRH_ZO(-h * aspect, h * aspect, -h, h,
						frame.farPlane, frame.nearPlane);
				}
				else {
					p = MakeReversedZProjRH(glm::radians(
						std::clamp(C->fovYDeg, 1.0f, 179.0f)), aspect,
						frame.nearPlane, frame.farPlane);
				}
				p[1][1] *= -1.0f;
				proj = p;
			}
		}
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
			flight * sizeof(engine::FrameUniforms);
	}

	void EditorApp::DrawWorldNodes(smvk::CommandBuffer& cmd,
		const engine::IRenderer::PushConstants& basePc, bool maskedPhase, bool selectedOnly,
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
			engine::IRenderer::PushConstants objPc{};
			std::memcpy(objPc.model, glm::value_ptr(model), sizeof(objPc.model));
			objPc.materialId = item.material;
			cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, materialId),
				sizeof(objPc.materialId), &objPc.materialId, smvk::ShaderStage::All);
			cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, model),
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
		const engine::IRenderer::PushConstants& pc) {
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
			cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, materialId),
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
		uint32_t subtreeRoot, float zoom, glm::vec2 pan) const {
		// UV wires from the world's RETAINED uploads (the editor keeps no
		// parsed glTF; the cluster registry resolves node -> CPU geometry).
		outLines.clear();
		if (!m_world)
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
		const bool lightmap = (mode == ViewMode::FlatUv1);
		// Interactive panels keep wires readable; exports wire everything.
		const size_t wireTriCap = wholeScene ? 400000 : 60000;
		size_t tris = 0;
		// Whole-scene export uses identity (pane nav never affects PNGs).
		const bool applyNav = !wholeScene;
		for (const auto& [node, meshIdx] : m_world->scene.meshForNode) {
			if (!wholeScene && !subtree.contains(node))
				continue;
			const smvk::StaticMeshUpload* up = MeshUploadOf(meshIdx);
			if (!up || up->vertices.empty() || up->indices.empty())
				continue; // no exact geometry: no wires
			const size_t vcount = up->vertices.size();
			auto uvOf = [&](size_t i, glm::vec2& out) -> bool {
				if (i >= vcount)
					return false;
				out = lightmap ? up->vertices[i].texCoord2
					: up->vertices[i].texCoord;
				return true;
			};
			const size_t tcount = up->indices.size() / 3;
			for (size_t t = 0; t < tcount; ++t) {
				glm::vec2 uv[3];
				if (!uvOf(up->indices[t * 3 + 0], uv[0])
					|| !uvOf(up->indices[t * 3 + 1], uv[1])
					|| !uvOf(up->indices[t * 3 + 2], uv[2]))
					continue; // corrupt face: skip, keep the rest
				for (int k = 0; k < 3; ++k) {
					glm::vec2 na = applyNav ? UvToFlatNdc(uv[k], zoom, pan)
						: (uv[k] * 2.0f - glm::vec2(1.0f));
					glm::vec2 nb = applyNav ? UvToFlatNdc(uv[(k + 1) % 3], zoom, pan)
						: (uv[(k + 1) % 3] * 2.0f - glm::vec2(1.0f));
					outLines.push_back({ glm::vec3(na.x, na.y, 1.0f) });
					outLines.push_back({ glm::vec3(nb.x, nb.y, 1.0f) });
				}
				if (++tris > wireTriCap)
					return;
			}
		}
	}

	void EditorApp::RecordUvWindowPass(smvk::LogicalImageViewHandle swapView,
		uint32_t flight) {
		// UV editor window canvas: the selection's unwrapped shell + texture
		// fill (DrawFlatUvNodes), shell wires + face/vertex highlight on top
		// (own line buffer, identity viewProj, zoom/pan baked into verts).
		// Called right after DrawUI and BEFORE the imgui pass: the canvas
		// fill sits under the (alpha-0) window chrome; the color clear is
		// the canvas background. Canvas rect is same-frame (DrawUI just
		// laid the window out); clamped into the framebuffer because the
		// window can hang half off-screen.
		(void)flight;
		if (!m_showUvEditor)
			return;
		const ImVec2 fb = ImGui::GetIO().DisplaySize;
		const float cx0 = std::clamp(m_uvWinRectMin.x, 0.0f, fb.x);
		const float cy0 = std::clamp(m_uvWinRectMin.y, 0.0f, fb.y);
		const float cx1 = std::clamp(m_uvWinRectMax.x, 0.0f, fb.x);
		const float cy1 = std::clamp(m_uvWinRectMax.y, 0.0f, fb.y);
		const float w = cx1 - cx0, h = cy1 - cy0;
		if (w < 2.0f || h < 2.0f)
			return;
		// Wire geometry: selection subtree wires (depth-tested over the
		// fill) + the face/vertex highlight (on top, no depth).
		std::vector<smvk::DebugLineVertex> wires, hl;
		const bool hasSel = m_world && m_selected != UINT32_MAX &&
			m_selected < m_world->scene.hierarchy.size();
		if (hasSel)
			BuildFlatUvWires(wires, m_uvWinMode, m_selected, m_uvWinZoom,
				m_uvWinPan);
		glm::vec2 fuv[3];
		if (m_selMode == SelMode::Face && m_selFaceNode != UINT32_MAX
			&& SelectedFaceUvs(fuv, m_uvWinMode)) {
			for (int k = 0; k < 3; ++k) {
				const glm::vec2 na = UvToFlatNdc(fuv[k]);
				const glm::vec2 nb = UvToFlatNdc(fuv[(k + 1) % 3]);
				hl.push_back({ glm::vec3(na.x, na.y, 1.0f) });
				hl.push_back({ glm::vec3(nb.x, nb.y, 1.0f) });
			}
		}
		else if (m_selMode == SelMode::Vertex && m_selVertNode != UINT32_MAX) {
			glm::vec2 vuv;
			if (SelectedVertUv(vuv, m_uvWinMode)) {
				const glm::vec2 nc = UvToFlatNdc(vuv);
				const float s = 0.03f;
				hl.push_back({ glm::vec3(nc.x - s, nc.y, 1.0f) });
				hl.push_back({ glm::vec3(nc.x + s, nc.y, 1.0f) });
				hl.push_back({ glm::vec3(nc.x, nc.y - s, 1.0f) });
				hl.push_back({ glm::vec3(nc.x, nc.y + s, 1.0f) });
			}
		}
		const uint32_t wc = uint32_t(wires.size());
		const uint32_t hc = uint32_t(hl.size());
		std::vector<smvk::DebugLineVertex> all = std::move(wires);
		all.insert(all.end(), hl.begin(), hl.end());
		if (!all.empty()) {
			const size_t bytes = all.size() * sizeof(all[0]);
			if (m_uvWinLines.empty() || m_uvWinLinesCap < uint32_t(all.size())) {
				m_uvWinLines = m_context->CreateBuffer(bytes,
					smvk::BufferType_Vertex, "editor uv window lines");
				m_uvWinLinesCap = uint32_t(all.size());
			}
			if (!m_uvWinLines.empty())
				m_context->UploadDataToBuffer(m_uvWinLines, all.data(), bytes,
					smvk::PipelineStageBit_Vertex,
					smvk::AccessFlags::VertexAttributeRead);
		}
		// Flat fill push: identity model with the window zoom/pan (must
		// match uvflat.vert.hlsli + UvToFlatNdc).
		engine::IRenderer::PushConstants pc{};
		{
			const float z = (m_uvWinZoom > 1e-6f) ? m_uvWinZoom : 1.0f;
			pc.model[0] = z;
			pc.model[5] = z;
			pc.model[10] = 1.0f;
			pc.model[15] = 1.0f;
			pc.model[12] = m_uvWinPan.x;
			pc.model[13] = m_uvWinPan.y;
		}
		auto& pass = m_rendergraph->AddPass("UvWindow")
			.SetOffset(uint32_t(cx0), uint32_t(cy0))
			.SetExtent(smvk::Extent2D{ uint32_t(w), uint32_t(h) })
			.SetColor({ swapView },
				smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0.05f, 0.05f, 0.07f, 1.0f))
			.SetDepth(m_depth.logicalView,
				smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
				smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
		pass.SetRecordingFunc([this, pc, wc, hc](smvk::CommandBuffer& cmd) {
			// Fill: the selection's subtree (empty canvas when nothing is
			// selected — classic one-asset-at-a-time UV editor).
			DrawFlatUvNodes(cmd, m_uvWinMode, pc);
			if (m_uvWinLines.empty() || m_lineProgram.shaderCount != 2)
				return;
			const smvk::RasterSettings raster{
				.cullMode = smvk::CullMode::None };
			cmd.BindVertexBuffer(m_uvWinLines, 0, 0);
			engine::LineDebugPush lp{};
			const glm::mat4 ident(1.0f); // verts are already NDC-space
			std::memcpy(lp.viewProj, glm::value_ptr(ident), sizeof(lp.viewProj));
			// Wires: depth-tested over the fill (z=1 vs the fill's 0.9999).
			if (wc > 0) {
				cmd.BindRenderPipeline(m_lineProgram,
					smvk::VertexFormat::DebugLine,
					// Reversed-Z: GreaterEqual = closer-or-equal passes.
					smvk::DepthSettings{
						.depthFunc = smvk::CompareOp::GreaterEqual,
						.writeEnable = false, .depthTestEnable = true },
						smvk::StencilSettings{}, smvk::BlendSettings{},
						smvk::MSAASettings{}, raster,
						smvk::PrimitiveTopology::LineList);
				const glm::vec4 c = (m_uvWinMode == ViewMode::FlatUv1)
					? glm::vec4(0.35f, 0.7f, 1.0f, 1.0f)
					: glm::vec4(0.15f, 0.95f, 0.55f, 1.0f);
				lp.color[0] = c.x; lp.color[1] = c.y;
				lp.color[2] = c.z; lp.color[3] = c.w;
				cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
				cmd.Draw(wc, 1, 0, 0);
			}
			// Highlight: face loop (yellow) or vertex cross (cyan), on top.
			if (hc > 0) {
				cmd.BindRenderPipeline(m_lineProgram,
					smvk::VertexFormat::DebugLine,
					smvk::DepthSettings{
						.depthFunc = smvk::CompareOp::Always,
						.writeEnable = false, .depthTestEnable = false },
						smvk::StencilSettings{}, smvk::BlendSettings{},
						smvk::MSAASettings{}, raster,
						smvk::PrimitiveTopology::LineList);
				const glm::vec4 c = (m_selMode == SelMode::Face)
					? glm::vec4(1.0f, 0.9f, 0.2f, 1.0f)
					: glm::vec4(0.3f, 1.0f, 1.0f, 1.0f);
				lp.color[0] = c.x; lp.color[1] = c.y;
				lp.color[2] = c.z; lp.color[3] = c.w;
				cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
				cmd.Draw(hc, 1, wc, 0);
			}
		});
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

	float EditorApp::PreviewExposure() const {
		// The preview's standing exposure: the authored MANUAL value. The
		// App seeds its runtime adaptation from the same number
		// (m_exposureAdapted starts at manual, see ResolveSceneLights)
		// and meters + adapts from there; the preview has no
		// histogram pass to meter with, so the manual look is its best
		// faithful standing approximation (the same "the App renders the
		// rest" contract as the volumetric settings).
		return std::max(RenderLook().manualExposure, 1e-6f);
	}

	void EditorApp::RecordViewportTonemap(smvk::LogicalImageViewHandle swapView,
		const ViewportRect& rect, const char* name) {
		// One tonemap layer per rect (the nvapp element-layer pattern, see
		// m_sceneHdr): the layer writes the rect of the swap (Load keeps
		// the chrome + sibling layers outside it) and samples the MATCHING
		// sub-rect of the shared full-window HDR through the srcUv window -
		// texel centers, so linear taps never bleed the neighbouring
		// viewport's content at the rect edges.
		if (swapView.empty() || !m_previewTonemap.ShadersValid() || !m_sceneHdr.Valid())
			return;
		if (rect.w < 1.0f || rect.h < 1.0f)
			return;
		const float invW = 1.0f / std::max(float(m_sceneHdr.extent.width), 1.0f);
		const float invH = 1.0f / std::max(float(m_sceneHdr.extent.height), 1.0f);
		const float uvMinX = (rect.x + 0.5f) * invW;
		const float uvMinY = (rect.y + 0.5f) * invH;
		const float uvMaxX = (rect.x + rect.w - 0.5f) * invW;
		const float uvMaxY = (rect.y + rect.h - 0.5f) * invH;
		smvk::TonemapDesc d{};
		// Authored look: the render settings' operator + the standing
		// exposure (see PreviewExposure - no bloom layer, the preview
		// shows the scene).
		d.exposure = PreviewExposure();
		d.tonemapMode = RenderLook().tonemapMode;
		m_previewTonemap.AddTonemapPass(*m_rendergraph,
			m_sceneHdr.logicalView,
			m_sceneHdr.bindlessIndex,
			{}, UINT32_MAX, 0.0f,   // no bloom layer in the preview
			m_context->GetOrCreateSampler(smvk::SamplerCreateInfo{
				.magFilter = smvk::Filter::Linear, .minFilter = smvk::Filter::Linear,
				.mipmapMode = smvk::MipmapMode::Linear,
				.addressU = smvk::AddressMode::ClampToEdge,
				.addressV = smvk::AddressMode::ClampToEdge,
				.addressW = smvk::AddressMode::ClampToEdge,
				.maxAnisotropy = 1.0f }), // dedupes to slot 0 (EnsureShaders)
			{ swapView },
			smvk::Extent2D{ uint32_t(rect.w), uint32_t(rect.h) },
			d, name,
			{}, UINT32_MAX, 0u, 16.0f,   // no motion-vector debug layer
			{}, UINT32_MAX, 0.0f,        // no AO layer
			false,                       // sRGB swapchain: linear store
			UINT32_MAX, 0.0f,            // no artistic LUT
			0.0f, 0.0f,                  // no jitter (no temporal AA here)
			uint32_t(rect.x), uint32_t(rect.y),
			smvk::AttachmentLoadOp::Load, // keep chrome + sibling layers
			uvMinX, uvMinY, uvMaxX, uvMaxY);
	}

	void EditorApp::RecordMainPass(smvk::LogicalImageViewHandle swapView, int vpIdx,
		uint32_t flight, bool selectedWireOverlay) {
		const ViewportRect& vp = m_viewports[vpIdx].rect;
		// View mode is PER VIEWPORT (UV views are panel types too).
		const ViewMode mode = m_viewports[vpIdx].viewMode;
		smvk::ShaderProgram& prog = MainProgram(mode);
		// Meshlet view has its own program + draw path (no material legs).
		const bool meshletDbg =
			(mode == ViewMode::Meshlets) && m_meshDebugProgram.shaderCount == 2;
		if (!meshletDbg && prog.shaderCount != 2)
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

		engine::IRenderer::PushConstants pc{};
		pc.frameUniforms = frameAddr;
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
		pc.pad_pc = 0; // dithered transparency
		pc.renderViews = 0;
		pc.mainRenderView = UINT32_MAX;
		pc.renderViewIndex = UINT32_MAX;

		// Viewport rect only (the EditorBg pass clears the rest of the
		// window behind the panels). Color loads the bg clear; depth
		// clears in the rect (reversed-Z).
		// Display-chain routing (see m_sceneHdr + the file header): every
		// MainProgram view except Baked is a tonemap-chain shader (raw-lux
		// radiance or a 1/frame.exposure pre-scale for the App's
		// hdr * exposure -> operator chain) - those render into the shared
		// full-window HDR at the SAME rect (offset/extent/depth unchanged:
		// the gizmo grid keeps its depth-tested occlusion) and a tonemap
		// layer resolves the rect to the swap. Baked carries its own
		// Reinhard + gamma display mapping and stays direct. HDR/tonemapper
		// unavailable (creation failure) falls back to the direct write
		// (washed out, but never dead).
		const bool viaHdr = !bakedOnly
			&& m_previewTonemap.ShadersValid() && m_sceneHdr.Valid();
		const smvk::LogicalImageViewHandle colorTarget =
			viaHdr ? m_sceneHdr.logicalView : swapView;
		auto& pass = m_rendergraph->AddPass("EditorMain")
			.SetOffset(uint32_t(vp.x), uint32_t(vp.y))
			.SetExtent(smvk::Extent2D{ uint32_t(vp.w), uint32_t(vp.h) })
		.SetColor({ colorTarget },
			smvk::AttachmentLoadOp::Load, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0.07f, 0.07f, 0.09f, 1.0f))
		.SetDepth(m_depth.logicalView,
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
		pass.SetRecordingFunc([this, pc, selectedWireOverlay, bakedOnly, meshletDbg, uvDbg, mode](smvk::CommandBuffer& cmd) {
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
				engine::IRenderer::PushConstants tpc = pc;
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
						cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, materialId),
							sizeof(matId), &matId, smvk::ShaderStage::All);
						cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, model),
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
		// The matching tonemap layer for this viewport rect (declared right
		// after the scene pass: the topo sort orders the HDR read after this
		// write, and the stable declaration order keeps it under the gizmo/
		// imgui overlays - see RenderFrame).
		if (viaHdr)
			RecordViewportTonemap(swapView, vp, "EditorTonemap");
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

		engine::IRenderer::PushConstants pc{};
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

		// Display-chain routing (same as RecordMainPass, see m_sceneHdr):
		// the inset's programs (uv0/uv1/lit) are tonemap-chain shaders, so
		// the strip renders into the shared HDR at the strip rect and a
		// tonemap layer resolves it; a missing HDR/tonemapper falls back to
		// the direct write.
		const bool viaHdr =
			m_previewTonemap.ShadersValid() && m_sceneHdr.Valid();
		const smvk::LogicalImageViewHandle colorTarget =
			viaHdr ? m_sceneHdr.logicalView : swapView;
		auto& pass = m_rendergraph->AddPass("EditorInset")
			.SetOffset(stripX, uint32_t(vp.y))
			.SetExtent(strip)
			.SetColor({ colorTarget },
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
				engine::IRenderer::PushConstants tpc = pc;
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
						cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, materialId),
							sizeof(matId), &matId, smvk::ShaderStage::All);
						cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, model),
							sizeof(model), glm::value_ptr(model), smvk::ShaderStage::All);
					for (const auto& s : mesh->geometry.surfaces)
						cmd.DrawIndexed(s.indexCount, 1, s.firstIndex, s.baseVertex);
					});
				}
			}
		});
		// The matching tonemap layer for the strip rect (see RecordMainPass).
		if (viaHdr)
			RecordViewportTonemap(swapView,
				ViewportRect{ float(stripX), float(vp.y), float(strip.width),
					float(strip.height) },
				"EditorInsetTonemap");
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
		// The meshdebug contract (meshdebug.frag.hlsl): the C++ side
		// pre-scales the push color by 1/frame exposure so the
		// tonemapper's hdr * exposure reproduces the authored display
		// color (App parity - the preview's tonemap layer applies the
		// same exposure, see PreviewExposure).
		const float meshletColorPreScale = 1.0f / PreviewExposure();
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
		engine::MeshDebugPush dp{};
		std::memcpy(dp.viewProj, glm::value_ptr(m_lastViewProj),
			sizeof(dp.viewProj));
		const smvk::IteratorFlags req = smvk::IteratorFlags(
			smvk::IteratorFlagBit_Renderable |
			smvk::IteratorFlagBit_Opaque |
			smvk::IteratorFlagBit_StaticVertexFmt);
		// Fallback leg (static monoblock indices): meshes without cooked
		// meshlets draw dim gray — nothing vanishes in the debug view.
		// (Pre-scaled per the contract above, like the palette leg below.)
		dp.color[0] = 0.45f * meshletColorPreScale;
		dp.color[1] = 0.45f * meshletColorPreScale;
		dp.color[2] = 0.5f * meshletColorPreScale;
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
				// Contract pre-scale (see DrawMeshletNodes top): rgb only,
				// the alpha carries no color.
				dp.color[0] *= meshletColorPreScale;
				dp.color[1] *= meshletColorPreScale;
				dp.color[2] *= meshletColorPreScale;
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
		// Preview HDR (best-effort, see m_sceneHdr): a creation failure
		// falls back to the direct LDR write (RecordMainPass/RecordInsetPass
		// gate on m_sceneHdr.Valid()), never a dead editor.
		(void)m_sceneHdr.Ensure(*m_context, *m_rendergraph, extent,
			smvk::HdrDesc{
				.name = "editorPreviewHdr",
				.viewName = "editorPreviewHdrView",
				.logicalName = "editorPreviewHdrLogical" });

		// Viewport rects (single central or 2x2 quad) between the panels;
		  // the hovered viewport is ACTIVE (input, gizmo, shadows, framing).
		ComputeViewportRects(extent);

		UpdateCamera(delta_time_s);
		// vi-style modal keys (editor_modal.cpp): the ACTIVE mode's keymap
		// (LAYOUT/LIGHTING/TEST/PROFILE; 1-4 switch from any mode) interprets
		// keys, with a global fallback table (mode switches, shading F1-F3,
		// undo/redo/save, window toggles). Skipped while ImGui wants the
		// keyboard (typing) or the Q radial popup is open.
		UpdateModalInput();

		// Per-flight slot: fb.frameIndex is the fence-gated in-flight index
		// (AcquireFramebuffer waited its fence before returning it), so the
		// ring slices sized by framesInFlight() are safe to overwrite.
		// fb.imageIndex is presentation order (0/2/1/0...) and MUST NOT be
		// used here: two swapchain images can map to the same
		// imageIndex % flightCount while the GPU still consumes that slice
		// (frameHot BDA, uniforms, lights, cascades...). fb.imageIndex only
		// identifies the swap target: m_swapViews[fb.imageIndex].
		const uint32_t flight = fb.frameIndex;
		assert(flight < m_flightCount);
		UpdateTextureStreaming(extent);

		// ----------------------------------------------------
		// Process ALL editor interaction here.
		// GizmoHandleInput(), selection, painting, etc.
		// ----------------------------------------------------

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

		// Viewports keep the mesh/face/vertex click + rubber-band box.
		// Flat-UV picking lives in the UV editor window (ImGui mouse).
		// ---- terrain height painting --------------------------------------
		// Paint mode replaces the LMB press path entirely: no box select, no
		// gizmo grab, no picking. One stamp per click; drags stamp at
		// radius-spaced intervals along the surface.
		const bool painting = m_world && m_paintTerrain
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
		else if (m_world && !m_gizmoDragging && !altNav &&
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
		if (m_vpBoxDragging) {
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
					// X-ray markers first (lights/cvol centers/cubemap
					// pins carry no mesh to raycast — the click twin of
					// the gizmo diamonds and capture balls); meshes
					// otherwise.
					std::optional<PickHit> hit =
						PickGizmoMarker(m_activeVp, lmx, lmy, vpExt);
					if (!hit)
						hit = PickNode(m_activeVp, lmx, lmy, vpExt);
					if (hit) {
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
		else if (m_gizmoDragging || (inVp && !altNav &&
			m_input.isMouseButtonHeld(GLFW_MOUSE_BUTTON_LEFT) && !uiEats)) {
			GizmoHandleInput();
		}

		// Any operation above that changes local transforms must also
		// have propagated scene.globalTransform by this point.

		// NOW snapshot camera/render state.
		UpdateFrameData(flight);
		// Second-viewport frame slice (strip aspect) for the inset pass.
		smvk::Extent2D insetStrip{ 0, 0 };
		if (SecondViewActive() && m_vpLayout == 0) {
			uint32_t insetX = 0;
			insetStrip = InsetStrip(m_viewports[0].rect, insetX);
			if (insetStrip.width != 0)
				UpdateInsetFrameData(flight, insetStrip);
		}

		// ---- PASSES: background clear + per-viewport scene + overlays ----
		// The whole window clears FIRST (the dark chrome color shows
		// through the gaps between the ImGui panels), then each viewport
		// renders its editor pass, then the gizmo overlay draws on top
		// — one command buffer, ordered by the graph rounds
		// (cross-round ordering rides the context-global sync-state
		// tables: every round's first use of a shared image barriers
		// against the previous round's writes).
		m_rendergraph->NewFrame();
		{
			// Window chrome backdrop: neutral mid grey (no blue tint — the
			// old 0.07/0.07/0.09 read blue next to the scene). Shows
			// through the gaps between panels + viewports.
			auto& bg = m_rendergraph->AddPass("EditorBg")
				.SetOffset(0, 0)
				.SetExtent(extent)
				.SetColor({ m_swapViews[fb.imageIndex] },
					smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
					smvk::ClearColor(0.18f, 0.18f, 0.18f, 1.0f))
				.SetDepth(m_depth.logicalView,
					smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
					smvk::ClearDepthStencil(0.0f, 0));
			bg.SetRecordingFunc([](smvk::CommandBuffer& cmd) { (void)cmd; });
		}
		if (m_sceneHdr.Valid()) {
			// Preview HDR black clear (full window, see m_sceneHdr): the
			// sub-rect scene passes LOAD their rects, so UV-shell gaps
			// (and a shrunken viewport's vacated margin) would otherwise
			// tonemap STALE rects. Full-extent clear = no sub-rect render
			// area (the driver slow path the shadow-tile workaround exists
			// for), and black clears to black through every operator.
			auto& hdrBg = m_rendergraph->AddPass("EditorHdrBg")
				.SetOffset(0, 0)
				.SetExtent(extent)
				.SetColor({ m_sceneHdr.logicalView },
					smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
					smvk::ClearColor(0, 0, 0, 1));
			hdrBg.SetRecordingFunc([](smvk::CommandBuffer& cmd) { (void)cmd; });
		}
		m_rendergraph->Execute(fb);

		// ---- scene passes (per viewport) + overlay ---------------------
		m_rendergraph->NewFrame();
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
			// UV editor window canvas: after DrawUI (same-frame rect), before
			// the imgui pass (the alpha-0 chrome draws over the fill).
			RecordUvWindowPass(m_swapViews[fb.imageIndex], flight);
			m_imgui->AddPass(*m_rendergraph, m_swapViews[fb.imageIndex], extent,
				smvk::AttachmentLoadOp::Load, smvk::ClearColor(0, 0, 0, 1));
		}
		// Flush bindless descriptor writes (scene texture views, samplers)
		// BEFORE Execute records — App does the same (see WorldRenderer).
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
