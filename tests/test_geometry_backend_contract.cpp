// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Renderer/GeometryBackend.h>
#include <cassert>
#include <initializer_list>
#include <type_traits>

using namespace engine;

static_assert(std::has_virtual_destructor_v<IGeometryBackend>);
static_assert(ValidateGeometryPipeline(GeometryPipelineConfig{}) ==
              GeometryConfigError::None);

int main() {
    GeometryPipelineConfig c{};
    for (const auto backend : {GeometryBackendKind::CpuIndirect,
                               GeometryBackendKind::GpuSceneIndexed,
                               GeometryBackendKind::GpuSceneTaskMesh,
                               GeometryBackendKind::LegacyGfxFilter}) {
        c.backend = backend;
        assert(ValidateGeometryPipeline(c) == GeometryConfigError::None);
    }
    c.depthPrepass = false;
    assert(ValidateGeometryPipeline(c) == GeometryConfigError::HizNeedsDepth);
    c.depthPrepass = true;
    c.hiz = false;
    assert(ValidateGeometryPipeline(c) == GeometryConfigError::OcclusionNeedsHiz);
    c.hizOcclusion = false;
    assert(ValidateGeometryPipeline(c) == GeometryConfigError::None);
    c.geometrySubmission = false;
    assert(ValidateGeometryPipeline(c) == GeometryConfigError::FragmentNeedsGeometry);
    c.fragmentPass = false;
    assert(ValidateGeometryPipeline(c) == GeometryConfigError::None);
    return 0;
}
