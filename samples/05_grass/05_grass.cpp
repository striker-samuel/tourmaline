// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// 05_grass: dense terrain-backed GPU instancing (no per-blade CPU objects).
// 256m x 256m; 16 x 64m tiles; 65,536 candidates/tile at density=16,
// i.e. 1,048,576 blades. Each blade is one two-triangle vertical quad.
// The production PlacementWorldRuntime does the actual GPU scatter,
// frustum/distance cull, visible-index compaction and indirect drawbuild.
// No CPU instance readbacks or per-instance draw calls.
//
// Environment: GRASS_DENSITY=1..16, GRASS_DISTANCE=32..512.
//
// This sample intentionally uses the renderer-agnostic runtime directly
// to give the GPU pipeline a small, reproducible on-screen stress test.
// The WorldRenderer integration is a separate opt-in consumer.

#include <Engine/core.h>
#include <Engine/Placement/placement_draw.h>
#include <Engine/Rendergraph/placement_pass.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/renderview.h>
#include <Engine/Rendergraph/terrain_pack.h>

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {
uint32_t EnvBounded(const char* key, uint32_t fallback,
    uint32_t lo, uint32_t hi) {
    const char* value = std::getenv(key);
    if (!value || !*value) return fallback;
    const long n = std::strtol(value, nullptr, 10);
    if (n < long(lo)) return lo;
    if (n > long(hi)) return hi;
    return uint32_t(n);
}

constexpr uint32_t kTerrainSize = 256;
constexpr uint32_t kTileSize = 64;
constexpr uint32_t kTilesPerAxis = kTerrainSize / kTileSize;
constexpr uint32_t kSlotCount = kTilesPerAxis * kTilesPerAxis;
constexpr uint32_t kDemoHeightRes = 64;
constexpr uint32_t kDemoMaskRes = 128;
constexpr uint32_t kSwapImagesMax = 8;
}

class GrassApp final : public smvk::App {
    using Super = smvk::App;
public:
    ~GrassApp() override {
        if (m_runtime && m_rendergraph)
            m_runtime->Destroy(*m_rendergraph);
        delete m_runtime;
        m_runtime = nullptr;
        m_draw.Destroy();
        if (m_rendergraph) {
            if (!m_viewsLogical.empty())
                m_rendergraph->FreeBuffer(m_viewsLogical);
            if (!m_depthLogical.empty())
                m_rendergraph->FreeImageView(m_depthLogical);
            for (uint32_t i = 0; i < m_swapCount; ++i)
                if (!m_swapViews[i].empty())
                    m_rendergraph->FreeImageView(m_swapViews[i]);
        }
        delete m_cluster;
    }

private:
    bool Initialize() override {
        if (!Super::Initialize()) return false;
        smvk::AllocateStaticVertexFmt(*m_context);
        m_density = EnvBounded("GRASS_DENSITY", 16, 1, 16);
        m_distance = float(EnvBounded("GRASS_DISTANCE", 190, 32, 512));
        if (!BuildTerrain() || !BuildBladeMesh()) return false;
        if (!m_views.Ensure(*m_context, m_swapchain->framesInFlight()))
            return false;
        m_viewsLogical = m_rendergraph->AddExternalBuffer(m_views.Buffer(),
            "grass render view slab");
        if (m_viewsLogical.empty() || !ImportSwapViews()) return false;
        m_runtime = new tour::placement::PlacementWorldRuntime();
        m_runtime->layer = &m_layer;
        m_runtime->hfield = &m_hfield;
        m_runtime->masks = &m_maskGrid;
        m_runtime->tileSize = float(kTileSize);
        m_runtime->haloMeters = 2.0f;
        m_runtime->slotBudget = kSlotCount;
        m_runtime->viewBudget = 1;
        if (!m_runtime->Create(*m_context, *m_rendergraph)) return false;
        if (!m_draw.Ensure(*m_context)) return false;
        if (!m_runtime->SeedIndirect(*m_context, 0, &m_seed, 1)) return false;
        const uint32_t count = kSlotCount * m_layer.config.candidatesPerTile;
        std::fprintf(stderr,
            "[grass] terrain %um x %um, density=%u/m2, "
            "candidates=%u, gpu tiles=%u, max distance=%.0fm\n",
            kTerrainSize, kTerrainSize, m_density, count,
            kSlotCount, m_distance);
        return true;
    }

