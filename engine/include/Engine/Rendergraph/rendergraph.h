// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

#include <Engine/Memory/memory.h>
#include <Engine/Memory/arena.h>
#include <Engine/Memory/pool.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/Renderer/vertex_layouts.h>
#include <Engine/smvk.h>
// The per-builder hot-reload swap helper (SwapShaderModule): every
// rendergraph builder includes this header, so the consumer leg's
// ReloadShaders implementations share one exact module-swap recipe.
#include <Engine/Rendergraph/ShaderSwap.h>
#include <Engine/Rendergraph/static_graph_compiler.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smvk {

struct RenderGraph;
struct RenderPass;
struct FrameContext; // defined in Rendergraph/renderer.h (per-frame input)

struct CommandBuffer final {
    CommandBuffer(void* cmdOpaque,
                  RenderGraph& graph,
                  RenderPass& pass,
                  void* stageTimerOpaque = nullptr,
                  void* primitiveCountOpaque = nullptr);
    void BindRenderPipeline(
        ShaderProgram& shaderProgram,
        const VertexLayoutDesc& vertexFormat = {},
        const DepthSettings& depthSettings = DepthSettings::DepthTest(),
        const StencilSettings& stencilSettings = StencilSettings::DefaultDisabled(),
        const BlendSettings& blendSettings = BlendSettings::Opaque(),
        const MSAASettings& msaaSettings = MSAASettings::Count1(),
        const RasterSettings& rasterSettings = {},
        PrimitiveTopology topology = PrimitiveTopology::TriangleList);

    void PushConstants(uint32_t offset,
                       uint32_t size,
                       const void* data,
                       ShaderStage stages = ShaderStage::AllGraphics);

    void Draw(uint32_t vertexCount,
              uint32_t instanceCount = 1,
              uint32_t firstVertex = 0,
              uint32_t firstInstance = 0);

    void DrawIndexed(uint32_t indexCount,
                     uint32_t instanceCount = 1,
                     uint32_t firstIndex = 0,
                     int32_t vertexOffset = 0,
                     uint32_t firstInstance = 0);

    // Binds an arbitrary RHI buffer as the index buffer (UINT32).
    // All index streams (mesh, cluster-ordered, filtered) live in the
    // static index monoblock with absolute firstIndex, so in practice
    // the offset-0 monoblock binding from BindStaticVertexFmt is the
    // only one ever needed (see BindStaticVertexFmt).
    void BindIndexBuffer(BufferHandle buffer, uint64_t offset = 0);

    // Indirect indexed draws consuming VkDrawIndexedIndirectCommand
    // entries produced by the filter compute (see Engine/Renderer/gfx_types.h).
    // The indirect buffer must have been created with BufferType_Indirect.
    void DrawIndexedIndirect(BufferHandle buffer,
                             uint64_t offset,
                             uint32_t drawCount,
                             uint32_t stride);

    // Indirect-COUNT indexed draws (Vulkan 1.2 core, drawIndirectCount):
    // drawCount is read from countBuffer[countOffset] BY THE DEVICE — the
    // GPU-produced survivor count (the compacted meshlet PSO bins' exact
    // classCounts), never a CPU readback. maxCount clamps a corrupt count
    // to the reserved range. Same VkDrawIndexedIndirectCommand stride
    // contract as DrawIndexedIndirect; both buffers carry INDIRECT usage
    // (Storage buffers get it by the resource manager's convention).
    void DrawIndexedIndirectCount(BufferHandle buffer,
                                  uint64_t offset,
                                  BufferHandle countBuffer,
                                  uint64_t countOffset,
                                  uint32_t maxCount,
                                  uint32_t stride);

    // Mesh-task draw (VK_EXT_mesh_shader): launches groupCountX/Y/Z
    // task-shader workgroups (or mesh workgroups directly when the
    // pipeline has no task stage). Requires SupportsMeshShaders() —
    // the mesh-shader gfx filter path gates on it and keeps the
    // compute+indexed CPU fallback otherwise.
    void DrawMeshTasks(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
    // VK_EXT_mesh_shader: dispatch task/mesh workgroups from GPU-generated
    // VkDrawMeshTasksIndirectCommandEXT (3 uint32 words, 12-byte stride).
    // Caller must declare compute-write -> draw-indirect-read dependencies.
    void DrawMeshTasksIndirect(BufferHandle buffer,
                               uint64_t offset,
                               uint32_t drawCount,
                               uint32_t stride);

    void BindComputePipeline(ShaderProgram& shaderProgram);

    // GPU stage timer write (see GpuStageTimer in smvk.h): records a
    // timestamp query into this pass's command buffer at the flight's
    // frame-timer pool. begin=true fires after prior commands (the
    // stage's start), begin=false after the stage's work (its end) —
    // recording funcs call this around a STAGE (a group of passes,
    // e.g. begin at the first cascade's lambda start, end at the last
    // spot tile's lambda end). No-op when the device/frame lacks
    // timestamp support (the impl context is null).
    void WriteStageTimestamp(uint32_t stageTimer, bool begin);

    // Primitive-statistics query (drawn triangle count) around a mesh/
    // task dispatch. Begin fires the reset + query start before the
    // draw calls, End fires the query stop after them; the swapchain
    // reads back the total primitives (triangles) generated between the
    // two (Swapchain::GetPrimitiveCount). No-op when the device/frame
    // lacks the query support (the impl context is null).
    void BeginPrimitiveCount(uint32_t slot);
    void EndPrimitiveCount(uint32_t slot);

    // Regular (vertex-shader indexed path) primitives-generated query —
    // the same contract as Begin/EndPrimitiveCount but against the
    // VK_QUERY_TYPE_PRIMITIVES_GENERATED_EXT pool (Swapchain::
    // GetPrimitiveCountRegular).
    void BeginPrimitiveCountRegular(uint32_t slot);
    void EndPrimitiveCountRegular(uint32_t slot);

    // Sets the stencil reference for both faces (dynamic state —
    // pipelines are built with VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    // so the reference never enters the pipeline cache key and no
    // per-reference pipeline variants are ever created). Recorded
    // into the command stream like PushConstants: valid until the
    // next call. Passes that never call it keep the initial 0.
    void SetStencilReference(uint32_t frontAndBackReference);

    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);

