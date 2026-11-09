// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// A renderer-independent, CPU-only producer channel for immediate-mode
// debug geometry. The host owns this channel and can connect it to any
// consumer of tour::DebugDrawList. It does not own GPU state or scene data.
#include <Engine/Renderer/DebugDraw.h>

#include <functional>
#include <utility>

namespace tour {

class DebugDrawChannel {
  public:
    using Producer = std::function<void(DebugDrawList&)>;

    void SetSource(Producer producer) {
        m_source = std::move(producer);
    }

    void ClearSource() {
        m_source = {};
    }

    [[nodiscard]] bool HasSource() const {
        return static_cast<bool>(m_source);
    }

    // The consumer owns list clearing and frame lifetime. Like the existing
    // WorldRenderer callback, this invokes the producer on the calling thread.
    void CollectInto(DebugDrawList& list) const {
        if (m_source)
            m_source(list);
    }

  private:
    Producer m_source;
};

} // namespace tour
