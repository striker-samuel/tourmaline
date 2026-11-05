// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Cook-only hierarchy projection. The authored scene is never mutated.
#include <Engine/Scene/scene.h>

#include <vector>

namespace tour::scene {

struct StaticExportHierarchy {
    std::vector<Hierarchy> hierarchy;
    std::vector<glm::mat4> transforms;
    std::vector<uint8_t> staticRoots;
};

// Static subtrees have no runtime transform dependencies: emit each member
// as an independent root with its complete authored world transform. Dynamic
// nodes retain their hierarchy, including original sibling order.
inline StaticExportHierarchy BuildStaticExportHierarchy(const Scene& scene) {
    const size_t count = scene.hierarchy.size();
    StaticExportHierarchy out{
        scene.hierarchy, scene.localTransform, std::vector<uint8_t>(count, 0)
    };
    std::vector<uint8_t> flattened(count, 0);
    const auto live = [&](int n) {
        return n >= 0 && size_t(n) < count
            && IsLiveSlot(scene, uint32_t(n));
    };

    // Recompute from local transforms instead of trusting globalTransform:
    // an editor may export before its next transform-update tick.
    for (size_t s = 0; s < count; ++s) {
        if (!live(int(s)))
            continue;
        bool isStatic = false;
        glm::mat4 world(1.0f);
        int n = int(s);
        size_t depth = 0;
        while (live(n) && depth++ < count) {
            if (size_t(n) < scene.staticRoot.size()
                && scene.staticRoot[size_t(n)] != 0)
                isStatic = true;
            world = scene.localTransform[size_t(n)] * world;
            n = scene.hierarchy[size_t(n)].parent;
        }
        if (!isStatic)
            continue;
        flattened[s] = 1;
        out.staticRoots[s] = 1;
        out.transforms[s] = world;
        out.hierarchy[s] = Hierarchy{
            .parent = -1, .firstChild = -1, .nextSibling = -1,
            .lastSibling = -1, .level = 0
        };
    }

    // A dynamic parent must no longer point into a detached static tree.
    // Preserve the relative ordering of its remaining dynamic children.
    for (size_t s = 0; s < count; ++s) {
        if (!live(int(s)) || flattened[s])
            continue;
        int first = -1;
        int prev = -1;
        int child = scene.hierarchy[s].firstChild;
        size_t visited = 0;
        while (live(child) && visited++ < count) {
            const int next = scene.hierarchy[size_t(child)].nextSibling;
            if (!flattened[size_t(child)]) {
                if (first < 0)
                    first = child;
                if (prev >= 0)
                    out.hierarchy[size_t(prev)].nextSibling = child;
                prev = child;
            }
            child = next;
        }
        out.hierarchy[s].firstChild = first;
        if (prev >= 0) {
            out.hierarchy[size_t(prev)].nextSibling = -1;
            out.hierarchy[size_t(first)].lastSibling = prev;
        }
    }
    return out;
}

} // namespace tour::scene