    bool BuildTerrain() {
        using namespace smvk::terrain;
        m_hfield.meta.worldSize = float(kTerrainSize);
        m_hfield.meta.originX = -float(kTerrainSize) * 0.5f;
        m_hfield.meta.originZ = -float(kTerrainSize) * 0.5f;
        m_hfield.meta.hTileSize = float(kTileSize);
        m_hfield.meta.hRes = kDemoHeightRes;
        m_hfield.meta.gBlock = float(kTerrainSize) / 2.0f;
        m_hfield.meta.baseY = kTerrainBaseY;
        m_hfield.meta.range = kTerrainRange;
        m_hfield.tiles.resize(kSlotCount);
        for (uint32_t tz = 0; tz < kTilesPerAxis; ++tz) {
            for (uint32_t tx = 0; tx < kTilesPerAxis; ++tx) {
                auto& tile = m_hfield.tiles[tz * kTilesPerAxis + tx];
                tile.resize(kDemoHeightRes * kDemoHeightRes * 4);
                for (uint32_t z = 0; z < kDemoHeightRes; ++z)
                    for (uint32_t x = 0; x < kDemoHeightRes; ++x) {
                        const float wx = float(tx * kDemoHeightRes + x) - 127.5f;
                        const float wz = float(tz * kDemoHeightRes + z) - 127.5f;
                        const float y = 2.5f * std::sin(wx * 0.035f)
                            * std::cos(wz * 0.04f)
                            + 0.6f * std::sin((wx + wz) * 0.13f);
                        const float h01 = (y - kTerrainBaseY) / kTerrainRange;
                        uint8_t r = 0, g = 0;
                        EncodeRG16(h01, r, g);
                        const size_t o = (size_t(z) * kDemoHeightRes + x) * 4;
                        tile[o] = r; tile[o + 1] = g;
                        tile[o + 2] = 0; tile[o + 3] = 255;
                    }
            }
        }
        // 2 x 2 S-tiles. Fully painted channel 0 tests real ecotope
        // sampling + the seam-correct halo; the other channels stay 0.
        m_maskTiles.resize(4);
        for (uint32_t tile = 0; tile < 4; ++tile) {
            auto& px = m_maskTiles[tile];
            px.resize(kDemoMaskRes * kDemoMaskRes * 4);
            for (size_t i = 0; i < px.size(); i += 4) {
                px[i] = 255;
                px[i + 1] = 0;
                px[i + 2] = 0;
                px[i + 3] = 255;
            }
        }
        m_maskGrid = tour::placement::WorldMaskGrid{
            &m_maskTiles, 2u, kDemoMaskRes };
        if (!m_hfield.valid() || !m_maskGrid.Valid()) return false;

        m_layer.programId = 0x4752415353ull; // "GRASS"
        m_layer.program.graphSeed = 0x127ab98ull;
        m_layer.program.nodeId = 1;
        m_layer.program.instrs = {
            tour::placement::FieldInstr{
                .op = tour::placement::FieldOp::EcotopeMask,
                .params = { 0.0f, 0.0f, 0.0f, 0.0f }
            }
        };
        m_layer.program.acceptIndex = 0;
        m_layer.domainKind = tour::placement::DomainKind::Heightfield;
        m_layer.config.candidatesPerTile = kTileSize * kTileSize * m_density;
        m_layer.config.densityScale = 1.0f;
        m_layer.config.minScale = 0.70f;
        m_layer.config.maxScale = 1.3f;
        m_layer.config.minSpacing = 0.0f; // dense: no grass-to-grass exclusion
        tour::placement::Prototype blade{};
        blade.weight = 1.0f;
        blade.boundsMin = glm::vec3(-0.08f, 0.0f, -0.03f);
        blade.boundsMax = glm::vec3(0.08f, 0.95f, 0.03f);
        blade.maxDrawDistance = m_distance;
        m_layer.prototypes.add(blade);
        return true;
    }

