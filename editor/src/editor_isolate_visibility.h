// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Maya-style Isolate Select: viewport-only visibility mask. No SetVisibility
// commands are emitted and the author's hidden flags return exactly on exit.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace editor {

class IsolateVisibility {
  public:
    bool active() const {
        return active_;
    }

    void restore(std::vector<uint8_t>& hidden) {
        if (!active_)
            return;
        // A node created during isolation keeps its own authored visibility.
        const size_t n = std::min(hidden.size(), authored_.size());
        std::copy_n(authored_.begin(), n, hidden.begin());
        authored_.clear();
        active_ = false;
    }

    void abandon() {
        // Use only when the old scene is being discarded.
        authored_.clear();
        active_ = false;
    }

    bool begin(std::vector<uint8_t>& hidden,
               const std::vector<int>& parent,
               const std::vector<uint32_t>& selected) {
        if (active_)
            restore(hidden);
        const size_t count = parent.size();
        if (count == 0 || selected.empty())
            return false;
        std::vector<uint8_t> keep(count, 0);
        bool found = false;
        for (uint32_t node : selected) {
            if (node >= count)
                continue;
            found = true;
            // The selected subtree (including mesh descendants) is visible.
            for (size_t i = 0; i < count; ++i) {
                int n = int(i);
                for (size_t hops = 0; n >= 0 && size_t(n) < count && hops < count; ++hops) {
                    if (uint32_t(n) == node) {
                        keep[i] = 1;
                        break;
                    }
                    n = parent[size_t(n)];
                }
            }
            // Keep ancestors unmasked so selected descendants can render.
            int ancestor = int(node);
            for (size_t hops = 0; ancestor >= 0 && size_t(ancestor) < count && hops < count;
                 ++hops) {
                keep[size_t(ancestor)] = 1;
                ancestor = parent[size_t(ancestor)];
            }
        }
        if (!found)
            return false;
        // Save the exact authored flags (including hidden ancestors).
        // Temporarily unhide the kept nodes, hide everything else.
        authored_ = hidden;
        authored_.resize(count, 0);
        hidden.resize(count, 0);
        for (size_t i = 0; i < count; ++i)
            hidden[i] = keep[i] ? uint8_t(0) : uint8_t(1);
        active_ = true;
        return true;
    }

  private:
    bool active_ = false;
    std::vector<uint8_t> authored_;
};

} // namespace editor