    // Indirect compute dispatch consuming a VkDispatchIndirectCommand
    // (3 uints: groupCountX/Y/Z) produced by an earlier compute pass
    // (the SSR classify's indirect-args builder). The indirect buffer
    // must have been created with BufferType_Indirect; the consuming
    // pass declares it with AccessFlags::IndirectCommandRead +
    // PipelineStageBit_DrawIndirect so the graph barriers the
    // producing writes before this read.
    void DispatchIndirect(BufferHandle buffer, uint64_t offset);

    // Global compute -> graphics memory dependency: every write from
    // any prior stage becomes visible to every subsequent read/write.
    // The escape hatch for resources the graph's per-resource sync
    // machinery cannot see (BDA-only heaps - raw device addresses
    // with no ImageHandle/BufferHandle to hint on; the GPU skinning
    // deformed heap is the engine's case). Backend mapping: Vulkan
    // emits a vkCmdPipelineBarrier2 all-commands barrier; D3D12 emits
    // a null-resource UAV barrier (all prior UAV work completes
    // before any later UAV work). No-op inside a graphics-only
    // sequence is guaranteed safe (over-synchronization only).
    void EmitGlobalMemoryBarrier();

    // Intra-pass compute producer -> compute consumer (including BDA).
    // When consumesIndirect is true, also publish generated dispatch args
    // to DRAW_INDIRECT. External graphics/transfer consumers still require
    // their own graph edges; this is not a replacement for those barriers.
    void EmitComputeMemoryBarrier(bool consumesIndirect = false);

    void BindStaticVertexFmt();

    // Binds an arbitrary RHI buffer as vertex binding N (pos-only debug
    // streams use binding 0 with ::tour::vertex_layouts::DebugLine).
    void BindVertexBuffer(BufferHandle buffer, uint32_t binding = 0, uint64_t offset = 0);

    // Access native VkCommandBuffer for integrations like ImGui.
    void* GetNativeHandle() const {
        return cmdOpaque_;
    }

  private:
    void* cmdOpaque_ = nullptr;
    void* currentPipeline_ = nullptr;
    void* currentLayout_ = nullptr;
    // GPU stage-timer context (impl-side StageTimerCtx: pool +
    // support flag), or null when the frame lacks timestamps —
    // WriteStageTimestamp no-ops then.
    void* stageTimerOpaque_ = nullptr;
    // Primitive-statistics context (impl-side PrimitiveCountCtx: pool +
    // support flag), or null when the frame lacks the query — the
    // Begin/EndPrimitiveCount methods no-op then.
    void* primitiveCountOpaque_ = nullptr;
#ifndef NDEBUG
    // Debug-only: last index buffer bound on this command buffer.
    // DrawIndexed* wrappers assert it is set, catching leaked/stale
    // index bindings across passes (the exploded-vertices class).
    BufferHandle boundIndexBuffer_{};
#endif
    RenderGraph* graph_ = nullptr;
    RenderPass* renderPass_ = nullptr;

    // ---- Graphics-bind fast path -------------------------------
    // BindRenderPipeline used to pay the full pipeline-cache lookup
    // (format copy + several vector allocs + a ~100-way hash combine)
    // even when the requested pipeline was already bound. Recorders
    // rebind per phase and per material, so the redundant case is
    // common. A CommandBuffer is per-pass (RenderGraph::Execute) and
    // attachment formats are pass-invariants, so remembering the raw
    // input state of the last graphics bind in this pass is enough to
    // skip the lookup and the vkCmdBind* re-issue entirely.
    bool graphicsBound_ = false;
    ShaderProgram boundProgram_{};
    VertexLayoutDesc boundVertexFormat_{};
    DepthSettings boundDepth_{};
    StencilSettings boundStencil_{};
    BlendSettings boundBlend_{};
    MSAASettings boundMsaa_{};
    RasterSettings boundRaster_{};
    PrimitiveTopology boundTopology_ = PrimitiveTopology::TriangleList;
    // Last graphics bundle: a redundant bind after a compute bind
    // (which overwrites currentPipeline_) restores the cached
    // pointers without re-recording vkCmdBindPipeline — the graphics
    // bind point still holds the right pipeline.
    void* graphicsPipeline_ = nullptr;
    void* graphicsLayout_ = nullptr;
};

using LogicalBufferHandle = Handle<struct LogicalBuffer>;

using LogicalImageHandle = Handle<struct LogicalImage>;

using LogicalImageViewHandle = Handle<struct LogicalImageView>;

// ============================================================
// Logical resource descriptions
// ============================================================

using BufferKey = BufferCreateInfo;
using ImageKey = ImageCreateInfo;
struct ImageViewKey {
    // cant reuse ImageCreateInfo beacuse it lacks handle property. Otherwise should be the same as
    // this
    LogicalImageHandle image{};
    ImageViewType viewType = ImageViewType::View2D;
    Format format = Format::Undefined;
    ImageSubresourceRange subresourceRange{};
    bool operator==(const ImageViewKey&) const = default;
};

// ============================================================
// Logical resources
// ============================================================

enum class ResourceType : uint8_t { Transient, External };

struct LogicalBuffer {
    ResourceType type = ResourceType::Transient;
    BufferKey key{};
    BufferHandle resolvedBufferHandle{};
    const char* name = "";
};

struct LogicalImage {
    ResourceType type = ResourceType::Transient;
    ImageKey key{};
    ImageHandle resolvedImageHandle{};
    const char* name = "";
};

struct LogicalImageView {
    enum class Type : uint8_t { Logical, ExternalPhysical };
    Type type = Type::Logical;
    ImageViewKey key{};
    ImageViewHandle resolvedViewHandle{};
    ImageViewHandle externalViewHandle{};
    Format externalFormat = Format::Undefined;
    const char* name = "";
};

// ============================================================
// Render attachments
// ============================================================

struct RenderAttachment {
    LogicalImageViewHandle view{};
    AttachmentLoadOp loadOp = AttachmentLoadOp::DontCare;
    AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
    ClearColor clearColor{};
    // Optional MSAA resolve destination (single-sampled). When set,
    // Execute() fills VkRenderingAttachmentInfo::resolveImageView with
    // resolveMode AVERAGE. view is then the multisampled image whose
    // storeOp should typically be DontCare.
    LogicalImageViewHandle resolveView{};
};

