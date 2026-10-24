// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The SIDECAR MATERIAL BALL PREVIEW (see editor_app.h's decl block): an
// offscreen 256x256 pass — a unit sphere wearing the material's PBR look
// under a FIXED default sky (matball.frag.hlsl's kDefaultEnv) — read back
// after the submit and written as a display-ready PNG next to the
// material's SOURCE .material asset (the sidecar: <file>.png).
// The render rides the lightmap-export pattern: a queued material index
// records into the frame's graph (the between-rounds position), the
// post-submit WaitForDeviceIdle + DownloadDataFromImage lands the bytes,
// stb_image_write writes the file.

#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <Engine/Renderer/mesh.h> // TerrainVertex + TerrainVertexFormat
#include <Engine/platform.h>       // readFile (the .spv loader)
#include <stb_image_write.h>

namespace editor {

// Mirrors matball.vert.hlsl / matball.frag.hlsl's MatBallPush EXACTLY —
// the layout-identity contract (check the static_assert).
struct MatBallPush {
	glm::mat4 viewProj;        // 0   fixed preview camera (reversed-Z, Y-flipped)
	glm::vec4 eye;            // 64  xyz camera
	glm::vec4 baseColorFactor; // 80  rgba
	glm::vec4 emissiveFactor;  // 96  rgb, w = strength
	glm::vec4 params;         // 112 x metallic, y perceptual roughness
	uint32_t baseTex;         // 128 bindless (0xffffffffu = unbound)
	uint32_t mrTex;           // 132
	uint32_t normalTex;       // 136
	uint32_t occlTex;         // 140
};
static_assert(sizeof(MatBallPush) == 144);

bool EditorApp::EnsureMatBallResources() {
	if (m_matBallReady)
		return true;
	if (!m_context || !m_rendergraph)
		return false;
	// ---- The offscreen target (sRGB storage like the swapchain — the
	// tonemapped+gamma'd FS output lands PNG-ready; TransferSrc for the
	// readback; NO depth: the sky tri draws first, the opaque sphere
	// second, nothing overlaps).
	{
		smvk::ImageCreateInfo ci{};
		ci.extent = { 256, 256, 1 };
		ci.format = smvk::Format::RGBA8_SRGB;
		ci.usage = smvk::ImageUsage::ColorAttachment
			| smvk::ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "matBall";
		m_matBallImage = m_context->CreateImage(ci, nullptr);
		if (m_matBallImage.empty())
			return false;
		smvk::ImageViewCreateInfo vci{};
		vci.viewType = smvk::ImageViewType::View2D;
		vci.format = smvk::Format::Undefined; // inherit
		vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
		vci.name = "matBallView";
		m_matBallView = m_context->CreateImageView(m_matBallImage, vci);
		if (m_matBallView.empty()) {
			m_matBallImage = {};
			return false;
		}
		m_matBallLogical = m_rendergraph->AddExternalImageView(
			m_matBallView, smvk::Format::RGBA8_SRGB, "matBallLogical");
		if (m_matBallLogical.empty()) {
			m_matBallImage = {};
			m_matBallView = {};
			return false;
		}
	}
	// ---- Geometry: the sphere + the fullscreen-sky tri, both in
	// TerrainVertex (the probeball sphere recipe — the uv rides the
	// tangent halves' xy, the draw FLAG rides color.a: 255 = sphere,
	// 0 = sky).
	{
		constexpr int kSlices = 48;
		constexpr int kStacks = 32;
		constexpr float kPi = 3.14159265359f;
		const uint32_t rows = uint32_t(kStacks) + 1u;
		const uint32_t cols = uint32_t(kSlices) + 1u;
		const uint32_t verts = rows * cols;
		const uint32_t tris = uint32_t(kStacks) * uint32_t(kSlices) * 2u;
		// (Plain-size locals: `std::vector<T> v(size_t(n))` parses as a
		// function declaration — the most vexing parse; same as
		// probeball.cpp.)
		const size_t vertCountSz = size_t(verts);
		const size_t idxCountSz = size_t(tris) * 3u;
		std::vector<smvk::TerrainVertex> sv(vertCountSz);
		std::vector<uint32_t> si(idxCountSz);
		{
			smvk::TerrainVertex* out = sv.data();
			for (int j = 0; j <= kStacks; ++j) {
				const float phi = kPi * float(j) / float(kStacks);
				for (int i = 0; i <= kSlices; ++i) {
					const float theta =
						2.0f * kPi * float(i) / float(kSlices);
					const glm::vec3 n(std::sin(phi) * std::cos(theta),
						std::cos(phi), std::sin(phi) * std::sin(theta));
					smvk::TerrainVertexSrc src{};
					src.pos = n; // the unit sphere: pos == normal
					src.normal = n;
					// The sphere's analytic uv (equi): u = theta/2pi,
					// v = phi/pi — packed into the tangent slot's xy.
					src.tangent = glm::vec3(
						theta / (2.0f * kPi), phi / kPi, 0.0f);
					src.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f); // flag=1
					smvk::PackTerrainVertex(src, *out);
					++out;
				}
			}
		}
		{
			uint32_t k = 0;
			for (int j = 0; j < kStacks; ++j) {
				for (int i = 0; i < kSlices; ++i) {
					const uint32_t a = uint32_t(j) * cols + uint32_t(i);
					const uint32_t b = a + 1u;
					const uint32_t c = a + cols;
					const uint32_t d = c + 1u;
					// (The probeball winding note: the mirrored (a, +col,
					// +row) order keeps the normal OUTWARD.)
					si[k++] = a;
					si[k++] = b;
					si[k++] = c;
					si[k++] = b;
					si[k++] = d;
					si[k++] = c;
				}
			}
		}
		m_matBallSphereVB = m_context->CreateBuffer(
			sizeof(smvk::TerrainVertex) * sv.size(),
			smvk::BufferType_Vertex, "matBall sphere");
		m_matBallSphereIB = m_context->CreateBuffer(
			sizeof(uint32_t) * si.size(), smvk::BufferType_Index,
			"matBall indices");
		if (m_matBallSphereVB.empty() || m_matBallSphereIB.empty()) {
			m_matBallSphereVB = {};
			m_matBallSphereIB = {};
			return false;
		}
		m_context->UploadDataToBuffer(m_matBallSphereVB, sv.data(),
			sizeof(smvk::TerrainVertex) * sv.size(),
			smvk::PipelineStage(smvk::PipelineStageBit_AllGraphics),
			smvk::AccessFlags::VertexAttributeRead);
		m_context->UploadDataToBuffer(m_matBallSphereIB, si.data(),
			sizeof(uint32_t) * si.size(),
			smvk::PipelineStage(smvk::PipelineStageBit_AllGraphics),
			smvk::AccessFlags::IndexRead);
		m_matBallSphereIdx = uint32_t(si.size());
	}
	// ---- The sky triangle: NDC corners + the CPU-built world RAYS in
	// the normal slot (flag 0 via color.a). The camera: eye elevated
	// slightly above the equator, 2.9 units out, fovY 38deg, 1:1.
	glm::vec3 eye(0.0f, 0.55f, 2.9f);
	glm::vec3 at(0.0f, 0.0f, 0.0f);
	glm::vec3 up(0.0f, 1.0f, 0.0f);
	const glm::mat4 view = glm::lookAtRH(eye, at, up);
	const glm::mat4 proj = MakeReversedZProjRH(glm::radians(38.0f), 1.0f,
		0.05f, 20.0f);
	m_matBallViewProj = proj;
	m_matBallViewProj[1][1] *= -1.0f; // Vulkan Y-flip (EditorCamProj's)
	m_matBallEye = eye;
	{
		// Corner rays: unproject the NDC corners through the inverse
		// viewProj on the w=1 plane (a ray per corner; the FS normalizes
		// the interpolated ray).
		const glm::mat4 invVP = glm::inverse(m_matBallViewProj);
		const glm::vec2 corners[3] = { { -1.0f, -1.0f }, { 3.0f, -1.0f },
			{ -1.0f, 3.0f } };
		smvk::TerrainVertex tv[3]{};
		for (int c = 0; c < 3; ++c) {
			glm::vec4 wp = invVP * glm::vec4(corners[c], 1.0f, 1.0f);
			wp /= wp.w;
			smvk::TerrainVertexSrc src{};
			src.pos = glm::vec3(corners[c], 0.0f); // NDC through the VS
			src.normal = glm::normalize(glm::vec3(wp) - eye); // the ray
			src.tangent = glm::vec3(0.0f);
			src.color = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // flag=0
			smvk::PackTerrainVertex(src, tv[c]);
		}
		m_matBallSkyVB = m_context->CreateBuffer(sizeof(tv),
			smvk::BufferType_Vertex, "matBall sky tri");
		if (m_matBallSkyVB.empty())
			return false;
		m_context->UploadDataToBuffer(m_matBallSkyVB, tv, sizeof(tv),
			smvk::PipelineStage(smvk::PipelineStageBit_AllGraphics),
			smvk::AccessFlags::VertexAttributeRead);
	}
	// ---- The program (the compiled shader pair — the engine's shader
	// set, matball.vert/frag.hlsl).
	try {
		m_matBallVsModule = m_context->CreateShaderModule(
			readFile("content/shaders/matball.vert.hlsl.spv"), "matBallVS");
		m_matBallFsModule = m_context->CreateShaderModule(
			readFile("content/shaders/matball.frag.hlsl.spv"), "matBallFS");
	}
	catch (const std::exception& e) {
		std::fprintf(stderr, "[MatBall] shader compile: %s\n", e.what());
		return false;
	}
	if (m_matBallVsModule.empty() || m_matBallFsModule.empty())
		return false;
	const smvk::Shader vs{ .module = m_matBallVsModule,
		.entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	const smvk::Shader fs{ .module = m_matBallFsModule,
		.entryPoint = "mainPS", .stage = smvk::ShaderStage::Fragment };
	m_matBallProgram = { .shaders = { vs, fs }, .shaderCount = 2 };
	m_matBallReady = true;
	return true;
}

void EditorApp::RecordMatBallPass() {
	// The queued material's ball into the frame's graph (the lightmap
	// export's record position — between the scene passes and the imgui
	// pass; the offscreen logical is an external view, the cross-graph
	// barrier rides the context-global sync tables).
	if (m_matBallLogical.empty() || m_matBallQueuedMat == UINT32_MAX)
		return;
	smvk::Material* m = m_world ? m_world->GetMaterial(m_matBallQueuedMat)
		: nullptr;
	if (!m)
		return;
	MatBallPush push{};
	push.viewProj = m_matBallViewProj;
	push.eye = glm::vec4(m_matBallEye, 1.0f);
	push.baseColorFactor = m->values.diffuse;
	push.emissiveFactor = glm::vec4(
		glm::vec3(m->values.emissiveFactor), m->values.params2.z);
	push.params = glm::vec4(m->values.params.y, // metallic
		m->values.params.z, 0.0f, 0.0f);        // roughness
	const auto slotOrNone = [](int s) -> uint32_t {
		return s >= 0 ? uint32_t(s) : 0xffffffffu;
	};
	push.baseTex = slotOrNone(m->values.textures.x);
	push.mrTex = slotOrNone(m->values.textures.y);
	push.normalTex = slotOrNone(m->values.textures.z);
	push.occlTex = slotOrNone(m->values.textures.w);

	smvk::ShaderProgram progCopy = m_matBallProgram; // copyable (handles)
	// Raw handles into the lambda (Holder is move-only; the members keep
	// the ownership — the recorded pass borrows).
	const smvk::BufferHandle skyVB = m_matBallSkyVB;
	const smvk::BufferHandle sphereVB = m_matBallSphereVB;
	const smvk::BufferHandle sphereIB = m_matBallSphereIB;
	const uint32_t sphereIdx = m_matBallSphereIdx;
	auto& rg = *m_rendergraph;
	rg.AddPass("MatBall")
		.SetExtent(smvk::Extent2D{ 256, 256 })
		.SetColor({ m_matBallLogical },
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0.03f, 0.03f, 0.05f, 1.0f))
		.SetRecordingFunc([progCopy, push, skyVB, sphereVB, sphereIB,
			sphereIdx](smvk::CommandBuffer& cmd) mutable {
			// No depth (nothing overlaps: sky first, opaque sphere
			// second), no cull (the sky tri's raster-space winding under
			// the Y-flip is not worth a correctness bet; the closed
			// sphere renders identically either way).
			smvk::RasterSettings raster{};
			raster.cullMode = smvk::CullMode::None;
			cmd.BindRenderPipeline(progCopy,
				smvk::VertexFormat::TerrainVertexFormat,
				smvk::DepthSettings::Disabled(),
				smvk::StencilSettings::DefaultDisabled(),
				smvk::BlendSettings::Opaque(), smvk::MSAASettings::Count1(),
				raster);
			cmd.PushConstants(0, sizeof(MatBallPush), &push,
				smvk::ShaderStage::All);
			// Draw 1: the fullscreen default-sky background.
			cmd.BindVertexBuffer(skyVB, 0, 0);
			cmd.Draw(3, 1, 0, 0);
			// Draw 2: the material ball.
			cmd.BindVertexBuffer(sphereVB, 0, 0);
			cmd.BindIndexBuffer(sphereIB, 0);
			cmd.DrawIndexed(sphereIdx, 1, 0, 0, 0);
		});
}

