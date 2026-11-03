// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// CPU-only inspector regression: zero scale must remain editable without
// creating a NaN transform, including when loading an already bad matrix.
#include "editor_numeric.h"
#include "editor_gizmo_rotation.h"
#include "editor_transform_trs.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    if (!ok) { ++failures; std::cerr << "FAIL " << name << "\n"; }
    else std::cout << "PASS " << name << "\n";
}

glm::mat4 Compose(const glm::vec3& t, const glm::vec3& r, const glm::vec3& s) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, t);
    m = glm::rotate(m, glm::radians(r.z), glm::vec3(0, 0, 1));
    m = glm::rotate(m, glm::radians(r.y), glm::vec3(0, 1, 0));
    m = glm::rotate(m, glm::radians(r.x), glm::vec3(1, 0, 0));
    return glm::scale(m, s);
}
bool Near(float x, float y) { return std::abs(x - y) < 0.001f; }
}

int main() {
    using editor::transform::Finite;
    const glm::vec3 originalT(3, -2, 7);
    const glm::vec3 originalR(15, 25, 30);
    for (const glm::vec3 originalS : {
            glm::vec3(0, 2, 3), glm::vec3(2, 0, 3),
            glm::vec3(2, 3, 0), glm::vec3(0, 0, 3),
            glm::vec3(0, 0, 0) }) {
        const glm::mat4 before = Compose(originalT, originalR, originalS);
        glm::vec3 t, r, s;
        editor::transform::ReadLocalTRS(before, t, r, s);
        Check(Finite(t) && Finite(r) && Finite(s), "zero scale decomposes to finite TRS");
        Check(Near(t.x, 3) && Near(t.y, -2) && Near(t.z, 7), "translation retained");
        Check(Near(s.x, originalS.x) && Near(s.y, originalS.y)
            && Near(s.z, originalS.z), "zero scale retained");
        const glm::mat4 roundTrip = Compose(t, r, s);
        Check(Finite(roundTrip), "singular transform round-trip stays finite");
        const glm::mat4 restored = Compose(t, r, glm::vec3(1, 2, 3));
        Check(Finite(restored), "scale can be edited back from zero");
        if (originalS == glm::vec3(0, 2, 3))
            Check(Near(roundTrip[1][0], before[1][0])
                && Near(roundTrip[2][1], before[2][1]),
                "single collapsed axis preserves surviving rotation");
    }

    glm::mat4 corrupted(1.0f);
    corrupted[0][0] = std::numeric_limits<float>::quiet_NaN();
    corrupted[3][0] = 4.0f;
    glm::vec3 t, r, s;
    editor::transform::ReadLocalTRS(corrupted, t, r, s);
    Check(Finite(t) && Finite(r) && Finite(s), "previous NaN transform can be repaired");
    Check(Near(t.x, 4.0f) && Finite(Compose(t, r, s)), "repair keeps valid position");

    const float bad[] = { 2.0f, std::numeric_limits<float>::infinity() };
    const float good[] = { 0.0f, -2.0f };
    Check(!editor::numeric::AllFinite(bad, 2), "reject invalid vector edit");
    Check(editor::numeric::AllFinite(good, 2), "accept zero and finite vector edit");
    Check(editor::numeric::Display(bad[1]) == 0.0f,
        "invalid stored number has editable finite display value");
    Check(!editor::numeric::Finite(std::numeric_limits<float>::quiet_NaN()),
        "reject NaN scalar");
    // Bounds-center pivot regression. Identity drag must not move a
    // pre-rotated object, and a 90-degree delta orbits the pivot once.
    using namespace editor::gizmo_rotation;
    const glm::vec3 baseT(2, 0, 0);
    const glm::vec3 pivot(1, 0, 0);
    const glm::vec3 baseR(0, 0, 45);
    const glm::quat unchanged = LocalDelta(baseR, baseR);
    const glm::vec3 sameT = RotatePosition(baseT, pivot, unchanged);
    Check(glm::length(sameT - baseT) < 1e-4f,
        "bounds-center unchanged rotation does not move node");
    const glm::quat quarterTurn = LocalDelta(baseR, glm::vec3(0, 0, 135));
    const glm::vec3 orbit = RotatePosition(baseT, pivot, quarterTurn);
    Check(Near(orbit.x, 1) && Near(orbit.y, 1),
        "bounds-center uses drag delta to orbit pivot");
    const glm::quat parentQ = Orientation(glm::vec3(0, 0, 30));
    const glm::quat worldDelta =
        parentQ * quarterTurn * glm::inverse(parentQ);
    const glm::quat nextParentQ = Orientation(glm::vec3(0, 25, 0));
    const glm::quat nextDelta = InFrame(worldDelta, nextParentQ);
    const glm::vec3 multiT = RotatePosition(baseT, pivot, nextDelta);
    Check(Finite(multiT) && Near(glm::length(multiT - pivot), 1),
        "multi-select rotation preserves distance to shared pivot");
    const float angleRad = glm::radians(90.0f);
    const glm::quat worldRing = glm::angleAxis(angleRad, glm::vec3(0, 0, 1));
    Check(Near((worldRing * glm::vec3(1, 0, 0)).y, 1.0f),
        "world ring angle remains in radians");

    return failures ? 1 : 0;
}