struct DepthAttachment {
    LogicalImageViewHandle view{};
    AttachmentLoadOp loadOp = AttachmentLoadOp::DontCare;
    AttachmentStoreOp storeOp = AttachmentStoreOp::Store;
    ClearDepthStencil clear{};
};

// ============================================================
// Sync hints for bindless / explicit resources
// ============================================================
struct ImageHint {
    LogicalImageViewHandle view{};
    ImageSyncRequest request{};
};
struct BufferHint {
    LogicalBufferHandle buffer{};
    BufferSyncRequest request{};
};

// ============================================================
// Render pass
// ============================================================

// ------------------------------------------------------------
// Stable record-function ABI (static compiled render graph)
// ------------------------------------------------------------
// The compiled graph is retained across frames, so a pass's record function
// must be a NON-CAPTURING function pointer: it reads every frame-dependent
// value through FrameContext (never through a captured frame-local). The
// immutable per-pass configuration lives in a stable, caller-owned block the
// record function downcasts through PassData::config.
//
// FrameContext (Rendergraph/renderer.h) is the per-frame input: the resource
// manager, graph, world, views, camera eye, render extent and frame index.
// Renderers extend it (App/frame_context.h) with pass-specific addresses and
// static_cast back to what they need.

// Immutable per-pass configuration + per-flight slot reference. `config`
// points to a typed, caller-owned POD that is stable for the lifetime of the
// compiled graph (typically the owner renderer or a value stashed beside it);
// `frameSlot` is this pass's slot in the graph's FrameDataTable (allocated via
// RenderGraph::PassFrameData). A record function that needs no per-pass config
// or frame data leaves them null / UINT32_MAX.
struct PassData {
    const void* config = nullptr;
    uint32_t frameSlot = UINT32_MAX;
};

using RecordFn = void (*)(CommandBuffer&, const FrameContext&, const PassData&);

// Shared no-op record function for passes with no draw/dispatch (clears,
// attachment-only passes). Stable and allocation-free.
inline void RecordNoop(CommandBuffer&, const FrameContext&, const PassData&) {}

// Per-flight data table owned by the graph (RFC "per-flight binding tables").
// Slot storage rides a linear Arena — FrameArena while the declaration set is
// rebuilt each frame (reset wholesale at frame start); AppArena once the
// build-once executor keeps the graph across frames. Slot types must be
// trivially copyable/destructible (no owning std::vector): variable-length
// per-frame data is stored as arena-allocated spans via CopyArray, so the
// arena reset reclaims everything without per-slot destructors or malloc.
class FrameDataTable {
  public:
    explicit FrameDataTable(Arena& arena) : arena_(&arena) {}
    FrameDataTable(const FrameDataTable&) = delete;
    FrameDataTable& operator=(const FrameDataTable&) = delete;

    // Allocate a default-constructed slot of type T; returns its stable index.
    template <typename T>
    uint32_t Allocate() {
        static_assert(std::is_trivially_copyable_v<T>,
                      "FrameDataTable slots must be trivially copyable");
        const uint32_t id = size_;
        if (id == capacity_)
            Grow();
        T* p = arena_->Create<T>();
        pointers_[id] = p;
        size_ = id + 1;
        return id;
    }

    // Copy a span of trivially-copyable elements into the arena; the returned
    // span lives until the next arena reset (the whole frame).
    template <typename T>
    std::span<const T> CopyArray(std::span<const T> src) {
        if (src.empty())
            return {};
        T* dst = static_cast<T*>(arena_->Allocate(src.size() * sizeof(T), alignof(T)));
        std::memcpy(dst, src.data(), src.size() * sizeof(T));
        return {dst, src.size()};
    }

    template <typename T>
    T& Get(uint32_t id) {
        return *static_cast<T*>(pointers_[id]);
    }
    template <typename T>
    const T& Get(uint32_t id) const {
        return *static_cast<const T*>(pointers_[id]);
    }

    [[nodiscard]] uint32_t Size() const { return size_; }

    // Rewind the cursor at frame start (paired with the arena reset, which
    // reclaims the storage). Next frame re-grows from scratch.
    void Reset() {
        size_ = 0;
        pointers_ = nullptr;
        capacity_ = 0;
    }

  private:
    void Grow() {
        const uint32_t next = capacity_ ? capacity_ * 2u : 16u;
        void** np = static_cast<void**>(arena_->Allocate(next * sizeof(void*), alignof(void*)));
        if (size_)
            std::memcpy(np, pointers_, size_ * sizeof(void*));
        pointers_ = np;
        capacity_ = next;
    }

    Arena* arena_ = nullptr;
    void** pointers_ = nullptr;
    uint32_t size_ = 0;
    uint32_t capacity_ = 0;
};

struct RenderPass final {
    explicit RenderPass(const char* name = nullptr) : name_(name) {}

    // Resets a REUSED pass back to the default declaration before the renderer
    // re-applies this frame's fields. frameSlot_ is preserved when the pass
    // identity (its name) is unchanged, so the per-flight slot is stable across
    // frames; a renamed/removed pass re-allocates its slot (the Frame<T> type
    // may have changed with the declaration).
    void Reset(const char* name) {
        const bool sameName = (name_ == name) || (name_ && name && std::strcmp(name_, name) == 0);
        if (!sameName)
            frameSlot_ = UINT32_MAX;
        name_ = name;
        inputImages_.clear();
        storageImages_.clear();
        storageBuffers_.clear();
        imageHints_.clear();
        bufferHints_.clear();
        colorAttachments_.clear();
        depthAttachment_ = {};
        hasDepthAttachment_ = false;
        extent_ = {};
        offsetX_ = 0;
        offsetY_ = 0;
        recordFn_ = nullptr;
        passDataConfig_ = nullptr;
        isCompute_ = false;
        queueHint_ = QueueHint::Graphics;
    }

    // --------------------------------------------------------
    // Attachments
    // --------------------------------------------------------

