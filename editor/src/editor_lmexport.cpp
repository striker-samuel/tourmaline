// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Lightmap flat-view PNG export: renders the WHOLE scene unwrapped by
// its atlas uv2 over the baked lightmap content into an offscreen
// target (atlas resolution), with an optional UV wireframe overlay,
// then reads the pixels back and writes a PNG (exports/ folder). The
// offscreen + readback path mirrors the App screenshot (Capture pass +
// ResourceManager::DownloadDataFromImage).

#include "editor_app.h"

#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/RendererLimits.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// stb PNG writer impl lives in editor_branding.cpp; header-only here.
#include <stb_image_write.h>

namespace editor {
namespace {
// Mirrors the main-pass ring strides (editor_render.cpp): the
// renderer-owned budgets from Engine/Renderer/RendererLimits.h.
constexpr uint32_t kMaxLights =
	engine::renderer::limits::MaxSimultaneousLights;
constexpr uint32_t kMaxMaterials =
	engine::renderer::limits::MaxMaterials;
} // namespace

void EditorApp::EnsureLightmapExportTarget(smvk::Extent2D extent) {
	if (!m_lmExportImage.empty() &&
		m_lmExportExtent.width == extent.width &&
		m_lmExportExtent.height == extent.height)
		return;
	if (!m_lmExportLogical.empty()) {
		m_rendergraph->FreeImageView(m_lmExportLogical);
		m_lmExportLogical = {};
	}
	if (!m_lmExportDepthLogical.empty()) {
		m_rendergraph->FreeImageView(m_lmExportDepthLogical);
		m_lmExportDepthLogical = {};
	}
	m_lmExportView = {};
	m_lmExportImage = {};
	m_lmExportDepthView = {};
	m_lmExportDepth = {};
	// Color: sRGB storage like the swapchain (bytes land PNG-ready).
	smvk::ImageCreateInfo ci{};
	ci.extent = { extent.width, extent.height, 1 };
	ci.format = smvk::Format::RGBA8_SRGB;
	ci.usage = smvk::ImageUsage::ColorAttachment |
		smvk::ImageUsage::TransferSrc;
	ci.mipLevels = 1;
	ci.arrayLayers = 1;
	ci.name = "lmExport";
	m_lmExportImage = m_context->CreateImage(ci, nullptr);
	if (m_lmExportImage.empty())
		return;
	smvk::ImageViewCreateInfo vci{};
	vci.viewType = smvk::ImageViewType::View2D;
	vci.format = smvk::Format::Undefined; // inherit
	vci.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 };
	vci.name = "lmExportView";
	m_lmExportView = m_context->CreateImageView(m_lmExportImage, vci);
	if (m_lmExportView.empty()) {
		m_lmExportImage = {};
		return;
	}
	m_lmExportLogical = m_rendergraph->AddExternalImageView(
		m_lmExportView, smvk::Format::RGBA8_SRGB, "lmExportLogical");
	// Depth: reversed-Z (the flat FS writes z=0.9999; wires at z=1 test
	// GreaterEqual over the fill).
	smvk::ImageCreateInfo dzi{};
	dzi.extent = { extent.width, extent.height, 1 };
	dzi.format = smvk::Format::D32_Float;
	dzi.usage = smvk::ImageUsage::DepthStencilAttachment;
	dzi.mipLevels = 1;
	dzi.arrayLayers = 1;
	dzi.name = "lmExportDepth";
	m_lmExportDepth = m_context->CreateImage(dzi, nullptr);
	if (!m_lmExportDepth.empty()) {
		smvk::ImageViewCreateInfo dvci{};
		dvci.viewType = smvk::ImageViewType::View2D;
		dvci.format = smvk::Format::Undefined;
		dvci.subresourceRange = { smvk::ImageAspect::Depth, 0, 1, 0, 1 };
		dvci.name = "lmExportDepthView";
		m_lmExportDepthView = m_context->CreateImageView(
			m_lmExportDepth, dvci);
		if (!m_lmExportDepthView.empty())
			m_lmExportDepthLogical = m_rendergraph->AddExternalImageView(
				m_lmExportDepthView, smvk::Format::D32_Float,
				"lmExportDepthLogical");
	}
	m_lmExportExtent = extent;
}

