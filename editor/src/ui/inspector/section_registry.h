// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
namespace editor {
// Scene-node section dispatch. Existing sections decide whether they
// apply to a node; their registration order is also their display order.
class InspectorSectionRegistry {
public:
    using DrawFn = std::function<void(uint32_t)>;
    void Register(const char* id, DrawFn draw) {
        IM_ASSERT(id && *id);
        const bool inserted = m_ids.emplace(id).second;
        IM_ASSERT(inserted && "Duplicate inspector section ID");
        if (inserted) m_sections.push_back(std::move(draw));
    }
    void DrawAll(uint32_t node) {
        for (auto& section : m_sections) section(node);
    }
private:
    std::unordered_set<std::string> m_ids;
    std::vector<DrawFn> m_sections;
};
} // namespace editor