    bool BuildBladeMesh() {
        m_cluster = new smvk::Cluster(m_context.get());
        smvk::StaticMeshUpload up{};
        up.vertices.resize(4);
        up.vertices[0].pos = glm::vec3(-0.085f, 0.0f, 0.0f);
        up.vertices[1].pos = glm::vec3( 0.085f, 0.0f, 0.0f);
        up.vertices[2].pos = glm::vec3(-0.02f, 0.95f, 0.0f);
        up.vertices[3].pos = glm::vec3( 0.02f, 0.95f, 0.0f);
        for (auto& v : up.vertices) {
            v.color = glm::vec4(0.24f, 0.70f, 0.18f, 1.0f);
            v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        }
        up.indices = { 0, 1, 2, 2, 1, 3 };
        up.aabbMin = glm::vec3(-0.085f, 0.0f, -0.01f);
        up.aabbMax = glm::vec3(0.085f, 0.95f, 0.01f);
        const auto handle = m_cluster->UploadStaticMesh(up);
        if (handle.empty()) return false;
        const smvk::StaticMesh* mesh = m_cluster->staticMeshes.get(handle);
        if (!mesh || mesh->geometry.surfaces.empty()) return false;
        const smvk::Surface& surface = mesh->geometry.surfaces[0];
        m_seed.indexCount = uint32_t(up.indices.size());
        m_seed.firstIndex = surface.firstIndex;
        m_seed.vertexOffset = surface.baseVertex;
        m_context->FlushUploads();
        m_context->WaitForUploads();
        return true;
    }

    bool ImportSwapViews() {
        for (uint32_t i = 0; i < m_swapCount; ++i)
            if (!m_swapViews[i].empty())
                m_rendergraph->FreeImageView(m_swapViews[i]);
        m_swapCount = m_swapchain->GetImageCount();
        if (m_swapCount > kSwapImagesMax) return false;
        for (uint32_t i = 0; i < m_swapCount; ++i)
            m_swapViews[i] = m_rendergraph->AddExternalImageView(
                m_swapchain->GetImageViewHandle(i),
                m_swapchain->GetSurfaceFormat(), "grass swap");
        return EnsureDepth(m_swapchain->GetExtent());
    }

    bool EnsureDepth(smvk::Extent2D extent) {
        if (m_depthExtent.width == extent.width &&
            m_depthExtent.height == extent.height && !m_depthLogical.empty())
            return true;
        if (!m_depthLogical.empty()) m_rendergraph->FreeImageView(m_depthLogical);
        m_depthLogical = {};
        m_depthView = {};
        m_depthImage = {};
        smvk::ImageCreateInfo ci{};
        ci.extent = { extent.width, extent.height, 1u };
        ci.format = smvk::Format::D32_Float;
        ci.usage = smvk::ImageUsage::DepthStencilAttachment;
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.name = "grass depth";
        m_depthImage = m_context->CreateImage(ci, nullptr);
        if (m_depthImage.empty()) return false;
        const smvk::ImageViewCreateInfo vci{
            .viewType = smvk::ImageViewType::View2D,
            .format = smvk::Format::D32_Float,
            .subresourceRange = { smvk::ImageAspect::Depth, 0, 1, 0, 1 },
            .name = "grass depth view",
        };
        m_depthView = m_context->CreateImageView(m_depthImage, vci);
        if (m_depthView.empty()) return false;
        m_depthLogical = m_rendergraph->AddExternalImageView(
            m_depthView, smvk::Format::D32_Float, "grass depth view");
        m_depthExtent = extent;
        return !m_depthLogical.empty();
    }