void EditorApp::RecordLightmapExportPass(uint32_t flight) {
	// The export draws through the flat lightmap program — baked content,
	// or the UV-coordinate QA program (same VS, uv-as-color FS). The
	// content FS samples frame.lmLayers[0], so any viewport frame slice
	// works (they all carry the atlas layers). Wireframe rides the line
	// program through an identity viewProj (wires are in NDC space).
	if (m_lmExportLogical.empty() || !m_lmGpu || !m_world)
		return;
	smvk::ShaderProgram& prog = m_lmExportUv
		? FlatUvCoordProgram() : FlatUvProgram(ViewMode::FlatUv1);
	if (prog.shaderCount != 2)
		return;
	const smvk::Extent2D extent = m_lmExportExtent;

	const uint64_t frameAddr = m_viewports[m_activeVp].frameAddr;
	const uint64_t lightsAddr =
		m_context->GetBufferDeviceAddress(m_lightsBuf) +
		flight * sizeof(smvk::Light) * kMaxLights;
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
	pc.pad_pc = 0;
	pc.renderViews = 0;
	pc.mainRenderView = UINT32_MAX;
	pc.renderViewIndex = UINT32_MAX;

	// UV wires for the WHOLE scene (export = full atlas, not the panel's
	// selection-focused view). Built + uploaded before the pass records.
	std::vector<smvk::DebugLineVertex> wires;
	if (m_lmExportWires && m_lineProgram.shaderCount == 2)
		BuildFlatUvWires(wires, ViewMode::FlatUv1); // UINT32_MAX = all
	const bool drawWires = m_lmExportWires && !wires.empty() &&
		m_lineProgram.shaderCount == 2;
	if (drawWires) {
		const size_t bytes = wires.size() * sizeof(wires[0]);
		if (m_lmExportLines.empty() ||
			m_lmExportLinesCap < uint32_t(wires.size())) {
			m_lmExportLines = m_context->CreateBuffer(bytes,
				smvk::BufferType_Vertex, "editor lm export wires");
			m_lmExportLinesCap = uint32_t(wires.size());
		}
		// Draw count is the CURRENT export's vertex count, not the
		// high-water cap: a smaller later export must not replay the
		// previous buffer's stale tail.
		m_lmExportLinesCount = uint32_t(wires.size());
		if (!m_lmExportLines.empty())
			m_context->UploadDataToBuffer(m_lmExportLines, wires.data(),
				bytes, smvk::PipelineStageBit_Vertex,
				smvk::AccessFlags::VertexAttributeRead);
	}
	else {
		m_lmExportLinesCount = 0;
	}

	auto& pass = m_rendergraph->AddPass("LmExport")
		.SetOffset(0, 0)
		.SetExtent(extent)
		.SetColor({ m_lmExportLogical },
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearColor(0.02f, 0.02f, 0.03f, 1.0f))
		.SetDepth(m_lmExportDepthLogical,
			smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
			smvk::ClearDepthStencil(0.0f, 0)); // reversed-Z
	pass.SetRecordingFunc([this, pc, drawWires](smvk::CommandBuffer& cmd) {
		// Fill: every BAKED mesh unwrapped by its atlas uv2 over the
		// lightmap content (no selection filter — the export is the full
		// atlas artifact), or the same layout with the atlas uv itself
		// as content (UV-coordinate QA mode).
		smvk::ShaderProgram& prog = m_lmExportUv
			? FlatUvCoordProgram() : FlatUvProgram(ViewMode::FlatUv1);
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
		m_world->forEachRenderItem(req,
			[&](const smvk::World::RenderItem& item) {
				if (item.material == smvk::World::kInvalidIndex)
					return;
				const smvk::StaticMesh* mesh = m_world->GetMesh(item.mesh);
				if (!mesh || mesh->lmChunk == 0xFFFFFFFFu)
					return; // baked meshes only
				uint32_t matId = item.material;
				cmd.PushConstants(offsetof(engine::IRenderer::PushConstants, materialId),
					sizeof(matId), &matId, smvk::ShaderStage::All);
				// Per-mesh chart tail (same unpack as the flat panel).
				smvk::gfx::UvFlatVsPushTail tail{};
				tail.hasXforms = 1;
				tail.chartOffset = m_world->ChartOffsetFor(item.mesh, 0);
				tail.lmXforms = EnsureChartXforms();
				cmd.PushConstants(smvk::gfx::kUvFlatVsPushOffset,
					sizeof(tail), &tail, smvk::ShaderStage::All);
				for (const auto& s : mesh->geometry.surfaces)
					cmd.DrawIndexed(s.indexCount, 1, s.firstIndex,
						s.baseVertex);
			});
		// Wireframe overlay: shell edges in NDC space through the
		// identity projection, depth-tested over the fill.
		if (!drawWires || m_lmExportLines.empty()
			|| m_lmExportLinesCount == 0)
			return;
		cmd.BindRenderPipeline(m_lineProgram,
			smvk::VertexFormat::DebugLine,
			smvk::DepthSettings{ .depthFunc = smvk::CompareOp::GreaterEqual,
				.writeEnable = false, .depthTestEnable = true },
			smvk::StencilSettings{}, smvk::BlendSettings{},
			smvk::MSAASettings{},
			smvk::RasterSettings{ .cullMode = smvk::CullMode::None },
			smvk::PrimitiveTopology::LineList);
		cmd.BindVertexBuffer(m_lmExportLines, 0, 0);
		engine::LineDebugPush lp{};
		const glm::mat4 identity(1.0f);
		std::memcpy(lp.viewProj, glm::value_ptr(identity),
			sizeof(lp.viewProj));
		lp.color[0] = 0.25f;
		lp.color[1] = 0.9f;
		lp.color[2] = 0.6f;
		lp.color[3] = 1.0f;
		cmd.PushConstants(0, sizeof(lp), &lp, smvk::ShaderStage::All);
		cmd.Draw(m_lmExportLinesCount, 1, 0, 0);
	});
}

