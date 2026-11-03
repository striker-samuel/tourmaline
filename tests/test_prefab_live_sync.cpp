// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// GPU-free regression for live prefab source -> parent instance previews.
// This is the *same policy* used by the editor's second WorldSession.
#include "../editor/src/editor_prefab_sync.h"
#include <glm/mat4x4.hpp>
#include <cassert>
#include <cmath>
#include <iostream>
#include <unordered_map>

static glm::mat4 At(float x) {
    glm::mat4 m(1.0f);
    m[3].x=x;
    return m;
}
int main() {
    using namespace editor::prefab_sync;
    std::unordered_map<uint32_t,glm::mat4> original, applied;
    glm::mat4 sceneA=At(1), sceneB=At(1), overridden=At(8);
    const glm::mat4 authored=At(1);
    // Two occurrences of the same prefab preview the same source edit.
    assert(Apply(original,applied,11,authored,At(4),sceneA,false));
    assert(Apply(original,applied,19,authored,At(4),sceneB,false));
    assert(Near(sceneA,At(4)) && Near(sceneB,At(4)));
    // An explicit per-occurrence parent transform stays untouched.
    assert(!Apply(original,applied,25,authored,At(4),overridden,false));
    assert(Near(overridden,At(8)));
    // Source Undo propagates to every occurrence, retaining overrides.
    assert(Apply(original,applied,11,authored,authored,sceneA,false));
    assert(Apply(original,applied,19,authored,authored,sceneB,false));
    assert(original.empty() && applied.empty());
    assert(Near(sceneA,authored) && Near(sceneB,authored));
    // Same source re-edited, Save establishes a NEW revert baseline.
    assert(Apply(original,applied,11,authored,At(3),sceneA,false));
    assert(Apply(original,applied,19,authored,At(3),sceneB,false));
    Commit(original,applied);
    assert(!RestoreOne(original,applied,11,sceneA));
    assert(!RestoreOne(original,applied,19,sceneB));
    assert(Near(sceneA,At(3)) && Near(sceneB,At(3)));
    // The next edit must use the *saved* source as the new base.
    assert(Apply(original,applied,11,At(3),At(5),sceneA,false));
    assert(Apply(original,applied,19,At(3),At(5),sceneB,false));
    assert(Apply(original,applied,11,At(3),At(3),sceneA,false));
    assert(Apply(original,applied,19,At(3),At(3),sceneB,false));
    assert(Near(sceneA,At(3)) && Near(sceneB,At(3)));
    // An external parent edit during a preview is NOT rolled back.
    assert(Apply(original,applied,11,At(3),At(9),sceneA,false));
    sceneA=At(21);
    assert(!RestoreOne(original,applied,11,sceneA));
    assert(Near(sceneA,At(21)));
    // If an instance override is now owned by parent editing, stop
    // tracking it immediately and never overwrite its pose.
    sceneB=At(3);
    assert(Apply(original,applied,19,At(3),At(7),sceneB,false));
    assert(!Apply(original,applied,19,At(3),At(8),sceneB,true));
    assert(Near(sceneB,At(7)));
    assert(!RestoreOne(original,applied,19,sceneB));
    assert(Near(sceneB,At(7)));
    // Non-finite matrices never pass as equal/safe to overwrite.
    glm::mat4 invalid=At(2);
    invalid[0][0]=NAN;
    assert(!Near(invalid,invalid));
    std::cout<<"PrefabLiveSyncTests passed\n";
    return 0;
}