    void DrawFrame() override {
        if (m_swapchain->FramebufferResized()) {
            m_swapchain->Recreate();
            m_swapchain->ClearFramebufferResized();
            if (!ImportSwapViews()) return;
        }
        smvk::Framebuffer fb = m_swapchain->AcquireFramebuffer();
        const smvk::Extent2D extent = m_swapchain->GetExtent();
        if (!extent.height) return;
        m_rendergraph->NewFrame();
        m_frame += 1;
        const float angle = 0.42f + 0.002f * float(m_frame);
        const glm::vec3 eye(
            105.0f * std::sin(angle), 31.0f,
            105.0f * std::cos(angle));
        const glm::vec3 at(0.0f, 0.0f, 0.0f);
        glm::mat4 projection = glm::perspective(glm::radians(66.0f),
            float(extent.width) / float(extent.height),
            0.1f, 420.0f);
        projection[1][1] *= -1.0f;
        const glm::mat4 vp = projection *
            glm::lookAt(eye, at, glm::vec3(0.0f, 1.0f, 0.0f));
        m_views.NewFrame();
        const uint32_t mainView = m_views.AllocateMainView(vp,
            glm::vec2(float(extent.width), float(extent.height)));
        m_views.Upload(*m_context, fb.frameIndex);

        const glm::vec4 interests[] = {
            glm::vec4(-128.0f, -128.0f, 128.0f, 128.0f) };
        m_runtime->BeginFrame(*m_context, *m_rendergraph,
            interests, 1, m_deps);
        m_runtime->RecordCullChain(*m_context, *m_rendergraph,
            0, m_viewsLogical,
            m_views.DeviceAddress(*m_context, fb.frameIndex),
            mainView, eye,
            tour::placement::kPlacementCullFrustum |
            tour::placement::kPlacementCullDistance);

        auto& pass = m_rendergraph->AddPass("MillionGrassBlades");
        pass.SetOffset(0, 0);
        pass.SetExtent(extent);
        pass.SetColor({ m_swapViews[fb.imageIndex] },
            smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
            smvk::ClearColor(0.13f, 0.22f, 0.32f, 1.0f));
        pass.SetDepth(m_depthLogical,
            smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
            smvk::ClearDepthStencil{1.0f, 0});
        smvk::BufferSyncRequest vertexRead{};
        vertexRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
        vertexRead.barrierMasks.stageMask = smvk::PipelineStageBit_Vertex;
        smvk::BufferSyncRequest indirectRead{};
        indirectRead.barrierMasks.accessMask =
            smvk::AccessFlags::IndirectCommandRead;
        indirectRead.barrierMasks.stageMask =
            smvk::PipelineStageBit_DrawIndirect;
        for (uint32_t i = 0; i < kSlotCount; ++i) {
            const auto* vis = m_runtime->VisSet(int32_t(i));
            if (!vis) continue;
            pass.AddBufferHint(m_runtime->pass.instanceLogicals[i], vertexRead);
            pass.AddBufferHint(vis->visibleL, vertexRead);
            pass.AddBufferHint(vis->indirectL, indirectRead);
        }
        pass.SetRecordingFunc([this, mainView, frameIndex = fb.frameIndex](
                smvk::CommandBuffer& cmd) {
            cmd.BindStaticVertexFmt();
            cmd.BindRenderPipeline(m_draw.program,
                smvk::VertexLayoutId::Static,
                smvk::DepthSettings{
                    .depthFunc = smvk::CompareOp::LessEqual,
                    .writeEnable = true, .depthTestEnable = true },
                smvk::StencilSettings{}, smvk::BlendSettings::Opaque(),
                smvk::MSAASettings::Count1(),
                smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
            for (uint32_t i = 0; i < kSlotCount; ++i) {
                const auto* vis = m_runtime->VisSet(int32_t(i));
                if (!vis) continue;
                const tour::placement::PlacementVsPush push{
                    vis->VisibleAddress(*m_context),
                    m_runtime->pass.TileInstancesAddress(*m_context, i),
                    m_views.DeviceAddress(*m_context, frameIndex),
                    mainView, 0u
                };
                cmd.PushConstants(tour::placement::kPlacementVsPushOffset,
                    sizeof(push), &push, smvk::ShaderStage::All);
                cmd.DrawIndexedIndirect(vis->IndirectBuffer(), 0, 1,
                    tour::placement::kPlacementIndirectStride);
            }
        });
        m_rendergraph->Execute(fb);
        m_swapchain->EndAndPresentFramebuffer(fb);
        m_context->ProcessDeferrredTasks();
    }

    smvk::terrain::TerrainHField m_hfield{};
    decltype(m_hfield.tiles) m_maskTiles{};
    tour::placement::WorldMaskGrid m_maskGrid{};
    tour::placement::PlacementSystem::LayerBinding m_layer{};
    tour::placement::DependencyVersions m_deps{};
    tour::placement::PlacementWorldRuntime* m_runtime = nullptr;
    tour::placement::PlacementDrawPrograms m_draw{};
    tour::placement::PlacementIndirectCmd m_seed{};
    smvk::RenderViewSet m_views{};
    smvk::LogicalBufferHandle m_viewsLogical{};
    smvk::Cluster* m_cluster = nullptr;
    smvk::Holder<smvk::ImageHandle> m_depthImage{};
    smvk::Holder<smvk::ImageViewHandle> m_depthView{};
    smvk::LogicalImageViewHandle m_depthLogical{};
    smvk::Extent2D m_depthExtent{};
    smvk::LogicalImageViewHandle m_swapViews[kSwapImagesMax]{};
    uint32_t m_swapCount = 0;
    uint32_t m_density = 16;
    float m_distance = 190.0f;
    uint64_t m_frame = 0;
};

int main() {
    GrassApp app;
    app.Run();
    return EXIT_SUCCESS;
}