    RenderPass& SetColor(std::span<const LogicalImageViewHandle> views,
                         AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
                         AttachmentStoreOp storeOp = AttachmentStoreOp::Store,
                         ClearColor clearValue = {}) {
        colorAttachments_.resize(views.size());

        for (size_t i = 0; i < views.size(); ++i) {
            assert(!views[i].empty());

            colorAttachments_[i] = {
                .view = views[i],
                .loadOp = loadOp,
                .storeOp = storeOp,
                .clearColor = clearValue,
            };
        }

        return *this;
    }

    // Keep initializer-list/std::vector call sites source-compatible.
    RenderPass& SetColor(const std::vector<LogicalImageViewHandle>& views,
                         AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
                         AttachmentStoreOp storeOp = AttachmentStoreOp::Store,
                         ClearColor clearValue = {}) {
        return SetColor(
            std::span<const LogicalImageViewHandle>(views), loadOp, storeOp, clearValue);
    }

    // MSAA variant: views are multisampled, resolveViews are the
    // single-sampled resolve destinations (same count, may be empty
    // per-index for no-resolve). resolveViews must match views in size
    // when provided.
    RenderPass& SetColorWithResolve(std::span<const LogicalImageViewHandle> views,
                                    std::span<const LogicalImageViewHandle> resolveViews,
                                    AttachmentLoadOp loadOp = AttachmentLoadOp::Clear,
                                    AttachmentStoreOp storeOp = AttachmentStoreOp::DontCare,
                                    ClearColor clearValue = {}) {
        assert(resolveViews.empty() || resolveViews.size() == views.size());

        colorAttachments_.resize(views.size());

        for (size_t i = 0; i < views.size(); ++i) {
            assert(!views[i].empty());

            colorAttachments_[i] = {
                .view = views[i],
                .loadOp = loadOp,
                .storeOp = storeOp,
                .clearColor = clearValue,
                .resolveView = resolveViews.empty() ? LogicalImageViewHandle{} : resolveViews[i],
            };
        }

        return *this;
    }

    RenderPass& SetColorWithResolve(const std::vector<LogicalImageViewHandle>& views,
                                    const std::vector<LogicalImageViewHandle>& resolveViews,
                                    AttachmentLoadOp loadOp = AttachmentLoadOp::Clear,
                                    AttachmentStoreOp storeOp = AttachmentStoreOp::DontCare,
                                    ClearColor clearValue = {}) {
        return SetColorWithResolve(std::span<const LogicalImageViewHandle>(views),
                                   std::span<const LogicalImageViewHandle>(resolveViews),
                                   loadOp,
                                   storeOp,
                                   clearValue);
    }

    // Per-attachment color setup: each RenderAttachment carries its own
    // loadOp/storeOp/clearColor/resolveView, allowing mixed clear values
    // (e.g. HDR clear 0 + roughness clear 1).
    RenderPass& SetColorAttachments(std::span<const RenderAttachment> attachments) {
        colorAttachments_.assign(attachments.begin(), attachments.end());

        for (const auto& att : colorAttachments_) {
            assert(!att.view.empty());
        }

        return *this;
    }
    RenderPass& SetColorAttachments(const std::vector<RenderAttachment>& attachments) {
        return SetColorAttachments(std::span<const RenderAttachment>(attachments));
    }

    RenderPass& SetDepth(LogicalImageViewHandle view,
                         AttachmentLoadOp loadOp = AttachmentLoadOp::Load,
                         AttachmentStoreOp storeOp = AttachmentStoreOp::Store,
                         ClearDepthStencil clearValue = {}) {
        assert(!view.empty());

        depthAttachment_ = {
            .view = view,
            .loadOp = loadOp,
            .storeOp = storeOp,
            .clear = clearValue,
        };

        hasDepthAttachment_ = true;

        return *this;
    }

    // --------------------------------------------------------
    // Shader resources (legacy sugar – derives sync hints)
    // --------------------------------------------------------

    // Non-owning source list: only the pass-owned copy survives.
    RenderPass& SetInputImages(std::span<const LogicalImageViewHandle> images) {
        inputImages_.assign(images.begin(), images.end());
        for (auto v : inputImages_) {
            bool exists = false;
            for (auto& h : imageHints_)
                if (h.view == v) {
                    exists = true;
                    break;
                }
            if (!exists) {
                ImageSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead;
                req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
                req.layout = ImageLayout::ShaderReadOnlyOptimal;
                imageHints_.push_back({v, req});
            }
        }
        return *this;
    }

    RenderPass& SetInputImages(std::vector<LogicalImageViewHandle> images) {
        inputImages_ = std::move(images);
        // Also populate sync hints with derived stage/layout if not already hinted.
        for (auto v : inputImages_) {
            bool exists = false;
            for (auto& h : imageHints_)
                if (h.view == v) {
                    exists = true;
                    break;
                }
            if (!exists) {
                ImageSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead;
                req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
                req.layout = ImageLayout::ShaderReadOnlyOptimal;
                imageHints_.push_back({v, req});
            }
        }
        return *this;
    }

    RenderPass& SetStorageImages(std::vector<LogicalImageViewHandle> images) {
        storageImages_ = std::move(images);
        for (auto v : storageImages_) {
            bool exists = false;
            for (auto& h : imageHints_)
                if (h.view == v) {
                    exists = true;
                    break;
                }
            if (!exists) {
                ImageSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
                req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
                req.layout = ImageLayout::General;
                imageHints_.push_back({v, req});
            }
        }
        return *this;
    }

    RenderPass& SetStorageBuffers(std::vector<LogicalBufferHandle> buffers) {
        storageBuffers_ = std::move(buffers);
        for (auto b : storageBuffers_) {
            bool exists = false;
            for (auto& h : bufferHints_)
                if (h.buffer == b) {
                    exists = true;
                    break;
                }
            if (!exists) {
                BufferSyncRequest req{};
                req.barrierMasks.accessMask = AccessFlags::ShaderRead | AccessFlags::ShaderWrite;
                req.barrierMasks.stageMask = DerivePipelineStage(req.barrierMasks.accessMask);
                bufferHints_.push_back({b, req});
            }
        }
        return *this;
    }

