// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Engine CPU-reference scene renderer: the editor is a client, not the
// owner of the tracer. The renderer consumes a fixed, detached snapshot.
#include "editor_app.h"

#include <Engine/GI/bake.h>
#include <Engine/App/cluster.h>
#include <span>
#include <Engine/Renderer/ReferenceSceneRenderer.h>
#include <Engine/Rendergraph/environment.h>
#include <Engine/Scene/scene.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <imgui.h>
#include <stb_image_write.h>

namespace editor {

EditorApp::~EditorApp() {
	CancelReferenceRender();
}

void EditorApp::CancelReferenceRender() {
	m_referenceCancel.store(true, std::memory_order_relaxed);
	if (m_referenceJob.valid()) {
		m_referenceJob.wait(); // worker cannot outlive EditorApp's atomics
		try { (void)m_referenceJob.get(); }
		catch (...) {} // cancellation may have stopped a failing job
	}
	m_referenceRunning = false;
	m_referenceRows.store(0, std::memory_order_relaxed);
}

void EditorApp::StartReferenceRender() {
	if (!m_world || m_referenceRunning)
		return;
	CancelReferenceRender();

	auto scene = std::make_shared<tour::gi::GiTraceScene>();
	tour::gi::GiSceneBuildOptions opts{};
	engine::FrameUniforms frame{};
	ApplyEnvToFrame(frame, opts.env);
	opts.includeDynamicLights = true;
	opts.staticLights = true;
	opts.retainVertexNormals = true;
	// All visible meshes, not just GI-shadow-casting surfaces.
	opts.excludeDecalOverlays = false;
	opts.traceBackendEmbree = true;
	// Decode the same source KTX data as the GI baker: texture samples
	// are evaluated at camera hits, not reduced to flat material factors.
	for (const auto& cluster : m_world->clusters) {
		if (!cluster) continue;
		for (const auto& [uri, handle] : cluster->ListTextureUris()) {
			const auto* source = cluster->GetTextureSource(uri);
			if (!source || source->empty()) continue;
			const std::span<const uint8_t> bytes(*source);
			if (auto avg = tour::gi::GiAverageKtxTexel(bytes))
				opts.texelAvg[handle.index()] = *avg;
			if (auto data = tour::gi::GiDecodeKtxTexelData(
					bytes, tour::gi::kGiBakeTexelMaxDim))
				opts.texelData[handle.index()] = std::move(*data);
		}
	}
	std::string error;
	tour::gi::ExtractTraceSceneFromWorld(*m_world, opts, *scene, error);
	if (!error.empty()) {
		m_referenceStatus = "Reference extraction: " + error;
		return;
	}

	tour::renderer::ReferenceCamera camera{};
	BoundCameraPose pose{};
	if (BoundCameraPoseFor(m_activeVp, pose)) {
		camera.position = pose.eye;
		camera.forward = pose.fwd;
		camera.up = pose.up;
		camera.fovYDeg = pose.fovYDeg;
		camera.orthoHalfHeight = pose.orthoHalfH;
	}
	else {
		const EditorCam& cam = ActiveCam();
		camera.position = EditorCamEye(cam);
		camera.forward = glm::normalize(cam.target - camera.position);
		camera.up = glm::vec3(0, 1, 0);
		camera.fovYDeg = cam.fovYDeg;
		camera.orthoHalfHeight = cam.type == EditorCam::Type::Orthographic
			? cam.orthoHalfH : 0.0f;
	}
	tour::renderer::ReferenceRenderSettings settings{};
	settings.width = settings.height =
		uint32_t(std::clamp(m_referenceResolution, 64, 1024));
	settings.samplesPerPixel = uint32_t(std::clamp(m_referenceSpp, 1, 1024));
	settings.maxBounces = uint32_t(std::clamp(m_referenceBounces, 0, 16));
	settings.exposure = std::clamp(m_referenceExposure, 0.000001f, 1.0f);
	settings.seed = 0x2026u;
	m_referenceExposure = settings.exposure;

	namespace fs = std::filesystem;
	m_referencePath = (fs::path(ResolveVfsRoot()) /
		"exports" / "reference_scene.png").generic_string();
	m_referenceCancel.store(false, std::memory_order_relaxed);
	m_referenceRows.store(0, std::memory_order_relaxed);
	m_referenceStatus = "Rendering Embree CPU reference";
	m_referenceRunning = true;
	// Keep all editor/World access outside the task. A snapshot captures
	// unsaved edits and can survive a scene reload without stale pointers.
	m_referenceJob = std::async(std::launch::async,
		[this, scene = std::move(scene), camera, settings]() mutable {
			scene->Finalize();
			if (scene->Bvh().Backend() != tour::gi::TraceBackend::Embree)
				throw std::runtime_error("Embree scene construction failed");
			return tour::renderer::ReferenceSceneRenderer::Render(
				*scene, camera, settings, &m_referenceCancel, &m_referenceRows);
		});
}

void EditorApp::DrawReferencePanel() {
	if (m_referenceRunning && m_referenceJob.valid()
		&& m_referenceJob.wait_for(std::chrono::seconds(0))
			== std::future_status::ready) {
		m_referenceRunning = false;
		try {
			auto frame = m_referenceJob.get();
			if (frame.linearRadiance.empty()) {
				m_referenceStatus = "Reference render cancelled";
			}
			else {
				namespace fs = std::filesystem;
				std::error_code ec;
				fs::create_directories(fs::path(m_referencePath).parent_path(), ec);
				const auto png = frame.ToRgba8(m_referenceExposure);
				const bool saved = !png.empty() && stbi_write_png(
					m_referencePath.c_str(), int(frame.width), int(frame.height),
					4, png.data(), int(frame.width * 4)) != 0;
				const std::string hdr = fs::path(m_referencePath)
					.replace_extension(".pfm").generic_string();
				const bool savedHdr = frame.WritePfm(hdr);
				m_referenceHaveImage = saved;
				m_referenceStatus = saved && savedHdr
					? "Saved PNG + linear HDR PFM"
					: "Reference export failed (PNG or PFM)";
				Log("Reference scene: " + m_referenceStatus + " (" + m_referencePath + ")");
			}
		}
		catch (const std::exception& e) {
			m_referenceStatus = std::string("Reference failed: ") + e.what();
		}
	}
	ImGui::TextWrapped("CPU scene reference (Embree + GI BSDF). "
		"Uses a snapshot of the current editor world and active viewport camera.");
	if (!m_world) {
		ImGui::TextDisabled("Open a scene to render.");
		return;
	}
	ImGui::SliderInt("Resolution", &m_referenceResolution, 64, 1024, "%d px");
	ImGui::SliderInt("Samples/pixel", &m_referenceSpp, 1, 256);
	ImGui::SliderInt("Bounces", &m_referenceBounces, 0, 8);
	ImGui::DragFloat("Exposure", &m_referenceExposure, 0.0001f,
		0.000001f, 1.0f, "%.5f");
	if (m_referenceRunning) {
		if (ImGui::Button("Cancel reference render")) {
			CancelReferenceRender();
			m_referenceStatus = "Reference render cancelled";
		}
		ImGui::ProgressBar(float(m_referenceRows.load(std::memory_order_relaxed))
			/ float(std::max(m_referenceResolution, 1)), ImVec2(-1, 0));
	}
	else if (ImGui::Button("Render scene reference"))
		StartReferenceRender();
	ImGui::TextWrapped("%s", m_referenceStatus.c_str());
	if (m_referenceHaveImage && !m_referencePath.empty()) {
		const ImTextureID texture = AssetSidecarThumb(m_referencePath);
		if (texture) {
			const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
			ImGui::Image(texture, ImVec2(w, w));
		}
		ImGui::TextWrapped("%s", m_referencePath.c_str());
	}
	ImGui::Separator();
	ImGui::TextDisabled("Reference limits: static geometry, no normal maps, "
		"no refractive glass, no authored shader hooks or post-FX. "
		"GPU image and CPU image require matching exposure to compare.");
}

} // namespace editor
