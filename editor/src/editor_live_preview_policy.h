// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// One inline WorldRenderer can draw one editor viewport at a time.
// F8's baked-GI view needs that renderer (the editor's legacy atlas
// shader cannot sample current baked probe/volume sources). Prefer the
// active baked viewport, then another baked viewport, then viewport 0's
// ordinary Lit view. All other modes remain editor-only.
namespace editor {

template <typename Mode, typename ModeAt>
constexpr int SelectLivePreviewViewport(int count,
                                        int active,
                                        ModeAt modeAt,
                                        Mode lit,
                                        Mode baked) {
    if (count <= 0)
        return -1;
    if (active >= 0 && active < count && modeAt(active) == baked)
        return active;
    for (int vp = 0; vp < count; ++vp)
        if (modeAt(vp) == baked)
            return vp;
    return modeAt(0) == lit ? 0 : -1;
}

} // namespace editor