    // --------------------------------------------------------
    // Explicit sync hints (bindless model)
    // --------------------------------------------------------
    RenderPass& AddImageHint(LogicalImageViewHandle view, ImageSyncRequest request) {
        assert(!view.empty());
        if (request.barrierMasks.stageMask == PipelineStageBit_None)
            request.barrierMasks.stageMask = DerivePipelineStage(request.barrierMasks.accessMask);
        // Every hand-written (stage x access) pair gets debug-validated
        // here - the ingestion choke point every hint path funnels
        // through (AddImageHints batches route through this too). The
        // class this pins: IndirectCommandRead under a stage mask
        // without the DrawIndirect bit (AllGraphics is the SHADER union
        // only - the barrier would leave the fixed-function command
        // fetch unsynchronized; see AssertStageAccessSupported).
        AssertStageAccessSupported(request.barrierMasks.stageMask,
                                   request.barrierMasks.accessMask,
                                   "RenderGraph.AddImageHint");
        // deduplicate – replace if exists
        for (auto& h : imageHints_)
            if (h.view == view) {
                h.request = request;
                return *this;
            }
        imageHints_.push_back({view, request});
        return *this;
    }
    RenderPass& AddBufferHint(LogicalBufferHandle buffer, BufferSyncRequest request) {
        assert(!buffer.empty());
        if (request.barrierMasks.stageMask == PipelineStageBit_None)
            request.barrierMasks.stageMask = DerivePipelineStage(request.barrierMasks.accessMask);
        // Same (stage x access) contract as AddImageHint - the buffer
        // twin of the ingestion choke point (the indirect-draw hints are
        // the classic offender: IndirectCommandRead @ DrawIndirect, see
        // AssertStageAccessSupported).
        AssertStageAccessSupported(request.barrierMasks.stageMask,
                                   request.barrierMasks.accessMask,
                                   "RenderGraph.AddBufferHint");
        for (auto& h : bufferHints_)
            if (h.buffer == buffer) {
                h.request = request;
                return *this;
            }
        bufferHints_.push_back({buffer, request});
        return *this;
    }
    // Batch variants
    RenderPass& AddImageHints(
        const std::vector<std::pair<LogicalImageViewHandle, ImageSyncRequest>>& hints) {
        for (auto& p : hints)
            AddImageHint(p.first, p.second);
        return *this;
    }
    RenderPass& AddBufferHints(
        const std::vector<std::pair<LogicalBufferHandle, BufferSyncRequest>>& hints) {
        for (auto& p : hints)
            AddBufferHint(p.first, p.second);
        return *this;
    }

    // --------------------------------------------------------
    // Render area
    // --------------------------------------------------------

    RenderPass& SetExtent(Extent2D extent) {
        extent_ = extent;
        return *this;
    }

    RenderPass& SetOffset(int32_t x, int32_t y) {
        offsetX_ = x;
        offsetY_ = y;
        return *this;
    }

    // Stable record-function ABI (static compiled graph): a non-capturing
    // function pointer plus an immutable, caller-owned configuration block.
    // `config` must outlive the compiled graph (typically a renderer member).
    // The per-flight frame slot is allocated via RenderGraph::PassFrameData and
    // carried to the record function through PassData::frameSlot.
    RenderPass& SetRecordFn(RecordFn fn, const void* config = nullptr) {
        recordFn_ = fn;
        passDataConfig_ = config;
        return *this;
    }

    [[nodiscard]] bool HasRecordFn() const { return recordFn_ != nullptr; }

    // --------------------------------------------------------
    // Compute support (Forward+ light culling, etc.)
    // --------------------------------------------------------
    // Marks this pass as compute: Execute() skips BeginRendering
    // and runs the recording func directly (barriers still emitted
    // from image/buffer hints). Compute passes must have no color
    // or depth attachments.
    RenderPass& MarkAsCompute() {
        isCompute_ = true;
        return *this;
    }

    [[nodiscard]] bool IsCompute() const {
        return isCompute_;
    }

    // Queue placement for compute passes (EVSM prefiltering).
    // Graphics (default): recorded into the main graphics command
    //   buffer, in-order with the rest of the graph.
    // AsyncCompute: hint that this pass prefers the dedicated
    //   compute queue. The current Execute() still records it on
    //   graphics (emulated async) — EvsmAsyncPrefilter consumes
    //   this hint to route the blur outside the graph onto the
    //   real compute queue with cross-queue ownership transfers.
    //   Keeping the hint on the pass (rather than only in EVSM
    //   code) makes the future cutover — true async inside
    //   Execute() — a local change to the executor.
    enum class QueueHint : uint8_t { Graphics = 0, AsyncCompute = 1 };
    RenderPass& SetQueueHint(QueueHint q) {
        queueHint_ = q;
        return *this;
    }
    RenderPass& MarkAsAsyncCompute() {
        isCompute_ = true;
        queueHint_ = QueueHint::AsyncCompute;
        return *this;
    }

    [[nodiscard]] bool IsAsyncCompute() const {
        return isCompute_ && queueHint_ == QueueHint::AsyncCompute;
    }

  private:
    friend RenderGraph;
    friend CommandBuffer;

    // Shader-readable resources (legacy, also drives imageHints_ defaults).
    std::vector<LogicalImageViewHandle> inputImages_;

    // Read/write shader resources.
    std::vector<LogicalImageViewHandle> storageImages_;

    std::vector<LogicalBufferHandle> storageBuffers_;

    // Explicit sync hints – primary source for barrier generation.
    std::vector<ImageHint> imageHints_;
    std::vector<BufferHint> bufferHints_;

    // Render targets.
    std::vector<RenderAttachment> colorAttachments_;

    DepthAttachment depthAttachment_{};

    bool hasDepthAttachment_ = false;

    Extent2D extent_{};

    int32_t offsetX_ = 0;
    int32_t offsetY_ = 0;

    RecordFn recordFn_ = nullptr;
    const void* passDataConfig_ = nullptr;
    // This pass's slot in the graph's per-flight FrameDataTable (allocated
    // lazily by RenderGraph::PassFrameData). UINT32_MAX = no frame data.
    uint32_t frameSlot_ = UINT32_MAX;

    bool isCompute_ = false;
    QueueHint queueHint_ = QueueHint::Graphics;

