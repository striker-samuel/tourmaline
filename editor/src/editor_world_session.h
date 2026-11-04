// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Pure editor-document selection boundary: prevent node IDs from a previous
// world from surviving a failed load, revert, or a different hierarchy size.
// No GPU resources or scene history are owned by this helper.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace editor::world_session {

struct Selection {
    uint32_t primary = UINT32_MAX;
    std::vector<uint32_t> nodes;
};

// A valid primary is retained, even if the multi-selection is empty.
// If the former primary was deleted/invalid, use the last surviving
// selected node; otherwise clear selection. Preserve selection order.
[[nodiscard]] inline Selection RestoreSelection(
    uint32_t primary, const std::vector<uint32_t>& saved,
    size_t nodeCount) {
    Selection out{};
    for (uint32_t node : saved) {
        if (node < nodeCount &&
            std::find(out.nodes.begin(), out.nodes.end(), node) == out.nodes.end())
            out.nodes.push_back(node);
    }
    if (primary < nodeCount) out.primary = primary;
    else if (!out.nodes.empty()) out.primary = out.nodes.back();
    return out;
}

} // namespace editor::world_session
