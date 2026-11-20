// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/GeometryBackendRegistry.h>

#include <Engine/Renderer/CpuIndirectBackend.h>
#include <Engine/Renderer/GpuSceneIndexedBackend.h>

namespace engine {

bool GeometryBackendRegistry::IsAvailable(GeometryBackendKind kind) noexcept {
    switch (kind) {
    case GeometryBackendKind::CpuIndirect:
    case GeometryBackendKind::GpuSceneIndexed:
        return true;
    // Task/mesh needs mesh-shader support + its own adapter (the
    // WorldRenderer path is not yet wrapped); the legacy filter is not
    // yet ported. Both report unavailable until their adapters land.
    case GeometryBackendKind::GpuSceneTaskMesh:
    case GeometryBackendKind::LegacyGfxFilter:
        return false;
    }
    return false;
}

std::unique_ptr<IGeometryBackend> GeometryBackendRegistry::Create(GeometryBackendKind kind) {
    switch (kind) {
    case GeometryBackendKind::CpuIndirect:
        return std::make_unique<CpuIndirectBackend>();
    case GeometryBackendKind::GpuSceneIndexed:
        return std::make_unique<GpuSceneIndexedBackend>();
    case GeometryBackendKind::GpuSceneTaskMesh:
    case GeometryBackendKind::LegacyGfxFilter:
        return nullptr; // not yet adapted/ported
    }
    return nullptr;
}

} // namespace engine