    const char* name_ = nullptr;
};

// ============================================================
// Pass-usage table + the sorter core (public test seam)
// ============================================================
// The per-pass usage table the sorter + the transient-heap packer
// consume (RESOLVED PHYSICAL handles - see CollectPassUsages).
// Namespace-level + public so the sorter's contracts are unit-
// testable without a device (RendergraphContractTests).
struct PassUsage {
    std::vector<std::pair<ImageHandle, ImageSyncRequest>> images; // resolved physical
    std::vector<std::pair<BufferHandle, BufferSyncRequest>> buffers;
};

// The sorter's persistent scratch (RenderGraph keeps one reused
// across frames; tests mint their own).
struct TopoSortScratch {
    std::vector<size_t> sorted;
    std::vector<size_t> heap;
    std::vector<std::vector<size_t>> adj;
    std::vector<int> indeg;
    std::unordered_map<uint64_t, size_t> lastWriterImage;
    std::unordered_map<uint64_t, std::vector<size_t>> lastReadersImage;
    std::unordered_map<uint64_t, size_t> lastWriterBuffer;
    std::unordered_map<uint64_t, std::vector<size_t>> lastReadersBuffer;
    // Cycle diagnostics: the sorter's contract is a DAG over DISTINCT
    // passes - a true here is a BUG (self-edges from same-physical
    // aliasing inside one pass were the review finding this seam was
    // extracted to pin), never a legitimate input state. The fallback
    // still emits insertion order + asserts.
    bool hadCycle = false;
};
// The sorter core (impl.cpp): edges connect DISTINCT passes only -
// each pass's declarations are combined BY PHYSICAL RESOURCE first
// (write wins), and every edge insert carries a self-edge guard
// (defense in depth). Returns the scratch's internally-owned order;
// check scratch.hadCycle for the insertion-order fallback.
const std::vector<size_t>& TopologicalSortUsages(const std::vector<PassUsage>& usages,
                                                 TopoSortScratch& s);

// ============================================================
// Render graph
// ============================================================

class ResourceManager;
struct Framebuffer;
struct RenderGraph final {
    explicit RenderGraph(ResourceManager& ctx);

    ~RenderGraph();

    // Targeted teardown of a single external view/image. Used to refresh
    // swapchain color views on Recreate() without clearing every external
    // resource the graph holds.
    void FreeImageView(LogicalImageViewHandle handle);

    void FreeImage(LogicalImageHandle handle);

    void FreeBuffer(LogicalBufferHandle handle);

    // Destroys resolved views then transient images so each parent image's
    // liveViewCount reaches zero before the image is released. External
    // resources are owned by the caller and are left untouched.
    void ReleaseResolvedResources();

    // --------------------------------------------------------
    // Frame
    // --------------------------------------------------------

    // Build-once declaration lifecycle (checkpoint C): the pass list persists
    // across frames. NewFrame resets the declaration cursor; renderers re-add
    // passes through AddPass/AddComputePass, which REUSE the pass at the cursor
    // (resetting its declaration, keeping its frame slot) when one exists.
    // EndFrameDeclaration truncates any pass the renderers stopped emitting
    // (a feature toggled off).
    void NewFrame() {
        declarationCursor_ = 0;
    }

    // Truncate the pass list to the declaration cursor (called once at the end
    // of a frame so a feature that stopped emitting passes doesn't leave stale
    // passes behind).
    void EndFrameDeclaration() {
        if (declarationCursor_ < passes_.size())
            passes_.resize(declarationCursor_);
    }

    RenderPass& AddPass(const char* name = nullptr) {
        if (declarationCursor_ < passes_.size()) {
            RenderPass& pass = passes_[declarationCursor_];
            pass.Reset(name);
            ++declarationCursor_;
            return pass;
        }
        ++declarationCursor_;
        return passes_.emplace_back(name);
    }

    // Allocate (once) and refresh (each frame) the per-flight frame slot for
    // a pass. The slot is typed; the record function reads the same type back
    // through PassData::frameSlot. Returns a writable reference the caller
    // fills with this frame's values.
    template <typename T>
    T& PassFrameData(RenderPass& pass) {
        if (pass.frameSlot_ == UINT32_MAX)
            pass.frameSlot_ = frameData_.Allocate<T>();
        return frameData_.Get<T>(pass.frameSlot_);
    }

    RenderPass& AddComputePass(const char* name = nullptr) {
        if (declarationCursor_ < passes_.size()) {
            RenderPass& pass = passes_[declarationCursor_];
            pass.Reset(name);
            pass.MarkAsCompute();
            ++declarationCursor_;
            return pass;
        }
        ++declarationCursor_;
        RenderPass& pass = passes_.emplace_back(name);
        pass.MarkAsCompute();
        return pass;
    }

    // The graph's per-flight data table (RFC "per-flight binding tables").
    // Renderers allocate a typed slot once and refresh it each frame; record
    // functions read it by index through the FrameContext's graph pointer.
    [[nodiscard]] FrameDataTable& FrameData() { return frameData_; }
    [[nodiscard]] const FrameDataTable& FrameData() const { return frameData_; }

    // ========================================================
    // Images
    // ========================================================

    [[nodiscard]]
    LogicalImageHandle AddImage(const ImageCreateInfo& description, const char* name = "");

    [[nodiscard]]
    LogicalImageHandle AddExternalImage(ImageHandle imageHandle, const char* name = "");

    // ========================================================
    // Image views
    // ========================================================

    [[nodiscard]]
    LogicalImageViewHandle AddImageView(LogicalImageHandle image,
                                        ImageViewType type = ImageViewType::View2D,
                                        Format format = Format::Undefined,
                                        ImageSubresourceRange range = {},
                                        const char* name = "") {
        assert(!image.empty());

        LogicalImageView view{
            .key =
                {
                    .image = image,

                    .viewType = type,

                    .format = format,

                    .subresourceRange = range,
                },

            .name = name,
        };

        return imageViews_.create(std::move(view));
    }

    LogicalImageViewHandle AddExternalImageView(ImageViewHandle imageView,
                                                Format format,
                                                const char* name = "") {
        assert(!imageView.empty());

        LogicalImageView view{
            .type = LogicalImageView::Type::ExternalPhysical,
            .externalViewHandle = imageView,
            .externalFormat = format,
            .name = name,
        };

        return imageViews_.create(std::move(view));
    }

