// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Backend enumeration + selection. The frame composer (WorldRenderer) selects
// a geometry backend through this registry and fails loudly when a requested
// backend is unavailable (device feature / shader / not-yet-ported). It keeps
// the backend enum a pure data value while the registry owns the
// capability-to-instance mapping.
#include <Engine/Renderer/GeometryBackend.h>
#include <memory>

namespace engine {

class GeometryBackendRegistry {
  public:
    // Capability check for the CURRENT device. False when the backend cannot
    // run: GpuSceneTaskMesh needs mesh-shader support (and a separate adapter
    // instance), ComputeFilter's retired shaders are not yet ported.
    // CpuIndirect and GpuSceneIndexed are always constructible (their
    // shader/pipeline availability is validated at Initialize, not here).
    static bool IsAvailable(GeometryBackendKind kind) noexcept;

    // Constructs a backend instance for the requested kind, or null when
    // unavailable/not-yet-implemented. The caller owns the returned pointer.
    static std::unique_ptr<IGeometryBackend> Create(GeometryBackendKind kind);
};

} // namespace engine
