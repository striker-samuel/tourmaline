// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Renderer-owned persistent point/line shape instances. CPU-only registry:
// no Vulkan lifetime requirements and no per-frame retessellation.
// Handles carry a generation so a removed instance cannot alias a new one.
#include <Engine/Renderer/ShapeDebugDraw.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <limits>
#include <string>
#include <vector>

namespace tour::renderer {

struct ShapeInstanceHandle {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;
    [[nodiscard]] explicit operator bool() const {
        return index != UINT32_MAX && generation != 0;
    }
};

struct ShapeInstance {
    asset::ShapeGeometry geometry;
    glm::mat4 worldFromLocal{1.0f};
    glm::vec3 color{1.0f};
    DebugDepthMode depth = DebugDepthMode::Tested;
};

// Scene-side authoring representation, consumed by WorldRenderer without
// needing an immediate-mode debug producer or enabled debug toggles.
class ShapeScene {
public:
    static constexpr size_t MaxInstances = 4096;

    [[nodiscard]] ShapeInstanceHandle Add(const asset::ShapeAsset& asset,
        const glm::mat4& transform, const glm::vec3& color,
        std::string& error, DebugDepthMode depth = DebugDepthMode::Tested) {
        asset::ShapeGeometry geometry;
        if (!asset::TessellateShape(asset, geometry, error))
            return {};
        if (geometry.topology == asset::ShapeGeometry::Topology::Triangles) {
            error = "persistent ShapeScene supports splines and point clouds only";
            return {};
        }
        size_t index = slots_.size();
        for (size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i].active) { index = i; break; }
        if (index == slots_.size() && index >= MaxInstances) {
            error = "shape scene instance limit exceeded";
            return {};
        }
        if (index == slots_.size()) slots_.emplace_back();
        Slot& slot = slots_[index];
        slot.instance = {std::move(geometry), transform, color, depth};
        slot.active = true;
        ++count_;
        error.clear();
        return {uint32_t(index), slot.generation};
    }

    // In-memory source lane for content-registry / pak consumers. The host
    // owns resolution and supplies the decoded .shape bytes as UTF-8 text.
    [[nodiscard]] ShapeInstanceHandle LoadText(
        const std::string& text, const glm::mat4& transform,
        const glm::vec3& color, std::string& error,
        DebugDepthMode depth = DebugDepthMode::Tested) {
        asset::ShapeAsset shape;
        if (!asset::ParseShapeAsset(text, shape, error))
            return {};
        return Add(shape, transform, color, error, depth);
    }

    // Authoring/dev path: read a .shape document and instantiate it once.
    // Production packages should provide decoded text through the content
    // registry instead of relying on absolute host filesystem paths.
    [[nodiscard]] ShapeInstanceHandle LoadFile(
        const std::filesystem::path& path, const glm::mat4& transform,
        const glm::vec3& color, std::string& error,
        DebugDepthMode depth = DebugDepthMode::Tested) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "unable to open shape source: " + path.generic_string();
            return {};
        }
        // Bound input file size before allocating a source string.
        file.seekg(0, std::ios::end);
        const auto bytes = file.tellg();
        if (bytes < 0 || bytes > std::streamoff(8 * 1024 * 1024)) {
            error = "shape source exceeds 8 MiB limit";
            return {};
        }
        file.seekg(0);
        std::string source(static_cast<size_t>(bytes), '\0');
        if (!source.empty())
            file.read(source.data(), static_cast<std::streamsize>(source.size()));
        if (!file) {
            error = "failed to read shape source";
            return {};
        }
        return LoadText(source, transform, color, error, depth);
    }

    [[nodiscard]] const ShapeInstance* Get(ShapeInstanceHandle handle) const {
        if (handle.index >= slots_.size()) return nullptr;
        const Slot& slot = slots_[handle.index];
        if (!slot.active || slot.generation != handle.generation)
            return nullptr;
        return &slot.instance;
    }

    bool Remove(ShapeInstanceHandle handle) {
        if (!Get(handle)) return false;
        Slot& slot = slots_[handle.index];
        slot.active = false;
        slot.instance = {};
        ++slot.generation;
        if (!slot.generation) ++slot.generation;
        --count_;
        return true;
    }

    bool SetTransform(ShapeInstanceHandle handle, const glm::mat4& transform) {
        if (!Get(handle)) return false;
        slots_[handle.index].instance.worldFromLocal = transform;
        return true;
    }

    bool SetColor(ShapeInstanceHandle handle, const glm::vec3& color) {
        if (!Get(handle)) return false;
        slots_[handle.index].instance.color = color;
        return true;
    }

    [[nodiscard]] size_t Size() const { return count_; }

    void Clear() {
        for (Slot& slot : slots_) {
            if (!slot.active) continue;
            slot.active = false;
            slot.instance = {};
            ++slot.generation;
            if (!slot.generation) ++slot.generation;
        }
        count_ = 0;
    }

    // Emit into the existing native RHI point/line stream. Tessellation
    // occurs at Add, not per frame. Respect both stream limits and reject
    // a whole instance if it cannot fit (no partial line or point sets).
    // Returns the number of instances successfully submitted.
    size_t AppendDraws(DebugDrawList& draw) const {
        size_t submitted = 0;
        for (const Slot& slot : slots_) {
            if (!slot.active) continue;
            const ShapeInstance& instance = slot.instance;
            const auto& mesh = instance.geometry;
            const bool points = mesh.topology == asset::ShapeGeometry::Topology::Points;
            const size_t required = points ? mesh.vertices.size() : mesh.indices.size()/2;
            const size_t remaining = points
                ? DebugDrawList::MaxPoints - draw.PointCount()
                : DebugDrawList::MaxLines - draw.Size();
            if (required > remaining) continue;
            auto transform = [&](const asset::ShapePoint& p) {
                return glm::vec3(instance.worldFromLocal *
                    glm::vec4(p.x, p.y, p.z, 1.0f));
            };
            if (points) {
                for (const auto& p : mesh.vertices)
                    draw.Point(transform(p), instance.color, instance.depth);
            } else {
                for (size_t i = 0; i + 1 < mesh.indices.size(); i += 2)
                    draw.Line(transform(mesh.vertices[mesh.indices[i]]),
                              transform(mesh.vertices[mesh.indices[i+1]]),
                              instance.color, instance.depth);
            }
            ++submitted;
        }
        return submitted;
    }

private:
    struct Slot {
        ShapeInstance instance;
        uint32_t generation = 1;
        bool active = false;
    };
    std::vector<Slot> slots_;
    size_t count_ = 0;
};

} // namespace tour::renderer