    [[nodiscard]]
    LogicalImageViewHandle AddExternalImageView(ImageHandle image,
                                                ImageViewType type = ImageViewType::View2D,
                                                Format format = Format::Undefined,
                                                ImageSubresourceRange range = {},
                                                const char* name = "") {
        auto logicalImage = AddExternalImage(image, name);

        return AddImageView(logicalImage, type, format, range, name);
    }

    // ========================================================
    // Buffers
    // ========================================================

    template <typename T>
    [[nodiscard]]
    LogicalBufferHandle AddBuffer(uint32_t count,
                                  BufferType usage = BufferType_Storage,
                                  const char* name = "") {
        LogicalBuffer buffer{
            .type = ResourceType::Transient,

            .key =
                {
                    .size = sizeof(T) * count,

                    .bufferType = usage,

                    .name = name,
                },

            .resolvedBufferHandle = {},

            .name = name,
        };

        return buffers_.create(std::move(buffer));
    }

    [[nodiscard]]
    LogicalBufferHandle AddExternalBuffer(BufferHandle bufferHandle, const char* name = "") {
        assert(!bufferHandle.empty());

        LogicalBuffer buffer{
            .type = ResourceType::External,

            .resolvedBufferHandle = bufferHandle,

            .name = name,
        };

        return buffers_.create(std::move(buffer));
    }

    // ========================================================
    // Resolution
    // ========================================================

    [[nodiscard]]
    BufferHandle GetResolvedBufferHandle(LogicalBufferHandle handle) const {
        assert(!handle.empty());

        auto* buffer = buffers_.get(handle);

        assert(buffer != nullptr);

        return buffer->resolvedBufferHandle;
    }

    [[nodiscard]]
    ImageHandle GetResolvedImageHandle(LogicalImageHandle handle) const {
        assert(!handle.empty());

        auto* image = images_.get(handle);

        assert(image != nullptr);

        return image->resolvedImageHandle;
    }

    [[nodiscard]]
    Format GetResolvedImageViewFormat(LogicalImageViewHandle handle) const {
        assert(!handle.empty());

        const auto* view = imageViews_.get(handle);
        assert(view != nullptr);

        if (view->type == LogicalImageView::Type::ExternalPhysical) {
            assert(view->externalFormat != Format::Undefined);
            return view->externalFormat;
        }

        if (view->key.format != Format::Undefined) {
            return view->key.format;
        }

        const auto* image = images_.get(view->key.image);

        assert(image != nullptr);
        assert(image->key.format != Format::Undefined);

        return image->key.format;
    }

    // ========================================================
    // Execution
    // ========================================================

    // Records the frame. `fc` is the per-frame input the record functions
    // (RecordFn) read; legacy recording lambdas ignore it. Null during the
    // migration where the caller has no FrameContext yet (samples/tests).
    void Execute(Framebuffer& fb, const FrameContext* fc = nullptr);

    // Compile the current declaration set into an immutable plan (checkpoint B
    // foundation): canonicalizes the physical resources into dense ids,
    // extracts the per-pass access modes/stages/masks/layouts, and runs the
    // static timeline compiler over `order` (topological execution order).
    // `useAsync` matches the executor's async-split decision: when false, every
    // pass is compiled as Graphics so the plan's single-stream sections/batches
    // mirror the emulated (inline) execution. The returned GraphKey is the
    // structural permutation identity the executor caches on.
    [[nodiscard]] smvk::rg::CompiledGraph CompileGraph(const std::vector<PassUsage>& usages,
                                                       const std::vector<size_t>& order,
                                                       bool useAsync) const;

    // Precompile the CURRENT declaration set (passes_ as built this instant)
    // into the bounded permutation cache. Agnostic to the caller: app load can
    // prewarm the launch-time permutations, and level load can prewarm
    // level-specific ones later, both through this same entry point. The plan
    // (the expensive part) is cached under its GraphKey; per-flight handles are
    // still resolved each frame in Execute, so a later transient-heap relayout
    // never serves a stale handle.
    void PrewarmCompiledGraph(bool useAsync);

    // Number of distinct permutations currently cached.
    [[nodiscard]] size_t CompiledGraphCacheSize() const { return planCache_.size(); }

    // --------------------------------------------------------
    // Resource resolution
    // --------------------------------------------------------

    void ResolveImages();
    void ResolveBuffers();
    void ResolveImageViews();

    // --------------------------------------------------------
    // Sync – sorting & barrier generation (local bookkeeping)
    // --------------------------------------------------------
  private:
    friend struct CommandBuffer;

    // The flight slot (fb.frameIndex % smvk::limits::MaxFramesInFlight) Execute is
    // recording for: the bindless descriptor sets are REPLICATED PER FRAME
    // (one set per flight - see VulkanResourceManager's set allocation),
    // each set's kBinding_EngineGlobals permanently pointing at its own
    // EngineGlobals slice. Every CommandBuffer of this graph binds THIS
    // flight's set (BindRenderPipeline / BindComputePipeline read this), so
    // sibling in-flight flights never see each other's descriptor rewrites.
    uint32_t flightSlot_ = 0;

    // The cheap half of compilation: canonicalize resources (dense ids +
    // per-flight handle keys) and build the per-pass access lists + the
    // structural GraphKey, WITHOUT running the timeline compiler. Execute
    // uses this every frame (it is cheap and also refreshes the per-flight
    // handles), then only runs smvk::rg::Compile on a cache miss.
    struct GraphPrepare {
        smvk::rg::GraphKey key;
        std::vector<uint64_t> identities; // kind<<32 | index
        std::vector<uint64_t> handles;    // (gen<<32) | index, per-flight
        std::vector<smvk::rg::Pass> passes;
        smvk::rg::Id sectionCount = 0;
    };
    GraphPrepare PrepareGraph(const std::vector<PassUsage>& usages,
                              const std::vector<size_t>& order,
                              bool useAsync) const;

