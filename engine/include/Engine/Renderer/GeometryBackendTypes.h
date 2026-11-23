// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Concrete, device-facing types for the modular geometry backend contract
// (GeometryBackend.h). The contract header itself stays Vulkan-free; this
// header introduces the smvk render-graph types a live backend needs and
// is the seam the frame composer (WorldRenderer) wires through.
//
// Lifetime: every pointer/address in these structs is BORROWED from the
// frame composer for one frame. A backend must copy, not retain, anything it
// needs beyond the call that received it (frame-flight safety).
#include <Engine/Renderer/GeometryBackend.h>
#include <Engine/Rendergraph/renderer.h>
#include <Engine/Rendergraph/rendergraph.h>

namespace engine {

// Per-frame inputs. The camera/view is the frame context; the counts and
// device addresses are the canonical geometry publication for this flight.
struct GeometryFrameInputs {
    const smvk::FrameContext* frame = nullptr;
    uint32_t candidateCount = 0; // admitted static candidates this frame
    uint32_t workCapacity = 0;   // reserved work rows (bin stride)
    uint32_t meshCount = 0;
    uint32_t clusterCount = 0;
    uint32_t instanceCount = 0;
    // Device address of the GPU-produced VkDrawIndexedIndirectCommand stream.
    uint64_t rasterCommandsBda = 0;
};

// Command recording context for one backend pass. The composer owns the
// render pass and its targets; the backend only binds pipelines and records
// draws against the provided command buffer.
struct GeometryPassRecorder {
    smvk::CommandBuffer* cmd = nullptr;
    bool depthOnly = false; // depth prepass vs. opaque/masked shading pass
};

// Borrowed, composer-owned context a backend receives for its lifetime.
// Backends own their private buffers/pipelines internally (RAII holders);
// this bag carries only the shared device/rendergraph handles.
struct GeometryBackendResources {
    smvk::ResourceManager* ctx = nullptr;
    smvk::RenderGraph* rg = nullptr;
};

} // namespace engine