void EditorApp::ExportMatBallPng() {
	// The post-submit half (WaitForDeviceIdle ran at the caller): the
	// readback + the sidecar write. The pass left the image in
	// COLOR_ATTACHMENT_OPTIMAL.
	if (m_matBallImage.empty() || m_matBallOutPath.empty()) {
		m_matBallQueuedMat = UINT32_MAX;
		return;
	}
	std::vector<uint8_t> px(size_t(256) * 256 * 4);
	if (!m_context->DownloadDataFromImage(m_matBallImage,
			smvk::ImageLayout::ColorAttachmentOptimal, px.data(),
			px.size())) {
		Log("Material ball: GPU readback failed.");
		m_matBallQueuedMat = UINT32_MAX;
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(
		std::filesystem::path(m_matBallOutPath).parent_path(), ec);
	if (!stbi_write_png(m_matBallOutPath.c_str(), 256, 256, 4, px.data(),
			256 * 4)) {
		Log("Material ball: PNG write failed ("
			+ m_matBallOutPath + ").");
	}
	else {
		Log("Material ball preview: " + m_matBallOutPath);
	}
	m_matBallQueuedMat = UINT32_MAX;
	m_matBallOutPath.clear();
}

bool EditorApp::QueueMaterialBallPreview(uint32_t matWorld,
	const std::string& pngPath) {
	// The queue head (consumed at the next RenderFrame — the render must
	// sit inside a frame's graph Execute; a bare out-of-frame render
	// would need its own submit plumbing for one thumbnail).
	if (matWorld == UINT32_MAX || pngPath.empty())
		return false;
	if (!EnsureMatBallResources())
		return false;
	m_matBallQueuedMat = matWorld;
	m_matBallOutPath = pngPath;
	return true;
}

} // namespace editor