    // Bounded compiled-permutation cache (checkpoint E): GraphKey -> Plan.
    // The plan is structure-stable; handles are resolved per frame, so a
    // transient-heap relayout does not invalidate cached plans.
    mutable std::unordered_map<smvk::rg::GraphKey, std::shared_ptr<const smvk::rg::Plan>,
                               smvk::rg::GraphKeyHash>
        planCache_;
    static constexpr size_t kMaxCompiledPermutations = 16;

    // Build-once analysis cache (checkpoint C): keyed by a structural
    // fingerprint of the declaration set + the resource revision (bumped when
    // a logical resource resolves to NEW physical backing — resize/reload).
    // On a hit Execute skips CollectPassUsages + TopologicalSort entirely.
    [[nodiscard]] uint64_t StructureHash() const;
    mutable uint64_t resourceRevision_ = 0;
    mutable uint64_t analysisHash_ = 0;
    mutable uint64_t analysisRevision_ = 0;
    mutable std::vector<PassUsage> analysisUsages_;
    mutable std::vector<size_t> analysisSorted_;
    mutable bool analysisValid_ = false;

    // Cached prepare result (skips PrepareGraph's interning + hashing on a
    // steady-state hit). Invalidated on relayout (per-flight handles change).
    mutable GraphPrepare cachedPrep_;
    mutable bool prepValid_ = false;
    mutable uint64_t prepHash_ = 0;
    mutable uint64_t prepRevision_ = 0;
    mutable bool prepUseAsync_ = false;

    // Cached per-flight barrier tokens + resource bindings (Vulkan-typed;
    // defined in the impl to keep the public header backend-neutral).
    struct CompiledBarrierCache;
    mutable std::shared_ptr<CompiledBarrierCache> barrierCache_;

    // Collect per-pass physical image/buffer usage with derived layout/access
    // (the NAMESPACE-LEVEL smvk::PassUsage - hoisted for testability).
    std::vector<PassUsage> CollectPassUsages() const;
    static bool IsWriteImage(const ImageSyncRequest& r) {
        return IsWriteAccess(r.barrierMasks.accessMask);
    }
    static bool IsWriteBuffer(const BufferSyncRequest& r) {
        return IsWriteAccess(r.barrierMasks.accessMask);
    }
    // Returns an internally-owned order (reused across frames): the
    // sort used to rebuild its adjacency, indegree and frontier
    // buffers — and re-sort the whole ready set on every release —
    // once per Execute.
    const std::vector<size_t>& TopologicalSort(const std::vector<PassUsage>& usages) const;
    void EmitBarriers(void* cmdOpaque,
                      const std::vector<size_t>& sortedOrder,
                      const std::vector<PassUsage>& usages);

    // TopologicalSort scratch (mutable: reused across frames). One
    // TopoSortScratch (the same shape the free sorter core takes -
    // see TopologicalSortUsages) instead of the eight loose fields.
    mutable TopoSortScratch topo_{};

  public:
    // --------------------------------------------------------
    // Graph storage
    // --------------------------------------------------------

    Pool<LogicalImage, LogicalImage> images_;
    Pool<LogicalImageView, LogicalImageView> imageViews_;
    Pool<LogicalBuffer, LogicalBuffer> buffers_;
    std::vector<RenderPass> passes_;
    FrameDataTable frameData_;
    // Build-once declaration cursor: the next AddPass/AddComputePass reuses
    // passes_[declarationCursor_] (reset) when it exists, else appends.
    size_t declarationCursor_ = 0;

    ResourceManager& ctx_;
    struct PipelineCacheOpaque;
    // Linear process-owned (dtor-only deleter; never freed).
    mem::LinearPtr<PipelineCacheOpaque> pipelineCacheOpaque_;

    // ---- Transient heap (design B: interval-packed memory aliasing) ----
    // Execute compiles the frame: transient images are created UNBOUND
    // (handles + views resolve for the analysis), then packed by their
    // [firstUse, lastUse] pass intervals into ranges of graph-owned
    // device-local heap blocks. SHAPE-COMPATIBLE images with DISJOINT
    // intervals SHARE ranges (VK_IMAGE_CREATE_ALIAS_BIT + identical
    // parameters + no simultaneous use - the sharer's first barrier
    // transitions from UNDEFINED, discarding the previous occupant's
    // contents, the standard transient lifetime handoff). Overlapping
    // intervals or incompatible shapes get disjoint ranges. Steady
    // state (unchanged layout fingerprint): zero churn - the same
    // images + placements persist frame to frame. A layout change
    // (config toggles, extent changes, resource churn) detonates the
    // packed set once: fresh images re-create + re-pack + re-bind, the
    // old set retires through the RMI's deferred-destroy queue.
    // Returns true when the packed set was detonated (fresh handles -
    // the caller MUST recompute its usages/sort/snapshot).
    struct TransientHeapOpaque;
    mem::LinearPtr<TransientHeapOpaque> transientHeapOpaque_{};
    bool CompileTransientHeap(const std::vector<PassUsage>& usages,
                              const std::vector<size_t>& sorted);
    // Releases ONE placed transient (FreeImage + the repack detonation):
    // destroys the physical (deferred) and queues the packed range's
    // free BEHIND the destroy (DeferDestroy appends at the current
    // point, FIFO - a range can never be re-bound while its previous
    // binding's object still exists).
    void ReleaseTransientPlacement(LogicalImageHandle handle);

    // --------------------------------------------------------
    // Pipeline cache introspection
    // --------------------------------------------------------

    // Graphics-pipeline population count (debug: the hitch logger's
    // warm-up signal — a hitch frame that also mints a batch of
    // pipelines is shader compilation, not traversal cost).
    [[nodiscard]] size_t PipelineCacheCount() const;
    // HOT-RELOAD RETIREMENT (the consumer leg, Utils/shader_hot_reload.h):
    // evicts every cached pipeline built from `module` and retires the
    // VkPipelines behind the in-flight frames (the deferred-destroy
    // margin). Call at the frame boundary AFTER the consumer rebuilt
    // its module+program set over the replacement module — the next
    // bind mints fresh entries. Returns the retired pipeline count
    // (diagnostics; 0 on an unknown handle).
    size_t RetireShaderModule(ShaderModuleHandle module);
};

} // namespace smvk
