// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Backend registry + concrete backend construction. CPU-only: the backends'
// Kind()/construction paths run without a Vulkan device (their Initialize
// needs a ResourceManager and is exercised by the live WorldRenderer wiring).
#include <Engine/Renderer/GeometryBackendRegistry.h>

#include <cassert>

using namespace engine;

int main() {
    // Availability: the two live backends are constructible; the task/mesh
    // adapter and the legacy filter are not yet adapted/ported.
    assert(GeometryBackendRegistry::IsAvailable(GeometryBackendKind::CpuIndirect));
    assert(GeometryBackendRegistry::IsAvailable(GeometryBackendKind::GpuSceneIndexed));
    assert(!GeometryBackendRegistry::IsAvailable(GeometryBackendKind::GpuSceneTaskMesh));
    assert(!GeometryBackendRegistry::IsAvailable(GeometryBackendKind::LegacyGfxFilter));

    // Construction + kind round-trip.
    auto cpu = GeometryBackendRegistry::Create(GeometryBackendKind::CpuIndirect);
    assert(cpu && cpu->Kind() == GeometryBackendKind::CpuIndirect);
    auto gsi = GeometryBackendRegistry::Create(GeometryBackendKind::GpuSceneIndexed);
    assert(gsi && gsi->Kind() == GeometryBackendKind::GpuSceneIndexed);
    assert(!GeometryBackendRegistry::Create(GeometryBackendKind::GpuSceneTaskMesh));
    assert(!GeometryBackendRegistry::Create(GeometryBackendKind::LegacyGfxFilter));

    return 0;
}
