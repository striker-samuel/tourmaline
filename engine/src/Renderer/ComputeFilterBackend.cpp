// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/ComputeFilterBackend.h>

#include <cstdio>

namespace engine {

bool ComputeFilterBackend::Initialize(GeometryBackendResources& resources) {
    // The filter/seed/remainder shaders and their push-constant / draw-record /
    // cluster ABIs were retired; recovery is a port (historical revision must
    // be pinned), not a from-scratch rewrite. Report unavailable until that
    // port lands — the registry keeps this backend gated off.
    (void)resources;
    std::fprintf(stderr,
                 "[ComputeFilterBackend] not available: the retired filter shaders "
                 "have not been recovered/ported yet\n");
    return false;
}

void ComputeFilterBackend::Shutdown(GeometryBackendResources&) noexcept {}

void ComputeFilterBackend::Prepare(const GeometryFrameInputs&) {}

void ComputeFilterBackend::RecordDepth(GeometryPassRecorder&, const GeometryFrameInputs&) {}

void ComputeFilterBackend::RecordOpaque(GeometryPassRecorder&, const GeometryFrameInputs&) {}

void ComputeFilterBackend::RecordMasked(GeometryPassRecorder&, const GeometryFrameInputs&) {}

} // namespace engine
