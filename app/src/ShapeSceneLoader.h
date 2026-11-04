// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Entry-scene counterpart of ProjectScenes' additive scene attachment:
// parse a cooked shape-placement sidecar, resolve each shape through the
// content registry, and atomically populate a renderer-owned ShapeScene.
#include "ProjectRuntime.h"

#include <Engine/Asset/tour.h>
#include <Engine/Renderer/ShapeScene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>

namespace app {

inline bool LoadPackedShapePlacements(
    const std::string& placementText,
    ProjectRuntime& runtime, tour::renderer::ShapeScene& shapes,
    std::string& error,
    const glm::mat4& sceneFrame = glm::mat4(1.0f)) {
    tour::asset::Doc doc;
    if (!tour::asset::LoadTourFromString(placementText, doc, error)) {
        error = "invalid packed shape placements: " + error;
        return false;
    }
    std::vector<tour::renderer::ShapeInstanceHandle> created;
    for (const auto& placement : doc.shapePlacements) {
        if (!placement.enabled) continue;
        std::string shapeError;
        auto bytes = runtime.ReadAssetByPath(
            smvk::kCatalogKindShape, placement.source, shapeError);
        if (bytes && !bytes->empty()) {
            const auto local = glm::translate(glm::mat4(1.0f),
                placement.position);
            auto handle = shapes.LoadText(
                std::string(bytes->begin(), bytes->end()),
                sceneFrame * local, placement.color, shapeError);
            if (handle) {
                created.push_back(handle);
                continue;
            }
        }
        error = "shape placement '" + placement.name
            + "' could not resolve '" + placement.source + "': " + shapeError;
        for (auto handle : created)
            shapes.Remove(handle);
        return false;
    }
    error.clear();
    return true;
}

} // namespace app