void EditorApp::ExportLightmapPng() {
	if (m_lmExportImage.empty() || !m_hasSglm) {
		Log("Lightmap export: no capture target / atlas.");
		return;
	}
	// The export pass leaves the image in COLOR_ATTACHMENT_OPTIMAL.
	std::vector<uint8_t> px(size_t(m_lmExportExtent.width) *
		size_t(m_lmExportExtent.height) * 4);
	if (!m_context->DownloadDataFromImage(m_lmExportImage,
			smvk::ImageLayout::ColorAttachmentOptimal, px.data(),
			px.size())) {
		Log("Lightmap export: GPU readback failed.");
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories("exports", ec);
	std::string stem = "lightmap";
	if (!m_sglmPath.empty())
		stem = std::filesystem::path(m_sglmPath).stem().string();
	const std::string path = "exports/" + stem
		+ (m_lmExportUv ? "_uv" : "")
		+ (m_lmExportWires ? "_wire" : "") + ".png";
	// stb_image_write is compiled in editor_branding.cpp.
	if (!stbi_write_png(path.c_str(), int(m_lmExportExtent.width),
			int(m_lmExportExtent.height), 4, px.data(),
			int(m_lmExportExtent.width) * 4)) {
		Log("Lightmap export: PNG write failed (" + path + ").");
		return;
	}
	Log("Lightmap exported: " + path + " (" +
		std::to_string(m_lmExportExtent.width) + "x" +
		std::to_string(m_lmExportExtent.height) +
		(m_lmExportUv ? ", uv coords" : ", baked content") +
		(m_lmExportWires ? ", wireframe" : ", no wireframe") + ")");
}

} // namespace editor
